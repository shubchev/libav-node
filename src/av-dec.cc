#include <libav_service.h>
#include <string>
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

#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>
using namespace boost::interprocess;


class AVDecoder : public IAVDec {
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

uint8_t *frameData = nullptr;
  uint8_t *packetData = nullptr;

  shared_memory_object shm;
  mapped_region frameMapRegion;
  mapped_region packetMapRegion;

  bool init(int width, int height, int instanceId) {
    if (width <= 0 || height <= 0 || (width & 2) || (height % 2)) {
      return false;
    }


    std::vector<const AVCodec *> codecs;
    const AVCodec *codec = NULL;
    void *iter = NULL;
    while (codec = av_codec_iterate(&iter)) {
      if (!av_codec_is_decoder(codec)) continue;
      if (strstr(codec->name, "hevc") || strstr(codec->name, "h265") ||
          strstr(codec->name, "avc")  || strstr(codec->name, "h264")) {
        codecs.push_back(codec);
      }
    }

    for (auto &c : codecs) printf("Decoder: %s\n", c->name);

    codec = avcodec_find_decoder_by_name("hevc");
    if (!codec) {
      fprintf(stderr, "Could not find video codec\n");
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



    std::stringstream pipeName;
    pipeName << "avLibService" << instanceId;

    auto name = pipeName.str();
    shm = shared_memory_object(open_or_create, name.c_str(), read_write);

    offset_t frameSize = width * height * 3;
    offset_t packetSize = 1000000;
    shm.truncate(frameSize + packetSize);

    offset_t memSize = 0;
    shm.get_size(memSize);
    if (memSize < frameSize + packetSize) {
      fprintf(stderr, "Could not allocate IPC data\n");
      deinit();
      return false;
    }

    frameMapRegion = mapped_region(shm, read_write, 0, frameSize);
    packetMapRegion = mapped_region(shm, read_write, frameSize, packetSize);

    frameData = (uint8_t *)frameMapRegion.get_address();
    packetData = (uint8_t *)packetMapRegion.get_address();

    if (!frameData || !packetData) {
      fprintf(stderr, "Could not map IPC data\n");
      deinit();
      return false;
    }

    return true;
  }

  void deinit() {
    frameData = packetData = nullptr;
    shared_memory_object::remove(shm.get_name());
    if (parser) av_parser_close(parser); parser = nullptr;
    if (ctx) avcodec_free_context(&ctx); ctx = nullptr;
    if (frame) av_frame_free(&frame); frame = nullptr;
    if (pkt) av_packet_free(&pkt); pkt = nullptr;
  }

  bool decode() {
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

      auto dataPtr = frameData;
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

  bool decode(size_t &size) override {
    auto ptr = packetData;
    while (size > 0) {
      int ret = av_parser_parse2(parser, ctx, &pkt->data, &pkt->size, ptr, size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
      if (ret < 0) {
        fprintf(stderr, "Error while parsing\n");
        return false;
      }

      ptr  += ret;
      size -= ret;

      if (pkt->size) {
        return decode();
      }
    }

    return false;
  }

};



AVDec IAVDec::create(int width, int height, int instanceId) {
  auto dec = std::make_shared<AVDecoder>();
  if (!dec) {
    return nullptr;
  }

  if (!dec->init(width, height, instanceId)) {
    return nullptr;
  }

  return dec;
}
