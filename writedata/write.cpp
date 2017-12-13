#include "core/global.h"
#include "core/rand.h"
#include "fastboard.h"
#include "sgf.h"
#include "datapool.h"

#include <H5Cpp.h>
using namespace H5;

#define TCLAP_NAMESTARTSTRING "-" //Use single dashes for all flags
#include <tclap/CmdLine.h>

static const int maxBoardSize = 19;
static const int numFeatures = 13;
static const int inputLen = 19*19*13;
static const int targetLen = 19*19;
static const int targetWeightsLen = 1;
static const int totalRowLen = inputLen + targetLen + targetWeightsLen;

static const int chunkHeight = 10000;
static const int deflateLevel = 6;
static const int h5Dimension = 2;

static int xyToTensorPos(int x, int y, int offset) {
  return (y+offset) * maxBoardSize + (x+offset);
}
static int locToTensorPos(Loc loc, int bSize, int offset) {
  return (Location::getX(loc,bSize) + offset) * maxBoardSize + (Location::getY(loc,bSize) + offset);
}

static void setRow(float* row, int pos, int feature, float value) {
  row[pos*numFeatures + feature] = value;
}

static void fillRow(const FastBoard& board, const vector<Move>& moves, int nextMoveIdx, float* row, Rand& rand) {
  assert(board.x_size == board.y_size);
  assert(nextMoveIdx < moves.size());

  Player pla = moves[nextMoveIdx].pla;
  Player opp = getEnemy(pla);
  int bSize = board.x_size;
  int offset = (maxBoardSize - bSize) / 2;

  for(int y = 0; y<bSize; y++) {
    for(int x = 0; x<bSize; x++) {
      int pos = xyToTensorPos(x,y,offset);
      Loc loc = Location::getLoc(x,y,bSize);

      //Feature 0 - on board
      setRow(row,pos,0, 1.0);

      Color stone = board.colors[loc];

      //Features 1,2 - pla,opp stone
      //Features 3,4,5 and 6,7,8 - pla 1,2,3 libs and opp 1,2,3 libs.
      if(stone == pla) {
        setRow(row,pos,1, 1.0);
        int libs = board.getNumLiberties(pos);
        if(libs == 1) setRow(row,pos,3, 1.0);
        else if(libs == 2) setRow(row,pos,4, 1.0);
        else if(libs == 3) setRow(row,pos,5, 1.0);
      }
      else if(stone == opp) {
        setRow(row,pos,2, 1.0);
        int libs = board.getNumLiberties(pos);
        if(libs == 1) setRow(row,pos,6, 1.0);
        else if(libs == 2) setRow(row,pos,7, 1.0);
        else if(libs == 3) setRow(row,pos,8, 1.0);
      }
    }
  }

  //Probabilistically include prev move features
  //Features 9,10,11
  bool includePrev1 = rand.nextDouble() < 0.9;
  bool includePrev2 = includePrev1 && rand.nextDouble() < 0.95;
  bool includePrev3 = includePrev2 && rand.nextDouble() < 0.95;

  if(nextMoveIdx >= 1 && moves[nextMoveIdx-1].pla == opp && includePrev1) {
    Loc prev1Loc = moves[nextMoveIdx-1].loc;
    if(prev1Loc != FastBoard::PASS_LOC) {
      int pos = locToTensorPos(prev1Loc,bSize,offset);
      setRow(row,pos,9, 1.0);
    }
    if(nextMoveIdx >= 2 && moves[nextMoveIdx-2].pla == pla && includePrev2) {
      Loc prev2Loc = moves[nextMoveIdx-2].loc;
      if(prev2Loc != FastBoard::PASS_LOC) {
        int pos = locToTensorPos(prev2Loc,bSize,offset);
        setRow(row,pos,10, 1.0);
      }
      if(nextMoveIdx >= 3 && moves[nextMoveIdx-3].pla == opp && includePrev3) {
        Loc prev3Loc = moves[nextMoveIdx-3].loc;
        if(prev3Loc != FastBoard::PASS_LOC) {
          int pos = locToTensorPos(prev3Loc,bSize,offset);
          setRow(row,pos,11, 1.0);
        }
      }
    }
  }

  //Feature 12 - simple ko location
  if(board.ko_loc != FastBoard::NULL_LOC) {
    int pos = locToTensorPos(board.ko_loc,bSize,offset);
    setRow(row,pos,12, 1.0);
  }


  //Target - the move actually made
  Loc nextMoveLoc = moves[nextMoveIdx].loc;
  assert(nextMoveLoc != FastBoard::PASS_LOC);
  int nextMovePos = locToTensorPos(nextMovePos,bSize,offset);
  row[inputLen + nextMovePos] = 1.0;

  //Weight of the row, currently always 1.0
  row[inputLen + targetLen] = 1.0;
}

static void processSgf(Sgf* sgf, vector<Move>& placementsBuf, vector<Move>& movesBuf, DataPool& dataPool, Rand& rand) {
  int bSize;
  try {
    bSize = sgf->getBSize();

    //Apply some filters
    if(bSize != 19)
      return;

    sgf->getPlacements(placementsBuf,bSize);
    sgf->getMoves(movesBuf,bSize);
  }
  catch(const IOError &e) {
    cout << "Skipping sgf file: " << sgf->fileName << ": " << e.message << endl;
  }

  FastBoard board(bSize);
  for(int j = 0; j<placementsBuf.size(); j++) {
    Move m = placementsBuf[j];
    bool suc = board.setStone(m.loc,m.pla);
    if(!suc) {
      cout << sgf->fileName << endl;
      cout << ("Illegal stone placement " + Global::intToString(j)) << endl;
      cout << board << endl;
      return;
    }
  }

  //If there are multiple black moves in a row, then make them all right now.
  //Sometimes sgfs break the standard and do handicap setup in this way.
  int j = 0;
  if(movesBuf.size() > 1 && movesBuf[0].pla == P_BLACK && movesBuf[1].pla == P_BLACK) {
    for(; j<movesBuf.size(); j++) {
      Move m = movesBuf[j];
      if(m.pla != P_BLACK)
        break;
      bool suc = board.playMove(m.loc,m.pla);
      if(!suc) {
        cout << sgf->fileName << endl;
        cout << ("Illegal move! " + Global::intToString(j)) << endl;
        cout << board << endl;
      }
    }
  }

  Player prevPla = C_EMPTY;
  for(; j<movesBuf.size(); j++) {
    Move m = movesBuf[j];

    //Forbid consecutive moves by the same player
    if(m.pla == prevPla) {
      cout << sgf->fileName << endl;
      cout << ("Multiple moves in a row by same player at " + Global::intToString(j)) << endl;
      cout << board << endl;
    }

    //For now, only generate training rows for non-passes
    if(m.loc != FastBoard::PASS_LOC) {
      float* newRow = dataPool.addNewRow(rand);
      fillRow(board,movesBuf,j,newRow,rand);
    }

    bool suc = board.playMove(m.loc,m.pla);
    if(!suc) {
      cout << sgf->fileName << endl;
      cout << ("Illegal move! " + Global::intToString(j)) << endl;
      cout << board << endl;
    }

    prevPla = m.pla;
  }
}



int main(int argc, const char* argv[]) {
  assert(sizeof(size_t) == 8);
  FastBoard::initHash();

  cout << "Command: ";
  for(int i = 0; i<argc; i++)
    cout << argv[i];
  cout << endl;

  vector<string> gamesDirs;
  string outputFile;
  int trainPoolSize;
  int testSize;

  try {
    TCLAP::CmdLine cmd("Sgf->HDF5 data writer", ' ', "1.0");
    TCLAP::MultiArg<string> gamesdirArg("","gamesdir","Directory of sgf files",true,"DIR");
    TCLAP::ValueArg<string> outputArg("","output","H5 file to write",true,string(),"FILE");
    TCLAP::ValueArg<size_t> trainPoolSizeArg("","train-pool-size","Pool size for shuffling training rows",true,(size_t)0,"SIZE");
    TCLAP::ValueArg<size_t> testSizeArg("","test-size","Number of testing rows",true,(size_t)0,"SIZE");
    cmd.add(gamesdirArg);
    cmd.add(outputArg);
    cmd.add(trainPoolSizeArg);
    cmd.add(testSizeArg);
    cmd.parse(argc,argv);
    gamesDirs = gamesdirArg.getValue();
    outputFile = outputArg.getValue();
    trainPoolSize = trainPoolSizeArg.getValue();
    testSize = testSizeArg.getValue();
  }
  catch (TCLAP::ArgException &e) {
    cerr << "Error: " << e.error() << " for argument " << e.argId() << std::endl;
    return 1;
  }

  //Collect SGF files-----------------------------------------------------------------
  const string suffix = ".sgf";
  auto filter = [&suffix](const string& name) {
    return Global::isSuffix(name,suffix);
  };

  vector<string> files;
  for(int i = 0; i<gamesDirs.size(); i++)
    Global::collectFiles(gamesDirs[i], filter, files);
  cout << "Found " << files.size() << " sgf files!" << endl;

  cout << "Opening h5 file..." << endl;
  H5File* h5File = new H5File(H5std_string(outputFile), H5F_ACC_EXCL);
  hsize_t maxDims[h5Dimension] = {H5S_UNLIMITED, totalRowLen};
  hsize_t chunkDims[h5Dimension] = {chunkHeight, totalRowLen};

  hsize_t trainDims[h5Dimension] = {0, totalRowLen};
  DataSpace trainDataSpace(h5Dimension,trainDims,maxDims);

  DSetCreatPropList dataSetProps;
  // float* fillValue = new float[1];
  // fillValue[0] = 0.0;
  // dataSetProps.setFillValue(PredType::IEEE_F32LE, &fillValue);
  dataSetProps.setChunk(h5Dimension,chunkDims);
  dataSetProps.setDeflate(deflateLevel);

  H5std_string trainSetName("train");
  H5std_string testSetName("test");
  DataSet* trainDataSet = new DataSet(h5File->createDataSet(trainSetName, PredType::IEEE_F32LE, trainDataSpace, dataSetProps));

  size_t curTrainDataSetRow = 0;
  std::function<void(const float*)> writeTrainRow = [&trainDataSpace,&curTrainDataSetRow,&trainDataSet](const float* row) {
    hsize_t newDims[h5Dimension] = {curTrainDataSetRow+1,totalRowLen};
    trainDataSet->extend(newDims);
    hsize_t start[h5Dimension] = {curTrainDataSetRow,0};
    hsize_t count[h5Dimension] = {1,totalRowLen};
    trainDataSpace.selectHyperslab(H5S_SELECT_SET, count, start);
    trainDataSet->write(row, PredType::NATIVE_FLOAT, DataSpace::ALL, trainDataSpace);
    curTrainDataSetRow++;
  };

  DataPool dataPool(totalRowLen,trainPoolSize,testSize,writeTrainRow);

  //Process SGFS to make rows----------------------------------------------------------
  Rand rand;
  cout << "Processing SGFS..." << endl;

  vector<Sgf*> sgfs = Sgf::loadFiles(files);
  vector<Move> placementsBuf;
  vector<Move> movesBuf;
  for(int i = 0; i<sgfs.size(); i++) {
    if(i > 0 && i % 500 == 0)
      cout << "Processed " << i << " sgfs..." << endl;

    Sgf* sgf = sgfs[i];
    processSgf(sgf, placementsBuf, movesBuf, dataPool, rand);
  }

  //Empty out pools--------------------------------------------------------------------
  cout << "Emptying training pool" << endl;
  dataPool.finishAndWriteTrainPool(rand);
  //Close the training dataset
  delete trainDataSet;

  //Open the testing dataset
  hsize_t testDims[h5Dimension] = {0, totalRowLen};
  DataSpace testDataSpace(h5Dimension,testDims,maxDims);
  DataSet* testDataSet = new DataSet(h5File->createDataSet(testSetName, PredType::IEEE_F32LE, testDataSpace, dataSetProps));

  size_t curTestDataSetRow = 0;
  std::function<void(const float*)> writeTestRow = [&testDataSpace,&curTestDataSetRow,&testDataSet](const float* row) {
    hsize_t newDims[h5Dimension] = {curTestDataSetRow+1,totalRowLen};
    testDataSet->extend(newDims);
    hsize_t start[h5Dimension] = {curTestDataSetRow,0};
    hsize_t count[h5Dimension] = {1,totalRowLen};
    testDataSpace.selectHyperslab(H5S_SELECT_SET, count, start);
    testDataSet->write(row, PredType::NATIVE_FLOAT, DataSpace::ALL, testDataSpace);
    curTestDataSetRow++;
  };

  cout << "Writing testing set" << endl;
  dataPool.writeTestPool(writeTestRow, rand);

  //Close the testing dataset
  delete testDataSet;

  //Close the h5 file
  delete h5File;

  cout << "Done" << endl;

  //Cleanup----------------------------------------------------------------------------
  // delete[] fillValue;
  for(int i = 0; i<sgfs.size(); i++) {
    delete sgfs[i];
  }

  return 0;
}
