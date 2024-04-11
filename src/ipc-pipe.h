#pragma once

#include <memory>
#include <string>

class IIPCPipe;
typedef std::shared_ptr<IIPCPipe> IPCPipe;

class IIPCPipe {
public:
  virtual ~IIPCPipe() {}

  virtual bool isOpen() const = 0;

  virtual size_t write(const void *data, size_t size) = 0;
  virtual size_t read(void *data, size_t size, int timeoutMs = -1) = 0;

  static IPCPipe create(const std::string &name, size_t bufferStorageSize);
  static IPCPipe open(const std::string &name);
};
