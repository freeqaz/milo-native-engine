// DC3 Native Port - FFmpegMovieImpl
// Replaces BinkMovieImpl for FMV/cutscene video playback using FFmpeg.
// Decodes Bink video (.bik files) via libavcodec's open-source Bink decoder,
// converts YUV frames to RGBA, and uploads to an RndTex for display.
// The movie's selected Bink audio track plays through the engine's audio
// device (MovieAudioSource) and is the movie's clock, as Bink's own sound
// track is (G-15).

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

#include "audio/AudioDevice.h"
#include "os/Timer.h"
#include "utl/Str.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

struct SwrContext;

// Feature mark for consumers' tests: movies play their Bink sound track through
// MovieAudioSource, and the picture follows its clock.
#define MILO_MOVIE_AUDIO_SOURCE 1
// Feature mark: FFmpegMovieImpl::BeginFromMemory / MovieMemoryReader.
#define MILO_MOVIE_MEMORY 1

// A movie's sound track on the engine mixer.  The producer (the movie's Poll,
// main thread) places stereo float frames at TIMELINE positions -- frames of
// this source's own output since it started, the device clock.  The consumer
// (the audio callback) plays whatever is queued for the position it is at:
// samples that arrive late are dropped and a gap is played as silence, so a
// main-thread stall costs a dropout, never a lasting audio/picture offset.
class MovieAudioSource : public AudioSource {
public:
    MovieAudioSource(int sampleRate, int capacityFrames);

    // AudioSource (audio thread)
    virtual int RenderAudio(float *output, int frameCount) override;
    virtual bool IsFinished() const override { return false; }

    // Producer: queue `frames` interleaved stereo frames at timeline `pos`.
    // Overlap with what is queued already is discarded; a gap is filled with
    // silence.  Returns the frames accepted (fewer only if the ring is full).
    int Write(int64_t pos, const float *data, int frames);
    // Producer: discard everything queued at or after timeline `pos`.
    void TruncateFrom(int64_t pos);

    int SampleRate() const { return mSampleRate; }
    // The device clock: frames this source has rendered while unpaused.
    int64_t PlayedFrames() const { return mPlayed.load(std::memory_order_acquire); }
    void SetGain(float gain) { mGain.store(gain, std::memory_order_relaxed); }
    float Gain() const { return mGain.load(std::memory_order_relaxed); }
    void SetPaused(bool paused) { mPaused.store(paused, std::memory_order_release); }

    // Probes (tests): rendered frames that carried queued audio, frames
    // dropped for arriving late, and frames rendered as silence.
    int64_t AudioFramesRendered() const { return mAudioRendered.load(); }
    int64_t FramesDroppedLate() const { return mDroppedLate.load(); }
    int64_t SilenceFramesRendered() const { return mSilenceRendered.load(); }

private:
    void PopLocked(int frames);

    const int mSampleRate;
    std::mutex mLock;
    std::vector<float> mRing; // interleaved stereo
    int mCapacity;            // frames
    int mHead;                // ring index of the oldest queued frame
    int mCount;               // frames queued
    int64_t mHeadPos;         // timeline position of the oldest queued frame
    std::atomic<int64_t> mPlayed;
    std::atomic<float> mGain;
    std::atomic<bool> mPaused;
    std::atomic<int64_t> mAudioRendered;
    std::atomic<int64_t> mDroppedLate;
    std::atomic<int64_t> mSilenceRendered;
};

// A movie held in memory, as the demuxer reads it (AVIO read/seek).
//
// Retail opens a PRELOADED movie from its buffer, not from its file: RB3 TU5's
// Movie::Impl::CheckOpen (band.exe fn_82745428) keeps the FileLoader's buffer
// as mPreloadBuf and calls MovieOpen(mPreloadBuf, 0x4000400), whose BinkOpen
// gets BINKFROMMEMORY (0x04000000) | BINKNOFRAMEBUFFERS (0x400).  That buffer
// may be the only copy: a cached .milo embeds the movie's bytes in the object
// (TexMovie's Load hands Begin its stream; FileLoader reads the bytes out of
// it), and there is no file to open.
//
// Byte order.  Bink on the 360 reads the file as native (big-endian) 32-bit
// words, so the Xbox data is word-swapped: the tag reads "iKIB", not "BIKi".
// Retail swaps a PC-order buffer ("BIKi") the other way before BinkOpen
// (CheckOpen's EndianSwapBuffer).  FFmpeg's bink demuxer and decoder read the
// PC order, so a buffer whose first word is a byte-reversed Bink tag is read
// through a word-unswapping view: the caller's bytes are never modified (a
// cached Save writes them back as they were loaded).  A swapped buffer must be
// a whole number of words, as EndianSwapBuffer asserts.
//
// Lifetime: the reader does NOT copy.  `data` must stay valid and unchanged for
// as long as the reader is in use.
class MovieMemoryReader {
public:
    MovieMemoryReader(const void *data, int64_t size);

    // False for a null/empty buffer, or a swapped one that is not whole words.
    bool Valid() const { return mValid; }
    // The buffer is in the 360's word-swapped order (first word "iKIB"/"2BK...").
    bool WordSwapped() const { return mSwapped; }
    int64_t Size() const { return mSize; }
    int64_t Pos() const { return mPos; }
    // AVIO read: up to `size` bytes at the position, in PC byte order;
    // AVERROR_EOF at the end.
    int Read(uint8_t *buf, int size);
    // AVIO seek: SEEK_SET/SEEK_CUR/SEEK_END (AVSEEK_FORCE ignored); AVSEEK_SIZE
    // returns the size.  A target outside [0, size] fails with AVERROR(EINVAL)
    // and leaves the position where it was.
    int64_t Seek(int64_t offset, int whence);

    // True when the first four bytes are a Bink 1 ("BIK") or Bink 2 ("KB2") tag
    // with its bytes reversed.
    static bool IsWordSwappedBink(const void *data, int64_t size);

private:
    const uint8_t *mData;
    int64_t mSize;
    int64_t mPos;
    bool mSwapped;
    bool mValid;
};

class FFmpegMovieImpl : public MovieImpl {
public:
    FFmpegMovieImpl();
    virtual ~FFmpegMovieImpl();

    virtual void SetWidthHeight(int w, int h) override;
    virtual bool Ready() const override;
    // Arguments as BinkMovieImpl::BeginFromFile takes them (and as TexMovie
    // and MoviePanel pass them): path, volume (unused there too), noSound,
    // loop, preload, fillWidth, localization track, stream, loader pos.
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
    virtual void SetVolume(float db) override;

    // Open a movie held in memory (retail: MovieOpen(mPreloadBuf, BINKFROMMEMORY
    // | BINKNOFRAMEBUFFERS) for a preloaded movie, see MovieMemoryReader).
    // `name` only names the movie in messages and helps FFmpeg's probe; noSound,
    // loop and track are BeginFromFile's.  Returns false, with nothing open, for
    // a null/empty/non-word buffer or bytes FFmpeg cannot open as a movie.
    //
    // Lifetime: the bytes are NOT copied.  `data` must stay valid and unchanged
    // until End(), the next BeginFromFile/BeginFromMemory, or destruction --
    // retail's own contract: Impl::End closes the HBINK (MovieClose) before it
    // MemFrees mPreloadBuf.
    bool BeginFromMemory(const void *data, int size, const char *name, bool noSound,
                         bool loop, int track);
    // The movie open now was opened by BeginFromMemory (probe for consumers'
    // gates), and the reader's view of it.
    bool OpenFromMemory() const { return mOpen && mMemory != nullptr; }
    const MovieMemoryReader *MemoryReader() const { return mMemory; }

    void Terminate();

    // Tests: when non-zero, a movie's sound track goes to a MovieAudioSource
    // at this rate that is NOT added to the audio device; the test pulls it
    // with RenderAudio(), standing in for the device callback.
    static void SetDetachedAudioRateForTest(int rate) { sDetachedAudioRate = rate; }
    MovieAudioSource *AudioOut() const { return mAudioOut; }
    // Which audio stream of the file plays (-1: none), and the Bink track
    // index it stands for.
    int AudioStreamIndex() const { return mAudioStreamIdx; }
    int AudioTrack() const { return mAudioTrack; }
    bool Looping() const { return mLoop; }

private:
    bool FinishBegin(const char *name, bool noSound, bool loop, int track);
    bool OpenVideo(const char *path);
    bool OpenStreams(const char *name);
    bool OpenAudio(int track);
    bool ReadPacket();
    void DecodeAudioPacket(AVPacket *pkt);
    void WriteAudio(const float *frames, int count);
    void FlushAudio();
    void PumpAudio();
    bool DecodeNextVideoFrame();
    void RestartForLoop();
    double AbsClockMs();
    void ClearVideoQueue();
    void Close();

    // The game file the demuxer reads, and the AVIOContext over it.  Movies
    // are game data like any other: "videos/intro.bik" lives in the ark (and
    // under DC3_DATA), so it is opened through NewFile -- the archive / data
    // dir / overlay resolution every other asset read uses -- never handed to
    // FFmpeg as a path relative to the working directory (PLAN G-15).
    class File *mFile;
    AVIOContext *mAvio;
    // Or the caller's buffer, for a movie opened by BeginFromMemory (the
    // AVIO's opaque; owned here, the bytes it views are not).
    MovieMemoryReader *mMemory;

    // FFmpeg state
    AVFormatContext *mFmtCtx;
    AVCodecContext *mVideoCtx;
    int mVideoStreamIdx;
    SwsContext *mSwsCtx;
    AVPacket *mPacket;
    AVFrame *mAvFrame;

    // Sound track: decoder, resampler to the mixer's format, the source on
    // the mixer, and the video packets read ahead while filling it.
    AVCodecContext *mAudioCtx;
    int mAudioStreamIdx;
    int mAudioTrack;
    SwrContext *mSwr;
    AVFrame *mAudioFrame;
    MovieAudioSource *mAudioOut;
    bool mAudioOnDevice;
    std::deque<AVPacket *> mVideoQueue;
    bool mDemuxEOF;
    int64_t mAudioWritePos; // timeline position of the next decoded frame
    std::vector<float> mAudioScratch;
    float mVolumeGain; // persists across movies, as BinkMovieImpl::mVolume

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

    // Clock.  With a sound track on the device the clock is the audio
    // source's played frames; otherwise (headless, no track, noSound) wall
    // time from mPlayTimer, less time spent paused.
    Timer mPlayTimer;
    Timer mPauseTimer;
    double mWallBaseMs;
    double mPausedTotalMs;
    double mLoopStartMs; // absolute clock at which the current pass began
    bool mAudioClock;
    int64_t mLastPlayed;
    Timer mStallTimer;

    // Requested display dimensions
    int mDisplayWidth;
    int mDisplayHeight;

    // Whether a new frame is decoded and ready for upload
    bool mFrameDecoded;

    static int sDetachedAudioRate;

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
