#include "ipc-pipe.h"

#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/sync/named_condition.hpp>
#include <boost/interprocess/sync/named_mutex.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>

using namespace boost::interprocess;
using namespace boost::posix_time;

class IPCCmdQueue {
protected:
  uint32_t &readOffset;
  uint32_t &writeOffset;
  uint8_t *dataPtr = nullptr;
  uint32_t dataSize = 0;

  std::string condName;
  std::shared_ptr<named_condition> cond;
  std::string mutexName;
  std::shared_ptr<named_mutex> mutex;

  size_t getFreeSize() {
    return dataSize - getUsedSize();
  }
  size_t getUsedSize() {
    return (writeOffset - readOffset) % dataSize;
  }

public:
  IPCCmdQueue(const std::string &_mutexName, const std::string &_condName, bool initQ,
              uint8_t *basePtr, size_t totalSize) :
      readOffset (*((uint32_t *)&basePtr[               0])),
      writeOffset(*((uint32_t *)&basePtr[sizeof(uint32_t)])),
      mutexName(_mutexName),
      condName(_condName) {

    dataPtr = &basePtr[2 * sizeof(uint32_t)];
    dataSize = (uint32_t)(totalSize - 2 * sizeof(uint32_t));

    if (initQ) {
      readOffset = 0;
      writeOffset = 0;

      named_mutex::remove(mutexName.c_str());
      named_condition::remove(condName.c_str());
      mutex = std::make_shared<named_mutex>(create_only, mutexName.c_str());
      cond = std::make_shared<named_condition>(create_only, condName.c_str());
    } else {
      mutex = std::make_shared<named_mutex>(open_only, mutexName.c_str());
      cond = std::make_shared<named_condition>(open_only, condName.c_str());
    }
  }

  ~IPCCmdQueue() {
    mutex = nullptr;
    cond = nullptr;
    named_mutex::remove(mutexName.c_str());
    named_condition::remove(condName.c_str());
  }

  bool valid() {
    return mutex != nullptr && cond != nullptr && dataPtr != nullptr && dataSize > 0;
  }

  size_t size() {
    scoped_lock<named_mutex> lock(*mutex);
    return getUsedSize();
  }

  size_t freeSize() {
    scoped_lock<named_mutex> lock(*mutex);
    return getFreeSize();
  }

  void clear() {
    scoped_lock<named_mutex> lock(*mutex);
    readOffset = writeOffset = 0;
  }

  bool empty() {
    scoped_lock<named_mutex> lock(*mutex);
    return readOffset == writeOffset;
  }

  size_t write(const void *data, size_t sz) {
    scoped_lock<named_mutex> lock(*mutex);
    if (getFreeSize() < sz) return 0;

    uint8_t *ptr = (uint8_t *)data;
    size_t bytesWritten = 0;
    while (bytesWritten < sz) {
      size_t bytesToWrite = std::min(sz - bytesWritten, (size_t)dataSize - writeOffset);
      memcpy(&dataPtr[writeOffset], ptr, bytesToWrite);
      writeOffset += bytesToWrite;
      writeOffset %= dataSize;
      ptr += bytesToWrite;
      bytesWritten += bytesToWrite;
    }

    cond->notify_all();

    return bytesWritten;
  }

  size_t read(void *data, size_t sz, int timeoutMs) {
    scoped_lock<named_mutex> lock(*mutex);
    if (getUsedSize() < sz) {
      if (timeoutMs < 0) {
        cond->wait(lock);
      } else {
        auto timeSinceEpoch = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock().now().time_since_epoch()).count();
        ptime p(boost::gregorian::date(1970, 1, 1), milliseconds(timeSinceEpoch + timeoutMs));
        if (!cond->timed_wait(lock, p)) return 0;
        if (getUsedSize() < sz) return 0;
      }
    }

    uint8_t *ptr = (uint8_t *)data;
    size_t bytesRead = 0;
    while (bytesRead < sz) {
      size_t bytesToRead = std::min(sz - bytesRead, (size_t)dataSize - readOffset);
      memcpy(ptr, &dataPtr[readOffset], bytesToRead);
      readOffset += bytesToRead;
      readOffset %= dataSize;
      ptr += bytesToRead;
      bytesRead += bytesToRead;
    }
    return bytesRead;
  }
};

class IPCPipeImpl : public IIPCPipe {
public:
  IPCPipeImpl() {
  }
  ~IPCPipeImpl() {
    shm = nullptr;
    cmdQueA = nullptr;
    cmdQueB = nullptr;
    if (shmName.length()) shared_memory_object::remove(shmName.c_str());
  }

  size_t write(const void *data, size_t size) override {
    if (!data || !size) {
      return 0;
    }
    return cmdQueA->write(data, size);
  }

  size_t read(void *data, size_t size, int timeoutMs) override {
    if (!data || !size) {
      return 0;
    }
    return cmdQueB->read(data, size, timeoutMs);
  }

  void *getBuffer(size_t offset, size_t size) override {
    if (!dataBaseAddr) return nullptr;
    if (offset + size > dataSize) return nullptr;
    return &dataBaseAddr[offset];
  }

  std::string shmName;
  std::shared_ptr<shared_memory_object> shm;
  mapped_region shmMapRegion;

  std::shared_ptr<IPCCmdQueue> cmdQueA;
  std::shared_ptr<IPCCmdQueue> cmdQueB;

  size_t dataSize = 0;
  uint8_t *dataBaseAddr = nullptr;
};


IPCPipe IIPCPipe::create(const std::string &name, size_t maxMessagesSize, size_t bufferStorageSize) try {
  auto ipc = std::make_shared<IPCPipeImpl>();
  if (!ipc) {
    return nullptr;
  }

  size_t totalSize = 2 * maxMessagesSize + bufferStorageSize;

  ipc->shmName = name + "_data";
  shared_memory_object::remove(ipc->shmName.c_str());
  ipc->shm = std::make_shared<shared_memory_object>(create_only, ipc->shmName.c_str(), read_write);
  ipc->shm->truncate(totalSize);

  offset_t memSize = 0;
  ipc->shm->get_size(memSize);
  if (memSize < totalSize) {
    return nullptr;
  }

  ipc->shmMapRegion = mapped_region(*ipc->shm, read_write, 0, totalSize);
  auto baseAddr = (uint8_t *)ipc->shmMapRegion.get_address();
  if (!baseAddr) {
    return nullptr;
  }
  ipc->dataSize = bufferStorageSize;
  ipc->dataBaseAddr = &baseAddr[2 * maxMessagesSize];

  ipc->cmdQueA = std::make_shared<IPCCmdQueue>(name + "_lockA", name + "_condA", true, baseAddr, maxMessagesSize);
  if (!ipc->cmdQueA) {
    return nullptr;
  }

  ipc->cmdQueB = std::make_shared<IPCCmdQueue>(name + "_lockB", name + "_condB", true, &baseAddr[maxMessagesSize], maxMessagesSize);
  if (!ipc->cmdQueB) {
    return nullptr;
  }

  if (!ipc->cmdQueA->valid() || !ipc->cmdQueB->valid()) {
    return nullptr;
  }

  return ipc;
} catch (std::exception &e) {
  fprintf(stderr, "IPC-PIPE: Create error: %s\n", e.what());
  return nullptr;
}

IPCPipe IIPCPipe::open(const std::string &name, size_t maxMessagesSize, size_t bufferStorageSize) try {
  auto ipc = std::make_shared<IPCPipeImpl>();
  if (!ipc) {
    return nullptr;
  }

  size_t totalSize = 2 * maxMessagesSize + bufferStorageSize;

  ipc->shmName = name + "_data";
  ipc->shm = std::make_shared<shared_memory_object>(open_only, ipc->shmName.c_str(), read_write);

  ipc->shmMapRegion = mapped_region(*ipc->shm, read_write, 0, totalSize);
  auto baseAddr = (uint8_t *)ipc->shmMapRegion.get_address();
  if (!baseAddr) {
      return nullptr;
  }
  ipc->dataSize = bufferStorageSize;
  ipc->dataBaseAddr = &baseAddr[2 * maxMessagesSize];

  ipc->cmdQueB = std::make_shared<IPCCmdQueue>(name + "_lockA", name + "_condA", false, baseAddr, maxMessagesSize);
  if (!ipc->cmdQueB) {
    return nullptr;
  }

  ipc->cmdQueA = std::make_shared<IPCCmdQueue>(name + "_lockB", name + "_condB", false, &baseAddr[maxMessagesSize], maxMessagesSize);
  if (!ipc->cmdQueA) {
    return nullptr;
  }

  if (!ipc->cmdQueA->valid() || !ipc->cmdQueB->valid()) {
    return nullptr;
  }

  return ipc;
} catch (std::exception &e) {
  fprintf(stderr, "IPC-PIPE: Open error: %s\n", e.what());
  return nullptr;
}