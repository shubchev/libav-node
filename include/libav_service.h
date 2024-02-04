#pragma once

#include <stdint.h>
#include <string.h>

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
  Encode,
  Decode,
  Flush,
  GetPacket,
  GetFrame,

  StopService,
};

enum class AVCmdResult : uint8_t {
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
  AVCmdType type;
  union {
    AVInitInfo init;
    size_t size;
  };
} AVCmd;
#pragma pack(pop)
