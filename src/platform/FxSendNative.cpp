// DC3 Native Port - FxSendNative implementation
// Applies FxSend effect chains using portable DSP processors.

#include "platform/FxSendNative.h"

#include "synth/FxSendEQ.h"
#include "synth/FxSendCompress.h"
#include "synth/FxSendDelay.h"
#include "synth/FxSendDistortion.h"
#include "synth/FxSendChorus.h"
#include "synth/FxSendFlanger.h"
#include "synth/FxSendBitCrush.h"
#include "synth/FxSendReverb.h"
#include "synth/FxSendWah.h"

#include "synth/EQEffect.h"
#include "synth/CompressionEffect.h"
#include "synth/DelayEffect.h"
#include "synth/DistortionEffect.h"
#include "synth/FlangerEffect.h"
#include "synth/BitCrushEffect.h"
#include "synth/WahEffect.h"

#include "math/Decibels.h"

#include <cstring>
#include <cmath>
#include <algorithm>

// ============================================================================
// NativeEffectSlot
// ============================================================================

NativeEffectSlot::~NativeEffectSlot() {
    Destroy();
}

void NativeEffectSlot::Destroy() {
    if (!processor) return;
    switch (type) {
        case kEQ:         delete (EQEffect *)processor; break;
        case kCompressor: delete (CompressionEffect *)processor; break;
        case kDelay:      delete (DelayEffect *)processor; break;
        case kDistortion: delete (DistortionEffect *)processor; break;
        case kFlanger:    delete (FlangerEffect *)processor; break;
        case kBitCrush:   delete (BitCrushEffect *)processor; break;
        case kWah:        delete (WahEffect *)processor; break;
        default: break;
    }
    processor = nullptr;
    type = kNone;
    send = nullptr;
}

void NativeEffectSlot::Init(FxSend *fxSend) {
    Destroy();
    send = fxSend;

    // Determine effect type from class name
    const char *className = fxSend->ClassName().Str();

    if (strcmp(className, "FxSendEQ") == 0) {
        type = kEQ;
        processor = new EQEffect(nullptr);
    } else if (strcmp(className, "FxSendCompress") == 0) {
        type = kCompressor;
        processor = new CompressionEffect(nullptr);
    } else if (strcmp(className, "FxSendDelay") == 0) {
        type = kDelay;
        processor = new DelayEffect(nullptr);
    } else if (strcmp(className, "FxSendDistortion") == 0) {
        type = kDistortion;
        processor = new DistortionEffect(nullptr);
    } else if (strcmp(className, "FxSendFlanger") == 0) {
        type = kFlanger;
        processor = new FlangerEffect(nullptr);
    } else if (strcmp(className, "FxSendBitCrush") == 0) {
        type = kBitCrush;
        processor = new BitCrushEffect(nullptr);
    } else if (strcmp(className, "FxSendWah") == 0) {
        type = kWah;
        processor = new WahEffect(nullptr);
    } else if (strcmp(className, "FxSendReverb") == 0) {
        // Approximate reverb with a delay-based reverb
        type = kReverb;
        processor = new DelayEffect(nullptr);
    } else if (strcmp(className, "FxSendChorus") == 0) {
        // Chorus uses same DSP core as flanger with different params
        type = kChorus;
        processor = new FlangerEffect(nullptr);
    } else {
        // Unknown effect type — skip (meter, pitch shift, synapse)
        type = kNone;
    }

    if (processor) {
        SyncParams();
    }
}

void NativeEffectSlot::SyncParams() {
    if (!processor || !send) return;

    switch (type) {
        case kEQ: {
            FxSendEQ *eq = (FxSendEQ *)send;
            EQEffect *fx = (EQEffect *)processor;
            EQEffect::Params p;
            p.mActiveBands = 0x1F; // all 5 bands
            p.mBand1Freq = eq->mHighFreqCutoff;
            p.mBand1Gain = eq->mHighFreqGain;
            p.mBand1Q = 1.0f;
            p.mBand2Freq = eq->mMidFreqCutoff;
            p.mBand2Gain = eq->mMidFreqGain;
            p.mBand2Q = eq->mMidFreqBandwidth;
            p.mBand3Freq = eq->mLowFreqCutoff;
            p.mBand3Gain = eq->mLowFreqGain;
            p.mBand3Q = 1.0f;
            p.mBand4Freq = eq->mLowPassCutoff;
            p.mBand4Gain = 0.0f;
            p.mBand4Q = eq->mLowPassReso;
            p.mBand5Freq = eq->mHighPassCutoff;
            p.mBand5Q = eq->mHighPassReso;
            fx->SetParameters(p);
            break;
        }
        case kCompressor: {
            FxSendCompress *comp = (FxSendCompress *)send;
            CompressionEffect *fx = (CompressionEffect *)processor;
            CompressionEffect::Params p;
            p.unk0 = true;
            p.mThresholdDb = comp->mThresholdDB;
            p.mRatio = comp->mRatio;
            p.mOutputGainDb = comp->mOutputLevel;
            p.mAttackTime = comp->mAttack;
            p.mReleaseTime = comp->mRelease;
            p.mPostGain = 0.0f;
            p.mPeakAttackTime = comp->mExpAttack;
            p.mPeakReleaseTime = comp->mExpRelease;
            p.mGateThreshDb = comp->mGateThresholdDB;
            fx->SetParameters(p);
            break;
        }
        case kDelay: {
            FxSendDelay *dly = (FxSendDelay *)send;
            DelayEffect *fx = (DelayEffect *)processor;
            DelayEffect::Params p;
            p.unk0 = 0;
            p.mDelaySamples = dly->mDelayTime * 48.0f; // ms to samples at 48kHz
            p.mDecayDb = dly->mGain;
            p.mWetPercent = 50.0f;
            fx->SetParameters(p);
            break;
        }
        case kDistortion: {
            FxSendDistortion *dist = (FxSendDistortion *)send;
            DistortionEffect *fx = (DistortionEffect *)processor;
            DistortionEffect::Params p;
            p.unk0 = 0;
            p.unk4 = dist->mDrive;
            fx->SetParameters(p);
            break;
        }
        case kFlanger: {
            FxSendFlanger *fl = (FxSendFlanger *)send;
            FlangerEffect *fx = (FlangerEffect *)processor;
            FlangerEffect::Params p;
            p.unk0 = 0;
            p.mDelayMs = fl->mDelayMs;
            p.mRate = fl->mRate;
            p.mDepth = fl->mDepthPct;
            p.mFeedback = fl->mFeedbackPct;
            p.mWet = 50.0f;
            fx->SetParameters(p);
            break;
        }
        case kChorus: {
            FxSendChorus *ch = (FxSendChorus *)send;
            FlangerEffect *fx = (FlangerEffect *)processor;
            FlangerEffect::Params p;
            p.unk0 = 0;
            p.mDelayMs = ch->mDelayMs;
            p.mRate = ch->mRate;
            p.mDepth = ch->mDepth;
            p.mFeedback = ch->mFeedbackPct;
            p.mWet = 50.0f;
            fx->SetParameters(p);
            break;
        }
        case kBitCrush: {
            FxSendBitCrush *bc = (FxSendBitCrush *)send;
            BitCrushEffect *fx = (BitCrushEffect *)processor;
            BitCrushEffect::Params p;
            p.unk0 = 0;
            p.unk4 = bc->mAmount;
            fx->SetParameters(p);
            break;
        }
        case kWah: {
            FxSendWah *wah = (FxSendWah *)send;
            WahEffect *fx = (WahEffect *)processor;
            WahEffect::Params p;
            p.unk0 = 0;
            p.mGain = 1.0f;
            p.mFreqHi = wah->mUpperFreq;
            p.mFreqLo = wah->mLowerFreq;
            p.mResonance = wah->mResonance;
            p.mBandwidth = 1.0f;
            p.mSweepRate = wah->mLfoFreq;
            p.mSweepRange = 1.0f;
            p.mEnvAmount = false;
            p.mStaticSweep = 0.5f;
            fx->SetParameters(p);
            break;
        }
        case kReverb: {
            // Approximate reverb with a long delay + high feedback
            FxSendReverb *rev = (FxSendReverb *)send;
            DelayEffect *fx = (DelayEffect *)processor;
            DelayEffect::Params p;
            p.unk0 = 0;
            p.mDelaySamples = rev->mRoomSize * 48000.0f * 0.1f;
            p.mDecayDb = -6.0f * (1.0f - rev->mDamping);
            p.mWetPercent = 40.0f;
            fx->SetParameters(p);
            break;
        }
        default:
            break;
    }
}

void NativeEffectSlot::Process(float *buffer, int numSamples, int numChannels) {
    if (!processor || type == kNone) return;
    if (send->mBypass) return;

    float wetGain = DbToRatio(send->mWetGain);
    float dryGain = DbToRatio(send->mDryGain);
    float inputGain = DbToRatio(send->mInputGain);

    if (wetGain < 0.001f) return;

    int totalSamples = numSamples * numChannels;

    // Apply input gain
    if (fabsf(inputGain - 1.0f) > 0.001f) {
        for (int i = 0; i < totalSamples; i++) {
            buffer[i] *= inputGain;
        }
    }

    // Save dry copy for mixing
    bool needMix = (fabsf(dryGain - 1.0f) > 0.001f) || (fabsf(wetGain - 1.0f) > 0.001f);
    float *dry = nullptr;
    if (needMix) {
        dry = new float[totalSamples];
        memcpy(dry, buffer, totalSamples * sizeof(float));
    }

    // Process through the DSP effect
    switch (type) {
        case kEQ:
            ((EQEffect *)processor)->Process(buffer, numSamples, numChannels);
            break;
        case kCompressor:
            ((CompressionEffect *)processor)->Process(buffer, numSamples, numChannels);
            break;
        case kDelay:
        case kReverb:
            ((DelayEffect *)processor)->Process(buffer, numSamples, numChannels);
            break;
        case kDistortion:
            ((DistortionEffect *)processor)->Process(buffer, numSamples, numChannels);
            break;
        case kFlanger:
        case kChorus:
            ((FlangerEffect *)processor)->Process(buffer, numSamples, numChannels);
            break;
        case kBitCrush:
            ((BitCrushEffect *)processor)->Process(buffer, numSamples, numChannels);
            break;
        case kWah:
            ((WahEffect *)processor)->Process(buffer, numSamples, numChannels);
            break;
        default:
            break;
    }

    // Mix dry/wet
    if (needMix) {
        for (int i = 0; i < totalSamples; i++) {
            buffer[i] = dry[i] * dryGain + buffer[i] * wetGain;
        }
        delete[] dry;
    }
}

// ============================================================================
// FxSendNative_ProcessChain
// ============================================================================

void FxSendNative_ProcessChain(FxSend *head, float *buffer, int numSamples, int numChannels) {
    if (!head || !buffer || numSamples <= 0) return;

    FxSend *current = head;
    int count = 0;

    while (current && count < kMaxChainLength) {
        if (!current->mBypass) {
            NativeEffectSlot slot;
            slot.Init(current);
            slot.Process(buffer, numSamples, numChannels);
        }
        current = current->NextSend();
        count++;
    }
}
