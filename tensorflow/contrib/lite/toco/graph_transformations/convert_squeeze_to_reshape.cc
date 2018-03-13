/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "absl/strings/str_cat.h"
#include "tensorflow/contrib/lite/toco/graph_transformations/graph_transformations.h"
#include "tensorflow/contrib/lite/toco/model.h"
#include "tensorflow/contrib/lite/toco/tooling_util.h"
#include "tensorflow/core/platform/logging.h"

namespace toco {

// Replaces a tf.squeeze operator with a reshape.
// Squeeze removes dimensions == 1 (if in the list of squeeze_dims). This
// means that the data layout will never change with this op, just the shape.
// By converting these to reshapes once we have run shape propagation we allow
// standard reshape optimization transforms to do their magic.
bool ConvertSqueezeToReshape::Run(Model* model, std::size_t op_index) {
  auto squeeze_it = model->operators.begin() + op_index;
  if (squeeze_it->get()->type != OperatorType::kSqueeze) {
    return false;
  }
  auto squeeze_op = static_cast<SqueezeOperator*>(squeeze_it->get());
  CHECK_EQ(squeeze_op->inputs.size(), 1);
  CHECK_EQ(squeeze_op->outputs.size(), 1);

  const auto& input_array = model->GetArray(squeeze_op->inputs[0]);
  if (!input_array.has_shape()) {
    // Yield until input dims have been resolved.
    return false;
  }
  if (input_array.shape().dimensions_count() == 0) {
    // Input array cannot be 0-D.
    return false;
  }
  if (!model->HasArray(squeeze_op->outputs[0]) ||
      !model->GetArray(squeeze_op->outputs[0]).has_shape()) {
    // Yield until shape propagation has set the output shape for us.
    return false;
  }

  // We use the output shape that has been calculated by shape propagation.
  const auto& output_shape = model->GetArray(squeeze_op->outputs[0]).shape();

  auto* reshape_op = new TensorFlowReshapeOperator;
  reshape_op->inputs = {
      squeeze_op->inputs[0],
      CreateInt32Array(model, squeeze_op->outputs[0] + "_shape",
                       output_shape.dims()),
  };
  reshape_op->outputs = squeeze_op->outputs;

  AddMessageF("Replacing %s with %s", LogName(*squeeze_op),
              LogName(*reshape_op));

  // Replace the operator in the graph.
  const auto reshape_it = model->operators.emplace(squeeze_it, reshape_op);
  squeeze_it = reshape_it + 1;
  CHECK_EQ(squeeze_it->get(), squeeze_op);
  model->operators.erase(squeeze_it);

  return true;
}

}  // namespace toco
