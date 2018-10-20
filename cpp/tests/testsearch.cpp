#include "../tests/tests.h"
#include "../search/asyncbot.h"
#include "../dataio/sgf.h"
#include <algorithm>
#include <iterator>
using namespace TestCommon;

static string getSearchRandSeed() {
  static int seedCounter = 0;
  return string("testSearchSeed") + Global::intToString(seedCounter++);
}

struct TestSearchOptions {
  int numMovesInARow;
  TestSearchOptions()
    :numMovesInARow(1)
  {}
};

static void runBotOnSgf(AsyncBot* bot, const string& sgfStr, const Rules& rules, int turnNumber, double overrideKomi, TestSearchOptions opts) {
  CompactSgf* sgf = CompactSgf::parse(sgfStr);

  Board board;
  Player nextPla;
  BoardHistory hist;
  sgf->setupInitialBoardAndHist(rules, board, nextPla, hist);
  hist.rules.komi = overrideKomi;
  vector<Move>& moves = sgf->moves;

  assert(turnNumber < moves.size());
  for(size_t i = 0; i<turnNumber; i++) {
    hist.makeBoardMoveAssumeLegal(board,moves[i].loc,moves[i].pla,NULL);
    nextPla = getOpp(moves[i].pla);
  }
  bot->setPosition(nextPla,board,hist);

  Search* search = bot->getSearch();

  for(int i = 0; i<opts.numMovesInARow; i++) {
    Loc move = bot->genMoveSynchronous(nextPla);

    Board::printBoard(cout, board, Board::NULL_LOC, &(hist.moveHistory));

    cout << "Root visits: " << search->numRootVisits() << "\n";
    cout << "NN rows: " << search->nnEvaluator->numRowsProcessed() << endl;
    cout << "NN batches: " << search->nnEvaluator->numBatchesProcessed() << endl;
    cout << "NN avg batch size: " << search->nnEvaluator->averageProcessedBatchSize() << endl;
    cout << "PV: ";
    search->printPV(cout, search->rootNode, 25);
    cout << "\n";
    cout << "Tree:\n";

    PrintTreeOptions options;
    options = options.maxDepth(1);
    search->printTree(cout, search->rootNode, options);

    bot->makeMove(move, nextPla);
    hist.makeBoardMoveAssumeLegal(board,move,nextPla,NULL);
    nextPla = getOpp(nextPla);
  }

  search->nnEvaluator->clearCache();
  bot->clearSearch();
}

static NNEvaluator* startNNEval(
  const string& modelFile, Logger& logger,
  int defaultSymmetry, bool inputsUseNHWC, bool cudaUseNHWC, bool cudaUseFP16, bool debugSkipNeuralNet
) {
  int modelFileIdx = 0;
  int maxBatchSize = 16;
  int posLen = NNPos::MAX_BOARD_LEN;
  //bool inputsUseNHWC = true;
  int nnCacheSizePowerOfTwo = 16;
  //bool debugSkipNeuralNet = false;
  NNEvaluator* nnEval = new NNEvaluator(
    modelFile,
    modelFileIdx,
    maxBatchSize,
    posLen,
    inputsUseNHWC,
    nnCacheSizePowerOfTwo,
    debugSkipNeuralNet
  );
  (void)inputsUseNHWC;

  int numNNServerThreadsPerModel = 1;
  bool nnRandomize = false;
  string nnRandSeed = "runSearchTestsRandSeed";
  //int defaultSymmetry = 0;
  vector<int> cudaGpuIdxByServerThread = {0};
  //bool cudaUseFP16 = false;
  //bool cudaUseNHWC = false;

  nnEval->spawnServerThreads(
    numNNServerThreadsPerModel,
    nnRandomize,
    nnRandSeed,
    defaultSymmetry,
    logger,
    cudaGpuIdxByServerThread,
    cudaUseFP16,
    cudaUseNHWC
  );

  return nnEval;
}

static void runBasicPositions(const string& modelFile, Logger& logger)
{
  {
    NNEvaluator* nnEval = startNNEval(modelFile,logger,0,true,false,false,false);
    SearchParams params;
    params.maxVisits = 200;
    AsyncBot* bot = new AsyncBot(params, nnEval, &logger, getSearchRandSeed());
    Rules rules = Rules::getTrompTaylorish();
    TestSearchOptions opts;

    {
      string sgfStr = "(;SZ[19]FF[3]PW[An Seong-chun]WR[6d]PB[Chen Yaoye]BR[9d]DT[2016-07-02]KM[7.5]RU[Chinese]RE[B+R];B[qd];W[dc];B[pq];W[dp];B[nc];W[po];B[qo];W[qn];B[qp];W[pm];B[nq];W[qi];B[qg];W[oi];B[cn];W[ck];B[fp];W[co];B[dn];W[eo];B[cq];W[dq];B[bo];W[cp];B[bp];W[bq];B[fn];W[bm];B[bn];W[fo];B[go];W[cr];B[en];W[gn];B[ho];W[gm];B[er];W[dr];B[ek];W[di];B[in];W[gk];B[cl];W[dk];B[ej];W[dl];B[el];W[gi];B[fi];W[ch];B[gh];W[hi];B[hh];W[ii];B[eh];W[df];B[ih];W[ji];B[kg];W[fg];B[ff];W[gf];B[eg];W[ef];B[fe];W[ge];B[fd];W[gg];B[fh];W[gd];B[cg];W[dg];B[dh];W[bg];B[bh];W[cf];B[ci];W[qc];B[pc];W[mp];B[on];W[mn];B[om];W[iq];B[pn];W[ol];B[qm];W[pl];B[rn];W[gq];B[kn];W[jo];B[ko];W[jp];B[jn];W[li];B[mo];W[pb];B[rc];W[oc];B[qb];W[od];B[cg];W[pd];B[dd];W[fc];B[ec];W[eb];B[ed];W[cd];B[fb];W[gc];B[db];W[cc];B[ea];W[gb];B[cb];W[bb];B[be];W[ce];B[bf];W[bd];B[ag];W[ca];B[jc];W[qe];B[ep];W[do];B[gp];W[fr];B[qc];W[nb];B[ib];W[je];B[re];W[kd];B[ba];W[aa];B[lc];W[ha];B[ld];W[le];B[me];W[mb];B[ie];W[id];B[kc];W[if];B[lf];W[ke];B[nd];W[of];B[jh];W[qf];B[rf];W[pg];B[mh];W[mq];B[mi];W[mj];B[hl];W[kh];B[jf];W[gl];B[lo];W[np];B[nr];W[kq];B[no];W[he];B[mf];W[rg];B[kk];W[jk];B[kj];W[ki];B[kl];W[lj];B[qk];W[ml];B[pa];W[ob];B[hb];W[ga];B[op];W[mr];B[ms];W[ls];B[ns];W[lq];B[pj];W[oj];B[ng];W[qh];B[eq];W[es];B[rj];W[im];B[jj];W[ik];B[jl];W[il];B[hn];W[hm];B[nm];W[mm];B[nl];W[nk];B[sf];W[ri];B[ql];W[ok];B[qj];W[lb];B[hq];W[hr];B[hp])";

      cout << "GAME 1 ==========================================================================" << endl;
      runBotOnSgf(bot, sgfStr, rules, 20, 7.5, opts);
      runBotOnSgf(bot, sgfStr, rules, 40, 7.5, opts);
      runBotOnSgf(bot, sgfStr, rules, 61, 7.5, opts);
      runBotOnSgf(bot, sgfStr, rules, 82, 7.5, opts);
      runBotOnSgf(bot, sgfStr, rules, 103, 7.5, opts);
    }

    {
      string sgfStr = "(;SZ[19]FF[3]PW[Go Seigen]WR[9d]PB[Takagawa Shukaku]BR[8d]DT[1957-09-26]KM[0]RE[W+R];B[qd];W[dc];B[pp];W[cp];B[eq];W[oc];B[ce];W[dh];B[fe];W[gc];B[do];W[co];B[dn];W[cm];B[jq];W[qn];B[pn];W[pm];B[on];W[qq];B[qo];W[or];B[mr];W[mq];B[nr];W[oq];B[lq];W[qm];B[rp];W[rq];B[qg];W[mp];B[lp];W[mo];B[om];W[pk];B[kn];W[mm];B[ok];W[pj];B[mk];W[op];B[dm];W[cl];B[dl];W[dk];B[ek];W[ll];B[cn];W[bn];B[bo];W[bm];B[cq];W[bp];B[oj];W[ph];B[qh];W[oi];B[qi];W[pi];B[mi];W[of];B[ki];W[qc];B[rc];W[qe];B[re];W[pd];B[rd];W[de];B[df];W[cd];B[ee];W[dd];B[fg];W[hd];B[jl];W[dj];B[bf];W[fj];B[hg];W[dp];B[ep];W[jk];B[il];W[fk];B[ie];W[he];B[hf];W[gm];B[ke];W[fo];B[eo];W[in];B[ho];W[hn];B[fn];W[gn];B[go];W[io];B[ip];W[jp];B[hq];W[qf];B[rf];W[qb];B[ik];W[lr];B[id];W[kr];B[jr];W[bq];B[ib];W[hb];B[cr];W[rj];B[rb];W[kk];B[ij];W[ic];B[jc];W[jb];B[hc];W[iq];B[ir];W[ic];B[kq];W[kc];B[hc];W[nj];B[nk];W[ic];B[oe];W[jd];B[pe];W[pf];B[od];W[pc];B[md];W[mc];B[me];W[ld];B[ng];W[ri];B[rh];W[pg];B[fl];W[je];B[kg];W[be];B[cf];W[bh];B[bd];W[bc];B[ae];W[kl];B[rn];W[mj];B[lj];W[ni];B[lk];W[mh];B[li];W[mg];B[mf];W[nh];B[jf];W[qj];B[sh];W[rm];B[km];W[if];B[ig];W[dq];B[dr];W[br];B[ci];W[gi];B[ei];W[ej];B[di];W[gl];B[bi];W[cj];B[sq];W[sr];B[so];W[sp];B[fc];W[fb];B[sq];W[lo];B[rr];W[sp];B[ec];W[eb];B[sq];W[ko];B[jn];W[sp];B[nc];W[nb];B[sq];W[nd];B[jo];W[sp];B[qr];W[pq];B[sq];W[ns];B[ks];W[sp];B[bk];W[bj];B[sq];W[ol];B[nl];W[sp];B[aj];W[ck];B[sq];W[nq];B[ls];W[sp];B[gk];W[qp];B[po];W[ro];B[gj];W[eh];B[rp];W[fi];B[sq];W[pl];B[nm];W[sp];B[ch];W[ro];B[dg];W[sn];B[ne];W[er];B[fr];W[cs];B[es];W[fh];B[bb];W[cb];B[ac];W[ba];B[cc];W[el];B[fm];W[bc])";

      cout << "GAME 2 ==========================================================================" << endl;
      runBotOnSgf(bot, sgfStr, rules, 23, 0, opts);
      runBotOnSgf(bot, sgfStr, rules, 38, 0, opts);
      runBotOnSgf(bot, sgfStr, rules, 65, 0, opts);
      runBotOnSgf(bot, sgfStr, rules, 80, 0, opts);
      runBotOnSgf(bot, sgfStr, rules, 115, 0, opts);
    }

    {
      string sgfStr = "(;FF[4]GM[1]SZ[19]PB[v49-140-400v-fp16]PW[v49-140-400v-fp16-fpu25]HA[0]KM[7.5]RU[koPOSITIONALscoreAREAsui1]RE[W+0.5];B[qd];W[dp];B[cq];W[dq];B[cp];W[co];B[bo];W[bn];B[cn];W[do];B[bm];W[bp];B[an];W[bq];B[cd];W[qp];B[oq];W[pn];B[nd];W[ec];B[df];W[hc];B[jc];W[cb];B[lq];W[ch];B[cj];W[eh];B[gd];W[gc];B[fd];W[hd];B[gf];W[cl];B[dn];W[el];B[eo];W[fp];B[ej];W[bl];B[bk];W[al];B[cr];W[br];B[fi];W[gl];B[gn];W[gp];B[dk];W[dl];B[fm];W[fl];B[ho];W[iq];B[ip];W[jq];B[jp];W[hp];B[in];W[fh];B[gh];W[gg];B[gi];W[hg];B[fg];W[hf];B[eg];W[il];B[ii];W[kl];B[lo];W[jj];B[ql];W[pq];B[op];W[rm];B[ji];W[ki];B[kh];W[li];B[ij];W[gm];B[dc];W[eb];B[fn];W[jk];B[lk];W[ln];B[ll];W[km];B[mn];W[ko];B[kp];W[mo];B[lp];W[mm];B[nn];W[lm];B[on];W[nk];B[qn];W[qm];B[po];W[rn];B[pm];W[ed];B[cf];W[ni];B[rq];W[rp];B[pr];W[qq];B[qr];W[rr];B[rs];W[sr];B[lc];W[rd];B[rc];W[re];B[pd];W[rb];B[qc];W[qg];B[oj];W[ok];B[pk];W[sc];B[qb];W[bc];B[qh];W[ph];B[qi];W[og];B[kr];W[ff];B[ef];W[fe];B[bd];W[rg];B[oi];W[nh];B[pf];W[pg];B[is];W[hr];B[hs];W[gs];B[gr];W[js];B[fs];W[ir];B[ep];W[eq];B[fq];W[cs];B[er];W[dr];B[fo];W[gq];B[go];W[fr];B[ie];W[he];B[fq];W[hq];B[ib];W[bj];B[bi];W[aj];B[ai];W[ak];B[ci];W[rl];B[ee];W[ge];B[rk];W[ol];B[pl];W[mf];B[nf];W[of];B[ne];W[mg];B[qf];W[rf];B[hb];W[ad];B[jr];W[gs];B[hs];W[es];B[ds];W[fr];B[cc];W[bb];B[fq];W[es];B[ae];W[ac];B[ds];W[fr];B[lh];W[mh];B[fq];W[es];B[qo];W[ro];B[ds];W[fr];B[nj];W[mj];B[fq];W[es];B[bf];W[pi];B[pj];W[ri];B[qj];W[rh];B[gb];W[fb];B[ra];W[sb];B[le];W[me];B[md];W[rj];B[jf];W[sk];B[ks];W[fr];B[lf];W[if];B[id];W[ic];B[je];W[em];B[en];W[ck];B[ga];W[ek];B[dj];W[is];B[kq];W[gs];B[nm];W[nl];B[hk];W[im];B[jn];W[kn];B[gk];W[fk];B[db];W[da];B[fa];W[ea];B[jm];W[jl];B[ih];W[ig];B[jg];W[lg];B[kg];W[oe];B[od];W[dd];B[fj];W[ce];B[be];W[de];B[hh];W[jo];B[io];W[om];B[ap];W[aq];B[ao];W[qk];B[pn];W[am];B[bn];W[pp];B[rk];W[qe];B[pe];W[qk];B[sd];W[se];B[rk];W[qa];B[pa];W[qk];B[hl];W[hm];B[rk];W[jb];B[kb];W[qk];B[qs];W[rk];B[or];W[oh];B[ss];W[sq];B[ng];W[ik];B[hn];W[dm];B[cm];W[sd];B[qa];W[sa];B[kj];W[mk];B[ml];W[mi];B[ja];W[lj];B[dh];W[kk];B[ei];W[fc];B[bh];W[];B[cg];W[];B[no];W[];B[mp];W[];B[])";

      cout << "GAME 3 ==========================================================================" << endl;
      runBotOnSgf(bot, sgfStr, rules, 191, 7.5, opts);
      runBotOnSgf(bot, sgfStr, rules, 197, 7.5, opts);
      runBotOnSgf(bot, sgfStr, rules, 330, 7.5, opts);
      runBotOnSgf(bot, sgfStr, rules, 330, 7.0, opts);

      cout << "Jigo and drawUtility===================" << endl;
      SearchParams testParams = params;
      testParams.drawEquivalentWinsForWhite = 0.7;
      bot->setParams(testParams);
      runBotOnSgf(bot, sgfStr, rules, 330, 7.5, opts);
      runBotOnSgf(bot, sgfStr, rules, 330, 7.0, opts);
      bot->setParams(params);

      cout << "Consecutive searches playouts and visits===================" << endl;
      TestSearchOptions opts2 = opts;
      opts2.numMovesInARow = 3;
      runBotOnSgf(bot, sgfStr, rules, 85, 7.5, opts2);
      testParams = params;
      testParams.maxPlayouts = 200;
      testParams.maxVisits = 10000;
      bot->setParams(testParams);
      runBotOnSgf(bot, sgfStr, rules, 85, 7.5, opts2);
    }

    delete bot;
    delete nnEval;
  }

}

void Tests::runSearchTests(const string& modelFile) {
  cout << "Running search tests" << endl;
  string tensorflowGpuVisibleDeviceList = "";
  double tensorflowPerProcessGpuMemoryFraction = 0.3;
  NeuralNet::globalInitialize(tensorflowGpuVisibleDeviceList,tensorflowPerProcessGpuMemoryFraction);

  Logger logger;
  logger.setLogToStdout(true);
  logger.setLogTime(false);


  runBasicPositions(modelFile, logger);

  NeuralNet::globalCleanup();
}



void Tests::runAutoSearchTests() {
  cout << "Running automatic search tests" << endl;
  string tensorflowGpuVisibleDeviceList = "";
  double tensorflowPerProcessGpuMemoryFraction = 0.3;
  NeuralNet::globalInitialize(tensorflowGpuVisibleDeviceList,tensorflowPerProcessGpuMemoryFraction);

  //Placeholder, doesn't actually do anything since we have debugSkipNeuralNet = true
  string modelFile = "/dev/null";

  ostringstream out;

  Logger logger;
  logger.setLogToStdout(false);
  logger.setLogTime(false);
  logger.addOStream(out);

  NNEvaluator* nnEval = startNNEval(modelFile,logger,0,true,false,false,true);
  SearchParams params;
  params.maxVisits = 100;
  Search* search = new Search(params, nnEval, "autoSearchRandSeed");
  Rules rules = Rules::getTrompTaylorish();
  TestSearchOptions opts;

  {
    const char* name = "Basic search with debugSkipNeuralNet and chosen move randomization";
    Board board = Board::parseBoard(9,9,R"%%(
.........
.........
..x..o...
.........
..x...o..
...o.....
..o.x.x..
.........
.........
)%%");
    Player nextPla = P_BLACK;
    BoardHistory hist(board,nextPla,rules);

    search->setPosition(nextPla,board,hist);
    search->runWholeSearch(nextPla,logger,NULL);

    PrintTreeOptions options;
    options = options.maxDepth(1);
    search->printTree(out, search->rootNode, options);

    string expected = R"%%(
: T  -1.62c W  -1.62c S   0.00c V -34.38c N     100  --  J9 J2 J3 E6 F6 B9 H9
---Black(v)---
J9  : T  -5.41c W  -5.41c S   0.00c V  -4.26c P  3.92% VW  9.28% N      32  --  J2 J3 E6 F6 B9 H9
A5  : T  -0.67c W  -0.67c S   0.00c V  33.74c P 17.68% VW  8.42% N      22  --  G2 B2 B5 B6
H2  : T  -1.56c W  -1.56c S   0.00c V -22.09c P  3.83% VW  8.57% N      17  --  E1 A1 H5 J4
F8  : T  -2.47c W  -2.47c S   0.00c V -22.88c P  3.01% VW  8.68% N      11  --  A6 D3 B8 C2
G7  : T  -3.14c W  -3.14c S   0.00c V -19.88c P  2.22% VW  8.75% N       7  --  J1 E7 D1 C8
H8  : T  17.49c W  17.49c S   0.00c V  -1.26c P  3.34% VW  6.83% N       2  --  D2
J8  : T   3.44c W   3.44c S   0.00c V  -4.23c P  2.03% VW  8.08% N       2  --  F8
J7  : T  13.62c W  13.62c S   0.00c V  13.62c P  3.62% VW  7.37% N       1  --
E8  : T  31.63c W  31.63c S   0.00c V  31.63c P  2.90% VW  6.07% N       1  --
B1  : T  17.90c W  17.90c S   0.00c V  17.90c P  2.63% VW  7.05% N       1  --
D2  : T   9.85c W   9.85c S   0.00c V   9.85c P  2.58% VW  7.66% N       1  --
J3  : T   6.73c W   6.73c S   0.00c V   6.73c P  2.39% VW  7.89% N       1  --
G9  : T  42.59c W  42.59c S   0.00c V  42.59c P  2.14% VW  5.35% N       1  --

)%%";
    expect(name,out.str(),expected);
    out.str("");
    out.clear();

    auto sampleChosenMoves = [&]() {
      std::map<Loc,int> moveLocsAndCounts;
      for(int i = 0; i<10000; i++) {
        Loc loc = search->getChosenMoveLoc();
        moveLocsAndCounts[loc] += 1;
      }
      vector<pair<Loc,int>> moveLocsAndCountsSorted;
      std::copy(moveLocsAndCounts.begin(),moveLocsAndCounts.end(),std::back_inserter(moveLocsAndCountsSorted));
      std::sort(moveLocsAndCountsSorted.begin(), moveLocsAndCountsSorted.end(), [](pair<Loc,int> a, pair<Loc,int> b) { return a.second > b.second; });

      for(int i = 0; i<moveLocsAndCountsSorted.size(); i++) {
        out << Location::toString(moveLocsAndCountsSorted[i].first,board) << " " << moveLocsAndCountsSorted[i].second << endl;
      }
    };

    sampleChosenMoves();

    expected = R"%%(
J9 10000
)%%";
    expect(name,out.str(),expected);
    out.str("");
    out.clear();

    {
      //Should do nothing, since we're "early" with no moves yet.
      search->searchParams.chosenMoveTemperature = 1.0;
      search->searchParams.chosenMoveTemperatureEarly = 0.0;

      sampleChosenMoves();

      expected = R"%%(
J9 10000
)%%";
      expect(name,out.str(),expected);
      out.str("");
      out.clear();
    }

    {
      //Now should something
      search->searchParams.chosenMoveTemperature = 1.0;
      search->searchParams.chosenMoveTemperatureEarly = 1.0;

      sampleChosenMoves();

      expected = R"%%(
J9 3394
A5 2331
H2 1797
F8 1149
G7 716
J8 154
H8 138
G9 61
J3 60
E8 55
D2 53
B1 48
J7 44

)%%";
      expect(name,out.str(),expected);
      out.str("");
      out.clear();
    }

    {
      //Ugly hack to artifically fill history. Breaks all sorts of invariants, but should work to
      //make the search htink there's some history to choose an intermediate temperature
      for(int i = 0; i<16; i++)
        search->rootHistory.moveHistory.push_back(Move(Board::NULL_LOC,P_BLACK));

      search->searchParams.chosenMoveTemperature = 1.0;
      search->searchParams.chosenMoveTemperatureEarly = 0.0;
      search->searchParams.chosenMoveTemperatureHalflife = 16.0;

      sampleChosenMoves();

      expected = R"%%(
J9 5320
A5 2365
H2 1436
F8 608
G7 234
H8 17
J8 9
J7 3
J3 2
D2 2
B1 2
G9 1
E8 1
)%%";
      expect(name,out.str(),expected);
      out.str("");
      out.clear();
    }


  }

  NeuralNet::globalCleanup();
}
