# Copyright 2020 The TensorFlow Authors. All Rights Reserved.
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
import numpy as np

from tensorflow.compiler.plugin.poplar.driver.trace_pb2 import IpuTraceEvent
from tensorflow.compiler.plugin.poplar.tests import test_utils as tu
from tensorflow.python import keras
from tensorflow.python.eager import context
from tensorflow.python.eager import def_function
from tensorflow.python.eager.backprop import GradientTape
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import tensor_spec
from tensorflow.python.framework import test_util
from tensorflow.python.ipu import ipu_strategy
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import variables
from tensorflow.python.platform import test


def _get_compiled_modules(trace_events):
  compiled_modules = []
  for evt in trace_events:
    if evt.type == IpuTraceEvent.COMPILE_END:
      # Skip empty reports, i.e. when compilation was skipped.
      if evt.compile_end.compilation_report:
        compiled_modules.append(evt.compile_end.module_name)
  return compiled_modules


class IPUStrategyTest(test_util.TensorFlowTestCase):
  @test_util.run_v2_only
  def test_create_variable(self):
    # IPU 0 should be the default.
    ipu0_strategy = ipu_strategy.IPUStrategy()
    with ipu0_strategy.scope():
      v0 = variables.Variable(1.0)
      self.assertEqual("/job:localhost/replica:0/task:0/device:IPU:0",
                       v0.device)

    ipu1_strategy = ipu_strategy.IPUStrategy("/device:IPU:1")
    with ipu1_strategy.scope():
      v1 = variables.Variable(1.0)
      self.assertEqual("/job:localhost/replica:0/task:0/device:IPU:1",
                       v1.device)

  @test_util.run_v2_only
  def test_inference_step_fn_keras_model(self):
    strategy = ipu_strategy.IPUStrategy()

    with strategy.scope():

      model = keras.Sequential([
          keras.layers.Dense(5),
          keras.layers.Dense(10),
          keras.layers.Softmax(),
      ])

      @def_function.function
      def step_fn(x):
        return model(x)

      report = tu.ReportJSON(self, eager_mode=True)
      report.reset()

      inputs = np.ones((1, 2), dtype=np.float32)
      out = strategy.experimental_run_v2(step_fn, args=[inputs])
      self.assertEqual("/job:localhost/replica:0/task:0/device:IPU:0",
                       out.device)
      self.assertAllClose(1.0, np.sum(out.numpy()))

      # There should be a single engine, executed once.
      event_counts, _ = report.get_ipu_events()
      self.assertEqual(1, event_counts[IpuTraceEvent.EXECUTE])

  @test_util.run_v2_only
  def test_building_model_by_passing_input_shape_to_first_layer(self):
    strategy = ipu_strategy.IPUStrategy()

    with strategy.scope():

      report = tu.ReportJSON(self, eager_mode=True)
      report.reset()

      # Passing input_shape to first layer builds the model.
      model = keras.Sequential([
          keras.layers.Dense(5, input_shape=(2,)),
          keras.layers.Dense(10),
          keras.layers.Softmax(),
      ])

      # The model is built, meaning shapes are known and weights allocated,
      # but no engines should have been compiled or executed yet.
      self.assertTrue(model.built)
      self.assertEqual(4, len(model.variables))

      event_counts, trace_events = report.get_ipu_events()
      self.assertEqual([], _get_compiled_modules(trace_events))
      self.assertEqual(0, event_counts[IpuTraceEvent.EXECUTE])

  @test_util.run_v2_only
  def test_building_model_explicitly(self):
    strategy = ipu_strategy.IPUStrategy()

    with strategy.scope():

      report = tu.ReportJSON(self, eager_mode=True)
      report.reset()

      model = keras.Sequential([
          keras.layers.Dense(5),
          keras.layers.Dense(10),
          keras.layers.Softmax(),
      ])

      self.assertFalse(model.built)

      model.build(input_shape=(None, 2))

      # The model is now built, meaning shapes are known and weights are
      # allocated, but no engines should have been compiled or executed yet.
      self.assertTrue(model.built)
      self.assertEqual(4, len(model.variables))

      event_counts, trace_events = report.get_ipu_events()
      self.assertEqual([], _get_compiled_modules(trace_events))
      self.assertEqual(0, event_counts[IpuTraceEvent.EXECUTE])

  @test_util.run_v2_only
  def test_model_with_autograph_loop(self):
    strategy = ipu_strategy.IPUStrategy()

    with strategy.scope():

      model = keras.Sequential([
          keras.layers.Dense(1, activation='relu'),
      ])

      @def_function.function
      def step_fn(x):
        while x[0] < 0.0:
          x = model(x)
        return x

      report = tu.ReportJSON(self, eager_mode=True)
      report.reset()

      inputs = -1.0 * np.ones((1, 1), dtype=np.float32)
      out = strategy.experimental_run_v2(step_fn, args=[inputs])
      self.assertGreaterEqual(out, 0.0)

      # There should be a single engine, executed once. If auto-clustering
      # were enabled, it would usually produce multiple engines for the loop.
      event_counts, _ = report.get_ipu_events()
      self.assertEqual(1, event_counts[IpuTraceEvent.LOAD_ENGINE])
      self.assertEqual(1, event_counts[IpuTraceEvent.EXECUTE])

  @test_util.run_v2_only
  def test_train_step_fn_keras_model(self):
    strategy = ipu_strategy.IPUStrategy()

    with strategy.scope():

      model = keras.Sequential([
          keras.layers.Dense(1),
      ])

      optimizer = keras.optimizer_v2.gradient_descent.SGD(0.01)

      @def_function.function
      def step_fn(features, labels):
        with GradientTape() as tape:
          predictions = model(features, training=True)
          prediction_loss = keras.losses.mean_squared_error(
              labels, predictions)
          loss = math_ops.reduce_mean(prediction_loss)

        grads = tape.gradient(loss, model.trainable_variables)
        optimizer.apply_gradients(zip(grads, model.trainable_variables))
        return loss

      report = tu.ReportJSON(self, eager_mode=True)
      report.reset()

      batch_size = 5
      x_train = np.ones((batch_size, 10), dtype=np.float32)
      y_train = np.ones((batch_size, 1), dtype=np.float32)

      first_loss = strategy.experimental_run_v2(step_fn,
                                                args=[x_train, y_train])
      second_loss = strategy.experimental_run_v2(step_fn,
                                                 args=[x_train, y_train])

      # Check that loss is decreasing.
      self.assertLess(second_loss, first_loss)

      # There should be a single engine, loaded once, executed twice.
      event_counts, _ = report.get_ipu_events()
      self.assertEqual(1, event_counts[IpuTraceEvent.LOAD_ENGINE])
      self.assertEqual(2, event_counts[IpuTraceEvent.EXECUTE])

  @test_util.run_v2_only
  def test_train_step_fn_keras_model_known_input_size(self):
    strategy = ipu_strategy.IPUStrategy()

    with strategy.scope():

      model = keras.Sequential([
          keras.layers.Dense(1, input_shape=[10]),
      ])

      optimizer = keras.optimizer_v2.gradient_descent.SGD(0.01)

      @def_function.function
      def step_fn(features, labels):
        with GradientTape() as tape:
          predictions = model(features, training=True)
          prediction_loss = keras.losses.mean_squared_error(
              labels, predictions)
          loss = math_ops.reduce_mean(prediction_loss)

        grads = tape.gradient(loss, model.trainable_variables)
        optimizer.apply_gradients(zip(grads, model.trainable_variables))
        return loss

      report = tu.ReportJSON(self, eager_mode=True)
      report.reset()

      batch_size = 5
      x_train = np.ones((batch_size, 10), dtype=np.float32)
      y_train = np.ones((batch_size, 1), dtype=np.float32)

      first_loss = strategy.experimental_run_v2(step_fn,
                                                args=[x_train, y_train])
      second_loss = strategy.experimental_run_v2(step_fn,
                                                 args=[x_train, y_train])

      # Check that loss is decreasing.
      self.assertLess(second_loss, first_loss)

      # There should be a single engine, loaded once, executed twice.
      event_counts, _ = report.get_ipu_events()
      self.assertEqual(1, event_counts[IpuTraceEvent.LOAD_ENGINE])
      self.assertEqual(2, event_counts[IpuTraceEvent.EXECUTE])

  @test_util.run_v2_only
  def test_keras_mnist_model_compile_fit(self):
    num_examples = 100
    batch_size = 10
    num_classes = 10
    num_epochs = 3

    def mnist_model():
      model = keras.models.Sequential()
      model.add(keras.layers.Conv2D(32, (3, 3)))
      model.add(keras.layers.Conv2D(64, (3, 3)))
      model.add(keras.layers.Dropout(0.25))
      model.add(keras.layers.Flatten())
      model.add(keras.layers.Dense(num_classes, activation='softmax'))
      return model

    (x_train, y_train), _ = keras.datasets.mnist.load_data()
    x_train = x_train[:num_examples]
    y_train = y_train[:num_examples]

    x_train = x_train.reshape(*x_train.shape, 1)
    x_train = x_train.astype('float32')
    x_train /= 255
    y_train = keras.utils.np_utils.to_categorical(y_train, num_classes)

    report = tu.ReportJSON(self, eager_mode=True)
    report.reset()

    strategy = ipu_strategy.IPUStrategy()

    with strategy.scope():

      model = mnist_model()
      model.compile(loss=keras.losses.categorical_crossentropy,
                    optimizer=keras.optimizer_v2.gradient_descent.SGD(0.05))
      history = model.fit(
          x_train,
          y_train,
          batch_size=batch_size,
          shuffle=False,  # Try to make it deterministic.
          epochs=num_epochs,
          verbose=1)

      # Check that the loss decreased.
      losses = history.history["loss"]
      self.assertEqual(num_epochs, len(losses))
      self.assertLess(losses[1], losses[0])
      self.assertLess(losses[2], losses[1])

      num_batches = num_epochs * num_examples // batch_size

      # There should be be a single engine, loaded once, and executed one
      # time for each batch.
      event_counts, _ = report.get_ipu_events()
      self.assertEqual(1, event_counts[IpuTraceEvent.LOAD_ENGINE])
      self.assertEqual(num_batches, event_counts[IpuTraceEvent.EXECUTE])

  @test_util.run_v2_only
  def test_keras_mnist_model_compile_fit_fixed_input(self):
    num_examples = 100
    batch_size = 10
    num_classes = 10
    num_epochs = 3

    def mnist_model():
      model = keras.models.Sequential()
      model.add(keras.layers.Conv2D(32, (3, 3), input_shape=[28, 28, 1]))
      model.add(keras.layers.Conv2D(64, (3, 3)))
      model.add(keras.layers.Flatten())
      model.add(keras.layers.Dense(num_classes, activation='softmax'))
      return model

    (x_train, y_train), _ = keras.datasets.mnist.load_data()
    x_train = x_train[:num_examples]
    y_train = y_train[:num_examples]

    x_train = x_train.reshape(*x_train.shape, 1)
    x_train = x_train.astype('float32')
    x_train /= 255
    y_train = keras.utils.np_utils.to_categorical(y_train, num_classes)

    report = tu.ReportJSON(self, eager_mode=True)
    report.reset()

    strategy = ipu_strategy.IPUStrategy()

    with strategy.scope():

      model = mnist_model()
      model.compile(loss=keras.losses.categorical_crossentropy,
                    optimizer=keras.optimizer_v2.gradient_descent.SGD(0.05))
      history = model.fit(
          x_train,
          y_train,
          batch_size=batch_size,
          shuffle=False,  # Try to make it deterministic.
          epochs=num_epochs,
          verbose=1)

      # Check that the loss decreased.
      losses = history.history["loss"]
      self.assertEqual(num_epochs, len(losses))
      self.assertLess(losses[1], losses[0])
      self.assertLess(losses[2], losses[1])

      num_batches = num_epochs * num_examples // batch_size

      # There should be be a single engine, loaded once, and executed one
      # time for each batch.
      event_counts, _ = report.get_ipu_events()
      self.assertEqual(1, event_counts[IpuTraceEvent.LOAD_ENGINE])
      self.assertEqual(num_batches, event_counts[IpuTraceEvent.EXECUTE])

  @test_util.run_v2_only
  def test_keras_mnist_model_compile_fit_fixed_input_w_validation(self):
    num_train_examples = 1000
    num_valid_examples = 240
    batch_size = 10
    num_classes = 10
    num_epochs = 3
    validation_steps = 8

    def mnist_model():
      model = keras.models.Sequential([
          keras.layers.Flatten(),
          keras.layers.Dense(num_classes, activation='softmax')
      ])
      return model

    def load_data(n):
      (x, y), _ = keras.datasets.mnist.load_data()
      x = x[:n]
      y = y[:n]

      x = x.reshape(*x.shape, 1)
      x = x.astype('float32')
      x /= 255
      y = keras.utils.np_utils.to_categorical(y, num_classes)
      return (x, y)

    (x_train, y_train) = load_data(num_train_examples)
    (x_valid, y_valid) = load_data(num_valid_examples)

    report = tu.ReportJSON(self, eager_mode=True)
    report.reset()

    strategy = ipu_strategy.IPUStrategy()

    with strategy.scope():

      model = mnist_model()
      model.compile(loss=keras.losses.categorical_crossentropy,
                    optimizer=keras.optimizer_v2.gradient_descent.SGD(0.05))
      history = model.fit(
          x_train,
          y_train,
          batch_size=batch_size,
          shuffle=False,  # Try to make it deterministic.
          epochs=num_epochs,
          validation_data=(x_valid, y_valid),
          validation_steps=validation_steps,
          verbose=1)

      # Check that the loss decreased.
      losses = history.history["loss"]
      self.assertEqual(num_epochs, len(losses))
      self.assertLess(losses[1], losses[0])
      self.assertLess(losses[2], losses[1])

      train_steps = num_train_examples // batch_size
      num_execs = num_epochs * (train_steps + validation_steps)

      # There should be be a single engine, loaded once, and executed one
      # time for each batch.
      event_counts, _ = report.get_ipu_events()

      # Need to flip between main and validation engine 3 times = 6 loads
      self.assertEqual(num_epochs * 2, event_counts[IpuTraceEvent.LOAD_ENGINE])
      self.assertEqual(num_execs, event_counts[IpuTraceEvent.EXECUTE])

  @test_util.run_v2_only
  def test_unsupported_data_types(self):
    @def_function.function
    def identity(x):
      return x

    @def_function.function
    def cast_float64(x):
      return math_ops.cast(x, np.float64)

    strategy = ipu_strategy.IPUStrategy()

    with strategy.scope():

      with self.assertRaisesRegex(TypeError,
                                  "Unsupported data type for input: float64"):
        strategy.experimental_run_v2(identity,
                                     args=[np.array(1.0, dtype=np.float64)])

      with self.assertRaisesRegex(TypeError,
                                  "Unsupported data type for output: float64"):
        strategy.experimental_run_v2(cast_float64,
                                     args=[np.array(1.0, dtype=np.float32)])

  @test_util.run_v2_only
  def test_allowed_function_types_in_eager_mode(self):
    def identity(x):
      self.assertFalse(context.executing_eagerly())
      return x

    inputs = constant_op.constant(1.0, dtype=np.float32)

    strategy = ipu_strategy.IPUStrategy()

    with strategy.scope():
      self.assertTrue(context.executing_eagerly())

      # Passing plain function in eager mode is not allowed.
      with self.assertRaisesRegex(ValueError,
                                  "does not support eager execution"):
        strategy.experimental_run_v2(identity, args=[inputs])

      # But calling it from inside a `tf.function` is fine.
      @def_function.function
      def wrapper():
        return strategy.experimental_run_v2(identity, args=[inputs])

      self.assertEqual(wrapper(), inputs)

      # Converted to `tf.function` also fine.
      tf_function = def_function.function(identity)
      result = strategy.experimental_run_v2(tf_function, args=[inputs])
      self.assertEqual(result, inputs)

      # `ConcreteFunction` is fine as well.
      concrete_function = tf_function.get_concrete_function(
          tensor_spec.TensorSpec(inputs.shape, inputs.dtype))
      result = strategy.experimental_run_v2(concrete_function, args=[inputs])
      self.assertEqual(result, inputs)


if __name__ == "__main__":
  test.main()
