#include "core/global.h"
#include "core/makedir.h"
#include "core/config_parser.h"
#include "core/timer.h"
#include "core/threadsafequeue.h"
#include "dataio/sgf.h"
#include "dataio/trainingwrite.h"
#include "search/asyncbot.h"
#include "program/setup.h"
#include "program/play.h"
#include "program/gitinfo.h"
#include "main.h"

using namespace std;

#define TCLAP_NAMESTARTSTRING "-" //Use single dashes for all flags
#include <tclap/CmdLine.h>

#include <chrono>

#include <csignal>
static std::atomic<bool> sigReceived(false);
static std::atomic<bool> shouldStop(false);
static void signalHandler(int signal)
{
  if(signal == SIGINT || signal == SIGTERM) {
    sigReceived.store(true);
    shouldStop.store(true);
  }
}

//-----------------------------------------------------------------------------------------

//Class for running a game and enqueueing the result as training data.
//Wraps together most of the neural-net-independent parameters to spawn and run a full game.
class GameRunner {
  SearchParams params;
  bool logSearchInfo;
  bool logMoves;
  int maxMovesPerGame;
  string searchRandSeedBase;
  MatchPairer* matchPairer;
  GameInitializer* gameInit;

public:
  GameRunner(ConfigParser& cfg, const string& sRandSeedBase)
    :params(),logSearchInfo(),logMoves(),maxMovesPerGame(),
     searchRandSeedBase(sRandSeedBase),matchPairer(NULL),gameInit(NULL)
  {
    //TODO we should dynamically randomize the no result and draw utilities, and provide them as inputs to the net?
    vector<SearchParams> paramss = Setup::loadParams(cfg);
    if(paramss.size() != 1)
      throw StringError("Can only specify one set of search parameters for self-play");
    params = paramss[0];

    logSearchInfo = cfg.getBool("logSearchInfo");
    logMoves = cfg.getBool("logMoves");
    maxMovesPerGame = cfg.getInt("maxMovesPerGame",1,1 << 30);

    //Mostly the matchpairer for the logging and game counting
    bool forSelfPlay = true;
    matchPairer = new MatchPairer(cfg,forSelfPlay);

    //Initialize object for randomizing game settings
    gameInit = new GameInitializer(cfg);
  }

  ~GameRunner() {
    delete matchPairer;
    delete gameInit;
  }

  bool runGameAndWriteData(
    NNEvaluator* nnEval, Logger& logger,
    int dataPosLen, ThreadSafeQueue<FinishedGameData*>& finishedGameQueue
  ) {
    int64_t gameIdx;
    bool shouldContinue = matchPairer->getMatchup(gameIdx, logger, nnEval, NULL);
    if(!shouldContinue)
      return false;

    Board board; Player pla; BoardHistory hist; int numExtraBlack;
    gameInit->createGame(board,pla,hist,numExtraBlack);
    
    string searchRandSeed = searchRandSeedBase + ":" + Global::int64ToString(gameIdx);
    Rand gameRand(searchRandSeed + ":" + "forGameRand");

    //Avoid interactions between the two bots and make sure root noise is effective on each new search
    bool clearBotAfterSearchThisGame = true;
    //In 2% of games, don't autoterminate the game upon all pass alive, to just provide a tiny bit of training data on positions that occur
    //as both players must wrap things up manually, because within the search we don't autoterminate games, meaning that the NN will get
    //called on positions that occur after the game would have been autoterminated.
    bool doEndGameIfAllPassAlive = gameRand.nextBool(0.98);

    AsyncBot* bot = new AsyncBot(params, nnEval, &logger, searchRandSeed);
    FinishedGameData* finishedGameData = new FinishedGameData(dataPosLen, params.drawEquivalentWinsForWhite);
    Play::runGame(
      board,pla,hist,numExtraBlack,bot,bot,
      doEndGameIfAllPassAlive,clearBotAfterSearchThisGame,
      logger,logSearchInfo,logMoves,
      maxMovesPerGame,shouldStop,
      finishedGameData
    );
    delete bot;

    finishedGameQueue.waitPush(finishedGameData);
    return true;
  }

};

//Wraps together a neural net and handles for outputting training data for it.
//There should be one of these active per currently-loaded neural net, and one active thread
//looping and actually performing the data output
struct NetAndStuff {
  string nnName;
  NNEvaluator* nnEval;  

  ThreadSafeQueue<FinishedGameData*> finishedGameQueue;
  int numGameThreads;
  bool isDraining;

  TrainingDataWriter* dataWriter;
  ofstream* sgfOut;

public:
  NetAndStuff(const string& name, NNEvaluator* neval, int maxDataQueueSize, TrainingDataWriter* dWriter, ofstream* sOut)
    :nnName(name),nnEval(neval),
     finishedGameQueue(maxDataQueueSize),
     numGameThreads(0),isDraining(false),
     dataWriter(dWriter),sgfOut(sOut)
  {}

  ~NetAndStuff() {
    delete nnEval;
    delete dataWriter;
    if(sgfOut != NULL)
      delete sgfOut;
  }
  
  void runWriteDataLoop() {
    while(true) {
      FinishedGameData* data = finishedGameQueue.waitPop();
      if(data == NULL)
        break;
      dataWriter->writeGame(*data);
      if(sgfOut != NULL) {
        int startTurnIdx = data->startHist.moveHistory.size();
        assert(data->startHist.moveHistory.size() <= data->endHist.moveHistory.size());
        WriteSgf::writeSgf(*sgfOut,nnName,nnName,data->startHist.rules,data->startBoard,data->endHist,startTurnIdx,&(data->whiteValueTargetsByTurn));
        (*sgfOut) << endl;
      }
      delete data;
    }
    
    dataWriter->close();
    if(sgfOut != NULL)
      sgfOut->close();
  }

  //NOT threadsafe - needs to be externally synchronized
  //Game threads beginning a game using this net call this
  void registerGameThread() {
    assert(!isDraining);
    numGameThreads++;
  }
  
  //NOT threadsafe - needs to be externally synchronized
  //Game threads finishing a game using this net call this
  void unregisterGameThread() {
    numGameThreads--;
    if(isDraining && numGameThreads <= 0)
      finishedGameQueue.forcePush(NULL); //forcePush so as not to block   
  }

  //NOT threadsafe - needs to be externally synchronized
  //Mark that we should start draining this net and not starting new games with it
  void markAsDraining() {
    if(!isDraining) {
      isDraining = true;
      if(numGameThreads <= 0)
        finishedGameQueue.forcePush(NULL); //forcePush so as not to block   
    }
  }
 
};





//-----------------------------------------------------------------------------------------



int MainCmds::selfPlay(int argc, const char* const* argv) {
  Board::initHash();
  Rand seedRand;

  string configFile;
  string logFile;
  int inputsVersion;
  string modelFile;
  string sgfOutputDir;
  string trainDataOutputDir;
  try {
    TCLAP::CmdLine cmd("Generate training data via self play", ' ', "1.0",true);
    TCLAP::ValueArg<string> configFileArg("","config-file","Config file to use",true,string(),"FILE");
    TCLAP::ValueArg<string> logFileArg("","log-file","Log file to output to",true,string(),"FILE");
    //TODO do this instead
    //TCLAP::ValueArg<string> modelsDirArg("","models-dir","Dir to poll and load models from",true,string(),"DIR");
    TCLAP::ValueArg<int>    inputsVersionArg("","inputs-version","Version of neural net input features to use",true,0,"INT");
    TCLAP::ValueArg<string> modelFileArg("","model-file","Neural net model file to use",true,string(),"FILE");
    TCLAP::ValueArg<string> sgfOutputDirArg("","sgf-output-dir","Dir to output sgf files",true,string(),"DIR");
    TCLAP::ValueArg<string> trainDataOutputDirArg("","train-data-output-dir","Dir to output training data",true,string(),"DIR");
    cmd.add(configFileArg);
    cmd.add(logFileArg);
    //cmd.add(modelsDirArg);
    cmd.add(inputsVersionArg);
    cmd.add(modelFileArg);
    cmd.add(sgfOutputDirArg);
    cmd.add(trainDataOutputDirArg);
    cmd.parse(argc,argv);
    configFile = configFileArg.getValue();
    logFile = logFileArg.getValue();
    inputsVersion = inputsVersionArg.getValue();
    modelFile = modelFileArg.getValue();
    sgfOutputDir = sgfOutputDirArg.getValue();
    trainDataOutputDir = trainDataOutputDirArg.getValue();
  }
  catch (TCLAP::ArgException &e) {
    cerr << "Error: " << e.error() << " for argument " << e.argId() << endl;
    return 1;
  }
  ConfigParser cfg(configFile);

  Logger logger;
  logger.addFile(logFile);
  bool logToStdout = cfg.getBool("logToStdout");
  logger.setLogToStdout(logToStdout);

  logger.write("Self Play Engine starting...");
  logger.write(string("Git revision: ") + GIT_REVISION);

  //TODO this will go change a bit once we have a polling loop?
  NNEvaluator* nnEval;
  {
    Setup::initializeSession(cfg);
    vector<NNEvaluator*> nnEvals = Setup::initializeNNEvaluators({modelFile},cfg,logger,seedRand);
    assert(nnEvals.size() == 1);
    nnEval = nnEvals[0];
  }
  logger.write("Loaded neural net");

  //Load runner settings
  int numGameThreads = cfg.getInt("numGameThreads",1,16384);
  string searchRandSeedBase = Global::uint64ToHexString(seedRand.nextUInt64());

  //Width of the board to use when writing data, typically 19
  int dataPosLen = cfg.getInt("dataPosLen",9,37);

  GameRunner* gameRunner = new GameRunner(cfg, searchRandSeedBase);

  //Check for unused config keys
  {
    vector<string> unusedKeys = cfg.unusedKeys();
    for(size_t i = 0; i<unusedKeys.size(); i++) {
      string msg = "WARNING: Unused key '" + unusedKeys[i] + "' in " + configFile;
      logger.write(msg);
      cerr << msg << endl;
    }
  }

  //Done loading!
  //------------------------------------------------------------------------------------
  logger.write("Loaded all config stuff, starting self play");
  if(!logToStdout)
    cout << "Loaded all config stuff, starting self play" << endl;

  //TODO write to subdirs once we have proper polling for new nn models
  if(sgfOutputDir != string())
    MakeDir::make(sgfOutputDir);

  if(!std::atomic_is_lock_free(&shouldStop))
    throw StringError("shouldStop is not lock free, signal-quitting mechanism for terminating matches will NOT work!");
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  const int maxDataQueueSize = 5000;
  const int maxRowsPerFile = 100000;

  std::mutex netAndStuffsMutex;
  vector<NetAndStuff*> netAndStuffs;
  std::condition_variable netAndStuffsIsEmpty;

  //Looping thread for writing data for a single net
  auto dataWriteLoop = [&netAndStuffsMutex,&netAndStuffs,&netAndStuffsIsEmpty,&logger](NetAndStuff* netAndStuff) {
    netAndStuff->runWriteDataLoop();
    logger.write("Data write loop finishing for neural net: " + netAndStuff->nnName);

    std::unique_lock<std::mutex> lock(netAndStuffsMutex);

    //Find where our netAndStuff is and remove it
    bool found = false;
    for(int i = 0; i<netAndStuffs.size(); i++) {
      if(netAndStuffs[i] == netAndStuff) {
        netAndStuffs.erase(netAndStuffs.begin()+i);
        assert(netAndStuff->numGameThreads == 0);
        assert(netAndStuff->isDraining);
        delete netAndStuff;
        found = true;
        break;
      }
    }
    assert(found);
    if(netAndStuffs.size() == 0)
      netAndStuffsIsEmpty.notify_all();
    
    lock.unlock();
    logger.write("Data write loop terminating");
  };

  //Initialize the initial neural net
  {
    //TODO replace this with a call to the same code that the polling loop would use to find the most recent net
    Rand sgfsNameRand;
    string nnName = "bot";
    TrainingDataWriter* dataWriter = new TrainingDataWriter(trainDataOutputDir, inputsVersion, maxRowsPerFile, dataPosLen);
    ofstream* sgfOut = sgfOutputDir.length() > 0 ? (new ofstream(sgfOutputDir + "/" + Global::uint64ToHexString(sgfsNameRand.nextUInt64()) + ".sgfs")) : NULL;
    NetAndStuff* newNet = new NetAndStuff(nnName, nnEval, maxDataQueueSize, dataWriter, sgfOut);

    std::unique_lock<std::mutex> lock(netAndStuffsMutex);
    netAndStuffs.push_back(newNet);
    std::thread newThread(dataWriteLoop,newNet);
    newThread.detach();
  }

  auto gameLoop = [
    &gameRunner,
    &sgfOutputDir,&logger,
    &netAndStuffsMutex,&netAndStuffs,
    dataPosLen
  ]() {
    std::unique_lock<std::mutex> lock(netAndStuffsMutex);
    while(true) {
      if(shouldStop.load())
        break;

      assert(netAndStuffs.size() > 0);
      NetAndStuff* netAndStuff = netAndStuffs[netAndStuffs.size()-1];
      netAndStuff->registerGameThread();

      lock.unlock();

      bool shouldContinue = gameRunner->runGameAndWriteData(
        netAndStuff->nnEval, logger,
        dataPosLen, netAndStuff->finishedGameQueue
      );
      if(!shouldContinue)
        break;

      lock.lock();
      netAndStuff->unregisterGameThread();
    }
    
    lock.unlock();
    logger.write("Game loop terminating");
  };

  //Looping thread for polling for new neural nets and loading them in
  std::condition_variable pollSleepVar;
  auto pollLoop = [&netAndStuffsMutex,&netAndStuffs,&pollSleepVar,&logger,&dataWriteLoop]() {
    std::unique_lock<std::mutex> lock(netAndStuffsMutex);
    while(true) {
      if(shouldStop.load())
        break;

      //TODO
      //Poll to see if there are any new nets to load
      NetAndStuff* newNet = NULL;

      if(newNet != NULL) {
        logger.write("Loaded new neural net " + newNet->nnName);
        netAndStuffs.push_back(newNet);
        for(int i = 0; i<netAndStuffs.size()-1; i++) {
          netAndStuffs[i]->markAsDraining();
        }

        std::thread newThread(dataWriteLoop,newNet);
        newThread.detach();
      }
      pollSleepVar.wait_for(lock, std::chrono::seconds(60));
    }
    for(int i = 0; i<netAndStuffs.size(); i++)
      netAndStuffs[i]->markAsDraining();

    lock.unlock();
    logger.write("Polling net loading loop terminating");
  };

  
  vector<std::thread> threads;
  for(int i = 0; i<numGameThreads; i++) {
    threads.push_back(std::thread(gameLoop));
  }
  std::thread pollLoopThread(pollLoop);

  //Wait for all game threads to stop
  for(int i = 0; i<numGameThreads; i++)
    threads[i].join();

  //If by now somehow shouldStop is not true, set it to be true since all game threads are toast
  shouldStop.store(true);

  //Wake up the polling thread rather than waiting up to 60s for it to wake up on its own, and
  //wait for it to die.
  pollSleepVar.notify_all();
  pollLoopThread.join();

  //Wait for netAndStuffs to be empty, which indicates that the detached data writing threads
  //have all cleaned up and removed their netAndStuff.
  {
    std::unique_lock<std::mutex> lock(netAndStuffsMutex);
    while(netAndStuffs.size() > 0)
      netAndStuffsIsEmpty.wait(lock);
  }

  //Delete and clean up everything else
  delete nnEval;
  NeuralNet::globalCleanup();

  delete gameRunner;

  if(sigReceived.load())
    logger.write("Exited cleanly after signal");
  logger.write("All cleaned up, quitting");
  return 0;
}


