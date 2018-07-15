#ifndef NNEVAL_H
#define NNEVAL_H

#include <memory>

#include <tensorflow/cc/client/client_session.h>
#include <tensorflow/cc/ops/standard_ops.h>
#include <tensorflow/core/framework/tensor.h>
#include <tensorflow/core/framework/tensor_shape.h>
#include <tensorflow/core/platform/env.h>
#include <tensorflow/core/public/session.h>

using tensorflow::Tensor;
using tensorflow::Session;
using tensorflow::GraphDef;

#include "../core/global.h"
#include "../core/multithread.h"
#include "../game/board.h"
#include "../game/boardhistory.h"
#include "../neuralnet/nninputs.h"

class NNEvaluator;

struct NNOutput {
  //From the perspective of the player to move at the time of the eval
  float whiteValue;

  //Indexed by pos rather than loc
  //Values in here will be set to negative for illegal moves, including superko
  float policyProbs[NNPos::NN_POLICY_SIZE];

  NNOutput(); //Does NOT initialize values
  NNOutput(const NNOutput& other);

  //Utility --------------------------------------------------------------------
  //The utility of having a particular winner
  static double whiteValueOfWinner(Player winner);
  //The utility of achieving a certain score difference
  static double whiteValueOfScore(double finalWhiteMinusBlackScore, int bSize);
};

//Each thread should allocate and re-use one of these
struct NNResultBuf {
  condition_variable clientWaitingForResult;
  mutex resultMutex;
  bool hasResult;
  shared_ptr<NNOutput> result;

  NNResultBuf();
  ~NNResultBuf();
  NNResultBuf(const NNResultBuf& other) = delete;
  NNResultBuf& operator=(const NNResultBuf& other) = delete;
  NNResultBuf(NNResultBuf&& other) = delete;
  NNResultBuf& operator=(NNResultBuf&& other) = delete;
};

//Each server thread should allocate and re-use one of these
struct NNServerBuf {
  Session* session;
  vector<string> outputNames;
  vector<string> targetNames;
  vector<Tensor> outputsBuf;

  float* inputsBuffer;
  bool* symmetriesBuffer;
  vector<pair<string,Tensor>>* inputsList;
  NNResultBuf** resultBufs;

  NNServerBuf(const NNEvaluator& nneval);
  ~NNServerBuf();
  NNServerBuf(const NNServerBuf& other) = delete;
  NNServerBuf& operator=(const NNServerBuf& other) = delete;
  NNServerBuf(NNServerBuf&& other) = delete;
  NNServerBuf& operator=(NNServerBuf&& other) = delete;
};

class NNEvaluator {
 public:
  NNEvaluator(const string& pbModelFile, int maxBatchSize);
  ~NNEvaluator();

  int getMaxBatchSize() const;
  void killServers();
  void serve(NNServerBuf& buf, Rand* rand, int defaultSymmetry);
  void evaluate(Board& board, const BoardHistory& history, Player nextPlayer, NNResultBuf& buf);

 private:
  GraphDef* graphDef;

  condition_variable clientWaitingForRow;
  condition_variable serverWaitingForBatchStart;
  condition_variable serverWaitingForBatchFinish;
  mutex bufferMutex;
  bool isKilled;
  bool serverTryingToGrabBatch;

  int maxNumRows;
  int m_numRowsStarted;
  int m_numRowsFinished;

  float* m_inputsBuffer;
  bool* m_symmetriesBuffer;
  vector<pair<string,Tensor>>* m_inputsList;
  NNResultBuf** m_resultBufs;
};

#endif
