// DC3 Native Port - Audio Device
// Wraps miniaudio's ma_device for PCM output.
// All active audio sources register with the global mixer and
// get summed in the audio callback thread.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// Forward declare — we don't expose miniaudio types in the header
struct ma_device;
struct ma_context;

// Interface for anything that produces audio samples
class AudioSource {
public:
    virtual ~AudioSource() {}
    // Called from audio thread. Write `frameCount` interleaved stereo float samples
    // into `output`. Return the number of frames actually written.
    // If fewer than frameCount, the source is considered finished.
    virtual int RenderAudio(float *output, int frameCount) = 0;
    virtual bool IsFinished() const = 0;
#ifdef HX_WEB
    virtual void DebugDescribe(char *buf, size_t bufSize) const {
        if (bufSize > 0) {
            buf[0] = '\0';
        }
    }
#endif
};

#ifdef __EMSCRIPTEN__
// ---- Off-main web-audio (RB3_WEB_OFFMAIN_MIX, MVP-1) ----------------------
// A music stem (RB3StreamReceiverNative) that publishes its decoded int16-mono
// ring into a per-stem SharedArrayBuffer so the AudioWorklet mixes it on the
// audio thread (instead of the main thread draining a pre-mixed ring). The
// bridge (rb3_stream_receiver_native.cpp) implements this; AudioDevice owns the
// SAB pool + the per-tick publish loop. See
// docs/native/audio-thread-2026-06-20/05-BUILD-SPEC-offmain-mvp1.md.
struct OffMainStemState {
    const int16_t *ringPcm; // base of the producer int16-mono ring (WASM heap)
    int ringFrames;         // ring length in frames (mRingSize/2)
    // The producer DATA-VALIDITY frontier in frames (mRingWritePos/2): the SAB
    // PCM delta is copied up to here. NOT the availability frontier.
    int writeFrame;
    // The consumer play cursor in frames (mAudioReadPos/2) — the SAB readPos.
    int readFrame;
    // Frames available to play from readFrame, computed EXACTLY like
    // RenderAudio (mRingWrittenSpace - consumed). The SAB availability writePos
    // is published as (readFrame + availFrames) mod ringFrames so the worklet's
    // (writePos-readPos) gives the right depth even at the full-ring handoff.
    int availFrames;
    int startFrame;         // play-cursor start frame (first byte not yet handed off)
    float gain;             // per-stem volume (linear)
    float pan;              // per-stem pan (-1..+1)
    bool paused;
    bool finished;
};

class WebMusicStem {
public:
    virtual ~WebMusicStem() {}
    // True while this stem is actively playing (armed). A false here frees its slot.
    virtual bool OffMainActive() const = 0;
    // True once the first Play() has armed the play cursor (so the pump can seed
    // the worklet readPos at song start). Before this, the pump skips publishing.
    virtual bool OffMainArmed() const = 0;
    // Snapshot the current producer ring state + params into `out`. Pure read;
    // the pump copies the ring into the SAB and writes the cursors.
    virtual void OffMainSnapshot(OffMainStemState *out) const = 0;
    // Advance the producer back-pressure by `frameDelta` frames the worklet has
    // consumed since the last pump (mAudioReadPos += delta*2, mPlayedTotal += ...)
    // BEFORE the next producer Poll. Uses a monotonic delta (no wrap ambiguity).
    virtual void OffMainAdvanceConsumed(int frameDelta) = 0;
};
#endif

class AudioDevice {
public:
    static AudioDevice &GetInstance();

    // Initialize the audio device. Returns true on success.
    // sampleRate: 0 = use device default (usually 44100 or 48000)
    bool Init(int sampleRate = 0);
    void Terminate();
    bool IsInitialized() const { return mInitialized; }

    int GetSampleRate() const { return mSampleRate; }
#ifdef __EMSCRIPTEN__
    // Web only: the ACTUAL AudioContext rate (mDeviceSampleRate), which may differ
    // from the requested/mix rate (mSampleRate). Returns 0 before Init. PumpAudio
    // resamples mSampleRate -> this rate. == mSampleRate when the browser honored
    // the requested rate.
    int GetDeviceSampleRate() const { return mDeviceSampleRate; }
#endif

    // Source management (thread-safe)
    void AddSource(AudioSource *source);
    void RemoveSource(AudioSource *source);

    // Suspend/resume: blocks audio callback from accessing sources
    // Call Suspend() before destroying audio objects, Resume() after
    void Suspend();
    void Resume();

    // Called from miniaudio callback thread (desktop) or PumpAudio (web)
    void MixSources(float *output, int frameCount);

#ifdef __EMSCRIPTEN__
    // Web only: called each frame from main loop to push mixed audio
    // into the SharedArrayBuffer ring buffer for the AudioWorklet
    void PumpAudio();
#ifdef HX_WEB
    void DebugDumpSources();
#endif

    // Off-main mix (RB3_WEB_OFFMAIN_MIX, MVP-1). True when the flag is ON; set in
    // Init(). When ON, music stems publish to per-stem SABs and the worklet mixes
    // them on the audio thread; PumpAudio() becomes a decode/top-up pump.
    static bool OffMainMixEnabled();
    // A music stem registers/unregisters itself here when the flag is ON (instead
    // of AddSource). The pump assigns it a SAB slot and publishes it each tick.
    void RegisterMusicStem(WebMusicStem *stem);
    void UnregisterMusicStem(WebMusicStem *stem);
#endif

private:
    AudioDevice();
    ~AudioDevice();

    ma_device *mDevice;
    ma_context *mContext = nullptr;     // explicit backend context (Linux ALSA pin)
    bool mContextInited = false;
    bool mInitialized;
    int mSampleRate;

    std::mutex mSourceMutex;
    std::vector<AudioSource *> mSources;

    // Temp mix buffer for individual sources (stereo interleaved)
    std::vector<float> mMixBuffer;

    std::atomic<bool> mSuspended{false};

    // One-pole stereo-linked peak-limiter gain-reduction envelope (1.0 = no
    // reduction). Persists across audio callbacks so the release is continuous.
    float mLimiterEnv = 1.0f;

#ifdef __EMSCRIPTEN__
    // Web only. The engine mixes at mSampleRate (the mogg/decode rate, 44100). The
    // browser AudioContext may run at a DIFFERENT rate (commonly 48000, hardware-
    // locked — the requested 44100 is only a hint). mDeviceSampleRate holds the
    // ACTUAL ctx.sampleRate read back at init; PumpAudio resamples the mix from
    // mSampleRate -> mDeviceSampleRate before the SAB push so the worklet (which
    // runs at ctx.sampleRate) plays at the correct pitch instead of 48000/44100 =
    // 1.0884x fast ("chipmunks"). Persistent linear-resampler phase state below
    // keeps the resampling continuous across PumpAudio chunk boundaries.
    int mDeviceSampleRate = 0;     // 0 until Init reads it back; == mSampleRate when equal
    double mResamplePos = 0.0;     // fractional read offset from mResampleCarry[0]
    // Carry-all resampler state. MixSources() is a DESTRUCTIVE pull (it advances the
    // source stream by exactly the frames it mixes), so EVERY mix frame that was
    // pulled but not yet fully consumed must be carried, stream-contiguous, into the
    // next PumpAudio chunk. Carrying only ONE sample (the old mResampleLast*) silently
    // dropped the unconsumed tail frame on the ~8% of jittered chunks where the read
    // head didn't land on a frame boundary -> a 1-sample click/crackle on music
    // (inaudible on a constant-cadence sine, which is all earlier tests exercised).
    // 1-2 frames are carried in practice; 8 is ample headroom.
    static const int kResampleCarryMax = 8;
    float mResampleCarry[kResampleCarryMax * 2] = {0}; // interleaved stereo
    int mResampleCarryN = 0;       // valid carried frames at the front of the next mix

    // ---- Off-main music-stem registry (RB3_WEB_OFFMAIN_MIX) ----
    // Live music stems registered for off-main publishing. Indexed by SAB slot
    // (a fixed pool allocated at Init); nullptr == free slot. A stem holds the
    // same slot for its lifetime; slot reuse bumps the SAB `generation`.
    std::vector<WebMusicStem *> mMusicStems;   // slot -> stem (nullptr=free)
    std::mutex mMusicStemMutex;                // guards mMusicStems registration
    // Per-slot: last producer write frontier we published (frames), so the per-
    // tick copy only moves the newly-decoded delta into the SAB, not the whole
    // ring; and whether the slot has been primed (read cursor seeded at start).
    std::vector<int> mStemLastWrite;           // slot -> last published writeFrame
    std::vector<bool> mStemSeeded;             // slot -> has readPos been seeded?
    std::vector<int> mStemLastReadTotal;       // slot -> last-seen worklet readTotal
    void PumpAudioOffMainStems();              // top-up pump (flag ON)
#endif
};
