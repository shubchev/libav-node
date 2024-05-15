#include "common.h"


bool runEncodeTest(bool &isHEVC, int testWidth, int testHeight, const std::string &testFile) {
  int width  = testWidth;
  int height = testHeight;
  int fps = 30;
  int bps = 5000000;
  auto pipe = openService("test");
  if (!pipe) {
    LOG_ERROR << "[ENC] Failed to open service";
    return false;
  }

  SingleArray packetData;
  SingleArray frameData(3 * width * height / 2);

  AVCmd cmd;
  cmd.type = AVCmdType::OpenEncoder;
  cmd.init.width    = width;
  cmd.init.height   = height;
  cmd.init.fps      = fps;
  cmd.init.bps      = bps;

  // try open hevc
  if (isHEVC) strcpy(cmd.init.codecName, "hevc");
  else strcpy(cmd.init.codecName, "h264");
  if (sendAVCmd(pipe, cmd) != AVCmdResult::Ack) {
    isHEVC = false;
    // try open h264
    strcpy(cmd.init.codecName, "h264");
    if (sendAVCmd(pipe, cmd) != AVCmdResult::Ack) {
      LOG_ERROR << "[ENC] Enc service init failed";
      closeService(pipe);
      return false;
    }
  }

  FILE *dumpFile = fopen(testFile.c_str(), "wb");
  if (!dumpFile) {
    LOG_ERROR << "[ENC] Failed to open test.mp4";
    closeService(pipe);
    return false;
  }

  for (int i = 0; i < 120; i++) {

    auto startTs = std::chrono::system_clock::now();
    int stride = width;
    int plane1Offset = stride * height;
    int plane2Offset = plane1Offset + width * height / 4;
    for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
        frameData[y * stride + x] = x + y + i * 3;
      }
    }

    stride = width / 2;
    // Cb and Cr
    for (int y = 0; y < height/2; y++) {
      for (int x = 0; x < width/2; x++) {
        frameData[plane1Offset + y * stride + x] = 128 + y + i * 2;
        frameData[plane2Offset + y * stride + x] = 64 + x + i * 5;
      }
    }

    auto startTs1 = std::chrono::system_clock::now();

    // Send data for encoding
    cmd.type = AVCmdType::Encode;
    cmd.size = frameData.size();
    if (sendAVCmd(pipe, cmd) != AVCmdResult::Ack) {
      LOG_ERROR << "[ENC] Encode command got NACK response";
    }
    if (pipe->write(frameData.data(), frameData.size()) != frameData.size()) {
      LOG_ERROR << "[ENC] Encode command failed to send frame data";
    }
    if (readAVCmdResult(pipe) != AVCmdResult::Ack) {
      LOG_ERROR << "[ENC] Encoder failed to encode frame " << i;
    }

    // Get encoded data
    if (getPacket(pipe, packetData) == AVCmdResult::Ack) {
      auto endTs = std::chrono::system_clock::now();
      LOG_INFO << "Time for preprocess: " << std::chrono::duration<float>(startTs1 - startTs).count() <<
                  ", Time for encode: " << std::chrono::duration<float>(endTs - startTs1).count() << 
                  ", Packet size = " << packetData.size();

      fwrite(packetData.data(), 1, packetData.size(), dumpFile);
    }
  }

  {
    if (sendAVCmd(pipe, AVCmdType::Flush) != AVCmdResult::Ack) {
      LOG_ERROR << "[ENC] Encode flush command got NACK response";
    }

    while (1) {
      // Get encoded data
      if (getPacket(pipe, packetData) == AVCmdResult::Ack) {
        LOG_INFO << "Writing flush packet";
        fwrite(packetData.data(), 1, packetData.size(), dumpFile);
      } else {
        break;
      }
    }
  }

  fclose(dumpFile);

  return closeService(pipe);
}


bool runDecodeTest(bool isHEVC, int testWidth, int testHeight, const std::string &testFile) {
  FILE *dumpFile = fopen(testFile.c_str(), "rb");
  if (!dumpFile) {
    LOG_ERROR << "[DEC] Failed to open test.mp4";
    return false;
  }

  int width  = testWidth;
  int height = testHeight;
  auto pipe = openService("test");
  if (!pipe) {
    fclose(dumpFile);
    return false;
  }

  SingleArray packetData(16 * 1024);
  SingleArray frameData;

  AVCmd cmd;
  cmd.type = AVCmdType::OpenDecoder;
  cmd.init.width    = width;
  cmd.init.height   = height;

  if (isHEVC) strcpy(cmd.init.codecName, "hevc");
  else strcpy(cmd.init.codecName, "h264");
  if (sendAVCmd(pipe, cmd) != AVCmdResult::Ack) {
    LOG_ERROR << "[DEC] Service init failed";
    return false;
  }

  int frameId = 0;
  while (!feof(dumpFile)) {
    cmd.size = fread(packetData.data(), 1, packetData.size(), dumpFile);
    if (cmd.size) {
      // Send data for decoding
      cmd.type = AVCmdType::Decode;
      if (sendAVCmd(pipe, cmd) != AVCmdResult::Ack) {
        LOG_ERROR << "[DEC] Decode command got NACK response";
      }
      if (pipe->write(packetData.data(), cmd.size) != cmd.size) {
        LOG_ERROR << "[DEC] Decode command failed to send packet data";
      }
      if (readAVCmdResult(pipe) != AVCmdResult::Ack) {
        LOG_ERROR << "[DEC] Decoder failed to decode frame " << frameId;
      }
    }

    // Get decoded data
    while (1) {
      if (getFrame(pipe, frameData) != AVCmdResult::Ack) {
        break;
      }
      LOG_INFO << "Decoded frame " << frameId;

      std::string name = std::string("frame") + std::to_string(frameId++) + ".raw";
      FILE *fp = fopen(name.c_str(), "wb");
      fwrite(frameData.data(), 1, frameData.size(), fp);
      fclose(fp);
    }
  }
  fclose(dumpFile);

  while (1) {
    sendAVCmd(pipe, AVCmdType::Flush);
    if (getFrame(pipe, frameData) != AVCmdResult::Ack) {
      break;
    }
    LOG_INFO << "Decoded frame " << frameId;

    std::string name = std::string("frame") + std::to_string(frameId++) + ".raw";
    FILE *fp = fopen(name.c_str(), "wb");
    fwrite(frameData.data(), 1, frameData.size(), fp);
    fclose(fp);
  }

  return closeService(pipe);
}

int main(int argc, char **argv) {
  CLI::App app("libAV Node Service");

  dumpLog = true;

  bool isHEVC = false;
  bool testDec = false, testEnc = false;
  int testWidth = 1920, testHeight = 1080;
  std::string testFile;
  app.add_flag  ("-d", testDec, "Run a decoder test");
  app.add_option("-f", testFile, "Test file if decoder test is ran");
  app.add_flag  ("-e", testEnc, "Run an encoder test");
  app.add_option("--width", testWidth, "Test width for encoder test. Default 1920")->check(CLI::PositiveNumber);
  app.add_option("--height", testHeight, "Test height for encoder test. Default 1080")->check(CLI::PositiveNumber);
  app.add_flag("--hevc", isHEVC, "Use HEVC");

  static plog::ColorConsoleAppender<plog::TxtFormatter> consoleAppender;
  plog::init(plog::debug, &consoleAppender);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    LOG_ERROR << app.help().c_str();
    return 1;
  }

  if (!testDec && !testEnc) {
    LOG_ERROR << app.help().c_str();
    return 1;
  }

  if ((testDec || testEnc) && testFile.empty()) {
    LOG_ERROR << "When running test, specify test file name. See --help.";
    return 1;
  }


  if (testEnc) {
    LOG_INFO << "[AVTest] Starting encode test";
    if (!runEncodeTest(isHEVC, testWidth, testHeight, testFile)) {
      LOG_ERROR << "Encode test failed";
      return 2;
    }

    if (testDec) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
  }

  if (testDec) {
    LOG_INFO << "[AVTest] Starting decode test";
    if (!runDecodeTest(isHEVC, testWidth, testHeight, testFile)) {
      LOG_ERROR << "Decode test failed";
      return 2;
    }
  }

  return 0;
}