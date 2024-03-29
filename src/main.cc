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

#include "libav_service.h"
#include "av.h"
#include <chrono>
#include <string>
#include <sstream>
#include <thread>
#include <codecvt>
#include <locale>
#include "ipc-pipe.h"

#include <CLI/CLI.hpp>

bool dumpLog = false;
FILE *LOGFILE = stderr;

#include <functional>
class Scope {
protected:
  std::function<void()> func;
  bool disabled = false;
public:
  Scope() = default;
  Scope(Scope &) = delete;
  Scope(Scope &&) = delete;
  template<class ScopeFunc>
  Scope(ScopeFunc const &_func) : func(_func) { }
  ~Scope() { if (!disabled) func(); }
  Scope &operator = (Scope &) = delete;
  Scope &operator = (Scope &&) = delete;
  void disable() { disabled = true; }
  void enable() { disabled = false; }
};

std::string to_string(const std::wstring &str) {
  std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>, wchar_t> utf16conv;
  return utf16conv.to_bytes(str);
}

const char *appPath = nullptr;

bool readAVCmd(IPCPipe pipe, AVCmd *cmd, int timeoutMs) {
  if (pipe->read(cmd, sizeof(AVCmd), timeoutMs) != sizeof(AVCmd)) {
    return false;
  }
  return true;
}

void sendAVCmdResult(IPCPipe pipe, AVCmdResult res, size_t size = 0) {
  pipe->write(&res, sizeof(res));
  pipe->write(&size, sizeof(size));
}

AVCmdResult readAVCmdResult(IPCPipe pipe, size_t *size = nullptr) {
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

AVCmdResult sendAVCmd(IPCPipe pipe, const AVCmd &cmd, size_t *size = nullptr) {
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

AVCmdResult getPacket(IPCPipe pipe, std::vector<uint8_t> &data) {
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

AVCmdResult getFrame(IPCPipe pipe, std::vector<uint8_t> &data) {
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



bool startProccess(const std::string &path, const std::vector<std::string> &params) {
#ifdef WIN32
  std::stringstream ss;
  for (auto &p : params) ss << p << " ";
  ShellExecute(NULL, "open", path.c_str(), ss.str().c_str(), NULL, SW_SHOW);
  return true;
#else
  pid_t child_pid;
  int s;

  /* Spawn the child. The name of the program to execute and the
    command-line arguments are taken from the command-line arguments
    of this program. The environment of the program execed in the
    child is made the same as the parent's environment. */
  std::vector<const char *> argv;
  argv.push_back(path.c_str());
  for (auto &p : params) argv.push_back(p.c_str());
  argv.push_back(0);

  s = posix_spawnp(&child_pid, path.c_str(), NULL, NULL, (char *const *)argv.data(), environ);
  if (s != 0) {
    return false;
  }
  return true;
#endif
}



IPCPipe openService(const std::string &instanceId) {
  std::vector<std::string> params = { "-i", instanceId };
  if (dumpLog) params.push_back("--log");
  startProccess(appPath, params);
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));

  auto pipe = IIPCPipe::open(instanceId);
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));

  return pipe;
}

int closeService(IPCPipe pipe) {
  if (sendAVCmd(pipe, AVCmdType::StopService) != AVCmdResult::Ack) {
    fprintf(LOGFILE, "Failed to send stop command\n");
    return 3;
  }
  return 0;
}


int runEncodeTest(bool &isHEVC, int testWidth, int testHeight, const std::string &testFile) {
  int width  = testWidth;
  int height = testHeight;
  int fps = 30;
  int bps = 5000000;
  auto pipe = openService("avLibEncSvc");
  if (!pipe) {
    fprintf(LOGFILE, "Failed to open service\n");
    return 3;
  }

  SingleArray packetData;
  SingleArray frameData(3 * width * height / 2);

  AVCmd cmd;
  cmd.type = AVCmdType::OpenEncoder;
  cmd.init.width    = width;
  cmd.init.height   = height;
  cmd.init.fps      = fps;
  cmd.init.bps      = bps;

  // try open hevc
  if (isHEVC) strcpy(cmd.init.codecName, "hevc_nvenc");
  else strcpy(cmd.init.codecName, "h264_nvenc");
  if (sendAVCmd(pipe, cmd) != AVCmdResult::Ack) {
    isHEVC = false;
    // try open h264
    strcpy(cmd.init.codecName, "h264_nvenc");
    if (sendAVCmd(pipe, cmd) != AVCmdResult::Ack) {
      fprintf(LOGFILE, "Enc service init failed\n");
      closeService(pipe);
      return 3;
    }
  }

  FILE *dumpFile = fopen(testFile.c_str(), "wb");
  if (!dumpFile) {
    fprintf(LOGFILE, "Failed to open test.mp4\n");
    closeService(pipe);
    return 3;
  }

  for (int i = 0; i < 120; i++) {

    auto startTs = std::chrono::system_clock::now();
    int stride = width;
    int plane1Offset = stride * height;
    int plane2Offset = plane1Offset + width * height / 4;
    for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
        frameData[y * stride + x] = x + y + i * 3;
      }
    }

    stride = width / 2;
    // Cb and Cr
    for (int y = 0; y < height/2; y++) {
      for (int x = 0; x < width/2; x++) {
        frameData[plane1Offset + y * stride + x] = 128 + y + i * 2;
        frameData[plane2Offset + y * stride + x] = 64 + x + i * 5;
      }
    }

    auto startTs1 = std::chrono::system_clock::now();

    // Send data for encoding
    cmd.type = AVCmdType::Encode;
    cmd.size = frameData.size();
    if (sendAVCmd(pipe, cmd) != AVCmdResult::Ack) {
      fprintf(LOGFILE, "Encode command got NACK response\n");
    }
    if (pipe->write(frameData.data(), frameData.size()) != frameData.size()) {
      fprintf(LOGFILE, "Encode command failed to send frame data\n");
    }
    if (readAVCmdResult(pipe) != AVCmdResult::Ack) {
      fprintf(LOGFILE, "Encoder failed to encode frame %d\n", i);
    }

    // Get encoded data
    if (getPacket(pipe, packetData) == AVCmdResult::Ack) {
      auto endTs = std::chrono::system_clock::now();
      fprintf(LOGFILE, "Time for preprocess: %0.3f s, Time for encode: %0.3f s, Packet size = %d\n",
            std::chrono::duration<float>(startTs1 - startTs).count(),
            std::chrono::duration<float>(endTs - startTs1).count(),
            (int)packetData.size());

      fwrite(packetData.data(), 1, packetData.size(), dumpFile);
    }
  }

  {
    if (sendAVCmd(pipe, AVCmdType::Flush) != AVCmdResult::Ack) {
      fprintf(LOGFILE, "Encode flush command got NACK response\n");
    }

    while (1) {
      // Get encoded data
      if (getPacket(pipe, packetData) == AVCmdResult::Ack) {
        fprintf(LOGFILE, "Writing flush packet\n");
        fwrite(packetData.data(), 1, packetData.size(), dumpFile);
      } else {
        break;
      }
    }
  }

  fclose(dumpFile);

  return closeService(pipe);
}


int runDecodeTest(bool isHEVC, int testWidth, int testHeight, const std::string &testFile) {
  FILE *dumpFile = fopen(testFile.c_str(), "rb");
  if (!dumpFile) {
    fprintf(LOGFILE, "Failed to open test.mp4\n");
    return 3;
  }

  int width  = testWidth;
  int height = testHeight;
  auto pipe = openService("avLibDecSvc");
  if (!pipe) {
    fclose(dumpFile);
    return 3;
  }

  SingleArray packetData(16 * 1024);
  SingleArray frameData;

  AVCmd cmd;
  cmd.type = AVCmdType::OpenDecoder;
  cmd.init.width    = width;
  cmd.init.height   = height;

  if (isHEVC) strcpy(cmd.init.codecName, "hevc");
  else strcpy(cmd.init.codecName, "h264");
  if (sendAVCmd(pipe, cmd) != AVCmdResult::Ack) {
    fprintf(LOGFILE, "Dec service init failed\n");
    return 3;
  }

  int frameId = 0;
  while (!feof(dumpFile)) {
    cmd.size = fread(packetData.data(), 1, packetData.size(), dumpFile);
    if (cmd.size) {
      // Send data for decoding
      cmd.type = AVCmdType::Decode;
      if (sendAVCmd(pipe, cmd) != AVCmdResult::Ack) {
        fprintf(LOGFILE, "Decode command got NACK response\n");
      }
      if (pipe->write(packetData.data(), cmd.size) != cmd.size) {
        fprintf(LOGFILE, "Decode command failed to send packet data\n");
      }
      if (readAVCmdResult(pipe) != AVCmdResult::Ack) {
        fprintf(LOGFILE, "Decoder failed to decode frame %d\n", frameId);
      }
    }

    // Get decoded data
    while (1) {
      if (getFrame(pipe, frameData) != AVCmdResult::Ack) {
        break;
      }
      fprintf(LOGFILE, "Decoded frame %d\n", (int)frameId);

      std::string name = std::string("frame") + std::to_string(frameId++) + ".raw";
      FILE *fp = fopen(name.c_str(), "wb");
      fwrite(frameData.data(), 1, frameData.size(), fp);
      fclose(fp);
    }
  }
  fclose(dumpFile);

  while (1) {
    sendAVCmd(pipe, AVCmdType::Flush);
    if (getFrame(pipe, frameData) != AVCmdResult::Ack) {
      break;
    }
    fprintf(LOGFILE, "Decoded frame %d\n", (int)frameId);

    std::string name = std::string("frame") + std::to_string(frameId++) + ".raw";
    FILE *fp = fopen(name.c_str(), "wb");
    fwrite(frameData.data(), 1, frameData.size(), fp);
    fclose(fp);
  }

  return closeService(pipe);
}

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE hInstance,
                    HINSTANCE hPrevInstance,
                    PWSTR pCmdLine,
                    int nCmdShow) {
#else
int main(int argc, char **argv) {
#endif
  CLI::App app("libAV Node Service");

  bool isHEVC = false;
  bool testDec = false, testEnc = false;
  int testWidth = 1920, testHeight = 1080;
  std::string testFile;
  std::string instanceId;
  app.add_option("-i", instanceId, "Service instance. Required unless a test is ran");
  app.add_flag  ("-d", testDec, "Run a decoder test");
  app.add_option("-f", testFile, "Test file if decoder test is ran");
  app.add_flag  ("-e", testEnc, "Run an encoder test");
  app.add_option("--width", testWidth, "Test width for encoder test. Default 1920")
    ->check(CLI::PositiveNumber);
  app.add_option("--height", testHeight, "Test height for encoder test. Default 1080")
    ->check(CLI::PositiveNumber);
  app.add_flag("--hevc", isHEVC, "Use HEVC");
  app.add_flag("--log", dumpLog, "Save logs to a file");

#ifdef _WIN32
  char modulePath[1024] = { 0 };
  GetModuleFileName(NULL, modulePath, sizeof(modulePath));
  appPath = modulePath;
  try {
    app.parse(pCmdLine);
  } catch (const CLI::ParseError &e) {
    printf(app.help().c_str());
    return 1;
  }
#else
  appPath = argv[0];
  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    printf(app.help().c_str());
    return 1;
  }
#endif
  if (instanceId.empty() && !testDec && !testEnc) {
    printf(app.help().c_str());
    return 1;
  }
  if ((testDec || testEnc) && testFile.empty()) {
    printf("When running test, specify test file name. See --help.\n");
    return 1;
  }

  if (dumpLog) {
    if (testDec || testEnc) instanceId = "test";
    std::string fname = "libav-node-" + instanceId + ".log";
    freopen(fname.c_str(), "a", stderr);
  }

  Scope exitScope([&]() {
    if (dumpLog) {
      fclose(stderr);
    }
  });

  if (!testDec && !testEnc) {
    AVEnc enc;
    int width, height, fps, bps;

    if (instanceId.empty()) {
      fprintf(LOGFILE, "Invalid instance id\n");
      return 1;
    }

    auto pipe = IIPCPipe::create(instanceId, PIPE_BUFFER_SIZE);
    if (!pipe) {
      return 3;
    }

    fprintf(LOGFILE, "Starting libav-node service, session id %s\n", instanceId.c_str());

    auto encoders = IAVEnc::getEncoders();
    auto decoders = IAVEnc::getDecoders();

    SingleArray packetData;
    DoubleArray frameData;

    auto lastKeepAlive = std::chrono::system_clock::now();
    bool stopService = false;
    while (1) {
      auto keepAliveDur = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - lastKeepAlive);
      if (keepAliveDur.count() > 10) break;

      AVCmd cmd;
      if (!readAVCmd(pipe, &cmd, 200)) {
        continue;
      }
      lastKeepAlive = std::chrono::system_clock::now();

      switch (cmd.type) {
        case AVCmdType::GetEncoderCount: {
          sendAVCmdResult(pipe, AVCmdResult::Ack, encoders.size());
          break;
        }
        case AVCmdType::GetEncoderName: {
          if (cmd.size < encoders.size()) {
            sendAVCmdResult(pipe, AVCmdResult::Ack, encoders[cmd.size].length());
            pipe->write(encoders[cmd.size].c_str(), encoders[cmd.size].length());
          } else {
            sendAVCmdResult(pipe, AVCmdResult::Nack);
          }
          break;
        }
        case AVCmdType::GetDecoderCount: {
          sendAVCmdResult(pipe, AVCmdResult::Ack, decoders.size());
          break;
        }
        case AVCmdType::GetDecoderName: {
          if (cmd.size < decoders.size()) {
            sendAVCmdResult(pipe, AVCmdResult::Ack, decoders[cmd.size].length());
            pipe->write(decoders[cmd.size].c_str(), decoders[cmd.size].length());
          } else {
            sendAVCmdResult(pipe, AVCmdResult::Nack);
          }
          break;
        }
        case AVCmdType::OpenEncoder:
        case AVCmdType::OpenDecoder: {
          std::string codecName = cmd.init.codecName;
          if (cmd.type == AVCmdType::OpenDecoder) enc = IAVEnc::createDecoder(codecName, cmd.init.width, cmd.init.height);
          else enc = IAVEnc::createEncoder(codecName, cmd.init.width, cmd.init.height, cmd.init.fps, cmd.init.bps);
          if (enc) {
            width = cmd.init.width;
            height = cmd.init.height;
            sendAVCmdResult(pipe, AVCmdResult::Ack);
          } else {
            width = height = 0;
            packetData.clear(); packetData.shrink_to_fit();
            frameData.clear(); frameData.shrink_to_fit();
            sendAVCmdResult(pipe, AVCmdResult::Nack);
          }
          break;
        }
        case AVCmdType::Close: {
          enc = nullptr;
          width = height = 0;
          packetData.clear(); packetData.shrink_to_fit();
          frameData.clear(); frameData.shrink_to_fit();
          fprintf(LOGFILE, "Closing encoder/decoder\n");
          sendAVCmdResult(pipe, AVCmdResult::Ack);
          break;
        }
        case AVCmdType::Encode: {
          if (!enc || !enc->isEncoder()) {
            sendAVCmdResult(pipe, AVCmdResult::Nack);
            break;
          } else sendAVCmdResult(pipe, AVCmdResult::Ack);

          frameData.push_back(SingleArray());
          frameData.back().resize(cmd.size);
          packetData.clear();
          if (pipe->read(frameData.back().data(), cmd.size) != cmd.size) {
            sendAVCmdResult(pipe, AVCmdResult::Nack);
            break;
          }

          bool ret = enc->process(&frameData, &packetData);
          if (ret) sendAVCmdResult(pipe, AVCmdResult::Ack);
          else sendAVCmdResult(pipe, AVCmdResult::Nack);
          break;
        }
        case AVCmdType::Decode: {
          if (!enc || enc->isEncoder()) {
            sendAVCmdResult(pipe, AVCmdResult::Nack);
            break;
          } else sendAVCmdResult(pipe, AVCmdResult::Ack);

          packetData.resize(cmd.size);
          if (pipe->read(packetData.data(), cmd.size) != cmd.size) {
            sendAVCmdResult(pipe, AVCmdResult::Nack);
            break;
          }

          bool ret = enc->process(&frameData, &packetData);
          if (ret) sendAVCmdResult(pipe, AVCmdResult::Ack);
          else sendAVCmdResult(pipe, AVCmdResult::Nack);
          break;
        }
        case AVCmdType::GetPacket: {
          if (packetData.size()) {
            sendAVCmdResult(pipe, AVCmdResult::Ack, packetData.size());
            pipe->write(packetData.data(), packetData.size());
            packetData.clear();
          } else {
            sendAVCmdResult(pipe, AVCmdResult::Nack);
          }
          break;
        }
        case AVCmdType::GetFrame: {
          if (frameData.size()) {
            auto &data = frameData.front();
            sendAVCmdResult(pipe, AVCmdResult::Ack, data.size());
            pipe->write(data.data(), data.size());
            frameData.erase(frameData.begin());
          } else {
            sendAVCmdResult(pipe, AVCmdResult::Nack);
          }
          break;
        }
        case AVCmdType::Flush: {
          bool ret = false;
          if (enc->isEncoder()) ret = enc->process(nullptr, &packetData);
          else ret = enc->process(&frameData, nullptr);
          if (ret) sendAVCmdResult(pipe, AVCmdResult::Ack);
          else sendAVCmdResult(pipe, AVCmdResult::Nack);
          break;
        }

        case AVCmdType::StopService: {
          stopService = true;
          enc = nullptr;
          fprintf(LOGFILE, "Stopping service\n");
          sendAVCmdResult(pipe, AVCmdResult::Ack);
          break;
        }
        default: {
          sendAVCmdResult(pipe, AVCmdResult::Nack);
          break;
        }
      }

      if (stopService) {
        break;
      }
    }
    pipe = nullptr;
    fprintf(LOGFILE, "Exit service.\n");
  } else {
    if (testEnc) {
      fprintf(LOGFILE, "Starting encode test\n");
      int ret = runEncodeTest(isHEVC, testWidth, testHeight, testFile);
      if (ret) return ret;

      if (testDec) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      }
    }

    if (testDec) {
      fprintf(LOGFILE, "Starting decode test\n");
      int ret = runDecodeTest(isHEVC, testWidth, testHeight, testFile);
      if (ret) return ret;
    }
  }

  return 0;
}