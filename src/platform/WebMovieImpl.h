// DC3 Web Port - WebMovieImpl
// Video playback using browser's native <video> element + WebGPU texture upload.
// Pre-transcoded videos (BINK → WebM/MP4) are served alongside game assets.
// Uses EM_JS for JavaScript interop to control <video> and extract frames.

#pragma once

#include "movie/MovieImpl.h"
#include "os/Timer.h"
#include "utl/Str.h"

#include <vector>

class WebMovieImpl : public MovieImpl {
public:
    WebMovieImpl();
    virtual ~WebMovieImpl();

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
    virtual bool SetPaused(bool paused) override;
    virtual bool Paused() const override { return mPaused; }
    virtual void UnlockThread() override {}
    virtual void LockThread() override {}
    virtual int GetFrame() const override { return mCurrentFrame; }
    virtual float MsPerFrame() const override;
    virtual int NumFrames() const override { return mNumFrames; }
    virtual void SetVolume(float vol) override;

    void Terminate();

    // Show/hide the <video> element as a fullscreen overlay on the canvas.
    // Used by MoviePanel for intro/attract videos.
    void SetOverlay(bool show);

    // Accessors for texture upload
    const uint8_t* GetRGBABuffer() const { return mRGBABuffer.data(); }
    int GetDecodedWidth() const { return mVideoWidth; }
    int GetDecodedHeight() const { return mVideoHeight; }
    bool HasDecodedFrame() const { return mFrameDecoded; }

private:
    // JavaScript video element handle
    int mVideoHandle;

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

    // Requested display dimensions
    int mDisplayWidth;
    int mDisplayHeight;

    // Whether a new frame is decoded and ready for upload
    bool mFrameDecoded;
};
