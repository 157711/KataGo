import logging
import math
import traceback
import tensorflow as tf
import numpy as np

from board import Board

#Feature extraction functions-------------------------------------------------------------------

class ModelV3:
  NUM_BIN_INPUT_FEATURES = 22
  NUM_GLOBAL_INPUT_FEATURES = 15

  def __init__(self,config,pos_len,placeholders):
    self.pos_len = pos_len
    self.bin_input_shape = [self.pos_len*self.pos_len,ModelV3.NUM_BIN_INPUT_FEATURES]
    self.binp_input_shape = [ModelV3.NUM_BIN_INPUT_FEATURES,(self.pos_len*self.pos_len+7)//8]
    self.global_input_shape = [ModelV3.NUM_GLOBAL_INPUT_FEATURES]
    self.post_input_shape = [self.pos_len,self.pos_len,ModelV3.NUM_BIN_INPUT_FEATURES]
    self.policy_target_shape_nopass = [self.pos_len*self.pos_len]
    self.policy_target_shape = [self.pos_len*self.pos_len+1] #+1 for pass move
    self.policy_target_weight_shape = []
    self.value_target_shape = [3]
    self.miscvalues_target_shape = [5]
    self.scorevalue_target_shape = []
    self.utilityvar_target_shape = [4]
    self.ownership_target_shape = [self.pos_len,self.pos_len]
    self.target_weight_shape = []
    self.ownership_target_weight_shape = []

    self.pass_pos = self.pos_len * self.pos_len

    self.reg_variables = []
    self.lr_adjusted_variables = {}
    self.is_training = (placeholders["is_training"] if "is_training" in placeholders else tf.placeholder(tf.bool,name="is_training"))

    #Accumulates outputs for printing stats about their activations
    self.outputs_by_layer = []
    self.other_internal_outputs = []
    #Accumulates info about batch norm laywers
    self.batch_norms = {}

    self.build_model(config,placeholders)

  def assert_batched_shape(self,name,tensor,shape):
    if (len(tensor.shape) != len(shape)+1 or
        [int(tensor.shape[i+1].value) for i in range(len(shape))] != [int(x) for x in shape]):
      raise Exception("%s should have shape %s after a batch dimension but instead it had shape %s" % (
        name, str(shape), str([str(x.value) for x in tensor.shape])))

  def assert_shape(self,name,tensor,shape):
    if (len(tensor.shape) != len(shape) or
        [int(x.value) for x in tensor.shape] != [int(x) for x in shape]):
      raise Exception("%s should have shape %s but instead it had shape %s" % (
        name, str(shape), str([str(x.value) for x in tensor.shape])))

  def xy_to_tensor_pos(self,x,y):
    return y * self.pos_len + x
  def loc_to_tensor_pos(self,loc,board):
    assert(loc != Board.PASS_LOC)
    return board.loc_y(loc) * self.pos_len + board.loc_x(loc)

  def tensor_pos_to_loc(self,pos,board):
    if pos == self.pass_pos:
      return None
    pos_len = self.pos_len
    bsize = board.size
    assert(self.pos_len >= bsize)
    x = pos % pos_len
    y = pos // pos_len
    if x < 0 or x >= bsize or y < 0 or y >= bsize:
      return board.loc(-1,-1) #Return an illegal move since this is offboard
    return board.loc(x,y)

  def sym_tensor_pos(self,pos,symmetry):
    if pos == self.pass_pos:
      return pos
    pos_len = self.pos_len
    x = pos % pos_len
    y = pos // pos_len
    if symmetry >= 4:
      symmetry -= 4
      tmp = x
      x = y
      y = tmp
    if symmetry >= 2:
      symmetry -= 2
      x = pos_len-x-1
    if symmetry >= 1:
      symmetry -= 1
      y = pos_len-y-1
    return y * pos_len + x

  #Calls f on each location that is part of an inescapable atari, or a group that can be put into inescapable atari
  def iterLadders(self, board, f):
    chainHeadsSolved = {}
    copy = board.copy()

    bsize = board.size
    assert(self.pos_len >= bsize)

    for y in range(bsize):
      for x in range(bsize):
        pos = self.xy_to_tensor_pos(x,y)
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
  #TODO incomplete, need rules
  def fill_row_features(self, board, pla, opp, boards, moves, move_idx, rules, bin_input_data, global_input_data, use_history_prop, idx):
    bsize = board.size
    assert(self.pos_len >= bsize)
    assert(len(boards) > 0)
    assert(board.zobrist == boards[move_idx].zobrist)

    for y in range(bsize):
      for x in range(bsize):
        pos = self.xy_to_tensor_pos(x,y)
        bin_input_data[idx,pos,0] = 1.0
        loc = board.loc(x,y)
        stone = board.board[loc]
        if stone == pla:
          bin_input_data[idx,pos,1] = 1.0
        elif stone == opp:
          bin_input_data[idx,pos,2] = 1.0

        if stone == pla or stone == opp:
          libs = board.num_liberties(loc)
          if libs == 1:
            bin_input_data[idx,pos,3] = 1.0
          elif libs == 2:
            bin_input_data[idx,pos,4] = 1.0
          elif libs == 3:
            bin_input_data[idx,pos,5] = 1.0

    #Python code does NOT handle superko
    if board.simple_ko_point is not None:
      pos = self.loc_to_tensor_pos(board.simple_ko_point,board)
      bin_input_data[idx,pos,6] = 1.0
    #Python code does NOT handle ko-prohibited encore spots or anything relating to the encore
    #so features 7 and 8 leave that blank

    if use_history_prop > 0.0:
      if move_idx >= 1 and moves[move_idx-1][0] == opp:
        prev1_loc = moves[move_idx-1][1]
        if prev1_loc is not None and prev1_loc != Board.PASS_LOC:
          pos = self.loc_to_tensor_pos(prev1_loc,board)
          bin_input_data[idx,pos,9] = use_history_prop
        elif prev1_loc == Board.PASS_LOC:
          global_input_data[idx,0] = use_history_prop

        if move_idx >= 2 and moves[move_idx-2][0] == pla:
          prev2_loc = moves[move_idx-2][1]
          if prev2_loc is not None and prev2_loc != Board.PASS_LOC:
            pos = self.loc_to_tensor_pos(prev2_loc,board)
            bin_input_data[idx,pos,10] = use_history_prop
          elif prev2_loc == Board.PASS_LOC:
            global_input_data[idx,1] = use_history_prop

          if move_idx >= 3 and moves[move_idx-3][0] == opp:
            prev3_loc = moves[move_idx-3][1]
            if prev3_loc is not None and prev3_loc != Board.PASS_LOC:
              pos = self.loc_to_tensor_pos(prev3_loc,board)
              bin_input_data[idx,pos,11] = use_history_prop
            elif prev3_loc == Board.PASS_LOC:
              global_input_data[idx,2] = use_history_prop

            if move_idx >= 4 and moves[move_idx-4][0] == pla:
              prev4_loc = moves[move_idx-4][1]
              if prev4_loc is not None and prev4_loc != Board.PASS_LOC:
                pos = self.loc_to_tensor_pos(prev4_loc,board)
                bin_input_data[idx,pos,12] = use_history_prop
              elif prev4_loc == Board.PASS_LOC:
                global_input_data[idx,3] = use_history_prop

              if move_idx >= 5 and moves[move_idx-5][0] == opp:
                prev5_loc = moves[move_idx-5][1]
                if prev5_loc is not None and prev5_loc != Board.PASS_LOC:
                  pos = self.loc_to_tensor_pos(prev5_loc,board)
                  bin_input_data[idx,pos,13] = use_history_prop
                elif prev5_loc == Board.PASS_LOC:
                  global_input_data[idx,4] = use_history_prop

    def addLadderFeature(loc,pos,workingMoves):
      assert(board.board[loc] == Board.BLACK or board.board[loc] == Board.WHITE)
      bin_input_data[idx,pos,14] = 1.0
      if board.board[loc] == opp and board.num_liberties(loc) > 1:
        for workingMove in workingMoves:
          workingPos = self.loc_to_tensor_pos(workingMove,board)
          bin_input_data[idx,workingPos,17] = 1.0

    self.iterLadders(board, addLadderFeature)

    if move_idx > 0 and use_history_prop > 0.0:
      prevBoard = boards[move_idx-1]
    else:
      prevBoard = board
    def addPrevLadderFeature(loc,pos,workingMoves):
      assert(prevBoard.board[loc] == Board.BLACK or prevBoard.board[loc] == Board.WHITE)
      bin_input_data[idx,pos,15] = 1.0
    self.iterLadders(prevBoard, addPrevLadderFeature)

    if move_idx > 1 and use_history_prop > 0.0:
      prevPrevBoard = boards[move_idx-2]
    else:
      prevPrevBoard = prevBoard
    def addPrevPrevLadderFeature(loc,pos,workingMoves):
      assert(prevPrevBoard.board[loc] == Board.BLACK or prevPrevBoard.board[loc] == Board.WHITE)
      bin_input_data[idx,pos,16] = 1.0
    self.iterLadders(prevPrevBoard, addPrevPrevLadderFeature)

    #Features 18,19 - current territory
    area = [-1 for i in range(board.arrsize)]
    if rules["scoringRule"] == "SCORING_AREA":
      nonPassAliveStones = True
      safeBigTerritories = True
      unsafeBigTerritories = True
      board.calculateArea(area,nonPassAliveStones,safeBigTerritories,unsafeBigTerritories,rules["multiStoneSuicideLegal"])
    elif rules["scoringRule"] == "SCORING_TERRITORY":
      nonPassAliveStones = False
      safeBigTerritories = True
      unsafeBigTerritories = False
      board.calculateArea(area,nonPassAliveStones,safeBigTerritories,unsafeBigTerritories,rules["multiStoneSuicideLegal"])
    else:
      assert(False)

    for y in range(bsize):
      for x in range(bsize):
        loc = board.loc(x,y)
        pos = self.xy_to_tensor_pos(x,y)

        if area[loc] == pla:
          bin_input_data[idx,pos,18] = 1.0
        elif area[loc] == opp:
          bin_input_data[idx,pos,19] = 1.0

    #Features 20,21 - second encore phase starting stones, we just set them to the current stones in pythong
    #since we don't really have a jp rules impl
    if rules["encorePhase"] >= 2:
      for y in range(bsize):
        for x in range(bsize):
          pos = self.xy_to_tensor_pos(x,y)
          loc = board.loc(x,y)
          stone = board.board[loc]
          if stone == pla:
            bin_input_data[idx,pos,20] = 1.0
          elif stone == opp:
            bin_input_data[idx,pos,21] = 1.0


    #Not quite right, japanese rules aren't really implemented in the python
    bArea = board.size * board.size
    selfKomi = rules["selfKomi"]
    if selfKomi > bArea+1:
      selfKomi = bArea+1
    if selfKomi < -bArea-1:
      selfKomi = -bArea-1
    global_input_data[idx,5] = selfKomi/15.0

    if rules["koRule"] == "KO_SIMPLE":
      pass
    elif rules["koRule"] == "KO_POSITIONAL" or rules["koRule"] == "KO_SPIGHT":
      global_input_data[idx,6] = 1.0
      global_input_data[idx,7] = 0.5
    elif rules["koRule"] == "KO_SITUATIONAL":
      global_input_data[idx,6] = 1.0
      global_input_data[idx,7] = -0.5
    else:
      assert(False)

    if rules["multiStoneSuicideLegal"]:
      global_input_data[idx,8] = 1.0

    if rules["scoringRule"] == "SCORING_AREA":
      pass
    elif rules["scoringRule"] == "SCORING_TERRITORY":
      global_input_data[idx,9] = 1.0
    else:
      assert(False)

    if rules["encorePhase"] > 0:
      global_input_data[idx,10] = 1.0
    if rules["encorePhase"] > 1:
      global_input_data[idx,11] = 1.0

    passWouldEndPhase = rules["passWouldEndPhase"]
    global_input_data[idx,12] = (1.0 if passWouldEndPhase else 0.0)

    global_input_data[idx,13] = board.size / 16.0

    if rules["scoringRule"] == "SCORING_AREA":
      boardAreaIsEven = (board.size % 2 == 0)

      drawableKomisAreEven = boardAreaIsEven

      if drawableKomisAreEven:
        komiFloor = math.floor(selfKomi / 2.0) * 2.0
      else:
        komiFloor = math.floor((selfKomi-1.0) / 2.0) * 2.0 + 1.0

      delta = selfKomi - komiFloor
      assert(delta >= -0.0001)
      assert(delta <= 2.0001)
      if delta < 0.0:
        delta = 0.0
      if delta > 2.0:
        delta = 2.0

      if delta < 0.5:
        wave = delta
      elif delta < 1.5:
        wave = 1.0-delta
      else:
        wave = delta-2.0

      global_input_data[idx,14] = wave

    return idx+1


  # Build model -------------------------------------------------------------

  def ensure_variable_exists(self,name):
    for v in tf.trainable_variables():
      if v.name == name:
        return name
    raise Exception("Could not find variable " + name)

  def add_lr_factor(self,name,factor):
    self.ensure_variable_exists(name)
    if name in self.lr_adjusted_variables:
      self.lr_adjusted_variables[name] = factor * self.lr_adjusted_variables[name]
    else:
      self.lr_adjusted_variables[name] = factor

  def batchnorm_and_mask(self,name,tensor,mask,mask_sum):
    epsilon = 0.001
    has_bias = True
    has_scale = False
    self.batch_norms[name] = (tensor.shape[-1].value,epsilon,has_bias,has_scale)

    num_channels = tensor.shape[3].value
    collections = [tf.GraphKeys.GLOBAL_VARIABLES,tf.GraphKeys.MODEL_VARIABLES,tf.GraphKeys.MOVING_AVERAGE_VARIABLES]

    #Define variables to keep track of the mean and variance
    moving_mean = tf.Variable(tf.zeros([num_channels]),name=(name+"/moving_mean"),trainable=False,collections=collections)
    moving_var = tf.Variable(tf.ones([num_channels]),name=(name+"/moving_variance"),trainable=False,collections=collections)
    beta = self.weight_variable_init_constant(name+"/beta", [tensor.shape[3].value], 0.0, reg=False)

    #This is the mean, computed only over exactly the areas of the mask, weighting each spot equally,
    #even across different elements in the batch that might have different board sizes.
    mean = tf.reduce_sum(tensor * mask,axis=[0,1,2]) / mask_sum
    zmtensor = tensor-mean
    #Similarly, the variance computed exactly only over those spots
    var = tf.reduce_sum(tf.square(zmtensor * mask),axis=[0,1,2]) / mask_sum
    mean_op = tf.keras.backend.moving_average_update(moving_mean,mean,0.998)
    var_op = tf.keras.backend.moving_average_update(moving_var,var,0.998)
    tf.add_to_collection(tf.GraphKeys.UPDATE_OPS, mean_op)
    tf.add_to_collection(tf.GraphKeys.UPDATE_OPS, var_op)

    def training_f():
      return (mean,var)
    def inference_f():
      return (moving_mean,moving_var)

    use_mean,use_var = tf.cond(self.is_training,training_f,inference_f)
    return tf.nn.batch_normalization(tensor,use_mean,use_var,beta,None,epsilon) * mask

  # def batchnorm(self,name,tensor):
  #   epsilon = 0.001
  #   has_bias = True
  #   has_scale = False
  #   self.batch_norms[name] = (tensor.shape[-1].value,epsilon,has_bias,has_scale)
  #   return tf.layers.batch_normalization(
  #     tensor,
  #     axis=-1, #Because channels are our last axis, -1 refers to that via wacky python indexing
  #     momentum=0.99,
  #     epsilon=epsilon,
  #     center=has_bias,
  #     scale=has_scale,
  #     training=self.is_training,
  #     name=name,
  #   )

  def init_stdev(self,num_inputs,num_outputs):
    #xavier
    #return math.sqrt(2.0 / (num_inputs + num_outputs))
    #herangzhen
    return math.sqrt(2.0 / (num_inputs))

  def init_weights(self, shape, num_inputs, num_outputs):
    stdev = self.init_stdev(num_inputs,num_outputs) / 1.0
    return tf.truncated_normal(shape=shape, stddev=stdev)

  def weight_variable_init_constant(self, name, shape, constant, reg=True):
    init = tf.zeros(shape)
    if constant != 0.0:
      init = init + constant
    variable = tf.Variable(init,name=name)
    if reg:
      self.reg_variables.append(variable)
    return variable

  def weight_variable(self, name, shape, num_inputs, num_outputs, scale_initial_weights=1.0, extra_initial_weight=None, reg=True):
    initial = self.init_weights(shape, num_inputs, num_outputs)
    if extra_initial_weight is not None:
      initial = initial + extra_initial_weight
    initial = initial * scale_initial_weights

    variable = tf.Variable(initial,name=name)
    if reg:
      self.reg_variables.append(variable)
    return variable

  def conv2d(self, x, w):
    return tf.nn.conv2d(x, w, strides=[1,1,1,1], padding='SAME')

  def dilated_conv2d(self, x, w, dilation):
    return tf.nn.atrous_conv2d(x, w, rate = dilation, padding='SAME')

  def apply_symmetry(self,tensor,symmetries,inverse):
    ud = symmetries[0]
    lr = symmetries[1]
    transp = symmetries[2]

    if not inverse:
      tensor = tf.cond(
        ud,
        lambda: tf.reverse(tensor,[1]),
        lambda: tensor
      )
      tensor = tf.cond(
        lr,
        lambda: tf.reverse(tensor,[2]),
        lambda: tensor
      )

    tensor = tf.cond(
      transp,
      lambda: tf.transpose(tensor, [0,2,1,3]),
      lambda: tensor)

    if inverse:
      tensor = tf.cond(
        ud,
        lambda: tf.reverse(tensor,[1]),
        lambda: tensor
      )
      tensor = tf.cond(
        lr,
        lambda: tf.reverse(tensor,[2]),
        lambda: tensor
      )

    return tensor

  #Define useful components --------------------------------------------------------------------------

  def relu(self, name, layer):
    assert(len(layer.shape) == 4)
    #num_channels = layer.shape[3].value
    #alphas = self.weight_variable_init_constant(name+"/relu",[1,1,1,num_channels],constant=0.0)
    return tf.nn.relu(layer)

  def relu_non_spatial(self, name, layer):
    assert(len(layer.shape) == 2)
    #num_channels = layer.shape[1].value
    #alphas = self.weight_variable_init_constant(name+"/relu",[1,num_channels],constant=0.0)
    return tf.nn.relu(layer)

  def merge_residual(self,name,trunk,residual):
    trunk = trunk + residual
    self.outputs_by_layer.append((name,trunk))
    return trunk

  def conv_weight_variable(self, name, diam1, diam2, in_channels, out_channels, scale_initial_weights=1.0, emphasize_center_weight=None, emphasize_center_lr=None, reg=True):
    radius1 = diam1 // 2
    radius2 = diam2 // 2

    if emphasize_center_weight is None:
      weights = self.weight_variable(name,[diam1,diam2,in_channels,out_channels],in_channels*diam1*diam2,out_channels,scale_initial_weights,reg=reg)
    else:
      extra_initial_weight = self.init_weights([1,1,in_channels,out_channels], in_channels, out_channels) * emphasize_center_weight
      extra_initial_weight = tf.pad(extra_initial_weight, [(radius1,radius1),(radius2,radius2),(0,0),(0,0)])
      weights = self.weight_variable(name,[diam1,diam2,in_channels,out_channels],in_channels*diam1*diam2,out_channels,scale_initial_weights,extra_initial_weight,reg=reg)

    if emphasize_center_lr is not None:
      factor = tf.constant([emphasize_center_lr],dtype=tf.float32)
      factor = tf.reshape(factor,[1,1,1,1])
      factor = tf.pad(factor, [(radius1,radius1),(radius2,radius2),(0,0),(0,0)], constant_values=1.0)
      self.add_lr_factor(weights.name, factor)

    return weights

  #Convolutional layer with batch norm and nonlinear activation
  def conv_block(
      self, name, in_layer, mask, mask_sum, diam, in_channels, out_channels,
      scale_initial_weights=1.0, emphasize_center_weight=None, emphasize_center_lr=None
  ):
    weights = self.conv_weight_variable(name+"/w", diam, diam, in_channels, out_channels, scale_initial_weights, emphasize_center_weight, emphasize_center_lr)
    convolved = self.conv2d(in_layer, weights)
    self.outputs_by_layer.append((name+"/prenorm",convolved))
    out_layer = self.relu(name+"/relu",self.batchnorm_and_mask(name+"/norm",convolved,mask,mask_sum))
    self.outputs_by_layer.append((name,out_layer))
    return out_layer

  #Convolution only, no batch norm or nonlinearity
  def conv_only_block(
      self, name, in_layer, diam, in_channels, out_channels,
      scale_initial_weights=1.0, emphasize_center_weight=None, emphasize_center_lr=None, reg=True
  ):
    weights = self.conv_weight_variable(name+"/w", diam, diam, in_channels, out_channels, scale_initial_weights, emphasize_center_weight, emphasize_center_lr, reg=reg)
    out_layer = self.conv2d(in_layer, weights)
    self.outputs_by_layer.append((name,out_layer))
    return out_layer

  #Convolutional residual block with internal batch norm and nonlinear activation
  def res_conv_block(
      self, name, in_layer, mask, mask_sum, diam, main_channels, mid_channels,
      scale_initial_weights=1.0, emphasize_center_weight=None, emphasize_center_lr=None
  ):
    trans1_layer = self.relu(name+"/relu1",(self.batchnorm_and_mask(name+"/norm1",in_layer,mask,mask_sum)))
    self.outputs_by_layer.append((name+"/trans1",trans1_layer))

    weights1 = self.conv_weight_variable(name+"/w1", diam, diam, main_channels, mid_channels, scale_initial_weights, emphasize_center_weight, emphasize_center_lr)
    conv1_layer = self.conv2d(trans1_layer, weights1)
    self.outputs_by_layer.append((name+"/conv1",conv1_layer))

    trans2_layer = self.relu(name+"/relu2",(self.batchnorm_and_mask(name+"/norm2",conv1_layer,mask,mask_sum)))
    self.outputs_by_layer.append((name+"/trans2",trans2_layer))

    weights2 = self.conv_weight_variable(name+"/w2", diam, diam, mid_channels, main_channels, scale_initial_weights, emphasize_center_weight, emphasize_center_lr)
    conv2_layer = self.conv2d(trans2_layer, weights2)
    self.outputs_by_layer.append((name+"/conv2",conv2_layer))

    return conv2_layer

  #Convolutional residual block with internal batch norm and nonlinear activation
  def global_res_conv_block(
      self, name, in_layer, mask, mask_sum, mask_sum_hw, diam, main_channels, mid_channels, global_mid_channels,
      scale_initial_weights=1.0, emphasize_center_weight=None, emphasize_center_lr=None
  ):
    trans1_layer = self.relu(name+"/relu1",(self.batchnorm_and_mask(name+"/norm1",in_layer,mask,mask_sum)))
    self.outputs_by_layer.append((name+"/trans1",trans1_layer))

    weights1a = self.conv_weight_variable(name+"/w1a", diam, diam, main_channels, mid_channels, scale_initial_weights, emphasize_center_weight, emphasize_center_lr)
    weights1b = self.conv_weight_variable(name+"/w1b", diam, diam, main_channels, global_mid_channels, scale_initial_weights, emphasize_center_weight, emphasize_center_lr)
    conv1a_layer = self.conv2d(trans1_layer, weights1a)
    conv1b_layer = self.conv2d(trans1_layer, weights1b)
    self.outputs_by_layer.append((name+"/conv1a",conv1a_layer))
    self.outputs_by_layer.append((name+"/conv1b",conv1b_layer))

    trans1b_layer = self.relu(name+"/trans1b",(self.batchnorm_and_mask(name+"/norm1b",conv1b_layer,mask,mask_sum)))
    trans1b_mean = tf.reduce_sum(trans1b_layer,axis=[1,2],keepdims=True) / tf.reshape(mask_sum_hw,[-1,1,1,1])
    trans1b_max = tf.reduce_max(trans1b_layer,axis=[1,2],keepdims=True)
    trans1b_pooled = tf.concat([trans1b_mean,trans1b_max],axis=3)

    remix_weights = self.weight_variable(name+"/w1r",[global_mid_channels*2,mid_channels],global_mid_channels*2,mid_channels, scale_initial_weights = 0.5)
    conv1_layer = conv1a_layer + tf.tensordot(trans1b_pooled,remix_weights,axes=[[3],[0]])

    trans2_layer = self.relu(name+"/relu2",(self.batchnorm_and_mask(name+"/norm2",conv1_layer,mask,mask_sum)))
    self.outputs_by_layer.append((name+"/trans2",trans2_layer))

    weights2 = self.conv_weight_variable(name+"/w2", diam, diam, mid_channels, main_channels, scale_initial_weights, emphasize_center_weight, emphasize_center_lr)
    conv2_layer = self.conv2d(trans2_layer, weights2)
    self.outputs_by_layer.append((name+"/conv2",conv2_layer))

    return conv2_layer

  #Convolutional residual block with internal batch norm and nonlinear activation
  def dilated_res_conv_block(self, name, in_layer, mask, mask_sum, diam, main_channels, mid_channels, dilated_mid_channels, dilation, scale_initial_weights=1.0, emphasize_center_weight=None, emphasize_center_lr=None):
    trans1_layer = self.relu(name+"/relu1",(self.batchnorm_and_mask(name+"/norm1",in_layer,mask,mask_sum)))
    self.outputs_by_layer.append((name+"/trans1",trans1_layer))

    weights1a = self.conv_weight_variable(name+"/w1a", diam, diam, main_channels, mid_channels, scale_initial_weights, emphasize_center_weight, emphasize_center_lr)
    weights1b = self.conv_weight_variable(name+"/w1b", diam, diam, main_channels, dilated_mid_channels, scale_initial_weights, emphasize_center_weight, emphasize_center_lr)
    conv1a_layer = self.conv2d(trans1_layer, weights1a)
    conv1b_layer = self.dilated_conv2d(trans1_layer, weights1b, dilation=dilation)
    self.outputs_by_layer.append((name+"/conv1a",conv1a_layer))
    self.outputs_by_layer.append((name+"/conv1b",conv1b_layer))

    conv1_layer = tf.concat([conv1a_layer,conv1b_layer],axis=3)

    trans2_layer = self.relu(name+"/relu2",(self.batchnorm_and_mask(name+"/norm2",conv1_layer,mask,mask_sum)))
    self.outputs_by_layer.append((name+"/trans2",trans2_layer))

    weights2 = self.conv_weight_variable(name+"/w2", diam, diam, mid_channels+dilated_mid_channels, main_channels, scale_initial_weights, emphasize_center_weight, emphasize_center_lr)
    conv2_layer = self.conv2d(trans2_layer, weights2)
    self.outputs_by_layer.append((name+"/conv2",conv2_layer))

    return conv2_layer


  #Begin Neural net------------------------------------------------------------------------------------
  #Indexing:
  #batch, bsize, bsize, channel

  def build_model(self,config,placeholders):
    pos_len = self.pos_len

    #Model version-------------------------------------------------------------------------------
    #This is written out in the model file when it gets built for export
    #self.version = 0 #V1 features, with old head architecture using crelus (no longer supported)
    #self.version = 1 #V1 features, with new head architecture, no crelus
    #self.version = 2 #V2 features, no internal architecture change.
    self.version = 3 #V3 features, selfplay-planned features with lots of aux targets

    #Input layer---------------------------------------------------------------------------------
    bin_inputs = (placeholders["bin_inputs"] if "bin_inputs" in placeholders else
                  tf.placeholder(tf.float32, [None] + self.bin_input_shape, name="bin_inputs"))
    global_inputs = (placeholders["global_inputs"] if "global_inputs" in placeholders else
                    tf.placeholder(tf.float32, [None] + self.global_input_shape, name="global_inputs"))
    symmetries = (placeholders["symmetries"] if "symmetries" in placeholders else
                  tf.placeholder(tf.bool, [3], name="symmetries"))
    include_history = (placeholders["include_history"] if "include_history" in placeholders else
                       tf.placeholder(tf.float32, [None] + [5], name="include_history"))

    self.assert_batched_shape("bin_inputs",bin_inputs,self.bin_input_shape)
    self.assert_batched_shape("global_inputs",global_inputs,self.global_input_shape)
    self.assert_shape("symmetries",symmetries,[3])
    self.assert_batched_shape("include_history",include_history,[5])

    self.bin_inputs = bin_inputs
    self.global_inputs = global_inputs
    self.symmetries = symmetries
    self.include_history = include_history

    cur_layer = tf.reshape(bin_inputs, [-1] + self.post_input_shape)

    input_num_channels = self.post_input_shape[2]

    mask_before_symmetry = cur_layer[:,:,:,0:1]

    #Input symmetries - we apply symmetries during training by transforming the input and reverse-transforming the output
    cur_layer = self.apply_symmetry(cur_layer,symmetries,inverse=False)

    # #Disable various features
    # features_active = tf.constant([
    #   1.0, #0
    #   1.0, #1
    #   1.0, #2
    #   1.0, #3
    #   1.0, #4
    #   1.0, #5
    #   1.0, #6
    #   1.0, #7
    #   1.0, #8
    #   1.0, #9
    #   1.0, #10
    #   1.0, #11
    #   1.0, #12
    #   1.0, #13
    #   1.0, #14
    #   1.0, #15
    #   1.0, #16
    # ])
    # assert(features_active.dtype == tf.float32)
    # cur_layer = cur_layer * tf.reshape(features_active,[1,1,1,-1])

    #Apply history transform to turn off various features randomly.
    #We do this by building a matrix for each batch element, mapping input channels to possibly-turned off channels.
    #This matrix is a sum of hist_matrix_base which always turns off all the channels, and h0, h1, h2,... which perform
    #the modifications to hist_matrix_base to make it turn on channels based on whether we have move0, move1,...
    hist_matrix_base = np.diag(np.array([
      1.0, #0
      1.0, #1
      1.0, #2
      1.0, #3
      1.0, #4
      1.0, #5
      1.0, #6
      1.0, #7
      1.0, #8
      0.0, #9
      0.0, #10
      0.0, #11
      0.0, #12
      0.0, #13
      1.0, #14
      0.0, #15
      0.0, #16
      1.0, #17
      1.0, #18
      1.0, #19
      1.0, #20
      1.0, #21
    ],dtype=np.float32))
    #Because we have ladder features that express past states rather than past diffs, the most natural encoding when
    #we have no history is that they were always the same, rather than that they were all zero. So rather than zeroing
    #them we have no history, we add entries in the matrix to copy them over.
    #By default, without history, the ladder features 15 and 16 just copy over from 14.
    hist_matrix_base[14,15] = 1.0
    hist_matrix_base[14,16] = 1.0
    #When have the prev move, we enable feature 9 and 15
    h0 = np.zeros([ModelV3.NUM_BIN_INPUT_FEATURES,ModelV3.NUM_BIN_INPUT_FEATURES],dtype=np.float32)
    h0[9,9] = 1.0 #Enable 9 -> 9
    h0[14,15] = -1.0 #Stop copying 14 -> 15
    h0[14,16] = -1.0 #Stop copying 14 -> 16
    h0[15,15] = 1.0 #Enable 15 -> 15
    h0[15,16] = 1.0 #Start copying 15 -> 16
    #When have the prevprev move, we enable feature 10 and 16
    h1 = np.zeros([ModelV3.NUM_BIN_INPUT_FEATURES,ModelV3.NUM_BIN_INPUT_FEATURES],dtype=np.float32)
    h1[10,10] = 1.0 #Enable 10 -> 10
    h1[15,16] = -1.0 #Stop copying 15 -> 16
    h1[16,16] = 1.0 #Enable 16 -> 16
    #Further history moves
    h2 = np.zeros([ModelV3.NUM_BIN_INPUT_FEATURES,ModelV3.NUM_BIN_INPUT_FEATURES],dtype=np.float32)
    h2[11,11] = 1.0
    h3 = np.zeros([ModelV3.NUM_BIN_INPUT_FEATURES,ModelV3.NUM_BIN_INPUT_FEATURES],dtype=np.float32)
    h3[12,12] = 1.0
    h4 = np.zeros([ModelV3.NUM_BIN_INPUT_FEATURES,ModelV3.NUM_BIN_INPUT_FEATURES],dtype=np.float32)
    h4[13,13] = 1.0

    hist_matrix_base = tf.reshape(tf.constant(hist_matrix_base),[1,ModelV3.NUM_BIN_INPUT_FEATURES,ModelV3.NUM_BIN_INPUT_FEATURES])
    hist_matrix_builder = tf.constant(np.array([h0,h1,h2,h3,h4]))
    assert(hist_matrix_base.dtype == tf.float32)
    assert(hist_matrix_builder.dtype == tf.float32)
    assert(len(hist_matrix_builder.shape) == 3)
    assert(hist_matrix_builder.shape[0].value == 5)
    assert(hist_matrix_builder.shape[1].value == ModelV3.NUM_BIN_INPUT_FEATURES)
    assert(hist_matrix_builder.shape[2].value == ModelV3.NUM_BIN_INPUT_FEATURES)

    hist_filter_matrix = hist_matrix_base + tf.tensordot(include_history, hist_matrix_builder, axes=[[1],[0]]) #[batch,move] * [move,inc,outc] = [batch,inc,outc]
    cur_layer = tf.reshape(cur_layer,[-1,self.pos_len*self.pos_len,ModelV3.NUM_BIN_INPUT_FEATURES]) #[batch,xy,inc]
    cur_layer = tf.matmul(cur_layer,hist_filter_matrix) #[batch,xy,inc] * [batch,inc,outc] = [batch,xy,outc]
    cur_layer = tf.reshape(cur_layer,[-1,self.pos_len,self.pos_len,ModelV3.NUM_BIN_INPUT_FEATURES])

    assert(include_history.shape[1].value == 5)
    transformed_global_inputs = global_inputs * tf.pad(include_history, [(0,0),(0,ModelV3.NUM_GLOBAL_INPUT_FEATURES - include_history.shape[1].value)], constant_values=1.0)

    self.transformed_bin_inputs = cur_layer
    self.transformed_global_inputs = transformed_global_inputs

    #Channel counts---------------------------------------------------------------------------------------
    trunk_num_channels = config["trunk_num_channels"]
    mid_num_channels = config["mid_num_channels"]
    regular_num_channels = config["regular_num_channels"]
    dilated_num_channels = config["dilated_num_channels"]
    gpool_num_channels = config["gpool_num_channels"]

    assert(regular_num_channels + dilated_num_channels == mid_num_channels)

    self.trunk_num_channels = trunk_num_channels
    self.mid_num_channels = mid_num_channels
    self.regular_num_channels = regular_num_channels
    self.dilated_num_channels = dilated_num_channels
    self.gpool_num_channels = gpool_num_channels

    mask = cur_layer[:,:,:,0:1]
    mask_sum = tf.reduce_sum(mask) # Global sum
    mask_sum_hw = tf.reduce_sum(mask,axis=[1,2,3]) # Sum per batch element

    #Initial convolutional layer-------------------------------------------------------------------------------------
    trunk = self.conv_only_block("conv1",cur_layer,diam=5,in_channels=input_num_channels,out_channels=trunk_num_channels, emphasize_center_weight = 0.3, emphasize_center_lr=1.5)
    self.initial_conv = ("conv1",5,input_num_channels,trunk_num_channels)

    #Matrix multiply global inputs and accumulate them
    ginputw = self.weight_variable("ginputw",[ModelV3.NUM_GLOBAL_INPUT_FEATURES,trunk_num_channels],ModelV3.NUM_GLOBAL_INPUT_FEATURES*2,trunk_num_channels)
    ginputresult = tf.tensordot(transformed_global_inputs,ginputw,axes=[[1],[0]])
    trunk = trunk + tf.reshape(ginputresult, [-1,1,1,trunk_num_channels])

    self.initial_matmul = ("ginputw",ModelV3.NUM_GLOBAL_INPUT_FEATURES,trunk_num_channels)

    #Main trunk---------------------------------------------------------------------------------------------------
    self.blocks = []

    block_kind = config["block_kind"]

    for i in range(len(block_kind)):
      (name,kind) = block_kind[i]
      if kind == "regular":
        residual = self.res_conv_block(
          name,trunk,mask,mask_sum,diam=3,main_channels=trunk_num_channels,mid_channels=mid_num_channels,
          emphasize_center_weight = 0.3, emphasize_center_lr=1.5)
        trunk = self.merge_residual(name,trunk,residual)
        self.blocks.append(("ordinary_block",name,3,trunk_num_channels,mid_num_channels))
      elif kind == "dilated":
        residual = self.dilated_res_conv_block(
          name,trunk,mask,mask_sum,diam=3,main_channels=trunk_num_channels,mid_channels=regular_num_channels, dilated_mid_channels=dilated_num_channels, dilation=2,
          emphasize_center_weight = 0.3, emphasize_center_lr=1.5
        )
        trunk = self.merge_residual(name,trunk,residual)
        self.blocks.append(("dilated_block",name,3,trunk_num_channels,regular_num_channels,dilated_num_channels,3))
      elif kind == "gpool":
        residual = self.global_res_conv_block(
          name,trunk,mask,mask_sum,mask_sum_hw,diam=3,main_channels=trunk_num_channels,mid_channels=regular_num_channels, global_mid_channels=dilated_num_channels,
          emphasize_center_weight = 0.3, emphasize_center_lr=1.5
        )
        trunk = self.merge_residual(name,trunk,residual)
        self.blocks.append(("gpool_block",name,3,trunk_num_channels,regular_num_channels,gpool_num_channels))
      else:
        assert(False)

    #Postprocessing residual trunk----------------------------------------------------------------------------------

    #Normalize and relu just before the policy head
    trunk = self.relu("trunk/relu",(self.batchnorm_and_mask("trunk/norm",trunk,mask,mask_sum)))
    self.outputs_by_layer.append(("trunk",trunk))


    #Policy head---------------------------------------------------------------------------------
    p0_layer = trunk

    #This is the main path for policy information
    p1_num_channels = config["p1_num_channels"]
    p1_intermediate_conv = self.conv_only_block("p1/intermediate_conv",p0_layer,diam=3,in_channels=trunk_num_channels,out_channels=p1_num_channels)
    self.p1_conv = ("p1/intermediate_conv",3,trunk_num_channels,p1_num_channels)

    #But in parallel convolve to compute some features about the global state of the board
    #Hopefully the neural net uses this for stuff like ko situation, overall temperature/threatyness, who is leading, etc.
    g1_num_channels = config["g1_num_channels"]
    g1_layer = self.conv_block("g1",p0_layer,mask,mask_sum,diam=3,in_channels=trunk_num_channels,out_channels=g1_num_channels)
    self.g1_conv = ("g1",3,trunk_num_channels,g1_num_channels)

    #Fold g1 down to single values for the board.
    #For stdev, add a tiny constant to ensure numeric stability
    g1_mean = tf.reduce_sum(g1_layer,axis=[1,2],keepdims=True) / tf.reshape(mask_sum_hw,[-1,1,1,1])
    g1_max = tf.reduce_max(g1_layer,axis=[1,2],keepdims=True)
    g2_layer = tf.concat([g1_mean,g1_max],axis=3) #shape [b,1,1,2*convg1num_channels]
    g2_num_channels = 2*g1_num_channels
    self.outputs_by_layer.append(("g2",g2_layer))

    #Transform them into the space of the policy features to act as biases for the policy
    #Also divide the initial weights a bit more because we think these should matter a bit less than local shape stuff,
    #by multiplying the number of inputs for purposes of weight initialization (currently mult by 4)
    matmulg2w = self.weight_variable("matmulg2w",[g2_num_channels,p1_num_channels],g2_num_channels*4,p1_num_channels)
    g3_layer = tf.tensordot(g2_layer,matmulg2w,axes=[[3],[0]])
    self.outputs_by_layer.append(("g3",g3_layer))
    self.g1_num_channels = g1_num_channels
    self.g2_num_channels = g2_num_channels
    self.p1_num_channels = p1_num_channels

    #Add! This adds shapes [b,19,19,convp1_num_channels] + [b,1,1,convp1_num_channels]
    #so the second one should get broadcast up to the size of the first one.
    #We can think of p1 as being an ordinary convolution layer except that for every node of the convolution, the g2 values (g2_num_channels many of them)
    #have been appended to the p0 incoming values (p0_num_channels * convp1diam * convp1diam many of them).
    #The matrix matmulg2w is simply the set of weights for that additional part of the matrix. It's just that rather than appending beforehand,
    #we multiply separately and add to the output afterward.
    p1_intermediate_sum = p1_intermediate_conv + g3_layer

    #And now apply batchnorm and relu
    p1_layer = self.relu("p1/relu",self.batchnorm_and_mask("p1/norm",p1_intermediate_sum,mask,mask_sum))
    self.outputs_by_layer.append(("p1",p1_layer))

    #Finally, apply linear convolution to produce final output
    p2_layer = self.conv_only_block("p2",p1_layer,diam=1,in_channels=p1_num_channels,out_channels=1,scale_initial_weights=0.5,reg=False)
    p2_layer = p2_layer - (1.0-mask) * 5000.0 # mask out parts outside the board by making them a huge neg number, so that they're 0 after softmax
    self.p2_conv = ("p2",1,p1_num_channels,1)

    self.add_lr_factor("p1/norm/beta:0",0.25)
    self.add_lr_factor("p2/w:0",0.25)

    #Output symmetries - we apply symmetries during training by transforming the input and reverse-transforming the output
    policy_output = self.apply_symmetry(p2_layer,symmetries,inverse=True)
    policy_output = tf.reshape(policy_output, [-1] + self.policy_target_shape_nopass)

    #Add pass move based on the global g values
    matmulpass = self.weight_variable("matmulpass",[g2_num_channels,1],g2_num_channels*8,1)
    self.add_lr_factor("matmulpass:0",0.25)
    pass_output = tf.tensordot(g2_layer,matmulpass,axes=[[3],[0]])
    self.outputs_by_layer.append(("pass",pass_output))
    pass_output = tf.reshape(pass_output, [-1] + [1])
    policy_output = tf.concat([policy_output,pass_output],axis=1, name="policy_output")

    self.policy_output = policy_output

    #Value head---------------------------------------------------------------------------------
    v0_layer = trunk

    v1_num_channels = config["v1_num_channels"]
    v1_layer = self.conv_block("v1",v0_layer,mask,mask_sum,diam=3,in_channels=trunk_num_channels,out_channels=v1_num_channels)
    self.outputs_by_layer.append(("v1",v1_layer))
    self.v1_conv = ("v1",3,trunk_num_channels,v1_num_channels)
    self.v1_num_channels = v1_num_channels

    v1_div = tf.reshape(mask_sum_hw,[-1,1])
    v1_div_sqrt = tf.sqrt(v1_div)
    v1_layer_raw_mean = tf.reduce_sum(v1_layer,axis=[1,2],keepdims=False) / v1_div

    # 1, (x-14)/10, and (x-14)^2/100 - 0.1 are three orthogonal functions over [9,19], the range of reasonable board sizes.
    # We have the 14 in there since it's the midpoint of that range. The /10 and /100 are just sort of arbitrary normalization.
    center_bsize = 14.0
    v1_layer_0 = v1_layer_raw_mean
    v1_layer_1 = v1_layer_raw_mean * ((v1_div_sqrt - center_bsize) / 10.0)
    v1_layer_2 = v1_layer_raw_mean * (tf.square(v1_div_sqrt - center_bsize) / 100.0 - 0.1)
    v1_layer_pooled = tf.concat([v1_layer_0,v1_layer_1,v1_layer_2],axis=1)
    v1_size = v1_num_channels

    v2_size = config["v2_size"]
    v2w = self.weight_variable("v2/w",[v1_size*3,v2_size],v1_size*3,v2_size)
    v2b = self.weight_variable("v2/b",[v2_size],v1_size*3,v2_size,scale_initial_weights=0.2,reg=False)
    v2_layer = self.relu_non_spatial("v2/relu",tf.matmul(v1_layer_pooled, v2w) + v2b)
    self.v2_size = v2_size
    self.other_internal_outputs.append(("v2",v2_layer))

    v3_size = self.value_target_shape[0]
    v3w = self.weight_variable("v3/w",[v2_size,v3_size],v2_size,v3_size)
    v3b = self.weight_variable("v3/b",[v3_size],v2_size,v3_size,scale_initial_weights=0.2,reg=False)
    v3_layer = tf.matmul(v2_layer, v3w) + v3b
    self.v3_size = v3_size
    self.other_internal_outputs.append(("v3",v3_layer))

    mv3_size = self.miscvalues_target_shape[0]
    mv3w = self.weight_variable("mv3/w",[v2_size,mv3_size],v2_size,mv3_size)
    mv3b = self.weight_variable("mv3/b",[mv3_size],v2_size,mv3_size,scale_initial_weights=0.1,reg=False)
    mv3_layer = tf.matmul(v2_layer, mv3w) + mv3b
    self.mv3_size = mv3_size
    self.other_internal_outputs.append(("mv3",mv3_layer))

    value_output = tf.reshape(v3_layer, [-1] + self.value_target_shape, name = "value_output")
    miscvalues_output = tf.reshape(mv3_layer, [-1] + self.miscvalues_target_shape, name = "miscvalues_output")

    ownership_output = self.conv_only_block("vownership",v1_layer,diam=1,in_channels=v1_num_channels,out_channels=1, scale_initial_weights=0.2, reg=False) * mask
    self.vownership_conv = ("vownership",1,v1_num_channels,1)
    ownership_output = self.apply_symmetry(ownership_output,symmetries,inverse=True)
    ownership_output = tf.reshape(ownership_output, [-1] + self.ownership_target_shape, name = "ownership_output")

    self.add_lr_factor("v2/w:0",0.25)
    self.add_lr_factor("v2/b:0",0.25)
    self.add_lr_factor("v3/w:0",0.25)
    self.add_lr_factor("v3/b:0",0.25)
    self.add_lr_factor("mv3/w:0",0.25)
    self.add_lr_factor("mv3/b:0",0.25)
    self.add_lr_factor("vownership/w:0",0.25)

    self.value_output = value_output
    self.miscvalues_output = miscvalues_output
    self.ownership_output = ownership_output

    self.mask_before_symmetry = mask_before_symmetry
    self.mask = mask
    self.mask_sum = mask_sum
    self.mask_sum_hw = mask_sum_hw


class Target_varsV3:
  def __init__(self,model,for_optimization,require_last_move,placeholders):
    policy_output = model.policy_output
    value_output = model.value_output
    miscvalues_output = model.miscvalues_output
    ownership_output = model.ownership_output

    #Loss function
    self.policy_target = (placeholders["policy_target"] if "policy_target" in placeholders else
                          tf.placeholder(tf.float32, [None] + model.policy_target_shape))
    self.value_target = (placeholders["value_target"] if "value_target" in placeholders else
                         tf.placeholder(tf.float32, [None] + model.value_target_shape))
    self.scorevalue_target = (placeholders["scorevalue_target"] if "scorevalue_target" in placeholders else
                              tf.placeholder(tf.float32, [None] + model.scorevalue_target_shape))
    self.utilityvar_target = (placeholders["utilityvar_target"] if "utilityvar_target" in placeholders else
                              tf.placeholder(tf.float32, [None] + model.utilityvar_target_shape))
    self.ownership_target = (placeholders["ownership_target"] if "ownership_target" in placeholders else
                             tf.placeholder(tf.float32, [None] + model.ownership_target_shape))
    self.target_weight_from_data = (placeholders["target_weight_from_data"] if "target_weight_from_data" in placeholders else
                                    tf.placeholder(tf.float32, [None] + model.target_weight_shape))
    self.policy_target_weight = (placeholders["policy_target_weight"] if "policy_target_weight" in placeholders else
                                 tf.placeholder(tf.float32, [None] + model.policy_target_weight_shape))
    self.ownership_target_weight = (placeholders["ownership_target_weight"] if "ownership_target_weight" in placeholders else
                                    tf.placeholder(tf.float32, [None] + model.ownership_target_weight))

    model.assert_batched_shape("policy_target", self.policy_target, model.policy_target_shape)
    model.assert_batched_shape("policy_target_weight", self.policy_target_weight, model.policy_target_weight_shape)
    model.assert_batched_shape("value_target", self.value_target, model.value_target_shape)
    model.assert_batched_shape("scorevalue_target", self.scorevalue_target, model.scorevalue_target_shape)
    model.assert_batched_shape("utilityvar_target", self.utilityvar_target, model.utilityvar_target_shape)
    model.assert_batched_shape("ownership_target", self.ownership_target, model.ownership_target_shape)
    model.assert_batched_shape("target_weight_from_data", self.target_weight_from_data, model.target_weight_shape)
    model.assert_batched_shape("policy_target_weight", self.policy_target_weight, model.policy_target_weight_shape)
    model.assert_batched_shape("ownership_target_weight", self.ownership_target_weight, model.ownership_target_weight_shape)

    if require_last_move == "all":
      self.target_weight_used = self.target_weight_from_data * tf.reduce_sum(model.inputs[:,:,13],axis=[1])
    elif require_last_move is True:
      self.target_weight_used = self.target_weight_from_data * tf.reduce_sum(model.inputs[:,:,9],axis=[1])
    else:
      self.target_weight_used = self.target_weight_from_data

    self.policy_loss_unreduced = self.policy_target_weight * (
      tf.nn.softmax_cross_entropy_with_logits_v2(labels=self.policy_target, logits=policy_output)
    )
    self.value_loss_unreduced = 0.5 * (
      1.4 * tf.nn.softmax_cross_entropy_with_logits_v2(
        labels=self.value_target,
        logits=value_output
      ) +
      2.0 * tf.reduce_sum(tf.square(self.value_target - tf.nn.softmax(value_output,axis=1)),axis=1)
    )
    self.scorevalue_loss_unreduced = 0.5 * (
      tf.square(self.scorevalue_target - tf.tanh(miscvalues_output[:,0]))
    )
    self.utilityvar_loss_unreduced = (1.0/16.0) * (
      tf.reduce_sum(tf.square(self.utilityvar_target - tf.math.softplus(miscvalues_output[:,1:5])),axis=1)
    )

    #This uses a formulation where each batch element cares about its average loss.
    #In particular this means that ownership loss predictions on small boards "count more" per spot.
    #Not unlike the way that policy and value loss are also equal-weighted by batch element.
    self.ownership_loss_unreduced = 0.25 * self.ownership_target_weight * (
      tf.reduce_sum(
        1.4*tf.nn.softmax_cross_entropy_with_logits_v2(
          labels=tf.stack([(1+self.ownership_target)/2,(1-self.ownership_target)/2],axis=3),
          logits=tf.stack([ownership_output,tf.zeros_like(ownership_output)],axis=3)
        ) * tf.reshape(model.mask_before_symmetry,[-1,model.pos_len,model.pos_len]),
        axis=[1,2]
      ) / model.mask_sum_hw
    )

    self.policy_loss = tf.reduce_sum(self.target_weight_used * self.policy_loss_unreduced)
    self.value_loss = tf.reduce_sum(self.target_weight_used * self.value_loss_unreduced)
    self.scorevalue_loss = tf.reduce_sum(self.target_weight_used * self.scorevalue_loss_unreduced)
    self.utilityvar_loss = tf.reduce_sum(self.target_weight_used * self.utilityvar_loss_unreduced)
    self.ownership_loss = tf.reduce_sum(self.target_weight_used * self.ownership_loss_unreduced)
    self.weight_sum = tf.reduce_sum(self.target_weight_used)

    if for_optimization:
      #Prior/Regularization
      self.l2_reg_coeff = (placeholders["l2_reg_coeff"] if "l2_reg_coeff" in placeholders else
                           tf.placeholder(tf.float32))
      self.reg_loss_per_weight = self.l2_reg_coeff * tf.add_n([tf.nn.l2_loss(variable) for variable in model.reg_variables])
      self.reg_loss = self.reg_loss_per_weight * self.weight_sum

      #The loss to optimize
      self.opt_loss = self.policy_loss + self.value_loss + self.scorevalue_loss + self.utilityvar_loss + self.ownership_loss + self.reg_loss

class MetricsV3:
  def __init__(self,model,target_vars,include_debug_stats):
    #Training results
    policy_target_idxs = tf.argmax(target_vars.policy_target, 1)
    self.top1_prediction = tf.equal(tf.argmax(model.policy_output, 1), policy_target_idxs)
    self.top4_prediction = tf.nn.in_top_k(model.policy_output,policy_target_idxs,4)
    self.accuracy1_unreduced = tf.cast(self.top1_prediction, tf.float32)
    self.accuracy4_unreduced = tf.cast(self.top4_prediction, tf.float32)
    self.value_entropy_unreduced = tf.nn.softmax_cross_entropy_with_logits_v2(labels=tf.nn.softmax(model.value_output,axis=1), logits=model.value_output)
    self.value_conf_unreduced = 4 * tf.square(tf.nn.sigmoid(model.value_output[:,0] - model.value_output[:,1]) - 0.5)
    self.accuracy1 = tf.reduce_sum(target_vars.target_weight_used * self.accuracy1_unreduced)
    self.accuracy4 = tf.reduce_sum(target_vars.target_weight_used * self.accuracy4_unreduced)
    self.value_entropy = tf.reduce_sum(target_vars.target_weight_used * self.value_entropy_unreduced)
    self.value_conf = tf.reduce_sum(target_vars.target_weight_used * self.value_conf_unreduced)

    #Debugging stats
    if include_debug_stats:

      def reduce_norm(x, axis=None, keepdims=False):
        return tf.sqrt(tf.reduce_mean(tf.square(x), axis=axis, keepdims=keepdims))

      def reduce_stdev(x, axis=None, keepdims=False):
        m = tf.reduce_mean(x, axis=axis, keepdims=True)
        devs_squared = tf.square(x - m)
        return tf.sqrt(tf.reduce_mean(devs_squared, axis=axis, keepdims=keepdims))

      self.activated_prop_by_layer = dict([
        (name,tf.reduce_mean(tf.count_nonzero(layer,axis=[1,2])/layer.shape[1].value/layer.shape[2].value, axis=0)) for (name,layer) in model.outputs_by_layer
      ])
      self.mean_output_by_layer = dict([
        (name,tf.reduce_mean(layer,axis=[0,1,2])) for (name,layer) in model.outputs_by_layer
      ])
      self.stdev_output_by_layer = dict([
        (name,reduce_stdev(layer,axis=[0,1,2])) for (name,layer) in model.outputs_by_layer
      ])
      self.mean_weights_by_var = dict([
        (v.name,tf.reduce_mean(v)) for v in tf.trainable_variables()
      ])
      self.norm_weights_by_var = dict([
        (v.name,reduce_norm(v)) for v in tf.trainable_variables()
      ])

def build_model_from_tfrecords_features(features,mode,print_model,trainlog,model_config,pos_len,num_batches_per_epoch):
  trainlog("Building model")

  #L2 regularization coefficient
  l2_coeff_value = 0.00003

  placeholders = {}

  binchwp = features["binchwp"]
  #Unpack binary data
  bitmasks = tf.reshape(tf.constant([128,64,32,16,8,4,2,1],dtype=tf.uint8),[1,1,1,8])
  binchw = tf.reshape(tf.bitwise.bitwise_and(tf.expand_dims(binchwp,axis=3),bitmasks),[-1,ModelV3.NUM_BIN_INPUT_FEATURES,((pos_len*pos_len+7)//8)*8])
  binchw = binchw[:,:,:pos_len*pos_len]
  binhwc = tf.cast(tf.transpose(binchw, [0,2,1]),tf.float32)
  binhwc = tf.math.minimum(binhwc,tf.constant(1.0))

  placeholders["bin_inputs"] = binhwc

  placeholders["global_inputs"] = features["ginc"]
  placeholders["symmetries"] = tf.greater(tf.random_uniform([3],minval=0,maxval=2,dtype=tf.int32),tf.zeros([3],dtype=tf.int32))

  if mode == tf.estimator.ModeKeys.PREDICT:
    placeholders["is_training"] = tf.constant(False,dtype=tf.bool)
    model = ModelV3(model_config,pos_len,placeholders)
    return model

  placeholders["include_history"] = features["gtnc"][:,28:33]

  policy_target0 = features["ptncm"][:,0,:]
  policy_target0 = policy_target0 / tf.reduce_sum(policy_target0,axis=1,keepdims=True)
  placeholders["policy_target"] = policy_target0
  placeholders["policy_target_weight"] = features["gtnc"][:,25]

  placeholders["value_target"] = features["gtnc"][:,0:3]
  placeholders["scorevalue_target"] = features["gtnc"][:,3]
  placeholders["utilityvar_target"] = features["gtnc"][:,21:25]
  placeholders["ownership_target"] = tf.reshape(features["vtnchw"],[-1,pos_len,pos_len])
  placeholders["target_weight_from_data"] = features["gtnc"][:,0]*0 + 1
  placeholders["ownership_target_weight"] = features["gtnc"][:,26]
  placeholders["l2_reg_coeff"] = tf.constant(l2_coeff_value,dtype=tf.float32)

  if mode == tf.estimator.ModeKeys.EVAL:
    placeholders["is_training"] = tf.constant(False,dtype=tf.bool)
    model = ModelV3(model_config,pos_len,placeholders)

    target_vars = Target_varsV3(model,for_optimization=True,require_last_move=False,placeholders=placeholders)
    metrics = MetricsV3(model,target_vars,include_debug_stats=False)
    return (model,target_vars,metrics)

  if mode == tf.estimator.ModeKeys.TRAIN:
    placeholders["is_training"] = tf.constant(True,dtype=tf.bool)
    model = ModelV3(model_config,pos_len,placeholders)

    target_vars = Target_varsV3(model,for_optimization=True,require_last_move=False,placeholders=placeholders)
    metrics = MetricsV3(model,target_vars,include_debug_stats=False)
    global_step = tf.train.get_global_step()
    global_step_float = tf.cast(global_step, tf.float32)
    global_epoch = global_step_float / tf.constant(num_batches_per_epoch,dtype=tf.float32)

    global_epoch_float_capped = tf.math.minimum(tf.constant(180.0),global_epoch)
    per_sample_learning_rate = (
      tf.constant(0.00020) / tf.pow(global_epoch_float_capped * tf.constant(0.1) + tf.constant(1.0), tf.constant(1.333333))
    )

    lr_adjusted_variables = model.lr_adjusted_variables
    update_ops = tf.get_collection(tf.GraphKeys.UPDATE_OPS) #collect batch norm update operations
    with tf.control_dependencies(update_ops):
      optimizer = tf.train.MomentumOptimizer(per_sample_learning_rate, momentum=0.9, use_nesterov=True)
      gradients = optimizer.compute_gradients(target_vars.opt_loss)
      adjusted_gradients = []
      for (grad,x) in gradients:
        adjusted_grad = grad
        if x.name in lr_adjusted_variables and grad is not None:
          adj_factor = lr_adjusted_variables[x.name]
          adjusted_grad = grad * adj_factor
          if print_model:
            trainlog("Adjusting gradient for " + x.name + " by " + str(adj_factor))

        adjusted_gradients.append((adjusted_grad,x))
      train_step = optimizer.apply_gradients(adjusted_gradients, global_step=global_step)


    if print_model:
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

    return (model,target_vars,metrics,global_step,global_step_float,per_sample_learning_rate,train_step)


