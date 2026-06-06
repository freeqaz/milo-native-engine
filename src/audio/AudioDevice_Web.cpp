// Audio Device (AudioWorklet + SharedArrayBuffer) — browser-native path.
//
//   WASM main thread mixes all AudioSources -> SharedArrayBuffer ring buffer
//   AudioWorklet thread reads from ring buffer -> speaker output
//
// No ASYNCIFY needed. Push model: PumpAudio() called each frame from main loop.
//
// Consumer namespace (MILO_WEB_AUDIO_NS, identifier form, default `milo`)
// prefixes both the C-side `_start_capture/_download_capture/_dump_sab/
// _audio_stats` exports and the JS-side `window._<ns>Audio` global state,
// AudioWorklet name `<ns>-audio-processor`, capture WAV filename
// `<ns>_web_capture.wav`, and `window.<ns>CaptureAudio()` debug helpers.
// DC3 sets it to `dc3`, keeping its existing public ABI; RB3 will set `rb3`.

#ifdef __EMSCRIPTEN__

#include "audio/AudioDevice.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <emscripten.h>

#ifndef MILO_WEB_AUDIO_NS
#define MILO_WEB_AUDIO_NS milo
#endif

#define MILO_WEB_AUDIO_STR_(x)        #x
#define MILO_WEB_AUDIO_STR(x)         MILO_WEB_AUDIO_STR_(x)
#define MILO_WEB_AUDIO_NS_STR         MILO_WEB_AUDIO_STR(MILO_WEB_AUDIO_NS)

// C-side token-paste: <ns>_start_capture, etc.
#define MILO_WEB_AUDIO_CAT_(a, b)     a##b
#define MILO_WEB_AUDIO_CAT(a, b)      MILO_WEB_AUDIO_CAT_(a, b)
#define MILO_WEB_AUDIO_FN(name)       MILO_WEB_AUDIO_CAT(MILO_WEB_AUDIO_NS, MILO_WEB_AUDIO_CAT(_, name))
#define MILO_WEB_AUDIO_FN_STR(name)   MILO_WEB_AUDIO_STR(MILO_WEB_AUDIO_FN(name))

// Ring buffer: 32768 frames of stereo float (~743ms at 44100Hz)
// Enough headroom for 1-3 FPS WASM frame rates
static const int RING_FRAMES = 32768;
static const int RING_SAMPLES = RING_FRAMES * 2; // stereo interleaved
static const int HEADER_BYTES = 8; // 2 x Int32 (writePos, readPos)

// Local mix buffer (WASM heap) -- MixSources writes here, then we copy to SAB
static float *sMixBuffer = nullptr;
static const int MIX_BUF_FRAMES = 8192; // mix in chunks

// Whether the AudioWorklet has been set up
static bool sWorkletReady = false;

// Soft-knee saturator (mirrors AudioDevice.cpp): transparent below kSoftKnee, then
// smoothly compresses toward (never reaching) full scale — the limiter's safety net
// so residual transient tips round off instead of square-wave clipping. Peak < 1.0.
static const float kSoftKnee = 0.95f;
static inline float SoftClip(float x) {
    float a = x < 0.0f ? -x : x;
    if (a <= kSoftKnee) return x;
    float shaped = kSoftKnee + (1.0f - kSoftKnee) * tanhf((a - kSoftKnee) / (1.0f - kSoftKnee));
    return x < 0.0f ? -shaped : shaped;
}

// ---- Audio capture for debugging ----
static const int CAPTURE_SECONDS = 3;
static const int CAPTURE_RATE = 44100;
static const int CAPTURE_FRAMES = CAPTURE_RATE * CAPTURE_SECONDS;
static float *sCaptureBuffer = nullptr; // stereo interleaved float
static int sCapturePos = 0;
static bool sCapturing = false;
static bool sCaptureReady = false;

// EM_JS functions for JS interop (handles complex brace nesting correctly).
//
// The DC3-legacy globals (window._<ns>Audio, '<ns>-audio-processor' worklet
// name, '<ns>_web_capture.wav' filename) are namespaced by MILO_WEB_AUDIO_NS.
// We pass the namespace string as a parameter to each EM_JS so the JS body
// can compose bracket-form globals (window[stateKey]) and string literals.

EM_JS(void, js_audio_init,
      (int totalBytes, int sampleRate, int bufFrames,
       const char *stateKey, const char *workletName),
{
    var key = UTF8ToString(stateKey);
    var worklet = UTF8ToString(workletName);
    try {
        var sab = new SharedArrayBuffer(totalBytes);
        new Int32Array(sab, 0, 2).fill(0);

        window[key] = {
            sab: sab,
            bufFrames: bufFrames,
            ctx: null,
            worklet: null,
            started: false
        };

        var ctx = new AudioContext({ sampleRate: sampleRate });
        window[key].ctx = ctx;

        ctx.audioWorklet.addModule('audio-worklet.js').then(function() {
            var node = new AudioWorkletNode(ctx, worklet, {
                numberOfInputs: 0,
                numberOfOutputs: 1,
                outputChannelCount: [2]
            });
            node.connect(ctx.destination);

            node.port.postMessage({
                type: 'init',
                sab: sab,
                bufFrames: bufFrames
            });

            window[key].worklet = node;
            window[key].started = true;
            console.log('AudioDevice: AudioWorklet connected (' + sampleRate + ' Hz, ring ' + bufFrames + ' frames)');
        }).catch(function(e) {
            console.error('AudioDevice: AudioWorklet failed: ' + e);
        });

        // Resume AudioContext on user gesture (browsers require interaction)
        function resumeAudio() {
            if (ctx.state === 'suspended') {
                ctx.resume().then(function() {
                    console.log('AudioDevice: AudioContext resumed');
                });
            }
        }
        document.addEventListener('keydown', resumeAudio);
        document.addEventListener('click', resumeAudio);
        document.addEventListener('touchstart', resumeAudio);

        // Try immediate resume (works if page already had interaction)
        if (ctx.state === 'suspended') {
            ctx.resume().catch(function() {});
        }
    } catch (e) {
        console.error('AudioDevice: init failed: ' + e);
        if (e.message && e.message.indexOf('SharedArrayBuffer') >= 0) {
            console.error('AudioDevice: SharedArrayBuffer not available. Check COOP/COEP headers.');
        }
    }
});

EM_JS(void, js_audio_terminate, (const char *stateKey), {
    var key = UTF8ToString(stateKey);
    if (window[key] && window[key].ctx) {
        window[key].ctx.close();
        window[key] = null;
    }
});

EM_JS(int, js_audio_worklet_started, (const char *stateKey), {
    var key = UTF8ToString(stateKey);
    return (window[key] && window[key].started) ? 1 : 0;
});

EM_JS(int, js_audio_ring_free_frames, (const char *stateKey), {
    var key = UTF8ToString(stateKey);
    var audio = window[key];
    if (!audio || !audio.sab) return 0;
    var cursors = new Int32Array(audio.sab, 0, 2);
    var writePos = Atomics.load(cursors, 0);
    var readPos = Atomics.load(cursors, 1);
    var bufFrames = audio.bufFrames;
    var used = writePos - readPos;
    if (used < 0) used += bufFrames;
    return bufFrames - used - 1;
});

EM_JS(void, js_audio_ring_write, (float *srcPtr, int frames, const char *stateKey), {
    var key = UTF8ToString(stateKey);
    var audio = window[key];
    if (!audio || !audio.sab) return;

    var cursors = new Int32Array(audio.sab, 0, 2);
    var writePos = Atomics.load(cursors, 0);
    var bufFrames = audio.bufFrames;
    var data = new Float32Array(audio.sab, 8);

    var floatIdx = srcPtr >> 2;
    var src = HEAPF32.subarray(floatIdx, floatIdx + frames * 2);

    var dstOffset = writePos * 2;
    var ringSize = bufFrames * 2;
    var remaining = ringSize - dstOffset;
    var srcSamples = frames * 2;

    if (srcSamples <= remaining) {
        data.set(src, dstOffset);
    } else {
        data.set(src.subarray(0, remaining), dstOffset);
        data.set(src.subarray(remaining), 0);
    }

    var newWritePos = (writePos + frames) % bufFrames;
    Atomics.store(cursors, 0, newWritePos);
});

// Download captured audio as WAV from the browser
EM_JS(void, js_download_wav,
      (float *pcmPtr, int numFrames, int sampleRate, const char *filename),
{
    var fname = UTF8ToString(filename);
    var numSamples = numFrames * 2;
    var floatIdx = pcmPtr >> 2;
    var pcm = HEAPF32.subarray(floatIdx, floatIdx + numSamples);

    // Convert float32 to int16
    var i16 = new Int16Array(numSamples);
    for (var i = 0; i < numSamples; i++) {
        var s = pcm[i];
        if (s > 1.0) s = 1.0;
        if (s < -1.0) s = -1.0;
        i16[i] = s * 32767;
    }

    // Build WAV file
    var dataSize = numSamples * 2;
    var buf = new ArrayBuffer(44 + dataSize);
    var view = new DataView(buf);

    // RIFF header
    view.setUint32(0, 0x52494646, false); // "RIFF"
    view.setUint32(4, 36 + dataSize, true);
    view.setUint32(8, 0x57415645, false); // "WAVE"

    // fmt chunk
    view.setUint32(12, 0x666d7420, false); // "fmt "
    view.setUint32(16, 16, true); // chunk size
    view.setUint16(20, 1, true);  // PCM
    view.setUint16(22, 2, true);  // stereo
    view.setUint32(24, sampleRate, true);
    view.setUint32(28, sampleRate * 4, true); // byte rate
    view.setUint16(32, 4, true);  // block align
    view.setUint16(34, 16, true); // bits per sample

    // data chunk
    view.setUint32(36, 0x64617461, false); // "data"
    view.setUint32(40, dataSize, true);

    // Copy PCM
    var dst = new Int16Array(buf, 44);
    dst.set(i16);

    // Download
    var blob = new Blob([buf], { type: 'audio/wav' });
    var url = URL.createObjectURL(blob);
    var a = document.createElement('a');
    a.href = url;
    a.download = fname;
    a.click();
    URL.revokeObjectURL(url);
    console.log('AudioDevice: downloaded ' + fname + ' (' + numFrames + ' frames, ' + sampleRate + ' Hz)');
});

// Dump first N samples from SAB ring buffer for inspection
EM_JS(void, js_dump_sab_samples, (int count, const char *stateKey), {
    var key = UTF8ToString(stateKey);
    var audio = window[key];
    if (!audio || !audio.sab) {
        console.log('SAB not available');
        return;
    }
    var data = new Float32Array(audio.sab, 8);
    var cursors = new Int32Array(audio.sab, 0, 2);
    console.log('SAB writePos=' + Atomics.load(cursors, 0) + ' readPos=' + Atomics.load(cursors, 1));

    var samples = [];
    var n = Math.min(count, data.length);
    for (var i = 0; i < n; i++) {
        samples.push(data[i].toFixed(6));
    }
    console.log('SAB first ' + n + ' samples: ' + samples.join(', '));

    // Stats
    var min = 0, max = 0, nonZero = 0;
    for (var i = 0; i < data.length; i++) {
        if (data[i] < min) min = data[i];
        if (data[i] > max) max = data[i];
        if (Math.abs(data[i]) > 0.0001) nonZero++;
    }
    console.log('SAB stats: min=' + min.toFixed(6) + ' max=' + max.toFixed(6) + ' nonZero=' + nonZero + '/' + data.length);
});

// ============================================================================
// AudioDevice implementation
// ============================================================================

AudioDevice &AudioDevice::GetInstance() {
    static AudioDevice instance;
    return instance;
}

AudioDevice::AudioDevice() : mDevice(nullptr), mInitialized(false), mSampleRate(0) {}
AudioDevice::~AudioDevice() { Terminate(); }

bool AudioDevice::Init(int sampleRate) {
    if (mInitialized)
        return true;

    mSampleRate = (sampleRate > 0) ? sampleRate : 44100;

    // Allocate local mix buffer
    sMixBuffer = new float[MIX_BUF_FRAMES * 2];

    // Per-consumer namespaced JS globals + symbol names.
    static const char *kStateKey       = "_" MILO_WEB_AUDIO_NS_STR "Audio";
    // The worklet processor identity is engine-wide (matches the verbatim
    // audio-worklet.js shipped by milo-native-engine). Only window globals
    // and C-exported symbols are namespaced.
    static const char *kWorkletName    = "milo-audio-processor";
    static const char *kFnStartCapture = MILO_WEB_AUDIO_FN_STR(start_capture);
    static const char *kFnDownloadCap  = MILO_WEB_AUDIO_FN_STR(download_capture);
    static const char *kFnDumpSab      = MILO_WEB_AUDIO_FN_STR(dump_sab);
    static const char *kFnAudioStats   = MILO_WEB_AUDIO_FN_STR(audio_stats);
    static const char *kNs             = MILO_WEB_AUDIO_NS_STR;

    // Create SharedArrayBuffer + AudioContext + AudioWorklet
    int totalBytes = HEADER_BYTES + RING_SAMPLES * sizeof(float);
    js_audio_init(totalBytes, mSampleRate, RING_FRAMES, kStateKey, kWorkletName);

    // Set up console commands for audio debugging
    EM_ASM({
        var ns = UTF8ToString($0);
        var fnStart = UTF8ToString($1);
        var fnDl    = UTF8ToString($2);
        var fnDump  = UTF8ToString($3);
        var fnStats = UTF8ToString($4);
        var cap = ns + 'CaptureAudio';
        var dl  = ns + 'DownloadAudio';
        var dmp = ns + 'DumpSAB';
        var st  = ns + 'AudioStats';
        window[cap] = function() {
            Module.ccall(fnStart, null, [], []);
            console.log('Audio capture started (3 seconds)...');
        };
        window[dl] = function() {
            Module.ccall(fnDl, null, [], []);
        };
        window[dmp] = function(n) {
            Module.ccall(fnDump, null, ['number'], [n || 20]);
        };
        window[st] = function() {
            Module.ccall(fnStats, null, [], []);
        };
        console.log('Audio debug commands: ' + cap + '(), ' + dl + '(), ' + dmp + '(n), ' + st + '()');
    }, kNs, kFnStartCapture, kFnDownloadCap, kFnDumpSab, kFnAudioStats);

    mInitialized = true;
    sWorkletReady = true;
    printf("AudioDevice: initialized (web) -- %d Hz, ring %d frames\n", mSampleRate, RING_FRAMES);
    return true;
}

void AudioDevice::Terminate() {
    if (!mInitialized)
        return;

    static const char *kStateKey = "_" MILO_WEB_AUDIO_NS_STR "Audio";
    js_audio_terminate(kStateKey);

    delete[] sMixBuffer;
    sMixBuffer = nullptr;
    delete[] sCaptureBuffer;
    sCaptureBuffer = nullptr;
    sWorkletReady = false;

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

void AudioDevice::MixSources(float *output, int frameCount) {
    int totalSamples = frameCount * 2;
    memset(output, 0, totalSamples * sizeof(float));

    std::lock_guard<std::mutex> lock(mSourceMutex);

    if (mSources.empty())
        return;

    // Ensure temp buffer is large enough
    if ((int)mMixBuffer.size() < totalSamples) {
        mMixBuffer.resize(totalSamples);
    }

    for (auto it = mSources.begin(); it != mSources.end(); ) {
        AudioSource *src = *it;
        memset(mMixBuffer.data(), 0, totalSamples * sizeof(float));

        int framesWritten = src->RenderAudio(mMixBuffer.data(), frameCount);

        // Additive mix
        int samplesToMix = framesWritten * 2;
        for (int i = 0; i < samplesToMix; i++) {
            output[i] += mMixBuffer[i];
        }

        if (src->IsFinished()) {
            it = mSources.erase(it);
        } else {
            ++it;
        }
    }

    // Master bus: one-pole stereo-LINKED peak limiter (web has no master gain),
    // then the hard clamp as a sub-ms transient backstop. Same processor + constants
    // as the native path (AudioDevice.cpp) so the browser output matches the desktop
    // A/B. Content-adaptive -> correct for both RB3 and DC3 without a per-game gain.
    {
        const float kLimThreshold = 0.90f, kLimAttackMs = 3.0f, kLimReleaseMs = 80.0f;
        const float aAtk = expf(-1.0f / (mSampleRate * (kLimAttackMs  / 1000.0f)));
        const float aRel = expf(-1.0f / (mSampleRate * (kLimReleaseMs / 1000.0f)));
        float env = mLimiterEnv;
        for (int f = 0; f < frameCount; f++) {
            float l = output[f * 2 + 0];
            float r = output[f * 2 + 1];
            float la = l < 0.0f ? -l : l;
            float ra = r < 0.0f ? -r : r;
            float level = la > ra ? la : ra;
            float desired = (level > kLimThreshold) ? (kLimThreshold / level) : 1.0f;
            float coeff = (desired < env) ? aAtk : aRel;   // fast down, slow up
            env = coeff * env + (1.0f - coeff) * desired;
            l *= env;
            r *= env;
            output[f * 2 + 0] = SoftClip(l);
            output[f * 2 + 1] = SoftClip(r);
        }
        mLimiterEnv = env;
    }
}

static int sPumpCount = 0;

void AudioDevice::PumpAudio() {
    if (!mInitialized || !sWorkletReady || !sMixBuffer)
        return;

    static const char *kStateKey = "_" MILO_WEB_AUDIO_NS_STR "Audio";

    // Check if AudioWorklet is ready (async setup)
    if (!js_audio_worklet_started(kStateKey))
        return;

    // Query free space in the SAB ring buffer
    int freeFrames = js_audio_ring_free_frames(kStateKey);
    if (freeFrames <= 0)
        return;

    sPumpCount++;

    // Mix and push in chunks
    while (freeFrames > 0) {
        int chunk = std::min(freeFrames, MIX_BUF_FRAMES);

        MixSources(sMixBuffer, chunk);

        // Audio capture: record MixSources output pre-SAB
        if (sCapturing && sCaptureBuffer && sCapturePos < CAPTURE_FRAMES) {
            int framesToCapture = std::min(chunk, CAPTURE_FRAMES - sCapturePos);
            memcpy(sCaptureBuffer + sCapturePos * 2, sMixBuffer, framesToCapture * 2 * sizeof(float));
            sCapturePos += framesToCapture;
            if (sCapturePos >= CAPTURE_FRAMES) {
                sCapturing = false;
                sCaptureReady = true;
                printf("AudioDevice: capture complete (%d frames). Call %sDownloadAudio() to save.\n",
                       sCapturePos, MILO_WEB_AUDIO_NS_STR);
            }
        }

        js_audio_ring_write(sMixBuffer, chunk, kStateKey);

        freeFrames -= chunk;
    }
}

#ifdef HX_WEB
void AudioDevice::DebugDumpSources() {
    std::lock_guard<std::mutex> lock(mSourceMutex);
    std::printf("AudioDevice: active source count=%zu\n", mSources.size());
    for (size_t i = 0; i < mSources.size(); i++) {
        char desc[256];
        mSources[i]->DebugDescribe(desc, sizeof(desc));
        std::printf("  [%zu] %s\n", i, desc);
    }
}
#endif

// ---- Exported C functions for JS console commands ----

extern "C" {

EMSCRIPTEN_KEEPALIVE void MILO_WEB_AUDIO_FN(start_capture)() {
    if (!sCaptureBuffer) {
        sCaptureBuffer = new float[CAPTURE_FRAMES * 2];
    }
    memset(sCaptureBuffer, 0, CAPTURE_FRAMES * 2 * sizeof(float));
    sCapturePos = 0;
    sCapturing = true;
    sCaptureReady = false;
    printf("AudioDevice: capturing %d seconds of MixSources output...\n", CAPTURE_SECONDS);
}

EMSCRIPTEN_KEEPALIVE void MILO_WEB_AUDIO_FN(download_capture)() {
    if (!sCaptureReady || !sCaptureBuffer) {
        printf("AudioDevice: no capture ready. Call %sCaptureAudio() first.\n",
               MILO_WEB_AUDIO_NS_STR);
        return;
    }
    static const char *kWavName = MILO_WEB_AUDIO_NS_STR "_web_capture.wav";
    js_download_wav(sCaptureBuffer, sCapturePos, CAPTURE_RATE, kWavName);
}

EMSCRIPTEN_KEEPALIVE void MILO_WEB_AUDIO_FN(dump_sab)(int count) {
    static const char *kStateKey = "_" MILO_WEB_AUDIO_NS_STR "Audio";
    js_dump_sab_samples(count, kStateKey);
}

EMSCRIPTEN_KEEPALIVE void MILO_WEB_AUDIO_FN(audio_stats)() {
    auto &dev = AudioDevice::GetInstance();
    printf("AudioDevice stats: initialized=%d, sampleRate=%d, pumpCount=%d\n",
           dev.IsInitialized(), dev.GetSampleRate(), sPumpCount);
    printf("  capture: active=%d, ready=%d, pos=%d/%d\n",
           sCapturing, sCaptureReady, sCapturePos, CAPTURE_FRAMES);
#ifdef HX_WEB
    dev.DebugDumpSources();
#endif
}

} // extern "C"

#endif // __EMSCRIPTEN__
