// DC3 Web Port - Audio Device (AudioWorklet + SharedArrayBuffer)
//
// Replaces the miniaudio-based AudioDevice with a browser-native path:
//   WASM main thread mixes all AudioSources -> SharedArrayBuffer ring buffer
//   AudioWorklet thread reads from ring buffer -> speaker output
//
// No ASYNCIFY needed. Push model: PumpAudio() called each frame from main loop.

#ifdef __EMSCRIPTEN__

#include "audio/AudioDevice.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <emscripten.h>

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

// ---- Audio capture for debugging ----
static const int CAPTURE_SECONDS = 3;
static const int CAPTURE_RATE = 44100;
static const int CAPTURE_FRAMES = CAPTURE_RATE * CAPTURE_SECONDS;
static float *sCaptureBuffer = nullptr; // stereo interleaved float
static int sCapturePos = 0;
static bool sCapturing = false;
static bool sCaptureReady = false;

// EM_JS functions for JS interop (handles complex brace nesting correctly)

EM_JS(void, js_audio_init, (int totalBytes, int sampleRate, int bufFrames), {
    try {
        var sab = new SharedArrayBuffer(totalBytes);
        new Int32Array(sab, 0, 2).fill(0);

        window._dc3Audio = {
            sab: sab,
            bufFrames: bufFrames,
            ctx: null,
            worklet: null,
            started: false
        };

        var ctx = new AudioContext({ sampleRate: sampleRate });
        window._dc3Audio.ctx = ctx;

        ctx.audioWorklet.addModule('audio-worklet.js').then(function() {
            var node = new AudioWorkletNode(ctx, 'dc3-audio-processor', {
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

            window._dc3Audio.worklet = node;
            window._dc3Audio.started = true;
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

EM_JS(void, js_audio_terminate, (), {
    if (window._dc3Audio && window._dc3Audio.ctx) {
        window._dc3Audio.ctx.close();
        window._dc3Audio = null;
    }
});

EM_JS(int, js_audio_worklet_started, (), {
    return (window._dc3Audio && window._dc3Audio.started) ? 1 : 0;
});

EM_JS(int, js_audio_ring_free_frames, (), {
    var audio = window._dc3Audio;
    if (!audio || !audio.sab) return 0;
    var cursors = new Int32Array(audio.sab, 0, 2);
    var writePos = Atomics.load(cursors, 0);
    var readPos = Atomics.load(cursors, 1);
    var bufFrames = audio.bufFrames;
    var used = writePos - readPos;
    if (used < 0) used += bufFrames;
    return bufFrames - used - 1;
});

EM_JS(void, js_audio_ring_write, (float *srcPtr, int frames), {
    var audio = window._dc3Audio;
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
EM_JS(void, js_download_wav, (float *pcmPtr, int numFrames, int sampleRate), {
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
    a.download = 'dc3_web_capture.wav';
    a.click();
    URL.revokeObjectURL(url);
    console.log('AudioDevice: downloaded dc3_web_capture.wav (' + numFrames + ' frames, ' + sampleRate + ' Hz)');
});

// Dump first N samples from SAB ring buffer for inspection
EM_JS(void, js_dump_sab_samples, (int count), {
    var audio = window._dc3Audio;
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

    // Create SharedArrayBuffer + AudioContext + AudioWorklet
    int totalBytes = HEADER_BYTES + RING_SAMPLES * sizeof(float);
    js_audio_init(totalBytes, mSampleRate, RING_FRAMES);

    // Set up console commands for audio debugging
    EM_ASM({
        window.dc3CaptureAudio = function() {
            Module.ccall('dc3_start_capture', null, [], []);
            console.log('Audio capture started (3 seconds)...');
        };
        window.dc3DownloadAudio = function() {
            Module.ccall('dc3_download_capture', null, [], []);
        };
        window.dc3DumpSAB = function(n) {
            Module.ccall('dc3_dump_sab', null, ['number'], [n || 20]);
        };
        window.dc3AudioStats = function() {
            Module.ccall('dc3_audio_stats', null, [], []);
        };
        console.log('Audio debug commands: dc3CaptureAudio(), dc3DownloadAudio(), dc3DumpSAB(n), dc3AudioStats()');
    });

    mInitialized = true;
    sWorkletReady = true;
    printf("AudioDevice: initialized (web) -- %d Hz, ring %d frames\n", mSampleRate, RING_FRAMES);
    return true;
}

void AudioDevice::Terminate() {
    if (!mInitialized)
        return;

    js_audio_terminate();

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

    // Clamp to [-1, 1]
    for (int i = 0; i < totalSamples; i++) {
        if (output[i] > 1.0f) output[i] = 1.0f;
        else if (output[i] < -1.0f) output[i] = -1.0f;
    }
}

static int sPumpCount = 0;

void AudioDevice::PumpAudio() {
    if (!mInitialized || !sWorkletReady || !sMixBuffer)
        return;

    // Check if AudioWorklet is ready (async setup)
    if (!js_audio_worklet_started())
        return;

    // Query free space in the SAB ring buffer
    int freeFrames = js_audio_ring_free_frames();
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
                printf("AudioDevice: capture complete (%d frames). Call dc3DownloadAudio() to save.\n", sCapturePos);
            }
        }

        js_audio_ring_write(sMixBuffer, chunk);

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

EMSCRIPTEN_KEEPALIVE void dc3_start_capture() {
    if (!sCaptureBuffer) {
        sCaptureBuffer = new float[CAPTURE_FRAMES * 2];
    }
    memset(sCaptureBuffer, 0, CAPTURE_FRAMES * 2 * sizeof(float));
    sCapturePos = 0;
    sCapturing = true;
    sCaptureReady = false;
    printf("AudioDevice: capturing %d seconds of MixSources output...\n", CAPTURE_SECONDS);
}

EMSCRIPTEN_KEEPALIVE void dc3_download_capture() {
    if (!sCaptureReady || !sCaptureBuffer) {
        printf("AudioDevice: no capture ready. Call dc3CaptureAudio() first.\n");
        return;
    }
    js_download_wav(sCaptureBuffer, sCapturePos, CAPTURE_RATE);
}

EMSCRIPTEN_KEEPALIVE void dc3_dump_sab(int count) {
    js_dump_sab_samples(count);
}

EMSCRIPTEN_KEEPALIVE void dc3_audio_stats() {
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
