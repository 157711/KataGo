#include "core/global.h"
#include "core/rand.h"
#include "fastboard.h"
#include "sgf.h"
#include "datapool.h"

#include <H5Cpp.h>
using namespace H5;

#define TCLAP_NAMESTARTSTRING "-" //Use single dashes for all flags
#include <tclap/CmdLine.h>

//Data and feature row parameters
static const int maxBoardSize = 19;
static const int numFeatures = 26;

//Different segments of the data row
static const int inputStart = 0;
static const int inputLen = maxBoardSize * maxBoardSize * numFeatures;

static const int chainStart = inputStart + inputLen;
static const int chainLen = 0;
// static const int chainLen = maxBoardSize * maxBoardSize + 1;

static const int targetStart = chainStart + chainLen;
static const int targetLen = maxBoardSize * maxBoardSize;

static const int ladderTargetStart = targetStart + targetLen;
static const int ladderTargetLen = 0;
// static const int ladderTargetLen = maxBoardSize * maxBoardSize;

static const int targetWeightsStart = ladderTargetStart + ladderTargetLen;
static const int targetWeightsLen = 1;

static const int totalRowLen = targetWeightsStart + targetWeightsLen;

//HDF5 parameters
static const int chunkHeight = 2000;
static const int deflateLevel = 6;
static const int h5Dimension = 2;

static int xyToTensorPos(int x, int y, int offset) {
  return (y+offset) * maxBoardSize + (x+offset);
}
static int locToTensorPos(Loc loc, int bSize, int offset) {
  return (Location::getY(loc,bSize) + offset) * maxBoardSize + (Location::getX(loc,bSize) + offset);
}

static void setRow(float* row, int pos, int feature, float value) {
  row[pos*numFeatures + feature] = value;
}

static const int TARGET_NEXT_MOVE_AND_LADDER = 0;

//Calls f on each location that is part of an inescapable atari, or a group that can be put into inescapable atari
static void iterLadders(const FastBoard& board, std::function<void(Loc,int,const vector<Loc>&)> f) {
  int bSize = board.x_size;
  int offset = (maxBoardSize - bSize) / 2;

  Loc chainHeadsSolved[bSize*bSize];
  bool chainHeadsSolvedValue[bSize*bSize];
  int numChainHeadsSolved = 0;
  FastBoard copy(board);
  vector<Loc> buf;
  vector<Loc> workingMoves;

  for(int y = 0; y<bSize; y++) {
    for(int x = 0; x<bSize; x++) {
      int pos = xyToTensorPos(x,y,offset);
      Loc loc = Location::getLoc(x,y,bSize);
      Color stone = board.colors[loc];
      if(stone == P_BLACK || stone == P_WHITE) {
        int libs = board.getNumLiberties(loc);
        if(libs == 1 || libs == 2) {
          bool alreadySolved = false;
          Loc head = board.chain_head[loc];
          for(int i = 0; i<numChainHeadsSolved; i++) {
            if(chainHeadsSolved[i] == head) {
              alreadySolved = true;
              if(chainHeadsSolvedValue[i]) {
                workingMoves.clear();
                f(loc,pos,workingMoves);
              }
              break;
            }
          }
          if(!alreadySolved) {
            //Perform search on copy so as not to mess up tracking of solved heads
            bool laddered;
            if(libs == 1)
              laddered = copy.searchIsLadderCaptured(loc,true,buf);
            else {
              workingMoves.clear();
              laddered = copy.searchIsLadderCapturedAttackerFirst2Libs(loc,buf,workingMoves);
            }

            chainHeadsSolved[numChainHeadsSolved] = head;
            chainHeadsSolvedValue[numChainHeadsSolved] = laddered;
            numChainHeadsSolved++;
            if(laddered)
              f(loc,pos,workingMoves);
          }
        }
      }
    }
  }
}

// //Calls f on each location that is part of an inescapable atari, or a group that can be put into inescapable atari
// static void iterWouldBeLadder(const FastBoard& board, Player pla, std::function<void(Loc,int)> f) {
//   Player opp = getEnemy(pla);
//   int bSize = board.x_size;
//   int offset = (maxBoardSize - bSize) / 2;

//   FastBoard copy(board);
//   vector<Loc> buf;

//   for(int y = 0; y<bSize; y++) {
//     for(int x = 0; x<bSize; x++) {
//       int pos = xyToTensorPos(x,y,offset);
//       Loc loc = Location::getLoc(x,y,bSize);
//       Color stone = board.colors[loc];
//       if(stone == C_EMPTY && board.getNumLibertiesAfterPlay(loc,pla,3) == 2) {

//       }
//     }
//   }
// }

static void fillRow(const FastBoard& board, const vector<Move>& moves, int nextMoveIdx, int target, float* row, Rand& rand) {
  assert(board.x_size == board.y_size);
  assert(nextMoveIdx < moves.size());

  Player pla = moves[nextMoveIdx].pla;
  Player opp = getEnemy(pla);
  int bSize = board.x_size;
  int offset = (maxBoardSize - bSize) / 2;

  // int nextChainLabel = 1;
  // int chainLabelsByHeadLoc[FastBoard::MAX_ARR_SIZE];
  // for(int i = 0; i<FastBoard::MAX_ARR_SIZE; i++)
  //   chainLabelsByHeadLoc[i] = 0;

  for(int y = 0; y<bSize; y++) {
    for(int x = 0; x<bSize; x++) {
      int pos = xyToTensorPos(x,y,offset);
      Loc loc = Location::getLoc(x,y,bSize);

      //Feature 0 - on board
      setRow(row,pos,0, 1.0);

      Color stone = board.colors[loc];

      //Features 1,2 - pla,opp stone
      //Features 3,4,5,6 and 7,8,9,10 - pla 1,2,3,4 libs and opp 1,2,3,4 libs.
      if(stone == pla) {
        setRow(row,pos,1, 1.0);
        int libs = board.getNumLiberties(loc);
        if(libs == 1) setRow(row,pos,3, 1.0);
        else if(libs == 2) setRow(row,pos,4, 1.0);
        else if(libs == 3) setRow(row,pos,5, 1.0);
        else if(libs == 4) setRow(row,pos,6, 1.0);
      }
      else if(stone == opp) {
        setRow(row,pos,2, 1.0);
        int libs = board.getNumLiberties(loc);
        if(libs == 1) setRow(row,pos,7, 1.0);
        else if(libs == 2) setRow(row,pos,8, 1.0);
        else if(libs == 3) setRow(row,pos,9, 1.0);
        else if(libs == 4) setRow(row,pos,10, 1.0);
      }

      if(stone == pla || stone == opp) {
        //Fill chain feature
        // Loc headLoc = board.chain_head[loc];
        // if(chainLabelsByHeadLoc[headLoc] == 0)
        //   chainLabelsByHeadLoc[headLoc] = (nextChainLabel++);

        // row[chainStart + pos] = chainLabelsByHeadLoc[headLoc];
      }
      else {
        //Feature 11,12,13 - 1, 2, 3 liberties after own play.
        //Feature 14,15,16 - 1, 2, 3 liberties after opponent play
        int plaLibAfterPlay = board.getNumLibertiesAfterPlay(loc,pla,4);
        int oppLibAfterPlay = board.getNumLibertiesAfterPlay(loc,opp,4);
        if(plaLibAfterPlay == 1)      setRow(row,pos,11, 1.0);
        else if(plaLibAfterPlay == 2) setRow(row,pos,12, 1.0);
        else if(plaLibAfterPlay == 3) setRow(row,pos,13, 1.0);

        if(oppLibAfterPlay == 1)      setRow(row,pos,14, 1.0);
        else if(oppLibAfterPlay == 2) setRow(row,pos,15, 1.0);
        else if(oppLibAfterPlay == 3) setRow(row,pos,16, 1.0);
      }
    }
  }

  //Last chain entry is the number of chain segments
  // row[chainStart + maxBoardSize * maxBoardSize] = nextChainLabel;

  //Feature 17 - simple ko location
  if(board.ko_loc != FastBoard::NULL_LOC) {
    int pos = locToTensorPos(board.ko_loc,bSize,offset);
    setRow(row,pos,17, 1.0);
  }

  //Probabilistically include prev move features
  //Features 18,19,20,21,22
  bool includePrev1 = rand.nextDouble() < 0.9;
  bool includePrev2 = includePrev1 && rand.nextDouble() < 0.95;
  bool includePrev3 = includePrev2 && rand.nextDouble() < 0.95;
  bool includePrev4 = includePrev3 && rand.nextDouble() < 0.98;
  bool includePrev5 = includePrev4 && rand.nextDouble() < 0.98;

  if(nextMoveIdx >= 1 && moves[nextMoveIdx-1].pla == opp && includePrev1) {
    Loc prev1Loc = moves[nextMoveIdx-1].loc;
    if(prev1Loc != FastBoard::PASS_LOC) {
      int pos = locToTensorPos(prev1Loc,bSize,offset);
      setRow(row,pos,18, 1.0);
    }
    if(nextMoveIdx >= 2 && moves[nextMoveIdx-2].pla == pla && includePrev2) {
      Loc prev2Loc = moves[nextMoveIdx-2].loc;
      if(prev2Loc != FastBoard::PASS_LOC) {
        int pos = locToTensorPos(prev2Loc,bSize,offset);
        setRow(row,pos,19, 1.0);
      }
      if(nextMoveIdx >= 3 && moves[nextMoveIdx-3].pla == opp && includePrev3) {
        Loc prev3Loc = moves[nextMoveIdx-3].loc;
        if(prev3Loc != FastBoard::PASS_LOC) {
          int pos = locToTensorPos(prev3Loc,bSize,offset);
          setRow(row,pos,20, 1.0);
        }
        if(nextMoveIdx >= 4 && moves[nextMoveIdx-4].pla == pla && includePrev4) {
          Loc prev4Loc = moves[nextMoveIdx-4].loc;
          if(prev4Loc != FastBoard::PASS_LOC) {
            int pos = locToTensorPos(prev4Loc,bSize,offset);
            setRow(row,pos,21, 1.0);
          }
          if(nextMoveIdx >= 5 && moves[nextMoveIdx-5].pla == opp && includePrev5) {
            Loc prev5Loc = moves[nextMoveIdx-5].loc;
            if(prev5Loc != FastBoard::PASS_LOC) {
              int pos = locToTensorPos(prev5Loc,bSize,offset);
              setRow(row,pos,22, 1.0);
            }
          }
        }
      }
    }
  }

  //Ladder features 23,24,25
  auto addLadderFeature = [&board,bSize,offset,row](Loc loc, int pos, const vector<Loc>& workingMoves){
    assert(board.colors[loc] == P_BLACK || board.colors[loc] == P_WHITE);
    int libs = board.getNumLiberties(loc);
    if(libs == 1)
      setRow(row,pos,23,1.0);
    else {
      setRow(row,pos,24,1.0);
      for(size_t j = 0; j < workingMoves.size(); j++) {
        int workingPos = locToTensorPos(workingMoves[j],bSize,offset);
        setRow(row,workingPos,25,1.0);
      }
    }
  };
  iterLadders(board, addLadderFeature);


  if(target == TARGET_NEXT_MOVE_AND_LADDER) {
    //Next move target
    Loc nextMoveLoc = moves[nextMoveIdx].loc;
    assert(nextMoveLoc != FastBoard::PASS_LOC);
    int nextMovePos = locToTensorPos(nextMoveLoc,bSize,offset);
    row[targetStart + nextMovePos] = 1.0;

    //Ladder target
    // auto addLadderTarget = [&board,row](Loc loc, int pos, const vector<Loc>& workingMoves){
    //   (void)workingMoves;
    //   assert(board.colors[loc] == P_BLACK || board.colors[loc] == P_WHITE);
    //   row[ladderTargetStart + pos] = 1.0;
    // };
    // iterLadders(board, addLadderTarget);
  }

  //Weight of the row, currently always 1.0
  row[targetWeightsStart] = 1.0;
}

//SGF sources
int SOURCE_GOGOD = 0;
int SOURCE_KGS = 1;
static int parseSource(const Sgf* sgf) {
  if(sgf->fileName.find("GoGoD") != string::npos)
    return SOURCE_GOGOD;
  else if(sgf->fileName.find("kgs-19") != string::npos || sgf->fileName.find("KGS2") != string::npos)
    return SOURCE_KGS;
  else
    throw IOError("Unknown source for sgf: " + sgf->fileName);
}

static int parseHandicap(const string& handicap) {
  int h;
  bool suc = Global::tryStringToInt(handicap,h);
  if(!suc)
    throw IOError("Unknown handicap: " + handicap);
  return h;
}

int RANK_UNRANKED = -1000;

//1 dan = 0, higher is stronger, pros are assumed to be 9d.
static int parseRank(const string& rank) {
  string r = Global::toLower(rank);
  if(r.length() < 2 || r.length() > 3)
    throw IOError("Could not parse rank: " + rank);

  int n = 0;
  bool isK = false;
  bool isD = false;
  bool isP = false;
  if(r.length() == 2) {
    if(r[1] != 'k' && r[1] != 'd' && r[1] != 'p')
      throw IOError("Could not parse rank: " + rank);
    if(!Global::isDigits(r,0,1))
      throw IOError("Could not parse rank: " + rank);
    n = Global::parseDigits(r,0,1);
    isK = r[1] == 'k';
    isD = r[1] == 'd';
    isP = r[1] == 'p';
  }

  else if(r.length() == 3) {
    if(r[2] != 'k' && r[2] != 'd' && r[2] != 'p')
      throw IOError("Could not parse rank: " + rank);
    if(!Global::isDigits(r,0,2))
      throw IOError("Could not parse rank: " + rank);
    n = Global::parseDigits(r,0,2);
    isK = r[2] == 'k';
    isD = r[2] == 'd';
    isP = r[2] == 'p';
  }
  else {
    assert(false);
  }

  if(isK)
    return -n;
  else if(isD)
    return n >= 9 ? 8 : n-1;
  else if(isP)
    return 8;
  else {
    assert(false);
    return 0;
  }
}

struct Stats {
  size_t count;
  map<int,int64_t> countBySource;
  map<int,int64_t> countByRank;
  map<int,int64_t> countByOppRank;
  map<string,int64_t> countByUser;
  map<int,int64_t> countByHandicap;

  Stats()
    :count(),countBySource(),countByRank(),countByOppRank(),countByUser(),countByHandicap() {

  }

  void print() {
    cout << "Count: " << count << endl;
    cout << "Sources:" << endl;
    for(auto const& kv: countBySource) {
      cout << kv.first << " " << kv.second << endl;
    }
    cout << "Ranks:" << endl;
    for(auto const& kv: countByRank) {
      cout << kv.first << " " << kv.second << endl;
    }
    cout << "OppRanks:" << endl;
    for(auto const& kv: countByOppRank) {
      cout << kv.first << " " << kv.second << endl;
    }
    cout << "Handicap:" << endl;
    for(auto const& kv: countByHandicap) {
      cout << kv.first << " " << kv.second << endl;
    }
    cout << "Major Users:" << endl;
    for(auto const& kv: countByUser) {
      if(kv.second > count / 2000)
        cout << kv.first << " " << kv.second << endl;
    }
  }
};

static void iterSgfMoves(
  Sgf* sgf,
  //board,source,rank,oppRank,user,handicap,moves,index within moves
  std::function<void(const FastBoard&,int,int,int,const string&,int,const vector<Move>&,int)> f
) {
  int bSize;
  int source;
  int wRank;
  int bRank;
  string wUser;
  string bUser;
  int handicap;
  vector<Move> placementsBuf;
  vector<Move> movesBuf;
  try {
    bSize = sgf->getBSize();

    assert(sgf->nodes.size() > 0);
    SgfNode* root = sgf->nodes[0];

    source = parseSource(sgf);
    if(source == SOURCE_GOGOD) {
      wRank = 8;
      bRank = 8;
    }
    else {
      if(contains(root->props,"WR"))
        wRank = parseRank(root->getSingleProperty("WR"));
      else
        wRank = RANK_UNRANKED;

      if(contains(root->props,"BR"))
        bRank = parseRank(root->getSingleProperty("BR"));
      else
        bRank = RANK_UNRANKED;
    }

    wUser = root->getSingleProperty("PW");
    bUser = root->getSingleProperty("PB");

    handicap = 0;
    if(contains(root->props,"HA"))
      handicap = parseHandicap(root->getSingleProperty("HA"));

    //Apply some filters
    if(bSize != 19)
      return;

    sgf->getPlacements(placementsBuf,bSize);
    sgf->getMoves(movesBuf,bSize);
  }
  catch(const IOError &e) {
    cout << "Skipping sgf file: " << sgf->fileName << ": " << e.message << endl;
    return;
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
      break;
    }

    int rank = m.pla == P_WHITE ? wRank : bRank;
    int oppRank = m.pla == P_WHITE ? bRank : wRank;
    const string& user = m.pla == P_WHITE ? wUser : bUser;
    f(board,source,rank,oppRank,user,handicap,movesBuf,j);

    bool suc = board.playMove(m.loc,m.pla);
    if(!suc) {
      cout << sgf->fileName << endl;
      cout << ("Illegal move! " + Global::intToString(j)) << endl;
      cout << board << endl;
      break;
    }

    prevPla = m.pla;
  }

  return;
}

static void iterSgfsMoves(
  vector<Sgf*> sgfs,
  uint64_t trainTestSeed, bool isTest, double testGameProb,
  uint64_t shardSeed, int numShards,
  const size_t& numMovesUsed, const size_t& curDataSetRow,
  //source,rank,user,handicap,moves,index within moves,
  std::function<void(const FastBoard&,int,int,int,const string&,int,const vector<Move>&,int)> f
) {

  size_t numMovesItered = 0;
  size_t numMovesIteredOrSkipped = 0;

  for(int shard = 0; shard < numShards; shard++) {
    Rand trainTestRand(trainTestSeed);
    Rand shardRand(shardSeed);

    std::function<void(const FastBoard&,int,int,int,const string&,int,const vector<Move>&,int)> g =
      [f,shard,numShards,&shardRand,&numMovesIteredOrSkipped,&numMovesItered](
        const FastBoard& board, int source, int rank, int oppRank, const string& user, int handicap, const vector<Move>& moves, int moveIdx
      ) {
      //Only use this move if it's within our shard.
      numMovesIteredOrSkipped++;
      if(numShards <= 1 || shard == shardRand.nextUInt(numShards)) {
        numMovesItered++;
        f(board,source,rank,oppRank,user,handicap,moves,moveIdx);
      }
    };

    for(int i = 0; i<sgfs.size(); i++) {
      if(i % 1000 == 0)
        cout << "Shard " << shard << " "
             << "processed " << i << "/" << sgfs.size() << " sgfs, "
             << "itered " << numMovesItered << " moves, "
             << "used " << numMovesUsed << " moves, "
             << "written " << curDataSetRow << " rows..." << endl;

      bool gameIsTest = trainTestRand.nextDouble() < testGameProb;
      if(isTest == gameIsTest)
        iterSgfMoves(sgfs[i],g);
    }
  }

  assert(numMovesIteredOrSkipped == numMovesItered * numShards);
  cout << "Over all shards, numMovesItered = " << numMovesItered
       << " numMovesIteredOrSkipped = " << numMovesIteredOrSkipped
       << " numMovesItered*numShards = " << (numMovesItered * numShards) << endl;
}


static void maybeUseRow(
  const FastBoard& board, int source, int rank, int oppRank, const string& user, int handicap, const vector<Move>& movesBuf, int moveIdx,
  DataPool& dataPool,
  Rand& rand, double keepProb, int minRank, int minOppRank, int maxHandicap, int target,
  set<Hash>& posHashes, Stats& total, Stats& used
) {
  //For now, only generate training rows for non-passes
  //Also only use moves by this player if that player meets rank threshold
  if(movesBuf[moveIdx].loc != FastBoard::PASS_LOC && rank >= minRank && oppRank >= minOppRank && handicap <= maxHandicap) {
    float* newRow = NULL;
    if(keepProb >= 1.0 || (rand.nextDouble() < keepProb))
      newRow = dataPool.addNewRow(rand);

    if(newRow != NULL) {
      fillRow(board,movesBuf,moveIdx,target,newRow,rand);
      posHashes.insert(board.pos_hash);

      used.count += 1;
      used.countBySource[source] += 1;
      used.countByRank[rank] += 1;
      used.countByOppRank[oppRank] += 1;
      used.countByUser[user] += 1;
      used.countByHandicap[handicap] += 1;
    }
  }

  total.count += 1;
  total.countBySource[source] += 1;
  total.countByRank[rank] += 1;
  total.countByOppRank[oppRank] += 1;
  total.countByUser[user] += 1;
  total.countByHandicap[handicap] += 1;
}

static void processSgfs(
  vector<Sgf*> sgfs, DataSet* dataSet,
  size_t poolSize,
  uint64_t trainTestSeed, bool isTest, double testGameProb,
  uint64_t shardSeed, int numShards,
  Rand& rand, double keepProb,
  int minRank, int minOppRank, int maxHandicap, int target,
  set<Hash>& posHashes, Stats& total, Stats& used
) {
  size_t curDataSetRow = 0;
  std::function<void(const float*,size_t)> writeRow = [&curDataSetRow,&dataSet](const float* rows, size_t numRows) {
    hsize_t newDims[h5Dimension] = {curDataSetRow+numRows,totalRowLen};
    dataSet->extend(newDims);
    DataSpace fileSpace = dataSet->getSpace();
    hsize_t memDims[h5Dimension] = {numRows,totalRowLen};
    DataSpace memSpace(h5Dimension,memDims);
    hsize_t start[h5Dimension] = {curDataSetRow,0};
    hsize_t count[h5Dimension] = {numRows,totalRowLen};
    fileSpace.selectHyperslab(H5S_SELECT_SET, count, start);
    dataSet->write(rows, PredType::NATIVE_FLOAT, memSpace, fileSpace);
    curDataSetRow += numRows;
  };

  DataPool dataPool(totalRowLen,poolSize,chunkHeight,writeRow);

  std::function<void(const FastBoard&,int,int,int,const string&,int,const vector<Move>&,int)> f =
    [&dataPool,&rand,keepProb,minRank,minOppRank,maxHandicap,target,&posHashes,&total,&used](
      const FastBoard& board, int source, int rank, int oppRank, const string& user, int handicap, const vector<Move>& moves, int moveIdx
    ) {
    maybeUseRow(
      board,source,rank,oppRank,user,handicap,moves,moveIdx,
      dataPool,rand,keepProb,minRank,minOppRank,maxHandicap,target,
      posHashes,total,used
    );
  };

  iterSgfsMoves(
    sgfs,
    trainTestSeed,isTest,testGameProb,
    shardSeed,numShards,
    used.count,curDataSetRow,
    f
  );

  cout << "Emptying pool" << endl;
  dataPool.finishAndWritePool(rand);
}



int main(int argc, const char* argv[]) {
  assert(sizeof(size_t) == 8);
  FastBoard::initHash();

//   string s =
// ". . . . . O O O O . . . . . . O O X ."
// ". . . . X X O X O O . . . . . O X . X"
// ". . . X X O O X X . O O . X . O X X ."
// ". . X X . X X . . O . X O . . O X . X"
// ". X O O O X . X O . . O . O . O X . O"
// "X X X O O X . X O . X X O X O O X . O"
// ". X O O O X . X . O . . X . O X X X ."
// "X O O . O X O X X O . X X O . O . . ."
// ". X X O . O X X O X X . . X . O X X X"
// "X . X O O O O O O X . . . . . O O O ."
// ". X O O O X . O X X . . . . X X O . ."
// ". X O X . X . O O X X . X X . X O . ."
// "X . X . X . . O X X O O O O X X . . ."
// "X X O X X X . O . X X O . . O . X X ."
// "X O O X . O O . X . . X O . O O X O ."
// "O . O X O . O . X O . X O . O * O O ."
// ". O O X . O O X X X X O O O X O O . ."
// ". O X X X O O X O O O O O . . . . . ."
// ". O . . . O X X . . . . . . . . . . ."
// ;

//   FastBoard testBoard(19);

//   int next = -1;
//   for(int y = 0; y<19; y++) {
//     for(int x = 0; x < 19; x++) {
//       next += 1;
//       while(s[next] != '.' && s[next] != '*' && s[next] != 'O' && s[next] != 'X')
//         next += 1;
//       if(s[next] == 'O')
//         testBoard.setStone(Location::getLoc(x,y,19),P_WHITE);
//       if(s[next] == 'X')
//         testBoard.setStone(Location::getLoc(x,y,19),P_BLACK);
//     }
//   }

//   cout << testBoard << endl;
//   FastBoard testCopy(testBoard);
//   vector<Loc> buf;
//   cout << testCopy << endl;
//   cout << testCopy.searchIsLadderCaptured(Location::getLoc(11,4,19),true,buf) << endl;
//   cout << testCopy.searchIsLadderCaptured(Location::getLoc(6,7,19),true,buf) << endl;
//   return 0;

  cout << "Command: ";
  for(int i = 0; i<argc; i++)
    cout << argv[i] << " ";
  cout << endl;

  vector<string> gamesDirs;
  string outputFile;
  size_t poolSize;
  int trainShards;
  double testGameProb;
  double keepTestProb;
  int minRank;
  int minOppRank;
  int maxHandicap;
  int target;

  try {
    TCLAP::CmdLine cmd("Sgf->HDF5 data writer", ' ', "1.0");
    TCLAP::MultiArg<string> gamesdirArg("","gamesdir","Directory of sgf files",true,"DIR");
    TCLAP::ValueArg<string> outputArg("","output","H5 file to write",true,string(),"FILE");
    TCLAP::ValueArg<size_t> poolSizeArg("","pool-size","Pool size for shuffling rows",true,(size_t)0,"SIZE");
    TCLAP::ValueArg<int>    trainShardsArg("","train-shards","Make this many passes processing 1/N of the data each time",true,0,"INT");
    TCLAP::ValueArg<double> testGameProbArg("","test-game-prob","Probability of using a game for test instead of train",true,0.0,"PROB");
    TCLAP::ValueArg<double> keepTestProbArg("","keep-test-prob","Probability per-move of keeping a move in the test set",true,0.0,"PROB");
    TCLAP::ValueArg<int>    minRankArg("","min-rank","Min rank to use a player's move",true,0,"RANK");
    TCLAP::ValueArg<int>    minOppRankArg("","min-opp-rank","Min rank of opp to use a player's move",true,0,"RANK");
    TCLAP::ValueArg<int>    maxHandicapArg("","max-handicap","Max handicap of game to use a player's move",true,0,"HCAP");
    TCLAP::ValueArg<string> targetArg("","target","nextmove",true,string(),"TARGET");
    cmd.add(gamesdirArg);
    cmd.add(outputArg);
    cmd.add(poolSizeArg);
    cmd.add(trainShardsArg);
    cmd.add(testGameProbArg);
    cmd.add(keepTestProbArg);
    cmd.add(minRankArg);
    cmd.add(minOppRankArg);
    cmd.add(maxHandicapArg);
    cmd.add(targetArg);
    cmd.parse(argc,argv);
    gamesDirs = gamesdirArg.getValue();
    outputFile = outputArg.getValue();
    poolSize = poolSizeArg.getValue();
    trainShards = trainShardsArg.getValue();
    testGameProb = testGameProbArg.getValue();
    keepTestProb = keepTestProbArg.getValue();
    minRank = minRankArg.getValue();
    minOppRank = minOppRankArg.getValue();
    maxHandicap = maxHandicapArg.getValue();

    if(targetArg.getValue() == "nextmove")
      target = TARGET_NEXT_MOVE_AND_LADDER;
    else
      throw IOError("Must specify target nextmove or... no other options right now");
  }
  catch (TCLAP::ArgException &e) {
    cerr << "Error: " << e.error() << " for argument " << e.argId() << std::endl;
    return 1;
  }

  //Print some stats-----------------------------------------------------------------
  cout << "maxBoardSize " << maxBoardSize << endl;
  cout << "numFeatures " << numFeatures << endl;
  cout << "inputLen " << inputLen << endl;
  cout << "chainLen " << chainLen << endl;
  cout << "targetLen " << targetLen << endl;
  cout << "ladderTargetLen " << ladderTargetLen << endl;
  cout << "targetWeightsLen " << targetWeightsLen << endl;
  cout << "totalRowLen " << totalRowLen << endl;
  cout << "chunkHeight " << chunkHeight << endl;
  cout << "deflateLevel " << deflateLevel << endl;
  cout << "poolSize " << poolSize << endl;
  cout << "trainShards " << trainShards << endl;
  cout << "testGameProb " << testGameProb << endl;
  cout << "keepTestProb " << keepTestProb << endl;
  cout << "minRank " << minRank << endl;
  cout << "minOppRank " << minOppRank << endl;
  cout << "maxHandicap " << maxHandicap << endl;
  cout << "target " << target << endl;

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
  H5File* h5File = new H5File(H5std_string(outputFile), H5F_ACC_TRUNC);
  hsize_t maxDims[h5Dimension] = {H5S_UNLIMITED, totalRowLen};
  hsize_t chunkDims[h5Dimension] = {chunkHeight, totalRowLen};
  hsize_t initFileDims[h5Dimension] = {0, totalRowLen};

  DSetCreatPropList dataSetProps;
  dataSetProps.setChunk(h5Dimension,chunkDims);
  dataSetProps.setDeflate(deflateLevel);

  //Process SGFS to make rows----------------------------------------------------------
  Rand rand;
  cout << "Loading SGFS..." << endl;
  vector<Sgf*> sgfs = Sgf::loadFiles(files);

  //Shuffle sgfs
  cout << "Shuffling SGFS..." << endl;
  for(int i = 1; i<sgfs.size(); i++) {
    int r = rand.nextUInt(i+1);
    Sgf* tmp = sgfs[i];
    sgfs[i] = sgfs[r];
    sgfs[r] = tmp;
  }

  uint64_t trainTestSeed = rand.nextUInt64();
  uint64_t trainShardSeed = rand.nextUInt64();
  uint64_t testShardSeed = rand.nextUInt64();

  cout << "Generating TRAINING set..." << endl;
  H5std_string trainSetName("train");
  DataSet* trainDataSet = new DataSet(h5File->createDataSet(trainSetName, PredType::IEEE_F32LE, DataSpace(h5Dimension,initFileDims,maxDims), dataSetProps));
  set<Hash> trainPosHashes;
  Stats trainTotalStats;
  Stats trainUsedStats;
  processSgfs(
    sgfs,trainDataSet,
    poolSize,
    trainTestSeed, false, testGameProb,
    trainShardSeed, trainShards,
    rand, 1.0,
    minRank, minOppRank, maxHandicap, target,
    trainPosHashes, trainTotalStats, trainUsedStats
  );
  delete trainDataSet;

  cout << "Generating TEST set..." << endl;
  H5std_string testSetName("test");
  DataSet* testDataSet = new DataSet(h5File->createDataSet(testSetName, PredType::IEEE_F32LE, DataSpace(h5Dimension,initFileDims,maxDims), dataSetProps));
  set<Hash> testPosHashes;
  Stats testTotalStats;
  Stats testUsedStats;
  processSgfs(
    sgfs,testDataSet,
    poolSize,
    trainTestSeed, true, testGameProb,
    testShardSeed, 1,
    rand, keepTestProb,
    minRank, minOppRank, maxHandicap, target,
    testPosHashes, testTotalStats, testUsedStats
  );
  delete testDataSet;

  //Close the h5 file
  delete h5File;

  cout << "Done" << endl;

  cout << "TRAIN TOTAL------------------------------------" << endl;
  cout << trainPosHashes.size() << " unique pos hashes" << endl;
  trainTotalStats.print();
  cout << "TRAIN USED------------------------------------" << endl;
  trainUsedStats.print();

  cout << "TEST TOTAL------------------------------------" << endl;
  cout << testPosHashes.size() << " unique pos hashes" << endl;
  testTotalStats.print();
  cout << "TEST USED------------------------------------" << endl;
  testUsedStats.print();

  //Cleanup----------------------------------------------------------------------------
  for(int i = 0; i<sgfs.size(); i++) {
    delete sgfs[i];
  }
  cout << "Everything cleaned up" << endl;

  return 0;
}
