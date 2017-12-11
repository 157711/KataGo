#!/usr/bin/python3
import sys
import os
import argparse
import traceback
import random
import math
import time
import re
import logging
import colorsys
import tensorflow as tf
import numpy as np

from board import Board

description = """
Train neural net on Go games!
"""

parser = argparse.ArgumentParser(description=description)
parser.add_argument('-model', help='Path to model to use', required=True)
parser.add_argument('-white', help='Model plays white', action="store_true", required=False)
args = vars(parser.parse_args())

modelpath = args["model"]
is_white = args["white"]

# Model ----------------------------------------------------------------

import model
policy_output = tf.nn.softmax(model.policy_output)

# Moves ----------------------------------------------------------------

def fetch_output(session, board, moves, fetch):
  input_data = np.zeros(shape=[1]+model.input_shape, dtype=np.float32)
  pla = board.pla
  opp = Board.get_opp(pla)
  move_idx = len(moves)
  model.fill_row_features(board,pla,opp,moves,move_idx,input_data,target_data=None,target_data_mask=None,for_training=False,idx=0)

  output = session.run(fetches=[fetch], feed_dict={
    model.inputs: input_data,
    model.symmetries: [False,False,False],
    model.is_training: False
  })
  return output[0][0]

def get_policy_output(session, board, moves):
  return fetch_output(session,board,moves,policy_output)

def get_moves_and_probs(session, board, moves):
  pla = board.pla
  policy = get_policy_output(session, board, moves)
  moves_and_probs = []
  for i in range(len(policy)):
    move = model.tensor_pos_to_loc(i,board)
    if board.would_be_legal(pla,move) and not board.is_simple_eye(pla,move):
      moves_and_probs.append((move,policy[i]))
  return moves_and_probs

def genmove(session, board, moves):
  moves_and_probs = get_moves_and_probs(session, board, moves)
  moves_and_probs = sorted(moves_and_probs, key=lambda moveandprob: moveandprob[1], reverse=True)

  #Generate a random number biased small and then find the appropriate move to make
  #Interpolate from moving uniformly to choosing from the triangular distribution
  alpha = 1
  beta = 1 + math.sqrt(len(moves)) / 2
  r = np.random.beta(alpha,beta)
  probsum = 0.0
  i = 0
  while True:
    (move,prob) = moves_and_probs[i]
    probsum += prob
    if i >= len(moves_and_probs)-1 or probsum > r:
      return move
    i += 1

def get_layer_values(session, board, moves, layer, channel):
  layer = fetch_output(session,board,moves,layer)
  layer = layer.reshape([board.size * board.size,-1])
  locs_and_values = []
  for i in range(board.size * board.size):
    loc = model.tensor_pos_to_loc(i,board)
    locs_and_values.append((loc,layer[i,channel]))
  return locs_and_values


def fill_gfx_commands_for_heatmap(gfx_commands, locs_and_values, board):
  for (loc,value) in locs_and_values:
    if loc is not None:
      hueshift = 0.0
      huemult = 1.0
      if value < 0:
        value = -value
        huemult = 0.8
        hueshift = 0.5

      if value <= 0.02:
        (r,g,b) = colorsys.hls_to_rgb(hueshift + huemult*0.0, 0.5, max(value,0.0) / 0.02)
      elif value <= 0.80:
        hue = (value-0.02)/(0.80-0.02) * 0.45
        (r,g,b) = colorsys.hsv_to_rgb(hueshift + huemult*hue, 1.0, 1.0)
      else:
        lightness = 0.5 + 0.25*(min(value,1.0)-0.80)/(1.00-0.80)
        (r,g,b) = colorsys.hls_to_rgb(hueshift + huemult*0.45, lightness, 1.0)

      r = ("%02x" % int(r*255))
      g = ("%02x" % int(g*255))
      b = ("%02x" % int(b*255))
      gfx_commands.append("COLOR #%s%s%s %s" % (r,g,b,str_coord(loc,board)))


# Basic parsing --------------------------------------------------------
colstr = 'ABCDEFGHJKLMNOPQRST'
def parse_coord(s,board):
  if s == 'pass':
    return None
  return board.loc(colstr.index(s[0].upper()), board.size - int(s[1:]))

def str_coord(loc,board):
  if loc is None:
    return 'pass'
  x = board.loc_x(loc)
  y = board.loc_y(loc)
  return '%c%d' % (colstr[x], board.size - y)


# GTP Implementation -----------------------------------------------------

#Adapted from https://github.com/pasky/michi/blob/master/michi.py, which is distributed under MIT license
#https://opensource.org/licenses/MIT
def run_gtp(session):
  known_commands = [
    'boardsize',
    'clear_board',
    'komi',
    'play',
    'genmove',
    'final_score',
    'quit',
    'name',
    'version',
    'known_command',
    'list_commands',
    'protocol_version',
    'gogui-analyze_commands',
    'policy',
  ]
  known_analyze_commands = [
    'gfx/Policy/policy',
  ]

  board_size = 19
  board = Board(size=board_size)
  moves = []

  layerdict = dict(model.outputs_by_layer)
  layer_command_lookup = dict()

  def add_board_size_visualizations(layer_name):
    layer = layerdict[layer_name]
    assert(layer.shape[1].value == board_size)
    assert(layer.shape[2].value == board_size)
    num_channels = layer.shape[3].value
    for i in range(num_channels):
      command_name = layer_name + "-" + str(i)
      known_commands.append(command_name)
      known_analyze_commands.append("gfx/" + command_name + "/" + command_name)
      layer_command_lookup[command_name] = (layer,i)

  add_board_size_visualizations("conv1")
  add_board_size_visualizations("conv2")
  add_board_size_visualizations("conv3")
  add_board_size_visualizations("conv4")
  add_board_size_visualizations("conv5")
  add_board_size_visualizations("convg1")
  add_board_size_visualizations("convp1")

  while True:
    try:
      line = input().strip()
    except EOFError:
      break
    if line == '':
      continue
    command = [s.lower() for s in line.split()]
    if re.match('\d+', command[0]):
      cmdid = command[0]
      command = command[1:]
    else:
      cmdid = ''

    ret = ''
    if command[0] == "boardsize":
      if int(command[1]) > model.max_board_size:
        print("Warning: Trying to set incompatible boardsize %s (!= %d)" % (command[1], N), file=sys.stderr)
        ret = None
      board_size = int(command[1])
      board = Board(size=board_size)
      moves = []
    elif command[0] == "clear_board":
      board = Board(size=board_size)
      moves = []
    elif command[0] == "komi":
      pass
    elif command[0] == "play":
      loc = parse_coord(command[2],board)
      if loc is not None:
        moves.append((board.pla,loc))
        board.play(board.pla,loc)
      else:
        moves.append((board.pla,loc))
        board.do_pass()
    elif command[0] == "genmove":
      loc = genmove(session, board, moves)
      if loc is not None:
        moves.append((board.pla,loc))
        board.play(board.pla,loc)
        ret = str_coord(loc,board)
      else:
        moves.append((board.pla,loc))
        board.do_pass()
        ret = 'pass'
    elif command[0] == "final_score":
      ret = '0'
    elif command[0] == "name":
      ret = 'simplenn'
    elif command[0] == "version":
      ret = '0.1'
    elif command[0] == "list_commands":
      ret = '\n'.join(known_commands)
    elif command[0] == "known_command":
      ret = 'true' if command[1] in known_commands else 'false'
    elif command[0] == "gogui-analyze_commands":
      ret = '\n'.join(known_analyze_commands)
    elif command[0] == "policy":
      moves_and_probs = get_moves_and_probs(session, board, moves)
      gfx_commands = []
      fill_gfx_commands_for_heatmap(gfx_commands, moves_and_probs, board)

      moves_and_probs = sorted(moves_and_probs, key=lambda move_and_prob: move_and_prob[1], reverse=True)
      texts = []
      for i in range(min(len(moves_and_probs),8)):
        (move,prob) = moves_and_probs[i]
        texts.append("%s %4.1f%%" % (str_coord(move,board),prob*100))
      gfx_commands.append("TEXT " + ", ".join(texts))

      ret = "\n".join(gfx_commands)
    elif command[0] in layer_command_lookup:
      (layer,channel) = layer_command_lookup[command[0]]
      locs_and_values = get_layer_values(session, board, moves, layer, channel)
      max_abs_value = max(abs(value) for (loc,value) in locs_and_values)
      max_abs_value = max(0.0000000001,max_abs_value) #avoid divide by zero

      normalized = [(loc,value/max_abs_value) for (loc,value) in locs_and_values]
      gfx_commands = []
      fill_gfx_commands_for_heatmap(gfx_commands, normalized, board)
      ret = "\n".join(gfx_commands)

    elif command[0] == "protocol_version":
      ret = '2'
    elif command[0] == "quit":
      print('=%s \n\n' % (cmdid,), end='')
      break
    else:
      print('Warning: Ignoring unknown command - %s' % (line,), file=sys.stderr)
      ret = None

    if ret is not None:
      print('=%s %s\n\n' % (cmdid, ret,), end='')
    else:
      print('?%s ???\n\n' % (cmdid,), end='')
    sys.stdout.flush()

saver = tf.train.Saver(
  max_to_keep = 10000,
  save_relative_paths = True,
)

with tf.Session() as session:
  saver.restore(session, modelpath)
  run_gtp(session)



