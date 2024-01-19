#pragma once

#include <cstdint>
#include <memory>
#include <vector>

enum class AVCmdType : uint8_t {
  Unknown = 0,
  EncodeFrame,
  DecodeFrame,
  StopService,

  Ack,
  Nack,
};

#pragma pack(push, 1)
typedef struct {
  uint32_t bps;
  uint16_t width;
  uint16_t height;
  uint8_t  fps;
} AVInitInfo;

typedef struct {
  size_t size;
} AVEncodeInfo;

typedef struct {
  size_t size;
} AVDecodeInfo;

typedef struct {
  AVCmdType type;
  union {
    AVInitInfo init;
    AVEncodeInfo eframe;
    AVEncodeInfo dframe;
  };
} AVCmd;
#pragma pack(pop)

class IAVEnc;
typedef std::shared_ptr<IAVEnc> AVEnc;
class IAVEnc {
public:
  virtual ~IAVEnc() {}

  static AVEnc create(int width, int height, int framesPerSecond, int bitsPerSecond, int instanceId);

  virtual size_t encode(const void *frameData, void *packetData) = 0;
};



class IAVDec;
typedef std::shared_ptr<IAVDec> AVDec;
class IAVDec {
public:
  virtual ~IAVDec() {}

  static AVDec create(int width, int height, int instanceId);

  virtual bool decode(const void *packetData, size_t &packetSize, void *frameData) = 0;
};