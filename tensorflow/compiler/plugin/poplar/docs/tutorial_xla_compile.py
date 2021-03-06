import numpy as np

from tensorflow.python import ipu
from tensorflow.python.ipu.scopes import ipu_scope
import tensorflow.compat.v1 as tf
tf.disable_v2_behavior()

# Configure argument for targeting the IPU
cfg = ipu.utils.create_ipu_config(profiling=True, use_poplar_text_report=True)
cfg = ipu.utils.set_ipu_model_options(cfg, compile_ipu_code=False)
cfg = ipu.utils.auto_select_ipus(cfg, 1)
ipu.utils.configure_ipu_system(cfg)

with tf.device("cpu"):
  pa = tf.placeholder(np.float32, [2], name="a")
  pb = tf.placeholder(np.float32, [2], name="b")
  pc = tf.placeholder(np.float32, [2], name="c")


def basic_graph(pa, pb, pc):
  # Do basic addition on tensors
  o1 = pa + pb
  o2 = pa + pc
  simple_graph_output = o1 + o2
  return simple_graph_output


with ipu_scope("/device:IPU:0"):
  xla_result = ipu.ipu_compiler.compile(basic_graph, [pa, pb, pc])

with tf.Session() as sess:
  # Base run
  result = sess.run(xla_result,
                    feed_dict={
                        pa: [1., 1.],
                        pb: [0., 1.],
                        pc: [1., 5.]
                    })

  print(result)
