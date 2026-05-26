// DC3 Native Port - StreamReceiverNative implementation
// Bridges engine's StreamReceiver protocol to AudioDevice output.

#include "platform/StreamReceiver_Native.h"

#include <cstring>
#include <algorithm>
#include <cmath>
#include <cstdio>

// Define the static factory pointer (declared in StreamReceiver.h)
StreamReceiverFactoryFunc *StreamReceiver::sFactory = nullptr;

#ifdef HX_WEB
static int gNextWebReceiverId = 1;

static void LogWebReceiverEvent(
    const char *event,
    int id,
    const char *label,
    bool inMixer,
    bool playing,
    bool paused,
    int writeCursor,
    int playCursor
) {
    std::printf(
        "DC3 WEBAUDIO receiver[%d] %s label='%s' inMixer=%d playing=%d paused=%d write=%d play=%d\n",
        id,
        event,
        label ? label : "",
        inMixer,
        playing,
        paused,
        writeCursor,
        playCursor
    );
}
#endif

// StreamReceiver::New — dispatches to platform factory (declared in StreamReceiver.h)
StreamReceiver *StreamReceiver::New(int numBuffers, int sampleRate, bool slip, int channel) {
    MILO_ASSERT(sFactory, 0x20);
    return sFactory(numBuffers, sampleRate, slip, channel);
}

// StreamReceiver::GetBytesPlayed and ::Poll — now in StreamReceiver.cpp

StreamReceiverNative::StreamReceiverNative(int numBuffers, bool slip)
    : StreamReceiver(numBuffers, slip),
      mWriteCursor(0), mPlayCursor(0),
      mVolume(1.0f), mPan(0.0f), mSpeed(1.0f),
      mPlaying(false), mPaused(false), mSampleRate(44100),
      mTotalBytesPlayed(0)
#ifdef HX_WEB
      , mDebugId(gNextWebReceiverId++), mInMixer(false), mSilentRenderCount(0),
      mActiveRenderCount(0), mLastPeak(0.0f)
#endif
{
    memset(mPCMBuf, 0, sizeof(mPCMBuf));
    mState = kReady;
#ifdef HX_WEB
    mDebugLabel[0] = '\0';
#endif
}

StreamReceiverNative::~StreamReceiverNative() {
#ifdef HX_WEB
    LogWebReceiverEvent(
        "destroy",
        mDebugId,
        mDebugLabel,
        mInMixer,
        mPlaying,
        mPaused,
        mWriteCursor,
        mPlayCursor
    );
#endif
    if (mPlaying) {
        AudioDevice::GetInstance().RemoveSource(this);
#ifdef HX_WEB
        mInMixer = false;
#endif
    }
}

StreamReceiver *StreamReceiverNative::Create(int numBuffers, int sampleRate, bool slip, int /*channel*/) {
    StreamReceiverNative *rcvr = new StreamReceiverNative(numBuffers, slip);
    rcvr->mSampleRate = sampleRate;
    return rcvr;
}

void StreamReceiverNative::PlayImpl() {
    mPlaying = true;
    mPaused = false;
    AudioDevice::GetInstance().AddSource(this);
#ifdef HX_WEB
    mInMixer = true;
    LogWebReceiverEvent(
        "play",
        mDebugId,
        mDebugLabel,
        mInMixer,
        mPlaying,
        mPaused,
        mWriteCursor,
        mPlayCursor
    );
#endif
}

void StreamReceiverNative::PauseImpl(bool pause) {
    mPaused = pause;
#ifdef HX_WEB
    LogWebReceiverEvent(
        pause ? "pause" : "resume",
        mDebugId,
        mDebugLabel,
        mInMixer,
        mPlaying,
        mPaused,
        mWriteCursor,
        mPlayCursor
    );
#endif
}

void StreamReceiverNative::StartSendImpl(unsigned char *data, int size, int /*targetIdx*/) {
    int wc = mWriteCursor;
    int pc = mPlayCursor;
    int bufSamples = kPCMBufSize / 2;

    // Flow control: don't overflow the ring buffer
    int availBytes = kPCMBufSize - (wc - pc);
    if (size > availBytes) {
        size = availBytes;
        if (size <= 0) return;
    }

    int writePos = (wc / 2) % bufSamples;
    int samplesIn = size / 2;

    int16_t *src = (int16_t *)data;
    for (int i = 0; i < samplesIn; i++) {
        mPCMBuf[(writePos + i) % bufSamples] = src[i];
    }
    mWriteCursor = wc + size;
    mSending = true;
    mWantToSend = false;
}

bool StreamReceiverNative::SendDoneImpl() {
    return true;
}

int StreamReceiverNative::GetPlayCursor() {
    // Update base class mLastPlayCursor with total bytes played for GetBytesPlayed()
    mLastPlayCursor = mPlayCursor;
    return mPlayCursor % kStreamRcvrBufSize;
}

int StreamReceiverNative::RenderAudio(float *output, int frameCount) {
    if (!mPlaying || mPaused) {
        memset(output, 0, frameCount * 2 * sizeof(float));
#ifdef HX_WEB
        mSilentRenderCount++;
        mLastPeak = 0.0f;
#endif
        return frameCount;
    }

    int wc = mWriteCursor;
    int pc = mPlayCursor;
    int availBytes = wc - pc;
    int availSamples = availBytes / 2;
    int samplesToRender = std::min(frameCount, availSamples);

    if (samplesToRender <= 0) {
        memset(output, 0, frameCount * 2 * sizeof(float));
#ifdef HX_WEB
        mSilentRenderCount++;
        mLastPeak = 0.0f;
#endif
        // When source data is exhausted, keep advancing the play cursor
        // through silence so GetBytesPlayed() / GetRawTime() keeps moving.
        // Xbox hardware ring buffers do this automatically (zero-fill after
        // EndData). Without this, songMs freezes and end-of-song events
        // never fire, hanging gameplay.
        if (mEndData) {
            int silentBytes = frameCount * 2;
            mPlayCursor += silentBytes;
            mTotalBytesPlayed += silentBytes;
        }
        return frameCount;
    }

    int bufSamples = kPCMBufSize / 2;
    int readPos = (pc / 2) % bufSamples;

    float volL = mVolume * std::max(0.0f, 1.0f - mPan);
    float volR = mVolume * std::max(0.0f, 1.0f + mPan);
#ifdef HX_WEB
    mLastPeak = 0.0f;
#endif

    for (int i = 0; i < samplesToRender; i++) {
        float sample = mPCMBuf[(readPos + i) % bufSamples] / 32768.0f;
#ifdef HX_WEB
        float absSample = std::fabs(sample);
        if (absSample > mLastPeak) {
            mLastPeak = absSample;
        }
#endif
        output[i * 2 + 0] = sample * volL;
        output[i * 2 + 1] = sample * volR;
    }

    for (int i = samplesToRender; i < frameCount; i++) {
        output[i * 2 + 0] = 0.0f;
        output[i * 2 + 1] = 0.0f;
    }

    int bytesConsumed = samplesToRender * 2;
    mPlayCursor = pc + bytesConsumed;
    mTotalBytesPlayed += bytesConsumed;
#ifdef HX_WEB
    mActiveRenderCount++;
#endif

    return frameCount;
}

#ifdef HX_WEB
void StreamReceiverNative::DebugDescribe(char *buf, size_t bufSize) const {
    if (bufSize == 0) {
        return;
    }
    std::snprintf(
        buf,
        bufSize,
        "receiver[%d] label='%s' inMixer=%d playing=%d paused=%d write=%d play=%d totalBytes=%llu renders(active=%u silent=%u) peak=%.5f finished=%d",
        mDebugId,
        mDebugLabel,
        mInMixer,
        mPlaying,
        mPaused,
        mWriteCursor,
        mPlayCursor,
        (unsigned long long)mTotalBytesPlayed,
        mActiveRenderCount,
        mSilentRenderCount,
        mLastPeak,
        IsFinished()
    );
}

void StreamReceiverNative::SetDebugLabel(const char *label) {
    if (!label) {
        mDebugLabel[0] = '\0';
        return;
    }
    std::snprintf(mDebugLabel, sizeof(mDebugLabel), "%s", label);
}
#endif
