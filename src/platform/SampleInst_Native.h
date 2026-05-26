// DC3 Native Port - SampleInstNative
// Concrete SampleInst for one-shot sound effect playback via AudioDevice.

#pragma once

#include "synth/SampleInst.h"
#include "audio/AudioDevice.h"

class SampleInstNative : public SampleInst, public AudioSource {
public:
    SampleInstNative(SynthSample *sample, bool loop, int startSample, int endSample);
    virtual ~SampleInstNative();

    // SampleInst pure virtuals
    virtual bool IsPlaying() const override { return mPlaying; }
    virtual void SetFXCore(FXCore) override {}
    virtual void StartImpl() override;
    virtual void StopImpl(bool) override;
    virtual void SetVolumeImpl(float vol) override { mInstVolume = vol; }
    virtual void SetPanImpl(float pan) override { mInstPan = pan; }
    virtual void SetSpeedImpl(float speed) override { mInstSpeed = speed; }
    virtual void Pause(bool pause) override { mPaused = pause; }
    virtual void SetADSR(const ADSRImpl &) override {}

    // AudioSource interface (called from audio thread)
    virtual int RenderAudio(float *output, int frameCount) override;
    virtual bool IsFinished() const override { return !mPlaying; }

protected:
    virtual void SetSendImpl(FxSend *send) override { mFxSend = send; }

private:
    FxSend *mFxSend = nullptr;
    const int16_t *mPCMData;    // pointer into SampleData's buffer
    int mPCMSamples;            // total samples available
    double mPlayPos;            // fractional sample position for resampling
    int mEndSample;             // where to stop (-1 = end of data)
    bool mLoop;
    volatile bool mPlaying;
    bool mPaused;
    float mInstVolume;
    float mInstPan;
    float mInstSpeed;
    int mSampleRate;
    int mNumChannels;
};
