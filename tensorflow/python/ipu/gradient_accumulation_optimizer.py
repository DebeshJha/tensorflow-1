# Copyright 2019 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# =============================================================================
"""
Optimizer wrappers which perform local gradient accumulation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
"""

from tensorflow.python.ipu.optimizers.gradient_accumulation_optimizer import *  # pylint: disable=wildcard-import,unused-wildcard-import,enable=unused-import
from tensorflow.python.platform import tf_logging

tf_logging.warning(
    "tensorflow.python.ipu.gradient_accumulation_optimizer has been moved to "
    "tensorflow.python.ipu.optimizers.gradient_accumulation_optimizer. This "
    "namespace will be removed in the future.")
