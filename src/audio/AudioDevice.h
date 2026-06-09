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
#endif
};
