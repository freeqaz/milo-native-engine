// Decode the raw XMA2 packets stored in Xbox 360 SynthSample banks.
#include "platform/XmaSampleDecoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {
constexpr int kPacketBytes = 2048;

void FreeContext(AVCodecContext *ctx) { avcodec_free_context(&ctx); }
void FreePacket(AVPacket *packet) { av_packet_free(&packet); }
void FreeFrame(AVFrame *frame) { av_frame_free(&frame); }

bool AppendFrame(const AVFrame &frame, int channels, std::vector<int16_t> &pcm) {
    if (frame.ch_layout.nb_channels != channels) return false;
    const auto format = static_cast<AVSampleFormat>(frame.format);
    const bool planar = av_sample_fmt_is_planar(format);
    const auto packed = av_get_packed_sample_fmt(format);
    if (packed != AV_SAMPLE_FMT_FLT && packed != AV_SAMPLE_FMT_S16) return false;

    for (int i = 0; i < frame.nb_samples; ++i) {
        for (int ch = 0; ch < channels; ++ch) {
            const uint8_t *plane = frame.extended_data[planar ? ch : 0];
            int index = planar ? i : i * channels + ch;
            if (packed == AV_SAMPLE_FMT_FLT) {
                const float sample = reinterpret_cast<const float *>(plane)[index];
                const float scaled = std::isfinite(sample) ? sample * 32768.0f : 0.0f;
                pcm.push_back(static_cast<int16_t>(std::clamp(scaled, -32768.0f, 32767.0f)));
            } else {
                pcm.push_back(reinterpret_cast<const int16_t *>(plane)[index]);
            }
        }
    }
    return true;
}
}

bool DecodeXMAToPCM(const void *xmaData, int xmaSize, int numSamples, int sampleRate,
                    int numChannels, void **outPCM, int *outPCMSize) {
    *outPCM = nullptr;
    *outPCMSize = 0;
    if (!xmaData || xmaSize <= 0 || xmaSize % kPacketBytes || sampleRate <= 0 ||
        (numChannels != 1 && numChannels != 2)) {
        return false;
    }

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
    if (!codec) return false;
    std::unique_ptr<AVCodecContext, decltype(&FreeContext)> ctx(
        avcodec_alloc_context3(codec), FreeContext);
    std::unique_ptr<AVPacket, decltype(&FreePacket)> packet(av_packet_alloc(), FreePacket);
    std::unique_ptr<AVFrame, decltype(&FreeFrame)> frame(av_frame_alloc(), FreeFrame);
    if (!ctx || !packet || !frame) return false;

    ctx->sample_rate = sampleRate;
    ctx->pkt_timebase = AVRational{1, sampleRate};
    av_channel_layout_default(&ctx->ch_layout, numChannels);
    ctx->block_align = kPacketBytes;
    ctx->bits_per_coded_sample = 16;

    // XMA2WAVEFORMATEX's 34 extra bytes. Every bank entry is a single
    // stream. Its mono/stereo mask follows NumStreams in little-endian order.
    ctx->extradata_size = 34;
    ctx->extradata = static_cast<uint8_t *>(av_mallocz(
        ctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!ctx->extradata) return false;
    ctx->extradata[0] = 1;
    ctx->extradata[2] = numChannels == 1 ? 0x04 : 0x03;
    if (avcodec_open2(ctx.get(), codec, nullptr) < 0) return false;
    if (av_new_packet(packet.get(), kPacketBytes) < 0) return false;

    std::vector<int16_t> pcm;
    if (numSamples > 0) pcm.reserve(static_cast<size_t>(numSamples) * numChannels);
    auto receive = [&] {
        int result;
        while ((result = avcodec_receive_frame(ctx.get(), frame.get())) == 0) {
            if (!AppendFrame(*frame, numChannels, pcm)) return false;
        }
        return result == AVERROR(EAGAIN) || result == AVERROR_EOF;
    };

    const auto *bytes = static_cast<const uint8_t *>(xmaData);
    for (int offset = 0; offset < xmaSize; offset += kPacketBytes) {
        // FFmpeg's bit reader requires a zero-padded buffer, including after the
        // final packet. Make it writable before replacing the packet data.
        if (av_packet_make_writable(packet.get()) < 0) return false;
        std::memcpy(packet->data, bytes + offset, kPacketBytes);
        if (avcodec_send_packet(ctx.get(), packet.get()) < 0 || !receive()) return false;
    }
    if (avcodec_send_packet(ctx.get(), nullptr) < 0 || !receive() || pcm.empty()) return false;

    const size_t bytesDecoded = pcm.size() * sizeof(int16_t);
    if (bytesDecoded > INT_MAX) return false;
    void *decoded = std::malloc(bytesDecoded);
    if (!decoded) return false;
    std::memcpy(decoded, pcm.data(), bytesDecoded);
    *outPCM = decoded;
    *outPCMSize = static_cast<int>(bytesDecoded);
    return true;
}
