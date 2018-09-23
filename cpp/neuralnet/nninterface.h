#ifndef NNINTERFACE_H
#define NNINTERFACE_H

#include "../core/global.h"
#include "../core/hash.h"
#include "../core/logger.h"
#include "../neuralnet/nninputs.h"

struct LocalGpuHandle; //Not thread-safe, each handle should only be used by one thread
struct LoadedModel;
struct InputBuffers;

//Generic interface to neural net inference.
//There are two backends - a Tensorflow backend, and a CUDA backend.
//Some parameters to these functions only apply for one backend or another.

namespace NeuralNet {
  void globalInitialize(
    const string& tensorflowGpuVisibleGpuList,
    double tensorflowPerProcessGpuMemoryFraction
  );
  void globalCleanup();

  LoadedModel* loadModelFile(const string& file, int modelFileIdx);
  void freeLoadedModel(LoadedModel* loadedModel);

  //Any given thread should only ever create one of these at a time.
  //When using the CUDA backend, will mutably set the GPU that this thread is associated with to the specified index.
  //If logger is specified, may output some info messages to it.
  LocalGpuHandle* createLocalGpuHandle(const LoadedModel* loadedModel, Logger* logger, int maxBatchSize, int cudaGpuIdxForThisThread, bool cudaUseFP16, bool cudaUseNHWC);
  void freeLocalGpuHandle(LocalGpuHandle* gpuHandle);

  InputBuffers* createInputBuffers(const LoadedModel* loadedModel, int maxBatchSize);
  void freeInputBuffers(InputBuffers* buffers);

  float* getRowInplace(InputBuffers* buffers, int rowIdx);
  bool* getSymmetriesInplace(InputBuffers* buffers);

  void getOutput(LocalGpuHandle* gpuHandle, InputBuffers* buffers, int numFilledRows, vector<NNOutput*>& outputs);
}



#endif
