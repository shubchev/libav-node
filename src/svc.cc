#include "common.h"
#include <condition_variable>
#include <mutex>
#include <thread>

static std::thread svcThread;

static std::condition_variable svcCond;
static std::mutex svcLock;
static bool svcExitFlag = false;

static IPCPipe svcPipe;

void svcWorker(const std::string &instanceId) {
  AVEnc enc;
  int width, height, fps, bps;

  LOG_INFO << "[AV] Starting libav-node service, session id \"" << instanceId << '"';

  auto encoders = IAVEnc::getEncoders();
  auto decoders = IAVEnc::getDecoders();

  LOG_INFO << "Available encoders:";
  for (auto &e : encoders) LOG_INFO << "  Name: " << e;

  LOG_INFO << "Available decoders:";
  for (auto &d : decoders) LOG_INFO << "  Name: " << d;

  SingleArray packetData;
  DoubleArray frameData;

  auto lastKeepAlive = std::chrono::system_clock::now();
  bool stopService = false;
  while (1) {
    auto keepAliveDur = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - lastKeepAlive);
    if (keepAliveDur.count() > 10) {
      LOG_INFO << "[AV] Keep alive exit";
      break;
    }

    AVCmd cmd;
    if (!svcPipe->isOpen()) {
      break;
    }
    if (!readAVCmd(svcPipe, &cmd, 200)) {
      continue;
    }
    lastKeepAlive = std::chrono::system_clock::now();

    switch (cmd.type) {
      case AVCmdType::KeepAlive: {
        LOG_DEBUG << "[AV] KeepAlive CMD";
        sendAVCmdResult(svcPipe, AVCmdResult::Ack);
        break;
      }
      case AVCmdType::GetEncoderCount: {
        LOG_INFO << "[AV] GetEncoderCount CMD: " << encoders.size();
        sendAVCmdResult(svcPipe, AVCmdResult::Ack, encoders.size());
        break;
      }
      case AVCmdType::GetEncoderName: {
        LOG_INFO << "[AV] GetEncoderName CMD";
        if (cmd.size < encoders.size()) {
          sendAVCmdResult(svcPipe, AVCmdResult::Ack, encoders[cmd.size].length());
          svcPipe->write(encoders[cmd.size].c_str(), encoders[cmd.size].length());
        } else {
          sendAVCmdResult(svcPipe, AVCmdResult::Nack);
        }
        break;
      }
      case AVCmdType::GetDecoderCount: {
        LOG_INFO << "[AV] GetDecoderCount CMD: " << decoders.size();
        sendAVCmdResult(svcPipe, AVCmdResult::Ack, decoders.size());
        break;
      }
      case AVCmdType::GetDecoderName: {
        LOG_INFO << "[AV] GetDecoderName CMD";
        if (cmd.size < decoders.size()) {
          sendAVCmdResult(svcPipe, AVCmdResult::Ack, decoders[cmd.size].length());
          svcPipe->write(decoders[cmd.size].c_str(), decoders[cmd.size].length());
        } else {
          sendAVCmdResult(svcPipe, AVCmdResult::Nack);
        }
        break;
      }
      case AVCmdType::OpenEncoder:
      case AVCmdType::OpenDecoder: {
        std::string codecName = cmd.init.codecName;

        std::vector<std::string> *coderNames;
        if (cmd.type == AVCmdType::OpenDecoder) coderNames = &decoders;
        else coderNames = &encoders;

        std::vector<std::string> matches;
        for (auto &c : *coderNames) {
          if (c.find(codecName) != std::string::npos) {
            matches.push_back(c);
            LOG_INFO << "match: " << c;
          }
        }

        std::sort(matches.begin(), matches.end(),
          [] (const std::string &a, const std::string &b) {
            return a.compare(b);
          });

        for (auto &name : matches) {
          LOG_INFO << "match test: " << name;
          if (cmd.type == AVCmdType::OpenDecoder) enc = IAVEnc::createDecoder(name, cmd.init.width, cmd.init.height);
          else enc = IAVEnc::createEncoder(name, cmd.init.width, cmd.init.height, cmd.init.fps, cmd.init.bps);
          if (enc) break;
        }

        if (enc) {
          width = cmd.init.width;
          height = cmd.init.height;
          sendAVCmdResult(svcPipe, AVCmdResult::Ack);
          LOG_INFO << "[AV] " << ((cmd.type == AVCmdType::OpenDecoder) ? "Decoder" : "Encoder") << " " <<
                      "created: name=" << codecName << " " << cmd.init.width << "x" << cmd.init.height << " " <<
                      "fps = " << cmd.init.fps << " bps=" << cmd.init.bps;
        } else {
          width = height = 0;
          packetData.clear(); packetData.shrink_to_fit();
          frameData.clear(); frameData.shrink_to_fit();
          sendAVCmdResult(svcPipe, AVCmdResult::Nack);
          LOG_INFO << "[AV] Failed to create " << ((cmd.type == AVCmdType::OpenDecoder) ? "decoder" : "encoder");
        }
        break;
      }
      case AVCmdType::Close: {
        enc = nullptr;
        width = height = 0;
        packetData.clear(); packetData.shrink_to_fit();
        frameData.clear(); frameData.shrink_to_fit();
        LOG_INFO << "[AV] Closing encoder/decoder";
        sendAVCmdResult(svcPipe, AVCmdResult::Ack);
        break;
      }
      case AVCmdType::Encode: {
        LOG_DEBUG << "[AV] Encode CMD: ";
        if (!enc || !enc->isEncoder()) {
          sendAVCmdResult(svcPipe, AVCmdResult::Nack);
          LOG_ERROR << "[AV]    no encoder opened";
          break;
        } else sendAVCmdResult(svcPipe, AVCmdResult::Ack);

        frameData.push_back(SingleArray());
        frameData.back().resize(cmd.size);
        packetData.clear();
        if (svcPipe->read(frameData.back().data(), cmd.size) != cmd.size) {
          sendAVCmdResult(svcPipe, AVCmdResult::Nack);
          LOG_ERROR << "[AV]    failed to read data";
          break;
        }

        bool ret = enc->process(&frameData, &packetData);
        LOG_DEBUG << "[AV]    process result " << ret;
        if (ret) sendAVCmdResult(svcPipe, AVCmdResult::Ack);
        else sendAVCmdResult(svcPipe, AVCmdResult::Nack);
        break;
      }
      case AVCmdType::Decode: {
        LOG_DEBUG << "[AV] Decode CMD: ";
        if (!enc || enc->isEncoder()) {
          sendAVCmdResult(svcPipe, AVCmdResult::Nack);
          LOG_ERROR << "[AV]    no decoder opened";
          break;
        } else sendAVCmdResult(svcPipe, AVCmdResult::Ack);

        packetData.resize(cmd.size);
        if (svcPipe->read(packetData.data(), cmd.size) != cmd.size) {
          sendAVCmdResult(svcPipe, AVCmdResult::Nack);
          LOG_ERROR << "[AV]    failed to read data";
          break;
        }

        bool ret = enc->process(&frameData, &packetData);
        LOG_DEBUG << "[AV]    process result " << ret;
        if (ret) sendAVCmdResult(svcPipe, AVCmdResult::Ack);
        else sendAVCmdResult(svcPipe, AVCmdResult::Nack);
        break;
      }
      case AVCmdType::GetPacket: {
        LOG_DEBUG << "[AV] GetPacket CMD: size = " << packetData.size();

        if (packetData.size()) {
          sendAVCmdResult(svcPipe, AVCmdResult::Ack, packetData.size());
          svcPipe->write(packetData.data(), packetData.size());
          packetData.clear();
        } else {
          sendAVCmdResult(svcPipe, AVCmdResult::Nack);
        }
        break;
      }
      case AVCmdType::GetFrame: {
        LOG_DEBUG << "[AV] GetFrame CMD";
        for (auto &f : frameData) {
          LOG_DEBUG << "[AV]    frame size " << f.size();
        }

        if (frameData.size()) {
          auto &data = frameData.front();
          sendAVCmdResult(svcPipe, AVCmdResult::Ack, data.size());
          svcPipe->write(data.data(), data.size());
          frameData.erase(frameData.begin());
        } else {
          sendAVCmdResult(svcPipe, AVCmdResult::Nack);
        }
        break;
      }
      case AVCmdType::Flush: {
        LOG_DEBUG << "[AV] Flush CMD";
        bool ret = false;
        if (enc->isEncoder()) ret = enc->process(nullptr, &packetData);
        else ret = enc->process(&frameData, nullptr);
        if (ret) sendAVCmdResult(svcPipe, AVCmdResult::Ack);
        else sendAVCmdResult(svcPipe, AVCmdResult::Nack);
        break;
      }

      case AVCmdType::StopService: {
        stopService = true;
        enc = nullptr;
        LOG_INFO << "[AV] Stopping service";
        sendAVCmdResult(svcPipe, AVCmdResult::Ack);
        break;
      }
      default: {
        sendAVCmdResult(svcPipe, AVCmdResult::Nack);
        break;
      }
    }

    if (stopService) {
      break;
    }
  }

  {
    std::lock_guard<std::mutex> lg(svcLock);
    svcPipe = nullptr;
    svcExitFlag = true;
    svcCond.notify_all();
  }
  LOG_DEBUG << "[AV] Exit service.";
}


bool startService(const std::string &instanceId) {
  if (instanceId.empty()) {
    LOG_ERROR << "[AV] Invalid instance id";
    return false;
  }

  svcPipe = IIPCPipe::create(instanceId, PIPE_BUFFER_SIZE);
  if (!svcPipe) {
    LOG_ERROR << "[AV] Failed to create pipe";
    return false;
  }

  svcThread = std::thread(svcWorker, instanceId);

  return true;
}

void waitServiceToExit() {
  LOG_INFO << "Waiting for service exit";
  if (svcThread.joinable()) {
    svcThread.join();
  }
  LOG_INFO << "Done";
}