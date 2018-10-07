#ifndef SETUP_H
#define SETUP_H

#include "core/global.h"
#include "core/config_parser.h"
#include "search/asyncbot.h"

//Some bits of initialization and main function logic shared between various programs
namespace Setup {

  void initializeSession(ConfigParser& cfg);

  vector<NNEvaluator*> initializeNNEvaluators(
    const vector<string>& nnModelFiles,
    ConfigParser& cfg,
    Logger& logger,
    Rand& seedRand
  );

  vector<SearchParams> loadParams(
    ConfigParser& cfg
  );

}

#endif
