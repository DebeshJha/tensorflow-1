/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_KERNELS_FUNCTIONAL_FUNCTIONAL_UTIL_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_KERNELS_FUNCTIONAL_FUNCTIONAL_UTIL_H_
#include <vector>

#include "tensorflow/compiler/tf2xla/xla_compiler.h"
#include "tensorflow/core/framework/function.h"
#include "tensorflow/core/graph/graph.h"

namespace tensorflow {
namespace poplarplugin {
// Return the default compilation comptions for functions.
XlaCompiler::CompileOptions GetDefaultCompileOptions();

// Function which tries to get all the arguments to the Op. It tries to evaluate
// any constant inputs to a value so that they can be propagated.
xla::StatusOr<std::vector<XlaCompiler::Argument>> GetXlaArguments(
    XlaOpKernelContext* ctx, const DataTypeVector& input_types,
    int* num_resource_args);

// Function which gets all non-constant function inputs.
xla::StatusOr<std::vector<xla::XlaOp>> GetXlaInputs(
    XlaOpKernelContext* ctx,
    const std::vector<XlaCompiler::Argument>& arguments,
    const std::vector<int>& input_mapping);
}  // namespace poplarplugin
}  // namespace tensorflow

#endif  // TENSORFLOW_COMPILER_PLUGIN_POPLAR_KERNELS_FUNCTIONAL_FUNCTIONAL_UTIL_H_
