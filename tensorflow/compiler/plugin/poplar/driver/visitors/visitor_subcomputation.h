/* Copyright 2017 Graphcore Ltd
 */

/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_VISITORS_VISITOR_SUBCOMPUTATION_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_VISITORS_VISITOR_SUBCOMPUTATION_H_

#include "tensorflow/compiler/plugin/poplar/driver/visitors/deferred_allocation_visitor.h"

#include "absl/container/flat_hash_map.h"

namespace poplar {
class Graph;
class Tensor;
}  // namespace poplar

namespace xla {
class HloParameterInstruction;
namespace poplarplugin {

using TensorInputDescription = std::vector<std::vector<bool>>;

class SubComputationVisitor : public DeferredAllocationVisitor {
 public:
  SubComputationVisitor(CompilerResources& res, const ArgVectors& inputs,
                        const std::vector<const SubComputationVisitor*>&
                            dependent_subcomputations = {});

  Status HandleParameter(HloInstruction* inst) override;
  Status FinishVisit(HloInstruction* inst) override;

  const ArgVectors& inputs() const;

  const OutVector& outputs() const;

  bool InputIsAllocated(int64 param, unsigned int index) const;

  bool InputIsUsed(int64 param, unsigned int index) const;

  bool InputHasAllocationTarget(int64 param, unsigned int index) const;

 protected:
  virtual StatusOr<bool> HandleTensor(HloParameterInstruction* inst,
                                      Shape& shape, const uint64 tuple_index,
                                      poplar::Tensor& tensor);

  StatusOr<poplar::Tensor> PostProcessParameterAllocation(
      const HloInstruction* inst, int64 flat_tuple_index, const Shape& shape,
      poplar::Tensor tensor) override;

  bool InputIsUsedInThisSubComputation(HloParameterInstruction* inst,
                                       const std::vector<xla::Shape>& shapes,
                                       unsigned int index);
  bool InputIsUsedInDependentSubComputations(HloParameterInstruction* inst,
                                             unsigned int index);
  ArgVectors temp_inputs_;
  ArgVectors inputs_;
  OutVector outputs_;

  const std::vector<const SubComputationVisitor*>& dependent_subcomputations_;

  // Allocated tensors for inputs which are used by this subcomputation only.
  TensorInputDescription used_tensors_;
  // Allocated tensors for inputs which are used by this or dependent
  // subcomputations.
  TensorInputDescription allocated_tensors_;
  // Allocated tensors which have an allocation target;
  TensorInputDescription has_allocation_target_;
};

// Similar to SubComputationVisitor, but the inputs are used inplace.
class InplaceSubComputationVisitor : public SubComputationVisitor {
 public:
  InplaceSubComputationVisitor(CompilerResources& res, const ArgVectors& inputs,
                               const TensorInputDescription& input_has_layout,
                               const std::vector<const SubComputationVisitor*>&
                                   dependent_subcomputations = {});

  // Version of the visitor which assumes all inputs have a layout.
  InplaceSubComputationVisitor(CompilerResources& res, const ArgVectors& inputs,
                               const std::vector<const SubComputationVisitor*>&
                                   dependent_subcomputations = {});

  StatusOr<bool> HandleTensor(HloParameterInstruction* inst, Shape& shape,
                              const uint64 tuple_index,
                              poplar::Tensor& tensor) override;

  // For each operand to the inplace subcomputation, check if the tensor coming
  // in has a layout. If the tensor does not have a layout then the inplace
  // subcomputation visitor might create one for this tensor.
  static StatusOr<TensorInputDescription> GetInplaceSubcomputationLayoutInfo(
      CompilerResources& res, const HloInstruction* inst);

  // Even though computation is inplace, some of the computation inputs might
  // allocate their inputs as they have allocation targets. In these cases make
  // sure to copy the values of the tensors.
  poplar::program::Sequence GetPreambleCopies();

  // If the subcomputation is used as a loop, then add input/output aliasing
  // copies. Returns the loop state (i.e. the output of the loop).
  StatusOr<ArgVector> AddLoopInputOutputAliasingCopies(
      poplar::Graph& graph, const HloComputation* computation,
      const std::string& debug_name);

 protected:
  // Given the flat tensor index, get the sequence the copy should be inserted
  // into.
  virtual poplar::program::Sequence& GetSequenceForAliasingCopy(
      int64 flat_tensor_index, const HloComputation* computation);

  // Given an output flat index get the corresponding paramter number and flat
  // index.
  std::pair<int64, int64> GetParameterNumberAndFlatIndex(
      int64 output_flat_index);

 private:
  // Indicates whether the input has a layout.
  TensorInputDescription input_has_layout_;
};

}  // namespace poplarplugin
}  // namespace xla

#endif
