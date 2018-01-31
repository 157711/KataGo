#!/usr/bin/python3
import sys
import os
import argparse
import traceback
import random
import math
import time
import logging
import h5py
import contextlib
import tensorflow as tf
import numpy as np

import data
from board import Board

#Command and args-------------------------------------------------------------------

description = """
Train neural net on Go games!
"""

parser = argparse.ArgumentParser(description=description)
parser.add_argument('-traindir', help='Dir to write to for recording training results', required=True)
parser.add_argument('-gamesh5', help='H5 file of preprocessed game data', required=True)
parser.add_argument('-verbose', help='verbose', required=False, action='store_true')
parser.add_argument('-restart-file', help='restart training from file', required=False)
parser.add_argument('-restart-epoch', help='restart training epoch', required=False)
args = vars(parser.parse_args())

traindir = args["traindir"]
gamesh5 = args["gamesh5"]
verbose = args["verbose"]
restart_file = None
start_epoch = 0
logfilemode = "w"
if "restart_file" in args and args["restart_file"] is not None:
  restart_file = args["restart_file"]
  start_epoch = int(args["restart_epoch"])
  logfilemode = "a"

if not os.path.exists(traindir):
  os.makedirs(traindir)

bareformatter = logging.Formatter("%(message)s")
trainlogger = logging.getLogger("trainlogger")
trainlogger.setLevel(logging.INFO)
fh = logging.FileHandler(traindir+"/train.log", mode=logfilemode)
fh.setFormatter(bareformatter)
trainlogger.addHandler(fh)

detaillogger = logging.getLogger("detaillogger")
detaillogger.setLevel(logging.INFO)
fh = logging.FileHandler(traindir+"/detail.log", mode=logfilemode)
fh.setFormatter(bareformatter)
detaillogger.addHandler(fh)

np.set_printoptions(linewidth=150)

def trainlog(s):
  print(s,flush=True)
  trainlogger.info(s)
  detaillogger.info(s)

def detaillog(s):
  detaillogger.info(s)

# Model ----------------------------------------------------------------
print("Building model", flush=True)
import model

policy_output = model.policy_output
ladder_output = model.ladder_output

#Loss function
targets = tf.placeholder(tf.float32, [None] + model.target_shape)
ladder_targets = tf.placeholder(tf.float32, [None] + model.ladder_target_shape)
target_weights = tf.placeholder(tf.float32, [None] + model.target_weights_shape)
data_loss = tf.reduce_mean(target_weights * tf.nn.softmax_cross_entropy_with_logits(labels=targets, logits=policy_output))
# ladder_loss = tf.reduce_mean(target_weights * tf.reduce_sum(tf.square(ladder_targets-tf.sigmoid(ladder_output)),axis=1))
ladder_loss = tf.reduce_mean(target_weights * tf.reduce_sum(tf.nn.sigmoid_cross_entropy_with_logits(labels=ladder_targets,logits=ladder_output),axis=1))

#Prior/Regularization
l2_reg_coeff = tf.placeholder(tf.float32)
reg_loss = l2_reg_coeff * tf.add_n([tf.nn.l2_loss(variable) for variable in model.reg_variables])

#The loss to optimize
opt_loss = data_loss + reg_loss

#Training operation
batch_learning_rate = tf.placeholder(tf.float32)
lr_adjusted_variables = dict(model.lr_adjusted_variables)
update_ops = tf.get_collection(tf.GraphKeys.UPDATE_OPS) #collect batch norm update operations
with tf.control_dependencies(update_ops):
  #optimizer = tf.train.AdamOptimizer(batch_learning_rate)
  optimizer = tf.train.MomentumOptimizer(batch_learning_rate, momentum=0.9, use_nesterov=True)
  gradients = optimizer.compute_gradients(opt_loss)
  adjusted_gradients = []
  for (grad,x) in gradients:
    adjusted_grad = grad
    if x.name in lr_adjusted_variables and grad is not None:
      adjusted_grad = grad * lr_adjusted_variables[x.name]
    adjusted_gradients.append((adjusted_grad,x))
  train_step = optimizer.apply_gradients(adjusted_gradients)

#Training results
target_idxs = tf.argmax(targets, 1)
top1_prediction = tf.equal(tf.argmax(policy_output, 1), target_idxs)
top4_prediction = tf.nn.in_top_k(policy_output,target_idxs,4)
accuracy1 = tf.reduce_mean(tf.cast(top1_prediction, tf.float32))
accuracy4 = tf.reduce_mean(tf.cast(top4_prediction, tf.float32))

#Debugging stats

def reduce_norm(x, axis=None, keepdims=False):
  return tf.sqrt(tf.reduce_mean(tf.square(x), axis=axis, keep_dims=keepdims))

def reduce_stdev(x, axis=None, keepdims=False):
  m = tf.reduce_mean(x, axis=axis, keep_dims=True)
  devs_squared = tf.square(x - m)
  return tf.sqrt(tf.reduce_mean(devs_squared, axis=axis, keep_dims=keepdims))

activated_prop_by_layer = dict([
  (name,tf.reduce_mean(tf.count_nonzero(layer,axis=[1,2])/layer.shape[1].value/layer.shape[2].value, axis=0)) for (name,layer) in model.outputs_by_layer
])
mean_output_by_layer = dict([
  (name,tf.reduce_mean(layer,axis=[0,1,2])) for (name,layer) in model.outputs_by_layer
])
stdev_output_by_layer = dict([
  (name,reduce_stdev(layer,axis=[0,1,2])**2) for (name,layer) in model.outputs_by_layer
])
norm_weights_by_var = dict([
  (v.name,reduce_norm(v)) for v in tf.trainable_variables()
])
relative_update_by_var = dict([
  (v.name,batch_learning_rate * reduce_norm(grad) / (1e-10 + reduce_norm(v))) for (grad,v) in adjusted_gradients if grad is not None
])


total_parameters = 0
for variable in tf.trainable_variables():
  shape = variable.get_shape()
  variable_parameters = 1
  for dim in shape:
    variable_parameters *= dim.value
  total_parameters += variable_parameters
  trainlog("Model variable %s, %d parameters" % (variable.name,variable_parameters))

trainlog("Built model, %d total parameters" % total_parameters)

for update_op in tf.get_collection(tf.GraphKeys.UPDATE_OPS):
  trainlog("Additional update op on train step: %s" % update_op.name)

# Open H5 file---------------------------------------------------------
print("Opening H5 file: " + gamesh5)

h5_propfaid = h5py.h5p.create(h5py.h5p.FILE_ACCESS)
h5_settings = list(h5_propfaid.get_cache())
assert(h5_settings[2] == 1048576) #Default h5 cache size is 1 MB
h5_settings[2] *= 128 #Make it 128 MB
print("Adjusting H5 cache settings to: " + str(h5_settings))
h5_propfaid.set_cache(*h5_settings)

h5fid = h5py.h5f.open(str.encode(str(gamesh5)), fapl=h5_propfaid)
h5file = h5py.File(h5fid)
h5train = h5file["train"]
h5test = h5file["test"]
h5_chunk_size = h5train.chunks[0]
num_h5_train_rows = h5train.shape[0]
num_h5_test_rows = h5test.shape[0]

# Learning rate -------------------------------------------------------

class LR:
  def __init__(
    self,
    initial_lr,          #Initial learning rate by sample
    decay_exponent,      #Exponent of the polynomial decay in learning rate based on number of plateaus
    decay_offset,        #Offset of the exponent
    drop_every_epochs,   #Drop the learning rate after these epochs
  ):
    self.initial_lr = initial_lr
    self.decay_exponent = decay_exponent
    self.decay_offset = decay_offset

    self.reduction_count = 0
    self.drop_every_epochs = drop_every_epochs

  def lr(self):
    factor = (self.reduction_count + self.decay_offset) / self.decay_offset
    return self.initial_lr / (factor ** self.decay_exponent)

  def report_epoch_done(self,epoch):
    self.reduction_count = epoch // self.drop_every_epochs

# Training ------------------------------------------------------------

print("Training", flush=True)

num_epochs = 300
num_samples_per_epoch = 500000
batch_size = 100
num_batches_per_epoch = num_samples_per_epoch//batch_size

assert(h5_chunk_size % batch_size == 0)
assert(num_samples_per_epoch % batch_size == 0)

lr = LR(
  initial_lr = 0.00032,
  decay_exponent = 4,
  decay_offset = 14,
  drop_every_epochs = 8,
)

# l2_coeff_value = 0
l2_coeff_value = 0.3 / max(1000,num_h5_train_rows)

saver = tf.train.Saver(
  max_to_keep = 10000,
  save_relative_paths = True,
)

#Some tensorflow options
tfconfig = tf.ConfigProto(log_device_placement=False)
#tfconfig.gpu_options.allow_growth = True
#tfconfig.gpu_options.per_process_gpu_memory_fraction = 0.4
with tf.Session(config=tfconfig) as session:
  if restart_file is not None:
    saver.restore(session, restart_file)
  else:
    session.run(tf.global_variables_initializer())

  sys.stdout.flush()
  sys.stderr.flush()

  trainlog("Began session")
  trainlog("Training on " + str(num_h5_train_rows) + " rows, testing on " + str(num_h5_test_rows) + " rows")
  trainlog("Epoch size = " + str(num_samples_per_epoch))
  trainlog("h5_chunk_size = " + str(h5_chunk_size))
  trainlog("Batch size = " + str(batch_size))
  trainlog("L2 coeff value = " + str(l2_coeff_value))

  sys.stdout.flush()
  sys.stderr.flush()

  def run(fetches, rows, training, symmetries, blr=0.0):
    assert(len(model.input_shape) == 2)
    assert(len(model.chain_shape) == 1)
    assert(len(model.target_shape) == 1)
    assert(len(model.ladder_target_shape) == 1)
    assert(len(model.target_weights_shape) == 0)
    input_len = model.input_shape[0] * model.input_shape[1]
    chain_len = model.chain_shape[0] + 1
    target_len = model.target_shape[0]
    ladder_target_len = model.ladder_target_shape[0]

    if not isinstance(rows, np.ndarray):
      rows = np.array(rows)

    row_inputs = rows[:,0:input_len].reshape([-1] + model.input_shape)
    row_chains = rows[:,input_len:input_len+chain_len-1].reshape([-1] + model.chain_shape).astype(np.int32)
    row_num_chain_segments = rows[:,input_len+chain_len-1].astype(np.int32)
    row_targets = rows[:,input_len+chain_len:input_len+chain_len+target_len]
    row_ladder_targets = rows[:,input_len+chain_len+target_len:input_len+chain_len+target_len+ladder_target_len]
    row_target_weights = rows[:,input_len+chain_len+target_len+ladder_target_len]

    #DEBUG-----------------------------
    # for bidx in range(len(rows)):
    #   print("Comparing " + str(bidx),flush=True)
    #   board = Board(model.max_board_size)
    #   for y in range(board.size):
    #     for x in range(board.size):
    #       if row_inputs[bidx,y*board.size+x,1] == 1.0:
    #         board.set_stone(Board.BLACK,board.loc(x,y))
    #       elif row_inputs[bidx,y*board.size+x,2] == 1.0:
    #         board.set_stone(Board.WHITE,board.loc(x,y))

    #   debug_input_data = np.zeros(shape=[1]+model.input_shape, dtype=np.float32)
    #   debug_chain_data = np.zeros(shape=[1]+model.chain_shape, dtype=np.int32)
    #   debug_num_chain_segments = np.zeros(shape=[1])
    #   model.fill_row_features(board,pla=Board.BLACK,opp=Board.WHITE,moves=[],move_idx=0,
    #                           input_data=debug_input_data,chain_data=debug_chain_data,num_chain_segments=debug_num_chain_segments,
    #                           target_data=None,target_data_weights=None,for_training=False,idx=0)

    #   # print(board.to_string(),flush=True)
    #   # print(row_inputs[bidx,:,23].reshape([19,19]),flush=True)
    #   # print(debug_input_data[0,:,23].reshape([19,19]),flush=True)
    #   # print(row_inputs[bidx,:,24].reshape([19,19]),flush=True)
    #   # print(debug_input_data[0,:,24].reshape([19,19]),flush=True)
    #   # print(row_inputs[bidx,:,25].reshape([19,19]),flush=True)
    #   # print(debug_input_data[0,:,25].reshape([19,19]),flush=True)
    #   # print(row_inputs[bidx,:,18].reshape([19,19]),flush=True)
    #   # print(row_chains[0].reshape([19,19]),flush=True)
    #   # print(row_num_chain_segments[0],flush=True)

    #   has_ko = (sum(row_inputs[bidx,:,17]) > 0)
    #   # print(has_ko,flush=True)

    #   for y in range(board.size):
    #     for x in range(board.size):
    #       for fidx in range(17):
    #         assert(debug_input_data[0,y*board.size+x,fidx] == row_inputs[bidx,y*board.size+x,fidx])
    #       if not has_ko:
    #         for fidx in range(23,26):
    #           assert(debug_input_data[0,y*board.size+x,fidx] == row_inputs[bidx,y*board.size+x,fidx])
    #       assert(debug_chain_data[0,y*board.size+x] == row_chains[bidx,y*board.size+x])


    #   assert(debug_num_chain_segments[0] == row_num_chain_segments[bidx])
    #---------------

    return session.run(fetches, feed_dict={
      model.inputs: row_inputs,
      model.chains: row_chains,
      model.num_chain_segments: row_num_chain_segments,
      targets: row_targets,
      ladder_targets: row_ladder_targets,
      target_weights: row_target_weights,
      model.symmetries: symmetries,
      batch_learning_rate: blr,
      l2_reg_coeff: l2_coeff_value,
      model.is_training: training
    })

  def np_array_str(arr,precision):
    return np.array_str(arr, precision=precision, suppress_small = True, max_line_width = 200)

  def run_validation_in_batches(fetches):
    #Run validation accuracy in batches to avoid out of memory error from processing one supergiant batch
    validation_batch_size = 256
    num_validation_batches = num_h5_test_rows//validation_batch_size
    results = [[] for j in range(len(fetches))]
    for i in range(num_validation_batches):
      rows = h5test[i*validation_batch_size : min((i+1)*validation_batch_size, num_h5_test_rows)]
      result = run(fetches, rows, symmetries=[False,False,False], training=False)
      for j in range(len(fetches)):
        results[j].append(result[j])
    return results
  def merge_dicts(dicts,merge_list):
    keys = dicts[0].keys()
    return dict((key,merge_list([d[key] for d in dicts])) for key in keys)

  def val_accuracy_and_loss():
    (acc1s,acc4s,data_losses,ladder_losses) = run_validation_in_batches([accuracy1,accuracy4,data_loss,ladder_loss])
    return (np.mean(acc1s),np.mean(acc4s),np.mean(data_losses),np.mean(ladder_losses))

  def train_stats_str(tacc1,tacc4,tdata_loss,tladder_loss,treg_loss):
    return "tacc1 %5.2f%% tacc4 %5.2f%% tdloss %f tlloss %f trloss %f" % (tacc1*100, tacc4*100, tdata_loss, tladder_loss, treg_loss)

  def validation_stats_str(vacc1,vacc4,vdata_loss,vladder_loss):
    return "vacc1 %5.2f%% vacc4 %5.2f%% vdloss %f vlloss %f" % (vacc1*100, vacc4*100, vdata_loss, vladder_loss)

  def time_str(elapsed):
    return "time %.3f" % elapsed

  def log_detail_stats(relupdates):
    apbls,mobls,sobls = run_validation_in_batches([activated_prop_by_layer, mean_output_by_layer, stdev_output_by_layer])

    apbl = merge_dicts(apbls, (lambda x: np.mean(x,axis=0)))
    mobl = merge_dicts(mobls, (lambda x: np.mean(x,axis=0)))
    sobl = merge_dicts(sobls, (lambda x: np.sqrt(np.mean(np.square(x),axis=0))))

    for key in apbl:
      detaillog("%s: activated_prop %s" % (key, np_array_str(apbl[key], precision=3)))
      detaillog("%s: mean_output %s" % (key, np_array_str(mobl[key], precision=4)))
      detaillog("%s: stdev_output %s" % (key, np_array_str(sobl[key], precision=4)))

    (nw,) = session.run([norm_weights_by_var])

    for key in nw:
      detaillog("%s: norm weight %f" % (key, nw[key]))

    if relupdates is not None:
      for key in relupdates:
        detaillog("%s: relative update %f" % (key,relupdates[key]))

  def make_batch_generator():
    while(True):
      chunk_perm = np.random.permutation(num_h5_train_rows // h5_chunk_size)
      batch_perm = np.random.permutation(h5_chunk_size // batch_size)
      for chunk_perm_idx in range(len(chunk_perm)):
        chunk_start = chunk_perm[chunk_perm_idx] * h5_chunk_size
        chunk_end = chunk_start + h5_chunk_size
        chunk = np.array(h5train[chunk_start:chunk_end])
        for batch_perm_idx in range(len(batch_perm)):
          batch_start = batch_perm[batch_perm_idx] * batch_size
          batch_end = batch_start + batch_size
          yield chunk[batch_start:batch_end]
        np.random.shuffle(batch_perm)

  batch_generator = make_batch_generator()
  def run_batches(num_batches):
    tacc1_sum = 0
    tacc4_sum = 0
    tdata_loss_sum = 0
    tladder_loss_sum = 0
    treg_loss_sum = 0
    relupdates = dict([(key,0.0) for key in relative_update_by_var])

    for i in range(num_batches):
      rows = next(batch_generator)

      # input_len = model.input_shape[0] * model.input_shape[1]
      # target_len = model.target_shape[0]
      # row_inputs = rows[:,0:input_len].reshape([-1] + model.input_shape)
      # row_targets = rows[:,input_len:input_len+target_len]
      # row_target_weights = rows[:,input_len+target_len]
      # for j in range(len(row_inputs)):
      #   print("BOARD")
      #   print((row_inputs[i,:,0] + row_inputs[i,:,1] + row_inputs[i,:,2]*2).reshape([19,19]))
      #   print("MYLIB")
      #   print((row_inputs[i,:,3] + row_inputs[i,:,4]*2 + row_inputs[i,:,5]*3).reshape([19,19]))
      #   print("OPPLIB")
      #   print((row_inputs[i,:,6] + row_inputs[i,:,7]*2 + row_inputs[i,:,8]*3).reshape([19,19]))
      #   print("LAST")
      #   print((row_inputs[i,:,9] + row_inputs[i,:,10]*2 + row_inputs[i,:,11]*3).reshape([19,19]))
      #   print("KO")
      #   print((row_inputs[i,:,12]).reshape([19,19]))
      #   print("TARGET")
      #   print(row_targets[i].reshape([19,19]))
      #   print("WEIGHT")
      #   print(row_target_weights[i])

      # assert(False)

      (bacc1, bacc4, bdata_loss, bladder_loss, breg_loss, brelupdates, _) = run(
        fetches=[accuracy1, accuracy4, data_loss, ladder_loss, reg_loss, relative_update_by_var, train_step],
        rows=rows,
        training=True,
        symmetries=[np.random.random() < 0.5, np.random.random() < 0.5, np.random.random() < 0.5],
        blr=lr.lr() * batch_size
      )

      tacc1_sum += bacc1
      tacc4_sum += bacc4
      tdata_loss_sum += bdata_loss
      tladder_loss_sum += bladder_loss
      treg_loss_sum += breg_loss
      for key in brelupdates:
        relupdates[key] += brelupdates[key]

      if i % (max(1,num_batches // 30)) == 0:
        print(".", end='', flush=True)

    tacc1 = tacc1_sum / num_batches
    tacc4 = tacc4_sum / num_batches
    tdata_loss = tdata_loss_sum / num_batches
    tladder_loss = tladder_loss_sum / num_batches
    treg_loss = treg_loss_sum / num_batches
    for key in relupdates:
      relupdates[key] = relupdates[key] / num_batches
    return (tacc1,tacc4,tdata_loss,tladder_loss,treg_loss,relupdates)

  (vacc1,vacc4,vdata_loss,vladder_loss) = val_accuracy_and_loss()
  vstr = validation_stats_str(vacc1,vacc4,vdata_loss,vladder_loss)

  trainlog("Initial: %s" % (vstr))
  log_detail_stats(relupdates=None)

  start_time = time.perf_counter()

  if start_epoch > 0:
    lr.report_epoch_done(start_epoch-1)

  for e in range(num_epochs):
    epoch = start_epoch + e
    print("Epoch %d" % (epoch), end='', flush=True)
    (tacc1,tacc4,tdata_loss,tladder_loss,treg_loss,relupdates) = run_batches(num_batches_per_epoch)
    (vacc1,vacc4,vdata_loss,vladder_loss) = val_accuracy_and_loss()
    lr.report_epoch_done(epoch)
    print("")

    elapsed = time.perf_counter() - start_time

    tstr = train_stats_str(tacc1,tacc4,tdata_loss,tladder_loss,treg_loss)
    vstr = validation_stats_str(vacc1,vacc4,vdata_loss,vladder_loss)
    timestr = time_str(elapsed)

    trainlogger.info("Epoch %d--------------------------------------------------" % (epoch))
    detaillogger.info("Epoch %d--------------------------------------------------" % (epoch))

    trainlog("%s %s lr %f %s" % (tstr,vstr,lr.lr(),timestr))
    log_detail_stats(relupdates)

    if epoch % 4 == 0 or epoch == num_epochs-1:
      saver.save(session, traindir + "/model" + str(epoch))

  (vacc1,vacc4,vdata_loss,vladder_loss) = val_accuracy_and_loss()
  vstr = validation_stats_str(vacc1,vacc4,vdata_loss,vladder_loss)
  trainlog("Final: %s" % (vstr))

  variables_names =[v.name for v in tf.trainable_variables()]
  values = session.run(variables_names)
  for k,v in zip(variables_names, values):
    print(k, v)

# Finish
h5file.close()
h5fid.close()


