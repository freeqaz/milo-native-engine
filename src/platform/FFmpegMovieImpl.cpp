// DC3 Native Port - FFmpegMovieImpl
// Replaces BinkMovieImpl for FMV/cutscene video playback using FFmpeg.
//
// Flow: BeginFromFile → OpenVideo → Poll (decode frames on timer) → Draw (upload RGBA)
// The original BinkMovieImpl decodes one frame per Poll cycle, synced to BinkTimer.
// We replicate this with a simple frame-rate timer.

#include "platform/FFmpegMovieImpl.h"
#include "os/Debug.h"
#include "os/File.h"

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
}

#include <cstdio>
#include <cstring>

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

FFmpegMovieImpl::FFmpegMovieImpl()
    : mFile(nullptr), mAvio(nullptr), mFmtCtx(nullptr), mVideoCtx(nullptr), mVideoStreamIdx(-1),
      mSwsCtx(nullptr), mPacket(nullptr), mAvFrame(nullptr),
      mVideoWidth(0), mVideoHeight(0),
      mOpen(false), mLoop(false), mPaused(false), mReady(false),
      mCurrentFrame(0), mNumFrames(0), mFrameRate(30.0f),
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

bool FFmpegMovieImpl::BeginFromFile(
    char const *path, float volume, bool loop, bool /*unk1*/,
    bool /*unk2*/, bool /*unk3*/, int /*unk4*/, BinStream * /*bs*/, LoaderPos /*pos*/
) {
    Close();

    mFilename = path;
    mLoop = loop;

    if (!OpenVideo(path)) {
        MILO_WARN("FFmpegMovieImpl: failed to open %s", path);
        return false;
    }

    printf("FFmpegMovieImpl: opened %s (%dx%d, %d frames at %.2f fps)\n", path,
           mVideoWidth, mVideoHeight, mNumFrames, mFrameRate);

    mReady = true;
    mOpen = true;
    mCurrentFrame = 0;
    mPaused = false;
    mPlayTimer.Restart();

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

    return true;
}

bool FFmpegMovieImpl::DecodeNextVideoFrame() {
    while (true) {
        int ret = av_read_frame(mFmtCtx, mPacket);
        if (ret < 0) {
            // EOF
            return false;
        }

        if (mPacket->stream_index != mVideoStreamIdx) {
            av_packet_unref(mPacket);
            continue;
        }

        ret = avcodec_send_packet(mVideoCtx, mPacket);
        av_packet_unref(mPacket);
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
    float elapsed = mUseVirtualTime ? mVirtualTimeMs : mPlayTimer.SplitMs();
    float msPerFrame = MsPerFrame();
    int targetFrame = (int)(elapsed / msPerFrame);

    while (mCurrentFrame <= targetFrame) {
        if (!DecodeNextVideoFrame()) {
            if (mLoop) {
                // Seek back to start
                av_seek_frame(mFmtCtx, mVideoStreamIdx, 0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(mVideoCtx);
                mCurrentFrame = 0;
                mPlayTimer.Restart();
                return true;
            }
            return false; // Video ended
        }
    }

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

void FFmpegMovieImpl::SetPaused(bool paused) {
    mPaused = paused;
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
}
