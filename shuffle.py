#!/usr/bin/python3
import sys
import os
import argparse
import traceback
import math
import time
import logging
import zipfile
import shutil
import psutil

import multiprocessing

import numpy as np
import tensorflow as tf
from tensorflow.python_io import *

keys = [
  "binaryInputNCHWPacked",
  "floatInputNC",
  "policyTargetsNCMove",
  "floatTargetsNC",
  "valueTargetsNCHW"
]

def joint_shuffle(arrs):
  rand_state = np.random.get_state()
  for arr in arrs:
    assert(len(arr) == len(arrs[0]))
  for arr in arrs:
    np.random.set_state(rand_state)
    np.random.shuffle(arr)

def memusage_mb():
  return psutil.Process(os.getpid()).memory_info().rss // 1048576

def shardify(input_idx, input_file, num_out_files, out_tmp_dirs):
  np.random.seed([int.from_bytes(os.urandom(4), byteorder='little') for i in range(4)])

  print("Reading: " + input_file)
  npz = np.load(input_file)
  assert(set(npz.keys()) == set(keys))

  binaryInputNCHWPacked = npz["binaryInputNCHWPacked"]
  floatInputNC = npz["floatInputNC"]
  policyTargetsNCMove = npz["policyTargetsNCMove"]
  floatTargetsNC = npz["floatTargetsNC"]
  valueTargetsNCHW = npz["valueTargetsNCHW"]

  print("Shuffling... (mem usage %dMB)" % memusage_mb())
  joint_shuffle((binaryInputNCHWPacked,floatInputNC,policyTargetsNCMove,floatTargetsNC,valueTargetsNCHW))

  num_rows = binaryInputNCHWPacked.shape[0]
  rand_assts = np.random.randint(num_out_files,size=[num_rows])
  counts = np.bincount(rand_assts)
  countsums = np.cumsum(counts)

  print("Writing shards... (mem usage %dMB)" % memusage_mb())
  for out_idx in range(num_out_files):
    start = countsums[out_idx]-counts[out_idx]
    stop = countsums[out_idx]
    np.savez_compressed(
      os.path.join(out_tmp_dirs[out_idx], str(input_idx) + ".npz"),
      binaryInputNCHWPacked = binaryInputNCHWPacked[start:stop],
      floatInputNC = floatInputNC[start:stop],
      policyTargetsNCMove = policyTargetsNCMove[start:stop],
      floatTargetsNC = floatTargetsNC[start:stop],
      valueTargetsNCHW = valueTargetsNCHW[start:stop]
    )

def merge_shards(filename, num_shards_to_merge, out_tmp_dir, batch_size):
  print("Merging shards for output file: %s (%d shards to merge)" % (filename,num_shards_to_merge))
  tfoptions = TFRecordOptions(TFRecordCompressionType.ZLIB)
  record_writer = TFRecordWriter(filename,tfoptions)

  binaryInputNCHWPackeds = []
  floatInputNCs = []
  policyTargetsNCMoves = []
  floatTargetsNCs = []
  valueTargetsNCHWs = []

  for input_idx in range(num_shards_to_merge):
    shard_filename = os.path.join(out_tmp_dir, str(input_idx) + ".npz")
    print("Loading shard: %d (mem usage %dMB)" % (input_idx,memusage_mb()))

    npz = np.load(shard_filename)
    assert(set(npz.keys()) == set(keys))

    binaryInputNCHWPacked = npz["binaryInputNCHWPacked"]
    floatInputNC = npz["floatInputNC"]
    policyTargetsNCMove = npz["policyTargetsNCMove"].astype(np.float32)
    floatTargetsNC = npz["floatTargetsNC"]
    valueTargetsNCHW = npz["valueTargetsNCHW"].astype(np.float32)

    binaryInputNCHWPackeds.append(binaryInputNCHWPacked)
    floatInputNCs.append(floatInputNC)
    policyTargetsNCMoves.append(policyTargetsNCMove)
    floatTargetsNCs.append(floatTargetsNC)
    valueTargetsNCHWs.append(valueTargetsNCHW)

  print("Concatenating... (mem usage %dMB)" % memusage_mb())
  binaryInputNCHWPacked = np.concatenate(binaryInputNCHWPackeds)
  floatInputNC = np.concatenate(floatInputNCs)
  policyTargetsNCMove = np.concatenate(policyTargetsNCMoves)
  floatTargetsNC = np.concatenate(floatTargetsNCs)
  valueTargetsNCHW = np.concatenate(valueTargetsNCHWs)

  print("Shuffling... (mem usage %dMB)" % memusage_mb())
  joint_shuffle((binaryInputNCHWPacked,floatInputNC,policyTargetsNCMove,floatTargetsNC,valueTargetsNCHW))

  print("Writing in batches...")
  num_rows = binaryInputNCHWPacked.shape[0]
  #Just truncate and lose the batch at the end, it's fine
  num_batches = num_rows // batch_size
  for i in range(num_batches):
    start = i*batch_size
    stop = (i+1)*batch_size
    example = tf.train.Example(features=tf.train.Features(feature={
      "binchwp": tf.train.Feature(
        bytes_list=tf.train.BytesList(value=[binaryInputNCHWPacked[start:stop].reshape(-1).tobytes()])
      ),
      "finc": tf.train.Feature(
        float_list=tf.train.FloatList(value=floatInputNC[start:stop].reshape(-1))
      ),
      "ptncm": tf.train.Feature(
        float_list=tf.train.FloatList(value=policyTargetsNCMove[start:stop].reshape(-1))
      ),
      "ftnc": tf.train.Feature(
        float_list=tf.train.FloatList(value=floatTargetsNC[start:stop].reshape(-1))
      ),
      "vtnchw": tf.train.Feature(
        float_list=tf.train.FloatList(value=valueTargetsNCHW[start:stop].reshape(-1))
      )
    }))
    record_writer.write(example.SerializeToString())

  print("Done %s (%d rows)" % (filename, num_batches * batch_size))

  record_writer.close()


if __name__ == '__main__':
  parser = argparse.ArgumentParser(description='Shuffle data files')
  parser.add_argument('dirs', metavar='DIR', nargs='+', help='Directories of training data files')
  parser.add_argument('-min-rows', type=int, required=True, help='Minimum training rows to use')
  parser.add_argument('-max-rows', type=int, required=True, help='Maximum training rows to use')
  parser.add_argument('-window-factor', type=float, required=True, help='Beyond min rows, add 1 more row per this many')
  parser.add_argument('-out-dir', required=True, help='Dir to output training files')
  parser.add_argument('-approx-rows-per-out-file', type=int, required=True, help='Number of rows per output tf records file')
  parser.add_argument('-pos-len', type=int, required=True, help='Go spatial length dimension')
  parser.add_argument('-num-processes', type=int, required=True, help='Number of multiprocessing processes')
  parser.add_argument('-batch-size', type=int, required=True, help='Batck size to write training examples in')

  args = parser.parse_args()
  dirs = args.dirs
  min_rows = args.min_rows
  max_rows = args.max_rows
  window_factor = args.window_factor
  out_dir = args.out_dir
  approx_rows_per_out_file = args.approx_rows_per_out_file
  pos_len = args.pos_len
  num_processes = args.num_processes
  batch_size = args.batch_size

  all_files = []
  for d in dirs:
    print(d)
    for (path,dirnames,filenames) in os.walk(d):
      filenames = [os.path.join(path,filename) for filename in filenames if filename.endswith('.npz')]
      filenames = [(filename,os.path.getmtime(filename)) for filename in filenames]
      all_files.extend(filenames)

  all_files.sort(key=(lambda x: x[1]), reverse=True)


  def get_numpy_npz_headers(filename):
    with zipfile.ZipFile(filename) as z:
      numrows = 0
      npzheaders = {}
      for subfilename in z.namelist():
        npyfile = z.open(subfilename)
        version = np.lib.format.read_magic(npyfile)
        (shape, is_fortran, dtype) = np.lib.format._read_array_header(npyfile,version)
        npzheaders[subfilename] = (shape, is_fortran, dtype)
      return npzheaders


  files_with_num_rows = []
  num_rows_total = 0
  for (filename,mtime) in all_files:
    npheaders = get_numpy_npz_headers(filename)
    if len(npheaders) <= 0:
      continue
    (shape, is_fortran, dtype) = npheaders["binaryInputNCHWPacked"]
    num_rows = shape[0]
    num_rows_total += num_rows

    print("Training data file %s: %d rows" % (filename,num_rows))
    files_with_num_rows.append((filename,num_rows))

    #If we have more rows than we could possibly need to hit max rows, then just stop
    if num_rows_total >= min_rows + (max_rows - min_rows) * window_factor:
      break


  #Now assemble only the files we need to hit our desired window size
  desired_num_rows = int(min_rows + (num_rows_total - min_rows) / window_factor)
  desired_num_rows = max(desired_num_rows,min_rows)
  desired_num_rows = min(desired_num_rows,max_rows)
  desired_input_files = []
  num_rows_total = 0
  for (filename,num_rows) in files_with_num_rows:
    desired_input_files.append(filename)
    num_rows_total += num_rows
    print("Using: %s (%d/%d rows)" % (filename,num_rows_total,desired_num_rows))
    if num_rows_total >= desired_num_rows:
      break

  np.random.seed()
  np.random.shuffle(desired_input_files)

  num_out_files = int(round(num_rows_total / approx_rows_per_out_file))
  num_out_files = max(num_out_files,1)
  out_files = [os.path.join(out_dir, "data%d.tfrecord" % i) for i in range(num_out_files)]
  out_tmp_dirs = [os.path.join(out_dir, "tmp.shuf%d" % i) for i in range(num_out_files)]
  print("Writing %d output files" % num_out_files)

  def clean_tmp_dirs():
    for tmp_dir in out_tmp_dirs:
      if os.path.exists(tmp_dir):
        print("Cleaning up tmp dir: " + tmp_dir)
        shutil.rmtree(tmp_dir)

  clean_tmp_dirs()
  for tmp_dir in out_tmp_dirs:
    os.mkdir(tmp_dir)

  with multiprocessing.Pool(num_processes) as pool:
    pool.starmap(shardify, [
      (input_idx, desired_input_files[input_idx], num_out_files, out_tmp_dirs) for input_idx in range(len(desired_input_files))
    ])

    num_shards_to_merge = len(desired_input_files)
    pool.starmap(merge_shards, [
      (out_files[idx],num_shards_to_merge,out_tmp_dirs[idx],batch_size) for idx in range(len(out_files))
    ])



clean_tmp_dirs()
