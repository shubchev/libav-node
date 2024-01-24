#pragma once

#include <stdint.h>
#include <string.h>

#define MESSAGE_BUFFER_SIZE (100 * 1024)
#define PIPE_BUFFER_SIZE (128 * 1024 * 1024)

enum class AVCmdType : uint8_t {
  Unknown = 0,

  GetEncoderCount,
  GetEncoderName,
  GetDecoderCount,
  GetDecoderName,

  OpenEncoder,
  OpenDecoder,
  Close,
  Process,
  Flush,
  HasFrame,

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
  char codecName[30];
} AVInitInfo;

typedef struct {
  size_t size;
  uint8_t bufferIndex;
} AVEncodeInfo;

typedef struct {
  AVCmdType type;
  union {
    AVInitInfo init;
    AVEncodeInfo info;
  };
} AVCmd;
#pragma pack(pop)
