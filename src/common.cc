#include "common.h"

std::string to_string(const std::wstring& str) {
  std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>, wchar_t> utf16conv;
  return utf16conv.to_bytes(str);
}

bool readAVCmd(IPCPipe pipe, AVCmd* cmd, int timeoutMs) {
  if (pipe->read(cmd, sizeof(AVCmd), timeoutMs) != sizeof(AVCmd)) {
    return false;
  }
  return true;
}

void sendAVCmdResult(IPCPipe pipe, AVCmdResult res, size_t size) {
  pipe->write(&res, sizeof(res));
  pipe->write(&size, sizeof(size));
}

AVCmdResult readAVCmdResult(IPCPipe pipe, size_t* size) {
  AVCmdResult res;
  size_t tmpSize = 0;
  auto r1 = pipe->read(&res, sizeof(res), 5000);
  auto r2 = pipe->read(&tmpSize, sizeof(tmpSize), 5000);
  if (r1 != sizeof(res) || r2 != sizeof(tmpSize)) {
    return AVCmdResult::Nack;
  }
  if (size) *size = tmpSize;
  return res;
}

AVCmdResult sendAVCmd(IPCPipe pipe, const AVCmd& cmd, size_t* size) {
  if (pipe->write(&cmd, sizeof(cmd)) != sizeof(cmd)) {
    return AVCmdResult::Nack;
  }

  return readAVCmdResult(pipe, size);
}

AVCmdResult sendAVCmd(IPCPipe pipe, AVCmdType cmd) {
  AVCmd cmdMsg;
  cmdMsg.type = cmd;
  return sendAVCmd(pipe, cmdMsg);
}

AVCmdResult getPacket(IPCPipe pipe, std::vector<uint8_t>& data) {
  AVCmd cmdMsg;
  size_t size = 0;

  data.clear();

  cmdMsg.type = AVCmdType::GetPacket;
  if (sendAVCmd(pipe, cmdMsg, &size) != AVCmdResult::Ack || size == 0) {
    return AVCmdResult::Nack;
  }

  data.resize(size);
  if (pipe->read(data.data(), size, 5000) != size) {
    data.clear();
    return AVCmdResult::Nack;
  }
  return AVCmdResult::Ack;
}

AVCmdResult getFrame(IPCPipe pipe, std::vector<uint8_t>& data) {
  AVCmd cmdMsg;
  size_t size = 0;

  data.clear();

  cmdMsg.type = AVCmdType::GetFrame;
  if (sendAVCmd(pipe, cmdMsg, &size) != AVCmdResult::Ack || size == 0) {
    return AVCmdResult::Nack;
  }

  data.resize(size);
  if (pipe->read(data.data(), size, 5000) != size) {
    data.clear();
    return AVCmdResult::Nack;
  }
  return AVCmdResult::Ack;
}



bool startProccess(const std::string& path, const std::vector<std::string>& params) {
#ifdef WIN32
  std::stringstream ss;
  for (auto& p : params) ss << p << " ";
  ShellExecute(NULL, "open", path.c_str(), ss.str().c_str(), NULL, SW_SHOW);
  return true;
#else
  pid_t child_pid;
  int s;

  /* Spawn the child. The name of the program to execute and the
    command-line arguments are taken from the command-line arguments
    of this program. The environment of the program execed in the
    child is made the same as the parent's environment. */
  std::vector<const char*> argv;
  argv.push_back(path.c_str());
  for (auto& p : params) argv.push_back(p.c_str());
  argv.push_back(0);

  s = posix_spawnp(&child_pid, path.c_str(), NULL, NULL, (char* const*)argv.data(), environ);
  if (s != 0) {
    return false;
  }
  return true;
#endif
}

bool dumpLog = false;

IPCPipe openService(const std::string& instanceId) {
  if (!startService(instanceId)) {
    return nullptr;
  }

  auto pipe = IIPCPipe::open(instanceId);
  if (pipe) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  return pipe;
}

bool closeService(IPCPipe pipe) {
  if (sendAVCmd(pipe, AVCmdType::StopService) != AVCmdResult::Ack) {
    LOG_ERROR << "[ENC] Failed to send stop command";
    return false;
  }

  waitServiceToExit();

  return true;
}