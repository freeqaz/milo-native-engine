// DC3 Native Port - Audio Device implementation
// Uses miniaudio for cross-platform audio output.
//
// WAV dump mode:
//   Set DC3_DUMP_AUDIO=/path/to/output.wav to capture the first N seconds
//   of mixed audio output as a 16-bit PCM WAV file.
//   Set DC3_DUMP_SECONDS=N to control duration (default: 5).
//   The WAV is finalized and closed when the cap is reached or on shutdown.

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING   // we don't encode audio
#define MA_NO_GENERATION // we don't need built-in waveform generation
#include "audio/miniaudio.h"

#include "audio/AudioDevice.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>

// ---------------------------------------------------------------------------
// WAV dump state (file-scope, only used by desktop builds)
// ---------------------------------------------------------------------------
static FILE *sDumpFile = nullptr;
static int sDumpMaxFrames = 0;       // total frames to capture
static int sDumpFramesWritten = 0;   // frames captured so far

// Master gain applied to all mixed audio output.
// Default 2.0 compensates for quiet mix levels in the native port.
// Override with DC3_AUDIO_GAIN=<float> environment variable.
static float sMasterGain = 1.1f;
static std::atomic<bool> sDumpFinalized{false};

// Write a 44-byte RIFF/WAV header for 16-bit stereo PCM.
// dataSize can be 0 on first write; we patch it later.
static void WriteWavHeader(FILE *f, int sampleRate, int dataSize) {
    auto write16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    auto write32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };

    int channels = 2;
    int bitsPerSample = 16;
    int byteRate = sampleRate * channels * (bitsPerSample / 8);
    int blockAlign = channels * (bitsPerSample / 8);

    fwrite("RIFF", 1, 4, f);
    write32(36 + dataSize);           // ChunkSize
    fwrite("WAVE", 1, 4, f);

    fwrite("fmt ", 1, 4, f);
    write32(16);                      // Subchunk1Size (PCM)
    write16(1);                       // AudioFormat = PCM
    write16(channels);
    write32(sampleRate);
    write32(byteRate);
    write16(blockAlign);
    write16(bitsPerSample);

    fwrite("data", 1, 4, f);
    write32(dataSize);                // Subchunk2Size
}

// Patch the WAV header with final data size, then close.
// Thread-safe: uses atomic exchange so only one thread finalizes.
static void FinalizeWavDump(int sampleRate) {
    if (sDumpFinalized.exchange(true))
        return; // another thread already finalized
    if (!sDumpFile)
        return;

    int dataSize = sDumpFramesWritten * 2 * sizeof(int16_t); // stereo, 16-bit
    fseek(sDumpFile, 0, SEEK_SET);
    WriteWavHeader(sDumpFile, sampleRate, dataSize);
    fclose(sDumpFile);
    sDumpFile = nullptr;

    printf("AudioDevice: WAV dump finalized — %d frames (%.2f seconds)\n",
           sDumpFramesWritten, (float)sDumpFramesWritten / sampleRate);
}

// Append float samples as 16-bit PCM.  Returns frames actually written.
static int DumpFramesToWav(const float *output, int frameCount) {
    if (!sDumpFile || sDumpFinalized)
        return 0;

    int framesToWrite = frameCount;
    if (sDumpFramesWritten + framesToWrite > sDumpMaxFrames) {
        framesToWrite = sDumpMaxFrames - sDumpFramesWritten;
    }
    if (framesToWrite <= 0)
        return 0;

    int samplesToWrite = framesToWrite * 2; // stereo
    // Stack-allocate for small chunks (typical: 512 frames = 2048 bytes)
    int16_t buf[2048];
    int remaining = samplesToWrite;
    const float *src = output;

    while (remaining > 0) {
        int chunk = (remaining > 2048) ? 2048 : remaining;
        for (int i = 0; i < chunk; i++) {
            float s = src[i];
            if (s > 1.0f) s = 1.0f;
            else if (s < -1.0f) s = -1.0f;
            buf[i] = (int16_t)(s * 32767.0f);
        }
        fwrite(buf, sizeof(int16_t), chunk, sDumpFile);
        src += chunk;
        remaining -= chunk;
    }

    sDumpFramesWritten += framesToWrite;
    return framesToWrite;
}

static void MaDataCallback(ma_device *device, void *output, const void * /*input*/, ma_uint32 frameCount) {
    AudioDevice *ad = (AudioDevice *)device->pUserData;
    ad->MixSources((float *)output, (int)frameCount);
}

AudioDevice &AudioDevice::GetInstance() {
    static AudioDevice instance;
    return instance;
}

AudioDevice::AudioDevice() : mDevice(nullptr), mInitialized(false), mSampleRate(0) {}

AudioDevice::~AudioDevice() {
    Terminate();
}

bool AudioDevice::Init(int sampleRate) {
    if (mInitialized)
        return true;

    // Opt-out: skip audio init entirely for headless test runs.
    // PipeWire/SPA plugins on some hosts crash on the audio thread during
    // device init; headless tests don't need audio output anyway.
    // DC3_NO_AUDIO=1 explicitly opts out; MILO_HEADLESS=1 implies it.
    if (getenv("DC3_NO_AUDIO") || getenv("MILO_HEADLESS")) {
        printf("AudioDevice: skipped (DC3_NO_AUDIO or MILO_HEADLESS set)\n");
        return true;
    }

    mDevice = new ma_device;

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2; // stereo
    config.sampleRate = (sampleRate > 0) ? (ma_uint32)sampleRate : 0; // 0 = device default
    config.dataCallback = MaDataCallback;
    config.pUserData = this;
    config.periodSizeInFrames = 512; // ~10ms at 48kHz — low latency for rhythm game

    // Suppress ALSA error spam during device enumeration (no sound card = ~30 lines of noise)
    int savedStderr = dup(STDERR_FILENO);
    int devNull = open("/dev/null", O_WRONLY);
    if (devNull >= 0) {
        dup2(devNull, STDERR_FILENO);
        close(devNull);
    }

    ma_result result = ma_device_init(nullptr, &config, mDevice);

    // Restore stderr
    if (savedStderr >= 0) {
        dup2(savedStderr, STDERR_FILENO);
        close(savedStderr);
    }

    if (result != MA_SUCCESS) {
        fprintf(stderr, "AudioDevice: ma_device_init failed: %d\n", result);
        delete mDevice;
        mDevice = nullptr;
        return false;
    }

    mSampleRate = (int)mDevice->sampleRate;

    result = ma_device_start(mDevice);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "AudioDevice: ma_device_start failed: %d\n", result);
        ma_device_uninit(mDevice);
        delete mDevice;
        mDevice = nullptr;
        return false;
    }

    mInitialized = true;

    // Read master gain from environment
    const char *gainEnv = getenv("DC3_AUDIO_GAIN");
    if (gainEnv) sMasterGain = (float)atof(gainEnv);
    printf("AudioDevice: initialized — %d Hz, %d channels, period %d frames, gain %.1f\n",
           mSampleRate, 2, 512, sMasterGain);

    // --- WAV dump setup ---
    const char *dumpPath = getenv("DC3_DUMP_AUDIO");
    if (dumpPath && dumpPath[0]) {
        const char *dumpSecsStr = getenv("DC3_DUMP_SECONDS");
        int dumpSecs = 5; // default
        if (dumpSecsStr && dumpSecsStr[0])
            dumpSecs = atoi(dumpSecsStr);
        if (dumpSecs <= 0) dumpSecs = 5;

        sDumpMaxFrames = mSampleRate * dumpSecs;
        sDumpFramesWritten = 0;
        sDumpFinalized = false;

        sDumpFile = fopen(dumpPath, "wb");
        if (sDumpFile) {
            // Unbuffered writes so _exit(0) doesn't lose data
            setvbuf(sDumpFile, nullptr, _IONBF, 0);
            WriteWavHeader(sDumpFile, mSampleRate, 0); // placeholder header
            printf("AudioDevice: WAV dump enabled — %s (%d seconds, %d Hz)\n",
                   dumpPath, dumpSecs, mSampleRate);
        } else {
            fprintf(stderr, "AudioDevice: failed to open WAV dump file: %s\n", dumpPath);
        }
    }

    return true;
}

void AudioDevice::Terminate() {
    if (!mInitialized)
        return;

    // Finalize WAV dump before shutting down
    FinalizeWavDump(mSampleRate);

    ma_device_uninit(mDevice);
    delete mDevice;
    mDevice = nullptr;
    mInitialized = false;
    mSampleRate = 0;

    std::lock_guard<std::mutex> lock(mSourceMutex);
    mSources.clear();
}

void AudioDevice::AddSource(AudioSource *source) {
    std::lock_guard<std::mutex> lock(mSourceMutex);
    mSources.push_back(source);
}

void AudioDevice::RemoveSource(AudioSource *source) {
    std::lock_guard<std::mutex> lock(mSourceMutex);
    mSources.erase(
        std::remove(mSources.begin(), mSources.end(), source),
        mSources.end()
    );
}

void AudioDevice::Suspend() {
    mSuspended.store(true, std::memory_order_release);
    // Acquire the mutex to ensure the audio thread isn't mid-render
    std::lock_guard<std::mutex> lock(mSourceMutex);
}

void AudioDevice::Resume() {
    mSuspended.store(false, std::memory_order_release);
}

void AudioDevice::MixSources(float *output, int frameCount) {
    int totalSamples = frameCount * 2; // stereo
    memset(output, 0, totalSamples * sizeof(float));

    if (mSuspended.load(std::memory_order_acquire)) {
        goto wav_dump;
    }

    {
        std::lock_guard<std::mutex> lock(mSourceMutex);

        if (!mSources.empty()) {
            // Ensure temp buffer is large enough
            if ((int)mMixBuffer.size() < totalSamples) {
                mMixBuffer.resize(totalSamples);
            }

            for (auto it = mSources.begin(); it != mSources.end(); ) {
                AudioSource *src = *it;
                if (!src) {
                    it = mSources.erase(it);
                    continue;
                }
                memset(mMixBuffer.data(), 0, totalSamples * sizeof(float));

                int framesWritten = src->RenderAudio(mMixBuffer.data(), frameCount);

                // Additive mix into output
                int samplesToMix = framesWritten * 2;
                for (int i = 0; i < samplesToMix; i++) {
                    output[i] += mMixBuffer[i];
                }

                // Remove finished sources
                if (src->IsFinished()) {
                    it = mSources.erase(it);
                } else {
                    ++it;
                }
            }

            // Apply master gain and clamp to [-1, 1]
            for (int i = 0; i < totalSamples; i++) {
                output[i] *= sMasterGain;
                if (output[i] > 1.0f) output[i] = 1.0f;
                else if (output[i] < -1.0f) output[i] = -1.0f;
            }
        }
    }

wav_dump:
    // WAV dump: capture post-mix output (including silence)
    if (sDumpFile && !sDumpFinalized) {
        DumpFramesToWav(output, frameCount);
        if (sDumpFramesWritten >= sDumpMaxFrames) {
            FinalizeWavDump(mSampleRate);
        }
    }
}
