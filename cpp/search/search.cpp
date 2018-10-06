
#include <inttypes.h>
#include <algorithm>
#include "../search/search.h"
#include "../core/fancymath.h"
#include "../search/distributiontable.h"

NodeStats::NodeStats()
  :visits(0),winLossValueSum(0.0),scoreValueSum(0.0),valueSumWeight(0.0)
{}
NodeStats::~NodeStats()
{}

NodeStats::NodeStats(const NodeStats& other)
  :visits(other.visits),
   winLossValueSum(other.winLossValueSum),
   scoreValueSum(other.scoreValueSum),
   valueSumWeight(other.valueSumWeight)
{}
NodeStats& NodeStats::operator=(const NodeStats& other) {
  visits = other.visits;
  winLossValueSum = other.winLossValueSum;
  scoreValueSum = other.scoreValueSum;
  valueSumWeight = other.valueSumWeight;
  return *this;
}

double NodeStats::getCombinedValueSum(const SearchParams& searchParams) const {
  return (
    winLossValueSum * searchParams.winLossUtilityFactor +
    scoreValueSum * searchParams.scoreUtilityFactor
  );
}

//-----------------------------------------------------------------------------------------

SearchNode::SearchNode(Search& search, SearchThread& thread, Loc moveLoc)
  :lockIdx(),statsLock(ATOMIC_FLAG_INIT),nextPla(thread.pla),prevMoveLoc(moveLoc),
   nnOutput(),
   children(NULL),numChildren(0),childrenCapacity(0),
   stats(),virtualLosses(0)
{
  lockIdx = thread.rand.nextUInt(search.mutexPool->getNumMutexes());
}
SearchNode::~SearchNode() {
  if(children != NULL) {
    for(int i = 0; i<numChildren; i++)
      delete children[i];
  }
  delete[] children;
}

SearchNode::SearchNode(SearchNode&& other) noexcept
:lockIdx(other.lockIdx),statsLock(),
  nextPla(other.nextPla),prevMoveLoc(other.prevMoveLoc),
  nnOutput(std::move(other.nnOutput)),
  stats(other.stats),virtualLosses(other.virtualLosses)
{
  children = other.children;
  other.children = NULL;
  numChildren = other.numChildren;
  childrenCapacity = other.childrenCapacity;
}
SearchNode& SearchNode::operator=(SearchNode&& other) noexcept {
  lockIdx = other.lockIdx;
  nextPla = other.nextPla;
  prevMoveLoc = other.prevMoveLoc;
  nnOutput = std::move(other.nnOutput);
  children = other.children;
  other.children = NULL;
  numChildren = other.numChildren;
  childrenCapacity = other.childrenCapacity;
  stats = other.stats;
  virtualLosses = other.virtualLosses;
  return *this;
}

//-----------------------------------------------------------------------------------------

static string makeSeed(const Search& search, int threadIdx) {
  stringstream ss;
  ss << search.randSeed;
  ss << "$searchThread$";
  ss << threadIdx;
  ss << "$";
  ss << search.rootBoard.pos_hash;
  ss << "$";
  ss << search.rootHistory.moveHistory.size();
  ss << "$";
  ss << search.numSearchesBegun;
  return ss.str();
}

SearchThread::SearchThread(int tIdx, const Search& search, Logger* logger)
  :threadIdx(tIdx),
   pla(search.rootPla),board(search.rootBoard),
   history(search.rootHistory),
   rand(makeSeed(search,tIdx)),
   nnResultBuf(),
   logStream(NULL),
   valueChildWeightsBuf(),
   winLossValuesBuf(),
   scoreValuesBuf(),
   valuesBuf(),
   visitsBuf()
{
  if(logger != NULL)
    logStream = logger->createOStream();

  winLossValuesBuf.resize(NNPos::NN_POLICY_SIZE);
  scoreValuesBuf.resize(NNPos::NN_POLICY_SIZE);
  valuesBuf.resize(NNPos::NN_POLICY_SIZE);
  visitsBuf.resize(NNPos::NN_POLICY_SIZE);

}
SearchThread::~SearchThread() {
  if(logStream != NULL)
    delete logStream;
  logStream = NULL;
}

//-----------------------------------------------------------------------------------------

static const double VALUE_WEIGHT_DEGREES_OF_FREEDOM = 3.0;

Search::Search(SearchParams params, NNEvaluator* nnEval, const string& rSeed)
  :rootPla(P_BLACK),rootBoard(),rootHistory(),rootPassLegal(true),
   searchParams(params),numSearchesBegun(0),randSeed(rSeed),
   nnEvaluator(nnEval),
   nonSearchRand(rSeed + string("$nonSearchRand"))
{
  rootKoHashTable = new KoHashTable();

  valueWeightDistribution = new DistributionTable(
    [](double z) { return FancyMath::tdistpdf(z,VALUE_WEIGHT_DEGREES_OF_FREEDOM); },
    [](double z) { return FancyMath::tdistcdf(z,VALUE_WEIGHT_DEGREES_OF_FREEDOM); },
    -50.0,
    50.0,
    2000
  );

  rootNode = NULL;
  mutexPool = new MutexPool(params.mutexPoolSize);

  rootHistory.clear(rootBoard,rootPla,Rules());
  rootKoHashTable->recompute(rootHistory);
}

Search::~Search() {
  delete rootKoHashTable;
  delete valueWeightDistribution;
  delete rootNode;
  delete mutexPool;
}

void Search::setPosition(Player pla, const Board& board, const BoardHistory& history) {
  clearSearch();
  rootPla = pla;
  rootBoard = board;
  rootHistory = history;
  rootKoHashTable->recompute(rootHistory);
}

void Search::setPlayerAndClearHistory(Player pla) {
  clearSearch();
  rootPla = pla;
  rootBoard.clearSimpleKoLoc();
  Rules rules = rootHistory.rules;
  rootHistory.clear(rootBoard,rootPla,rules);
  rootKoHashTable->recompute(rootHistory);
}

void Search::setRulesAndClearHistory(Rules rules) {
  clearSearch();
  rootBoard.clearSimpleKoLoc();
  rootHistory.clear(rootBoard,rootPla,rules);
  rootKoHashTable->recompute(rootHistory);
}

void Search::setKomi(float newKomi) {
  if(rootHistory.rules.komi != newKomi) {
    clearSearch();
    rootHistory.setKomi(newKomi);
  }
}

void Search::setRootPassLegal(bool b) {
  clearSearch();
  rootPassLegal = b;
}

void Search::setParams(SearchParams params) {
  clearSearch();
  searchParams = params;
}

void Search::clearSearch() {
  delete rootNode;
  rootNode = NULL;
}

bool Search::isLegal(Loc moveLoc, Player movePla) const {
  //If we somehow have the same player making multiple moves in a row (possible in GTP or an sgf file),
  //clear the ko loc - the simple ko loc of a player should not prohibit the opponent playing there!
  if(movePla != rootPla) {
    Board copy = rootBoard;
    copy.clearSimpleKoLoc();
    return copy.isLegal(moveLoc,movePla,rootHistory.rules.multiStoneSuicideLegal);
  }
  else {
    //Don't require that the move is legal for the history, merely the board, so that
    //we're robust to GTP or an sgf file saying that a move was made that violates superko or things like that.
    //In the encore, we also need to ignore the simple ko loc, since the board itself will report a move as illegal
    //when actually it is a legal pass-for-ko.
    if(rootHistory.encorePhase >= 1)
      return rootBoard.isLegalIgnoringKo(moveLoc,rootPla,rootHistory.rules.multiStoneSuicideLegal);
    else
      return rootBoard.isLegal(moveLoc,rootPla,rootHistory.rules.multiStoneSuicideLegal);
  }
}

bool Search::makeMove(Loc moveLoc, Player movePla) {
  if(!isLegal(moveLoc,movePla))
    return false;

  if(movePla != rootPla)
    setPlayerAndClearHistory(movePla);

  if(rootNode != NULL) {
    bool foundChild = false;
    for(int i = 0; i<rootNode->numChildren; i++) {
      SearchNode* child = rootNode->children[i];
      if(child->prevMoveLoc == moveLoc) {
        //Grab out the node to prevent its deletion along with the root
        SearchNode* node = new SearchNode(std::move(*child));
        //Delete the root and replace it with the child
        delete rootNode;
        rootNode = node;
        rootNode->prevMoveLoc = Board::NULL_LOC;
        foundChild = true;
        break;
      }
    }
    if(!foundChild) {
      clearSearch();
    }
  }
  rootHistory.makeBoardMoveAssumeLegal(rootBoard,moveLoc,rootPla,rootKoHashTable);
  rootPla = getOpp(rootPla);
  rootKoHashTable->recompute(rootHistory);
  return true;
}

static const double POLICY_ILLEGAL_SELECTION_VALUE = -1e50;

bool Search::getPlaySelectionValues(vector<Loc>& locs, vector<double>& playSelectionValues) {
  locs.clear();
  playSelectionValues.clear();

  if(rootNode == NULL)
    return false;

  const SearchNode& node = *rootNode;
  std::mutex& mutex = mutexPool->getMutex(node.lockIdx);
  unique_lock<std::mutex> lock(mutex);

  int numChildren = node.numChildren;

  for(int i = 0; i<numChildren; i++) {
    SearchNode* child = node.children[i];
    Loc moveLoc = child->prevMoveLoc;
    double selectionValue = getPlaySelectionValue(node,child);
    assert(selectionValue >= 0.0);
    locs.push_back(moveLoc);
    playSelectionValues.push_back(selectionValue);
  }
  shared_ptr<NNOutput> nnOutput = node.nnOutput;
  lock.unlock();

  //If we have no children, then use the policy net directly
  if(numChildren == 0) {
    if(nnOutput == nullptr)
      return false;
    for(int movePos = 0; movePos<NNPos::NN_POLICY_SIZE; movePos++) {
      assert(rootBoard.x_size == rootBoard.y_size);
      int offset = NNPos::getOffset(rootBoard.x_size);
      Loc moveLoc = NNPos::posToLoc(movePos,rootBoard.x_size,offset);
      double policyProb = nnOutput->policyProbs[movePos];
      if(!rootHistory.isLegal(rootBoard,moveLoc,rootPla) || policyProb <= 0)
        continue;
      locs.push_back(moveLoc);
      playSelectionValues.push_back(policyProb);
      numChildren++;
    }
  }

  //Might happen absurdly rarely if we have a hash collision or something on the nnOutput
  if(numChildren == 0)
    return false;

  double maxValue = 0.0;
  for(int i = 0; i<numChildren; i++) {
    if(playSelectionValues[i] > maxValue)
      maxValue = playSelectionValues[i];
  }

  if(maxValue <= 1e-50)
    return false;

  double amountToSubtract = std::min(searchParams.chosenMoveSubtract, maxValue/2.0);
  for(int i = 0; i<numChildren; i++) {
    playSelectionValues[i] -= amountToSubtract;
    if(playSelectionValues[i] <= 0.0)
      playSelectionValues[i] = 0.0;
  }

  return true;
}

Loc Search::getChosenMoveLoc() {
  if(rootNode == NULL)
    return Board::NULL_LOC;

  vector<Loc> locs;
  vector<double> playSelectionValues;
  bool suc = getPlaySelectionValues(locs,playSelectionValues);
  if(!suc)
    return Board::NULL_LOC;

  assert(locs.size() == playSelectionValues.size());
  int numChildren = locs.size();

  double maxValue = 0.0;
  for(int i = 0; i<numChildren; i++) {
    if(playSelectionValues[i] > maxValue)
      maxValue = playSelectionValues[i];
  }
  assert(maxValue > 0.0);

  double temperature = searchParams.chosenMoveTemperature;
  temperature +=
    (searchParams.chosenMoveTemperatureEarly - searchParams.chosenMoveTemperature) *
    pow(0.5,rootHistory.moveHistory.size() / searchParams.chosenMoveTemperatureHalflife);

  //Temperature so close to 0 that we just calculate the max directly
  if(temperature <= 1.0e-4) {
    double bestSelectionValue = POLICY_ILLEGAL_SELECTION_VALUE;
    Loc bestChildMoveLoc = Board::NULL_LOC;
    for(int i = 0; i<numChildren; i++) {
      if(playSelectionValues[i] > bestSelectionValue) {
        bestSelectionValue = playSelectionValues[i];
        bestChildMoveLoc = locs[i];
      }
    }
    return bestChildMoveLoc;
  }
  //Actual temperature
  else {
    double sum = 0.0;
    for(int i = 0; i<numChildren; i++) {
      //Numerically stable way to raise to power and normalize
      playSelectionValues[i] = exp((log(playSelectionValues[i]) - log(maxValue)) / searchParams.chosenMoveTemperature);
      sum += playSelectionValues[i];
    }
    assert(sum > 0.0);
    uint32_t idxChosen = nonSearchRand.nextUInt(playSelectionValues.data(),playSelectionValues.size());
    return locs[idxChosen];
  }
}

void Search::beginSearch() {
  numSearchesBegun++;
  if(rootNode == NULL) {
    SearchThread dummyThread(-1, *this, NULL);
    rootNode = new SearchNode(*this, dummyThread, Board::NULL_LOC);
  }
}

uint64_t Search::numRootVisits() {
  if(rootNode == NULL)
    return 0;
  while(rootNode->statsLock.test_and_set(std::memory_order_acquire));
  uint64_t n = rootNode->stats.visits;
  rootNode->statsLock.clear(std::memory_order_release);
  return n;
}

//Assumes node is locked
void Search::maybeAddPolicyNoise(SearchThread& thread, SearchNode& node, bool isRoot) const {
  if(!isRoot || !searchParams.rootNoiseEnabled)
    return;
  //Copy nnOutput as we're about to modify its policy to add noise
  shared_ptr<NNOutput> newNNOutput = std::make_shared<NNOutput>(*(node.nnOutput));
  //Replace the old pointer
  node.nnOutput = newNNOutput;

  int legalCount = 0;
  for(int i = 0; i<NNPos::NN_POLICY_SIZE; i++) {
    if(node.nnOutput->policyProbs[i] >= 0)
      legalCount += 1;
  }

  if(legalCount <= 0)
    throw StringError("maybeAddPolicyNoise: No move with nonnegative policy value - can't even pass?");

  //Generate gamma draw on each move
  double alpha = searchParams.rootDirichletNoiseTotalConcentration / legalCount;
  double rSum = 0.0;
  double r[NNPos::NN_POLICY_SIZE];
  for(int i = 0; i<NNPos::NN_POLICY_SIZE; i++) {
    if(node.nnOutput->policyProbs[i] >= 0) {
      r[i] = thread.rand.nextGamma(alpha);
      rSum += r[i];
    }
    else
      r[i] = 0.0;
  }

  //Normalized gamma draws -> dirichlet noise
  for(int i = 0; i<NNPos::NN_POLICY_SIZE; i++)
    r[i] /= rSum;

  //At this point, r[i] contains a dirichlet distribution draw, so add it into the nnOutput.
  for(int i = 0; i<NNPos::NN_POLICY_SIZE; i++) {
    if(node.nnOutput->policyProbs[i] >= 0) {
      double weight = searchParams.rootDirichletNoiseWeight;
      node.nnOutput->policyProbs[i] = r[i] * weight + node.nnOutput->policyProbs[i] * (1.0-weight);
    }
  }
}

void Search::getValueChildWeights(
  int numChildren,
  //Unlike everywhere else where values are from white's perspective, values here are from one's own perspective
  const vector<double>& childSelfValuesBuf,
  const vector<uint64_t>& childVisitsBuf,
  vector<double>& resultBuf
) const {
  resultBuf.clear();
  if(numChildren <= 0)
    return;
  if(numChildren == 1) {
    resultBuf.push_back(1.0);
    return;
  }

  double stdevs[numChildren];
  for(int i = 0; i<numChildren; i++) {
    uint64_t numVisits = childVisitsBuf[i];
    assert(numVisits > 0);
    double precision = 1.5 * sqrt((double)numVisits);

    //Ensure some minimum variance for stability regardless of how we change the above formula
    static const double minVariance = 0.00000001;
    stdevs[i] = sqrt(minVariance + 1.0 / precision);
  }

  double simpleValueSum = 0.0;
  uint64_t numChildVisits = 0;
  for(int i = 0; i<numChildren; i++) {
    simpleValueSum += childSelfValuesBuf[i] * childVisitsBuf[i];
    numChildVisits += childVisitsBuf[i];
  }

  double simpleValue = simpleValueSum / numChildVisits;

  double weight[numChildren];
  for(int i = 0; i<numChildren; i++) {
    double z = (childSelfValuesBuf[i] - simpleValue) / stdevs[i];
    weight[i] = valueWeightDistribution->getCdf(z);
  }

  double sum = 0;
  for(int i = 0; i<numChildren; i++) {
    sum += weight[i];
  }

  //Post-process and normalize, to make sure we exactly have a probability distribution and sum exactly to 1.
  double totalWeight = 0.0;
  for(int i = 0; i<numChildren; i++) {
    double p = weight[i];
    totalWeight += p;
    resultBuf.push_back(p);
  }

  assert(totalWeight > 0);
  for(int i = 0; i<numChildren; i++) {
    resultBuf[i] /= totalWeight;
  }

}

double Search::getPlaySelectionValue(
  double nnPolicyProb, uint64_t childVisits,
  double childValue, Player pla
) const {
  if(nnPolicyProb < 0)
    return POLICY_ILLEGAL_SELECTION_VALUE;

  (void)(childValue);
  (void)(pla);
  return (double)childVisits;
}

double Search::getExploreSelectionValue(
  double nnPolicyProb, uint64_t totalChildVisits, uint64_t childVisits,
  double childValue, Player pla
) const {
  if(nnPolicyProb < 0)
    return POLICY_ILLEGAL_SELECTION_VALUE;

  double exploreComponent =
    searchParams.cpuctExploration
    * nnPolicyProb
    * sqrt((double)totalChildVisits + 0.01) //TODO this is weird when totalChildVisits == 0, first exploration
    / (1.0 + childVisits);

  //At the last moment, adjust value to be from the player's perspective, so that players prefer values in their favor
  //rather than in white's favor
  double valueComponent = pla == P_WHITE ? childValue : -childValue;
  return exploreComponent + valueComponent;
}

int Search::getPos(Loc moveLoc) const {
  int offset = NNPos::getOffset(rootBoard.x_size);
  return NNPos::locToPos(moveLoc,rootBoard.x_size,offset);
}

double Search::getPlaySelectionValue(const SearchNode& parent, const SearchNode* child) const {
  Loc moveLoc = child->prevMoveLoc;
  int movePos = getPos(moveLoc);
  float nnPolicyProb = parent.nnOutput->policyProbs[movePos];

  while(child->statsLock.test_and_set(std::memory_order_acquire));
  uint64_t childVisits = child->stats.visits;
  double childValueSum = child->stats.getCombinedValueSum(searchParams);
  double valueSumWeight = child->stats.valueSumWeight;
  child->statsLock.clear(std::memory_order_release);

  //valueSumWeight < 0 shouldn't ever happen here, since this is just used to choose the root move,
  //but just in case, add a filler value
  double childValue = valueSumWeight <= 0.0 ? parent.nnOutput->whiteValue : childValueSum / valueSumWeight;
  return getPlaySelectionValue(nnPolicyProb,childVisits,childValue,parent.nextPla);
}
double Search::getExploreSelectionValue(const SearchNode& parent, const SearchNode* child, uint64_t totalChildVisits, double fpuValue) const {
  Loc moveLoc = child->prevMoveLoc;
  int movePos = getPos(moveLoc);
  float nnPolicyProb = parent.nnOutput->policyProbs[movePos];

  while(child->statsLock.test_and_set(std::memory_order_acquire));
  uint64_t childVisits = child->stats.visits;
  double childValueSum = child->stats.getCombinedValueSum(searchParams);
  double valueSumWeight = child->stats.valueSumWeight;
  int32_t childVirtualLosses = child->virtualLosses;
  child->statsLock.clear(std::memory_order_release);

  //It's possible that childVisits is actually 0 here with multithreading because we're visiting this node while a child has
  //been expanded but its thread not yet finished its first visit
  double childValue;
  if(childVisits <= 0)
    childValue = fpuValue;
  else {
    assert(valueSumWeight > 0.0);
    childValue = childValueSum / valueSumWeight;
  }

  //When multithreading, totalChildVisits could be out of sync with childVisits, so if they provably are, then fix that up
  if(totalChildVisits < childVisits)
    totalChildVisits = childVisits;

  //Virtual losses to direct threads down different paths
  if(childVirtualLosses > 0) {
    //totalChildVisits += childVirtualLosses; //Should get better thread dispersal without this
    childVisits += childVirtualLosses;
    double virtualLossUtility = (parent.nextPla == P_WHITE ? -1.0 : 1.0) * (searchParams.winLossUtilityFactor + searchParams.scoreUtilityFactor);
    double virtualLossVisitFrac = (double)childVirtualLosses / childVisits;
    childValue = childValue + (virtualLossUtility - childValue) * virtualLossVisitFrac;
  }
  return getExploreSelectionValue(nnPolicyProb,totalChildVisits,childVisits,childValue,parent.nextPla);
}
double Search::getNewExploreSelectionValue(const SearchNode& parent, int movePos, uint64_t totalChildVisits, double fpuValue) const {
  float nnPolicyProb = parent.nnOutput->policyProbs[movePos];
  uint64_t childVisits = 0;
  double childValue = fpuValue;
  return getExploreSelectionValue(nnPolicyProb,totalChildVisits,childVisits,childValue,parent.nextPla);
}


//Assumes node is locked
void Search::selectBestChildToDescend(
  const SearchThread& thread, const SearchNode& node, int& bestChildIdx, Loc& bestChildMoveLoc,
  int posesWithChildBuf[NNPos::NN_POLICY_SIZE],
  bool isRoot) const
{
  assert(thread.pla == node.nextPla);

  double maxSelectionValue = POLICY_ILLEGAL_SELECTION_VALUE;
  bestChildIdx = -1;
  bestChildMoveLoc = Board::NULL_LOC;

  int numChildren = node.numChildren;

  double policyProbMassVisited = 0.0;
  uint64_t totalChildVisits = 0;
  for(int i = 0; i<numChildren; i++) {
    const SearchNode* child = node.children[i];
    Loc moveLoc = child->prevMoveLoc;
    int movePos = getPos(moveLoc);
    float nnPolicyProb = node.nnOutput->policyProbs[movePos];
    policyProbMassVisited += nnPolicyProb;

    while(child->statsLock.test_and_set(std::memory_order_acquire));
    uint64_t childVisits = child->stats.visits;
    child->statsLock.clear(std::memory_order_release);

    totalChildVisits += childVisits;
  }
  //Probability mass should not sum to more than 1, giving a generous allowance
  //for floating point error.
  assert(policyProbMassVisited <= 1.0001);

  //First play urgency
  double parentValue;
  if(searchParams.fpuUseParentAverage) {
    while(node.statsLock.test_and_set(std::memory_order_acquire));
    uint64_t parentVisits = node.stats.visits;
    double valueSumWeight = node.stats.valueSumWeight;
    parentValue = node.stats.getCombinedValueSum(searchParams);
    node.statsLock.clear(std::memory_order_release);
    assert(parentVisits > 0);
    assert(valueSumWeight > 0.0);
    parentValue /= valueSumWeight;
  }
  else
    parentValue = node.nnOutput->whiteValue;

  double fpuValue;
  if(isRoot && searchParams.rootNoiseEnabled)
    fpuValue = parentValue;
  else {
    if(thread.pla == P_WHITE)
      fpuValue = parentValue - searchParams.fpuReductionMax * sqrt(policyProbMassVisited);
    else
      fpuValue = parentValue + searchParams.fpuReductionMax * sqrt(policyProbMassVisited);
  }

  //Try all existing children
  for(int i = 0; i<numChildren; i++) {
    const SearchNode* child = node.children[i];
    Loc moveLoc = child->prevMoveLoc;
    double selectionValue = getExploreSelectionValue(node,child,totalChildVisits,fpuValue);
    if(selectionValue > maxSelectionValue) {
      maxSelectionValue = selectionValue;
      bestChildIdx = i;
      bestChildMoveLoc = moveLoc;
    }

    posesWithChildBuf[i] = getPos(moveLoc);
  }
  std::sort(posesWithChildBuf, posesWithChildBuf + numChildren);

  //Try all new children
  for(int movePos = 0; movePos<NNPos::NN_POLICY_SIZE; movePos++) {
    bool alreadyTried = std::binary_search(posesWithChildBuf, posesWithChildBuf + numChildren, movePos);
    if(alreadyTried)
      continue;
    if(isRoot && !rootPassLegal && NNPos::isPassPos(movePos))
      continue;
    int offset = NNPos::getOffset(thread.board.x_size);
    Loc moveLoc = NNPos::posToLoc(movePos,thread.board.x_size,offset);

    double selectionValue = getNewExploreSelectionValue(node,movePos,totalChildVisits,fpuValue);
    if(selectionValue > maxSelectionValue) {
      maxSelectionValue = selectionValue;
      bestChildIdx = numChildren;
      bestChildMoveLoc = moveLoc;
    }
  }

}

void Search::updateStatsAfterPlayout(SearchNode& node, SearchThread& thread, int32_t virtualLossesToSubtract) {
  //Find all children and compute weighting of the children based on their values
  vector<double>& valueChildWeights = thread.valueChildWeightsBuf;
  vector<double>& winLossValues = thread.winLossValuesBuf;
  vector<double>& scoreValues = thread.scoreValuesBuf;
  vector<double>& values = thread.valuesBuf;
  vector<uint64_t>& visits = thread.visitsBuf;

  uint64_t totalChildVisits = 0;

  std::mutex& mutex = mutexPool->getMutex(node.lockIdx);
  unique_lock<std::mutex> lock(mutex);

  int numChildren = node.numChildren;
  int numGoodChildren = 0;
  for(int i = 0; i<numChildren; i++) {
    const SearchNode* child = node.children[i];

    while(child->statsLock.test_and_set(std::memory_order_acquire));
    uint64_t childVisits = child->stats.visits;
    double winLossValueSum = child->stats.winLossValueSum;
    double scoreValueSum = child->stats.scoreValueSum;
    double valueSumWeight = child->stats.valueSumWeight;
    double childValueSum = child->stats.getCombinedValueSum(searchParams);
    child->statsLock.clear(std::memory_order_release);

    if(childVisits <= 0)
      continue;
    assert(valueSumWeight > 0.0);

    double childValue = childValueSum / valueSumWeight;

    winLossValues[numGoodChildren] = winLossValueSum / valueSumWeight;
    scoreValues[numGoodChildren] = scoreValueSum / valueSumWeight;
    values[numGoodChildren] = node.nextPla == P_WHITE ? childValue : -childValue;
    visits[numGoodChildren] = childVisits;
    totalChildVisits += childVisits;

    numGoodChildren++;
  }
  lock.unlock();

  if(searchParams.valueWeightExponent > 0)
    getValueChildWeights(numGoodChildren,values,visits,valueChildWeights);

  double winLossValueSum = 0.0;
  double scoreValueSum = 0.0;
  double valueSumWeight = 0.0;
  for(int i = 0; i<numGoodChildren; i++) {
    double weight = visits[i];
    if(searchParams.visitsExponent != 1.0)
      weight = pow(weight, searchParams.visitsExponent);
    if(searchParams.valueWeightExponent > 0)
      weight *= pow(valueChildWeights[i], searchParams.valueWeightExponent);

    winLossValueSum += weight * winLossValues[i];
    scoreValueSum += weight * scoreValues[i];
    valueSumWeight += weight;
  }
  //Also add in the direct evaluation of this node
  //TODO update this and other places when we have a score prediction on the net
  winLossValueSum += (double)node.nnOutput->whiteValue;
  valueSumWeight += 1.0;


  while(node.statsLock.test_and_set(std::memory_order_acquire));
  node.stats.visits += 1;
  //It's possible that these values are a bit wrong if there's a race and two threads each try to update this
  //each of them only having some of the latest updates for all the children. We just accept this and let the
  //error persist, it will get fixed the next time a visit comes through here and the values will at least
  //be consistent with each other within this node, since statsLock at least ensures these three are set atomically.
  node.stats.winLossValueSum = winLossValueSum;
  node.stats.scoreValueSum = scoreValueSum;
  node.stats.valueSumWeight = valueSumWeight;
  node.virtualLosses -= virtualLossesToSubtract;
  node.statsLock.clear(std::memory_order_release);
}

void Search::runSinglePlayout(SearchThread& thread) {
  int posesWithChildBuf[NNPos::NN_POLICY_SIZE];
  playoutDescend(thread,*rootNode,posesWithChildBuf,true,0);

  //Restore thread state back to the root state
  thread.pla = rootPla;
  thread.board = rootBoard;
  thread.history = rootHistory;
}

void Search::setTerminalValue(SearchNode& node, double winLossValue, double scoreValue, int32_t virtualLossesToSubtract) {
  while(node.statsLock.test_and_set(std::memory_order_acquire));
  node.stats.visits += 1;
  node.stats.winLossValueSum = winLossValue;
  node.stats.scoreValueSum = scoreValue;
  node.stats.valueSumWeight = 1.0;
  node.virtualLosses -= virtualLossesToSubtract;
  node.statsLock.clear(std::memory_order_release);
}

void Search::initNodeNNOutput(
  SearchThread& thread, SearchNode& node,
  bool isRoot, bool skipCache, int32_t virtualLossesToSubtract
) {
  nnEvaluator->evaluate(thread.board, thread.history, thread.pla, thread.nnResultBuf, thread.logStream, skipCache);
  node.nnOutput = std::move(thread.nnResultBuf.result);
  maybeAddPolicyNoise(thread,node,isRoot);

  //TODO update this and other places when we have a score prediction on the net
  //Values in the search are from the perspective of white positive always
  double value = (double)node.nnOutput->whiteValue;
  while(node.statsLock.test_and_set(std::memory_order_acquire));
  node.stats.visits += 1;
  node.stats.winLossValueSum = value;
  node.stats.scoreValueSum = 0.0;
  node.stats.valueSumWeight = 1.0;
  node.virtualLosses -= virtualLossesToSubtract;
  node.statsLock.clear(std::memory_order_release);
}

void Search::playoutDescend(
  SearchThread& thread, SearchNode& node,
  int posesWithChildBuf[NNPos::NN_POLICY_SIZE],
  bool isRoot, int32_t virtualLossesToSubtract
) {
  //Hit terminal node, finish
  //In the case where we're forcing the search to make another move at the root, don't terminate, actually run search for a move more.
  if(!isRoot && thread.history.isGameFinished) {
    if(thread.history.isNoResult) {
      double winLossValue = searchParams.noResultUtilityForWhite;
      double scoreValue = 0.0;
      setTerminalValue(node, winLossValue, scoreValue, virtualLossesToSubtract);
      return;
    }
    else {
      double winLossValue = NNOutput::whiteValueOfWinner(thread.history.winner, searchParams.drawUtilityForWhite);
      assert(thread.board.x_size == thread.board.y_size);
      double scoreValue = NNOutput::whiteValueOfScore(thread.history.finalWhiteMinusBlackScore, thread.board.x_size);
      setTerminalValue(node, winLossValue, scoreValue, virtualLossesToSubtract);
      return;
    }
  }

  std::mutex& mutex = mutexPool->getMutex(node.lockIdx);
  unique_lock<std::mutex> lock(mutex);

  //Hit leaf node, finish
  if(node.nnOutput == nullptr) {
    initNodeNNOutput(thread,node,isRoot,false,virtualLossesToSubtract);
    return;
  }

  //Not leaf node, so recurse

  //Find the best child to descend down
  int bestChildIdx;
  Loc bestChildMoveLoc;
  selectBestChildToDescend(thread,node,bestChildIdx,bestChildMoveLoc,posesWithChildBuf,isRoot);

  //The absurdly rare case that the move chosen is not legal
  //(this should only happen either on a bug or where the nnHash doesn't have full legality information or when there's an actual hash collision).
  //Regenerate the neural net call and continue
  if(!(thread.history.isLegal(thread.board,bestChildMoveLoc,thread.pla))) {
    initNodeNNOutput(thread,node,isRoot,true,virtualLossesToSubtract);
    lock.unlock();
    if(thread.logStream != NULL)
      (*thread.logStream) << "WARNING: Chosen move not legal so regenerated nn output, nnhash=" << node.nnOutput->nnHash << endl;
    return;
  }

  if(bestChildIdx < -1) {
    lock.unlock();
    throw StringError("Search error: No move with sane selection value - can't even pass?");
  }

  //Reallocate the children array to increase capacity if necessary
  if(bestChildIdx >= node.childrenCapacity) {
    int newCapacity = node.childrenCapacity + (node.childrenCapacity / 4) + 1;
    assert(newCapacity < 0x3FFF);
    SearchNode** newArr = new SearchNode*[newCapacity];
    for(int i = 0; i<node.numChildren; i++) {
      newArr[i] = node.children[i];
      node.children[i] = NULL;
    }
    SearchNode** oldArr = node.children;
    node.children = newArr;
    node.childrenCapacity = (uint16_t)newCapacity;
    delete[] oldArr;
  }

  Loc moveLoc = bestChildMoveLoc;

  //Allocate a new child node if necessary
  SearchNode* child;
  if(bestChildIdx == node.numChildren) {
    assert(thread.history.isLegal(thread.board,moveLoc,thread.pla));
    thread.history.makeBoardMoveAssumeLegal(thread.board,moveLoc,thread.pla,rootKoHashTable);
    thread.pla = getOpp(thread.pla);

    node.numChildren++;
    child = new SearchNode(*this,thread,moveLoc);
    node.children[bestChildIdx] = child;

    while(node.statsLock.test_and_set(std::memory_order_acquire));
    child->virtualLosses += searchParams.numVirtualLossesPerThread;
    node.statsLock.clear(std::memory_order_release);

    lock.unlock();
  }
  else {
    child = node.children[bestChildIdx];

    while(node.statsLock.test_and_set(std::memory_order_acquire));
    child->virtualLosses += searchParams.numVirtualLossesPerThread;
    node.statsLock.clear(std::memory_order_release);

    //Unlock before making moves if the child already exists since we don't depend on it at this point
    lock.unlock();

    assert(thread.history.isLegal(thread.board,moveLoc,thread.pla));
    thread.history.makeBoardMoveAssumeLegal(thread.board,moveLoc,thread.pla,rootKoHashTable);
    thread.pla = getOpp(thread.pla);
  }

  //Recurse!
  playoutDescend(thread,*child,posesWithChildBuf,false,searchParams.numVirtualLossesPerThread);

  //Update this node stats
  updateStatsAfterPlayout(node,thread,virtualLossesToSubtract);
}

void Search::printPV(ostream& out, const SearchNode* n, int maxDepth) {
  if(n == NULL)
    return;
  for(int depth = 0; depth < maxDepth; depth++) {
    const SearchNode& node = *n;
    std::mutex& mutex = mutexPool->getMutex(node.lockIdx);
    unique_lock<std::mutex> lock(mutex);

    double maxSelectionValue = POLICY_ILLEGAL_SELECTION_VALUE;
    int bestChildIdx = -1;
    Loc bestChildMoveLoc = Board::NULL_LOC;

    for(int i = 0; i<node.numChildren; i++) {
      SearchNode* child = node.children[i];
      Loc moveLoc = child->prevMoveLoc;
      double selectionValue = getPlaySelectionValue(node,child);
      if(selectionValue > maxSelectionValue) {
        maxSelectionValue = selectionValue;
        bestChildIdx = i;
        bestChildMoveLoc = moveLoc;
      }
    }
    if(bestChildIdx < 0 || bestChildMoveLoc == Board::NULL_LOC)
      return;
    n = node.children[bestChildIdx];
    lock.unlock();

    if(depth > 0)
      out << " ";
    out << Location::toString(bestChildMoveLoc,rootBoard);
  }
}

void Search::printTree(ostream& out, const SearchNode* node, PrintTreeOptions options) {
  string prefix;
  printTreeHelper(out, node, options, prefix, 0, 0, NAN, NAN);
}

void Search::printTreeHelper(
  ostream& out, const SearchNode* n, const PrintTreeOptions& options,
  string& prefix, uint64_t origVisits, int depth, double policyProb, double valueWeight
) {
  if(n == NULL)
    return;
  const SearchNode& node = *n;
  std::mutex& mutex = mutexPool->getMutex(node.lockIdx);
  unique_lock<std::mutex> lock(mutex,std::defer_lock);

  while(node.statsLock.test_and_set(std::memory_order_acquire));
  uint64_t visits = node.stats.visits;
  double winLossValueSum = node.stats.winLossValueSum;
  double scoreValueSum = node.stats.scoreValueSum;
  double valueSumWeight = node.stats.valueSumWeight;
  node.statsLock.clear(std::memory_order_release);

  if(depth == 0)
    origVisits = visits;

  //Output for this node
  {
    out << prefix;
    char buf[64];

    out << ": ";

    if(visits > 0) {
      sprintf(buf,"T %6.2fc ",(winLossValueSum * searchParams.winLossUtilityFactor + scoreValueSum * searchParams.scoreUtilityFactor) / valueSumWeight * 100.0);
      out << buf;
      sprintf(buf,"W %6.2fc ",(winLossValueSum * searchParams.winLossUtilityFactor) / valueSumWeight * 100.0);
      out << buf;
      sprintf(buf,"S %6.2fc ",(scoreValueSum * searchParams.scoreUtilityFactor) / valueSumWeight * 100.0);
      out << buf;
    }

    bool hasNNValue = false;
    double nnValue;
    lock.lock();
    if(node.nnOutput != nullptr) {
      nnValue = node.nnOutput->whiteValue;
      hasNNValue = true;
    }
    lock.unlock();

    if(hasNNValue) {
      sprintf(buf,"V %6.2fc ", nnValue * 100.0);
      out << buf;
    }
    else {
      sprintf(buf,"V --.--c ");
      out << buf;
    }

    if(!isnan(policyProb)) {
      sprintf(buf,"P %5.2f%% ", policyProb * 100.0);
      out << buf;
    }
    if(!isnan(valueWeight)) {
      sprintf(buf,"VW %5.2f%% ", valueWeight * 100.0);
      out << buf;
    }

    sprintf(buf,"N %7" PRIu64 "  --  ", visits);
    out << buf;

    printPV(out, &node, 7);
    out << endl;
  }

  if(depth >= options.branch_.size()) {
    if(depth >= options.maxDepth_ + options.branch_.size())
      return;
    if(visits < options.minVisitsToExpand_)
      return;
    if((double)visits < origVisits * options.minVisitsPropToExpand_)
      return;
  }
  if(depth == options.branch_.size()) {
    out << "---" << playerToString(node.nextPla) << "(" << (node.nextPla == P_WHITE ? "^" : "v") << ")---" << endl;
  }

  lock.lock();

  int numChildren = node.numChildren;

  //Find all children and compute weighting of the children based on their values
  vector<double> valueChildWeights;
  {
    int numGoodChildren = 0;
    vector<double> goodValueChildWeights;
    vector<double> origMoveIdx;
    vector<double> valuesBuf;
    vector<uint64_t> visitsBuf;
    for(int i = 0; i<numChildren; i++) {
      const SearchNode* child = node.children[i];

      while(child->statsLock.test_and_set(std::memory_order_acquire));
      uint64_t childVisits = child->stats.visits;
      double childValueSum = child->stats.getCombinedValueSum(searchParams);
      double childValueSumWeight = child->stats.valueSumWeight;
      child->statsLock.clear(std::memory_order_release);

      if(childVisits <= 0)
        continue;
      assert(childValueSumWeight > 0.0);

      double childValue = childValueSum / childValueSumWeight;

      numGoodChildren++;
      valuesBuf.push_back(node.nextPla == P_WHITE ? childValue : -childValue);
      visitsBuf.push_back(childVisits);
      origMoveIdx.push_back(i);
    }

    getValueChildWeights(numGoodChildren,valuesBuf,visitsBuf,goodValueChildWeights);
    for(int i = 0; i<numChildren; i++)
      valueChildWeights.push_back(0.0);
    for(int i = 0; i<numGoodChildren; i++)
      valueChildWeights[origMoveIdx[i]] = goodValueChildWeights[i];
  }

  //Find all children and record their play values
  vector<tuple<const SearchNode*,double,double,double>> valuedChildren;

  valuedChildren.reserve(numChildren);
  assert(node.nnOutput != nullptr);

  for(int i = 0; i<numChildren; i++) {
    const SearchNode* child = node.children[i];
    Loc moveLoc = child->prevMoveLoc;
    int movePos = getPos(moveLoc);
    double childPolicyProb = node.nnOutput->policyProbs[movePos];
    double selectionValue = getPlaySelectionValue(node,child);
    valuedChildren.push_back(std::make_tuple(child,childPolicyProb,selectionValue,valueChildWeights[i]));
  }

  lock.unlock();

  //Sort in order that we would want to play them
  auto compByValue = [](const tuple<const SearchNode*,double,double,double>& a, const tuple<const SearchNode*,double,double,double>& b) {
    return (std::get<2>(a)) > (std::get<2>(b));
  };
  std::stable_sort(valuedChildren.begin(),valuedChildren.end(),compByValue);

  //Apply filtering conditions, but include children that don't match the filtering condition
  //but where there are children afterward that do, in case we ever use something more complex
  //than plain visits as a filter criterion. Do this by finding the last child that we want as the threshold.
  int lastIdxWithEnoughVisits = numChildren-1;
  while(true) {
    if(lastIdxWithEnoughVisits <= 0)
      break;
    const SearchNode* child = std::get<0>(valuedChildren[lastIdxWithEnoughVisits]);

    while(child->statsLock.test_and_set(std::memory_order_acquire));
    uint64_t childVisits = child->stats.visits;
    child->statsLock.clear(std::memory_order_release);

    bool hasEnoughVisits = childVisits >= options.minVisitsToShow_
      && (double)childVisits >= origVisits * options.minVisitsPropToShow_;
    if(hasEnoughVisits)
      break;
    lastIdxWithEnoughVisits--;
  }

  int numChildrenToRecurseOn = numChildren;
  if(options.maxChildrenToShow_ < numChildrenToRecurseOn)
    numChildrenToRecurseOn = options.maxChildrenToShow_;
  if(lastIdxWithEnoughVisits+1 < numChildrenToRecurseOn)
    numChildrenToRecurseOn = lastIdxWithEnoughVisits+1;


  for(int i = 0; i<numChildren; i++) {
    const SearchNode* child = std::get<0>(valuedChildren[i]);
    double childPolicyProb =  std::get<1>(valuedChildren[i]);
    double childModelProb = std::get<3>(valuedChildren[i]);

    Loc moveLoc = child->prevMoveLoc;

    if((depth >= options.branch_.size() && i < numChildrenToRecurseOn) ||
       (depth < options.branch_.size() && moveLoc == options.branch_[depth]))
    {
      size_t oldLen = prefix.length();
      prefix += Location::toString(moveLoc,rootBoard);
      prefix += " ";
      if(prefix.length() < oldLen+4)
        prefix += " ";
      printTreeHelper(out,child,options,prefix,origVisits,depth+1,childPolicyProb,childModelProb);
      prefix.erase(oldLen);
    }
  }
}



