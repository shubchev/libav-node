#pragma once

#include <cstdint>
#include <memory>
#include <vector>

typedef enum {
  EncodeFrame,
  DecodeFrame,
  StopService,

  Ack,
  Nack,
} AVCmdType;

typedef struct {
  int width;
  int height;
  int fps;
  int bps;
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

class IAVEnc;
typedef std::shared_ptr<IAVEnc> AVEnc;
class IAVEnc {
public:
  virtual ~IAVEnc() {}

  static AVEnc create(int width, int height, int framesPerSecond, int bitsPerSecond, int instanceId);

  virtual size_t encode() = 0;
};



class IAVDec;
typedef std::shared_ptr<IAVDec> AVDec;
class IAVDec {
public:
  virtual ~IAVDec() {}

  static AVDec create(int width, int height, int instanceId);

  virtual bool decode(size_t &size) = 0;
};