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

    auto codec = avcodec_find_decoder_by_name(name.c_str());
    if (!codec) {
      fprintf(stderr, "Could not find video codec: %s\n", name.c_str());
      return false;
    }

    parser = av_parser_init(codec->id);
    if (!parser) {
      fprintf(stderr, "parser not found\n");
      return false;
    }

    ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
      fprintf(stderr, "Could not allocate video encoder context\n");
      deinit();
      return false;
    }

    pkt = av_packet_alloc();
    if (!pkt) {
      fprintf(stderr, "Could not allocate video packet\n");
      deinit();
      return false;
    }


    ctx->width  = width;
    ctx->height = height;

    ctx->opaque = this;

    char errstr[256];
    auto ret = avcodec_open2(ctx, codec, NULL);
    if (ret < 0) {
      fprintf(stderr, "Could not open codec: %s\n", av_make_error_string(errstr, sizeof(errstr), ret));
      return false;
    }

    frame = av_frame_alloc();
    if (!frame) {
      fprintf(stderr, "Could not allocate video frame\n");
      deinit();
      return false;
    }

    codecName = name;
    printf("Decoder opened: %s\n", codec->name);

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
      fprintf(stderr, "Error sending a packet for decoding\n");
      return false;
    }

    while (ret >= 0) {
      ret = avcodec_receive_frame(ctx, frame);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        if (ret == AVERROR_EOF) return false;
        continue;
      } else if (ret < 0) {
        fprintf(stderr, "Error during decoding\n");
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
        fprintf(stderr, "Error while parsing\n");
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

std::vector<std::string> IAVEnc::getDecoders() {
  std::vector<std::string> codecs;
  const AVCodec *codec = NULL;
  void *iter = NULL;
  while (codec = av_codec_iterate(&iter)) {
    if (!av_codec_is_decoder(codec)) continue;
    if (strstr(codec->name, "hevc") || strstr(codec->name, "h265") ||
        strstr(codec->name, "avc")  || strstr(codec->name, "h264")) {
      codecs.push_back(codec->name);
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
