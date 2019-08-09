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

#include "tensorflow/compiler/plugin/poplar/driver/tools/custom_ops/user_op_hlo.h"
#include "tensorflow/compiler/plugin/poplar/kernels/custom_kernels_util.h"
#include "tensorflow/compiler/plugin/poplar/kernels/poplibs_ops.pb.h"

#include "absl/strings/str_cat.h"

namespace xla {
namespace poplarplugin {

HloUserOpInstruction::HloUserOpInstruction(
    absl::Span<HloInstruction* const> inputs, const Shape& shape,
    const std::string& path, void* fn_ptr, void* elementwise_fn_ptr,
    void* allocate_input_fn_ptr, bool is_gradient)
    : HloPoplarInstruction(
          shape, inputs,
          GetPoplibsCustomOpTargetString(PoplibsOp::Poputil, PoplibsOp::UserOp),
          fn_ptr, elementwise_fn_ptr, allocate_input_fn_ptr, path, is_gradient),
      function_ptr_(fn_ptr),
      elementwise_ptr_(elementwise_fn_ptr),
      allocate_input_ptr_(allocate_input_fn_ptr),
      gp_path(path),
      is_gradient_(is_gradient) {
  set_custom_call_has_side_effect(true);
  num_inputs_ = inputs.size();
}

absl::flat_hash_set<int64> HloUserOpInstruction::AllocatingIndices() const {
  return {};
}

absl::flat_hash_map<int64, int64> HloUserOpInstruction::LayoutDependencies()
    const {
  return {};
}

uint64 HloUserOpInstruction::NumberOfInplaceOperands() const { return 0; }

bool HloUserOpInstruction::IsPopOpsElementwise() const {
  bool (*ElementwiseFn)();

  if (elementwise_ptr_ != nullptr) {
    return reinterpret_cast<decltype(ElementwiseFn)>(elementwise_ptr_)();
  } else {
    return false;
  }
}

std::vector<string> HloUserOpInstruction::ExtraPoplarAttributesToStringImpl(
    const HloPrintOptions& options) const {
  std::stringstream ss;
  ss << function_ptr_;
  std::string function_ptr_address = ss.str();
  ss.clear();

  ss << elementwise_ptr_;
  std::string elementwise_ptr_address = ss.str();
  ss.clear();

  ss << allocate_input_ptr_;
  std::string allocate_input_ptr_address = ss.str();

  std::vector<string> attributes;
  attributes.push_back(absl::StrCat("function_ptr=", function_ptr_address));
  attributes.push_back(
      absl::StrCat("elementwise_ptr_=", elementwise_ptr_address));
  attributes.push_back(
      absl::StrCat("allocate_input_ptr_=", allocate_input_ptr_address));
  attributes.push_back(absl::StrCat("num_inputs_=", num_inputs_));
  attributes.push_back(absl::StrCat("gp_path=", gp_path));

  return attributes;
}

std::unique_ptr<HloInstruction> HloUserOpInstruction::CloneWithNewOperandsImpl(
    const Shape& shape, absl::Span<HloInstruction* const> new_operands,
    HloCloneContext*) const {
  return CreateUserOp(new_operands, shape, GetPath(), function_ptr_,
                      elementwise_ptr_, allocate_input_ptr_, is_gradient_);
}

std::unique_ptr<HloInstruction> CreateUserOp(
    absl::Span<HloInstruction* const> inputs, const Shape& shape,
    const std::string& gp_path, void* function_ptr, void* elementwise_fn,
    void* allocate_fn, bool is_gradient) {
  return absl::make_unique<HloUserOpInstruction>(inputs, shape, gp_path,
                                                 function_ptr, elementwise_fn,
                                                 allocate_fn, is_gradient);
}

namespace {

static HloPoplarInstructionFactory user_op_factory(
    GetPoplibsCustomOpTargetString(PoplibsOp::Poputil, PoplibsOp::UserOp),
    [](HloCustomCallInstruction* call)
        -> StatusOr<std::unique_ptr<xla::HloInstruction>> {
      auto attribute_map = IPUCustomKernelsUtil::AttributeMap(call);

      TF_ASSIGN_OR_RETURN(uint64 operation_fn,
                          attribute_map.GetAttributeAsUInt64("operation_fn"));
      void* operation_fn_ptr = reinterpret_cast<void*>(operation_fn);

      TF_ASSIGN_OR_RETURN(uint64 elementwise_fn,
                          attribute_map.GetAttributeAsUInt64("elementwise_fn"));
      void* elementwise_fn_ptr = reinterpret_cast<void*>(elementwise_fn);

      TF_ASSIGN_OR_RETURN(
          uint64 allocate_input_fn,
          attribute_map.GetAttributeAsUInt64("allocate_input_fn"));
      void* allocate_input_fn_ptr = reinterpret_cast<void*>(allocate_input_fn);

      TF_ASSIGN_OR_RETURN(std::string gp_path,
                          attribute_map.GetAttributeAsString("gp_path"));

      TF_ASSIGN_OR_RETURN(bool is_gradient,
                          attribute_map.GetAttributeAsBool("is_gradient"));

      return CreateUserOp(call->operands(), call->shape(), gp_path,
                          operation_fn_ptr, elementwise_fn_ptr,
                          allocate_input_fn_ptr, is_gradient);
    });
}  // namespace

}  // namespace poplarplugin
}  // namespace xla
