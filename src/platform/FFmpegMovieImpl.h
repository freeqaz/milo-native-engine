// DC3 Native Port - FFmpegMovieImpl
// Replaces BinkMovieImpl for FMV/cutscene video playback using FFmpeg.
// Decodes Bink video (.bik files) via libavcodec's open-source Bink decoder,
// converts YUV frames to RGBA, and uploads to an RndTex for display.

#pragma once

#include "movie/MovieImpl.h"
#include "os/Timer.h"
#include "utl/Str.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include <vector>

class FFmpegMovieImpl : public MovieImpl {
public:
    FFmpegMovieImpl();
    virtual ~FFmpegMovieImpl();

    virtual void SetWidthHeight(int w, int h) override;
    virtual bool Ready() const override;
    virtual bool BeginFromFile(
        char const *, float, bool, bool, bool, bool, int, BinStream *, LoaderPos
    ) override;
    virtual void Draw() override;
    virtual bool Poll() override;
    virtual void Save(BinStream *) override {}
    virtual void End() override;
    virtual bool IsOpen() override { return mOpen; }
    virtual bool IsLoading() override { return false; }
    virtual bool CheckOpen(bool) override;
    virtual void SetPaused(bool paused) override;
    virtual bool Paused() const override { return mPaused; }
    virtual void UnlockThread() override {}
    virtual void LockThread() override {}
    virtual int GetFrame() const override { return mCurrentFrame; }
    virtual float MsPerFrame() const override;
    virtual int NumFrames() const override { return mNumFrames; }
    virtual void SetVolume(float) override {}

    void Terminate();

private:
    bool OpenVideo(const char *path);
    bool DecodeNextVideoFrame();
    void Close();

    // FFmpeg state
    AVFormatContext *mFmtCtx;
    AVCodecContext *mVideoCtx;
    int mVideoStreamIdx;
    SwsContext *mSwsCtx;
    AVPacket *mPacket;
    AVFrame *mAvFrame;

    // Decoded RGBA pixel buffer
    std::vector<uint8_t> mRGBABuffer;
    int mVideoWidth;
    int mVideoHeight;

    // Playback state
    String mFilename;
    bool mOpen;
    bool mLoop;
    bool mPaused;
    bool mReady;
    int mCurrentFrame;
    int mNumFrames;
    float mFrameRate;
    Timer mPlayTimer;

    // Requested display dimensions
    int mDisplayWidth;
    int mDisplayHeight;

    // Whether a new frame is decoded and ready for upload
    bool mFrameDecoded;

public:
    // Accessors for native render-to-texture upload
    const uint8_t* GetRGBABuffer() const { return mRGBABuffer.data(); }
    int GetDecodedWidth() const { return mVideoWidth; }
    int GetDecodedHeight() const { return mVideoHeight; }
    bool HasDecodedFrame() const { return mFrameDecoded; }

    // Virtual time: override wall-clock timer for headless/capture rendering.
    // When enabled, Poll() uses the provided time instead of mPlayTimer.
    void SetVirtualTime(float ms) { mVirtualTimeMs = ms; mUseVirtualTime = true; }
    void ClearVirtualTime() { mUseVirtualTime = false; }

private:
    float mVirtualTimeMs = 0.0f;
    bool  mUseVirtualTime = false;
};
