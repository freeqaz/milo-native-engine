// Master-bus level meter tap (header-only).
//
// What it is for: DC3's Synth360 hangs a MeterEffect XAPO on the XAudio2
// mastering voice, behind the limiter, and every frame HamMaster::CheckLevels
// turns the resulting bus levels into the `audio_channels` channel0..7
// properties. The boombox's EQ bars (multiplier_meter's perform_*.anim), the
// shell background's eq4/pulse anims and every other copy of multiplier_meter
// are keyed off those properties. Native had no meter, so Synth::mLevelData was
// empty, CheckLevels returned at its first test, and every one of them sat at
// frame 0.
//
// The tap has two halves so the meter's state machine runs on ONE thread:
//
//  * MasterMeterTap::Process (audio thread, or PumpAudio on the web) cuts the
//    finished stereo mix into XAudio2-sized quanta and, per quantum, records
//    what one MeterEffect::DoProcess pass reads: a sum of squares and a peak
//    per channel. It lays the stereo mix out as the 5.1 mastering voice the
//    image meters ([L, R, 0, 0, 0, 0] per frame) and reads it the way the
//    image's DoProcess does: channel c takes the c-th block of frameCount
//    floats of the interleaved buffer (buffer[c * frameCount + i]), not the
//    c-th channel. See src/system/synth_xbox/MeterEffect.cpp in dc3-decomp,
//    "reproduced, not corrected". Quanta go into a single-producer /
//    single-consumer ring stamped with the mix frame they end on.
//
//  * MeterEffectState::Apply (main thread) replays those quanta through the
//    image's bookkeeping: a running sum of squares that only a parameter set
//    clears, a per-quantum peak, and an RMS published as sqrt(sum / frames
//    counted BEFORE this quantum), skipped while that count is 0. Synth360::Poll
//    sets the effect's parameters every frame, which is the reset.
//
// The consumer passes the frame the listener has reached. On desktop that is
// everything rendered (the device callback runs about one period ahead); on the
// web the mix is pulled a queue depth (140-500 ms) ahead of the worklet, so the
// web reader passes MixFramesConsumed() and the bars move with what is heard
// rather than with what was mixed.
//
// Off unless a consumer enables it (DC3's NativeSynth does); RB3 and RB3-xenon
// never call it, so their mix path does no extra work.

#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

struct MeterQuantum {
    long long endFrame;   // mix frame (exclusive) this quantum ends on
    int frames;           // frames in the quantum (kQuantumFrames)
    float sumSquares[6];  // per meter channel, as DoProcess accumulates it
    float peak[6];        // per meter channel, max |x| over the quantum
};

class MasterMeterTap {
public:
    // XAudio2 on the 360 processes 256-frame quanta.
    static const int kQuantumFrames = 256;
    static const int kChannels = 6;
    // ~1.5 s of quanta at 44.1 kHz: more than the web queue depth, so a reader
    // one queue behind never finds its quanta overwritten.
    static const int kRingSize = 256;

    void SetEnabled(bool on) { mEnabled.store(on, std::memory_order_release); }
    bool Enabled() const { return mEnabled.load(std::memory_order_acquire); }

    // Producer. `stereo` is interleaved L/R, `frames` long.
    void Process(const float *stereo, int frames) {
        if (!Enabled()) return;
        while (frames > 0) {
            int take = kQuantumFrames - mPending;
            if (take > frames) take = frames;
            for (int i = 0; i < take * 2; i++) mStash[mPending * 2 + i] = stereo[i];
            mPending += take;
            stereo += take * 2;
            frames -= take;
            if (mPending == kQuantumFrames) {
                EmitQuantum();
                mPending = 0;
            }
        }
    }

    // Consumer. Hands every quantum ending at or before `heardFrame` to `fn`,
    // oldest first; returns how many. Quanta the producer has already lapped
    // are skipped: a reader stalled for more than the ring (~1.5 s) loses the
    // oldest levels instead of replaying them.
    template <class Fn>
    int Drain(long long heardFrame, Fn fn) {
        uint32_t w = mWrite.load(std::memory_order_acquire);
        if (w - mRead > (uint32_t)kRingSize) mRead = w - kRingSize;
        int n = 0;
        while (mRead != w) {
            const MeterQuantum &q = mRing[mRead % kRingSize];
            if (q.endFrame > heardFrame) break;
            fn(q);
            mRead++;
            n++;
        }
        return n;
    }

    long long FramesMetered() const { return mFrameCursor; }

private:
    void EmitQuantum() {
        uint32_t w = mWrite.load(std::memory_order_relaxed);
        MeterQuantum &q = mRing[w % kRingSize];
        const int n = kQuantumFrames;
        // The 5.1 buffer is n frames x 6 channels = 6n floats; float k is frame
        // k / 6, channel k % 6, and only channels 0 (L) and 1 (R) carry signal.
        // Meter channel c reads floats [c*n, (c+1)*n).
        for (int c = 0; c < kChannels; c++) {
            float sum = 0.0f, peak = 0.0f;
            const int begin = c * n, end = begin + n;
            for (int k = begin; k < end; k++) {
                const int ch = k % 6;
                if (ch > 1) continue;
                const float s = mStash[(k / 6) * 2 + ch];
                sum += s * s;
                const float a = std::fabs(s);
                if (a > peak) peak = a;
            }
            q.sumSquares[c] = sum;
            q.peak[c] = peak;
        }
        mFrameCursor += n;
        q.endFrame = mFrameCursor;
        q.frames = n;
        mWrite.store(w + 1, std::memory_order_release);
    }

    std::atomic<bool> mEnabled{false};
    float mStash[kQuantumFrames * 2] = {0};
    int mPending = 0;              // producer only
    long long mFrameCursor = 0;    // producer only
    MeterQuantum mRing[kRingSize];
    std::atomic<uint32_t> mWrite{0};
    uint32_t mRead = 0;            // consumer only
};

// The image's MeterEffect bookkeeping, fed quanta on the consumer's thread.
class MeterEffectState {
public:
    // Synth360::Poll's SetEffectParameters -> MeterEffect::OnSetParameters.
    void Reset() {
        mFrameCount = 0;
        for (int c = 0; c < MasterMeterTap::kChannels; c++) mSumSquares[c] = 0.0f;
    }

    // One DoProcess pass with a LevelData array present: peak always written,
    // RMS only once a previous quantum has been counted. `rms`/`peak` are the
    // six LevelData slots the effect writes; untouched slots keep their value.
    void Apply(const MeterQuantum &q, float *rms, float *peak) {
        for (int c = 0; c < MasterMeterTap::kChannels; c++) {
            mSumSquares[c] += q.sumSquares[c];
            peak[c] = q.peak[c];
            if (mFrameCount) rms[c] = std::sqrt(mSumSquares[c] / (float)mFrameCount);
        }
        mFrameCount += (unsigned int)q.frames;
    }

private:
    float mSumSquares[MasterMeterTap::kChannels] = {0};
    unsigned int mFrameCount = 0;
};
