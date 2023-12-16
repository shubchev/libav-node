#include "libav_service.h"
#include <chrono>
#include <string>
#include <sstream>
#include <thread>
#include <Windows.h>

#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>
using namespace boost::interprocess;

#define PIPE_BUFFER_SIZE 1024
const char *appPath = nullptr;

shared_memory_object shm;
mapped_region frameMapRegion;
mapped_region packetMapRegion;
uint8_t *frameData = nullptr;
uint8_t *packetData = nullptr;

HANDLE createPipe(int instanceId) {
  std::string memName = "avLibService" + std::to_string(instanceId);
  std::string pipeName = "\\\\.\\pipe\\" + memName;

  auto hPipe = CreateNamedPipe(pipeName.c_str(),
                               PIPE_ACCESS_DUPLEX,
                               PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS,
                               1, PIPE_BUFFER_SIZE, PIPE_BUFFER_SIZE, 0, NULL);
  if (hPipe == INVALID_HANDLE_VALUE) {
    return hPipe;
  }

  return hPipe;
}

HANDLE openPipe(int instanceId, offset_t frameSize, offset_t packetSize) {
  std::string memName = "avLibService" + std::to_string(instanceId);
  std::string pipeName = "\\\\.\\pipe\\" + memName;

  HANDLE hPipe = INVALID_HANDLE_VALUE;
  while (1) {
    hPipe = CreateFile(pipeName.c_str(), GENERIC_WRITE | GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hPipe != INVALID_HANDLE_VALUE) {
      break;
    }

    if (GetLastError() != ERROR_PIPE_BUSY) {
      return INVALID_HANDLE_VALUE;
    }
 
    if (!WaitNamedPipe(pipeName.c_str(), 5000)) {
      return INVALID_HANDLE_VALUE;
    }
  }

  shm = shared_memory_object(open_only, memName.c_str(), read_write);

  frameMapRegion = mapped_region(shm, read_write, 0, frameSize);
  packetMapRegion = mapped_region(shm, read_write, frameSize, packetSize);

  frameData  = (uint8_t *)frameMapRegion.get_address();
  packetData = (uint8_t *)packetMapRegion.get_address();

  if (!frameData || !packetData) {
    CloseHandle(hPipe);
    return INVALID_HANDLE_VALUE;
  }

  return hPipe;
}

bool writePipe(HANDLE pipe, const void *data, size_t size) {
  if (!data || !size) return false;
  auto ptr = (const uint8_t *)data;
  DWORD bytesWritten = 0;
  int retry = 100;
  while (bytesWritten < size && retry > 0) {
    DWORD bytesSent = 0;
    if (!WriteFile(pipe, &ptr[bytesWritten], size - bytesWritten, &bytesSent, NULL)) {
      retry--;
    } else {
      bytesWritten += bytesSent;
    }
  }
  return bytesWritten == size;
}

bool readPipe(HANDLE pipe, void *data, size_t size) {
  if (!data || !size) return false;
  auto ptr = (uint8_t *)data;
  DWORD bytesRead = 0;
  int retry = 100;
  while (bytesRead < size && retry > 0) {
    DWORD bytesRecv = 0;
    //if (!ReadFile(hPipeRead, &ptr[bytesRead], size - bytesRead, &bytesRecv, NULL)) {
    if (!ReadFile(pipe, &ptr[bytesRead], size - bytesRead, &bytesRecv, NULL)) {
      retry--;
    } else {
      bytesRead += bytesRecv;
    }
  }
  return bytesRead == size;
}




HANDLE openService(bool isEnc, int width, int height, int fps, int bps, int instanceId) {
  std::stringstream params;
  if (isEnc) {
    params << "enc " << instanceId << " " << width << " " << height << " " << fps << " " << bps;
  } else {
    params << "dec " << instanceId << " " << width << " " << height;
  }
  ShellExecute(NULL, "open", appPath, params.str().c_str(), NULL, SW_SHOW);
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));

  return openPipe(instanceId, width * height * 3, 1000000);
}

int closeService(HANDLE pipe) {
  AVCmd cmd;
  cmd.type = AVCmdType::StopService;
  if (!writePipe(pipe, &cmd, sizeof(cmd))) {
    printf("Failed to send stop command\n");
    return 3;
  }
  readPipe(pipe, &cmd, sizeof(cmd));
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
  if (pipe == INVALID_HANDLE_VALUE) {
    fclose(dumpFile);
    return 3;
  }


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
    if (!writePipe(pipe, &cmd, sizeof(cmd))) {
      printf("Failed to send encode command %d\n", i);
    }
    readPipe(pipe, &cmd, sizeof(cmd));
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
  if (pipe == INVALID_HANDLE_VALUE) {
    fclose(dumpFile);
    return 3;
  }

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
      if (!writePipe(pipe, &cmd, sizeof(cmd))) {
        printf("Failed to send decode command\n");
      }
      readPipe(pipe, &cmd, sizeof(cmd));
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

    auto pipe = createPipe(instanceId);
    if (pipe == INVALID_HANDLE_VALUE) {
      return 3;
    }

    auto lastKeepAlive = std::chrono::system_clock::now();
    while (enc) {
      auto keepAliveDur = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - lastKeepAlive);
      if (keepAliveDur.count() > 10) break;

      AVCmd cmd;
      if (!readPipe(pipe, &cmd, sizeof(cmd))) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
          cmd.eframe.size = enc->encode();
          cmd.type = AVCmdType::Ack;
          break;
        }
        default: {
          cmd.type = AVCmdType::Nack;
          break;
        }
      }
      writePipe(pipe, &cmd, sizeof(cmd));
    }
    CloseHandle(pipe);

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

    auto pipe = createPipe(instanceId);
    if (pipe == INVALID_HANDLE_VALUE) {
      return 3;
    }

    auto lastKeepAlive = std::chrono::system_clock::now();
    while (dec) {
      auto keepAliveDur = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - lastKeepAlive);
      //if (keepAliveDur.count() > 10) break;

      AVCmd cmd;
      if (!readPipe(pipe, &cmd, sizeof(cmd))) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
          if (dec->decode(cmd.eframe.size)) cmd.type = AVCmdType::Ack;
          else cmd.type = AVCmdType::Nack;
          break;
        }
        default: {
          cmd.type = AVCmdType::Nack;
          break;
        }
      }
      writePipe(pipe, &cmd, sizeof(cmd));
    }
    CloseHandle(pipe);
  } else if (!strcmp(argv[1], "test")) {
    /*printf("Starting encode test\n");
    int ret = runEncodeTest(argv[0]);
    if (ret) return ret;

    std::this_thread::sleep_for(std::chrono::milliseconds(4000));*/
    printf("Starting decode test\n");
    return runDecodeTest(argv[0]);
  }
  printf("Exit service.\n");
  return 0;
}