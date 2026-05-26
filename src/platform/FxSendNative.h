// DC3 Native Port - FxSendNative
// Applies FxSend effect chains to audio buffers using the portable DSP classes
// (EQEffect, CompressionEffect, DelayEffect, etc.)

#pragma once

#include "synth/FxSend.h"

// Forward declarations for effect types
class FxSendEQ;
class FxSendCompress;
class FxSendDelay;
class FxSendDistortion;
class FxSendChorus;
class FxSendFlanger;
class FxSendBitCrush;
class FxSendReverb;
class FxSendWah;

class EQEffect;
class CompressionEffect;
class DelayEffect;
class DistortionEffect;
class FlangerEffect;
class BitCrushEffect;
class WahEffect;

// Wraps a single FxSend and its associated DSP processor
struct NativeEffectSlot {
    enum EffectType {
        kNone = 0,
        kEQ,
        kCompressor,
        kDelay,
        kDistortion,
        kFlanger,
        kBitCrush,
        kWah,
        kReverb,    // simple reverb via delay network
        kChorus,    // chorus via modulated delay
    };

    EffectType type;
    FxSend *send;     // the engine FxSend object (for reading parameters)
    void *processor;  // the DSP processor instance (EQEffect*, etc.)

    NativeEffectSlot() : type(kNone), send(nullptr), processor(nullptr) {}
    ~NativeEffectSlot();

    void Init(FxSend *fxSend);
    void SyncParams();
    void Process(float *buffer, int numSamples, int numChannels);
    void Destroy();
};

// Process an entire FxSend chain on an audio buffer
// Walks the chain via NextSend(), applies each non-bypassed effect
void FxSendNative_ProcessChain(FxSend *head, float *buffer, int numSamples, int numChannels);

// Maximum effects in a chain (prevents runaway)
static const int kMaxChainLength = 16;
