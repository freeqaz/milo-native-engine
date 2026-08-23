// DC3 Native Port - FFmpegMovieImpl
// Replaces BinkMovieImpl for FMV/cutscene video playback using FFmpeg.
// Decodes Bink video (.bik files) via libavcodec's open-source Bink decoder,
// converts YUV frames to RGBA, and uploads to an RndTex for display.

#pragma once

// The MovieImpl base header is spelled differently by the game trees that share
// this engine, so pick the spelling that is actually present.
//
// DC3 renamed src/system/movie/MovieImpl.h to MovieImpl_p.h (dc3-decomp
// b606a4c96), on the evidence of the __FILE__ retail embedded in the asserts
// inside it. rb3-xenon still ships it under the old name. Probe the retail-
// correct _p spelling first: DC3 carries a native-only compatibility shim at
// native/include/movie/MovieImpl.h, so testing the old name first would keep
// resolving through the shim and the shim could never be retired.
#if defined(__has_include)
#if __has_include("movie/MovieImpl_p.h")
#include "movie/MovieImpl_p.h"
#define MILO_MOVIEIMPL_QUERY_CONST const
#else
#include "movie/MovieImpl.h"
#define MILO_MOVIEIMPL_QUERY_CONST
#endif
#else
#include "movie/MovieImpl.h"
#define MILO_MOVIEIMPL_QUERY_CONST
#endif

// MovieImpl::IsOpen/IsLoading are const in DC3 -- ?IsOpen@MovieImpl@@UBA_NXZ and
// ?IsLoading@MovieImpl@@UBA_NXZ in orig/373307D9/ham_xbox_r.map, corroborated by
// rb3's independent Wii decomp (Movie::Impl::IsOpen() const). rb3-xenon's
// movie/MovieImpl.h still declares them non-const; that is very likely the same
// latent bug DC3 had (a const override there silently became two EXTRA vtable
// slots instead of overriding), but fixing it belongs in that tree, so key the
// qualifier off the same header probe used above and keep `override` so either
// tree drifting fails loudly instead of silently re-growing the vtable.

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
    virtual bool IsOpen() MILO_MOVIEIMPL_QUERY_CONST override { return mOpen; }
    virtual bool IsLoading() MILO_MOVIEIMPL_QUERY_CONST override { return false; }
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
