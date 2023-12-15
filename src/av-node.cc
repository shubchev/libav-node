#include <libav-node.h>
#include <string>
#include <vector>

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
  int width = 0;
  int height = 0;
  int sampleRate = 0;
  int frameIdx = 0;

  FILE *dumpFile = NULL;

  std::vector<uint8_t> dataStore;

  bool init(int w, int h, int bps, int fps) {
    if (w <= 0 || h <= 0 || (w & 2) || (h % 2) || bps < 1000000 || fps < 1) {
      return false;
    }
    width = w;
    height = h;
    sampleRate = bps;


    std::vector<const AVCodec *> codecs;
    const AVCodec *codec = NULL;
    void *iter = NULL;
    while (codec = av_codec_iterate(&iter)) {
      if (!av_codec_is_encoder(codec)) continue;
      if (strstr(codec->name, "hevc") || strstr(codec->name, "h265") ||
          strstr(codec->name, "avc")  || strstr(codec->name, "h264")) {
        codecs.push_back(codec);
      }
    }

    AVHWAccel *hwaccel = av_hwaccel_next(NULL);
    while (hwaccel != NULL) {
        if ( hwaccel != NULL) {
            printf("HW accel: %s ", hwaccel->name);
        }
        hwaccel=av_hwaccel_next(hwaccel);
    }
    printf("\n");

    for (auto &c : codecs) printf("Encoder: %s\n", c->name);

    codec = avcodec_find_encoder_by_name("hevc_nvenc");
    if (!codec) {
      fprintf(stderr, "Could not find video codec\n");
      return false;
    }

    ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
      fprintf(stderr, "Could not allocate video encoder context\n");
      return false;
    }

    pkt = av_packet_alloc();
    if (!pkt) {
      fprintf(stderr, "Could not allocate video packet\n");
      deinit();
      return false;
    }


    ctx->bit_rate = bps;
    /* resolution must be a multiple of two */
    ctx->width  = width;
    ctx->height = height;

    /* frames per second */
    ctx->time_base = (AVRational){1, fps};
    ctx->framerate = (AVRational){fps, 1};

    /* emit one intra frame every ten frames
     * check frame pict_type before passing frame
     * to encoder, if frame->pict_type is AV_PICTURE_TYPE_I
     * then gop_size is ignored and the output of encoder
     * will always be I frame irrespective to gop_size
     */
    ctx->gop_size = 10;
    ctx->max_b_frames = 1;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    dataStore.resize(10 * 1024 * 1024);
    ctx->opaque = this;
    ctx->get_buffer2 = AVEncoder::custom_get_buffer;

    if (codec->id == AV_CODEC_ID_H264) {
      av_opt_set(ctx->priv_data, "preset", "slow", 0);
    }

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
    frame->format = ctx->pix_fmt;
    frame->width  = ctx->width;
    frame->height = ctx->height;

    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
      fprintf(stderr, "Could not allocate the video frame data\n");
      deinit();
      return false;
    }

    dumpFile = fopen("test.mp4", "wb");
    if (!dumpFile) {
      deinit();
      return false;
    }


    return true;
  }

  void deinit() {
    if (dumpFile) {
      static uint8_t footer[] = { 0, 0, 1, 0xb7 };
      //if (codec->id == AV_CODEC_ID_MPEG1VIDEO || codec->id == AV_CODEC_ID_MPEG2VIDEO) fwrite(footer, 1, sizeof(footer), f);
      fclose(dumpFile);
      dumpFile = NULL;
    }
    if (ctx) avcodec_free_context(&ctx); ctx = nullptr;
    if (frame) av_frame_free(&frame); frame = nullptr;
    if (pkt) av_packet_free(&pkt); pkt = nullptr;
  }

  static void custom_buffer_free(void *opaque, uint8_t *data){
    printf("custom_buffer_free(%d)\n", data);
  }

  static int custom_get_buffer(struct AVCodecContext *c, AVFrame *pic, int flags) {
    printf("custom_get_buffer\n");
    auto pThis = (AVEncoder *)c->opaque;

    pic->data[0] = &pThis->dataStore[0];
    pic->data[1] = &pThis->dataStore[4 * 1024 * 1024];
    pic->data[2] = &pThis->dataStore[5 * 1024 * 1024];
    pic->linesize[0] = c->width;
    pic->linesize[1] = c->width / 2;
    pic->linesize[2] = c->width / 2;
    pic->buf[0] = av_buffer_create(pic->data[0], pic->linesize[0] * pic->height, custom_buffer_free, NULL, 0);
    pic->buf[1] = av_buffer_create(pic->data[1], pic->linesize[1] * pic->height / 2, custom_buffer_free, NULL, 0);
    pic->buf[2] = av_buffer_create(pic->data[2], pic->linesize[2] * pic->height / 2, custom_buffer_free, NULL, 0);
    return 0;
  }

  bool encode(const void *data, size_t size) override {
    printf("%d %d %d %p %p %p %d %d\n", frame->linesize[0], frame->linesize[1], frame->linesize[2],
    frame->data[0], frame->data[1], frame->data[2], frame->data[1] - frame->data[0], frame->data[2] - frame->data[1]);
    for (int y = 0; y < ctx->height; y++) {
        for (int x = 0; x < ctx->width; x++) {
            frame->data[0][y * frame->linesize[0] + x] = x + y + frameIdx * 3;
        }
    }

    /* Cb and Cr */
    for (int y = 0; y < ctx->height/2; y++) {
        for (int x = 0; x < ctx->width/2; x++) {
            frame->data[1][y * frame->linesize[1] + x] = 128 + y + frameIdx * 2;
            frame->data[2][y * frame->linesize[2] + x] = 64 + x + frameIdx * 5;
        }
    }

    frame->pts = frameIdx++;

    int ret = avcodec_send_frame(ctx, frame);
    if (ret < 0) {
      fprintf(stderr, "Error sending a frame for encoding\n");
      return false;
    }

    while (ret >= 0) {
      ret = avcodec_receive_packet(ctx, pkt);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return false;
      } else if (ret < 0) {
        fprintf(stderr, "Error during encoding\n");
        return false;
      }

      fwrite(pkt->data, 1, pkt->size, dumpFile);

      av_packet_unref(pkt);
    }
    return true;
  }

};



AVEnc IAVEnc::create(int width, int height, int fps, EncQuality quality) {
  auto enc = std::make_shared<AVEncoder>();
  if (!enc) {
    return nullptr;
  }

  int sps = 4000000;
  switch (quality) {
    case EncQuality::EXTRA_LOW:   sps =  2000000; break;
    case EncQuality::LOW:         sps =  4000000; break;
    case EncQuality::MEDIUM_LOW:  sps =  7500000; break;
    case EncQuality::MEDIUM:      sps = 10000000; break;
    case EncQuality::MEDIUM_HIGH: sps = 15000000; break;
    case EncQuality::HIGH:        sps = 20000000; break;
  }

  if (!enc->init(width, height, sps, fps)) {
    return nullptr;
  }

  return enc;
}