#include <plog/Log.h>
#include "av.h"
#include <string>
#include <sstream>
#include <vector>
#include <thread>

#if defined (__cplusplus)
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#if defined (__cplusplus)
}
#endif

extern FILE *LOGFILE;

class AVDecoder : public IAVEnc {
public:
  AVDecoder() {
  }
  ~AVDecoder() {
    deinit();
  }

  AVCodecParserContext *parser = nullptr;
  AVCodecContext *ctx = nullptr;
  AVFrame *frame = nullptr;
  AVPacket *pkt = nullptr;

  bool init(const std::string &name, int width, int height) {
    if (width <= 0 || height <= 0 || (width & 2) || (height % 2)) {
      return false;
    }
    const char *tmpName = name.c_str();
    if (name.find("sw-") == 0 || name.find("hw-") == 0) {
      tmpName += 3;
    }

    auto codec = avcodec_find_decoder_by_name(tmpName);
    if (!codec) {
      LOG_ERROR << "[DEC] Could not find video codec: " << name;
      return false;
    }

    parser = av_parser_init(codec->id);
    if (!parser) {
      LOG_ERROR << "[DEC] parser not found";
      return false;
    }

    ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
      LOG_ERROR << "[DEC] Could not allocate video encoder context";
      deinit();
      return false;
    }

    pkt = av_packet_alloc();
    if (!pkt) {
      LOG_ERROR << "[DEC] Could not allocate video packet";
      deinit();
      return false;
    }


    ctx->width  = width;
    ctx->height = height;

    ctx->opaque = this;

    char errstr[256];
    auto ret = avcodec_open2(ctx, codec, NULL);
    if (ret < 0) {
      LOG_ERROR << "[DEC] Could not open codec: " << av_make_error_string(errstr, sizeof(errstr), ret);
      return false;
    }

    frame = av_frame_alloc();
    if (!frame) {
      LOG_ERROR << "[DEC] Could not allocate video frame";
      deinit();
      return false;
    }

    codecName = codec->name;
    LOG_INFO << "[DEC] Decoder opened: " << codec->name;

    return true;
  }

  void deinit() {
    if (parser) av_parser_close(parser); parser = nullptr;
    if (ctx) avcodec_free_context(&ctx); ctx = nullptr;
    if (frame) av_frame_free(&frame); frame = nullptr;
    if (pkt) av_packet_free(&pkt); pkt = nullptr;
  }

  bool decode(DoubleArray *frameData) {
    int ret = avcodec_send_packet(ctx, pkt);
    if (ret < 0) {
      LOG_ERROR << "[DEC] Error sending a packet for decoding";
      return false;
    }

    while (ret >= 0) {
      ret = avcodec_receive_frame(ctx, frame);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        if (ret == AVERROR_EOF) return false;
        continue;
      } else if (ret < 0) {
        LOG_ERROR << "[DEC] Error during decoding";
        return false;
      }

      frameData->push_back(SingleArray());
      frameData->back().resize(3 * frame->width * frame->height / 2);

      auto dataPtr = frameData->back().data();
      int stride = frame->width;
      for (int y = 0; y < ctx->height; y++) {
        memcpy(dataPtr, &frame->data[0][y * frame->linesize[0]], stride);
        dataPtr += stride;
      }

      stride /= 2;
      int scanline = ctx->height / 2;
      for (int y = 0; y < ctx->height; y++) {
        int planeIdx = 1 + (y / scanline);
        memcpy(dataPtr, &frame->data[planeIdx][(y % scanline) * frame->linesize[planeIdx]], stride);
        dataPtr += stride;
      }
    }

    return true;
  }

  bool process(DoubleArray *frameData, SingleArray *packetData) override {
    if (!frameData) {
      return false;
    }

    auto ptr = (packetData) ? packetData->data() : nullptr;
    size_t packetSize = (packetData) ? packetData->size() : 0;
    do {
      int ret = av_parser_parse2(parser, ctx, &pkt->data, &pkt->size, ptr, packetSize, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
      if (ret < 0) {
        LOG_ERROR << "[DEC] Error while parsing";
        return false;
      }

      if (packetSize) {
        ptr += ret;
        packetSize -= ret;
      }

      if (pkt->size) {
        if (!decode(frameData)) return false;
      }
    } while (packetSize > 0);

    return true;
  }

  bool isEncoder() const override { return false; }

};

std::set<std::string> IAVEnc::getDecoders() {
  std::set<std::string> codecs;
  const AVCodec *codec = NULL;
#ifdef _WIN32
  void *iter = NULL;
  while (codec = av_codec_iterate(&iter)) {
#else
  while (codec = av_codec_next(codec)) {
#endif
    if (!av_codec_is_decoder(codec)) continue;
    if (strstr(codec->name, "hevc") || strstr(codec->name, "h265") ||
        strstr(codec->name, "avc")  || strstr(codec->name, "h264")) {
      std::string type = "sw-";
      if (avcodec_get_hw_config(codec, 0)) type = "hw-";
      codecs.insert(type + codec->name);
    }
  }
  return codecs;
}

AVEnc IAVEnc::createDecoder(const std::string &name, int width, int height) {
  auto dec = std::make_shared<AVDecoder>();
  if (!dec) {
    return nullptr;
  }

  if (!dec->init(name, width, height)) {
    return nullptr;
  }

  return dec;
}
