# Copyright 2017 The TensorFlow Authors. All Rights Reserved.
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
"""Tests for the LSTM cell and layer."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np
from tensorflow.python.client import session as session_lib
from tensorflow.python.platform import googletest
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import gradients_impl
from tensorflow.python.ops import init_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import nn
from tensorflow.python.ops import rnn
from tensorflow.python.ops import rnn_cell
from tensorflow.python.ops import variables
from tensorflow.python.ops import variable_scope
from tensorflow.python.ops import control_flow_ops
from tensorflow.python.ops.poplar import popnn_ops
from tensorflow.python.platform import test
from tensorflow.python.training import gradient_descent

dataType = np.float32
batch_size = 1
seq_len = 3
input_size = 5
num_channels = 8


def _get_variable(name, shape, initializer):
  return variable_scope.get_variable(name, shape=shape, initializer=initializer, dtype=dataType)


def _createLSTMInput(value, batch_size, seq_len, input_size):
  return np.full(fill_value=value, shape=[seq_len, batch_size, input_size], dtype=dataType)


def _createLSTMWeights(forget_bias, weights_value, input_size, num_channels):
  return (np.full(fill_value=weights_value, shape=[4, input_size, num_channels], dtype=dataType),
          np.full(fill_value=weights_value, shape=[
                  4, num_channels, num_channels], dtype=dataType),
          np.concatenate((np.full(fill_value=forget_bias, shape=[1, num_channels], dtype=dataType),
                          np.full(fill_value=0, shape=[3, num_channels], dtype=dataType))))


def _createLSTMInitialState(h_value, c_value, batch_size, num_channels):
  return (np.full(fill_value=h_value, shape=[batch_size, num_channels], dtype=dataType),
          np.full(fill_value=c_value, shape=[batch_size, num_channels], dtype=dataType))


class LSTMTest(test.TestCase):

  def _LSTMLayerCPU(self, inputs, weights_value, initial_state, forget_bias, name):
    with ops.device("/device:CPU:0"):
      lstm_cell = rnn_cell.LSTMCell(num_channels,
                                    name='basic_lstm_cell',
                                    forget_bias=forget_bias,
                                    initializer=init_ops.constant_initializer(
                                        weights_value, dtype=dataType),
                                    reuse=variable_scope.AUTO_REUSE)
      state = rnn_cell.LSTMStateTuple(initial_state[1], initial_state[0])
      outputs, states = rnn.dynamic_rnn(
          lstm_cell, inputs, dtype=dataType, initial_state=state, time_major=True)
      return outputs

  def _LSTMLayer(self, inputs, weights, initial_state, training, name):
    with ops.device("/device:IPU:0"):
      outputs, _ = popnn_ops.basic_lstm_layer(inputs=inputs,
                                              num_channels=num_channels,
                                              weights=weights,
                                              initial_state=initial_state,
                                              is_training=training,
                                              name=name)
      return outputs

  def _RunLSTMLayerCPUInference(self, name, input_value, forget_bias,
                                weights_value, h_value, c_value):
    pinputs = array_ops.placeholder(dataType,
                                    [seq_len, batch_size, input_size],
                                    name="inputs")
    pinitial_h_state = array_ops.placeholder(dataType,
                                             [batch_size, num_channels],
                                             name="init_h_state")
    pinitial_c_state = array_ops.placeholder(dataType,
                                             [batch_size, num_channels],
                                             name="init_c_state")
    lstm_output_seq = self._LSTMLayerCPU(inputs=pinputs,
                                         weights_value=weights_value,
                                         initial_state=(
                                             pinitial_h_state, pinitial_c_state),
                                         forget_bias=forget_bias,
                                         name=name)

    with session_lib.Session() as sess:
      inputs = _createLSTMInput(input_value, batch_size, seq_len, input_size)
      initial_state = _createLSTMInitialState(
          h_value, c_value, batch_size, num_channels)
      fd = {
          pinputs: inputs,
          pinitial_h_state: initial_state[0],
          pinitial_c_state: initial_state[1],
      }
      sess.run(variables.global_variables_initializer())
      return sess.run(lstm_output_seq, fd)

  def _RunLSTMLayerInference(self, name, input_value, forget_bias,
                             weights_value, h_value, c_value):
    pinputs = array_ops.placeholder(dataType,
                                    [seq_len, batch_size, input_size],
                                    name="inputs")
    pinput_weights = array_ops.placeholder(dataType,
                                           [4, input_size, num_channels],
                                           name="input_weights")
    poutput_weights = array_ops.placeholder(dataType,
                                            [4, num_channels, num_channels],
                                            name="output_weights")
    pbias = array_ops.placeholder(dataType,
                                  [4, num_channels],
                                  name="bias")
    pinitial_h_state = array_ops.placeholder(dataType,
                                             [batch_size, num_channels],
                                             name="init_h_state")
    pinitial_c_state = array_ops.placeholder(dataType,
                                             [batch_size, num_channels],
                                             name="init_c_state")
    lstm_output_seq = self._LSTMLayer(inputs=pinputs,
                                      weights=(pinput_weights,
                                               poutput_weights, pbias),
                                      initial_state=(
                                          pinitial_h_state, pinitial_c_state),
                                      training=False,
                                      name=name)

    with session_lib.Session() as sess:
      inputs = _createLSTMInput(input_value, batch_size, seq_len, input_size)
      weights = _createLSTMWeights(
          forget_bias, weights_value, input_size, num_channels)
      initial_state = _createLSTMInitialState(
          h_value, c_value, batch_size, num_channels)
      fd = {
          pinputs: inputs,
          pinput_weights: weights[0],
          poutput_weights: weights[1],
          pbias: weights[2],
          pinitial_h_state: initial_state[0],
          pinitial_c_state: initial_state[1],
      }
      return sess.run(lstm_output_seq, fd)

  def _RunInferenceComparison(self, name, input_value, forget_bias,
                              weights_value, h_value, c_value):
    ops.reset_default_graph()
    popnn_out = self._RunLSTMLayerInference(name=name,
                                            input_value=input_value,
                                            weights_value=weights_value,
                                            forget_bias=forget_bias,
                                            h_value=h_value,
                                            c_value=c_value)
    ref_out = self._RunLSTMLayerCPUInference(name=name,
                                             input_value=input_value,
                                             weights_value=weights_value,
                                             forget_bias=forget_bias,
                                             h_value=h_value,
                                             c_value=c_value)
    # Check that the whole outupt sequence matches
    self.assertAllClose(popnn_out, ref_out)

  def testLSTMLayerInference(self):
    np.random.seed(0)
    # Run with all-0 weights
    weight0 = 1.
    for forget_bias in [0., 1.]:
      for h_init in [0., 1.]:
        for c_init in [0., 1.]:
          self._RunInferenceComparison('ones',
                                       input_value=0.,
                                       forget_bias=forget_bias,
                                       weights_value=weight0,
                                       h_value=h_init,
                                       c_value=c_init)

    # Run with all-1 weights
    weight1 = 1.
    for forget_bias in [0., 1.]:
      for h_init in [0., 1.]:
        for c_init in [0., 1.]:
          self._RunInferenceComparison('ones',
                                       input_value=0.,
                                       forget_bias=forget_bias,
                                       weights_value=weight1,
                                       h_value=h_init,
                                       c_value=c_init)

    # Run with random weights
    for weight in np.random.rand(3):
      for forget_bias in [0., 1.]:
        for h_init in [0., 1.]:
          for c_init in [0., 1.]:
            self._RunInferenceComparison('rand',
                                         input_value=0.,
                                         forget_bias=forget_bias,
                                         weights_value=weight,
                                         h_value=h_init,
                                         c_value=c_init)

  def _RunLSTMLayerCPUTraining(self, name, input_value, forget_bias,
                               weights_value, h_value, c_value, training_steps,
                               labels_array):
    pinputs = array_ops.placeholder(dataType,
                                    [seq_len, batch_size, input_size],
                                    name="inputs")
    plabels = array_ops.placeholder(dataType, [seq_len, batch_size, num_channels],
                                    name="labels")

    with variable_scope.variable_scope("lstm_layer", use_resource=True):
      initial_h_state = _get_variable("initial_h_state",
                                      shape=[batch_size, num_channels],
                                      initializer=init_ops.constant_initializer(h_value, dataType))
      initial_c_state = _get_variable("initial_c_state",
                                      shape=[batch_size, num_channels],
                                      initializer=init_ops.constant_initializer(c_value, dataType))

    with ops.device("/device:CPU:0"):
      logits = self._LSTMLayerCPU(inputs=pinputs,
                                  weights_value=weights_value,
                                  initial_state=(
                                      initial_h_state, initial_c_state),
                                  forget_bias=forget_bias,
                                  name=name)
      softmax = nn.softmax_cross_entropy_with_logits(logits=logits, labels=plabels)
      loss = math_ops.reduce_mean(softmax)
      train = gradient_descent.GradientDescentOptimizer(0.01).minimize(loss)

    with session_lib.Session() as sess:
      sess.run(variables.global_variables_initializer())
      losses = []
      inputs = _createLSTMInput(input_value, batch_size, seq_len, input_size)
      fd = {
          pinputs: inputs,
          plabels: labels_array,
      }
      for _ in range(0, training_steps):
        l, _ = sess.run([loss, train], fd)
        losses.append(l)
      return losses

  def _RunLSTMLayerTraining(self, name, input_value, forget_bias,
                            weights_value, h_value, c_value, training_steps,
                            labels_array):
    with variable_scope.variable_scope("lstm_layer", use_resource=True):
      input_weights = _get_variable("input_weights",
                                    shape=[4, input_size, num_channels],
                                    initializer=init_ops.constant_initializer(weights_value, dataType))
      output_weights = _get_variable("output_weights",
                                     shape=[4, num_channels, num_channels],
                                     initializer=init_ops.constant_initializer(weights_value, dataType))
      bias = _get_variable("bias", shape=[4, num_channels],
                           initializer=init_ops.constant_initializer(0.0, dataType))
      initial_h_state = _get_variable("initial_h_state",
                                      shape=[batch_size, num_channels],
                                      initializer=init_ops.constant_initializer(h_value, dataType))
      initial_c_state = _get_variable("initial_c_state",
                                      shape=[batch_size, num_channels],
                                      initializer=init_ops.constant_initializer(c_value, dataType))
    pinputs = array_ops.placeholder(dataType,
                                    [seq_len, batch_size, input_size],
                                    name="inputs")
    plabels = array_ops.placeholder(dataType, [seq_len, batch_size, num_channels],
                                    name="labels")

    with ops.device("/device:IPU:0"):
      logits = self._LSTMLayer(inputs=pinputs,
                               weights=(input_weights,
                                        output_weights, bias),
                               initial_state=(
                                   initial_h_state, initial_c_state),
                               training=True,
                               name=name)
      softmax = nn.softmax_cross_entropy_with_logits(logits=logits, labels=plabels)
      loss = math_ops.reduce_mean(softmax)
      train = gradient_descent.GradientDescentOptimizer(0.01).minimize(loss)

    with session_lib.Session() as sess:
      sess.run(variables.global_variables_initializer())
      losses = []
      inputs = _createLSTMInput(input_value, batch_size, seq_len, input_size)
      fd = {
          pinputs: inputs,
          plabels: labels_array,
      }
      for _ in range(0, training_steps):
        l, _ = sess.run([loss, train], fd)
        losses.append(l)
      return losses

  def _RunTrainingComparison(self, name, input_value, forget_bias,
                             weights_value, h_value, c_value, training_steps):
    labels_array = np.ones(shape=[seq_len, batch_size, num_channels], dtype=np.float32)
    ops.reset_default_graph()
    popnn_losses = self._RunLSTMLayerTraining(name=name,
                                              input_value=input_value,
                                              weights_value=weights_value,
                                              forget_bias=forget_bias,
                                              h_value=h_value,
                                              c_value=c_value,
                                              training_steps=training_steps,
                                              labels_array=labels_array)
    ops.reset_default_graph()
    ref_losses = self._RunLSTMLayerCPUTraining(name=name,
                                               input_value=input_value,
                                               weights_value=weights_value,
                                               forget_bias=forget_bias,
                                               h_value=h_value,
                                               c_value=c_value,
                                               training_steps=training_steps,
                                               labels_array=labels_array)
    self.assertAllClose(popnn_losses, ref_losses)

  def testLSTMLayerTraining(self):
    np.random.seed(42)

    # Run with random weights
    for weight in np.random.rand(3):
      for h_init in [0., 1.]:
        for c_init in [0., 1.]:
          self._RunTrainingComparison('rand',
                                      input_value=0.,
                                      forget_bias=0.,
                                      weights_value=weight,
                                      h_value=h_init,
                                      c_value=c_init,
                                      training_steps=3)


if __name__ == "__main__":
  googletest.main()
