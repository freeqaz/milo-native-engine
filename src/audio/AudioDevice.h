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
};
