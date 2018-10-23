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

#include <boost/filesystem.hpp>

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
  bool logSearchInfo;
  bool logMoves;
  int maxMovesPerGame;
  string searchRandSeedBase;
  MatchPairer* matchPairer;
  GameInitializer* gameInit;

public:
  GameRunner(ConfigParser& cfg, const string& sRandSeedBase)
    :logSearchInfo(),logMoves(),maxMovesPerGame(),
     searchRandSeedBase(sRandSeedBase),matchPairer(NULL),gameInit(NULL)
  {
    vector<SearchParams> paramss = Setup::loadParams(cfg);
    if(paramss.size() != 1)
      throw StringError("Can only specify one set of search parameters for self-play");
    SearchParams baseParams = paramss[0];

    logSearchInfo = cfg.getBool("logSearchInfo");
    logMoves = cfg.getBool("logMoves");
    maxMovesPerGame = cfg.getInt("maxMovesPerGame",1,1 << 30);

    //Mostly the matchpairer for the logging and game counting
    bool forSelfPlay = true;
    matchPairer = new MatchPairer(cfg,forSelfPlay);

    //Initialize object for randomizing game settings
    gameInit = new GameInitializer(cfg,baseParams);
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
    SearchParams params;
    gameInit->createGame(board,pla,hist,numExtraBlack,params);

    string searchRandSeed = searchRandSeedBase + ":" + Global::int64ToString(gameIdx);
    Rand gameRand(searchRandSeed + ":" + "forGameRand");

    //Avoid interactions between the two bots and make sure root noise is effective on each new search
    bool clearBotAfterSearchThisGame = true;
    //In 2% of games, don't autoterminate the game upon all pass alive, to just provide a tiny bit of training data on positions that occur
    //as both players must wrap things up manually, because within the search we don't autoterminate games, meaning that the NN will get
    //called on positions that occur after the game would have been autoterminated.
    bool doEndGameIfAllPassAlive = gameRand.nextBool(0.98);

    Search* bot = new Search(params, nnEval, searchRandSeed);
    FinishedGameData* finishedGameData = new FinishedGameData(dataPosLen, params.drawEquivalentWinsForWhite);
    bool fancyModes = true;
    Play::runGame(
      board,pla,hist,numExtraBlack,bot,bot,
      doEndGameIfAllPassAlive,clearBotAfterSearchThisGame,
      logger,logSearchInfo,logMoves,
      maxMovesPerGame,shouldStop,
      fancyModes,
      finishedGameData,&gameRand
    );
    delete bot;

    //Make sure not to write the game if we terminated in the middle of this game!
    if(shouldStop.load())
      return false;

    finishedGameQueue.waitPush(finishedGameData);
    return true;
  }

};

//Wraps together a neural net and handles for outputting training data for it.
//There should be one of these active per currently-loaded neural net, and one active thread
//looping and actually performing the data output
struct NetAndStuff {
  string modelName;
  NNEvaluator* nnEval;

  int maxDataQueueSize;
  ThreadSafeQueue<FinishedGameData*> finishedGameQueue;
  int numGameThreads;
  bool isDraining;

  TrainingDataWriter* dataWriter;
  ofstream* sgfOut;

public:
  NetAndStuff(const string& name, NNEvaluator* neval, int maxDQueueSize, TrainingDataWriter* dWriter, ofstream* sOut)
    :modelName(name),nnEval(neval),
     maxDataQueueSize(maxDQueueSize),
     finishedGameQueue(maxDQueueSize),
     numGameThreads(0),isDraining(false),
     dataWriter(dWriter),sgfOut(sOut)
  {}

  ~NetAndStuff() {
    delete nnEval;
    delete dataWriter;
    if(sgfOut != NULL)
      delete sgfOut;
  }

  void runWriteDataLoop(Logger& logger) {
    while(true) {
      size_t size = finishedGameQueue.size();
      if(size > maxDataQueueSize / 2)
        logger.write(Global::strprintf("WARNING: Struggling to keep up writing data, %d games enqueued out of %d max",size,maxDataQueueSize));

      FinishedGameData* data = finishedGameQueue.waitPop();
      if(data == NULL)
        break;
      dataWriter->writeGame(*data);
      if(sgfOut != NULL) {
        int startTurnIdx = data->startHist.moveHistory.size();
        assert(data->startHist.moveHistory.size() <= data->endHist.moveHistory.size());
        WriteSgf::writeSgf(*sgfOut,modelName,modelName,data->startHist.rules,data->startBoard,data->endHist,startTurnIdx,&(data->whiteValueTargetsByTurn));
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
  int inputsVersion;
  string modelsDir;
  string outputDir;
  try {
    TCLAP::CmdLine cmd("Generate training data via self play", ' ', "1.0",true);
    TCLAP::ValueArg<string> configFileArg("","config-file","Config file to use",true,string(),"FILE");
    TCLAP::ValueArg<int>    inputsVersionArg("","inputs-version","Version of neural net input features to use for data",true,0,"INT");
    TCLAP::ValueArg<string> modelsDirArg("","models-dir","Dir to poll and load models from",true,string(),"DIR");
    TCLAP::ValueArg<string> outputDirArg("","output-dir","Dir to output files",true,string(),"DIR");
    cmd.add(configFileArg);
    cmd.add(inputsVersionArg);
    cmd.add(modelsDirArg);
    cmd.add(outputDirArg);
    cmd.parse(argc,argv);
    configFile = configFileArg.getValue();
    inputsVersion = inputsVersionArg.getValue();
    modelsDir = modelsDirArg.getValue();
    outputDir = outputDirArg.getValue();
  }
  catch (TCLAP::ArgException &e) {
    cerr << "Error: " << e.error() << " for argument " << e.argId() << endl;
    return 1;
  }
  ConfigParser cfg(configFile);

  MakeDir::make(outputDir);

  Logger logger;
  logger.addFile(outputDir + "/log.log");
  bool logToStdout = cfg.getBool("logToStdout");
  logger.setLogToStdout(logToStdout);

  logger.write("Self Play Engine starting...");
  logger.write(string("Git revision: ") + GIT_REVISION);

  //Load runner settings
  const int numGameThreads = cfg.getInt("numGameThreads",1,16384);
  const string searchRandSeedBase = Global::uint64ToHexString(seedRand.nextUInt64());

  //Width of the board to use when writing data, typically 19
  const int dataPosLen = cfg.getInt("dataPosLen",9,37);
  //Max number of games that we will allow to be queued up and not written out
  const int maxDataQueueSize = cfg.getInt("maxDataQueueSize",1,1000000);
  const int maxRowsPerFile = cfg.getInt("maxRowsPerFile",1,100000000);

  GameRunner* gameRunner = new GameRunner(cfg, searchRandSeedBase);

  Setup::initializeSession(cfg);
  
  //Done loading!
  //------------------------------------------------------------------------------------
  logger.write("Loaded all config stuff, starting self play");
  if(!logToStdout)
    cout << "Loaded all config stuff, starting self play" << endl;

  if(!std::atomic_is_lock_free(&shouldStop))
    throw StringError("shouldStop is not lock free, signal-quitting mechanism for terminating matches will NOT work!");
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  std::mutex netAndStuffsMutex;
  vector<NetAndStuff*> netAndStuffs;
  std::condition_variable netAndStuffsIsEmpty;

  //Looping thread for writing data for a single net
  auto dataWriteLoop = [&netAndStuffsMutex,&netAndStuffs,&netAndStuffsIsEmpty,&logger](NetAndStuff* netAndStuff) {
    logger.write("Data write loop starting for neural net: " + netAndStuff->modelName);
    netAndStuff->runWriteDataLoop(logger);
    logger.write("Data write loop finishing for neural net: " + netAndStuff->modelName);

    std::unique_lock<std::mutex> lock(netAndStuffsMutex);

    //Find where our netAndStuff is and remove it
    string name = netAndStuff->modelName;
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
    logger.write("Data write loop cleaned up and terminating for " + name);
  };

  auto loadLatestNeuralNet = [inputsVersion,maxDataQueueSize,maxRowsPerFile,dataPosLen,&modelsDir,&outputDir,&logger,&cfg](const string* lastNetName) -> NetAndStuff* {
    namespace bfs = boost::filesystem;

    bool hasLatestTime = false;
    std::time_t latestTime = 0;
    bfs::path latestPath;
    for(bfs::directory_iterator iter(modelsDir); iter != bfs::directory_iterator(); ++iter) {
      bfs::path dirPath = iter->path();
      if(!bfs::is_directory(dirPath))
        continue;

      time_t thisTime = bfs::last_write_time(dirPath);
      if(!hasLatestTime || thisTime > latestTime) {
        hasLatestTime = true;
        latestTime = thisTime;
        latestPath = dirPath;
      }
    }
    
    string modelName = "random";
    string modelFile = "/dev/null";
    if(hasLatestTime) {
      modelName = latestPath.filename().string();
      modelFile = modelsDir + "/" + modelName + "/model.txt.gz";
      if(!bfs::exists(bfs::path(modelFile))) {
        modelFile = modelsDir + "/" + modelName + "/model.txt";
        if(!bfs::exists(bfs::path(modelFile)))
          logger.write("Warning: Skipping model " + modelName + " due to not finding model.txt or model.txt.gz");
      }
    }

    //No new neural nets yet
    if(lastNetName != NULL && *lastNetName == modelName)
      return NULL;

    logger.write("Found new neural net " + modelName);

    bool debugSkipNeuralNetDefault = (modelFile == "/dev/null");

    Rand rand;
    vector<NNEvaluator*> nnEvals = Setup::initializeNNEvaluators({modelFile},cfg,logger,rand,debugSkipNeuralNetDefault);
    assert(nnEvals.size() == 1);
    NNEvaluator* nnEval = nnEvals[0];
    logger.write("Loaded latest neural net " + modelName + " from: " + modelFile);

    string modelDir = outputDir + "/" + modelName;
    string sgfOutputDir = modelDir + "/sgfs";
    string trainDataOutputDir = modelDir + "/tdata";
    assert(outputDir != string());
    
    MakeDir::make(modelDir);
    MakeDir::make(sgfOutputDir);
    MakeDir::make(trainDataOutputDir);
    
    //Note that this inputsVersion passed here is NOT necessarily the same as the one used in the neural net self play, it
    //simply controls the input feature version for the written data
    TrainingDataWriter* dataWriter = new TrainingDataWriter(trainDataOutputDir, inputsVersion, maxRowsPerFile, dataPosLen);
    ofstream* sgfOut = sgfOutputDir.length() > 0 ? (new ofstream(sgfOutputDir + "/" + Global::uint64ToHexString(rand.nextUInt64()) + ".sgfs")) : NULL;
    NetAndStuff* newNet = new NetAndStuff(modelName, nnEval, maxDataQueueSize, dataWriter, sgfOut);
    return newNet;
  };
    
  //Initialize the initial neural net
  {
    NetAndStuff* newNet = loadLatestNeuralNet(NULL);
    assert(newNet != NULL);
    
    std::unique_lock<std::mutex> lock(netAndStuffsMutex);
    netAndStuffs.push_back(newNet);
    std::thread newThread(dataWriteLoop,newNet);
    newThread.detach();
  }

  //Check for unused config keys
  {
    vector<string> unusedKeys = cfg.unusedKeys();
    for(size_t i = 0; i<unusedKeys.size(); i++) {
      string msg = "WARNING: Unused key '" + unusedKeys[i] + "' in " + configFile;
      logger.write(msg);
      cerr << msg << endl;
    }
  }
  
  auto gameLoop = [
    &gameRunner,
    &logger,
    &netAndStuffsMutex,&netAndStuffs,
    dataPosLen
  ](int threadIdx) {
    std::unique_lock<std::mutex> lock(netAndStuffsMutex);
    string prevModelName;
    while(true) {
      if(shouldStop.load())
        break;

      assert(netAndStuffs.size() > 0);
      NetAndStuff* netAndStuff = netAndStuffs[netAndStuffs.size()-1];
      netAndStuff->registerGameThread();

      lock.unlock();

      if(prevModelName != netAndStuff->modelName) {
        prevModelName = netAndStuff->modelName;
        logger.write("Game loop thread " + Global::intToString(threadIdx) + " starting game on new neural net: " + prevModelName);
      }

      bool shouldContinue = gameRunner->runGameAndWriteData(
        netAndStuff->nnEval, logger,
        dataPosLen, netAndStuff->finishedGameQueue
      );

      lock.lock();

      netAndStuff->unregisterGameThread();

      if(!shouldContinue)
        break;
    }

    lock.unlock();
    logger.write("Game loop thread " + Global::intToString(threadIdx) + " terminating");
  };

  //Looping thread for polling for new neural nets and loading them in
  std::condition_variable modelLoadSleepVar;
  auto modelLoadLoop = [&netAndStuffsMutex,&netAndStuffs,&modelLoadSleepVar,&logger,&dataWriteLoop,&loadLatestNeuralNet]() {
    logger.write("Model loading loop thread starting");

    string lastNetName;
    std::unique_lock<std::mutex> lock(netAndStuffsMutex);
    while(true) {
      if(shouldStop.load())
        break;
      if(netAndStuffs.size() <= 0) {
        logger.write("Model loop thread UNEXPECTEDLY found 0 netAndStuffs... terminating now..?");
        break;
      }
      
      lastNetName = netAndStuffs[netAndStuffs.size()-1]->modelName;

      lock.unlock();

      NetAndStuff* newNet = loadLatestNeuralNet(&lastNetName);

      lock.lock();

      //Check again if we should be stopping, after loading the new net, and quit more quickly.
      if(shouldStop.load()) {
        lock.unlock();
        delete newNet;
        lock.lock();
        break;
      }

      //Otherwise, we're not stopped yet, so stick a new net on to things.
      if(newNet != NULL) {
        logger.write("Model loading loop thread loaded new neural net " + newNet->modelName);
        netAndStuffs.push_back(newNet);
        for(int i = 0; i<netAndStuffs.size()-1; i++) {
          netAndStuffs[i]->markAsDraining();
        }

        std::thread newThread(dataWriteLoop,newNet);
        newThread.detach();
      }

      //Sleep for a while and then re-poll
      modelLoadSleepVar.wait_for(lock, std::chrono::seconds(60), [](){return shouldStop.load();});
    }

    //As part of cleanup, anything remaining, mark them as draining so that if they also have
    //no more game threads, they all quit out.
    for(int i = 0; i<netAndStuffs.size(); i++)
      netAndStuffs[i]->markAsDraining();

    lock.unlock();
    logger.write("Model loading loop thread terminating");
  };


  vector<std::thread> threads;
  for(int i = 0; i<numGameThreads; i++) {
    threads.push_back(std::thread(gameLoop,i));
  }
  std::thread modelLoadLoopThread(modelLoadLoop);

  //Wait for all game threads to stop
  for(int i = 0; i<numGameThreads; i++)
    threads[i].join();

  //Wake up the model loading thread rather than waiting up to 60s for it to wake up on its own, and
  //wait for it to die.
  {
    //Lock so that we don't race where we notify the loading thread to wake when it's still in
    //its own critical section but not yet slept
    std::lock_guard<std::mutex> lock(netAndStuffsMutex);
    //If by now somehow shouldStop is not true, set it to be true since all game threads are toast
    shouldStop.store(true);
    modelLoadSleepVar.notify_all();
  }
  modelLoadLoopThread.join();

  //Wait for netAndStuffs to be empty, which indicates that the detached data writing threads
  //have all cleaned up and removed their netAndStuff.
  {
    std::unique_lock<std::mutex> lock(netAndStuffsMutex);
    while(netAndStuffs.size() > 0) {
      netAndStuffsIsEmpty.wait(lock);
    }
  }

  //Delete and clean up everything else
  NeuralNet::globalCleanup();
  delete gameRunner;

  if(sigReceived.load())
    logger.write("Exited cleanly after signal");
  logger.write("All cleaned up, quitting");
  return 0;
}


