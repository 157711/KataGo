#ifndef LOGGER_H
#define LOGGER_H

#include <sstream>
#include <fstream>
#include "../core/global.h"
#include "../core/multithread.h"

class LogBuf;

class Logger {
 public:
  Logger();
  ~Logger();

  Logger(const Logger& other) = delete;
  Logger& operator=(const Logger& other) = delete;
  Logger(Logger&& other) = delete;
  Logger& operator=(Logger&& other) = delete;

  void setLogToStdout(bool b);
  void setLogToStderr(bool b);
  void addFile(const string& file);

  //write and ostreams returned are synchronized with other calls to write and other ostream calls
  //The lifetime of the Logger must exceed the lifetimes of any of the ostreams created from it.
  //The caller is responsible for freeing the ostreams
  void write(const string& str);
  ostream* createOStream();

 private:
  bool logToStdout;
  bool logToStderr;
  vector<ofstream*> files;
  vector<LogBuf*> logBufs;
  std::mutex mutex;
};

class LogBuf : public std::stringbuf {
 public:
  LogBuf(Logger* logger);
  ~LogBuf();

  LogBuf(const LogBuf& other) = delete;
  LogBuf& operator=(const LogBuf& other) = delete;
  LogBuf(LogBuf&& other) = delete;
  LogBuf& operator=(LogBuf&& other) = delete;

  virtual int sync();
 private:
  Logger* logger;
};

#endif
