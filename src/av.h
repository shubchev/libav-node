#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

typedef std::vector<uint8_t> SingleArray;
typedef std::vector<SingleArray> DoubleArray;

class IAVEnc;
typedef std::shared_ptr<IAVEnc> AVEnc;
class IAVEnc {
protected:
  std::string codecName;
public:
  virtual ~IAVEnc() {}

  static std::vector<std::string> getEncoders();
  static std::vector<std::string> getDecoders();

  static AVEnc createEncoder(const std::string &name, int width, int height, int framesPerSecond, int bitsPerSecond);
  static AVEnc createDecoder(const std::string &name, int width, int height);


  virtual bool isEncoder() const = 0;
  virtual bool process(DoubleArray *frameData, SingleArray *packetData) = 0;
  const std::string &getName() const { return codecName; }
};