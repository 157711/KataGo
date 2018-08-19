#include "../search/searchparams.h"

SearchParams::SearchParams()
  :winLossUtilityFactor(1.0),
   scoreUtilityFactor(0.08),
   noResultUtilityForWhite(0.0),
   drawUtilityForWhite(0.0),
   cpuctExploration(1.6),
   fpuReductionMax(0.5),
   fpuUseParentAverage(false),
   moveProbModelExponent(0.0),
   rootNoiseEnabled(false),
   rootDirichletNoiseTotalConcentration(10.0),
   rootDirichletNoiseWeight(0.25),
   chosenMoveTemperature(0.0),
   chosenMoveTemperatureEarly(0.0),
   chosenMoveTemperatureHalflife(20),
   chosenMoveSubtract(2.0),
   mutexPoolSize(8192),
   numVirtualLossesPerThread(3),
   numThreads(1),
   maxVisits(((uint64_t)1) << 63),
   maxPlayouts(((uint64_t)1) << 63),
   maxTime(1.0e20)
{}

SearchParams::~SearchParams()
{}
