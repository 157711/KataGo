
#include "../neuralnet/nneval.h"

using namespace tensorflow;

static const int BATCH_SIZE = 1;

static void checkStatus(Status status, const char* subLabel) {
  if(!status.ok())
    throw StringError("NNEvaluator initialization failed: ", string(subLabel) + status.ToString());
}

NNEvaluator::NNEvaluator(const string& pbModelFile)
{
  Status status;
  GraphDef graphDef;

  //Create session
  status = NewSession(SessionOptions(), &session);
  checkStatus(status,"creating session");

  //Read graph from file
  status = ReadBinaryProto(Env::Default(), pbModelFile, &graphDef);
  checkStatus(status,"reading graph");

  //Add graph to session
  status = session->Create(graphDef);
  checkStatus(status,"adding graph to session");

  //Set up inputs
  TensorShape inputsShape;
  TensorShape symmetriesShape;
  TensorShape isTrainingShape;
  int inputsShapeArr[3] = {BATCH_SIZE,NNPos::MAX_BOARD_AREA,NNInputs::NUM_FEATURES};
  status = TensorShapeUtils::MakeShape(inputsShapeArr,3,&inputsShape);
  checkStatus(status,"making inputs shape");
  int symmetriesShapeArr[1] = {NNInputs::NUM_SYMMETRIES};
  status = TensorShapeUtils::MakeShape(symmetriesShapeArr,1,&symmetriesShape);
  checkStatus(status,"making symmetries shape");
  int isTrainingShapeArr[0] = {};
  status = TensorShapeUtils::MakeShape(isTrainingShapeArr,0,&isTrainingShape);
  checkStatus(status,"making isTraining shape");

  Tensor inputs(DT_FLOAT,inputsShape);
  Tensor symmetries(DT_BOOL,symmetriesShape);
  Tensor isTraining(DT_BOOL,isTrainingShape);

  inputsList = {
    {"inputs",inputs},
    {"symmetries",symmetries},
    {"is_training",isTraining},
  };

  outputNames = {
    string("policy_output"),
    string("value_output")
  };
  targetNames = {};

  inputsBuffer = inputs.flat<float>().data();
  symmetriesBuffer = symmetries.flat<bool>().data();

  auto isTrainingMap = isTraining.tensor<bool, 0>();
  isTrainingMap(0) = false;
}

NNEvaluator::~NNEvaluator()
{
  //Clear these out - these are direct pointers into the inputs and symmetries tensor
  //and are invalid once inputList is cleared and those are freed
  inputsBuffer = NULL;
  symmetriesBuffer = NULL;

  //Explictly clean up tensors - their destructors should get called.
  inputsList.clear();
  outputsBuf.clear();

  session->Close();
  session = NULL;
}

shared_ptr<NNOutput> NNEvaluator::evaluate(
  Board& board, const BoardHistory& history, Player nextPlayer, float selfKomi, int symmetry
) {
  shared_ptr<NNOutput> nnOutput = std::make_shared<NNOutput>();
  outputsBuf.clear();

  int rowSize = NNPos::MAX_BOARD_AREA * NNInputs::NUM_FEATURES;
  int bufferSize = rowSize * BATCH_SIZE;

  std::fill(inputsBuffer,inputsBuffer+bufferSize,0.0f);

  //TODO send this for batching to another thread? Probably would do so by synchronizedly
  //acquiring a buffer from that thread to be filled that doesn't conflict with threads
  //filling other entries for the same batch
  //For now we just memory allocate

  int batch = 0;
  NNInputs::fillRow(
    board,history.moveHistory,(int)history.moveHistory.size(),nextPlayer,selfKomi,
    inputsBuffer+batch*rowSize
  );

  symmetriesBuffer[0] = (symmetry & 0x1) != 0;
  symmetriesBuffer[1] = (symmetry & 0x2) != 0;
  symmetriesBuffer[2] = (symmetry & 0x4) != 0;

  Status status;
  status = session->Run(inputsList, outputNames, targetNames, &outputsBuf);
  checkStatus(status,"running inference");

  assert(outputsBuf.size() == 2);
  assert(outputsBuf[0].dims() == 2);
  assert(outputsBuf[1].dims() == 1);
  assert(outputsBuf[0].dim_size(0) == BATCH_SIZE);
  assert(outputsBuf[0].dim_size(1) == NNPos::NN_POLICY_SIZE);
  assert(outputsBuf[1].dim_size(0) == BATCH_SIZE);

  auto policyMap = outputsBuf[0].matrix<float>();
  auto valueMap = outputsBuf[1].vec<float>();

  float* policy = nnOutput->policyProbs;
  float maxPolicy = -1e20f;

  for(int i = 0; i<NNPos::NN_POLICY_SIZE; i++) {
    policy[i] = policyMap(batch,i);
    if(policy[i] > maxPolicy)
      maxPolicy = policy[i];
  }

  float policySum = 0.0f;
  for(int i = 0; i<NNPos::NN_POLICY_SIZE; i++) {
    policy[i] = exp(policy[i] - maxPolicy);
    policySum += policy[i];
  }
  for(int i = 0; i<NNPos::NN_POLICY_SIZE; i++) {
    policy[i] /= policySum;
  }

  nnOutput->value = valueMap(batch);

  return nnOutput;
}
