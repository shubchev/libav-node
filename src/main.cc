#include "libav_service.h"
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

#define MESSAGE_BUFFER_SIZE (100 * 1024)
#define PIPE_BUFFER_SIZE (32 * 1024 * 1024)

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



IPCPipe openService(bool isEnc, int width, int height, int fps, int bps, int instanceId) {
  std::vector<std::string> params;
  if (isEnc) {
    params.push_back("enc");
    params.push_back(std::to_string(instanceId));
    params.push_back(std::to_string(width));
    params.push_back(std::to_string(height));
    params.push_back(std::to_string(fps));
    params.push_back(std::to_string(bps));
  } else {
    params.push_back("dec");
    params.push_back(std::to_string(instanceId));
    params.push_back(std::to_string(width));
    params.push_back(std::to_string(height));
  }
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
  auto pipe = openService(true, width, height, 30, 5000000, 1);
  if (!pipe) {
    printf("Failed to open service\n");
    fclose(dumpFile);
    return 3;
  }

  auto frameData = (uint8_t *)pipe->getBuffer(0, width * height * 3);
  auto packetData = (uint8_t *)pipe->getBuffer(width * height * 3, 1024 * 1024);

  AVCmd cmd;
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

    cmd.type = AVCmdType::EncodeFrame;
    cmd.eframe.size = width * height * 3 / 2;
    if (pipe->write(&cmd, sizeof(cmd)) != sizeof(cmd)) {
      printf("Failed to send encode command %d\n", i);
    }
    if (pipe->read(&cmd, sizeof(cmd)) != sizeof(cmd)) {
      printf("Failed to get encode response %d\n", i);
    }
    if (cmd.type != AVCmdType::Ack) {
      printf("Encode command got response: %d\n", (int)cmd.type);
    }
    auto endTs = std::chrono::system_clock::now();
    printf("Time for preprocess: %0.3f s\nTime for encode: %0.3f s\n",
            std::chrono::duration<float>(startTs1 - startTs).count(),
            std::chrono::duration<float>(endTs - startTs1).count());

    if (cmd.eframe.size) {
      fwrite(packetData, 1, cmd.eframe.size, dumpFile);
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
  auto pipe = openService(false, width, height, 0, 0, 1);
  if (!pipe) {
    fclose(dumpFile);
    return 3;
  }

  auto packetData = (uint8_t *)pipe->getBuffer(0, 1024 * 1024);
  auto frameData = (uint8_t *)pipe->getBuffer(1024 * 1024, width * height * 3);

  AVCmd cmd;
  cmd.dframe.size = 0;
  size_t lastRead;
  int frameId = 0;
  while (!feof(dumpFile)) {
    if (cmd.dframe.size) memmove(packetData, &packetData[lastRead - cmd.dframe.size], cmd.dframe.size);
    cmd.dframe.size += fread(&packetData[cmd.dframe.size], 1, 16 * 1024 - cmd.dframe.size, dumpFile);
    lastRead = cmd.dframe.size;
    cmd.type = AVCmdType::DecodeFrame;
    if (cmd.dframe.size) {
      if (pipe->write(&cmd, sizeof(cmd)) != sizeof(cmd)) {
        printf("Failed to send decode command\n");
      }
      pipe->read(&cmd, sizeof(cmd));
    }

    if (cmd.type == AVCmdType::Ack) {
      std::string name = std::string("frame") + std::to_string(frameId++) + ".raw";
      FILE *fp = fopen(name.c_str(), "wb");
      fwrite(frameData, 1, width * height * 3, fp);
      fclose(fp);
    }
      printf("Decode resp %d %d %d %d\n", (int)cmd.type, cmd.dframe.size, lastRead, frameId);
  }
  fclose(dumpFile);

  return closeService(pipe);
}

void printUsage(const char *appPath) {
  const char *pn = appPath;
  auto tmp = pn;
  while (tmp = strstr(tmp + 1, "\\")) pn = tmp + 1;
  printf("Insufficient arguments.\n"
          "  Usage: %s [test |"
                      "enc <instanceId> <width> <height> <fps> <bps> |"
                      "dec <instanceId> <width> <height>\n", pn);
}

int main(int argc, char **argv) {
  appPath = argv[0];
  if (argc < 2) {
    printUsage(argv[0]);
    return 1;
  }

  if (!strcmp(argv[1], "enc")) {
    if (argc < 7) {
      printUsage(argv[0]);
      return 1;
    }
    int instanceId  = atoi(argv[2]);
    int width       = atoi(argv[3]);
    int height      = atoi(argv[4]);
    int fps         = atoi(argv[5]);
    int bps         = atoi(argv[6]);

    auto enc = IAVEnc::create(width, height, fps, bps, instanceId);
    if (!enc) {
      return 2;
    }

    auto pipe = IIPCPipe::create("avLibService" + std::to_string(instanceId), MESSAGE_BUFFER_SIZE, PIPE_BUFFER_SIZE);
    if (!pipe) {
      return 3;
    }

    auto frameData = pipe->getBuffer(0, width * height * 3);
    auto packetData = pipe->getBuffer(width * height * 3, 1024 * 1024);

    AVCmd cmd;
    auto lastKeepAlive = std::chrono::system_clock::now();
    while (enc) {
      auto keepAliveDur = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - lastKeepAlive);
      if (keepAliveDur.count() > 10) break;

      if (pipe->read(&cmd, sizeof(cmd), 200) != sizeof(cmd)) {
        continue;
      }
      lastKeepAlive = std::chrono::system_clock::now();

      switch (cmd.type) {
        case AVCmdType::StopService: {
          enc = nullptr;
          cmd.type = AVCmdType::Ack;
          break;
        }
        case AVCmdType::EncodeFrame: {
          cmd.eframe.size = enc->encode(frameData, packetData);
          cmd.type = AVCmdType::Ack;
          break;
        }
        default: {
          cmd.type = AVCmdType::Nack;
          break;
        }
      }
      pipe->write(&cmd, sizeof(cmd));
    }
    pipe = nullptr;

  } else if (!strcmp(argv[1], "dec")) {
    if (argc < 5) {
      printUsage(argv[0]);
      return 1;
    }
    int instanceId  = atoi(argv[2]);
    int width       = atoi(argv[3]);
    int height      = atoi(argv[4]);

    auto dec = IAVDec::create(width, height, instanceId);
    if (!dec) {
      return 2;
    }

    auto pipe = IIPCPipe::create("avLibService" + std::to_string(instanceId), MESSAGE_BUFFER_SIZE, PIPE_BUFFER_SIZE);
    if (!pipe) {
      return 3;
    }

    auto packetData = pipe->getBuffer(0, 1024 * 1024);
    auto frameData = pipe->getBuffer(1024 * 1024, width * height * 3);

    auto lastKeepAlive = std::chrono::system_clock::now();
    while (dec) {
      auto keepAliveDur = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - lastKeepAlive);
      if (keepAliveDur.count() > 10) break;

      AVCmd cmd;
      if (pipe->read(&cmd, sizeof(cmd), 200) != sizeof(cmd)) {
        continue;
      }
      lastKeepAlive = std::chrono::system_clock::now();

      switch (cmd.type) {
        case AVCmdType::StopService: {
          dec = nullptr;
          cmd.type = AVCmdType::Ack;
          break;
        }
        case AVCmdType::DecodeFrame: {
          if (dec->decode(packetData, cmd.eframe.size, frameData)) cmd.type = AVCmdType::Ack;
          else cmd.type = AVCmdType::Nack;
          break;
        }
        default: {
          cmd.type = AVCmdType::Nack;
          break;
        }
      }
      pipe->write(&cmd, sizeof(cmd));
    }
    pipe = nullptr;
  } else if (!strcmp(argv[1], "test")) {
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