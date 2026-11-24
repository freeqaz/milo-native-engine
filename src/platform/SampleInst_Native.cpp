// DC3 Native Port - SampleInstNative implementation
// One-shot and looping sound effects via AudioDevice.
// Supports sample rate conversion (bank samples are 32kHz, output is 44.1kHz).

#include "platform/SampleInst_Native.h"
#include "platform/FxSendNative.h"
#include "platform/ImagePan_Native.h"
#include "synth/SampleData.h"
#include "synth/SynthSample.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const int kOutputSampleRate = 44100;

// SynthSample::NewInst — native implementation
SampleInst *SynthSample::NewInst(bool loop, int startSample, int endSample) {
    return new SampleInstNative(this, loop, startSample, endSample);
}

SampleInstNative::SampleInstNative(SynthSample *sample, bool loop, int startSample, int endSample)
    : SampleInst(sample),
      mPCMData(nullptr), mPCMSamples(0), mPlayPos(startSample > 0 ? (double)startSample : 0.0),
      mEndSample(endSample), mLoop(loop),
      mPlaying(false), mPaused(false),
      mInstVolume(1.0f), mInstPan(0.0f), mInstSpeed(1.0f),
      mSampleRate(44100), mNumChannels(1) {
}

SampleInstNative::~SampleInstNative() {
    // Always remove from audio device — even if mPlaying is false.
    // The audio thread may have set mPlaying=false in RenderAudio but
    // not yet erased us from the source list (happens after RenderAudio
    // returns). If we're deleted before the next audio callback, the
    // callback would access freed memory.
    AudioDevice::GetInstance().RemoveSource(this);
    mPlaying = false;
}

void SampleInstNative::StartImpl() {
    if (!mSample)
        return;

    const SampleData &data = mSample->GetSampleData();
    if (!data.HasData())
        return;

    // Compressed samples need to be decoded by the bank loader first.
    if (data.GetFormat() != SampleData::kPCM && data.GetFormat() != SampleData::kBigEndPCM)
        return;

    mSampleRate = data.GetSampleRate();
    mNumChannels = data.NumChannels();
    mPCMData = (const int16_t *)data.DataPtr();
    mBigEndian = data.GetFormat() == SampleData::kBigEndPCM;
    if (mSampleRate <= 0 || (mNumChannels != 1 && mNumChannels != 2)) return;
    mPCMSamples = std::min(data.GetNumSamples(), data.GetSizeBytes() / (2 * mNumChannels));
    if (mPCMSamples <= 0) return;

    if (mEndSample <= 0 || mEndSample > mPCMSamples)
        mEndSample = mPCMSamples;

    mPlaying = true;
    AudioDevice::GetInstance().AddSource(this);
    if (std::getenv("MILO_AUDIO_TRACE")) {
        std::printf("AudioDevice: sample '%s' start frames=%d rate=%d channels=%d format=%d\n",
                    mSample->Name(), mPCMSamples, mSampleRate, mNumChannels, data.GetFormat());
    }
}

void SampleInstNative::StopImpl(bool) {
    if (mPlaying) {
        AudioDevice::GetInstance().RemoveSource(this);
        mPlaying = false;
    }
}

// Linear interpolation between two samples
static inline float LerpSample(const int16_t *data, int totalSamples, double pos, int channel,
                               int numChannels, bool bigEndian) {
    int idx0 = (int)pos;
    int idx1 = idx0 + 1;
    float frac = (float)(pos - idx0);

    auto fetch = [&](int frame) {
        if (frame < 0 || frame >= totalSamples) return 0.0f;
        const int16_t *sample = data + frame * numChannels + channel;
        if (!bigEndian) return *sample / 32768.0f;
        const auto *bytes = reinterpret_cast<const uint8_t *>(sample);
        return static_cast<int16_t>((bytes[0] << 8) | bytes[1]) / 32768.0f;
    };
    const float s0 = fetch(idx0);
    const float s1 = fetch(idx1);
    return s0 + frac * (s1 - s0);
}

int SampleInstNative::RenderAudio(float *output, int frameCount) {
    if (!mPlaying || mPaused || !mPCMData) {
        memset(output, 0, frameCount * 2 * sizeof(float));
        return frameCount;
    }

    int endPos = (mEndSample > 0) ? mEndSample : mPCMSamples;
    // The image's pan law, folded to stereo (ImagePan_Native.h): a mono
    // sample is ring-panned, a stereo one scaled by the cos/sin pair.
    float volL, volR;
    if (mNumChannels == 2) ImagePan::Stereo(mInstVolume, mInstPan, volL, volR);
    else ImagePan::Mono(mInstVolume, mInstPan, volL, volR);

    // Rate ratio: how many source samples per output sample
    double rateRatio = (double)mSampleRate / (double)kOutputSampleRate * (double)mInstSpeed;

    for (int i = 0; i < frameCount; i++) {
        if (mPlayPos >= (double)endPos) {
            if (mLoop) {
                mPlayPos = 0.0;
            } else {
                for (int j = i; j < frameCount; j++) {
                    output[j * 2 + 0] = 0.0f;
                    output[j * 2 + 1] = 0.0f;
                }
                mPlaying = false;
                return frameCount;
            }
        }

        if (mNumChannels == 2) {
            float left = LerpSample(mPCMData, mPCMSamples, mPlayPos, 0, 2, mBigEndian);
            float right = LerpSample(mPCMData, mPCMSamples, mPlayPos, 1, 2, mBigEndian);
            output[i * 2 + 0] = left * volL;
            output[i * 2 + 1] = right * volR;
        } else {
            float sample = LerpSample(mPCMData, mPCMSamples, mPlayPos, 0, 1, mBigEndian);
            output[i * 2 + 0] = sample * volL;
            output[i * 2 + 1] = sample * volR;
        }

        mPlayPos += rateRatio;
    }

    return frameCount;
}
