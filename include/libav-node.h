#pragma once

#include <cstdint>
#include <memory>

class IAVEnc;
typedef std::shared_ptr<IAVEnc> AVEnc;

typedef void (*EncoderDoneCb)(const void *data, size_t size);

enum class EncQuality {
  EXTRA_LOW,
  LOW,
  MEDIUM_LOW,
  MEDIUM,
  MEDIUM_HIGH,
  HIGH,
};

class IAVEnc {
public:
  virtual ~IAVEnc() {}

  static AVEnc create(int width, int height, int fps, EncQuality quality);

  virtual bool encode(const void *data, size_t size) = 0;
};