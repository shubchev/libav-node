#include "ipc-pipe.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <Windows.h>
#else

#endif


class IPCPipeImpl : public IIPCPipe {
public:
  IPCPipeImpl() {
  }
  ~IPCPipeImpl() {
#ifdef _WIN32
    if (hPipe != INVALID_HANDLE_VALUE) CloseHandle(hPipe);
#else
#endif
  }

  size_t write(const void *data, size_t size) override {
    if (!data || !size) {
      return 0;
    }

    auto ptr = (const uint8_t *)data;
    int retry = 10;
    size_t totalBytes = 0;

#ifdef _WIN32
    DWORD bytesWritten = 0;
    while (totalBytes < size && retry > 0) {
      if (!WriteFile(hPipe, &ptr[bytesWritten], size - bytesWritten, &bytesWritten, NULL)) {
        retry--;
      } else {
        totalBytes += bytesWritten;
      }
    }
    return totalBytes;
#else

#endif
  }

  size_t read(void *data, size_t size, int timeoutMs) override {
    if (!data || !size) {
      return 0;
    }

    auto ptr = (uint8_t *)data;
    size_t totalBytes = 0;

#ifdef _WIN32
    DWORD bytesRead = 0;
    auto start = std::chrono::high_resolution_clock::now();
    while (totalBytes < size) {
      if (!ReadFile(hPipe, &ptr[bytesRead], size - bytesRead, &bytesRead, NULL)) {
        auto end = std::chrono::high_resolution_clock::now();
        if (timeoutMs >= 0 && std::chrono::duration<double>(end - start).count() > 0.001 * timeoutMs) {
          return totalBytes;
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      } else {
        totalBytes += bytesRead;
      }
    }
    return totalBytes;
#else

#endif
  }


#ifdef _WIN32
  HANDLE hPipe = INVALID_HANDLE_VALUE;
#else

#endif
};


IPCPipe IIPCPipe::create(const std::string &name, size_t bufferStorageSize) try {
  auto ipc = std::make_shared<IPCPipeImpl>();
  if (!ipc) {
    return nullptr;
  }


#ifdef _WIN32
  std::string pipeName = "\\\\.\\pipe\\" + name;
  ipc->hPipe = CreateNamedPipe(pipeName.c_str(), PIPE_ACCESS_DUPLEX,
                               PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                               1, bufferStorageSize, bufferStorageSize, 0, NULL);
  if (ipc->hPipe == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "Could not create pipe. Error %d\n", GetLastError());
    return nullptr;
  }
#else

#endif

  return ipc;
} catch (std::exception &e) {
  fprintf(stderr, "IPC-PIPE: Create error: %s\n", e.what());
  return nullptr;
}

IPCPipe IIPCPipe::open(const std::string &name) try {
  auto ipc = std::make_shared<IPCPipeImpl>();
  if (!ipc) {
    return nullptr;
  }

#ifdef _WIN32
  while (1) {
    std::string pipeName = "\\\\.\\pipe\\" + name;
    ipc->hPipe = CreateFile(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (ipc->hPipe != INVALID_HANDLE_VALUE) {
      break;
    }

    if (GetLastError() != ERROR_PIPE_BUSY) {
      fprintf(stderr, "Could not open pipe. Error %d\n", GetLastError());
      return nullptr;
    }

    // All pipe instances are busy, so wait for 20 seconds. 

    if (!WaitNamedPipe(name.c_str(), 20000)) {
      fprintf(stderr, "Could not open pipe: 20 second wait timed out.");
      return nullptr;
    }
  }
#else

#endif

  return ipc;
} catch (std::exception &e) {
  fprintf(stderr, "IPC-PIPE: Open error: %s\n", e.what());
  return nullptr;
}