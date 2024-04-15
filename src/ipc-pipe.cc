#include <plog/Log.h>
#include "ipc-pipe.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <thread>

#ifdef _WIN32
#include <Windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#define ACCESS_MODE (S_IRWXU | S_IRWXG | S_IRWXO)
#endif

extern FILE *LOGFILE;
#ifdef _WIN32
#undef errno
#define errno GetLastError()
#endif

class IPCPipeImpl : public IIPCPipe {
public:
  IPCPipeImpl() {
  }
  ~IPCPipeImpl() {
    close();
  }

  void close() {
#ifdef _WIN32
    if (hPipe != INVALID_HANDLE_VALUE) CloseHandle(hPipe);
    hPipe = INVALID_HANDLE_VALUE;
#else
    if (hPipe >= 0) ::close(hPipe);
    hPipe = -1;
    if (hClient >= 0) ::close(hClient);
    hClient = -1;
    if (pipeName.length()) ::unlink(pipeName.c_str());
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
      if (!WriteFile(hPipe, &ptr[totalBytes], size - totalBytes, &bytesWritten, NULL)) {
        close();
        return 0;
      } else {
        totalBytes += bytesWritten;
      }
    }
#else
    int ret;
    while (totalBytes < size && retry > 0) {
      ret = ::write(hClient, &ptr[totalBytes], size - totalBytes);
      if (ret <= 0) {
        close();
        return 0;
      } else {
        totalBytes += ret;
      }
    }
#endif
    return totalBytes;
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
      if (!ReadFile(hPipe, &ptr[totalBytes], size - totalBytes, &bytesRead, NULL)) {
        auto end = std::chrono::high_resolution_clock::now();
        if (timeoutMs >= 0 && std::chrono::duration<double>(end - start).count() > 0.001 * timeoutMs) {
          return totalBytes;
        } else {
          auto err = GetLastError();
          if (err != ERROR_SUCCESS && err != ERROR_PIPE_LISTENING) {
            close();
          }
          return 0;
        }
      } else {
        totalBytes += bytesRead;
      }
    }
#else
    int ret;
    struct pollfd fds;
    while (totalBytes < size) {
      fds.fd = hClient;
      fds.events = POLLIN;
      fds.revents = 0;
      ret = ::poll(&fds, 1, timeoutMs);
      if (ret < 0) {
        close();
        return 0;
      } else if (ret == 0) {
        return totalBytes;
      } else if (fds.revents & POLLIN) {
        ret = ::read(hClient, &ptr[totalBytes], size - totalBytes);
        if (ret <= 0) {
          close();
          return 0;
        }
        totalBytes += ret;
      }
    }
#endif
    return totalBytes;
  }

  bool isOpen() const {
#ifdef _WIN32
    return INVALID_HANDLE_VALUE != hPipe;
#else
    return -1 != hPipe && -1 != hClient;
#endif
  }

#ifdef _WIN32
  HANDLE hPipe = INVALID_HANDLE_VALUE;
#else
  int hPipe = -1;
  int hClient = -1;
  std::string pipeName;
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
    LOG_ERROR << "[IPC] Could not create pipe. Error " << errno;
    return nullptr;
  }
#else
  ipc->pipeName = "/tmp/" + name;
  ::unlink(ipc->pipeName.c_str());
  ipc->hPipe = socket(AF_UNIX, SOCK_STREAM, 0);
  if (ipc->hPipe == -1) {
    LOG_ERROR << "[IPC] Could not create pipe. Error " << errno;
    return nullptr;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, ipc->pipeName.c_str(), sizeof(addr.sun_path) - 1);

  int ret = bind(ipc->hPipe, (const struct sockaddr *)&addr, sizeof(addr));
  if (ret == -1) {
    LOG_ERROR << "[IPC] Could not bind pipe. Error " << errno;
    return nullptr;
  }

  ret = listen(ipc->hPipe, 1);
  if (ret == -1) {
    LOG_ERROR << "[IPC] Could not listen pipe. Error " << errno;
    return nullptr;
  }

  ipc->hClient = accept(ipc->hPipe, NULL, NULL);
  if (ipc->hClient == -1) {
    LOG_ERROR << "[IPC] Failed to accept pipe client. Error " << errno;
    return nullptr;
  }
#endif

  return ipc;
} catch (std::exception &e) {
  LOG_ERROR << "[IPC] Create error: " << e.what();
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
      LOG_ERROR << "[IPC] Could not open pipe. Error " << errno;
      return nullptr;
    }

    // All pipe instances are busy, so wait for 20 seconds. 

    if (!WaitNamedPipe(name.c_str(), 20000)) {
      LOG_ERROR << "[IPC] Could not open pipe: 20 second wait timed out.";
      return nullptr;
    }
  }
#else
  std::string pipeName = "/tmp/" + name;
  ipc->hClient = socket(AF_UNIX, SOCK_STREAM, 0);
  if (ipc->hClient == -1) {
    LOG_ERROR << "[IPC] Could not create pipe. Error " << errno;
    return nullptr;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, pipeName.c_str(), sizeof(addr.sun_path) - 1);

  int ret = connect(ipc->hClient, (const struct sockaddr *)&addr, sizeof(addr));
  if (ret == -1) {
    LOG_ERROR << "[IPC] Could not connect pipe. Error " << errno;
    return nullptr;
  }
#endif

  return ipc;
} catch (std::exception &e) {
  LOG_ERROR << "[IPC] Open error: " << e.what();
  return nullptr;
}