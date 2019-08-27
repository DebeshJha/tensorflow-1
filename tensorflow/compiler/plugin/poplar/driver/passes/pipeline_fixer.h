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

#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_PASSES_PIPELINE_FIXER_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_PASSES_PIPELINE_FIXER_H_

#include "tensorflow/compiler/plugin/poplar/driver/tools/pipeline_util.h"

#include "tensorflow/compiler/xla/service/hlo_pass_interface.h"

namespace xla {

class HloModule;

namespace poplarplugin {

/**
 * For pipelines we want a well defined dataflow, with the data flowing
 * from one stage to another and all the operations being performed inside the
 * pipeline stages. Current TF API for pipelinining does not guarantee
 * that dataflow, as for example gradient calculation might generate ops outside
 * of the pipeline stages.
 *
 * This pass uses simple data flow analysis in order to lower ops into stages
 * such that:
 * 1. Each parameter instruction is only used by a pipeline stage and/or its
 *    corresponding backward pipeline stage. Any other uses are illegal.
 * 2. An output of a pipeline stage can only be used by the next pipeline stage
 *    and/or its corresponding backward pipeline stage. Any other uses are
 *    illegal.
 */
class PipelineFixer : public HloModulePass {
 public:
  absl::string_view name() const override { return "pipeline-fixer"; }

  StatusOr<bool> Run(HloModule* module) override;

 private:
  // Fixes a pipeline.
  StatusOr<bool> FixPipeline(HloInstruction* pipeline_op);

  // Performs the lowering (if possible) of any operations which need to be
  // lowered for this pipeline to be correct.
  StatusOr<bool> LowerOpsIntoPipelineStages();

  // This function lowers inputs to the pipeline stage which are
  // arguments to the stage, but can be lowered (for examples constants etc.).
  // Returns true if the stage has changed.
  StatusOr<bool> LowerPipelineStagesInputs();

  // Lowers any outputs of the stage into the stage.
  // Returns true if the stage has changed.
  StatusOr<bool> LowerPipelineStagesOutputs();

  // A PipelineStage is being replaced - update internal storage.
  Status UpdateStage(const StageID& stage_id, HloInstruction* new_stage);

  // Get the pipeline stage given id.
  HloInstruction* GetStage(const StageID& stage_id);

  // Get the stages in order they are sequentially executed.
  std::vector<HloInstruction*> GetOrderedStages();

  PipelineStages stages_;
};

}  // namespace poplarplugin
}  // namespace xla

#endif
