#pragma once

#ifdef WIN32
#include <Windows.h>
#ifdef max
#undef max
#endif
#else
#include <errno.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wait.h>
#endif

#include <plog/Log.h>
#include <plog/Initializers/RollingFileInitializer.h>
#include <plog/Initializers/ConsoleInitializer.h>
#include <plog/Formatters/TxtFormatter.h>
#include "libav_service.h"
#include "av.h"
#include <chrono>
#include <string>
#include <sstream>
#include <thread>
#include <codecvt>
#include <locale>
#include <inttypes.h>
#include "ipc-pipe.h"

#include <CLI/CLI.hpp>

#include <functional>

extern bool dumpLog;

class Scope {
protected:
  std::function<void()> func;
  bool disabled = false;
public:
  Scope() = default;
  Scope(Scope &) = delete;
  Scope(Scope &&) = delete;
  template<class ScopeFunc>
  Scope(ScopeFunc const &_func) : func(_func) {}
  ~Scope() { if (!disabled) func(); }
  Scope &operator = (Scope &) = delete;
  Scope &operator = (Scope &&) = delete;
  void disable() { disabled = true; }
  void enable() { disabled = false; }
};

std::string to_string(const std::wstring &str);
bool readAVCmd(IPCPipe pipe, AVCmd *cmd, int timeoutMs);
void sendAVCmdResult(IPCPipe pipe, AVCmdResult res, size_t size = 0);
AVCmdResult readAVCmdResult(IPCPipe pipe, size_t *size = nullptr);
AVCmdResult sendAVCmd(IPCPipe pipe, const AVCmd &cmd, size_t *size = nullptr);
AVCmdResult sendAVCmd(IPCPipe pipe, AVCmdType cmd);
AVCmdResult getPacket(IPCPipe pipe, std::vector<uint8_t> &data);
AVCmdResult getFrame(IPCPipe pipe, std::vector<uint8_t> &data);

bool startProccess(const std::string &path, const std::vector<std::string> &params);

IPCPipe openService(const std::string &instanceId);
bool closeService(IPCPipe pipe);

bool startService(const std::string &instanceId);
void waitServiceToExit();