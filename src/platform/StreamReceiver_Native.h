// DC3 Native Port - StreamReceiverNative
// Concrete StreamReceiver that feeds decoded PCM to AudioDevice for playback.
// Each instance represents one audio channel of a StandardStream.

#pragma once

#include "synth/StreamReceiver.h"
#include "audio/AudioDevice.h"

class StreamReceiverNative : public StreamReceiver, public AudioSource {
public:
    StreamReceiverNative(int numBuffers, bool slip);
    virtual ~StreamReceiverNative();

    // StreamReceiver interface
    virtual bool IsOutputDrained() const override { return mPlayCursor >= mWriteCursor; }
    virtual void SetVolume(float vol) override { mVolume = vol; }
    virtual void SetPan(float pan) override { mPan = pan; }
    virtual void SetSpeed(float speed) override { mSpeed = speed; }
    virtual void SetSlipOffset(float) override {}
    virtual void SlipStop() override {}
    virtual void SetSlipSpeed(float) override {}
    virtual float GetSlipOffset() override { return 0.0f; }
    virtual int GetPlayCursor() override;
    virtual void PauseImpl(bool pause) override;
    virtual void PlayImpl() override;
    virtual void StartSendImpl(unsigned char *data, int size, int targetIdx) override;
    virtual bool SendDoneImpl() override;

    // AudioSource interface (called from audio thread)
    virtual int RenderAudio(float *output, int frameCount) override;
    // Delay reporting finished until mDoneBufferCounter exceeds the same
    // threshold StandardStream uses for kFinished. This keeps the source in
    // the mixer so RenderAudio() continues advancing mPlayCursor through
    // silence, which keeps GetRawTime()/songMs moving toward end-of-song.
    virtual bool IsFinished() const override {
        return mEndData && mPlayCursor >= mWriteCursor
            && mDoneBufferCounter > mNumBuffers + 2;
    }
#ifdef HX_WEB
    virtual void DebugDescribe(char *buf, size_t bufSize) const override;
    void SetDebugLabel(const char *label);
#endif

    // Factory function — register with StreamReceiver::sFactory
    static StreamReceiver *Create(int numBuffers, int sampleRate, bool slip, int channel);

    // Called by base StreamReceiver::GetBytesPlayed() (non-virtual, uses static_cast)
    u64 GetTotalBytesPlayed() const { return mTotalBytesPlayed; }

    // Available write space in the ring buffer (bytes)
    int AvailableWriteBytes() const {
        int avail = kPCMBufSize - (mWriteCursor - mPlayCursor);
        return (avail > 0) ? avail : 0;
    }

private:
    // PCM ring buffer (16-bit mono samples from the engine, converted to float on output)
    static const int kPCMBufSize = 0x10000; // 64KB
    int16_t mPCMBuf[kPCMBufSize / 2];

    volatile int mWriteCursor; // bytes written by engine
    volatile int mPlayCursor;  // bytes consumed by audio thread
    float mVolume;
    float mPan;    // -1 (left) to 1 (right)
    float mSpeed;
    bool mPlaying;
    bool mPaused;
    int mSampleRate;
    u64 mTotalBytesPlayed;
#ifdef HX_WEB
    int mDebugId;
    bool mInMixer;
    char mDebugLabel[96];
    unsigned int mSilentRenderCount;
    unsigned int mActiveRenderCount;
    float mLastPeak;
#endif
};
