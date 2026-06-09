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
#include <cmath>
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

// Master-bus peak limiter (replaces the old fixed master-gain boost). A song mixes
// 11-15 separate stems; even at their authored ~-4 dB vols the additive sum peaks
// ~3x full scale, so the prior 1.1x boost + hard clamp square-wave-clipped every
// loud section ("clipped noise"). The real Wii had a unity master fader and relied
// on the AX/DSP hardware mixer for saturation. A content-adaptive one-pole peak
// limiter tames only the transient peaks (the ~95% body passes through untouched),
// so it is correct for both RB3 and DC3 from one constant set — no per-game gain.
// The existing [-1,1] hard clamp is kept as a sub-ms brick-wall backstop (a
// lookahead-free limiter can't catch the very first sample of a fast attack).
// sPreGain is an optional pre-limiter trim, overridable via DC3_AUDIO_GAIN.
static float sPreGain = 1.0f;
static const float kLimThreshold = 0.90f;   // begin gain reduction when |peak| > this
static const float kLimReleaseMs = 80.0f;   // slow one-pole release (no pumping)
// DIAGNOSTIC A/B knobs (wave-09 HF-static probe). Default = current behavior.
//   RB3_LIM_BYPASS=1     -> skip the limiter entirely (raw additive sum, no gain mod)
//   RB3_LIM_ATTACK_MS=N  -> finite attack time instead of instant (0 / unset = instant)
static bool  sLimBypass   = false;
static float sLimAttackMs = 0.0f;    // 0 = instant attack (the committed behavior)

// Soft-knee saturator: transparent below kSoftKnee, smoothly compresses the region
// above it toward (but never reaching) full scale. Replaces the hard clamp as the
// limiter's safety net so the rare transient tips a lookahead-free one-pole can't
// pre-duck round off smoothly instead of square-wave clipping. Peak stays < 1.0.
static const float kSoftKnee = 0.95f;
static inline float SoftClip(float x) {
    float a = x < 0.0f ? -x : x;
    if (a <= kSoftKnee) return x;
    float shaped = kSoftKnee + (1.0f - kSoftKnee) * tanhf((a - kSoftKnee) / (1.0f - kSoftKnee));
    return x < 0.0f ? -shaped : shaped;
}
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
    // MILO_AUDIO=1 overrides both skips — allows audio-on with headless rendering
    // (e.g. V1 acceptance: MILO_HEADLESS=1 MILO_AUDIO=1 keeps Dawn windowless while
    // still opening the miniaudio output device for RenderAudio validation).
    bool forceAudio = (getenv("MILO_AUDIO") != nullptr &&
                       getenv("MILO_AUDIO")[0] == '1');
    if (!forceAudio && (getenv("DC3_NO_AUDIO") || getenv("MILO_HEADLESS"))) {
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

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    // BOOT RELIABILITY (Linux): pin the backend to ALSA, not PulseAudio.
    //
    // miniaudio's default Linux backend order is PulseAudio-first. On a host
    // running PipeWire (which provides the PulseAudio server via libpulse's
    // pipewire-pulse shim), the libpulse client connection spins up SPA-plugin
    // loader threads that dlopen EGL/GL plugins against the SAME GPU driver the
    // renderer is mid-initialising. That concurrent driver touch faults an
    // NVIDIA Vulkan driver worker thread (SIGSEGV deep in libnvidia-eglcore,
    // never in our code) ~90% of headless boots. ALSA opens the device directly
    // with no client/plugin threads, removing the contention. ALSA on a
    // PipeWire host still routes through the pipewire-alsa plug. Override with
    // MILO_AUDIO_BACKEND=pulseaudio|alsa|default to force a specific choice.
    const char *beEnv = getenv("MILO_AUDIO_BACKEND");
    ma_backend backends[1];
    ma_uint32 backendCount = 0;
    if (!beEnv || strcmp(beEnv, "default") != 0) {
        if (beEnv && strcmp(beEnv, "pulseaudio") == 0)
            backends[0] = ma_backend_pulseaudio;
        else if (beEnv && strcmp(beEnv, "null") == 0)
            // Always-openable dummy device on a real ~realtime clock. No
            // hardware, no plugin/SPA loader threads (so no GPU-driver
            // contention), but ma_device_init succeeds and the data callback
            // (MixSources) runs — proving the mix/synth pipeline + the
            // DC3_DUMP_AUDIO PCM-capture path on hosts with no usable audio
            // sink. Opt-in only via MILO_AUDIO_BACKEND=null; default (unset /
            // alsa / pulseaudio / default) is unchanged.
            backends[0] = ma_backend_null;
        else
            backends[0] = ma_backend_alsa; // default + "alsa"
        backendCount = 1;
    }

    ma_result result;
    if (backendCount > 0) {
        mContext = new ma_context;
        ma_context_config ctxConfig = ma_context_config_init();
        if (ma_context_init(backends, backendCount, &ctxConfig, mContext) == MA_SUCCESS) {
            mContextInited = true;
            result = ma_device_init(mContext, &config, mDevice);
        } else {
            // Backend unavailable — fall back to miniaudio's default selection.
            delete mContext;
            mContext = nullptr;
            result = ma_device_init(nullptr, &config, mDevice);
        }
    } else {
        result = ma_device_init(nullptr, &config, mDevice);
    }
#else
    ma_result result = ma_device_init(nullptr, &config, mDevice);
#endif

    // Restore stderr
    if (savedStderr >= 0) {
        dup2(savedStderr, STDERR_FILENO);
        close(savedStderr);
    }

    if (result != MA_SUCCESS) {
        fprintf(stderr, "AudioDevice: ma_device_init failed: %d\n", result);
        delete mDevice;
        mDevice = nullptr;
        if (mContextInited) { ma_context_uninit(mContext); mContextInited = false; }
        delete mContext; mContext = nullptr;
        return false;
    }

    mSampleRate = (int)mDevice->sampleRate;

    result = ma_device_start(mDevice);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "AudioDevice: ma_device_start failed: %d\n", result);
        ma_device_uninit(mDevice);
        delete mDevice;
        mDevice = nullptr;
        if (mContextInited) { ma_context_uninit(mContext); mContextInited = false; }
        delete mContext; mContext = nullptr;
        return false;
    }

    mInitialized = true;

    // Optional pre-limiter trim from environment (default unity).
    const char *gainEnv = getenv("DC3_AUDIO_GAIN");
    if (gainEnv) sPreGain = (float)atof(gainEnv);
    // DIAGNOSTIC limiter A/B knobs (wave-09 HF-static probe).
    sLimBypass = (getenv("RB3_LIM_BYPASS") != nullptr && getenv("RB3_LIM_BYPASS")[0] == '1');
    const char *atkEnv = getenv("RB3_LIM_ATTACK_MS");
    if (atkEnv) sLimAttackMs = (float)atof(atkEnv);
    printf("AudioDevice: initialized — %d Hz, %d channels, period %d frames, pregain %.2f (backend=%s)\n",
           mSampleRate, 2, 512, sPreGain,
           mContextInited ? ma_get_backend_name(mContext->backend) : "default");

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
    if (mContextInited) {
        ma_context_uninit(mContext);
        mContextInited = false;
    }
    delete mContext;
    mContext = nullptr;
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
    mLimiterEnv = 1.0f;   // reset gain-reduction envelope across a suspend gap
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

            // Master bus: optional pre-gain, one-pole stereo-LINKED peak limiter,
            // then the hard clamp as a sub-ms transient backstop. Stereo-linked
            // (one envelope driven by max(|L|,|R|)) keeps the stereo image stable.
            const float aRel = expf(-1.0f / (mSampleRate * (kLimReleaseMs / 1000.0f)));
            // DIAGNOSTIC: finite attack coefficient when RB3_LIM_ATTACK_MS>0 (else instant).
            const float aAtk = (sLimAttackMs > 0.0f)
                ? expf(-1.0f / (mSampleRate * (sLimAttackMs / 1000.0f))) : 0.0f;
            float env = mLimiterEnv;
            for (int f = 0; f < frameCount; f++) {
                float l = output[f * 2 + 0] * sPreGain;
                float r = output[f * 2 + 1] * sPreGain;
                if (sLimBypass) {
                    // DIAGNOSTIC bypass: raw additive sum, NO per-sample gain modulation.
                    // Pair with a low DC3_AUDIO_GAIN so SoftClip/clamp don't engage, to
                    // isolate the limiter's gain-modulation HF contribution.
                    output[f * 2 + 0] = SoftClip(l);
                    output[f * 2 + 1] = SoftClip(r);
                    continue;
                }
                float la = l < 0.0f ? -l : l;
                float ra = r < 0.0f ? -r : r;
                float level = la > ra ? la : ra;
                float desired = (level > kLimThreshold) ? (kLimThreshold / level) : 1.0f;
                // INSTANT attack: the gain drops immediately to the exact value that
                // holds the post-gain sample at the threshold (|out| <= kLimThreshold),
                // so the softclip/clamp never engage on a real peak and the output
                // cannot rail. A finite attack — even 0.05 ms — lets the FIRST sample
                // of a fast bass transient (a 1/4-cycle of 200 Hz is 1.25 ms) overshoot
                // into the rail (the old 3 ms attack square-wave-clipped loud sections).
                // One-pole release recovers smoothly with no pumping.
                if (desired < env) env = (aAtk > 0.0f) ? (aAtk * env + (1.0f - aAtk) * desired) : desired;
                else env = aRel * env + (1.0f - aRel) * desired;
                output[f * 2 + 0] = SoftClip(l * env);
                output[f * 2 + 1] = SoftClip(r * env);
            }
            mLimiterEnv = env;
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
