import logging
import math
import traceback
import tensorflow as tf
import numpy as np

from board import Board

#Feature extraction functions-------------------------------------------------------------------

max_board_size = 19
input_shape = [19*19,26]
post_input_shape = [19,19,26]
# chain_shape = [19*19]
# post_chain_shape = [19,19]
target_shape = [19*19]
target_weights_shape = []
pass_pos = max_board_size * max_board_size

def xy_to_tensor_pos(x,y,offset):
  return (y+offset) * max_board_size + (x+offset)
def loc_to_tensor_pos(loc,board,offset):
  return (board.loc_y(loc) + offset) * max_board_size + (board.loc_x(loc) + offset)

def tensor_pos_to_loc(pos,board):
  if pos == pass_pos:
    return None
  bsize = board.size
  offset = (max_board_size - bsize) // 2
  x = pos%max_board_size - offset
  y = pos//max_board_size - offset
  if x < 0 or x >= bsize or y < 0 or y >= bsize:
    return board.loc(-1,-1) #Return an illegal move since this is offboard
  return board.loc(x,y)

def sym_tensor_pos(pos,symmetry):
  if pos == pass_pos:
    return pos
  x = pos%max_board_size
  y = pos//max_board_size
  if symmetry >= 4:
    symmetry -= 4
    tmp = x
    x = y
    y = tmp
  if symmetry >= 2:
    symmetry -= 2
    x = max_board_size-x-1
  if symmetry >= 1:
    symmetry -= 1
    y = max_board_size-y-1
  return y*max_board_size+x


#Calls f on each location that is part of an inescapable atari, or a group that can be put into inescapable atari
def iterLadders(board, f):
  chainHeadsSolved = {}
  copy = board.copy()

  bsize = board.size
  offset = (max_board_size - bsize) // 2

  for y in range(bsize):
    for x in range(bsize):
      pos = xy_to_tensor_pos(x,y,offset)
      loc = board.loc(x,y)
      stone = board.board[loc]

      if stone == Board.BLACK or stone == Board.WHITE:
        libs = board.num_liberties(loc)
        if libs == 1 or libs == 2:
          head = board.group_head[loc]
          if head in chainHeadsSolved:
            laddered = chainHeadsSolved[head]
            if laddered:
              f(loc,pos,[])
          else:
            #Perform search on copy so as not to mess up tracking of solved heads
            if libs == 1:
              workingMoves = []
              laddered = copy.searchIsLadderCaptured(loc,True)
            else:
              workingMoves = copy.searchIsLadderCapturedAttackerFirst2Libs(loc)
              laddered = len(workingMoves) > 0

            chainHeadsSolved[head] = laddered
            if laddered:
              f(loc,pos,workingMoves)


#Returns the new idx, which could be the same as idx if this isn't a good training row
# def fill_row_features(board, pla, opp, moves, move_idx, input_data, chain_data, num_chain_segments, target_data, target_data_weights, for_training, idx):
def fill_row_features(board, pla, opp, moves, move_idx, input_data, target_data, target_data_weights, for_training, idx):
  if target_data is not None and moves[move_idx][1] is None:
    # TODO for now we skip passes
    return idx

  bsize = board.size
  offset = (max_board_size - bsize) // 2

  # nextChainLabel = 1
  # chainLabelsByHeadLoc = {}

  for y in range(bsize):
    for x in range(bsize):
      pos = xy_to_tensor_pos(x,y,offset)
      input_data[idx,pos,0] = 1.0
      loc = board.loc(x,y)
      stone = board.board[loc]
      if stone == pla:
        input_data[idx,pos,1] = 1.0
        libs = board.num_liberties(loc)
        if libs == 1:
          input_data[idx,pos,3] = 1.0
        elif libs == 2:
          input_data[idx,pos,4] = 1.0
        elif libs == 3:
          input_data[idx,pos,5] = 1.0
        elif libs == 4:
          input_data[idx,pos,6] = 1.0

      elif stone == opp:
        input_data[idx,pos,2] = 1.0
        libs = board.num_liberties(loc)
        if libs == 1:
          input_data[idx,pos,7] = 1.0
        elif libs == 2:
          input_data[idx,pos,8] = 1.0
        elif libs == 3:
          input_data[idx,pos,9] = 1.0
        elif libs == 4:
          input_data[idx,pos,10] = 1.0

      if stone == pla or stone == opp:
        pass
        # headloc = board.group_head[loc]
        # if headloc not in chainLabelsByHeadLoc:
        #   chainLabelsByHeadLoc[headloc] = nextChainLabel
        #   nextChainLabel += 1
        # chain_data[idx,pos] = chainLabelsByHeadLoc[headloc]

      else:
        pla_libs_after_play = board.get_liberties_after_play(pla,loc,4);
        opp_libs_after_play = board.get_liberties_after_play(opp,loc,4);
        if pla_libs_after_play == 1:
          input_data[idx,pos,11] = 1.0
        elif pla_libs_after_play == 2:
          input_data[idx,pos,12] = 1.0
        elif pla_libs_after_play == 3:
          input_data[idx,pos,13] = 1.0

        if opp_libs_after_play == 1:
          input_data[idx,pos,14] = 1.0
        elif opp_libs_after_play == 2:
          input_data[idx,pos,15] = 1.0
        elif opp_libs_after_play == 3:
          input_data[idx,pos,16] = 1.0

  # num_chain_segments[idx] = nextChainLabel

  if board.simple_ko_point is not None:
    pos = loc_to_tensor_pos(board.simple_ko_point,board,offset)
    input_data[idx,pos,17] = 1.0

  if for_training:
    prob_to_include_prev1 = 0.90
    prob_to_include_prev2 = 0.95
    prob_to_include_prev3 = 0.95
    prob_to_include_prev4 = 0.98
    prob_to_include_prev5 = 0.98
  else:
    prob_to_include_prev1 = 1.00
    prob_to_include_prev2 = 1.00
    prob_to_include_prev3 = 1.00
    prob_to_include_prev4 = 1.00
    prob_to_include_prev5 = 1.00

  if move_idx >= 1 and moves[move_idx-1][0] == opp and np.random.random() < prob_to_include_prev1:
    prev1_loc = moves[move_idx-1][1]
    if prev1_loc is not None:
      pos = loc_to_tensor_pos(prev1_loc,board,offset)
      input_data[idx,pos,18] = 1.0

    if move_idx >= 2 and moves[move_idx-1][0] == pla and np.random.random() < prob_to_include_prev2:
      prev2_loc = moves[move_idx-2][1]
      if prev2_loc is not None:
        pos = loc_to_tensor_pos(prev2_loc,board,offset)
        input_data[idx,pos,19] = 1.0

      if move_idx >= 3 and moves[move_idx-1][0] == opp and np.random.random() < prob_to_include_prev3:
        prev3_loc = moves[move_idx-3][1]
        if prev3_loc is not None:
          pos = loc_to_tensor_pos(prev3_loc,board,offset)
          input_data[idx,pos,20] = 1.0

        if move_idx >= 4 and moves[move_idx-1][0] == pla and np.random.random() < prob_to_include_prev4:
          prev4_loc = moves[move_idx-4][1]
          if prev4_loc is not None:
            pos = loc_to_tensor_pos(prev4_loc,board,offset)
            input_data[idx,pos,21] = 1.0

          if move_idx >= 5 and moves[move_idx-1][0] == opp and np.random.random() < prob_to_include_prev5:
            prev5_loc = moves[move_idx-5][1]
            if prev5_loc is not None:
              pos = loc_to_tensor_pos(prev5_loc,board,offset)
              input_data[idx,pos,22] = 1.0

  def addLadderFeature(loc,pos,workingMoves):
    assert(board.board[loc] == Board.BLACK or board.board[loc] == Board.WHITE);
    libs = board.num_liberties(loc)
    if libs == 1:
      input_data[idx,pos,23] = 1.0
    else:
      input_data[idx,pos,24] = 1.0
      for workingMove in workingMoves:
        workingPos = loc_to_tensor_pos(workingMove,board,offset)
        input_data[idx,workingPos,25] = 1.0

  iterLadders(board, addLadderFeature)


  if target_data is not None:
    next_loc = moves[move_idx][1]
    if next_loc is None:
      # TODO for now we skip passes
      return idx
      # target_data[idx,max_board_size*max_board_size] = 1.0
    else:
      pos = loc_to_tensor_pos(next_loc,board,offset)
      target_data[idx,pos] = 1.0
      target_data_weights[idx] = 1.0

  return idx+1


# Build model -------------------------------------------------------------

reg_variables = []
lr_adjusted_variables = {}
is_training = tf.placeholder(tf.bool)

def ensure_variable_exists(name):
  for v in tf.trainable_variables():
    if v.name == name:
      return name
  raise Exception("Could not find variable " + name)

def add_lr_factor(name,factor):
  ensure_variable_exists(name)
  if name in lr_adjusted_variables:
    lr_adjusted_variables[name] = factor * lr_adjusted_variables[name]
  else:
    lr_adjusted_variables[name] = factor

def batchnorm(name,tensor):
  return tf.layers.batch_normalization(
    tensor,
    axis=-1, #Because channels are our last axis, -1 refers to that via wacky python indexing
    momentum=0.99,
    epsilon=0.001,
    center=True,
    scale=True,
    training=is_training,
    name=name,
  )

def init_stdev(num_inputs,num_outputs):
  #xavier
  #return math.sqrt(2.0 / (num_inputs + num_outputs))
  #herangzhen
  return math.sqrt(2.0 / (num_inputs))

def init_weights(shape, num_inputs, num_outputs):
  stdev = init_stdev(num_inputs,num_outputs) / 1.0
  return tf.truncated_normal(shape=shape, stddev=stdev)

def weight_variable_init_constant(name, shape, constant):
  init = tf.zeros(shape)
  if constant != 0.0:
    init = init + constant
  variable = tf.Variable(init,name=name)
  reg_variables.append(variable)
  return variable

def weight_variable(name, shape, num_inputs, num_outputs, scale_initial_weights=1.0, extra_initial_weight=None):
  initial = init_weights(shape, num_inputs, num_outputs)
  if extra_initial_weight is not None:
    initial = initial + extra_initial_weight
  initial = initial * scale_initial_weights

  variable = tf.Variable(initial,name=name)
  reg_variables.append(variable)
  return variable

def conv2d(x, w):
  return tf.nn.conv2d(x, w, strides=[1,1,1,1], padding='SAME')

def dilated_conv2d(x, w, dilation):
  return tf.nn.atrous_conv2d(x, w, rate = dilation, padding='SAME')

def apply_symmetry(tensor,symmetries,inverse):
  ud = symmetries[0]
  lr = symmetries[1]
  transp = symmetries[2]

  rev_axes = tf.concat([
    tf.cond(ud, lambda: tf.constant([1]), lambda: tf.constant([],dtype='int32')),
    tf.cond(lr, lambda: tf.constant([2]), lambda: tf.constant([],dtype='int32')),
  ], axis=0)

  if not inverse:
    tensor = tf.reverse(tensor, rev_axes)

  assert(len(tensor.shape) == 4 or len(tensor.shape) == 3)
  if len(tensor.shape) == 3:
    tensor = tf.cond(
      transp,
      lambda: tf.transpose(tensor, [0,2,1]),
      lambda: tensor)
  else:
    tensor = tf.cond(
      transp,
      lambda: tf.transpose(tensor, [0,2,1,3]),
      lambda: tensor)

  if inverse:
    tensor = tf.reverse(tensor, rev_axes)

  return tensor


def chain_pool(tensor,chains,num_chain_segments,empty,nonempty,mode):
  bsize = max_board_size
  assert(len(tensor.shape) == 4)
  assert(len(chains.shape) == 3)
  assert(len(num_chain_segments.shape) == 1)
  assert(tensor.shape[1].value == bsize)
  assert(tensor.shape[2].value == bsize)
  assert(chains.shape[1].value == bsize)
  assert(chains.shape[2].value == bsize)
  assert(mode == "sum" or mode == "max")
  num_channels = tensor.shape[3].value

  #Since tf.unsorted_segment* doesn't operate by batches or channels, we need to manually construct
  #a different shift to add to each batch and each channel so that they pool into disjoint buckets.
  #Each one needs max_chain_idxs different buckets.
  num_segments_by_batch_and_channel = tf.fill([1,num_channels],1) * tf.expand_dims(num_chain_segments,axis=1)
  shift = tf.cumsum(tf.reshape(num_segments_by_batch_and_channel,[-1]),exclusive=True)
  num_segments = tf.reduce_sum(num_chain_segments) * num_channels
  shift = tf.reshape(shift,[-1,1,1,num_channels])

  segments = tf.expand_dims(chains,3) + shift
  if mode == "sum":
    pools = tf.unsorted_segment_sum(tensor,segments,num_segments=num_segments)
  elif mode == "max":
    pools = tf.unsorted_segment_max(tensor,segments,num_segments=num_segments)
  else:
    assert False

  gathered = tf.gather(pools,indices=segments)
  return gathered * tf.expand_dims(nonempty,axis=3) # + tensor * empty


manhattan_radius_3_kernel = tf.reshape(tf.constant([
  [0,0,0,1,0,0,0],[0,0,1,1,1,0,0],[0,1,1,1,1,1,0],[1,1,1,1,1,1,1],[0,1,1,1,1,1,0],[0,0,1,1,1,0,0],[0,0,0,1,0,0,0]
], dtype=tf.float32), [7,7,1,1])

#Define useful components --------------------------------------------------------------------------

#Accumulates outputs for printing stats about their activations
outputs_by_layer = []

def parametric_relu(name, layer):
  assert(len(layer.shape) == 4)
  num_channels = layer.shape[3].value
  alphas = weight_variable_init_constant(name+"/prelu",[1,1,1,num_channels],constant=0.0)
  return tf.nn.relu(layer) - alphas * tf.nn.relu(-layer)

def merge_residual(name,trunk,residual):
  trunk = trunk + residual
  outputs_by_layer.append((name,trunk))
  return trunk

def conv_weight_variable(name, diam1, diam2, in_channels, out_channels, scale_initial_weights=1.0, emphasize_center_weight=None, emphasize_center_lr=None):
  radius1 = diam1 // 2
  radius2 = diam2 // 2

  if emphasize_center_weight is None:
    weights = weight_variable(name,[diam1,diam2,in_channels,out_channels],in_channels*diam1*diam2,out_channels,scale_initial_weights)
  else:
    extra_initial_weight = init_weights([1,1,in_channels,out_channels], in_channels, out_channels) * emphasize_center_weight
    extra_initial_weight = tf.pad(extra_initial_weight, [(radius1,radius1),(radius2,radius2),(0,0),(0,0)])
    weights = weight_variable(name,[diam1,diam2,in_channels,out_channels],in_channels*diam1*diam2,out_channels,scale_initial_weights,extra_initial_weight)

  if emphasize_center_lr is not None:
    factor = tf.constant([emphasize_center_lr],dtype=tf.float32)
    factor = tf.reshape(factor,[1,1,1,1])
    factor = tf.pad(factor, [(radius1,radius1),(radius2,radius2),(0,0),(0,0)], constant_values=1.0)
    add_lr_factor(weights.name, factor)

  return weights

#Convolutional layer with batch norm and nonlinear activation
def conv_block(name, in_layer, diam, in_channels, out_channels, scale_initial_weights=1.0, emphasize_center_weight=None, emphasize_center_lr=None):
  weights = conv_weight_variable(name+"/w", diam, diam, in_channels, out_channels, scale_initial_weights, emphasize_center_weight, emphasize_center_lr)
  out_layer = parametric_relu(name+"/prelu",batchnorm(name+"/norm",conv2d(in_layer, weights)))
  outputs_by_layer.append((name,out_layer))
  return out_layer

#Convolution only, no batch norm or nonlinearity
def conv_only_block(name, in_layer, diam, in_channels, out_channels, scale_initial_weights=1.0, emphasize_center_weight=None, emphasize_center_lr=None):
  weights = conv_weight_variable(name+"/w", diam, diam, in_channels, out_channels, scale_initial_weights, emphasize_center_weight, emphasize_center_lr)
  out_layer = conv2d(in_layer, weights)
  outputs_by_layer.append((name,out_layer))
  return out_layer

#Convolution emphasizing the center
def conv_only_extra_center_block(name, in_layer, diam, in_channels, out_channels, scale_initial_weights=1.0):
  radius = diam // 2
  center_weights = weight_variable(name+"/wcenter",[1,1,in_channels,out_channels],in_channels,out_channels,scale_initial_weights=0.3*scale_initial_weights)
  weights = weight_variable(name+"/w",[diam,diam,in_channels,out_channels],in_channels*diam*diam,out_channels,scale_initial_weights)
  weights = weights + tf.pad(center_weights,[(radius,radius),(radius,radius),(0,0),(0,0)])
  out_layer = conv2d(in_layer, weights)
  outputs_by_layer.append((name,out_layer))
  return out_layer

#Convolutional residual block with internal batch norm and nonlinear activation
def res_conv_block(name, in_layer, diam, main_channels, mid_channels, scale_initial_weights=1.0, emphasize_center_weight=None, emphasize_center_lr=None):
  trans1_layer = parametric_relu(name+"/prelu1",(batchnorm(name+"/norm1",in_layer)))
  outputs_by_layer.append((name+"/trans1",trans1_layer))

  weights1 = conv_weight_variable(name+"/w1", diam, diam, main_channels, mid_channels, scale_initial_weights, emphasize_center_weight, emphasize_center_lr)
  conv1_layer = conv2d(trans1_layer, weights1)
  outputs_by_layer.append((name+"/conv1",conv1_layer))

  trans2_layer = parametric_relu(name+"/prelu2",(batchnorm(name+"/norm2",conv1_layer)))
  outputs_by_layer.append((name+"/trans2",trans2_layer))

  weights2 = conv_weight_variable(name+"/w2", diam, diam, mid_channels, main_channels, scale_initial_weights, emphasize_center_weight, emphasize_center_lr)
  conv2_layer = conv2d(trans2_layer, weights2)
  outputs_by_layer.append((name+"/conv2",conv2_layer))

  return conv2_layer

#Convolutional residual block with internal batch norm and nonlinear activation
def dilated_res_conv_block(name, in_layer, diam, main_channels, mid_channels, dilated_mid_channels, dilation, scale_initial_weights=1.0, emphasize_center_weight=None, emphasize_center_lr=None):
  trans1_layer = parametric_relu(name+"/prelu1",(batchnorm(name+"/norm1",in_layer)))
  outputs_by_layer.append((name+"/trans1",trans1_layer))

  weights1a = conv_weight_variable(name+"/w1a", diam, diam, main_channels, mid_channels, scale_initial_weights, emphasize_center_weight, emphasize_center_lr)
  weights1b = conv_weight_variable(name+"/w1b", diam, diam, main_channels, dilated_mid_channels, scale_initial_weights, emphasize_center_weight, emphasize_center_lr)
  conv1a_layer = conv2d(trans1_layer, weights1a)
  conv1b_layer = dilated_conv2d(trans1_layer, weights1b, dilation=dilation)
  outputs_by_layer.append((name+"/conv1a",conv1a_layer))
  outputs_by_layer.append((name+"/conv1b",conv1b_layer))

  conv1_layer = tf.concat([conv1a_layer,conv1b_layer],axis=3)

  trans2_layer = parametric_relu(name+"/prelu2",(batchnorm(name+"/norm2",conv1_layer)))
  outputs_by_layer.append((name+"/trans2",trans2_layer))

  weights2 = conv_weight_variable(name+"/w2", diam, diam, mid_channels+dilated_mid_channels, main_channels, scale_initial_weights, emphasize_center_weight, emphasize_center_lr)
  conv2_layer = conv2d(trans2_layer, weights2)
  outputs_by_layer.append((name+"/conv2",conv2_layer))

  return conv2_layer

#Convolutional residual block that does sequential horizontal and vertical convolutions, with internal batch norm and nonlinear activation
def hv_res_conv_block(name, in_layer, diam, main_channels, mid_channels):
  trans1_layer = parametric_relu(name+"/prelu1",(batchnorm(name+"/norm1",in_layer)))
  outputs_by_layer.append((name+"/trans1",trans1_layer))

  weights1 = weight_variable(name+"/w1",[diam,1,main_channels,mid_channels],main_channels*diam,mid_channels)
  weights2 = weight_variable(name+"/w2",[1,diam,mid_channels,main_channels],main_channels*diam,mid_channels)

  conv1_layer = conv2d(trans1_layer, weights1)
  outputs_by_layer.append((name+"/conv1",conv1_layer))

  trans2_layer = parametric_relu(name+"/prelu2",(batchnorm(name+"/norm2",conv1_layer)))
  outputs_by_layer.append((name+"/trans2",trans2_layer))

  conv2_layer = conv2d(trans2_layer, weights2)
  outputs_by_layer.append((name+"/conv2",conv2_layer))

  return conv2_layer * 0.5

#Same, but vertical then horizontal
def vh_res_conv_block(name, in_layer, diam, main_channels, mid_channels):
  trans1_layer = parametric_relu(name+"/prelu1",(batchnorm(name+"/norm1",in_layer)))
  outputs_by_layer.append((name+"/trans1",trans1_layer))

  weights1 = weight_variable(name+"/w1",[1,diam,main_channels,mid_channels],main_channels*diam,mid_channels)
  weights2 = weight_variable(name+"/w2",[diam,1,mid_channels,main_channels],main_channels*diam,mid_channels)

  conv1_layer = conv2d(trans1_layer, weights1)
  outputs_by_layer.append((name+"/conv1",conv1_layer))

  trans2_layer = parametric_relu(name+"/prelu2",(batchnorm(name+"/norm2",conv1_layer)))
  outputs_by_layer.append((name+"/trans2",trans2_layer))

  conv2_layer = conv2d(trans2_layer, weights2)
  outputs_by_layer.append((name+"/conv2",conv2_layer))

  return conv2_layer * 0.5


def chainpool_block(name, in_layer, chains, num_chain_segments, empty, nonempty, diam, main_channels, mid_channels):
  trans1_layer = parametric_relu(name+"/prelu1",(batchnorm(name+"/norm1",in_layer)))
  outputs_by_layer.append((name+"/trans1",trans1_layer))

  weights1max = conv_weight_variable(name+"/w1max", diam, diam, main_channels, mid_channels)
  # weights1sum = conv_weight_variable(name+"/w1sum", diam, diam, main_channels, mid_channels)
  conv1max_layer = conv2d(trans1_layer, weights1max)
  # conv1sum_layer = conv2d(trans1_layer, weights1sum)
  outputs_by_layer.append((name+"/conv1max",conv1max_layer))
  # outputs_by_layer.append((name+"/conv1sum",conv1sum_layer))

  trans2max_layer = parametric_relu(name+"/prelu2max",(batchnorm(name+"/norm2max",conv1max_layer)))
  # trans2sum_layer = parametric_relu(name+"/prelu2sum",(batchnorm(name+"/norm2sum",conv1sum_layer)))
  outputs_by_layer.append((name+"/trans2max",trans2max_layer))
  # outputs_by_layer.append((name+"/trans2sum",trans2sum_layer))

  maxpooled_layer = chain_pool(trans2max_layer,chains,num_chain_segments,empty,nonempty,mode="max")
  # sumpooled_layer = chain_pool(trans2sum_layer,chains,empty,nonempty,mode="sum")
  outputs_by_layer.append((name+"/maxpooled",maxpooled_layer))
  # outputs_by_layer.append((name+"/sumpooled",sumpooled_layer))

  pooled_layer = maxpooled_layer
  #pooled_layer = tf.concat([maxpooled_layer,sumpooled_layer],axis=3)

  weights2 = conv_weight_variable(name+"/w2", diam, diam, mid_channels, main_channels)
  conv2_layer = conv2d(pooled_layer, weights2)
  outputs_by_layer.append((name+"/conv2",conv2_layer))

  return conv2_layer


#Special block for detecting ladders, with mid_channels channels per each of 4 diagonal scans.
def ladder_block(name, in_layer, near_nonempty, main_channels, mid_channels):
  # Converts [[123][456][789]] to [[12300][04560][00789]]
  def skew_right(tensor):
    n = max_board_size
    assert(tensor.shape[1].value == n)
    assert(tensor.shape[2].value == n)
    c = tensor.shape[3].value
    tensor = tf.pad(tensor,[[0,0],[0,0],[0,n],[0,0]]) #Pad 19x19 -> 19x38
    tensor = tf.reshape(tensor,[-1,2*n*n,c]) #Linearize
    tensor = tensor[:,:((2*n-1)*n),:] #Chop off the 19 zeroes on the end
    tensor = tf.reshape(tensor,[-1,n,2*n-1,c]) #Now we are skewed 19x37 as desired
    return tensor
  # Converts [[12345][6789a][bcdef]] to [[123][789][def]]
  def unskew_right(tensor):
    n = max_board_size
    assert(tensor.shape[1].value == n)
    assert(tensor.shape[2].value == 2*n-1)
    c = tensor.shape[3].value
    tensor = tf.reshape(tensor,[-1,n*(2*n-1),c]) #Linearize
    tensor = tf.pad(tensor,[[0,0],[0,n],[0,0]]) #Pad 19*37 -> 19*38
    tensor = tf.reshape(tensor,[-1,n,2*n,c]) #Convert back to 19x38
    tensor = tensor[:,:,:n,:] #Chop off the extra, now we are 19x19
    return tensor

  # Converts [[123][456][789]] to [[00123][04560][78900]]
  def skew_left(tensor):
    n = max_board_size
    assert(tensor.shape[1].value == n)
    assert(tensor.shape[2].value == n)
    c = tensor.shape[3].value
    tensor = tf.pad(tensor,[[0,0],[1,1],[n-2,0],[0,0]]) #Pad 19x19 -> 21x36
    tensor = tf.reshape(tensor,[-1,(n+2)*(2*n-2),c]) #Linearize
    tensor = tensor[:,(2*n-3):(-n+1),:] #Chop off the 35 extra zeroes on the start and the 18 at the end.
    tensor = tf.reshape(tensor,[-1,n,2*n-1,c]) #Now we are skewed 19x37 as desired
    return tensor

  # Converts [[12345][6789a][bcdef]] to [[345][789][bcd]]
  def unskew_left(tensor):
    n = max_board_size
    assert(tensor.shape[1].value == n)
    assert(tensor.shape[2].value == 2*n-1)
    c = tensor.shape[3].value
    tensor = tf.reshape(tensor,[-1,n*(2*n-1),c]) #Linearize
    tensor = tf.pad(tensor,[[0,0],[2*n-3,n-1],[0,0]]) #Pad 19*37 -> 21*36
    tensor = tf.reshape(tensor,[-1,n+2,2*n-2,c]) #Convert back to 21x36
    tensor = tensor[:,1:(n+1),(n-2):,:] #Chop off the extra, now we are 19x19
    return tensor

  #First, as usual, batchnorm and relu the trunk to get the values to a reasonable scale
  trans1_layer = parametric_relu(name+"/prelu1",(batchnorm(name+"/norm1",in_layer)))
  outputs_by_layer.append((name+"/trans1",trans1_layer))

  c = mid_channels

  #The next part basically does a scan across the board each of the 4 diagonal ways, computing a moving average.
  #We use a convolution to let the neural net choose the values and weights:
  #a: value on this spot to be moving-averaged
  #b: if the weight on the moving average so far is 1, the value on this spot gets a factor of exp(b)-1 weight.
  diampre = 3
  weightsprea = conv_weight_variable(name+"/wprea", diampre, diampre, main_channels, c*4)
  weightspreb = conv_weight_variable(name+"/wpreb", diampre, diampre, main_channels, c*4)

  convprea_layer = conv2d(trans1_layer, weightsprea)
  convpreb_layer = conv2d(trans1_layer, weightspreb)
  outputs_by_layer.append((name+"/convprea",convprea_layer))
  outputs_by_layer.append((name+"/convpreb",convpreb_layer))

  assert(len(near_nonempty.shape) == 4)
  assert(near_nonempty.shape[1].value == max_board_size)
  assert(near_nonempty.shape[2].value == max_board_size)
  assert(near_nonempty.shape[3].value == 1)

  transprea_layer = parametric_relu(name+"/preluprea",(batchnorm(name+"/normprea",convprea_layer)))
  transpreb_layer = tf.nn.sigmoid(batchnorm(name+"/normpreb",convpreb_layer)) * near_nonempty * 1.5 + 0.0001
  outputs_by_layer.append((name+"/transprea",transprea_layer))
  outputs_by_layer.append((name+"/transpreb",transpreb_layer))

  #Now, skew each segment of the channels left and right, so that axis=1 now runs diagonally along the original board
  skewed_r_a = skew_right(transprea_layer[:,:,:,:(2*c)])
  skewed_r_b = skew_right(transpreb_layer[:,:,:,:(2*c)])
  skewed_l_a = skew_left(transprea_layer[:,:,:,(2*c):])
  skewed_l_b = skew_left(transpreb_layer[:,:,:,(2*c):])

  #And extract out all the necessary bits
  r_fwd_a = skewed_r_a[:,:,:,:c]
  r_rev_a = skewed_r_a[:,:,:,c:]
  r_fwd_b = skewed_r_b[:,:,:,:c]
  r_rev_b = skewed_r_b[:,:,:,c:]

  l_fwd_a = skewed_l_a[:,:,:,:c]
  l_rev_a = skewed_l_a[:,:,:,c:]
  l_fwd_b = skewed_l_b[:,:,:,:c]
  l_rev_b = skewed_l_b[:,:,:,c:]

  #Compute the proper weights based on b
  r_fwd_bsum = tf.cumsum(r_fwd_b, axis=1, exclusive=True)
  r_rev_bsum = tf.cumsum(r_rev_b, axis=1, exclusive=True, reverse=True)
  l_fwd_bsum = tf.cumsum(l_fwd_b, axis=1, exclusive=True)
  l_rev_bsum = tf.cumsum(l_rev_b, axis=1, exclusive=True, reverse=True)
  r_fwd_weight = tf.exp(r_fwd_b+r_fwd_bsum) - tf.exp(r_fwd_bsum)
  r_rev_weight = tf.exp(r_rev_b+r_rev_bsum) - tf.exp(r_rev_bsum)
  l_fwd_weight = tf.exp(l_fwd_b+l_fwd_bsum) - tf.exp(l_fwd_bsum)
  l_rev_weight = tf.exp(l_rev_b+l_rev_bsum) - tf.exp(l_rev_bsum)

  #Compute the moving averages
  result_r_fwd = tf.cumsum(r_fwd_a * r_fwd_weight, axis=1              ) / tf.cumsum(r_fwd_weight, axis=1)
  result_r_rev = tf.cumsum(r_rev_a * r_rev_weight, axis=1, reverse=True) / tf.cumsum(r_rev_weight, axis=1, reverse=True)
  result_l_fwd = tf.cumsum(l_fwd_a * l_fwd_weight, axis=1              ) / tf.cumsum(l_fwd_weight, axis=1)
  result_l_rev = tf.cumsum(l_rev_a * l_rev_weight, axis=1, reverse=True) / tf.cumsum(l_rev_weight, axis=1, reverse=True)

  #Unskew concatenate everything back together
  results = [unskew_right(result_r_fwd), unskew_right(result_r_rev), unskew_left(result_l_fwd), unskew_left(result_l_rev)]
  results = tf.concat(results,axis=3)

  #Apply a convolution to merge the result back into the trunk
  diampost = 1
  weightspost = conv_weight_variable(name+"/wpost", diampost, diampost, c*4, main_channels)

  convpost_layer = conv2d(results, weightspost)
  outputs_by_layer.append((name+"/convpost",convpost_layer))

  return convpost_layer


#Begin Neural net------------------------------------------------------------------------------------

#Indexing:
#batch, bsize, bsize, channel

#Input layer---------------------------------------------------------------------------------
inputs = tf.placeholder(tf.float32, [None] + input_shape)
# chains = tf.placeholder(tf.int32, [None] + chain_shape)
# num_chain_segments = tf.placeholder(tf.int32, [None])
symmetries = tf.placeholder(tf.bool, [3])

features_active = tf.constant([
  1.0, #0
  1.0, #1
  1.0, #2
  1.0, #3
  1.0, #4
  1.0, #5
  1.0, #6
  1.0, #7
  1.0, #8
  1.0, #9
  1.0, #10
  1.0, #11
  1.0, #12
  1.0, #13
  1.0, #14
  1.0, #15
  1.0, #16
  1.0, #17
  1.0, #18
  1.0, #19
  1.0, #20
  1.0, #21
  1.0, #22
  1.0, #23
  1.0, #24
  1.0, #25
])
assert(features_active.dtype == tf.float32)

cur_layer = tf.reshape(inputs, [-1] + post_input_shape)
# cur_chains = tf.reshape(chains, [-1] + post_chain_shape)

input_num_channels = post_input_shape[2]
#Input symmetries - we apply symmetries during training by transforming the input and reverse-transforming the output
cur_layer = apply_symmetry(cur_layer,symmetries,inverse=False)
# cur_chains = apply_symmetry(cur_chains,symmetries,inverse=False)
#Disable various features
cur_layer = cur_layer * tf.reshape(features_active,[1,1,1,-1])

nonempty = cur_layer[:,:,:,1] + cur_layer[:,:,:,2]
empty = 1.0 - nonempty
near_nonempty = tf.minimum(1.0,conv2d(tf.expand_dims(nonempty,axis=3),manhattan_radius_3_kernel))

#Convolutional RELU layer 1-------------------------------------------------------------------------------------
trunk = conv_only_extra_center_block("conv1",cur_layer,diam=5,in_channels=input_num_channels,out_channels=192)

#Residual Convolutional Block 1---------------------------------------------------------------------------------
residual = res_conv_block("rconv1",trunk,diam=3,main_channels=192,mid_channels=192, emphasize_center_weight = 0.3, emphasize_center_lr=1.5)
trunk = merge_residual("rconv1",trunk,residual)

#Residual Convolutional Block 2---------------------------------------------------------------------------------
residual = dilated_res_conv_block("rconv2",trunk,diam=3,main_channels=192,mid_channels=128, dilated_mid_channels=64, dilation=2, emphasize_center_weight = 0.3, emphasize_center_lr=1.5)
trunk = merge_residual("rconv2",trunk,residual)

#Residual Convolutional Block 3---------------------------------------------------------------------------------
residual = dilated_res_conv_block("rconv3",trunk,diam=3,main_channels=192,mid_channels=128, dilated_mid_channels=64, dilation=2, emphasize_center_weight = 0.3, emphasize_center_lr=1.5)
trunk = merge_residual("rconv3",trunk,residual)

#Residual Convolutional Block 4---------------------------------------------------------------------------------
residual = dilated_res_conv_block("rconv4",trunk,diam=3,main_channels=192,mid_channels=128, dilated_mid_channels=64, dilation=2, emphasize_center_weight = 0.3, emphasize_center_lr=1.5)
trunk = merge_residual("rconv4",trunk,residual)

#Residual Convolutional Block 5---------------------------------------------------------------------------------
residual = dilated_res_conv_block("rconv5",trunk,diam=3,main_channels=192,mid_channels=128, dilated_mid_channels=64, dilation=2, emphasize_center_weight = 0.3, emphasize_center_lr=1.5)
trunk = merge_residual("rconv5",trunk,residual)

#Residual Convolutional Block 6---------------------------------------------------------------------------------
residual = dilated_res_conv_block("rconv6",trunk,diam=3,main_channels=192,mid_channels=128, dilated_mid_channels=64, dilation=2, emphasize_center_weight = 0.3, emphasize_center_lr=1.5)
trunk = merge_residual("rconv6",trunk,residual)

#Residual Convolutional Block 7---------------------------------------------------------------------------------
residual = dilated_res_conv_block("rconv7",trunk,diam=3,main_channels=192,mid_channels=128, dilated_mid_channels=64, dilation=2, emphasize_center_weight = 0.3, emphasize_center_lr=1.5)
trunk = merge_residual("rconv7",trunk,residual)

#Residual Convolutional Block 8---------------------------------------------------------------------------------
residual = dilated_res_conv_block("rconv8",trunk,diam=3,main_channels=192,mid_channels=128, dilated_mid_channels=64, dilation=2, emphasize_center_weight = 0.3, emphasize_center_lr=1.5)
trunk = merge_residual("rconv8",trunk,residual)

#Residual Convolutional Block 9---------------------------------------------------------------------------------
residual = dilated_res_conv_block("rconv9",trunk,diam=3,main_channels=192,mid_channels=128, dilated_mid_channels=64, dilation=2, emphasize_center_weight = 0.3, emphasize_center_lr=1.5)
trunk = merge_residual("rconv9",trunk,residual)

#Residual Convolutional Block 10---------------------------------------------------------------------------------
residual = dilated_res_conv_block("rconv10",trunk,diam=3,main_channels=192,mid_channels=128, dilated_mid_channels=64, dilation=2, emphasize_center_weight = 0.3, emphasize_center_lr=1.5)
trunk = merge_residual("rconv10",trunk,residual)

#Postprocessing residual trunk----------------------------------------------------------------------------------

#Normalize and relu just before the policy head
trunk = parametric_relu("trunk/prelu",(batchnorm("trunk/norm",trunk)))
outputs_by_layer.append(("trunk",trunk))


#Policy head---------------------------------------------------------------------------------
p0_layer = trunk

#This is the main path for policy information
p1_num_channels = 48
p1_intermediate_conv = conv_only_block("p1/intermediate_conv",p0_layer,diam=3,in_channels=192,out_channels=p1_num_channels)

#But in parallel convolve to compute some features about the global state of the board
#Hopefully the neural net uses this for stuff like ko situation, overall temperature/threatyness, who is leading, etc.
g1_num_channels = 32
g1_layer = conv_block("g1",p0_layer,diam=3,in_channels=192,out_channels=g1_num_channels)

#Fold g1 down to single values for the board.
#For stdev, add a tiny constant to ensure numeric stability
g1_mean = tf.reduce_mean(g1_layer,axis=[1,2],keep_dims=True)
g1_max = tf.reduce_max(g1_layer,axis=[1,2],keep_dims=True)
#g1_stdev = tf.sqrt(tf.reduce_mean(tf.square(g1_layer - g1_mean), axis=[1,2], keep_dims=True) + (1e-4))
g2_layer = tf.concat([g1_mean,g1_max],axis=3) #shape [b,1,1,2*convg1num_channels]
g2_num_channels = 2*g1_num_channels
outputs_by_layer.append(("g2",g2_layer))

#Transform them into the space of the policy features to act as biases for the policy
#Also divide the initial weights a bit more because we think these should matter a bit less than local shape stuff,
#by multiplying the number of inputs for purposes of weight initialization (currently mult by 4)
matmulg2w = weight_variable("matmulg2w",[g2_num_channels,p1_num_channels],g2_num_channels*4,p1_num_channels)
g3_layer = tf.tensordot(g2_layer,matmulg2w,axes=[[3],[0]])
outputs_by_layer.append(("g3",g3_layer))

#Add! This adds shapes [b,19,19,convp1_num_channels] + [b,1,1,convp1_num_channels]
#so the second one should get broadcast up to the size of the first one.
#We can think of p1 as being an ordinary convolution layer except that for every node of the convolution, the g2 values (g2_num_channels many of them)
#have been appended to the p0 incoming values (p0_num_channels * convp1diam * convp1diam many of them).
#The matrix matmulg2w is simply the set of weights for that additional part of the matrix. It's just that rather than appending beforehand,
#we multiply separately and add to the output afterward.
p1_intermediate_sum = p1_intermediate_conv + g3_layer

#And now apply batchnorm and crelu
p1_layer = tf.nn.crelu(batchnorm("p1/norm",p1_intermediate_sum))
outputs_by_layer.append(("p1",p1_layer))

#Finally, apply linear convolution to produce final output
#2x in_channels due to crelu
p2_layer = conv_only_block("p2",p1_layer,diam=5,in_channels=p1_num_channels*2,out_channels=1,scale_initial_weights=0.5)

add_lr_factor("p1/norm/beta:0",0.25)
add_lr_factor("p1/norm/gamma:0",0.25)
add_lr_factor("p2/w:0",0.25)

#Output symmetries - we apply symmetries during training by transforming the input and reverse-transforming the output
policy_output = apply_symmetry(p2_layer,symmetries,inverse=True)
policy_output = tf.reshape(policy_output, [-1] + target_shape)

#Add pass move based on the global g values
#matmulpass = weight_variable("matmulpass",[pass_num_channels,1],g2_num_channels,1)
#pass_output = tf.matmul(g2_output,matmulpass)
#outputs_by_layer.append(("pass",pass_output))
#policy_output = tf.concat([policy_output,pass_output],axis=1)
