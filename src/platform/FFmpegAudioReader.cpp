// DC3 Native Port - FFmpegAudioReader
// Replaces BinkReader for audio-only .bik decoding via FFmpeg/libavcodec.
//
// BinkReader's audio format: 16-bit PCM, 44100 Hz, mono tracks.
// Each track maps to one audio stream in the .bik container.
// ConsumeData is called with an array of per-track PCM pointers.

#include "platform/FFmpegAudioReader.h"
#include "os/Debug.h"
#include "os/File.h"
#include "synth/StandardStream.h"
#include "utl/Str.h"

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

#include <cstring>

FFmpegAudioReader::FFmpegAudioReader(File *file, StandardStream *stream)
    : mFile(file), mStream(stream), mEnableReads(false),
      mState(0), mFmtCtx(nullptr), mSamplesReady(0),
      mSampleCurrent(0), mSamplesJump(0),
      mPacket(nullptr), mFrame(nullptr), mEOF(false) {
    if (!OpenFile()) {
        MILO_WARN("FFmpegAudioReader: failed to open file");
        mState = kFail;
    } else {
        mState = kInit;
    }
}

FFmpegAudioReader::~FFmpegAudioReader() {
    if (mFrame) av_frame_free(&mFrame);
    if (mPacket) av_packet_free(&mPacket);
    for (auto &t : mTracks) {
        if (t.codecCtx) avcodec_free_context(&t.codecCtx);
    }
    if (mFmtCtx) avformat_close_input(&mFmtCtx);
}

bool FFmpegAudioReader::OpenFile() {
    // Get the file path from the File object
    String filename = mFile->Filename();
    const char *path = filename.c_str();
    if (!path || !path[0]) return false;

    int ret = avformat_open_input(&mFmtCtx, path, nullptr, nullptr);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        MILO_WARN("FFmpegAudioReader: avformat_open_input failed: %s", errbuf);
        return false;
    }

    ret = avformat_find_stream_info(mFmtCtx, nullptr);
    if (ret < 0) {
        avformat_close_input(&mFmtCtx);
        return false;
    }

    // Find all audio streams (matching BinkReader's multi-track model)
    for (unsigned int i = 0; i < mFmtCtx->nb_streams; i++) {
        AVStream *s = mFmtCtx->streams[i];
        if (s->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;

        const AVCodec *codec = avcodec_find_decoder(s->codecpar->codec_id);
        if (!codec) {
            MILO_WARN("FFmpegAudioReader: no decoder for stream %d", i);
            continue;
        }

        AVCodecContext *ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(ctx, s->codecpar);
        ret = avcodec_open2(ctx, codec, nullptr);
        if (ret < 0) {
            avcodec_free_context(&ctx);
            continue;
        }

        TrackInfo ti;
        ti.streamIdx = i;
        ti.codecCtx = ctx;
        ti.sampleRate = ctx->sample_rate;
        ti.channels = ctx->ch_layout.nb_channels;
        mTracks.push_back(ti);
    }

    if (mTracks.empty()) {
        MILO_WARN("FFmpegAudioReader: no audio tracks found");
        avformat_close_input(&mFmtCtx);
        return false;
    }

    // Allocate PCM buffers and pointer array
    mPCMBuffers.resize(mTracks.size());
    mPCMPtrs.resize(mTracks.size(), nullptr);

    // BinkReader uses 0xB400 bytes per track (~23040 samples at 16-bit)
    for (size_t i = 0; i < mTracks.size(); i++) {
        mPCMBuffers[i].resize(0xB400 / 2); // 16-bit samples
        mPCMPtrs[i] = mPCMBuffers[i].data();
    }

    mPacket = av_packet_alloc();
    mFrame = av_frame_alloc();

    return true;
}

bool FFmpegAudioReader::DecodeNextFrame() {
    // Try to decode one frame's worth of audio for each track
    // BinkReader decodes one Bink frame at a time, which gives ~1 video frame
    // worth of audio samples per track.

    // Clear sample counts
    int samplesDecoded = 0;

    while (!mEOF) {
        int ret = av_read_frame(mFmtCtx, mPacket);
        if (ret < 0) {
            mEOF = true;
            break;
        }

        // Find which track this packet belongs to
        int trackIdx = -1;
        for (size_t i = 0; i < mTracks.size(); i++) {
            if (mTracks[i].streamIdx == mPacket->stream_index) {
                trackIdx = (int)i;
                break;
            }
        }

        if (trackIdx < 0) {
            // Not an audio packet (video or unknown stream)
            av_packet_unref(mPacket);
            continue;
        }

        ret = avcodec_send_packet(mTracks[trackIdx].codecCtx, mPacket);
        av_packet_unref(mPacket);
        if (ret < 0) continue;

        while (avcodec_receive_frame(mTracks[trackIdx].codecCtx, mFrame) == 0) {
            // Convert to 16-bit PCM mono (matching BinkReader's expected format)
            int numSamples = mFrame->nb_samples;
            auto &buf = mPCMBuffers[trackIdx];

            // Ensure buffer is large enough
            if (samplesDecoded + numSamples > (int)buf.size()) {
                buf.resize(samplesDecoded + numSamples + 4096);
            }

            // Convert from FFmpeg's native format to 16-bit PCM
            AVSampleFormat fmt = mTracks[trackIdx].codecCtx->sample_fmt;
            if (fmt == AV_SAMPLE_FMT_FLTP || fmt == AV_SAMPLE_FMT_FLT) {
                // Float to int16
                const float *src = (const float *)mFrame->data[0];
                for (int s = 0; s < numSamples; s++) {
                    float val = src[s] * 32767.0f;
                    if (val > 32767.0f) val = 32767.0f;
                    if (val < -32768.0f) val = -32768.0f;
                    buf[samplesDecoded + s] = (int16_t)val;
                }
            } else if (fmt == AV_SAMPLE_FMT_S16 || fmt == AV_SAMPLE_FMT_S16P) {
                // Already int16
                memcpy(&buf[samplesDecoded], mFrame->data[0], numSamples * 2);
            } else {
                // Unsupported format — zero fill
                memset(&buf[samplesDecoded], 0, numSamples * 2);
            }

            samplesDecoded += numSamples;
        }

        // After decoding a packet, if we got samples, return them
        if (samplesDecoded > 0) {
            mSamplesReady = samplesDecoded;
            for (size_t i = 0; i < mTracks.size(); i++) {
                mPCMPtrs[i] = mPCMBuffers[i].data();
            }
            return true;
        }
    }

    return samplesDecoded > 0;
}

void FFmpegAudioReader::Poll(float) {
    switch (mState) {
    case kFail:
        MILO_FAIL("FFmpegAudioReader::Poll() failed!");
        break;

    case kPlaying: {
        if (mSamplesReady > 0) {
            int consumed = mStream->ConsumeData(
                mPCMPtrs.data(), mSamplesReady, mSampleCurrent
            );

            mSampleCurrent += consumed;
            mSamplesReady -= consumed;

            // Advance pointers
            for (size_t i = 0; i < mTracks.size(); i++) {
                mPCMPtrs[i] = (void *)((char *)mPCMPtrs[i] + consumed * 2);
            }
        }

        if (mSamplesReady <= 0) {
            if (!DecodeNextFrame()) {
                mState = kDone;
            }
        }
        break;
    }

    case kSetup:
        mState = kPlaying;
        Init();
        break;

    case kInit: {
        if (mTracks.empty()) {
            mState = kDone;
            break;
        }
        mState = kSetup;
        break;
    }
    }
}

void FFmpegAudioReader::Seek(int targetSample) {
    if (!mFmtCtx || mState == kFail) return;
    if (mTracks.empty()) return;

    // Convert sample position to timestamp
    int sampleRate = mTracks[0].sampleRate;
    int64_t timestamp = (int64_t)targetSample * AV_TIME_BASE / sampleRate;

    int ret = av_seek_frame(mFmtCtx, -1, timestamp, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        MILO_WARN("FFmpegAudioReader::Seek failed");
        return;
    }

    // Flush all decoder buffers
    for (auto &t : mTracks) {
        avcodec_flush_buffers(t.codecCtx);
    }

    mEOF = false;
    mSamplesReady = 0;
    mSampleCurrent = targetSample;
    mSamplesJump = 0;
    mState = kPlaying;
}

void FFmpegAudioReader::Init() {
    if (!mStream) return;
    if (mTracks.empty()) return;

    // Match BinkReader::Init — tell StandardStream about track layout
    mStream->InitInfo(
        (int)mTracks.size(),
        mTracks[0].sampleRate,
        false,
        -1
    );
}
