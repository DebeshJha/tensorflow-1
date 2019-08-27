/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/compiler/plugin/poplar/driver/tools/pipeline_util.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/util.h"

#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/hlo_creation_utils.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/service/hlo_value.h"
#include "tensorflow/compiler/xla/shape_util.h"

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"

namespace xla {
namespace poplarplugin {

std::ostream& operator<<(std::ostream& stream, const StageID& stage_id) {
  stream << absl::StrCat("PipelineStage (",
                         (stage_id.is_forward ? "Forward" : "Backward"),
                         ") with ID ", stage_id.id, ".");
  return stream;
}

bool IsPiplineStageOrBackwardOp(const HloInstruction* inst) {
  return IsPipelineStage(inst) || IsPipelineStageBackward(inst);
}

bool IsProducerOp(const HloInstruction* inst) {
  switch (inst->opcode()) {
    case HloOpcode::kCall:
      return IsPiplineStageOrBackwardOp(inst);
    case HloOpcode::kParameter:
      return true;
    default:
      return false;
  }
}

StatusOr<PipelineStages> GetPipelineStages(
    HloComputation* pipeline_computation) {
  PipelineStages pipeline_stages;
  for (HloInstruction* inst :
       pipeline_computation->MakeInstructionPostOrder()) {
    if (IsPipelineStage(inst)) {
      pipeline_stages.forward.push_back(inst);
    } else if (IsPipelineStageBackward(inst)) {
      pipeline_stages.backward.push_back(inst);
    }
  }

  // Reverse bwd stages such that for i in [0, pipeline_stages.forward.size()),
  // pipeline_stages.backward[i] corresponds to pipeline_stages.forward[i].
  absl::c_reverse(pipeline_stages.backward);

  // If we have any bwd pipeline stages then we expect them to match the number
  // of fwd stages (i.e. backprop has a stage for each forward prop).
  if (pipeline_stages.backward.size() &&
      pipeline_stages.forward.size() != pipeline_stages.backward.size()) {
    return FailedPrecondition(
        "Expected the number of PiplineStages (%d) and PipelineStageBackwards "
        "(%d) "
        "to match.",
        pipeline_stages.forward.size(), pipeline_stages.backward.size());
  }
  return pipeline_stages;
}

Status VerifyPipelineStagesBeforeLowering(
    const PipelineStages& pipeline_stages) {
  for (auto& stages : {pipeline_stages.forward, pipeline_stages.backward}) {
    for (HloInstruction* stage : stages) {
      HloOpcode root_opcode = stage->to_apply()->root_instruction()->opcode();
      if (root_opcode != HloOpcode::kTuple) {
        return UnimplementedStrCat("Expected the PipelineStage(Backward) ",
                                   stage->ToString(),
                                   " to have a Tuple root instruction but got ",
                                   HloOpcodeString(root_opcode), " instead.");
      }
      if (!absl::c_all_of(stage->users(), [](HloInstruction* user) {
            return user->opcode() == HloOpcode::kGetTupleElement;
          })) {
        return UnimplementedStrCat(
            "Expected all the users of the PipelineStage(Backward) ",
            stage->ToString(), " to be GetTupleElement instructions.");
      }
      if (stage->parent()->root_instruction() == stage) {
        return UnimplementedStrCat(
            "Pipeline stage cannot be the root instruction of the Pipeline.");
      }
    }
  }
  return Status::OK();
}

Status VerifyPipelineStagesAfterLowering(HloInstruction* pipeline_op) {
  HloComputation* pipeline_computation = pipeline_op->to_apply();
  TF_ASSIGN_OR_RETURN(PipelineStages pipeline_stages,
                      GetPipelineStages(pipeline_computation));
  // Get the analysis.
  TF_ASSIGN_OR_RETURN(auto analysis,
                      PipelineDataflowAnalysis::GetAnalysis(pipeline_stages));
  // Make sure all the instructions in the pipeline_computation do not require
  // lowering.
  for (HloInstruction* inst : pipeline_computation->instructions()) {
    TF_ASSIGN_OR_RETURN(bool needs_lowering, analysis->HasToBeLowered(inst));
    if (needs_lowering) {
      return UnimplementedStrCat(
          "Detected instruction ", inst->ToString(),
          " which should have been lowered into a PipelineStage.");
    }
  }
  return Status::OK();
}

StatusOr<bool> DuplicateGTEEdges(PipelineStages& pipeline_stages) {
  bool added_edges = false;
  for (auto& stages : {pipeline_stages.forward, pipeline_stages.backward}) {
    for (HloInstruction* stage : stages) {
      std::vector<HloInstruction*> gtes = stage->users();
      for (HloInstruction* gte : gtes) {
        // We expect GTEs because we did the TF2XLA lowering.
        if (gte->opcode() != HloOpcode::kGetTupleElement) {
          return FailedPrecondition(
              "Expected user of a PipelineStage(Backward) to be a GTE.");
        }
        if (gte->user_count() == 1) {
          continue;
        }
        HloComputation* comp = gte->parent();

        std::vector<HloInstruction*> gte_users = gte->users();
        for (HloInstruction* gte_user : gte_users) {
          VLOG(2) << "Adding a new edge from " << stage->ToString() << " to "
                  << gte_user->ToString() << ".";
          HloInstruction* gte_clone = comp->AddInstruction(gte->Clone());
          gte->SetupDerivedInstruction(gte_clone);
          TF_RETURN_IF_ERROR(gte->ReplaceUseWith(gte_user, gte_clone));
        }
        added_edges = true;
        TF_RETURN_IF_ERROR(comp->ForceRemoveInstruction(gte));
      }
    }
  }
  return added_edges;
}

StatusOr<bool> UniquifyPipelineStageCallsites(PipelineStages& pipeline_stages) {
  absl::flat_hash_set<HloComputation*> already_used;
  bool added_computations = false;
  for (auto& stages : {pipeline_stages.forward, pipeline_stages.backward}) {
    for (HloInstruction* stage : stages) {
      HloComputation* comp = stage->to_apply();
      if (already_used.contains(comp)) {
        VLOG(2) << "Duplicating the computation for stage " << stage->ToString()
                << ".";
        HloModule* module = comp->parent();
        comp = module->AddEmbeddedComputation(comp->Clone());
        stage->set_to_apply(comp);
        added_computations = true;
      }
      already_used.insert(comp);
    }
  }
  return added_computations;
}

namespace {
// Tidy function to remove any dangling outputs.
Status RemovePipelineStageDeadUsers(HloInstruction* pipeline_stage) {
  CHECK(IsPiplineStageOrBackwardOp(pipeline_stage));
  std::vector<HloInstruction*> users = pipeline_stage->users();
  HloComputation* comp = pipeline_stage->parent();
  for (HloInstruction* gte : users) {
    CHECK_EQ(gte->opcode(), HloOpcode::kGetTupleElement);
    absl::optional<HloInstruction*> to_remove;
    if (gte->user_count() == 0) {
      to_remove = gte;
    } else {
      CHECK_EQ(gte->user_count(), 1);
      HloInstruction* gte_user = gte->users()[0];
      if (gte_user->user_count() == 0 && comp->root_instruction() != gte_user) {
        to_remove = gte_user;
      }
    }
    if (to_remove) {
      TF_RETURN_IF_ERROR(comp->RemoveInstructionAndUnusedOperands(*to_remove));
    }
  }
  return Status::OK();
}

// Replace pipeline stage with a new one with a new computation.
StatusOr<HloInstruction*> ReplacePipelineStageWith(
    HloInstruction* stage, std::unique_ptr<HloComputation> new_computation,
    const std::vector<HloInstruction*> new_operands,
    bool remove_unused_operands) {
  HloComputation* pipeline_computation = stage->parent();
  HloComputation* stage_computation = stage->to_apply();
  HloModule* module = stage->GetModule();

  HloComputation* new_stage_computation =
      module->AddEmbeddedComputation(std::move(new_computation));

  HloInstruction* new_stage =
      pipeline_computation->AddInstruction(HloInstruction::CreateCall(
          new_stage_computation->root_instruction()->shape(), new_operands,
          new_stage_computation));
  stage->SetupDerivedInstruction(new_stage);
  new_stage->set_raw_backend_config_string(stage->raw_backend_config_string());
  CHECK(IsPiplineStageOrBackwardOp(new_stage));

  VLOG(3) << "Replacing " << stage->ToString() << " and computation:";
  XLA_VLOG_LINES(3, stage_computation->ToString());
  VLOG(3) << "With " << new_stage->ToString() << " and computation:";
  XLA_VLOG_LINES(3, new_stage_computation->ToString());

  TF_RETURN_IF_ERROR(stage->ReplaceAllUsesWithDifferentShape(new_stage));
  if (remove_unused_operands) {
    TF_RETURN_IF_ERROR(
        pipeline_computation->RemoveInstructionAndUnusedOperands(stage));
  } else {
    TF_RETURN_IF_ERROR(pipeline_computation->RemoveInstruction(stage));
  }
  TF_RETURN_IF_ERROR(module->RemoveEmbeddedComputation(stage_computation));

  return new_stage;
}
}  // namespace

StatusOr<HloInstruction*> AddInstructionsToPipelineStage(
    HloInstruction* stage, const std::vector<HloInstruction*>& ordered_lowering,
    std::map<int64, HloInstruction*> replace_parameter_with_lowered_instruction,
    absl::flat_hash_set<HloInstruction*> forced_parameters) {
  CHECK(IsPiplineStageOrBackwardOp(stage));

  HloComputation* pipeline_computation = stage->parent();

  VLOG(3) << "Lowering the following into the computation "
          << pipeline_computation->ToString();
  for (HloInstruction* inst : ordered_lowering) {
    VLOG(3) << "\t* " << inst->ToShortString();
  }

  // Create a set of instructions to lower for faster lookup.
  const absl::flat_hash_set<HloInstruction*> ordered_lowering_set(
      ordered_lowering.begin(), ordered_lowering.end());

  // Mapping of the lowered instructions.
  absl::flat_hash_map<HloInstruction*, HloInstruction*> lowered_insts;
  // Mapping of operands to parameters inside the new computation.
  absl::flat_hash_map<HloInstruction*, HloInstruction*> operand_to_parameter;
  // Mapping from parameter number to instruction.
  absl::flat_hash_map<int64, HloInstruction*> parameter_instructions;

  // Note that current Hlo API does not allow us to modify the instruction or
  // computation so the algorithm builds new ones.
  HloComputation* stage_computation = stage->to_apply();
  auto builder = HloComputation::Builder(stage_computation->name());
  // A mapping from instructions in the old computation to the new one which is
  // currently being built.
  absl::flat_hash_map<HloInstruction*, HloInstruction*> old_to_new_computation;
  // Duplicate the computation.
  for (HloInstruction* old_inst :
       stage_computation->MakeInstructionPostOrder()) {
    // Get the operands for the instruction we are about to lower.
    std::vector<HloInstruction*> new_operands(old_inst->operand_count());
    absl::c_transform(old_inst->operands(), new_operands.begin(),
                      [&old_to_new_computation](HloInstruction* old_operand) {
                        return old_to_new_computation.at(old_operand);
                      });
    // Clone new instruction.
    HloInstruction* new_inst = builder.AddInstruction(
        old_inst->CloneWithNewOperands(old_inst->shape(), new_operands));
    if (new_inst->opcode() == HloOpcode::kParameter) {
      // Make sure to mark inputs to the computation.
      HloInstruction* input_inst =
          stage->mutable_operand(new_inst->parameter_number());
      operand_to_parameter[input_inst] = new_inst;
      parameter_instructions[new_inst->parameter_number()] = new_inst;
    }

    old_inst->SetupDerivedInstruction(new_inst);
    old_to_new_computation[old_inst] = new_inst;
  }

  // The new pipeline stage arguments.
  std::vector<HloInstruction*> new_stage_operands = {stage->operands().begin(),
                                                     stage->operands().end()};

  auto create_parameter_for = [&](HloInstruction* operand) {
    HloInstruction* parameter =
        builder.AddInstruction(HloInstruction::CreateParameter(
            new_stage_operands.size(), operand->shape(), operand->name()));
    new_stage_operands.push_back(operand);
    return parameter;
  };

  // Clone the instructions into the new computation.
  for (HloInstruction* inst : ordered_lowering) {
    // Get all the new operands - check if we need to lower any into the
    // pipeline stage.
    std::vector<HloInstruction*> new_operands(inst->operand_count());
    for (int64 operand_idx = 0; operand_idx != inst->operand_count();
         ++operand_idx) {
      HloInstruction* operand = inst->mutable_operand(operand_idx);
      // Check if the operand for the instruction being lowered is also being
      // lowered.
      if (!ordered_lowering_set.contains(operand)) {
        // The operand is not being lowered - we need to make sure it can be
        // accessed inside the computation.
        auto itr = operand_to_parameter.find(operand);
        if (itr == operand_to_parameter.end()) {
          // We have not tried to lower it yet.
          HloInstruction* lowered_operand;
          // If this is a GTE on current pipeline stage then just use the input
          // to the tuple as the operand which is already in the new
          // computation.
          if (operand->opcode() == HloOpcode::kGetTupleElement &&
              operand->operand(0) == stage) {
            CHECK_EQ(operand->user_count(), 1);
            // In TF2XLA we expect the root to be a tuple, hence we can
            // get the relevant instruction (guaranteed by
            // VerifyPipelineStagesBeforeLowering).
            HloInstruction* root = stage_computation->root_instruction();
            CHECK_EQ(root->opcode(), HloOpcode::kTuple);
            root = old_to_new_computation.at(root);
            lowered_operand = root->mutable_operand(operand->tuple_index());
          } else {
            // Check if the operand is already an operand of the pipeline
            // stage.
            std::vector<int64> indices = stage->OperandIndices(operand);
            if (indices.size()) {
              // If it is, then get the parameter instruction from the new
              // computation and use it as the operand.
              lowered_operand =
                  stage_computation->parameter_instruction(indices[0]);
              lowered_operand = old_to_new_computation.at(lowered_operand);
            } else {
              // Otherwise create a new parameter and add a new operand to the
              // computation.
              lowered_operand = create_parameter_for(operand);
            }
          }
          // Add the new operand to the mapping so that we don't lower the same
          // operand twice.
          itr = operand_to_parameter.emplace(operand, lowered_operand).first;
        }
        new_operands[operand_idx] = itr->second;
      } else {
        // Because we are iterating in post order we must have lowered the
        // operand already.
        new_operands[operand_idx] = lowered_insts.at(operand);
      }
    }
    // Clone the instruction inside the new computation with new operands.
    HloInstruction* lowered = builder.AddInstruction(
        inst->CloneWithNewOperands(inst->shape(), new_operands));
    inst->SetupDerivedInstruction(lowered);
    lowered_insts[inst] = lowered;
  }

  // Add any forced operands.
  for (HloInstruction* forced_operand : forced_parameters) {
    HloInstruction* parameter = create_parameter_for(forced_operand);
    operand_to_parameter[forced_operand] = parameter;
    lowered_insts[forced_operand] = parameter;
  }

  // Sanity check we lowered everything.
  if (lowered_insts.size() !=
      (ordered_lowering.size() + forced_parameters.size())) {
    return InternalError("Failed to fully lower a cluster into a computation.");
  }

  // Keep track of instructions which are being lowered and have users outside
  // of this lowering. Using a map so that we iterate in the same order.
  std::map<HloInstruction*, absl::flat_hash_set<HloInstruction*>> external_uses;
  for (HloInstruction* inst : ordered_lowering) {
    // Go through all the users.
    for (HloInstruction* user : inst->users()) {
      if (user != stage && !ordered_lowering_set.contains(user)) {
        // Find any outside uses of the inst - we need to create GTEs for
        // those.
        VLOG(3) << "Lowered instruction " << inst->ToString()
                << " has an external user: " << user->ToString();
        external_uses[inst].insert(user);
      }
    }
  }
  // Also track uses of forced parameters.
  for (HloInstruction* forced_operand : forced_parameters) {
    // Go through all the users.
    for (HloInstruction* user : forced_operand->users()) {
      if (user != stage) {
        // Find any outside uses of the inst - we need to create GTEs for
        // those.
        VLOG(3) << "Forced parameter instruction " << forced_operand->ToString()
                << " has an external user: " << user->ToString();
        external_uses[forced_operand].insert(user);
      }
    }
  }

  // Replace uses of parameters with lowered instruction. Note that this does
  // not remove parameters.
  for (auto& pair : replace_parameter_with_lowered_instruction) {
    int64 param_numer = pair.first;
    HloInstruction* parameter = parameter_instructions.at(param_numer);
    HloInstruction* inst_to_lower = pair.second;
    HloInstruction* lowered = lowered_insts.at(inst_to_lower);
    VLOG(3) << "Replacing the use of parameter " << parameter->ToString()
            << " with lowered " << lowered->ToString();
    TF_RETURN_IF_ERROR(parameter->ReplaceAllUsesWith(lowered));
  }

  // Finalize the computation extend the root by outputing any
  // instructions from the cluster which are used outside of the cluster.
  HloInstruction* builder_root;
  {
    HloInstruction* root =
        old_to_new_computation.at(stage_computation->root_instruction());
    CHECK_EQ(root->opcode(), HloOpcode::kTuple);
    // We are creating a new root tuple - first get all the existing
    // operands.
    std::vector<HloInstruction*> tuple_elements(root->operand_count() +
                                                external_uses.size());
    absl::c_copy(root->operands(), tuple_elements.begin());
    // Add extra outputs.
    auto tuple_elements_itr =
        std::next(tuple_elements.begin(), root->operand_count());
    for (auto users_pair : external_uses) {
      *tuple_elements_itr = lowered_insts.at(users_pair.first);
      tuple_elements_itr = std::next(tuple_elements_itr);
    }
    // Add the new root instruction.
    builder_root =
        builder.AddInstruction(HloInstruction::CreateTuple(tuple_elements));
    root->SetupDerivedInstruction(builder_root);
  }

  const int64 old_num_outputs = ShapeUtil::TupleElementCount(stage->shape());
  // Build the new computation and the new pipeline stage with new operands.
  std::unique_ptr<HloComputation> new_computation = builder.Build(builder_root);
  TF_ASSIGN_OR_RETURN(
      HloInstruction * new_stage,
      ReplacePipelineStageWith(stage, std::move(new_computation),
                               new_stage_operands, false));

  // Add GTEs for any new outputs.
  {
    auto itr = external_uses.begin();
    for (int64 idx = 0; idx != external_uses.size(); ++idx, ++itr) {
      int64 tuple_index = old_num_outputs + idx;
      HloInstruction* inst = itr->first;
      for (HloInstruction* user : itr->second) {
        // Create a separate GTE for each user to preserve the duplicate GTE
        // condition.
        std::vector<int64> op_indices = user->OperandIndices(inst);
        CHECK(op_indices.size());
        for (int64 op_idx : op_indices) {
          TF_ASSIGN_OR_RETURN(HloInstruction * gte,
                              MakeGetTupleElementHlo(new_stage, tuple_index));
          TF_RETURN_IF_ERROR(user->ReplaceOperandWith(op_idx, gte));
        }
      }
    }
  }
  // Remove any dead instructions.
  for (auto itr = ordered_lowering.rbegin(); itr != ordered_lowering.rend();
       ++itr) {
    if ((*itr)->user_count() == 0) {
      TF_RETURN_IF_ERROR(pipeline_computation->ForceRemoveInstruction(*itr));
    }
  }
  TF_RETURN_IF_ERROR(RemovePipelineStageDeadUsers(new_stage));

  return new_stage;
}

StatusOr<std::set<int64>> GetUnusedParametersInPipelineStage(
    const HloInstruction* stage) {
  const HloComputation* stage_computation = stage->to_apply();
  std::set<int64> unused_params;
  for (int64 param_number = 0;
       param_number != stage_computation->num_parameters(); ++param_number) {
    const HloInstruction* parameter =
        stage_computation->parameter_instruction(param_number);
    if (parameter->user_count() == 0) {
      unused_params.insert(param_number);
    }
  }
  return unused_params;
}

StatusOr<HloInstruction*> RemoveParametersFromStage(
    HloInstruction* stage, const std::set<int64>& parameters_to_remove) {
  CHECK(IsPiplineStageOrBackwardOp(stage));
  // Nothing to remove.
  if (parameters_to_remove.empty()) {
    return stage;
  }

  HloComputation* pipeline_computation = stage->parent();

  VLOG(3) << "Removing the following parameters from " << stage->ToString();
  for (int64 param_number : parameters_to_remove) {
    VLOG(3) << "\t* "
            << pipeline_computation->parameter_instruction(param_number)
                   ->ToString();
  }
  HloComputation* stage_computation = stage->to_apply();
  auto builder = HloComputation::Builder(stage_computation->name());

  std::vector<HloInstruction*> new_stage_operands(
      stage_computation->num_parameters() - parameters_to_remove.size());
  int64 next_parameter_number = 0;
  auto next_to_remove_itr = parameters_to_remove.begin();
  // A mapping from instructions in the old computation to the new one which is
  // currently being built.
  absl::flat_hash_map<HloInstruction*, HloInstruction*> old_to_new_computation;
  for (HloInstruction* old_inst :
       stage_computation->MakeInstructionPostOrder()) {
    // Skip the parameter if we are removing it.
    HloInstruction* new_inst;
    if (old_inst->opcode() == HloOpcode::kParameter) {
      if (next_to_remove_itr != parameters_to_remove.end() &&
          old_inst->parameter_number() == *next_to_remove_itr) {
        next_to_remove_itr = std::next(next_to_remove_itr);
        continue;
      } else {
        int64 param_number = old_inst->parameter_number();
        new_inst = builder.AddInstruction(HloInstruction::CreateParameter(
            next_parameter_number, old_inst->shape(), old_inst->name()));
        new_stage_operands[next_parameter_number++] =
            stage->mutable_operand(param_number);
      }
    } else {
      // Get the operands for the instruction we are about to lower.
      std::vector<HloInstruction*> new_operands(old_inst->operand_count());
      absl::c_transform(old_inst->operands(), new_operands.begin(),
                        [&old_to_new_computation](HloInstruction* old_operand) {
                          return old_to_new_computation.at(old_operand);
                        });
      // Clone new instruction.
      new_inst = builder.AddInstruction(
          old_inst->CloneWithNewOperands(old_inst->shape(), new_operands));
    }
    old_inst->SetupDerivedInstruction(new_inst);
    old_to_new_computation[old_inst] = new_inst;
  }
  CHECK_EQ(next_parameter_number, new_stage_operands.size());
  // Build the new computation and the new pipeline stage with new operands.
  HloInstruction* new_root =
      old_to_new_computation.at(stage_computation->root_instruction());
  std::unique_ptr<HloComputation> new_computation = builder.Build(new_root);
  TF_ASSIGN_OR_RETURN(
      HloInstruction * new_stage,
      ReplacePipelineStageWith(stage, std::move(new_computation),
                               new_stage_operands, true));
  return new_stage;
}

PipelineDataflowAnalysis::PipelineDataflowAnalysis(
    const PipelineStages& pipeline_stages)
    : pipeline_stages_(pipeline_stages) {
  // Put stages into lookup tables so that we can quickly get the stage id from
  // an instruction.
  for (int64 id = 0; id != pipeline_stages_.forward.size(); ++id) {
    fwd_stages_lookup_[pipeline_stages_.forward[id]] = id;
  }

  for (int64 id = 0; id != pipeline_stages_.backward.size(); ++id) {
    bwd_stages_lookup_[pipeline_stages_.backward[id]] = id;
  }
};

HloValueSet* PipelineDataflowAnalysis::GetMutableValueSet(
    HloInstruction* inst) {
  return inst_to_value_set_.at(inst).mutable_element(ShapeIndex());
}

const HloValueSet& PipelineDataflowAnalysis::GetValueSet(
    const HloInstruction* inst) const {
  return inst_to_value_set_.at(inst).element(ShapeIndex());
}

HloValueSet* PipelineDataflowAnalysis::CreateValueSet(HloInstruction* inst) {
  return inst_to_value_set_.emplace(inst, InstructionValueSet(inst->shape()))
      .first->second.mutable_element(ShapeIndex());
}

HloValueSet PipelineDataflowAnalysis::GetOperandsValueSet(
    const HloInstruction* inst) {
  std::vector<const HloValueSet*> operand_sets(inst->operand_count());
  absl::c_transform(
      inst->operands(), operand_sets.begin(),
      [&](HloInstruction* operand) { return &GetValueSet(operand); });
  HloValueSet operands_set;
  operands_set.AssignUnionOf(operand_sets);
  return operands_set;
}

HloValue* PipelineDataflowAnalysis::CreateValue(HloInstruction* inst) {
  auto id = next_value_id_++;
  // Create a value and add it to the internal storage.
  return &values_.emplace(id, HloValue(id, inst, ShapeIndex())).first->second;
}

StatusOr<StageID> PipelineDataflowAnalysis::GetStageID(
    const HloInstruction* inst) const {
  if (!IsPiplineStageOrBackwardOp(inst)) {
    return InternalErrorStrCat("Trying to get StageID for ", inst->ToString(),
                               " which is not a PipelineStage(Backward).");
  }

  const bool is_fwd_stage = IsPipelineStage(inst);
  const auto& stages = is_fwd_stage ? fwd_stages_lookup_ : bwd_stages_lookup_;

  auto stage_itr = stages.find(inst);
  if (stage_itr == stages.end()) {
    return InternalErrorStrCat("Could not find the stage for ",
                               inst->ToString());
  }

  return StageID(is_fwd_stage, stage_itr->second);
}

StatusOr<StageID> PipelineDataflowAnalysis::GetPreviousStageID(
    const HloInstruction* inst) const {
  TF_ASSIGN_OR_RETURN(StageID stage_id, GetStageID(inst));

  if (stage_id.is_forward) {
    if (stage_id.id == 0) {
      return InternalError(
          "Trying to call GetPreviousStageID on PipelineStage ID 0 which is "
          "the first pipeline stage.");
    }
    return StageID(true, stage_id.id - 1);
  } else {
    if (stage_id.id == pipeline_stages_.forward.size() - 1) {
      return StageID(true, stage_id.id);
    } else {
      return StageID(false, stage_id.id + 1);
    }
  }
}

Status PipelineDataflowAnalysis::VerifyPipelineUsage(
    const HloInstruction* pipeline_stage,
    const HloInstruction* pipeline_stage_user) const {
  if (pipeline_stage == pipeline_stage_user) {
    return Status::OK();
  }
  // After lowering, a PipelineStage(Backward) can be used:
  // (1) By the next pipeline stage.
  // (2) By the corresponding backward pipeline stage.
  // Any other use is illegal.
  TF_ASSIGN_OR_RETURN(StageID pipeline_stage_id, GetStageID(pipeline_stage));
  TF_ASSIGN_OR_RETURN(StageID pipeline_stage_user_id,
                      GetStageID(pipeline_stage_user));

  if (pipeline_stage_id.is_forward == pipeline_stage_user_id.is_forward) {
    // Case (1).
    // If fwd and the user is the next pipeline stage (asscending).
    if (pipeline_stage_id.is_forward &&
        (pipeline_stage_id.id + 1) == pipeline_stage_user_id.id) {
      return Status::OK();
    }
    // If bwd and the user is the next pipeline stage (descending).
    if (!pipeline_stage_id.is_forward &&
        pipeline_stage_id.id == (pipeline_stage_user_id.id + 1)) {
      return Status::OK();
    }
  } else {
    if (pipeline_stage_id.is_forward) {
      CHECK(!pipeline_stage_user_id.is_forward);
      // Handle case (2).
      if (pipeline_stage_id.id == pipeline_stage_user_id.id) {
        return Status::OK();
      }
    }
  }

  // Everything else is an error.
  return InternalErrorStrCat(
      "Trying to use an output of a PipelineStage",
      (pipeline_stage_id.is_forward ? "" : "Backward"), " with ID ",
      pipeline_stage_id.id, " as an input to a PipelineStage",
      (pipeline_stage_user_id.is_forward ? "" : "Backward"), " with ID ",
      pipeline_stage_user_id.id,
      " which violates the data flow constraints for Pipelines.");
}

Status PipelineDataflowAnalysis::VerifyParameterUsage(
    const HloInstruction* parameter,
    const HloInstruction* pipeline_stage_user) {
  CHECK_EQ(parameter->opcode(), HloOpcode::kParameter);
  TF_ASSIGN_OR_RETURN(StageID stage_id, GetStageID(pipeline_stage_user));
  // Get the parameter value and check where it is used.
  const HloValue& parameter_value = GetValueSet(parameter).GetUniqueValue();
  for (const HloInstruction* user_stage :
       used_by_stages_.at(&parameter_value)) {
    TF_ASSIGN_OR_RETURN(StageID user_stage_id, GetStageID(user_stage));
    if (stage_id.id != user_stage_id.id) {
      return UnimplementedStrCat(
          "The PipelineStage", (stage_id.is_forward ? "" : "Backward"),
          " with ID ", stage_id.id,
          " is trying to use an input to is already used by  the PipelineStage",
          (user_stage_id.is_forward ? "" : "Backward"), " with ID ",
          user_stage_id.id,
          ". This violates the dataflow "
          " constraints because an input can only be used by a single"
          " PipelineStage.");
    }
  }
  return Status::OK();
};

Status PipelineDataflowAnalysis::VerifyPipelineStageOperands(
    const HloInstruction* pipeline_stage, const HloValueSet& new_inputs) {
  CHECK(IsPiplineStageOrBackwardOp(pipeline_stage));
  TF_ASSIGN_OR_RETURN(StageID stage_id, GetStageID(pipeline_stage));
  // Get all the values used by the operands of inst.
  HloValueSet operands_set = GetOperandsValueSet(pipeline_stage);
  operands_set.AssignUnionOf({&operands_set, &new_inputs});

  for (const HloValue* value : operands_set.values()) {
    HloInstruction* producer = value->instruction();
    switch (producer->opcode()) {
      case HloOpcode::kParameter: {
        TF_RETURN_IF_ERROR(VerifyParameterUsage(producer, pipeline_stage));
        // A parameter can only be lowered if it was used in the fwd
        // stage.
        const HloInstruction* fwd_stage = pipeline_stages_.forward[stage_id.id];
        if (!fwd_stage->IsUserOf(producer)) {
        }
        break;
      }
      case HloOpcode::kCall: {
        if (IsPiplineStageOrBackwardOp(producer)) {
          TF_RETURN_IF_ERROR(VerifyPipelineUsage(producer, pipeline_stage));
          break;
        }
      }
      default: { return InternalError("Invalid producer in the pipelines."); }
    }
  }
  return Status::OK();
}

StatusOr<bool> PipelineDataflowAnalysis::HasToBeLowered(
    const HloInstruction* inst) const {
  const HloInstruction* root_instruction = inst->parent()->root_instruction();
  // A root never needs lowering.
  if (root_instruction == inst) {
    return false;
  }

  switch (inst->opcode()) {
    case HloOpcode::kCall:
      return !IsPiplineStageOrBackwardOp(inst);
    case HloOpcode::kParameter:
      return false;
    case HloOpcode::kGetTupleElement: {
      // A GTE on the output from a PipelineStage(Backward) needs to be lowered
      // unless:
      // (1) It is used by the next pipeline stage.
      // (2) It is used by the corresponding backward pipeline stage.
      // It is an error if a GTE on the output from a PipelineStage(Backward) is
      // used by any other pipeline stage.
      //
      // Any other GTE needs to be lowered.
      if (IsPiplineStageOrBackwardOp(inst->operand(0))) {
        // DuplicateGTEEdges should make sure each GTE only has one user.
        const HloInstruction* gte_input = inst->operand(0);
        if (inst->user_count() != 1) {
          return InternalErrorStrCat("Expected instruction ",
                                     gte_input->ToString(),
                                     " to have exactly one user.");
        }
        const HloInstruction* gte_user = inst->users()[0];
        // Verify that the pipeline usage is legal. If the user is not a
        // PipelineStage(Backward) then the user will be lowered later.
        if (IsPiplineStageOrBackwardOp(gte_user)) {
          TF_RETURN_IF_ERROR(VerifyPipelineUsage(gte_input, gte_user));
        }
        return false;
      } else {
        // Any other GTE has to be lowered.
        return true;
      }
    }
    default:
      return true;
  }
}

Status PipelineDataflowAnalysis::UpdateThroughInstruction(
    HloInstruction* inst) {
  HloValueSet operands_value_set = GetOperandsValueSet(inst);
  if (IsProducerOp(inst)) {
    // Producers create their sets.
    if (IsPiplineStageOrBackwardOp(inst)) {
      // Mark values as used by the stage.
      for (const HloValue* value : operands_value_set.values()) {
        used_by_stages_[value].insert(inst);
      }
      // Make sure the operands of the stage do not violate the data flow
      // constraints.
      TF_RETURN_IF_ERROR(VerifyPipelineStageOperands(inst));
    } else {
      CHECK(operands_value_set.values().empty());
    }
    HloValueSet* value_set = CreateValueSet(inst);
    value_set->AddValue(CreateValue(inst));
  } else {
    // Forward all the from operands.
    HloValueSet* value_set = CreateValueSet(inst);
    value_set->AssignUnionOf({&operands_value_set});
  }
  return Status::OK();
}

StatusOr<std::unique_ptr<PipelineDataflowAnalysis>>
PipelineDataflowAnalysis::GetAnalysis(const PipelineStages& pipeline_stages) {
  auto analysis = absl::make_unique<PipelineDataflowAnalysis>(pipeline_stages);

  if (analysis->pipeline_stages_.forward.size()) {
    HloComputation* pipeline_computation =
        analysis->pipeline_stages_.forward[0]->parent();
    for (HloInstruction* inst :
         pipeline_computation->MakeInstructionPostOrder()) {
      TF_RETURN_IF_ERROR(analysis->UpdateThroughInstruction(inst));
    }
  }

  return std::move(analysis);
}
}  // namespace poplarplugin
}  // namespace xla
