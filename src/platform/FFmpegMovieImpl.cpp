// DC3 Native Port - FFmpegMovieImpl
// Replaces BinkMovieImpl for FMV/cutscene video playback using FFmpeg.
//
// Flow: BeginFromFile → OpenVideo/OpenAudio → Poll (decode frames on the
// movie clock) → Draw (upload RGBA).
//
// Sound (G-15).  The image's BinkMovieImpl plays a movie's sound through
// Bink's XAudio2 mixer (BinkMovieSys_Xbox: BinkSetSoundSystem(BinkOpenXAudio2,
// ...)), and Bink paces the picture against that sound track.  Which track:
// MovieOpen calls BinkSetSoundTrack(1, {track - 1}) when the localization
// track argument is non-zero (overridden by `set_bink_track`), else Bink plays
// the file's first track; FinishOpen calls BinkSetSoundOnOff(!noSound).  The
// level is SetVolume(db) -> BinkSetVolume(DbToRatio(db) * 0x8000, at most
// 0x8000), which Movie::Poll pushes from the movie's FaderGroup (its own
// fader, the master fader, and e.g. SongPreview's faders).  Here the chosen
// track is decoded alongside the picture, resampled to the engine mixer's
// format and played by a MovieAudioSource on the AudioDevice, and that
// source's played-frame count is the clock the picture follows.

#include "platform/FFmpegMovieImpl.h"
#include "math/Decibels.h"
#include "moviebink/BinkMovieSys.h"
#include "os/Debug.h"
#include "os/File.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cstdio>
#include <cstring>

int FFmpegMovieImpl::sDetachedAudioRate = 0;

// How far ahead of the device the sound track is decoded, and how much the
// ring can hold.  Bink interleaves each frame's audio with its picture, so
// filling the lead means reading a few video packets ahead; they wait in
// mVideoQueue, bounded by kMaxQueuedVideo.
static const double kAudioLeadSeconds = 1.0;
static const double kAudioRingSeconds = 4.0;
static const size_t kMaxQueuedVideo = 120;
// The device callback has not advanced the clock for this long while the
// movie plays: treat the device as gone and continue on wall time.
static const float kAudioStallMs = 500.0f;

// ---------------------------------------------------------------------------
// MovieAudioSource
// ---------------------------------------------------------------------------

MovieAudioSource::MovieAudioSource(int sampleRate, int capacityFrames)
    : mSampleRate(sampleRate), mRing((size_t)capacityFrames * 2, 0.0f),
      mCapacity(capacityFrames), mHead(0), mCount(0), mHeadPos(0), mPlayed(0),
      mGain(1.0f), mPaused(false), mAudioRendered(0), mDroppedLate(0),
      mSilenceRendered(0) {}

void MovieAudioSource::PopLocked(int frames) {
    mHead = (mHead + frames) % mCapacity;
    mCount -= frames;
    mHeadPos += frames;
}

int MovieAudioSource::Write(int64_t pos, const float *data, int frames) {
    std::lock_guard<std::mutex> lock(mLock);
    if (mCount == 0)
        mHeadPos = pos;
    int64_t tail = mHeadPos + mCount;
    int skip = 0;
    if (pos < tail) {
        // Already queued (overlap): keep what is there.
        skip = (int)std::min<int64_t>(frames, tail - pos);
    } else if (pos > tail) {
        int64_t gap = pos - tail;
        if (gap >= mCapacity - mCount) {
            // A gap the ring cannot hold: nothing queued can still play
            // before it, so start over at pos.
            mHead = 0;
            mCount = 0;
            mHeadPos = pos;
        } else {
            for (int64_t i = 0; i < gap; i++) {
                int idx = (mHead + mCount) % mCapacity;
                mRing[idx * 2] = 0.0f;
                mRing[idx * 2 + 1] = 0.0f;
                mCount++;
            }
        }
    }
    int n = std::min(frames - skip, mCapacity - mCount);
    for (int i = 0; i < n; i++) {
        int idx = (mHead + mCount) % mCapacity;
        mRing[idx * 2] = data[(skip + i) * 2];
        mRing[idx * 2 + 1] = data[(skip + i) * 2 + 1];
        mCount++;
    }
    return skip + n;
}

void MovieAudioSource::TruncateFrom(int64_t pos) {
    std::lock_guard<std::mutex> lock(mLock);
    if (pos <= mHeadPos) {
        mCount = 0;
        mHeadPos = pos;
    } else if (pos < mHeadPos + mCount) {
        mCount = (int)(pos - mHeadPos);
    }
}

int MovieAudioSource::RenderAudio(float *output, int frameCount) {
    if (mPaused.load(std::memory_order_acquire)) {
        memset(output, 0, sizeof(float) * 2 * frameCount);
        return frameCount; // a paused movie holds its clock
    }
    std::lock_guard<std::mutex> lock(mLock);
    int64_t P = mPlayed.load(std::memory_order_relaxed);
    // Queued audio whose time has passed is dropped, not played late.
    if (mCount > 0 && mHeadPos < P) {
        int drop = (int)std::min<int64_t>(P - mHeadPos, mCount);
        PopLocked(drop);
        mDroppedLate += drop;
    }
    int out = 0;
    int silence = 0;
    if (mCount > 0 && mHeadPos > P) {
        int lead = (int)std::min<int64_t>(mHeadPos - P, frameCount);
        memset(output, 0, sizeof(float) * 2 * lead);
        out += lead;
        silence += lead;
    }
    float gain = mGain.load(std::memory_order_relaxed);
    int n = std::min(mCount, frameCount - out);
    for (int i = 0; i < n; i++) {
        int idx = (mHead + i) % mCapacity;
        output[(out + i) * 2] = mRing[idx * 2] * gain;
        output[(out + i) * 2 + 1] = mRing[idx * 2 + 1] * gain;
    }
    PopLocked(n);
    out += n;
    mAudioRendered += n;
    if (out < frameCount) {
        memset(output + out * 2, 0, sizeof(float) * 2 * (frameCount - out));
        silence += frameCount - out;
    }
    mSilenceRendered += silence;
    mPlayed.store(P + frameCount, std::memory_order_release);
    return frameCount;
}

// ---------------------------------------------------------------------------
// File IO
// ---------------------------------------------------------------------------

// AVIO callbacks over a game File (ArkFile out of the archive, AsyncFile for a
// loose or overlay file): the same reads the image's FileLoader/BinkMovieLoader
// make, here driven by the demuxer.
static int MovieFileRead(void *opaque, uint8_t *buf, int size) {
    File *file = static_cast<File *>(opaque);
    int got = file->Read(buf, size);
    if (got <= 0)
        return AVERROR_EOF;
    return got;
}

static int64_t MovieFileSeek(void *opaque, int64_t offset, int whence) {
    File *file = static_cast<File *>(opaque);
    if (whence & AVSEEK_SIZE)
        return file->Size();
    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET:
        return file->Seek((int)offset, 0);
    case SEEK_CUR:
        return file->Seek((int)offset, 1);
    case SEEK_END:
        return file->Seek((int)offset, 2);
    default:
        return -1;
    }
}

// ---------------------------------------------------------------------------
// FFmpegMovieImpl
// ---------------------------------------------------------------------------

FFmpegMovieImpl::FFmpegMovieImpl()
    : mFile(nullptr), mAvio(nullptr), mFmtCtx(nullptr), mVideoCtx(nullptr), mVideoStreamIdx(-1),
      mSwsCtx(nullptr), mPacket(nullptr), mAvFrame(nullptr),
      mAudioCtx(nullptr), mAudioStreamIdx(-1), mAudioTrack(-1), mSwr(nullptr),
      mAudioFrame(nullptr), mAudioOut(nullptr), mAudioOnDevice(false), mDemuxEOF(false),
      mAudioWritePos(0), mVolumeGain(1.0f),
      mVideoWidth(0), mVideoHeight(0),
      mOpen(false), mLoop(false), mPaused(false), mReady(false),
      mCurrentFrame(0), mNumFrames(0), mFrameRate(30.0f),
      mWallBaseMs(0), mPausedTotalMs(0), mLoopStartMs(0), mAudioClock(false),
      mLastPlayed(0),
      mDisplayWidth(0), mDisplayHeight(0), mFrameDecoded(false) {}

FFmpegMovieImpl::~FFmpegMovieImpl() {
    Close();
}

void FFmpegMovieImpl::SetWidthHeight(int w, int h) {
    mDisplayWidth = w;
    mDisplayHeight = h;
}

// The image's BinkMovieImpl::Ready (DC3 82E221C8) reports whether an async load
// is still pending -- mLoader / mMovieLoader IsLoaded(), and true with no loader.
// This impl opens synchronously in BeginFromFile, so nothing is ever pending:
// ready before any movie, after a successful open, and after a failed one.
// (mReady, "a video is open and decodable", stays internal.)
bool FFmpegMovieImpl::Ready() const {
    return true;
}

// BinkMovieImpl::BeginFromFile(path, volume, b1, b2, b3, b4, track, ...):
// b1 -> unk27, the BinkSetSoundOnOff(!unk27) flag (MoviePanel passes !mAudio);
// b2 -> unk26, the keep-playing-past-the-last-frame flag Poll and DoFrame test
// (TexMovie passes mLoop, MoviePanel mLoop); b3 preload; b4 fill width.  This
// impl used to read b1 as the loop flag, so a song preview (TexMovie: b1 = 0,
// b2 = loop) never looped.
bool FFmpegMovieImpl::BeginFromFile(
    char const *path, float /*volume: unused by the image too*/, bool noSound, bool loop,
    bool /*preload*/, bool /*fillWidth*/, int track, BinStream * /*bs*/, LoaderPos /*pos*/
) {
    Close();

    mFilename = path;
    mLoop = loop;

    if (!OpenVideo(path)) {
        MILO_WARN("FFmpegMovieImpl: failed to open %s", path);
        return false;
    }

    mReady = true;
    mOpen = true;
    mCurrentFrame = 0;
    mPaused = false;
    mDemuxEOF = false;
    mWallBaseMs = 0;
    mPausedTotalMs = 0;
    mLoopStartMs = 0;
    mPlayTimer.Restart();

    // MovieOpen: a non-zero localization track selects Bink track
    // (set_bink_track's override, else the argument) - 1; zero leaves Bink on
    // its first track.
    int binkTrack = 0;
    if (track != 0) {
        int t = TheBinkMovieSys.Track() ? TheBinkMovieSys.Track() : track;
        binkTrack = t - 1;
    }
    if (!noSound)
        OpenAudio(binkTrack);

    printf("FFmpegMovieImpl: opened %s (%dx%d, %d frames at %.2f fps, sound %s, track %d)\n",
           path, mVideoWidth, mVideoHeight, mNumFrames, mFrameRate,
           mAudioOut ? (mAudioOnDevice ? "on device" : "detached") : "off", mAudioTrack);

    return true;
}

bool FFmpegMovieImpl::OpenVideo(const char *path) {
    // An absolute path (tests, tools) is cwd-independent already: FFmpeg's own
    // file protocol opens it.  Anything else is a game path, resolved by the
    // File layer exactly as every other asset read is.
    int ret;
    if (path[0] == '/') {
        ret = avformat_open_input(&mFmtCtx, path, nullptr, nullptr);
    } else {
        mFile = NewFile(path, 2);
        if (!mFile) {
            String where;
            FileQualifiedFilename(where, path);
            MILO_WARN("FFmpegMovieImpl: %s is not in the archive or data dir (%s)",
                      path, where.c_str());
            return false;
        }
        const int kAvioBufSize = 64 * 1024;
        unsigned char *avioBuf = (unsigned char *)av_malloc(kAvioBufSize);
        mAvio = avio_alloc_context(avioBuf, kAvioBufSize, 0, mFile,
                                   MovieFileRead, nullptr, MovieFileSeek);
        mFmtCtx = avformat_alloc_context();
        if (!mAvio || !mFmtCtx) {
            Close();
            return false;
        }
        mFmtCtx->pb = mAvio;
        mFmtCtx->flags |= AVFMT_FLAG_CUSTOM_IO;
        // The url only names the stream in FFmpeg's own messages and helps
        // the probe; every byte comes through mAvio.
        ret = avformat_open_input(&mFmtCtx, path, nullptr, nullptr);
    }
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        MILO_WARN("FFmpegMovieImpl: avformat_open_input(%s): %s", path, errbuf);
        Close(); // avformat_open_input freed mFmtCtx; the custom IO is ours
        return false;
    }

    ret = avformat_find_stream_info(mFmtCtx, nullptr);
    if (ret < 0) {
        Close();
        return false;
    }

    // Find video stream
    mVideoStreamIdx = av_find_best_stream(
        mFmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0
    );
    if (mVideoStreamIdx < 0) {
        MILO_WARN("FFmpegMovieImpl: no video stream found");
        Close();
        return false;
    }

    AVStream *vs = mFmtCtx->streams[mVideoStreamIdx];
    const AVCodec *codec = avcodec_find_decoder(vs->codecpar->codec_id);
    if (!codec) {
        MILO_WARN("FFmpegMovieImpl: no decoder for codec %d", vs->codecpar->codec_id);
        Close();
        return false;
    }

    mVideoCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(mVideoCtx, vs->codecpar);
    ret = avcodec_open2(mVideoCtx, codec, nullptr);
    if (ret < 0) {
        Close();
        return false;
    }

    mVideoWidth = mVideoCtx->width;
    mVideoHeight = mVideoCtx->height;

    // Frame count and rate
    mNumFrames = (int)vs->nb_frames;
    if (vs->avg_frame_rate.den > 0) {
        mFrameRate = (float)vs->avg_frame_rate.num / (float)vs->avg_frame_rate.den;
    }
    // FFmpeg's bink demuxer leaves nb_frames unset (ffprobe: N/A) and puts the
    // header's frame count in the stream duration instead.  Splash::Poll times
    // a movie as MsPerFrame() * NumFrames(), so 0 here reads as a zero-length
    // movie.
    if (mNumFrames <= 0 && vs->duration != AV_NOPTS_VALUE && vs->duration > 0
        && mFrameRate > 0.0f) {
        mNumFrames = (int)(vs->duration * av_q2d(vs->time_base) * mFrameRate + 0.5);
    }

    // Create scaler for YUV→RGBA
    mSwsCtx = sws_getContext(
        mVideoWidth, mVideoHeight, mVideoCtx->pix_fmt,
        mVideoWidth, mVideoHeight, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    if (!mSwsCtx) {
        MILO_WARN("FFmpegMovieImpl: sws_getContext failed");
        Close();
        return false;
    }

    // Allocate RGBA buffer
    mRGBABuffer.resize(mVideoWidth * mVideoHeight * 4);

    mPacket = av_packet_alloc();
    mAvFrame = av_frame_alloc();

    // Until a sound track is chosen, no audio stream is demuxed.
    for (unsigned int i = 0; i < mFmtCtx->nb_streams; i++) {
        if ((int)i != mVideoStreamIdx)
            mFmtCtx->streams[i]->discard = AVDISCARD_ALL;
    }

    return true;
}

// Bink track `track` is the file's track-th audio stream: the header's track
// ids are 0..n-1 in order (intro.bik: 15 tracks, ids 0..14).
bool FFmpegMovieImpl::OpenAudio(int track) {
    int rate = sDetachedAudioRate;
    bool onDevice = false;
    if (rate <= 0) {
        AudioDevice &dev = AudioDevice::GetInstance();
        if (!dev.IsInitialized() || dev.GetSampleRate() <= 0)
            return false; // headless / DC3_NO_AUDIO: nothing to play to
        rate = dev.GetSampleRate();
        onDevice = true;
    }

    int nth = 0;
    int streamIdx = -1;
    int numTracks = 0;
    for (unsigned int i = 0; i < mFmtCtx->nb_streams; i++) {
        if (mFmtCtx->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;
        if (nth == track)
            streamIdx = (int)i;
        nth++;
        numTracks++;
    }
    if (numTracks == 0)
        return false; // a silent movie (campaign_intro.bik)
    if (streamIdx < 0) {
        // MovieOpen asserts track - 1 < NumTracks.
        MILO_WARN("FFmpegMovieImpl: %s has %d sound tracks, track %d asked for",
                  mFilename.c_str(), numTracks, track);
        return false;
    }

    AVStream *as = mFmtCtx->streams[streamIdx];
    const AVCodec *codec = avcodec_find_decoder(as->codecpar->codec_id);
    if (!codec) {
        MILO_WARN("FFmpegMovieImpl: no decoder for audio codec %d", as->codecpar->codec_id);
        return false;
    }
    mAudioCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(mAudioCtx, as->codecpar);
    if (avcodec_open2(mAudioCtx, codec, nullptr) < 0) {
        avcodec_free_context(&mAudioCtx);
        return false;
    }

    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&mSwr, &outLayout, AV_SAMPLE_FMT_FLT, rate,
                            &mAudioCtx->ch_layout, mAudioCtx->sample_fmt,
                            mAudioCtx->sample_rate, 0, nullptr) < 0
        || swr_init(mSwr) < 0) {
        MILO_WARN("FFmpegMovieImpl: cannot resample %d Hz sound to %d Hz",
                  mAudioCtx->sample_rate, rate);
        swr_free(&mSwr);
        avcodec_free_context(&mAudioCtx);
        return false;
    }

    mAudioStreamIdx = streamIdx;
    mAudioTrack = track;
    as->discard = AVDISCARD_DEFAULT;
    mAudioFrame = av_frame_alloc();
    mAudioOut = new MovieAudioSource(rate, (int)(rate * kAudioRingSeconds));
    mAudioOut->SetGain(mVolumeGain);
    mAudioWritePos = 0;
    mLastPlayed = 0;
    mStallTimer.Restart();

    // Fill the lead before the source goes live, so the first callback
    // already has the track's first samples.
    PumpAudio();

    mAudioOnDevice = onDevice;
    mAudioClock = true;
    if (onDevice)
        AudioDevice::GetInstance().AddSource(mAudioOut);
    return true;
}

// One demuxed packet: video waits in the queue for DecodeNextVideoFrame,
// the sound track is decoded into the ring now, anything else is dropped.
bool FFmpegMovieImpl::ReadPacket() {
    if (mDemuxEOF)
        return false;
    int ret = av_read_frame(mFmtCtx, mPacket);
    if (ret < 0) {
        mDemuxEOF = true;
        FlushAudio();
        return false;
    }
    if (mPacket->stream_index == mVideoStreamIdx) {
        AVPacket *queued = av_packet_alloc();
        av_packet_move_ref(queued, mPacket);
        mVideoQueue.push_back(queued);
    } else if (mPacket->stream_index == mAudioStreamIdx && mAudioCtx) {
        DecodeAudioPacket(mPacket);
        av_packet_unref(mPacket);
    } else {
        av_packet_unref(mPacket);
    }
    return true;
}

void FFmpegMovieImpl::WriteAudio(const float *frames, int count) {
    if (count <= 0)
        return;
    int took = mAudioOut->Write(mAudioWritePos, frames, count);
    if (took < count) {
        static int sWarned = 0;
        if (sWarned++ < 4)
            MILO_WARN("FFmpegMovieImpl: movie sound ring full, %d frames lost",
                      count - took);
    }
    mAudioWritePos += count;
}

void FFmpegMovieImpl::DecodeAudioPacket(AVPacket *pkt) {
    if (avcodec_send_packet(mAudioCtx, pkt) < 0)
        return;
    while (avcodec_receive_frame(mAudioCtx, mAudioFrame) == 0) {
        int outMax = swr_get_out_samples(mSwr, mAudioFrame->nb_samples);
        if (outMax <= 0) {
            av_frame_unref(mAudioFrame);
            continue;
        }
        if ((int)mAudioScratch.size() < outMax * 2)
            mAudioScratch.resize(outMax * 2);
        uint8_t *out[1] = {(uint8_t *)mAudioScratch.data()};
        int got = swr_convert(mSwr, out, outMax,
                              (const uint8_t **)mAudioFrame->extended_data,
                              mAudioFrame->nb_samples);
        av_frame_unref(mAudioFrame);
        if (got > 0)
            WriteAudio(mAudioScratch.data(), got);
    }
}

// End of the file: drain the resampler's delay line.
void FFmpegMovieImpl::FlushAudio() {
    if (!mSwr || !mAudioOut)
        return;
    int outMax = swr_get_out_samples(mSwr, 0);
    if (outMax <= 0)
        return;
    if ((int)mAudioScratch.size() < outMax * 2)
        mAudioScratch.resize(outMax * 2);
    uint8_t *out[1] = {(uint8_t *)mAudioScratch.data()};
    int got = swr_convert(mSwr, out, outMax, nullptr, 0);
    if (got > 0)
        WriteAudio(mAudioScratch.data(), got);
}

void FFmpegMovieImpl::PumpAudio() {
    if (!mAudioOut)
        return;
    int64_t lead = (int64_t)(mAudioOut->SampleRate() * kAudioLeadSeconds);
    while (!mDemuxEOF && mAudioWritePos - mAudioOut->PlayedFrames() < lead
           && mVideoQueue.size() < kMaxQueuedVideo) {
        if (!ReadPacket())
            break;
    }
}

bool FFmpegMovieImpl::DecodeNextVideoFrame() {
    while (true) {
        if (mVideoQueue.empty()) {
            if (!ReadPacket())
                return false; // EOF
            continue;
        }
        AVPacket *pkt = mVideoQueue.front();
        mVideoQueue.pop_front();
        int ret = avcodec_send_packet(mVideoCtx, pkt);
        av_packet_free(&pkt);
        if (ret < 0) continue;

        ret = avcodec_receive_frame(mVideoCtx, mAvFrame);
        if (ret == 0) {
            // Convert to RGBA
            int rgbaStride = mVideoWidth * 4;
            uint8_t *dstSlice[1] = {mRGBABuffer.data()};
            int dstStride[1] = {rgbaStride};
            sws_scale(mSwsCtx, mAvFrame->data, mAvFrame->linesize,
                      0, mVideoHeight, dstSlice, dstStride);

            mCurrentFrame++;
            mFrameDecoded = true;
            return true;
        }
    }
}

// The movie clock, ms, before the current pass's start is subtracted.
double FFmpegMovieImpl::AbsClockMs() {
    if (mUseVirtualTime)
        return mVirtualTimeMs;
    if (mAudioOut && mAudioClock) {
        int64_t played = mAudioOut->PlayedFrames();
        double audioMs = played * 1000.0 / mAudioOut->SampleRate();
        if (played != mLastPlayed || mPaused) {
            mLastPlayed = played;
            mStallTimer.Restart();
        } else if (mAudioOnDevice && mStallTimer.SplitMs() > kAudioStallMs) {
            MILO_WARN("FFmpegMovieImpl: audio device stopped; %s continues on wall time",
                      mFilename.c_str());
            mAudioClock = false;
            mWallBaseMs = audioMs;
            mPausedTotalMs = 0;
            mPlayTimer.Restart();
        }
        if (mAudioClock)
            return audioMs;
    }
    return mWallBaseMs + mPlayTimer.SplitMs() - mPausedTotalMs;
}

void FFmpegMovieImpl::ClearVideoQueue() {
    for (AVPacket *pkt : mVideoQueue)
        av_packet_free(&pkt);
    mVideoQueue.clear();
}

// Bink keeps playing past the last frame when told to loop (unk26): seek both
// streams back to the start; the next pass begins at the current clock.
void FFmpegMovieImpl::RestartForLoop() {
    av_seek_frame(mFmtCtx, mVideoStreamIdx, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(mVideoCtx);
    ClearVideoQueue();
    mDemuxEOF = false;
    mCurrentFrame = 0;
    mLoopStartMs = AbsClockMs();
    if (mAudioOut) {
        avcodec_flush_buffers(mAudioCtx);
        swr_close(mSwr);
        swr_init(mSwr);
        // The pass starts now on the device clock; whatever the last pass
        // queued beyond that is the old pass's tail.
        int64_t start = mAudioClock ? mAudioOut->PlayedFrames()
                                    : (int64_t)(mLoopStartMs * mAudioOut->SampleRate() / 1000.0);
        mAudioOut->TruncateFrom(start);
        mAudioWritePos = start;
    }
}

bool FFmpegMovieImpl::Poll() {
    // Convention: return true = still playing, false = done/ended
    // (TexMovie::Poll checks `if (!mMovie.Poll()) mMovie.End()`)
    //
    // A movie that is not open is DONE, as in the image: BinkMovieImpl::Poll
    // returns false when there is no HBINK (the `mBink && mInternalBufs` test
    // fails and it falls to `return false`), so MoviePanel::Poll fires
    // movie_done for a video that failed to open -- attract_screen goes on to
    // its next_screen.  Returning true here reported a missing .bik as
    // "still playing" forever.
    if (!mOpen) return false;
    if (mPaused) return true;

    // Check if it's time for the next frame
    double elapsed = AbsClockMs() - mLoopStartMs;
    float msPerFrame = MsPerFrame();
    int targetFrame = (int)(elapsed / msPerFrame);

    while (mCurrentFrame <= targetFrame) {
        if (!DecodeNextVideoFrame()) {
            if (mLoop) {
                RestartForLoop();
                PumpAudio();
                return true;
            }
            return false; // Video ended
        }
    }

    PumpAudio();
    return true; // Still playing
}

void FFmpegMovieImpl::Draw() {
    if (!mFrameDecoded) return;

    // In the full engine, this uploads mRGBABuffer to an RndTex.
    // For now the pixel data is available in mRGBABuffer for the
    // graphics backend to consume.
    mFrameDecoded = false;
}

void FFmpegMovieImpl::End() {
    Close();
}

bool FFmpegMovieImpl::CheckOpen(bool) {
    return mOpen;
}

// BinkPause stops picture and sound together; the clock stops with them.
void FFmpegMovieImpl::SetPaused(bool paused) {
    if (paused == mPaused)
        return;
    if (paused)
        mPauseTimer.Restart();
    else
        mPausedTotalMs += mPauseTimer.SplitMs();
    mPaused = paused;
    if (mAudioOut)
        mAudioOut->SetPaused(paused);
}

// BinkMovieImpl::SetVolume: ratio = DbToRatio(db) * 32768, clamped to
// [0, 0x8000] -- unity at most.  Kept across movies, as mVolume is.
void FFmpegMovieImpl::SetVolume(float db) {
    float ratio = DbToRatio(db);
    mVolumeGain = ratio > 1.0f ? 1.0f : (ratio < 0.0f ? 0.0f : ratio);
    if (mAudioOut)
        mAudioOut->SetGain(mVolumeGain);
}

float FFmpegMovieImpl::MsPerFrame() const {
    if (mFrameRate > 0.0f)
        return 1000.0f / mFrameRate;
    return 33.33f; // default ~30fps
}

void FFmpegMovieImpl::Terminate() {
    Close();
}

void FFmpegMovieImpl::Close() {
    if (mAudioOut) {
        printf("FFmpegMovieImpl: %s sound closed: %lld frames played (%lld with sound, "
               "%lld silent), %lld dropped late, picture at frame %d\n",
               mFilename.c_str(), (long long)mAudioOut->PlayedFrames(),
               (long long)mAudioOut->AudioFramesRendered(),
               (long long)mAudioOut->SilenceFramesRendered(),
               (long long)mAudioOut->FramesDroppedLate(), mCurrentFrame);
        // RemoveSource takes the mixer lock, so once it returns the audio
        // thread is not inside RenderAudio.
        if (mAudioOnDevice)
            AudioDevice::GetInstance().RemoveSource(mAudioOut);
        delete mAudioOut;
        mAudioOut = nullptr;
    }
    mAudioOnDevice = false;
    mAudioClock = false;
    if (mSwr) swr_free(&mSwr);
    if (mAudioFrame) av_frame_free(&mAudioFrame);
    if (mAudioCtx) avcodec_free_context(&mAudioCtx);
    mAudioStreamIdx = -1;
    mAudioTrack = -1;
    ClearVideoQueue();
    if (mSwsCtx) { sws_freeContext(mSwsCtx); mSwsCtx = nullptr; }
    if (mAvFrame) { av_frame_free(&mAvFrame); }
    if (mPacket) { av_packet_free(&mPacket); }
    if (mVideoCtx) { avcodec_free_context(&mVideoCtx); }
    if (mFmtCtx) { avformat_close_input(&mFmtCtx); }
    // Custom IO outlives the format context (avformat_close_input leaves a
    // caller-supplied pb alone), and the File outlives the IO.
    if (mAvio) {
        av_freep(&mAvio->buffer);
        avio_context_free(&mAvio);
    }
    if (mFile) {
        delete mFile;
        mFile = nullptr;
    }
    mRGBABuffer.clear();
    mOpen = false;
    mReady = false;
    mFrameDecoded = false;
    mCurrentFrame = 0;
    mDemuxEOF = false;
}
