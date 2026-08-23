// DC3 Web Port - WebMovieImpl
// Video playback using browser's native <video> element + WebGPU texture upload.
// Pre-transcoded videos (BINK → WebM/MP4) are served alongside game assets.
// Uses EM_JS for JavaScript interop to control <video> and extract frames.

#pragma once

// See the note in FFmpegMovieImpl.h: DC3 spells this header MovieImpl_p.h,
// rb3-xenon spells it MovieImpl.h, and DC3's native-only shim means the _p
// spelling has to be probed first.
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
