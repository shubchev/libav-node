#include "libav_service.h"
#include "av.h"
#include <chrono>
#include <string>
#include <sstream>
#include <thread>
#include "ipc-pipe.h"

#ifdef WIN32
  #include <Windows.h>
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

#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>
using namespace boost::interprocess;

const char *appPath = nullptr;


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

  auto pipe = IIPCPipe::open("avLibService" + std::to_string(instanceId), MESSAGE_BUFFER_SIZE, PIPE_BUFFER_SIZE);
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));

  return pipe;
}

int closeService(IPCPipe pipe) {
  AVCmd cmd;
  cmd.type = AVCmdType::StopService;
  if (pipe->write(&cmd, sizeof(cmd)) != sizeof(cmd)) {
    printf("Failed to send stop command\n");
    return 3;
  }
  pipe->read(&cmd, sizeof(cmd));
  if (cmd.type != AVCmdType::Ack) {
    printf("Stop command got response: %d\n", (int)cmd.type);
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

  int bufferSize = 3 * width * height;
  int nBuffers = PIPE_BUFFER_SIZE / bufferSize;
  auto packetData = (uint8_t *)pipe->getBuffer(             0,              bufferSize);
  auto frameData  = (uint8_t *)pipe->getBuffer(bufferSize, (nBuffers - 1) * bufferSize);

  AVCmd cmd;
  cmd.type = AVCmdType::OpenEncoder;
  cmd.init.width    = width;
  cmd.init.height   = height;
  cmd.init.fps      = fps;
  cmd.init.bps      = bps;
  strcpy(cmd.init.codecName, "hevc_nvenc");
  if (pipe->write(&cmd, sizeof(cmd)) != sizeof(cmd)) {
    printf("Enc service init failed\n");
    return 3;
  }

  pipe->read(&cmd, sizeof(cmd));
  if (cmd.type != AVCmdType::Ack) {
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

    cmd.type = AVCmdType::Process;
    cmd.info.size = width * height * 3 / 2;
    cmd.info.bufferIndex = 0;
    pipe->write(&cmd, sizeof(cmd));
    pipe->read(&cmd, sizeof(cmd));
    if (cmd.type != AVCmdType::Ack) {
      printf("Encode command got response: %d\n", (int)cmd.type);
    }

    auto endTs = std::chrono::system_clock::now();
    printf("Time for preprocess: %0.3f s, Time for encode: %0.3f s, Packet size = %d\n",
            std::chrono::duration<float>(startTs1 - startTs).count(),
            std::chrono::duration<float>(endTs - startTs1).count(),
            (int)cmd.info.size);

    if (cmd.info.size) {
      fwrite(packetData, 1, cmd.info.size, dumpFile);
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

  int bufferSize = 3 * width * height;
  int nBuffers = PIPE_BUFFER_SIZE / bufferSize;
  auto packetData = (uint8_t *)pipe->getBuffer(             0,              bufferSize);
  auto frameData  = (uint8_t *)pipe->getBuffer(bufferSize, (nBuffers - 1) * bufferSize);

  AVCmd cmd;
  cmd.type = AVCmdType::OpenDecoder;
  cmd.init.width    = width;
  cmd.init.height   = height;
  strcpy(cmd.init.codecName, "hevc");
  if (pipe->write(&cmd, sizeof(cmd)) != sizeof(cmd)) {
    printf("Enc service init failed\n");
    return 3;
  }

  pipe->read(&cmd, sizeof(cmd));
  if (cmd.type != AVCmdType::Ack) {
    printf("Enc service init failed\n");
    return 3;
  }


  size_t lastRead, remainingBytes = 0;
  int frameId = 0;
  while (!feof(dumpFile)) {
    if (remainingBytes) memmove(packetData, &packetData[lastRead - remainingBytes], remainingBytes);
    remainingBytes += fread(&packetData[remainingBytes], 1, 16 * 1024 - remainingBytes, dumpFile);
    lastRead = remainingBytes;
    cmd.type = AVCmdType::Process;
    if (remainingBytes) {
      cmd.info.size = remainingBytes;
      if (pipe->write(&cmd, sizeof(cmd)) != sizeof(cmd)) {
        printf("Failed to send decode command\n");
      }
      pipe->read(&cmd, sizeof(cmd));
      remainingBytes = cmd.info.size;
    }

    while (1) {
      cmd.type = AVCmdType::HasFrame;
      if (pipe->write(&cmd, sizeof(cmd)) != sizeof(cmd)) {
        printf("Failed to send frame check command\n");
        break;
      }
      pipe->read(&cmd, sizeof(cmd));
      if (cmd.type == AVCmdType::Ack) {
        std::string name = std::string("frame") + std::to_string(frameId++) + ".raw";
        FILE *fp = fopen(name.c_str(), "wb");
        fwrite(&frameData[cmd.info.bufferIndex * 3 * width * height], 1, width * height * 3, fp);
        fclose(fp);
      } else {
        break;
      }
    }
    printf("Decode resp %d %d %d %d\n", (int)cmd.type, (int)remainingBytes, (int)lastRead, (int)frameId);  
  }
  fclose(dumpFile);
  while (1) {
    cmd.type = AVCmdType::Flush;
    if (pipe->write(&cmd, sizeof(cmd)) != sizeof(cmd)) {
      printf("Failed to send flush command\n");
      break;
    }
    pipe->read(&cmd, sizeof(cmd));
    if (cmd.type == AVCmdType::Ack) {
      std::string name = std::string("frame") + std::to_string(frameId++) + ".raw";
      FILE *fp = fopen(name.c_str(), "wb");
      fwrite(&frameData[cmd.info.bufferIndex * 3 * width * height], 1, width * height * 3, fp);
      fclose(fp);
    } else {
      break;
    }
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
    int readBuffer = 0, writeBuffer = 0;

    int instanceId = -1;
    if (sscanf(argv[1], "%d", &instanceId) != 1 || instanceId < 0) {
      printf("Invalid instance id: %s\n", argv[1]);
      return 1;
    }

    auto pipe = IIPCPipe::create("avLibService" + std::to_string(instanceId), MESSAGE_BUFFER_SIZE, PIPE_BUFFER_SIZE);
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


    int bufferSize = 0, nBuffers = 0;
    uint8_t *packetData = nullptr, *frameData = nullptr;

    AVCmd cmd;
    cmd.type = AVCmdType::Ack;

    auto lastKeepAlive = std::chrono::system_clock::now();
    bool stopService = false;
    while (1) {
      auto keepAliveDur = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - lastKeepAlive);
      if (keepAliveDur.count() > 10) break;

      if (pipe->read(&cmd, sizeof(cmd), 200) != sizeof(cmd)) {
        continue;
      }
      lastKeepAlive = std::chrono::system_clock::now();

      switch (cmd.type) {
        case AVCmdType::GetEncoderCount: {
          cmd.type = AVCmdType::Ack;
          cmd.info.size = encoders.size();
          break;
        }
        case AVCmdType::GetEncoderName: {
          if (cmd.info.bufferIndex < encoders.size()) {
            cmd.type = AVCmdType::Ack;
            snprintf(cmd.init.codecName, sizeof(cmd.init.codecName), "%s", encoders[cmd.info.bufferIndex].c_str());
          } else cmd.type = AVCmdType::Nack;
          break;
        }
        case AVCmdType::GetDecoderCount: {
          cmd.type = AVCmdType::Ack;
          cmd.info.size = decoders.size();
          break;
        }
        case AVCmdType::GetDecoderName: {
          if (cmd.info.bufferIndex < decoders.size()) {
            cmd.type = AVCmdType::Ack;
            snprintf(cmd.init.codecName, sizeof(cmd.init.codecName), "%s", decoders[cmd.info.bufferIndex].c_str());
          } else cmd.type = AVCmdType::Nack;
          break;
        }
        case AVCmdType::OpenEncoder:
        case AVCmdType::OpenDecoder: {
          bufferSize = 3 * cmd.init.width * cmd.init.height;
          nBuffers = PIPE_BUFFER_SIZE / bufferSize;
          if (nBuffers < 2) {
            cmd.type = AVCmdType::Nack;
            break;
          }

          std::string codecName;
          if (cmd.type == AVCmdType::OpenDecoder) {
            for (auto &c : decoders) if (c.find(cmd.init.codecName) == 0) { codecName = c; break; }
          } else {
            for (auto &c : encoders) if (c.find(cmd.init.codecName) == 0) { codecName = c; break; }
          }

          writeBuffer = readBuffer = 0;
          packetData = (uint8_t *)pipe->getBuffer(             0,              bufferSize);
          frameData  = (uint8_t *)pipe->getBuffer(bufferSize, (nBuffers - 1) * bufferSize);

          if (cmd.type == AVCmdType::OpenDecoder) enc = IAVEnc::createDecoder(codecName, cmd.init.width, cmd.init.height);
          else enc = IAVEnc::createEncoder(codecName, cmd.init.width, cmd.init.height, cmd.init.fps, cmd.init.bps);
          if (enc) {
            cmd.type = AVCmdType::Ack;
            width = cmd.init.width;
            height = cmd.init.height;
          } else {
            cmd.type = AVCmdType::Nack;
            width = height = 0;
            packetData = frameData = nullptr;
          }
          break;
        }
        case AVCmdType::Close: {
          enc = nullptr;
          break;
        }
        case AVCmdType::Process: {
          bool ret;
          if (enc->isEncoder()) ret = enc->process(&frameData[cmd.info.bufferIndex * bufferSize], packetData, cmd.info.size);
          else ret = enc->process(&frameData[writeBuffer * bufferSize], packetData, cmd.info.size);
          if (ret) {
            cmd.type = AVCmdType::Ack;
            if (!enc->isEncoder()) writeBuffer = (writeBuffer + 1) % (nBuffers - 1);
          } else {
            cmd.type = AVCmdType::Nack;
          }
          break;
        }
        case AVCmdType::HasFrame: {
          if (readBuffer != writeBuffer) {
            cmd.info.bufferIndex = readBuffer;
            cmd.type = AVCmdType::Ack;
            readBuffer = (readBuffer + 1) % (nBuffers - 1);
          } else {
            cmd.type = AVCmdType::Nack;
          }
          break;
        }
        case AVCmdType::Flush: {
          size_t size = 0;
          bool ret = false;
          if (enc->isEncoder()) break;
          else ret = enc->process(frameData, nullptr, size);
          if (ret) {
            cmd.info.bufferIndex = 0;
            cmd.type = AVCmdType::Ack;
          } else {
            cmd.type = AVCmdType::Nack;
          }
          break;
        }

        case AVCmdType::StopService: {
          stopService = true;
          enc = nullptr;
          cmd.type = AVCmdType::Ack;
          break;
        }
        default: {
          cmd.type = AVCmdType::Nack;
          break;
        }
      }
      pipe->write(&cmd, sizeof(cmd));

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