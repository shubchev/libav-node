#pragma once

#include <memory>

class IIPCPipe;
typedef std::shared_ptr<IIPCPipe> IPCPipe;

class IIPCPipe {
public:
  virtual ~IIPCPipe() {}

  virtual size_t write(const void *data, size_t size) = 0;
  virtual size_t read(void *data, size_t size, int timeoutMs = -1) = 0;
  virtual void * getBuffer(size_t offset, size_t size) = 0;

  static IPCPipe create(const std::string &name,  size_t maxMessageSize, size_t bufferStorageSize);
  static IPCPipe open(const std::string &name, size_t maxMessagesSize, size_t bufferStorageSize);
};
