#include <libav_service.h>
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

  bool init(int width, int height, int bps, int fps, int instanceId) {
    int ret;
    if (width <= 0 || height <= 0 || (width & 2) || (height % 2) || bps < 1000000 || fps < 1) {
      return false;
    }


    std::vector<const AVCodec *> codecs;
    const AVCodec *codec = NULL;
    void *iter = NULL;
    while (codec = av_codec_iterate(&iter)) {
      if (!av_codec_is_encoder(codec)) continue;
      if (strstr(codec->name, "hevc") || strstr(codec->name, "h265") ||
          strstr(codec->name, "avc") || strstr(codec->name, "h264")) {
        codecs.push_back(codec);
      }
    }

    for (auto &c : codecs) printf("Encoder: %s\n", c->name);

    for (auto &c : codecs) {
      codec = avcodec_find_encoder_by_name(c->name);
      if (!codec) {
        fprintf(stderr, "Could not find video codec\n");
        continue;
      }

      ctx = avcodec_alloc_context3(codec);
      if (!ctx) {
        fprintf(stderr, "Could not allocate video encoder context\n");
        continue;
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
        fprintf(stderr, "Could not open codec '%s': %s\n", c->name, av_make_error_string(errstr, sizeof(errstr), ret));
        if (ctx) avcodec_free_context(&ctx);
        ctx = nullptr;
        continue;
      }
      printf("Codec opened: %s\n", c->name);
      break;
    }
    if (!ctx) {
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

    return true;
  }

  void deinit() {
    if (ctx) avcodec_free_context(&ctx); ctx = nullptr;
    if (frame) av_frame_free(&frame); frame = nullptr;
    if (pkt) av_packet_free(&pkt); pkt = nullptr;
  }

  size_t encode(const void *frameData, void *packetData) override {
    if (frameData) {
      auto dataPtr = (uint8_t *)frameData;
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

    int ret;
    if (frameData) ret = avcodec_send_frame(ctx, frame);
    else ret = avcodec_send_frame(ctx, 0);
    if (ret < 0) {
      fprintf(stderr, "Error sending a frame for encoding\n");
      return 0;
    }

    size_t encSize = 0;
    while (ret >= 0) {
      ret = avcodec_receive_packet(ctx, pkt);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        if (ret == AVERROR_EOF) return 0;
        continue;
      } else if (ret < 0) {
        fprintf(stderr, "Error during encoding\n");
        return 0;
      }

      if (packetData) {
        encSize = pkt->size;
        memcpy(packetData, pkt->data, encSize);
      }

      av_packet_unref(pkt);
    }

    return encSize;
  }

};



AVEnc IAVEnc::create(int width, int height, int fps, int bps, int instanceId) {
  auto enc = std::make_shared<AVEncoder>();
  if (!enc) {
    return nullptr;
  }

  if (!enc->init(width, height, bps, fps, instanceId)) {
    return nullptr;
  }

  return enc;
}





