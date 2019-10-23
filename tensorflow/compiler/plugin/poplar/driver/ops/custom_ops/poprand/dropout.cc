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

#include "tensorflow/compiler/plugin/poplar/driver/ops/custom_ops/poplibs_ops.h"
#include "tensorflow/compiler/plugin/poplar/driver/tensor.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/custom_ops/pooling.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/util.h"
#include "tensorflow/compiler/plugin/poplar/kernels/custom_kernels_util.h"

#include <popops/ElementWise.hpp>
#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/custom_ops/dropout_hlo.h"
#include "tensorflow/compiler/xla/service/hlo_casting_utils.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/core/lib/core/errors.h"

#include <poputil/TileMapping.hpp>
#include <random>

namespace {
std::once_flag seed_flag;
}

namespace xla {
namespace poplarplugin {
namespace {
class DropoutOp : public PoplibsOpDef {
  StatusOr<poplar::program::Program> Creator(poplar::Graph& graph,
                                             CompilerResources& res,
                                             const HloInstruction* inst,
                                             const xla::Shape& output_shape,
                                             TensorMap& tensor_map) override {
    // Seed the random number generator exactly once. If we don't wrap it we
    // will likely be seeding it with the same value as subsequent calls to this
    // Creator function are likely to be less than a second apart.
    std::call_once(seed_flag, []() { std::srand(std::time(nullptr)); });
    poplar::program::Sequence seq;
    // Get the "x" tensor, aka the input.
    TF_ASSIGN_OR_RETURN(
        poplar::Tensor input,
        FindInstructionInput(tensor_map, res, inst, 0, seq, false));

    TF_ASSIGN_OR_RETURN(
        poplar::Tensor in_seed,
        FindInstructionInput(tensor_map, res, inst, 1, seq, false));

    const HloDropoutInstruction* dropout_instruction =
        dynamic_cast<const HloDropoutInstruction*>(inst);
    assert(dropout_instruction &&
           "Expected operation to be an xla::poplarplugin::DropoutOp");

    // The probabilty that any given element of "x" will be discarded.
    double rate = dropout_instruction->Rate();

    // The value to scale the non-dropped elements by.
    double scale = dropout_instruction->Scale();

    // If false, pass in user_seed, else pass in global_seed.
    bool is_user_seed = dropout_instruction->IsUserSeed();
    int32_t seed_modifier = dropout_instruction->SeedModifier();

    // Create an empty tensor for the dropout. This is internal to the poprand
    // implementation but is exposed anyway so we need to provide it.
    TF_ASSIGN_OR_RETURN(
        poplar::Tensor reference,
        AddPlainTensor(
            graph, GetDebugName(inst) + "/Reference",
            XlaShapeFromPoplarShape(xla::PrimitiveType::F32, input.shape()),
            res, false));

    xla::Shape seed_shape =
        XlaShapeFromPoplarShape(xla::PrimitiveType::U32, {2});

    // By default we will use any seed provided by the user.
    poplar::Tensor* seed_to_use = &in_seed;

    poplar::Tensor global_seed_tensor;
    // If we aren't using a user provided seed we need to create a temp seed and
    // use that.
    if (!is_user_seed) {
      // Create the variable to hold the seed state.
      global_seed_tensor = graph.addVariable(poplar::INT, {2});
      poputil::mapTensorLinearly(graph, global_seed_tensor);

      // Make sure the random seed has a random number to make each operation
      // less deterministic.
      int random_seed[] = {std::rand(), std::rand()};
      graph.setInitialValue(global_seed_tensor,
                            poplar::ArrayRef<int>(random_seed, 2));

      // Previously we just added one to the seed tensor but it turns out that
      // could give very deterministic outputs so now we increment the seed by a
      // random number generated by the same seed. This keeps the incrementing
      // of the seed deterministic while giving us better seeds on each
      // iteration.
      poplar::Tensor temp_seed =
          global_seed_tensor.reinterpret(poplar::UNSIGNED_INT);

      // Also add the IPU number so each replica has a different seed.
      poplar::Tensor indexConstant =
          graph.addReplicationIndexConstant().reshape({1});
      graph.setTileMapping(indexConstant, 0);

      if (indexConstant.elementType() == poplar::UNSIGNED_INT) {
        indexConstant = indexConstant.reinterpret(poplar::INT);
      }

      poplar::Tensor one_tensor = graph.addConstant(poplar::INT, {1}, 1);
      graph.setTileMapping(one_tensor, 0);

      auto increment_tensor = poplar::concat(indexConstant, one_tensor);
      popops::addInPlace(graph, global_seed_tensor, increment_tensor, seq,
                         GetDebugName(inst) + "/AddIncrement");
      seed_to_use = &global_seed_tensor;
    }

    // Dropout expects an unsigned int but tensorflow takes in int32 when
    // targeting IPU.
    poplar::Tensor as_unsgined = seed_to_use->reinterpret(poplar::UNSIGNED_INT);

    // Perform the actual dropout by calling into the poprand function.
    poplar::Tensor final_output =
        poprand::dropout(graph, &as_unsgined, seed_modifier, input, reference,
                         rate, scale, seq, GetDebugName(inst));

    // If this operation has an allocation target allocate a tensor of that
    // layout and copy the result into it after the random numbers have been
    // generated.
    if (HasTensorAllocationTarget(std::make_pair(inst, 0), res)) {
      TF_ASSIGN_OR_RETURN(poplar::Tensor new_out,
                          AddTensor(graph, std::make_pair(inst, 0),
                                    output_shape, res, tensor_map));
      seq.add(poplar::program::Copy(final_output, new_out));
      final_output = new_out;
    }

    // Mark that tensor as our output.
    TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, final_output));
    TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 1, *seed_to_use));

    return seq;
  }
};

REGISTER_POPLIBS_OP(Poprand, Dropout, DropoutOp);

}  // namespace
}  // namespace poplarplugin
}  // namespace xla
