#include "../core/global.h"
#include "../search/asyncbot.h"
#include "../program/play.h"
#include "../program/setup.h"

static double nextGaussianTruncated(Rand& rand, double bound) {
  double d = rand.nextGaussian();
  //Truncated refers to the probability distribution, not the sample
  //So on falling outside the range, we redraw, rather than capping.
  while(d < -bound || d > bound)
    d = rand.nextGaussian();
  return d;
}

static int getMaxExtraBlack(int bSize) {
  if(bSize <= 10)
    return 0;
  if(bSize <= 14)
    return 1;
  if(bSize <= 18)
    return 2;
  return 3;
}

static pair<int,float> chooseExtraBlackAndKomi(
  float base, float stdev, double allowIntegerProb, double handicapProb, float handicapStoneValue, double bigStdevProb, float bigStdev, int bSize, Rand& rand
) {
  int extraBlack = 0;
  float komi = base;

  if(stdev > 0.0f)
    komi += stdev * (float)nextGaussianTruncated(rand,2.0);
  if(bigStdev > 0.0f && rand.nextDouble() < bigStdevProb)
    komi += bigStdev * (float)nextGaussianTruncated(rand,2.0);

  //Adjust for bSize, so that we don't give the same massive komis on smaller boards
  komi = base + (komi - base) * (float)bSize / 19.0f;

  //Add handicap stones compensated with komi
  int maxExtraBlack = getMaxExtraBlack(bSize);
  if(maxExtraBlack > 0 && rand.nextDouble() < handicapProb) {
    extraBlack += 1+rand.nextUInt(maxExtraBlack);
    komi += extraBlack * handicapStoneValue;
  }

  //Discretize komi
  float lower;
  float upper;
  if(rand.nextDouble() < allowIntegerProb) {
    lower = floor(komi*2.0f) / 2.0f;
    upper = ceil(komi*2.0f) / 2.0f;
  }
  else {
    lower = floor(komi+ 0.5f)-0.5f;
    upper = ceil(komi+0.5f)-0.5f;
  }

  if(lower == upper)
    komi = lower;
  else {
    assert(upper > lower);
    if(rand.nextDouble() < (komi - lower) / (upper - lower))
      komi = upper;
    else
      komi = lower;
  }

  assert((float)((int)(komi * 2)) == komi * 2);
  return make_pair(extraBlack,komi);
}

//------------------------------------------------------------------------------------------------

GameInitializer::GameInitializer(ConfigParser& cfg)
  :createGameMutex(),rand()
{
  initShared(cfg);
  noResultStdev = cfg.contains("noResultStdev") ? cfg.getDouble("noResultStdev",0.0,1.0) : 0.0;
  drawRandRadius = cfg.contains("drawRandRadius") ? cfg.getDouble("drawRandRadius",0.0,1.0) : 0.0;
}

void GameInitializer::initShared(ConfigParser& cfg) {

  allowedKoRuleStrs = cfg.getStrings("koRules", Rules::koRuleStrings());
  allowedScoringRuleStrs = cfg.getStrings("scoringRules", Rules::scoringRuleStrings());
  allowedMultiStoneSuicideLegals = cfg.getBools("multiStoneSuicideLegals");

  for(size_t i = 0; i < allowedKoRuleStrs.size(); i++)
    allowedKoRules.push_back(Rules::parseKoRule(allowedKoRuleStrs[i]));
  for(size_t i = 0; i < allowedScoringRuleStrs.size(); i++)
    allowedScoringRules.push_back(Rules::parseScoringRule(allowedScoringRuleStrs[i]));

  if(allowedKoRules.size() <= 0)
    throw IOError("koRules must have at least one value in " + cfg.getFileName());
  if(allowedScoringRules.size() <= 0)
    throw IOError("scoringRules must have at least one value in " + cfg.getFileName());
  if(allowedMultiStoneSuicideLegals.size() <= 0)
    throw IOError("multiStoneSuicideLegals must have at least one value in " + cfg.getFileName());

  allowedBSizes = cfg.getInts("bSizes", 9, 19);
  allowedBSizeRelProbs = cfg.getDoubles("bSizeRelProbs",0.0,1e100);

  komiMean = cfg.getFloat("komiMean",-60.0f,60.0f);
  komiStdev = cfg.getFloat("komiStdev",0.0f,60.0f);
  komiAllowIntegerProb = cfg.getDouble("komiAllowIntegerProb",0.0,1.0);
  handicapProb = cfg.getDouble("handicapProb",0.0,1.0);
  handicapStoneValue = cfg.getFloat("handicapStoneValue",0.0f,30.0f);
  komiBigStdevProb = cfg.getDouble("komiBigStdevProb",0.0,1.0);
  komiBigStdev = cfg.getFloat("komiBigStdev",0.0f,60.0f);

  if(allowedBSizes.size() <= 0)
    throw IOError("bSizes must have at least one value in " + cfg.getFileName());
  if(allowedBSizes.size() != allowedBSizeRelProbs.size())
    throw IOError("bSizes and bSizeRelProbs must have same number of values in " + cfg.getFileName());
}

GameInitializer::~GameInitializer()
{}

void GameInitializer::createGame(Board& board, Player& pla, BoardHistory& hist, int& numExtraBlack) {
  //Multiple threads will be calling this, and we have some mutable state such as rand.
  lock_guard<std::mutex> lock(createGameMutex);
  createGameSharedUnsynchronized(board,pla,hist,numExtraBlack);
  if(noResultStdev != 0.0 || drawRandRadius != 0.0)
    throw StringError("GameInitializer::createGame called in a mode that doesn't support specifying noResultStdev or drawRandRadius");
}

void GameInitializer::createGame(Board& board, Player& pla, BoardHistory& hist, int& numExtraBlack, SearchParams& params) {
  //Multiple threads will be calling this, and we have some mutable state such as rand.
  lock_guard<std::mutex> lock(createGameMutex);
  createGameSharedUnsynchronized(board,pla,hist,numExtraBlack);

  if(noResultStdev > 1e-30) {
    double mean = params.noResultUtilityForWhite;
    params.noResultUtilityForWhite = mean + noResultStdev * nextGaussianTruncated(rand, 2.0);
    while(params.noResultUtilityForWhite < -1.0 || params.noResultUtilityForWhite > 1.0)
      params.noResultUtilityForWhite = mean + noResultStdev * nextGaussianTruncated(rand, 2.0);
  }
  if(drawRandRadius > 1e-30) {
    double mean = params.drawEquivalentWinsForWhite;
    if(mean < 0.0 || mean > 1.0)
      throw StringError("GameInitializer: params.drawEquivalentWinsForWhite not within [0,1]: " + Global::doubleToString(mean));
    params.drawEquivalentWinsForWhite = mean + drawRandRadius * (rand.nextDouble() * 2 - 1);
    while(params.drawEquivalentWinsForWhite < 0.0 || params.drawEquivalentWinsForWhite > 1.0)
      params.drawEquivalentWinsForWhite = mean + drawRandRadius * (rand.nextDouble() * 2 - 1);
  }
}

void GameInitializer::createGameSharedUnsynchronized(Board& board, Player& pla, BoardHistory& hist, int& numExtraBlack) {
  int bSize = allowedBSizes[rand.nextUInt(allowedBSizeRelProbs.data(),allowedBSizeRelProbs.size())];
  board = Board(bSize,bSize);

  Rules rules;
  rules.koRule = allowedKoRules[rand.nextUInt(allowedKoRules.size())];
  rules.scoringRule = allowedScoringRules[rand.nextUInt(allowedScoringRules.size())];
  rules.multiStoneSuicideLegal = allowedMultiStoneSuicideLegals[rand.nextUInt(allowedMultiStoneSuicideLegals.size())];

  pair<int,float> extraBlackAndKomi = chooseExtraBlackAndKomi(
    komiMean, komiStdev, komiAllowIntegerProb, handicapProb, handicapStoneValue,
    komiBigStdevProb, komiBigStdev, bSize, rand
  );
  rules.komi = extraBlackAndKomi.second;

  pla = P_BLACK;
  hist.clear(board,pla,rules,0);
  numExtraBlack = extraBlackAndKomi.first;
}

//----------------------------------------------------------------------------------------------------------

MatchPairer::MatchPairer(
  ConfigParser& cfg,
  int nBots,
  const vector<string>& bNames,
  const vector<NNEvaluator*>& nEvals,
  const vector<SearchParams>& bParamss,
  bool forSelfPlay,
  bool forGateKeeper
)
  :numBots(nBots),
   botNames(bNames),
   nnEvals(nEvals),
   baseParamss(bParamss),
   secondaryBots(),
   nextMatchups(),
   rand(),
   numGamesStartedSoFar(0),
   numGamesTotal(),
   logGamesEvery(),
   getMatchupMutex()
{
  assert(!(forSelfPlay && forGateKeeper));
  assert(botNames.size() == numBots);
  assert(nnEvals.size() == numBots);
  assert(baseParamss.size() == numBots);
  if(forSelfPlay) {
    assert(numBots == 1);
    numGamesTotal = cfg.getInt64("numGamesTotal",1,((int64_t)1) << 62);
  }
  else if(forGateKeeper) {
    assert(numBots == 2);
    numGamesTotal = cfg.getInt64("numGamesPerGating",1,((int64_t)1) << 24);
  }
  else {
    if(cfg.contains("secondaryBots"))
      secondaryBots = cfg.getInts("secondaryBots",0,4096);
    for(int i = 0; i<secondaryBots.size(); i++)
      assert(secondaryBots[i] >= 0 && secondaryBots[i] < numBots);
    numGamesTotal = cfg.getInt64("numGamesTotal",1,((int64_t)1) << 62);
  }

  logGamesEvery = cfg.getInt64("logGamesEvery",1,1000000);
}

MatchPairer::~MatchPairer()
{}

int MatchPairer::getNumGamesTotalToGenerate() const {
  return numGamesTotal;
}

bool MatchPairer::getMatchup(
  int64_t& gameIdx, BotSpec& botSpecB, BotSpec& botSpecW, Logger& logger
)
{
  std::lock_guard<std::mutex> lock(getMatchupMutex);

  if(numGamesStartedSoFar >= numGamesTotal)
    return false;

  gameIdx = numGamesStartedSoFar;
  numGamesStartedSoFar += 1;

  if(numGamesStartedSoFar % logGamesEvery == 0)
    logger.write("Started " + Global::int64ToString(numGamesStartedSoFar) + " games");
  int logNNEvery = logGamesEvery*100 > 1000 ? logGamesEvery*100 : 1000;
  if(numGamesStartedSoFar % logNNEvery == 0) {
    for(int i = 0; i<nnEvals.size(); i++) {
      logger.write(nnEvals[i]->getModelFileName());
      logger.write("NN rows: " + Global::int64ToString(nnEvals[i]->numRowsProcessed()));
      logger.write("NN batches: " + Global::int64ToString(nnEvals[i]->numBatchesProcessed()));
      logger.write("NN avg batch size: " + Global::doubleToString(nnEvals[i]->averageProcessedBatchSize()));
    }
  }

  pair<int,int> matchup = getMatchupPair();
  botSpecB.botIdx = matchup.first;
  botSpecB.botName = botNames[matchup.first];
  botSpecB.nnEval = nnEvals[matchup.first];
  botSpecB.baseParams = baseParamss[matchup.first];

  botSpecW.botIdx = matchup.second;
  botSpecW.botName = botNames[matchup.second];
  botSpecW.nnEval = nnEvals[matchup.second];
  botSpecW.baseParams = baseParamss[matchup.second];

  return true;
}

pair<int,int> MatchPairer::getMatchupPair() {
  if(nextMatchups.size() <= 0) {
    if(numBots == 1)
      return make_pair(0,0);
    for(int i = 0; i<numBots; i++) {
      for(int j = 0; j<numBots; j++) {
        if(i != j && !(contains(secondaryBots,i) && contains(secondaryBots,j))) {
          nextMatchups.push_back(make_pair(i,j));
        }
      }
    }
    //Shuffle
    for(int i = nextMatchups.size()-1; i >= 1; i--) {
      int j = (int)rand.nextUInt(i+1);
      pair<int,int> tmp = nextMatchups[i];
      nextMatchups[i] = nextMatchups[j];
      nextMatchups[j] = tmp;
    }
  }
  pair<int,int> matchup = nextMatchups.back();
  nextMatchups.pop_back();
  return matchup;
}

//----------------------------------------------------------------------------------------------------------


static void failIllegalMove(Search* bot, Logger& logger, Board board, Loc loc) {
  ostringstream sout;
  sout << "Bot returned null location or illegal move!?!" << "\n";
  sout << board << "\n";
  sout << bot->getRootBoard() << "\n";
  sout << "Pla: " << playerToString(bot->getRootPla()) << "\n";
  sout << "Loc: " << Location::toString(loc,bot->getRootBoard()) << "\n";
  logger.write(sout.str());
  bot->getRootBoard().checkConsistency();
  assert(false);
}

static void logSearch(Search* bot, Logger& logger, Loc loc) {
  ostringstream sout;
  Board::printBoard(sout, bot->getRootBoard(), loc, &(bot->getRootHist().moveHistory));
  sout << "\n";
  sout << "Root visits: " << bot->numRootVisits() << "\n";
  sout << "PV: ";
  bot->printPV(sout, bot->rootNode, 25);
  sout << "\n";
  sout << "Tree:\n";
  bot->printTree(sout, bot->rootNode, PrintTreeOptions().maxDepth(1).maxChildrenToShow(10));
  logger.write(sout.str());
}


//Place black handicap stones, free placement
static void playExtraBlack(Search* bot, Logger& logger, int numExtraBlack, Board& board, BoardHistory& hist) {
  SearchParams oldParams = bot->searchParams;
  SearchParams tempParams = oldParams;
  tempParams.rootNoiseEnabled = false;
  tempParams.chosenMoveSubtract = 0.0;
  tempParams.chosenMoveTemperature = 1.0;
  tempParams.numThreads = 1;
  tempParams.maxVisits = 1;

  Player pla = P_BLACK;
  bot->setPosition(pla,board,hist);
  bot->setParams(tempParams);
  bot->setRootPassLegal(false);

  for(int i = 0; i<numExtraBlack; i++) {
    Loc loc = bot->runWholeSearchAndGetMove(pla,logger,NULL);
    if(loc == Board::NULL_LOC || !bot->isLegal(loc,pla))
      failIllegalMove(bot,logger,board,loc);
    assert(hist.isLegal(board,loc,pla));
    hist.makeBoardMoveAssumeLegal(board,loc,pla,NULL);
    hist.clear(board,pla,hist.rules,0);
    bot->setPosition(pla,board,hist);
  }

  bot->setParams(oldParams);
  bot->setRootPassLegal(true);
}

static bool shouldStop(vector<std::atomic<bool>*>& stopConditions) {
  for(int j = 0; j<stopConditions.size(); j++) {
    if(stopConditions[j]->load())
      return true;
  }
  return false;
}

static Loc chooseRandomLegalMove(const Board& board, const BoardHistory& hist, Player pla, Rand& gameRand) {
  int numLegalMoves = 0;
  Loc locs[Board::MAX_ARR_SIZE];
  for(Loc loc = 0; loc < Board::MAX_ARR_SIZE; loc++) {
    if(hist.isLegal(board,loc,pla)) {
      locs[numLegalMoves] = loc;
      numLegalMoves += 1;
    }
  }
  if(numLegalMoves > 0) {
    int n = gameRand.nextUInt(numLegalMoves);
    return locs[n];
  }
  return Board::NULL_LOC;
}

static Loc chooseRandomPolicyMove(const NNOutput* nnOutput, const Board& board, const BoardHistory& hist, Player pla, Rand& gameRand, double temperature) {
  const float* policyProbs = nnOutput->policyProbs;
  int posLen = nnOutput->posLen;
  int numLegalMoves = 0;
  double relProbs[NNPos::MAX_NN_POLICY_SIZE];
  int locs[NNPos::MAX_NN_POLICY_SIZE];
  double probSum = 0.0;
  for(int pos = 0; pos<NNPos::MAX_NN_POLICY_SIZE; pos++) {
    Loc loc = NNPos::posToLoc(pos,board.x_size,board.y_size,posLen);
    if(policyProbs[pos] > 0.0 && hist.isLegal(board,loc,pla)) {
      double relProb = (temperature == 1.0) ? policyProbs[pos] : pow(policyProbs[pos],1.0/temperature);
      relProbs[numLegalMoves] = relProb;
      locs[numLegalMoves] = loc;
      numLegalMoves += 1;
      probSum += relProb;
    }
  }

  //Just in case the policy map is somehow not consistent with the board position
  if(numLegalMoves > 0 && probSum > 0.01) {
    int n = gameRand.nextUInt(relProbs,numLegalMoves);
    return locs[n];
  }
  return Board::NULL_LOC;
}

static Loc chooseRandomForkingMove(const NNOutput* nnOutput, const Board& board, const BoardHistory& hist, Player pla, Rand& gameRand) {
  double r = gameRand.nextDouble();
  //80% of the time, do a random temperature 1 policy move
  if(r < 0.8)
    return chooseRandomPolicyMove(nnOutput, board, hist, pla, gameRand, 1.0);
  //15% of the time, do a random temperature 2 policy move
  else if(r < 0.95)
    return chooseRandomPolicyMove(nnOutput, board, hist, pla, gameRand, 2.0);
  //5% of the time, do a random legal move
  else
    return chooseRandomLegalMove(board, hist, pla, gameRand);
}

static void extractPolicyTarget(vector<PolicyTargetMove>& buf, const Search* toMoveBot, vector<Loc>& locsBuf, vector<double>& playSelectionValuesBuf) {
  locsBuf.clear();
  playSelectionValuesBuf.clear();
  double scaleMaxToAtLeast = 10.0;

  bool success = toMoveBot->getPlaySelectionValues(
    locsBuf,playSelectionValuesBuf,scaleMaxToAtLeast
  );
  assert(success);

  for(int moveIdx = 0; moveIdx<locsBuf.size(); moveIdx++) {
    double value = playSelectionValuesBuf[moveIdx];
    assert(value >= 0.0 && value < 30000.0); //Make sure we don't oveflow int16
    buf.push_back(PolicyTargetMove(locsBuf[moveIdx],(int16_t)round(value)));
  }
}

static void extractValueTargets(ValueTargets& buf, const Search* toMoveBot, vector<double>* recordUtilities) {
  double winValue;
  double lossValue;
  double noResultValue;
  double scoreValue;
  bool success = toMoveBot->getRootValues(
    winValue,lossValue,noResultValue,scoreValue
  );
  assert(success);

  buf.win = winValue;
  buf.loss = lossValue;
  buf.noResult = noResultValue;
  buf.scoreValue = scoreValue;

  //Not defined, only matters for the final value targets for the game result
  buf.score = 0.0f;

  assert(recordUtilities != NULL);
  assert(recordUtilities->size() > 255);
  buf.mctsUtility1 = (float)((*recordUtilities)[0]);
  buf.mctsUtility4 = (float)((*recordUtilities)[3]);
  buf.mctsUtility16 = (float)((*recordUtilities)[15]);
  buf.mctsUtility64 = (float)((*recordUtilities)[63]);
  buf.mctsUtility256 = (float)((*recordUtilities)[255]);
}

//Run a game between two bots. It is OK if both bots are the same bot.
FinishedGameData* Play::runGame(
  const Board& initialBoard, Player pla, const BoardHistory& initialHist, int numExtraBlack,
  MatchPairer::BotSpec& botSpecB, MatchPairer::BotSpec& botSpecW,
  const string& searchRandSeed,
  bool doEndGameIfAllPassAlive, bool clearBotAfterSearch,
  Logger& logger, bool logSearchInfo, bool logMoves,
  int maxMovesPerGame, vector<std::atomic<bool>*>& stopConditions,
  bool fancyModes, bool recordFullData, int dataPosLen,
  Rand& gameRand
) {
  FinishedGameData* gameData = new FinishedGameData();

  Search* botB;
  Search* botW;
  if(botSpecB.botIdx == botSpecW.botIdx) {
    botB = new Search(botSpecB.baseParams, botSpecB.nnEval, searchRandSeed);
    botW = botB;
  }
  else {
    botB = new Search(botSpecB.baseParams, botSpecB.nnEval, searchRandSeed + "@B");
    botW = new Search(botSpecW.baseParams, botSpecW.nnEval, searchRandSeed + "@W");
  }

  Board board(initialBoard);
  BoardHistory hist(initialHist);
  if(numExtraBlack > 0)
    playExtraBlack(botB,logger,numExtraBlack,board,hist);

  vector<double>* recordUtilities = NULL;

  gameData->bName = botSpecB.botName;
  gameData->wName = botSpecW.botName;
  gameData->bIdx = botSpecB.botIdx;
  gameData->wIdx = botSpecW.botIdx;

  gameData->preStartBoard = board;
  gameData->gameHash.hash0 = gameRand.nextUInt64();
  gameData->gameHash.hash1 = gameRand.nextUInt64();

  gameData->drawEquivalentWinsForWhite = botSpecB.baseParams.drawEquivalentWinsForWhite;

  gameData->firstTrainingTurn = 0;
  gameData->mode = 0;
  gameData->modeMeta1 = 0;
  gameData->modeMeta2 = 0;

  if(recordFullData)
    recordUtilities = new vector<double>(256);

  if(fancyModes) {
    //Try playing a bunch of pure policy moves instead of playing from the start to initialize the board
    //and add entropy
    {
      NNResultBuf buf;
      NNEvaluator* nnEval = botB->nnEvaluator;

      double r = 0;
      while(r < 0.00000001)
        r = gameRand.nextDouble();
      r = -log(r);
      //This gives us about 36 moves on average for 19x19.
      int numInitialMovesToPlay = floor(r * board.x_size * board.y_size / 10.0);
      assert(numInitialMovesToPlay >= 0);
      for(int i = 0; i<numInitialMovesToPlay; i++) {
        double drawEquivalentWinsForWhite = (pla == P_BLACK ? botB : botW)->searchParams.drawEquivalentWinsForWhite;
        nnEval->evaluate(board,hist,pla,drawEquivalentWinsForWhite,buf,NULL,false,false);
        std::shared_ptr<NNOutput> nnOutput = std::move(buf.result);

        //TODO maybe check the win chance after all these policy moves and try again if too lopsided
        //Or adjust the komi?
        vector<Loc> locs;
        vector<double> playSelectionValues;
        int posLen = nnOutput->posLen;
        assert(posLen >= board.x_size);
        assert(posLen >= board.y_size);
        assert(posLen > 0 && posLen < 100);
        int policySize = NNPos::getPolicySize(posLen);
        for(int movePos = 0; movePos<policySize; movePos++) {
          Loc moveLoc = NNPos::posToLoc(movePos,board.x_size,board.y_size,posLen);
          double policyProb = nnOutput->policyProbs[movePos];
          if(!hist.isLegal(board,moveLoc,pla) || policyProb <= 0)
            continue;
          locs.push_back(moveLoc);
          playSelectionValues.push_back(policyProb);
        }

        assert(playSelectionValues.size() > 0);

        //With a tiny probability, choose a uniformly random move instead of a policy move, to also
        //add a bit more outlierish variety
        uint32_t idxChosen;
        if(gameRand.nextBool(0.0002))
          idxChosen = gameRand.nextUInt(playSelectionValues.size());
        else
          idxChosen = gameRand.nextUInt(playSelectionValues.data(),playSelectionValues.size());
        Loc loc = locs[idxChosen];

        //Make the move!
        assert(hist.isLegal(board,loc,pla));
        hist.makeBoardMoveAssumeLegal(board,loc,pla,NULL);
        pla = getOpp(pla);

        //Rarely, playing the random moves out this way will end the game
        if(doEndGameIfAllPassAlive)
          hist.endGameIfAllPassAlive(board);
        if(hist.isGameFinished)
          break;
      }
    }

    //Make sure there's some minimum tiny amount of data about how the encore phases work
    if(hist.rules.scoringRule == Rules::SCORING_TERRITORY && hist.encorePhase == 0 && gameRand.nextBool(0.02)) {
      int encorePhase = gameRand.nextInt(1,2);
      hist.clear(board,pla,hist.rules,encorePhase);

      gameData->preStartBoard = board;
      gameData->mode = 1;
      gameData->modeMeta1 = encorePhase;
      gameData->modeMeta2 = 0;
    }
  }

  //Set in the starting board and history to gameData and both bots
  gameData->startBoard = board;
  gameData->startHist = hist;
  gameData->startPla = pla;
  gameData->firstTrainingTurn = hist.moveHistory.size();

  botB->setPosition(pla,board,hist);
  if(botB != botW)
    botW->setPosition(pla,board,hist);

  vector<Loc> locsBuf;
  vector<double> playSelectionValuesBuf;

  //Main play loop
  for(int i = 0; i<maxMovesPerGame; i++) {
    if(doEndGameIfAllPassAlive)
      hist.endGameIfAllPassAlive(board);
    if(hist.isGameFinished)
      break;
    if(shouldStop(stopConditions))
      break;

    Search* toMoveBot = pla == P_BLACK ? botB : botW;
    Loc loc = toMoveBot->runWholeSearchAndGetMove(pla,logger,recordUtilities);

    if(loc == Board::NULL_LOC || !toMoveBot->isLegal(loc,pla))
      failIllegalMove(toMoveBot,logger,board,loc);
    if(logSearchInfo)
      logSearch(toMoveBot,logger,loc);
    if(logMoves)
      logger.write("Move " + Global::intToString(hist.moveHistory.size()) + " made: " + Location::toString(loc,board));

    if(recordFullData) {
      vector<PolicyTargetMove>* policyTarget = new vector<PolicyTargetMove>();
      extractPolicyTarget(*policyTarget, toMoveBot, locsBuf, playSelectionValuesBuf);
      gameData->policyTargetsByTurn.push_back(policyTarget);

      ValueTargets whiteValueTargets;
      extractValueTargets(whiteValueTargets, toMoveBot, recordUtilities);
      gameData->whiteValueTargetsByTurn.push_back(whiteValueTargets);

      //Occasionally fork off some positions to evaluate
      if(fancyModes && gameRand.nextBool(0.10)) {
        assert(toMoveBot->rootNode != NULL);
        assert(toMoveBot->rootNode->nnOutput != nullptr);
        Loc forkLoc = chooseRandomForkingMove(toMoveBot->rootNode->nnOutput.get(), board, hist, pla, gameRand);
        if(forkLoc != Board::NULL_LOC) {
          SidePosition* sp = new SidePosition(board,hist,pla);
          sp->hist.makeBoardMoveAssumeLegal(sp->board,forkLoc,sp->pla,NULL);
          sp->pla = getOpp(sp->pla);
          if(sp->hist.isGameFinished) delete sp;
          else gameData->sidePositions.push_back(sp);
        }
      }
    }

    //In many cases, we are using root-level noise, so we want to clear the search each time so that we don't
    //bias the next search with the result of the previous... and also to make each color's search independent of the other's.
    if(clearBotAfterSearch)
      toMoveBot->clearSearch();

    //Finally, make the move on the bots
    bool suc;
    suc = botB->makeMove(loc,pla);
    assert(suc);
    if(botB != botW) {
      suc = botW->makeMove(loc,pla);
      assert(suc);
    }

    //And make the move on our copy of the board
    assert(hist.isLegal(board,loc,pla));
    hist.makeBoardMoveAssumeLegal(board,loc,pla,NULL);
    pla = getOpp(pla);

  }

  gameData->endHist = hist;
  if(hist.isGameFinished)
    gameData->hitTurnLimit = false;
  else
    gameData->hitTurnLimit = true;

  if(recordFullData) {
    ValueTargets finalValueTargets;
    Color area[Board::MAX_ARR_SIZE];
    if(hist.isGameFinished && hist.isNoResult) {
      finalValueTargets.win = 0.0f;
      finalValueTargets.loss = 0.0f;
      finalValueTargets.noResult = 1.0f;
      finalValueTargets.scoreValue = 0.0f;
      finalValueTargets.score = 0.0f;

      //Fill with empty so that we use "nobody owns anything" as the training target.
      //Although in practice actually the training normally weights by having a result or not, so it doesn't matter what we fill.
      std::fill(area,area+Board::MAX_ARR_SIZE,C_EMPTY);
    }
    else {
      //Relying on this to be idempotent, so that we can get the final territory map
      //We also do want to call this here to force-end the game if we crossed a move limit.
      hist.endAndScoreGameNow(board,area);

      finalValueTargets.win = (float)NNOutput::whiteWinsOfWinner(hist.winner, gameData->drawEquivalentWinsForWhite);
      finalValueTargets.loss = 1.0f - finalValueTargets.win;
      finalValueTargets.noResult = 0.0f;
      finalValueTargets.scoreValue = NNOutput::whiteScoreValueOfScoreGridded(hist.finalWhiteMinusBlackScore, gameData->drawEquivalentWinsForWhite, board, hist);
      finalValueTargets.score = hist.finalWhiteMinusBlackScore + hist.whiteKomiAdjustmentForDraws(gameData->drawEquivalentWinsForWhite);

      //Dummy values, doesn't matter since we didn't do a search for the final values
      finalValueTargets.mctsUtility1 = 0.0f;
      finalValueTargets.mctsUtility4 = 0.0f;
      finalValueTargets.mctsUtility16 = 0.0f;
      finalValueTargets.mctsUtility64 = 0.0f;
      finalValueTargets.mctsUtility256 = 0.0f;
    }
    gameData->whiteValueTargetsByTurn.push_back(finalValueTargets);

    assert(dataPosLen > 0);
    assert(gameData->finalWhiteOwnership == NULL);
    gameData->finalWhiteOwnership = new int8_t[dataPosLen*dataPosLen];
    std::fill(gameData->finalWhiteOwnership, gameData->finalWhiteOwnership + dataPosLen*dataPosLen, 0);
    for(int y = 0; y<board.y_size; y++) {
      for(int x = 0; x<board.x_size; x++) {
        int pos = NNPos::xyToPos(x,y,dataPosLen);
        Loc loc = Location::getLoc(x,y,board.x_size);
        if(area[loc] == P_BLACK)
          gameData->finalWhiteOwnership[pos] = -1;
        else if(area[loc] == P_WHITE)
          gameData->finalWhiteOwnership[pos] = 1;
        else if(area[loc] == C_EMPTY)
          gameData->finalWhiteOwnership[pos] = 0;
        else
          assert(false);
      }
    }

    gameData->hasFullData = true;
    gameData->posLen = dataPosLen;

    //Also evaluate all the side positions as well that we queued up to be searched
    int origSize = gameData->sidePositions.size();
    NNResultBuf nnResultBuf;
    for(int i = 0; i<gameData->sidePositions.size(); i++) {
      SidePosition* sp = gameData->sidePositions[i];
      Search* toMoveBot = sp->pla == P_BLACK ? botB : botW;
      toMoveBot->setPosition(sp->pla,sp->board,sp->hist);
      Loc responseLoc = toMoveBot->runWholeSearchAndGetMove(sp->pla,logger,recordUtilities);

      extractPolicyTarget(sp->policyTarget, toMoveBot, locsBuf, playSelectionValuesBuf);
      extractValueTargets(sp->whiteValueTargets, toMoveBot, recordUtilities);

      //Occasionally continue the fork a second move, to provide some situations where the opponent has played "weird" moves not
      //on the most immediate turn, but rather the turn before.
      if(i < origSize && gameRand.nextBool(0.25)) {
        if(responseLoc == Board::NULL_LOC || !sp->hist.isLegal(sp->board,responseLoc,sp->pla))
          failIllegalMove(toMoveBot,logger,sp->board,responseLoc);
        SidePosition* sp2 = new SidePosition(sp->board,sp->hist,sp->pla);
        sp2->hist.makeBoardMoveAssumeLegal(sp2->board,responseLoc,sp2->pla,NULL);
        sp2->pla = getOpp(sp2->pla);
        if(sp2->hist.isGameFinished)
          delete sp2;
        else {
          Search* toMoveBot2 = sp2->pla == P_BLACK ? botB : botW;
          toMoveBot2->nnEvaluator->evaluate(
            sp2->board,sp2->hist,sp2->pla,toMoveBot2->searchParams.drawEquivalentWinsForWhite,
            nnResultBuf,NULL,false,false
          );
          Loc forkLoc = chooseRandomForkingMove(nnResultBuf.result.get(), sp2->board, sp2->hist, sp2->pla, gameRand);
          if(forkLoc != Board::NULL_LOC) {
            sp2->hist.makeBoardMoveAssumeLegal(sp2->board,forkLoc,sp2->pla,NULL);
            sp2->pla = getOpp(sp2->pla);
            if(sp2->hist.isGameFinished) delete sp2;
            else gameData->sidePositions.push_back(sp2);
          }
        }
      }
    }
  }

  if(recordUtilities != NULL)
    delete recordUtilities;

  if(botW != botB)
    delete botW;
  delete botB;

  return gameData;
}



GameRunner::GameRunner(ConfigParser& cfg, const string& sRandSeedBase, bool forSelfP)
  :logSearchInfo(),logMoves(),forSelfPlay(forSelfP),maxMovesPerGame(),clearBotAfterSearch(),
   searchRandSeedBase(sRandSeedBase),gameInit(NULL)
{
  logSearchInfo = cfg.getBool("logSearchInfo");
  logMoves = cfg.getBool("logMoves");
  maxMovesPerGame = cfg.getInt("maxMovesPerGame",1,1 << 30);
  clearBotAfterSearch = cfg.contains("clearBotAfterSearch") ? cfg.getBool("clearBotAfterSearch") : false;

  //Initialize object for randomizing game settings
  gameInit = new GameInitializer(cfg);
}

GameRunner::~GameRunner() {
  delete gameInit;
}

bool GameRunner::runGame(
  MatchPairer* matchPairer, Logger& logger,
  int dataPosLen,
  ThreadSafeQueue<FinishedGameData*>* finishedGameQueue,
  std::function<void(const FinishedGameData&)>* reportGame,
  vector<std::atomic<bool>*>& stopConditions
) {
  int64_t gameIdx;
  bool shouldContinue;
  MatchPairer::BotSpec botSpecB;
  MatchPairer::BotSpec botSpecW;
  shouldContinue = matchPairer->getMatchup(gameIdx, botSpecB, botSpecW, logger);

  if(!shouldContinue)
    return false;

  Board board; Player pla; BoardHistory hist; int numExtraBlack;
  if(forSelfPlay) {
    assert(botSpecB.botIdx == botSpecW.botIdx);
    SearchParams params = botSpecB.baseParams;
    gameInit->createGame(board,pla,hist,numExtraBlack,params);
    botSpecB.baseParams = params;
    botSpecW.baseParams = params;
  }
  else {
    gameInit->createGame(board,pla,hist,numExtraBlack);
  }

  bool clearBotAfterSearchThisGame = clearBotAfterSearch;
  if(botSpecB.botIdx == botSpecW.botIdx) {
    //Avoid interactions between the two bots since they're the same.
    //Also in self-play this makes sure root noise is effective on each new search
    clearBotAfterSearchThisGame = true;
  }

  string searchRandSeed = searchRandSeedBase + ":" + Global::int64ToString(gameIdx);
  Rand gameRand(searchRandSeed + ":" + "forGameRand");

  //In 2% of games, don't autoterminate the game upon all pass alive, to just provide a tiny bit of training data on positions that occur
  //as both players must wrap things up manually, because within the search we don't autoterminate games, meaning that the NN will get
  //called on positions that occur after the game would have been autoterminated.
  bool doEndGameIfAllPassAlive = forSelfPlay ? gameRand.nextBool(0.98) : true;
  //In selfplay, do special entropy-introducing initialization of the game
  bool fancyModes = forSelfPlay;
  //In selfplay, record all the policy maps and evals and such as well for training data
  bool recordFullData = forSelfPlay;
  FinishedGameData* finishedGameData = Play::runGame(
    board,pla,hist,numExtraBlack,
    botSpecB,botSpecW,
    searchRandSeed,
    doEndGameIfAllPassAlive,clearBotAfterSearchThisGame,
    logger,logSearchInfo,logMoves,
    maxMovesPerGame,stopConditions,
    fancyModes,recordFullData,dataPosLen,
    gameRand
  );

  //Make sure not to write the game if we terminated in the middle of this game!
  if(shouldStop(stopConditions)) {
    delete finishedGameData;
    return false;
  }

  if(reportGame != NULL) {
    assert(finishedGameData != NULL);
    (*reportGame)(*finishedGameData);
  }

  if(finishedGameQueue != NULL) {
    assert(finishedGameData != NULL);
    finishedGameQueue->waitPush(finishedGameData);
  }
  else{
    delete finishedGameData;
  }

  return true;
}

