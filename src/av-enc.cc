#include "av.h"
#include <string>
#include <sstream>
#include <vector>
#include <thread>

#if defined (__cplusplus)
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#if defined (__cplusplus)
}
#endif

#ifndef _WIN32
class InitAV {
public:
  InitAV() {
    av_register_all();
  }
};
InitAV initAV;
#endif


class AVEncoder : public IAVEnc {
public:
  AVEncoder() {
  }
  ~AVEncoder() {
    deinit();
  }

  AVCodecContext *ctx = nullptr;
  AVFrame *frame = nullptr;
  AVPacket *pkt = nullptr;

  int frameIdx = 0;

  bool init(const std::string &name, int width, int height, int bps, int fps) {
    int ret;
    if (width <= 0 || height <= 0 || (width & 2) || (height % 2) || bps < 1000000 || fps < 1) {
      return false;
    }

    auto codec = avcodec_find_encoder_by_name(name.c_str());
    if (!codec) {
      fprintf(stderr, "Could not find video codec: %s\n", name.c_str());
      return false;
    }

    ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
      fprintf(stderr, "Could not allocate video encoder context\n");
      return false;
    }

    ctx->bit_rate = bps;
    /* resolution must be a multiple of two */
    ctx->width = width;
    ctx->height = height;

    /* frames per second */
    ctx->time_base = { 1, fps };
    ctx->framerate = { fps, 1 };

    /* emit one intra frame every ten frames
    * check frame pict_type before passing frame
    * to encoder, if frame->pict_type is AV_PICTURE_TYPE_I
    * then gop_size is ignored and the output of encoder
    * will always be I frame irrespective to gop_size
    */
    ctx->gop_size = 10;
    ctx->max_b_frames = 1;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    ctx->opaque = this;

    if (codec->id == AV_CODEC_ID_H264) {
      av_opt_set(ctx->priv_data, "preset", "slow", 0);
    }

    char errstr[256];
    ret = avcodec_open2(ctx, codec, NULL);
    if (ret < 0) {
      fprintf(stderr, "Could not open codec '%s': %s\n", codec->name, av_make_error_string(errstr, sizeof(errstr), ret));
      if (ctx) avcodec_free_context(&ctx);
      ctx = nullptr;
      return false;
    }

    pkt = av_packet_alloc();
    if (!pkt) {
      fprintf(stderr, "Could not allocate video packet\n");
      deinit();
      return false;
    }

    frame = av_frame_alloc();
    if (!frame) {
      fprintf(stderr, "Could not allocate video frame\n");
      deinit();
      return false;
    }
    frame->format = ctx->pix_fmt;
    frame->width = ctx->width;
    frame->height = ctx->height;

    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
      fprintf(stderr, "Could not allocate the video frame data\n");
      deinit();
      return false;
    }

    codecName = name;
    printf("Encoder opened: %s\n", codec->name);

    return true;
  }

  void deinit() {
    if (ctx) avcodec_free_context(&ctx); ctx = nullptr;
    if (frame) av_frame_free(&frame); frame = nullptr;
    if (pkt) av_packet_free(&pkt); pkt = nullptr;
  }

  bool process(DoubleArray *frameData, SingleArray *packetData) override {
    int ret = 0;
    for (size_t i = 0; frameData && i < frameData->size(); i++) {
      {
        auto dataPtr = frameData->at(i).data();
        int stride = frame->width;
        for (int y = 0; y < ctx->height; y++) {
          memcpy(&frame->data[0][y * frame->linesize[0]], dataPtr, stride);
          dataPtr += stride;
        }

        stride /= 2;
        int scanline = ctx->height / 2;
        for (int y = 0; y < ctx->height; y++) {
          int planeIdx = 1 + (y / scanline);
          memcpy(&frame->data[planeIdx][(y % scanline) * frame->linesize[planeIdx]], dataPtr, stride);
          dataPtr += stride;
        }

        frame->pts = frameIdx++;
      }

      ret = avcodec_send_frame(ctx, frame);
      if (ret < 0) {
        frameData->erase(frameData->begin(), frameData->begin() + i - 1);
        fprintf(stderr, "Error sending a frame for encoding\n");
        return false;
      }
    }

    if (!frameData) {
      ret = avcodec_send_frame(ctx, 0);
      if (ret < 0) {
        fprintf(stderr, "Error sending a frame for encoding\n");
        return false;
      }
    } else {
      frameData->clear();
    }

    while (ret >= 0) {
      ret = avcodec_receive_packet(ctx, pkt);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        if (ret == AVERROR_EOF) return (frameData) ? false : true;
        continue;
      } else if (ret < 0) {
        fprintf(stderr, "Error during encoding\n");
        return false;
      }

      if (packetData) {
        packetData->insert(packetData->end(), pkt->data, pkt->data + pkt->size);
      }

      av_packet_unref(pkt);
    }

    return true;
  }

  bool isEncoder() const override { return true; }

};

std::vector<std::string> IAVEnc::getEncoders() {
  std::vector<std::string> codecs;
  const AVCodec *codec = NULL;
#ifdef _WIN32
  void *iter = NULL;
  while (codec = av_codec_iterate(&iter)) {
#else
  while (codec = av_codec_next(codec)) {
#endif
    if (!av_codec_is_encoder(codec)) continue;
    if (strstr(codec->name, "hevc") || strstr(codec->name, "h265") ||
        strstr(codec->name, "avc") || strstr(codec->name, "h264")) {
      codecs.push_back(codec->name);
    }
  }
  return codecs;
}

AVEnc IAVEnc::createEncoder(const std::string &name, int width, int height, int fps, int bps) {
  auto enc = std::make_shared<AVEncoder>();
  if (!enc) {
    return nullptr;
  }

  if (!enc->init(name, width, height, bps, fps)) {
    return nullptr;
  }

  return enc;
}





