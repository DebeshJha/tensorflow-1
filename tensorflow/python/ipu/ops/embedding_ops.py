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
# ==============================================================================
"""
Popops embedding operators
~~~~~~~~~~~~~~~~~~~~~~~~~~
"""

from functools import reduce
from operator import mul

from tensorflow.compiler.plugin.poplar.ops import gen_popops_ops
from tensorflow.python.ipu.ops import functional_ops
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import custom_gradient
from tensorflow.python.ops import clip_ops
from tensorflow.python.ops import variable_scope
from tensorflow.python.ops import variables
from tensorflow.python.util import deprecation
from tensorflow.python.framework import ops
from tensorflow.python.framework import tensor_util

from tensorflow.compiler.plugin.poplar.ops import gen_pop_datastream_ops


@deprecation.deprecated_args(None, "stop passing this argument.",
                             "one_hot_threshold", "min_encoding_size")
def embedding_lookup(params,
                     ids,
                     name=None,
                     serialization_factor=1,
                     one_hot_threshold=0,
                     min_encoding_size=1216):
  """Looks up `ids` in a list of embedding tensors.

    This is designed to be a drop-in replacement for the typical use cases with
    `tf.nn.embedding_lookup` for the IPU.

    Args:
        params: A single tensor representing the complete embedding tensor.
        ids: A `Tensor` with type `int32` containing the slices to be extracted
             from `params`.
        name: A name for the operation.
        serialization_factor: If greater than 1, the embedding lookup will be
             broken up into `serialization_factor` smaller lookups, serialized
             along the 0th dimension. This option should not be used unless
             `params` is used by another operation, such as matrix
             multiplication. If `params` has multiple users, then serialization
             can reduce the maximum memory at the cost of extra computation.
        one_hot_threshold: Deprecated.
        min_encoding_size: Deprecated.
    Returns:
        A `Tensor` with the same type as the tensors in `params`.
    """
  serialization_factor = int(serialization_factor)
  if serialization_factor < 1:
    raise ValueError(
        'serialization_factor has to be at least 1, but was {}.'.format(
            serialization_factor))

  name = name or "embedding_lookup"
  ids_shape = ids.shape.as_list()
  params_shape = params.shape.as_list()

  # Flatten all the indices.
  num_ids = reduce(mul, ids_shape, 1)
  ids_flat = array_ops.reshape(ids, [num_ids])

  # Flatten params into a 2D shape.
  slice_dim_size = params_shape.pop(0)
  embedding_size = reduce(mul, params_shape, 1)
  params_2d = array_ops.reshape(params, [slice_dim_size, embedding_size])

  if (slice_dim_size % serialization_factor) != 0:
    raise ValueError(
        'The serialization_factor ({}) must divide the size of the 0th '
        'dimension of params ({}).'.format(serialization_factor,
                                           slice_dim_size))

  # Do the lookup.
  if serialization_factor == 1:
    result = gen_popops_ops.ipu_multi_slice(params_2d, ids_flat, name=name)
  else:

    @custom_gradient.custom_gradient
    def serialized_embedding_lookup(table, indices):
      @functional_ops.function
      def func(sliced_table, indices, min_idx, max_idx):
        # Do a serialized embedding lookup by adjusting the indices.
        adjusted_indices = indices - min_idx
        x = gen_popops_ops.ipu_multi_slice(sliced_table,
                                           adjusted_indices,
                                           name=name)

        # Mask out any outputs which are not in range [min_idx, max_idx).
        mask_max = indices < max_idx
        mask_min = indices >= min_idx
        mask = math_ops.logical_and(mask_max, mask_min)
        mask = array_ops.expand_dims(mask, 1)
        return array_ops.where_v2(mask,
                                  x,
                                  array_ops.constant(0, x.dtype),
                                  name=name + "/Mask")

      table_shape = table.shape.as_list()
      assert len(table_shape) == 2
      assert (table_shape[0] % serialization_factor) == 0
      split_size = table_shape[0] // serialization_factor

      # Create the first lookup.
      table_sliced = array_ops.slice(table, [0, 0],
                                     [split_size, table_shape[1]])
      output = func(table_sliced, indices, 0, split_size)

      for i in range(1, serialization_factor):
        min_idx = split_size * i
        max_idx = split_size * (i + 1)

        table_sliced = array_ops.slice(table, [min_idx, 0],
                                       [split_size, table_shape[1]])
        masked_output = func(table_sliced, indices, min_idx, max_idx)
        # Add the masked slice
        output = math_ops.add(output, masked_output, name=f"slice_{i}")

      # Need to redefine the gradient function.
      def grad(*dy):
        return [
            gen_popops_ops.ipu_multi_update_add(array_ops.zeros_like(table),
                                                indices=indices,
                                                updates=dy[0],
                                                scale=array_ops.constant(
                                                    1, table.dtype)), None
        ]

      return output, grad

    result = serialized_embedding_lookup(params_2d, ids_flat)

  # Reshape into [ids[0], ... , ids[n - 1], params[1], ..., params[n - 1]]
  return array_ops.reshape(result, list(ids_shape) + list(params_shape))


class HostEmbeddingOptimizerSpec:
  """ Description of the Host Embedding optimizer.

      Despite the embedding living on the host, we want to compute the gradients
      on the device. Additionally, the communication channel between the device
      and host is opaque to TensorFlow. For these reasons we need to describe
      the optimizer parameters separately.

      Currently only supports SGD.

  """
  def __init__(self, learning_rate):
    """
    Create a HostEmbeddingOptimizerSpec.

    Args:
        learning_rate: The SGD learning rate.

    """
    self._learning_rate = learning_rate

  def get_learning_rate(self):
    return self._learning_rate


class HostEmbedding:
  """ Host Embedding wrapper.

      HostEmbedding encapsulates the embedding tensor and the additional
      meta-data required to coordinate the host embedding and the device lookup.
      Through an instance of this class, an IPU can perform lookups on an
      embedding that resides on the host.

      It is assumed that the given embedding will be rank two where the
      outermost dimension (dimension zero) is the token dimension, and the
      innermost dimension is the encoding dimension.

  """
  def __init__(self,
               name,
               embedding_tensor,
               partition_strategy="TOKEN",
               optimizer_spec=None):
    """
    Create a HostEmbedding.

    Args:
        name: The name which uniquely identifies the embedding.
        embedding_tensor: The tensor which holds the embedding.
        optimizer_spec: A description of how the embedding will be optimized.
            When `None`, the embedding is assumed to not be trainable.
    """
    if not tensor_util.is_tensor(embedding_tensor):
      raise ValueError(
          "HostEmbedding embedding_tensor is not a tensorflow tensor")

    if not isinstance(optimizer_spec,
                      (type(None), HostEmbeddingOptimizerSpec)):
      raise ValueError(
          "HostEmbedding optimizer_spec is not a HostEmbeddingOptimizerSpec" +
          " or None")

    if partition_strategy not in ["TOKEN", "ENCODING"]:
      raise ValueError("Unknown partition strategy " + str(partition_strategy))

    self._name = name
    self._embedding_tensor = embedding_tensor
    self._partition_strategy = partition_strategy
    self._optimizer_spec = optimizer_spec
    self._has_lookup = False

  @deprecation.deprecated_args(None, "This argument no longer has any effect.",
                               "iteration_count", "replication_factor",
                               "training")
  def __call__(self, iteration_count=0, replication_factor=1, training=True):
    """ Register the host embedding with the session.

        Args:
          iteration_count: The number of iterations in the user model.
          replication_factor: The replication count of the user graph.
          training: Whether this host embedding will be trained on this run.
                    This allows the user to specify that the embedding won't be
                    updated, despite the construction of gradient operations.
                    This is useful for validation, using the training graph.
        Returns:
          A TensorFlow op which will serve the embedding to the device.
    """
    if self._has_lookup:
      return gen_pop_datastream_ops.ipu_host_embedding(self._embedding_tensor,
                                                       self._name)
    return self._embedding_tensor

  @deprecation.deprecated_args(None, "This argument no longer has any effect.",
                               "count")
  def lookup(self, indices, count=1, clip_indices=True):
    """ Perform a host embedding lookup on an IPU.

        Args:
          indices: The indices to lookup.
          count: The number of times, per iteration, that this op will be
                 executed.
          clip_indices: Whether to enforce a valid range on the lookup
                        indices with clipping. When False, out-of-range values
                        have undefined behaviour.
        Returns:
          A Tensor containing the elements requested by the user indices.
    """
    indices_shape = indices.shape.as_list()

    # Optionally clip the indices to a safe range
    if clip_indices:
      indices = clip_ops.clip_by_value(indices, 0,
                                       self._embedding_tensor.shape[0] - 1)

    # Flatten the indices.
    num_indices = reduce(mul, indices_shape, 1)
    indices_flat = array_ops.reshape(indices, [num_indices])

    if self._optimizer_spec is not None:
      with variable_scope.variable_scope(self._name,
                                         reuse=variable_scope.AUTO_REUSE):
        dummy = variable_scope.get_variable("__dummy",
                                            shape=[],
                                            dtype=self._embedding_tensor.dtype,
                                            trainable=True)
      result = gen_pop_datastream_ops.ipu_device_embedding_lookup_trainable(
          dummy,
          indices_flat,
          embedding_id=self._name,
          embedding_shape=self._embedding_tensor.shape,
          optimizer="SGD",
          partition_strategy=self._partition_strategy,
          learning_rate=self._optimizer_spec.get_learning_rate())
    else:
      result = gen_pop_datastream_ops.ipu_device_embedding_lookup(
          indices_flat,
          embedding_id=self._name,
          embedding_shape=self._embedding_tensor.shape,
          partition_strategy=self._partition_strategy,
          dtype=self._embedding_tensor.dtype)
    self._has_lookup = True
    # Reshape the result back to the caller's expected shape
    return array_ops.reshape(
        result,
        list(indices_shape) + [self._embedding_tensor.shape[1]])


def create_host_embedding(name,
                          shape,
                          dtype,
                          partition_strategy="TOKEN",
                          optimizer_spec=None,
                          initializer=None):
  """ Create a HostEmbedding.

      Args:
        name: The name which uniquely identifies the embedding.
        shape: The shape for the tensor which will hold the embedding.
        dtype: The dtype for the tensor which will hold the embedding.
        partition_strategy: When
          `enable_experimental_remote_buffer_embedding` is `True` and using
          replication, the embedding must be distributed across the replicas.
          This option decides on which axis the embedding will be split. Options
          are "TOKEN" or "ENCODING".
        optimizer_spec: A description of how the embedding will be optimized.
          When `None`, the embedding is assumed to not be trainable.
        initializer: The initializer to use when creating the embedding tensor.

      Returns:
        A `HostEmbedding` object that wraps the created embedding tensor.

  """
  if initializer is None:
    initializer = array_ops.zeros(shape, dtype)
  with ops.device('cpu'):
    embedding_tensor = variables.RefVariable(initial_value=initializer,
                                             name=name)
  return HostEmbedding(name,
                       embedding_tensor,
                       partition_strategy=partition_strategy,
                       optimizer_spec=optimizer_spec)
