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
#include "ipc-pipe.h"

#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>
using namespace boost::interprocess;

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
  auto r1 = pipe->read(&res, sizeof(res));
  auto r2 = pipe->read(&tmpSize, sizeof(tmpSize));
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
  if (pipe->read(data.data(), size) != size) {
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
  if (pipe->read(data.data(), size) != size) {
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

  printf("PID of child: %jd\n", (intmax_t) child_pid);
  return true;
#endif
}



IPCPipe openService(int instanceId) {
  std::vector<std::string> params = { std::to_string(instanceId) };
  startProccess(appPath, params);
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));

  auto pipe = IIPCPipe::open("avLibService" + std::to_string(instanceId));
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));

  return pipe;
}

int closeService(IPCPipe pipe) {
  if (sendAVCmd(pipe, AVCmdType::StopService) != AVCmdResult::Ack) {
    printf("Failed to send stop command\n");
    return 3;
  }
  return 0;
}


int runEncodeTest(const char *appPath) {
  FILE *dumpFile = fopen("test.mp4", "wb");
  if (!dumpFile) {
    printf("Failed to open test.mp4\n");
    return 3;
  }

  int width  = 1920;
  int height = 1080;
  int fps = 30;
  int bps = 5000000;
  auto pipe = openService(1);
  if (!pipe) {
    printf("Failed to open service\n");
    fclose(dumpFile);
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
  strcpy(cmd.init.codecName, "hevc_nvenc");
  if (sendAVCmd(pipe, cmd) != AVCmdResult::Ack) {
    printf("Enc service init failed\n");
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
      printf("Encode command got NACK response\n");
    }
    if (pipe->write(frameData.data(), frameData.size()) != frameData.size()) {
      printf("Encode command failed to send frame data\n");
    }
    if (readAVCmdResult(pipe) != AVCmdResult::Ack) {
      printf("Encoder failed to encode frame %d\n", i);
    }

    // Get encoded data
    if (getPacket(pipe, packetData) == AVCmdResult::Ack) {
      auto endTs = std::chrono::system_clock::now();
      printf("Time for preprocess: %0.3f s, Time for encode: %0.3f s, Packet size = %d\n",
            std::chrono::duration<float>(startTs1 - startTs).count(),
            std::chrono::duration<float>(endTs - startTs1).count(),
            (int)packetData.size());

      fwrite(packetData.data(), 1, packetData.size(), dumpFile);
    }
  }

  {
    if (sendAVCmd(pipe, AVCmdType::Flush) != AVCmdResult::Ack) {
      printf("Encode flush command got NACK response\n");
    }

    while (1) {
      // Get encoded data
      if (getPacket(pipe, packetData) == AVCmdResult::Ack) {
        printf("Writing flush packet\n");
        fwrite(packetData.data(), 1, packetData.size(), dumpFile);
      } else {
        break;
      }
    }
  }

  fclose(dumpFile);

  return closeService(pipe);
}


int runDecodeTest(const char *appPath) {
  FILE *dumpFile = fopen("test.mp4", "rb");
  if (!dumpFile) {
    printf("Failed to open test.mp4\n");
    return 3;
  }

  int width  = 1920;
  int height = 1080;
  auto pipe = openService(1);
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
  strcpy(cmd.init.codecName, "hevc");
  if (sendAVCmd(pipe, cmd) != AVCmdResult::Ack) {
    printf("Dec service init failed\n");
    return 3;
  }

  int frameId = 0;
  while (!feof(dumpFile)) {
    cmd.size = fread(packetData.data(), 1, packetData.size(), dumpFile);
    if (cmd.size) {
      // Send data for decoding
      cmd.type = AVCmdType::Decode;
      if (sendAVCmd(pipe, cmd) != AVCmdResult::Ack) {
        printf("Decode command got NACK response\n");
      }
      if (pipe->write(packetData.data(), cmd.size) != cmd.size) {
        printf("Decode command failed to send packet data\n");
      }
      if (readAVCmdResult(pipe) != AVCmdResult::Ack) {
        printf("Decoder failed to decode frame %d\n", frameId);
      }
    }

    // Get decoded data
    while (1) {
      if (getFrame(pipe, frameData) != AVCmdResult::Ack) {
        break;
      }
      printf("Decoded frame %d\n", (int)frameId);

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
    printf("Decoded frame %d\n", (int)frameId);

    std::string name = std::string("frame") + std::to_string(frameId++) + ".raw";
    FILE *fp = fopen(name.c_str(), "wb");
    fwrite(frameData.data(), 1, frameData.size(), fp);
    fclose(fp);
  }

  return closeService(pipe);
}

void printUsage(const char *appPath) {
  const char *pn = appPath;
  auto tmp = pn;
  while (tmp = strstr(tmp + 1, "\\")) pn = tmp + 1;
  printf("Insufficient arguments.\n"
          "  Usage: %s [test | <instanceId>]\n", pn);
}

int main(int argc, char **argv) {
  appPath = argv[0];
  if (argc < 2) {
    printUsage(argv[0]);
    return 1;
  }

  bool isTest = !strcmp(argv[1], "test");
  if (!isTest) {
    AVEnc enc;
    int width, height, fps, bps;

    int instanceId = -1;
    if (sscanf(argv[1], "%d", &instanceId) != 1 || instanceId < 0) {
      printf("Invalid instance id: %s\n", argv[1]);
      return 1;
    }

    auto pipe = IIPCPipe::create("avLibService" + std::to_string(instanceId), PIPE_BUFFER_SIZE);
    if (!pipe) {
      return 3;
    }

    printf("Starting libav-node service, session id %d\n", instanceId);

    auto encoders = IAVEnc::getEncoders();
    auto decoders = IAVEnc::getDecoders();
    printf("Encoders\t|\tDecoders\n");
    for (size_t i = 0; i < std::max(encoders.size(), decoders.size()); i++) {
      if (i < encoders.size()) printf("%s\t|\t", encoders[i].c_str());
      else printf("\t\t|\t");
      if (i < decoders.size()) printf("%s\n", decoders[i].c_str());
      else printf("\t\n");
    }


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
          std::string codecName;
          if (cmd.type == AVCmdType::OpenDecoder) {
            for (auto &c : decoders) if (c.find(cmd.init.codecName) == 0) { codecName = c; break; }
          } else {
            for (auto &c : encoders) if (c.find(cmd.init.codecName) == 0) { codecName = c; break; }
          }

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
  } else {
    printf("Starting encode test\n");
    int ret = runEncodeTest(argv[0]);
    if (ret) return ret;

    std::this_thread::sleep_for(std::chrono::milliseconds(4000));
    printf("Starting decode test\n");
    return runDecodeTest(argv[0]);
  }
  printf("Exit service.\n");
  return 0;
}