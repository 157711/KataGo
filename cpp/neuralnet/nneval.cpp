
#include "../neuralnet/nneval.h"

using namespace tensorflow;

NNOutput::NNOutput() {}
NNOutput::NNOutput(const NNOutput& other) {
  whiteValue = other.whiteValue;
  std::copy(other.policyProbs, other.policyProbs+NNPos::NN_POLICY_SIZE, policyProbs);
}

double NNOutput::whiteValueOfWinner(Player winner) {
  if(winner == P_WHITE)
    return 1.0;
  else if(winner == P_BLACK)
    return -1.0;
  return 0.0;
}

double NNOutput::whiteValueOfScore(double finalWhiteMinusBlackScore, int bSize) {
  return tanh(finalWhiteMinusBlackScore / (bSize*2));
}

//-------------------------------------------------------------------------------------

NNResultBuf::NNResultBuf()
  :clientWaitingForResult(),resultMutex(),hasResult(false),result(nullptr)
{}

NNResultBuf::~NNResultBuf()
{}

//-------------------------------------------------------------------------------------

static void checkStatus(const Status& status, const char* subLabel) {
  if(!status.ok())
    throw StringError("NN Eval Error: " + string(subLabel) + status.ToString());
}

//-------------------------------------------------------------------------------------

static void initTensorIOBufs(
  int maxNumRows,
  float*& inputsBuffer,
  bool*& symmetriesBuffer,
  vector<pair<string,Tensor>>*& inputsList,
  NNResultBuf**& resultBufs
) {
  Status status;
  //Set up inputs
  TensorShape inputsShape;
  TensorShape symmetriesShape;
  TensorShape isTrainingShape;
  int inputsShapeArr[3] = {maxNumRows,NNPos::MAX_BOARD_AREA,NNInputs::NUM_FEATURES_V1};
  status = TensorShapeUtils::MakeShape(inputsShapeArr,3,&inputsShape);
  checkStatus(status,"making inputs shape");
  int symmetriesShapeArr[1] = {NNInputs::NUM_SYMMETRY_BOOLS};
  status = TensorShapeUtils::MakeShape(symmetriesShapeArr,1,&symmetriesShape);
  checkStatus(status,"making symmetries shape");
  int isTrainingShapeArr[0] = {};
  status = TensorShapeUtils::MakeShape(isTrainingShapeArr,0,&isTrainingShape);
  checkStatus(status,"making isTraining shape");

  Tensor inputs(DT_FLOAT,inputsShape);
  Tensor symmetries(DT_BOOL,symmetriesShape);
  Tensor isTraining(DT_BOOL,isTrainingShape);

  assert(inputs.IsAligned());
  assert(symmetries.IsAligned());

  inputsBuffer = inputs.flat<float>().data();
  symmetriesBuffer = symmetries.flat<bool>().data();
  auto isTrainingMap = isTraining.tensor<bool, 0>();
  isTrainingMap(0) = false;

  inputsList = new vector<pair<string,Tensor>>();
  *inputsList = {
    {"inputs",inputs},
    {"symmetries",symmetries},
    {"is_training",isTraining},
  };

  resultBufs = new NNResultBuf*[maxNumRows];
  for(int i = 0; i < maxNumRows; i++)
    resultBufs[i] = NULL;
}

static void freeTensorInputBufs(
  float*& inputsBuffer,
  bool*& symmetriesBuffer,
  vector<pair<string,Tensor>>*& inputsList,
  NNResultBuf**& resultBufs
) {
  //Clear these out - these are direct pointers into the inputs and symmetries tensor
  //and are invalid once inputList is cleared and those are freed
  inputsBuffer = NULL;
  symmetriesBuffer = NULL;

  //Explictly clean up tensors - their destructors should get called.
  if(inputsList != NULL)
    inputsList->clear();

  delete inputsList;
  inputsList = NULL;

  //Pointers inside here don't need to be deleted, they simply point to the clients waiting for results
  delete[] resultBufs;
  resultBufs = NULL;
}


NNServerBuf::NNServerBuf(const NNEvaluator& nnEval)
  :session(NULL),
   outputNames(),
   targetNames(),
   outputsBuf(),
   inputsBuffer(NULL),
   symmetriesBuffer(NULL),
   inputsList(NULL),
   resultBufs(NULL)
{
  Status status;

  //Create session
  status = NewSession(SessionOptions(), &session);
  checkStatus(status,"creating session");

  outputNames = {
    string("policy_output"),
    string("value_output")
  };
  targetNames = {};

  initTensorIOBufs(nnEval.getMaxBatchSize(), inputsBuffer, symmetriesBuffer, inputsList, resultBufs);
}

NNServerBuf::~NNServerBuf() {
  //Explictly clean up tensors - their destructors should get called.
  outputsBuf.clear();
  freeTensorInputBufs(inputsBuffer, symmetriesBuffer, inputsList, resultBufs);

  session->Close();
  session = NULL;
}

//-------------------------------------------------------------------------------------

NNEvaluator::NNEvaluator(const string& pbModelFile, int maxBatchSize)
  :clientWaitingForRow(),serverWaitingForBatchStart(),serverWaitingForBatchFinish(),
   bufferMutex(),
   isKilled(false),
   serverTryingToGrabBatch(false),
   maxNumRows(maxBatchSize),
   m_numRowsStarted(0),
   m_numRowsFinished(0),
   m_inputsBuffer(NULL),
   m_symmetriesBuffer(NULL),
   m_inputsList(NULL)
{
  Status status;
  graphDef = new GraphDef();

  //Read graph from file
  status = ReadBinaryProto(Env::Default(), pbModelFile, graphDef);
  checkStatus(status,"reading graph");

  initTensorIOBufs(maxNumRows, m_inputsBuffer, m_symmetriesBuffer, m_inputsList, m_resultBufs);
}

NNEvaluator::~NNEvaluator()
{
  assert(isKilled);
  assert(!serverTryingToGrabBatch);
  freeTensorInputBufs(m_inputsBuffer, m_symmetriesBuffer, m_inputsList, m_resultBufs);
  delete graphDef;
}

int NNEvaluator::getMaxBatchSize() const {
  return maxNumRows;
}

void NNEvaluator::killServers() {
  unique_lock<std::mutex> lock(bufferMutex);
  isKilled = true;
  lock.unlock();
  serverWaitingForBatchStart.notify_all();
  serverWaitingForBatchFinish.notify_all();
}

void NNEvaluator::serve(NNServerBuf& buf, Rand* rand, int defaultSymmetry) {
  Status status;
  //Add graph to session
  status = buf.session->Create(*graphDef);
  checkStatus(status,"adding graph to session");

  vector<pair<string,Tensor>> slicedInputsList;

  unique_lock<std::mutex> lock(bufferMutex,std::defer_lock);
  while(true) {
    lock.lock();
    while((m_numRowsStarted <= 0 || serverTryingToGrabBatch) && !isKilled)
      serverWaitingForBatchStart.wait(lock);

    if(isKilled)
      break;

    serverTryingToGrabBatch = true;
    while(m_numRowsFinished < m_numRowsStarted && !isKilled)
      serverWaitingForBatchFinish.wait(lock);

    if(isKilled)
      break;

    //It should only be possible for one thread to make it through to here
    assert(serverTryingToGrabBatch);
    assert(m_numRowsFinished > 0);

    int numRows = m_numRowsFinished;
    std::swap(m_inputsBuffer, buf.inputsBuffer);
    std::swap(m_symmetriesBuffer, buf.symmetriesBuffer);
    std::swap(m_inputsList,buf.inputsList);
    std::swap(m_resultBufs,buf.resultBufs);

    m_numRowsStarted = 0;
    m_numRowsFinished = 0;
    serverTryingToGrabBatch = false;
    clientWaitingForRow.notify_all();
    lock.unlock();

    slicedInputsList = *buf.inputsList;
    slicedInputsList[0].second = (*buf.inputsList)[0].second.Slice(0,numRows);

    int symmetry = defaultSymmetry;
    if(rand != NULL)
      symmetry = rand->nextUInt(NNInputs::NUM_SYMMETRY_COMBINATIONS);
    buf.symmetriesBuffer[0] = (symmetry & 0x1) != 0;
    buf.symmetriesBuffer[1] = (symmetry & 0x2) != 0;
    buf.symmetriesBuffer[2] = (symmetry & 0x4) != 0;

    status = buf.session->Run(slicedInputsList, buf.outputNames, buf.targetNames, &(buf.outputsBuf));
    checkStatus(status,"running inference");

    assert(buf.outputsBuf.size() == 2);
    assert(buf.outputsBuf[0].dims() == 2);
    assert(buf.outputsBuf[1].dims() == 1);
    assert(buf.outputsBuf[0].dim_size(0) == numRows);
    assert(buf.outputsBuf[0].dim_size(1) == NNPos::NN_POLICY_SIZE);
    assert(buf.outputsBuf[1].dim_size(0) == numRows);

    assert(buf.outputsBuf[0].IsAligned());
    assert(buf.outputsBuf[1].IsAligned());

    float* policyData = buf.outputsBuf[0].flat<float>().data();
    float* valueData = buf.outputsBuf[1].flat<float>().data();

    for(int row = 0; row < numRows; row++) {
      assert(buf.resultBufs[row] != NULL);
      NNResultBuf* resultBuf = buf.resultBufs[row];
      buf.resultBufs[row] = NULL;

      unique_lock<std::mutex> resultLock(resultBuf->resultMutex);
      assert(resultBuf->hasResult == false);
      resultBuf->result = std::make_shared<NNOutput>();
      float* policyProbs = resultBuf->result->policyProbs;

      //These are not actually correct, the client does the postprocessing to turn them into
      //probabilities and white value
      std::copy(
        policyData + row * NNPos::NN_POLICY_SIZE,
        policyData + (row+1) * NNPos::NN_POLICY_SIZE,
        policyProbs
      );
      resultBuf->result->whiteValue = valueData[row];
      resultBuf->hasResult = true;
      resultBuf->clientWaitingForResult.notify_all();
      resultLock.unlock();
    }
    buf.outputsBuf.clear();
    continue;
  }

}

void NNEvaluator::evaluate(Board& board, const BoardHistory& history, Player nextPlayer, NNResultBuf& buf) {
  assert(!isKilled);
  buf.hasResult = false;

  unique_lock<std::mutex> lock(bufferMutex);
  while(m_numRowsStarted >= maxNumRows || serverTryingToGrabBatch)
    clientWaitingForRow.wait(lock);

  int rowIdx = m_numRowsStarted;
  m_numRowsStarted += 1;
  float* rowInput = m_inputsBuffer + rowIdx * NNInputs::ROW_SIZE_V1;

  if(m_numRowsStarted == 1)
    serverWaitingForBatchStart.notify_one();
  lock.unlock();

  std::fill(rowInput,rowInput+NNInputs::ROW_SIZE_V1,0.0f);
  NNInputs::fillRowV1(board, history, nextPlayer, rowInput);

  lock.lock();
  m_resultBufs[rowIdx] = &buf;
  m_numRowsFinished += 1;
  if(m_numRowsFinished >= m_numRowsStarted)
    serverWaitingForBatchFinish.notify_all();
  lock.unlock();

  unique_lock<std::mutex> resultLock(buf.resultMutex);
  while(!buf.hasResult)
    buf.clientWaitingForResult.wait(resultLock);
  resultLock.unlock();

  float* policy = buf.result->policyProbs;

  assert(board.x_size == board.y_size);
  int bSize = board.x_size;
  int offset = NNPos::getOffset(bSize);

  float maxPolicy = -1e25f;
  bool isLegal[NNPos::NN_POLICY_SIZE];
  int legalCount = 0;
  for(int i = 0; i<NNPos::NN_POLICY_SIZE; i++) {
    Loc loc = NNPos::posToLoc(i,bSize,offset);
    isLegal[i] = history.isLegal(board,loc,nextPlayer);

    float policyValue;
    if(isLegal[i]) {
      legalCount += 1;
      policyValue = policy[i];
    }
    else {
      policyValue = -1e30f;
      policy[i] = policyValue;
    }

    if(policyValue > maxPolicy)
      maxPolicy = policyValue;
  }

  assert(legalCount > 0);

  float policySum = 0.0f;
  for(int i = 0; i<NNPos::NN_POLICY_SIZE; i++) {
    policy[i] = exp(policy[i] - maxPolicy);
    policySum += policy[i];
  }

  //Somehow all legal moves rounded to 0 probability
  //TODO maybe log how many times probabilities all round to 0 or log some error?
  if(policySum <= 0.0) {
    float uniform = 1.0f / legalCount;
    for(int i = 0; i<NNPos::NN_POLICY_SIZE; i++) {
      policy[i] = isLegal[i] ? uniform : -1.0f;
    }
  }
  else {
    for(int i = 0; i<NNPos::NN_POLICY_SIZE; i++)
      policy[i] = isLegal[i] ? (policy[i] / policySum) : -1.0f;
  }

  if(nextPlayer == P_WHITE)
    buf.result->whiteValue = tanh(buf.result->whiteValue);
  else
    buf.result->whiteValue = -tanh(buf.result->whiteValue);

}

