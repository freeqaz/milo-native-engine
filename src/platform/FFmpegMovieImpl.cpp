// DC3 Native Port - FFmpegMovieImpl
// Replaces BinkMovieImpl for FMV/cutscene video playback using FFmpeg.
//
// Flow: BeginFromFile → OpenVideo → Poll (decode frames on timer) → Draw (upload RGBA)
// The original BinkMovieImpl decodes one frame per Poll cycle, synced to BinkTimer.
// We replicate this with a simple frame-rate timer.

#include "platform/FFmpegMovieImpl.h"
#include "os/Debug.h"

extern "C" {
#include <libavutil/imgutils.h>
}

#include <cstring>

FFmpegMovieImpl::FFmpegMovieImpl()
    : mFmtCtx(nullptr), mVideoCtx(nullptr), mVideoStreamIdx(-1),
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

bool FFmpegMovieImpl::Ready() const {
    return mReady;
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

    mReady = true;
    mOpen = true;
    mCurrentFrame = 0;
    mPaused = false;
    mPlayTimer.Restart();

    return true;
}

bool FFmpegMovieImpl::OpenVideo(const char *path) {
    int ret = avformat_open_input(&mFmtCtx, path, nullptr, nullptr);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        MILO_WARN("FFmpegMovieImpl: avformat_open_input: %s", errbuf);
        return false;
    }

    ret = avformat_find_stream_info(mFmtCtx, nullptr);
    if (ret < 0) {
        avformat_close_input(&mFmtCtx);
        return false;
    }

    // Find video stream
    mVideoStreamIdx = av_find_best_stream(
        mFmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0
    );
    if (mVideoStreamIdx < 0) {
        MILO_WARN("FFmpegMovieImpl: no video stream found");
        avformat_close_input(&mFmtCtx);
        return false;
    }

    AVStream *vs = mFmtCtx->streams[mVideoStreamIdx];
    const AVCodec *codec = avcodec_find_decoder(vs->codecpar->codec_id);
    if (!codec) {
        MILO_WARN("FFmpegMovieImpl: no decoder for codec %d", vs->codecpar->codec_id);
        avformat_close_input(&mFmtCtx);
        return false;
    }

    mVideoCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(mVideoCtx, vs->codecpar);
    ret = avcodec_open2(mVideoCtx, codec, nullptr);
    if (ret < 0) {
        avcodec_free_context(&mVideoCtx);
        avformat_close_input(&mFmtCtx);
        return false;
    }

    mVideoWidth = mVideoCtx->width;
    mVideoHeight = mVideoCtx->height;

    // Frame count and rate
    mNumFrames = (int)vs->nb_frames;
    if (vs->avg_frame_rate.den > 0) {
        mFrameRate = (float)vs->avg_frame_rate.num / (float)vs->avg_frame_rate.den;
    }

    // Create scaler for YUV→RGBA
    mSwsCtx = sws_getContext(
        mVideoWidth, mVideoHeight, mVideoCtx->pix_fmt,
        mVideoWidth, mVideoHeight, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    if (!mSwsCtx) {
        MILO_WARN("FFmpegMovieImpl: sws_getContext failed");
        avcodec_free_context(&mVideoCtx);
        avformat_close_input(&mFmtCtx);
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
    if (!mOpen || mPaused) return true;

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
    mRGBABuffer.clear();
    mOpen = false;
    mReady = false;
    mFrameDecoded = false;
    mCurrentFrame = 0;
}
