// DC3 Native Port - FFmpegAudioReader
// Replaces BinkReader for audio-only .bik decoding using FFmpeg/libavcodec.
// BinkReader extracts 16-bit PCM mono audio tracks from .bik files for
// song preview and store preview playback.

#pragma once

#include "synth/StreamReader.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include <vector>

class File;
class StandardStream;

class FFmpegAudioReader : public StreamReader {
public:
    FFmpegAudioReader(File *file, StandardStream *stream);
    virtual ~FFmpegAudioReader();
    virtual void Poll(float) override;
    virtual void Seek(int targetSample) override;
    virtual void EnableReads(bool enable) override { mEnableReads = enable; }
    virtual bool Done() override { return mState == kDone; }
    virtual bool Fail() override { return mState == kFail; }
    virtual void Init() override;

private:
    enum State {
        kInit = 1,
        kSetup = 2,
        kPlaying = 3,
        kDone = 4,
        kFail = 5
    };

    bool OpenFile();
    bool DecodeNextFrame();

    File *mFile;
    StandardStream *mStream;
    bool mEnableReads;
    int mState;

    // FFmpeg state
    AVFormatContext *mFmtCtx;
    struct TrackInfo {
        int streamIdx;
        AVCodecContext *codecCtx;
        int sampleRate;
        int channels;
    };
    std::vector<TrackInfo> mTracks;

    // PCM decode buffers — one per track, matching BinkReader's layout
    std::vector<std::vector<int16_t>> mPCMBuffers;
    // Pointers into mPCMBuffers for ConsumeData (void* per track)
    std::vector<void *> mPCMPtrs;

    int mSamplesReady;
    unsigned int mSampleCurrent;
    unsigned int mSamplesJump;

    // Current decode packet
    AVPacket *mPacket;
    AVFrame *mFrame;
    bool mEOF;
};
