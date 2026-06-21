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
#include <cstdlib>
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

// ---- Off-main mix (RB3_WEB_OFFMAIN_MIX, MVP-1) ----------------------------
// The flag, read once in Init() (env getenv). When ON, music stems publish their
// decoded int16 rings into per-stem SABs and the AudioWorklet mixes on the audio
// thread; PumpAudio() becomes a decode/top-up pump. Default OFF (shipping path
// untouched). See docs/native/audio-thread-2026-06-20/05-BUILD-SPEC-offmain-mvp1.md.
static bool sOffMainMix = false;
// Fixed pool of stem slots (a song has ~6-15 stems; 16 covers worst case).
static const int kMaxStems = 16;
// Per-stem SAB ring length in frames = mBuffer/2 for the full 16-chunk ring
// (0xC0000 bytes = 393216 int16 frames ~= 8.9 s). Mirrors StreamReceiver.mBuffer.
static const int kStemRingFrames = 0xC0000 / 2;
// Per-stem SAB header: 8 x Int32, then `ringFrames` x Int16 PCM.
//   [0] writePos   (frames, producer/pump)   [1] readPos    (frames, worklet)
//   [2] ringFrames (const)                   [3] generation (producer)
//   [4] readTotalLo (MONOTONIC frames consumed since seed, worklet-owned; the
//       pump diffs it to advance producer back-pressure with NO wrap ambiguity)
//   [5..7] reserved
static const int kStemHeaderBytes = 32;
static const int kStemHdrReadTotal = 4; // int32 index of the monotonic counter
// Control SAB header (Int32): activeMask, targetDepthFrames, mixRate, ctxRate.
// Then per slot s: gain(f32), pan(f32), flags(i32), generation(i32) at
// word index 4 + s*4.
static const int kCtrlHeaderInts = 4;
// Fixed output latency floor for off-main (ctx frames computed at Init). The
// stem rings (~9 s) carry the stall budget; the output floor is just the prime
// cushion + a small target depth. 70 ms initial (tune 60-80 from low-water).
static int sOffMainFloorMs = 70;
static int sOffMainTargetFrames = 0; // ctx frames, published to control SAB

// Local mix buffer (WASM heap) -- MixSources writes here (at the MIX/engine rate).
static float *sMixBuffer = nullptr;
static const int MIX_BUF_FRAMES = 8192; // mix in chunks
// Output buffer (WASM heap) -- the mix resampled to the DEVICE/ctx rate, pushed to
// the SAB. Only used when ctx rate != mix rate; same frame capacity as the mix buf
// (we always produce <= MIX_BUF_FRAMES device-rate frames per inner iteration).
static float *sOutBuffer = nullptr;

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

// ---- Debug rate/pitch verification tone (opt-in via rb3_debug_tone(hz)) ----
// When sDebugToneHz > 0, PumpAudio overwrites the freshly-mixed MIX-rate block
// with a pure sine at sDebugToneHz generated AT mSampleRate (the engine rate),
// using a persistent phase. The resampler then converts it to the device rate. A
// correct resampler keeps the captured tone at sDebugToneHz (NOT hz*devRate/mixRate),
// giving a deterministic, content-controlled, single-run proof of the rate fix.
static double sDebugToneHz = 0.0;
static double sDebugTonePhase = 0.0;
static void FillDebugTone(float *mixStereo, int frames, int mixRate) {
    if (sDebugToneHz <= 0.0 || mixRate <= 0) return;
    const double inc = 2.0 * 3.14159265358979323846 * sDebugToneHz / (double)mixRate;
    for (int f = 0; f < frames; f++) {
        float v = (float)(0.5 * sin(sDebugTonePhase));
        mixStereo[f * 2 + 0] = v;
        mixStereo[f * 2 + 1] = v;
        sDebugTonePhase += inc;
        if (sDebugTonePhase > 2.0 * 3.14159265358979323846)
            sDebugTonePhase -= 2.0 * 3.14159265358979323846;
    }
}

// ---- Audio capture for debugging ----
// 30s so a headless harness can capture a full sustained-gameplay window and
// compute a per-second RMS envelope + spectrogram-shape correlation vs the
// ground-truth reference (3s was too short to prove a continuous run).
static const int CAPTURE_SECONDS = 30;
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

// Returns the ACTUAL AudioContext sample rate (the browser may ignore the
// requested rate and lock to the hardware rate, commonly 48000). The caller
// resamples the mix from the engine rate to this rate before the SAB push.
// Returns 0 on failure (caller falls back to the requested rate).
EM_JS(int, js_audio_init,
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

        // Create the context at the HARDWARE's native rate -- do NOT request the
        // mix rate (sampleRate). If we ask for 44100 the browser honors it, then the
        // OS/driver (PipeWire/ALSA/CoreAudio) must resample 44100 -> the real device
        // rate (commonly 48000) at the output. On some stacks (notably Linux) that
        // final SRC is bypassed/broken, so 44100 PCM is clocked out at 48000 =
        // 1.0884x fast (chipmunk). Letting the context default to the device rate
        // means the OS never resamples OUR stream; instead PumpAudio's resampler
        // converts the 44100 mix -> ctx.sampleRate (this is what the native build
        // does via miniaudio). `sampleRate` is still passed in as the MIX rate.
        var ctx = new AudioContext();
        window[key].ctx = ctx;
        // The real device rate the worklet + our resampler must target.
        var actualRate = ctx.sampleRate;

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

            // Receive periodic underrun summaries from the worklet (additive,
            // backward-compatible). Stash the latest on the state object so a
            // headless harness can poll window[key].underruns and compute
            // underruns-per-second during sustained gameplay.
            window[key].underruns = {
                underrunEvents: 0, underrunFrames: 0,
                totalQuanta: 0, totalFrames: 0,
                minRingDepthFrames: 0
            };
            node.port.onmessage = function(ev) {
                if (ev.data && ev.data.type === 'underrun-stats') {
                    window[key].underruns = ev.data;
                } else if (ev.data && ev.data.type === 'offmain-dbg') {
                    console.log('OFFMAIN-DBG ' + JSON.stringify(ev.data));
                }
            };

            window[key].worklet = node;
            window[key].started = true;
            // Off-main: if a stem/control SAB config was allocated before the
            // worklet connected (js_offmain_alloc set offmainPending), post it now.
            if (window[key].offmainPending) {
                node.port.postMessage(window[key].offmainPending);
                window[key].offmainPending = null;
            }
            console.log('AudioDevice: AudioWorklet connected (ctx ' + actualRate + ' Hz' +
                        (actualRate !== sampleRate ? ' [requested ' + sampleRate + ', resampling]' : '') +
                        ', ring ' + bufFrames + ' frames)');
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
        return actualRate;
    } catch (e) {
        console.error('AudioDevice: init failed: ' + e);
        if (e.message && e.message.indexOf('SharedArrayBuffer') >= 0) {
            console.error('AudioDevice: SharedArrayBuffer not available. Check COOP/COEP headers.');
        }
        return 0;
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

// Read the latest worklet underrun summary into a caller-provided int[4]
// (events, paddedFrames, totalQuanta, totalFrames). Returns 1 if stats are
// present, 0 otherwise. Lets the C side print them in rb3AudioStats().
EM_JS(int, js_audio_underrun_stats, (int *out4, const char *stateKey), {
    var key = UTF8ToString(stateKey);
    var audio = window[key];
    if (!audio || !audio.underruns) return 0;
    var u = audio.underruns;
    var idx = out4 >> 2;
    HEAP32[idx + 0] = u.underrunEvents | 0;
    HEAP32[idx + 1] = u.underrunFrames | 0;
    HEAP32[idx + 2] = u.totalQuanta | 0;
    HEAP32[idx + 3] = u.totalFrames | 0;
    return 1;
});

// Read the worklet's per-window ring low-water mark (smallest `available` ring
// depth, in device frames, seen during the last ~0.5s window). Separate from the
// fixed 4-int js_audio_underrun_stats contract so existing int[4] callers are
// untouched. Writes 0 and returns 0 if the field isn't available yet (pre-first
// worklet message, where the init literal seeds minRingDepthFrames: 0).
EM_JS(int, js_audio_min_ring_depth, (int *outFrames, const char *stateKey), {
    var key = UTF8ToString(stateKey);
    var audio = window[key];
    if (!audio || !audio.underruns ||
        typeof audio.underruns.minRingDepthFrames === 'undefined') {
        if (outFrames) HEAP32[outFrames >> 2] = 0;
        return 0;
    }
    HEAP32[outFrames >> 2] = audio.underruns.minRingDepthFrames | 0;
    return 1;
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

// ============================================================================
// Off-main mix (RB3_WEB_OFFMAIN_MIX) — per-stem SAB allocation + publish
// ============================================================================

// Allocate the fixed stem-SAB pool + the control SAB, stash them on the state
// object, and post 'init-offmain' to the worklet. Called once from Init() when
// the flag is ON (the worklet is already being created by js_audio_init; this
// re-posts the off-main init once the node connects — see the onmessage hookup
// in js_audio_init, which forwards a pending off-main config). Returns 1 on
// success, 0 on failure (caller falls back to the OFF path).
EM_JS(int, js_offmain_alloc,
      (int maxStems, int stemHeaderBytes, int ringFrames, int ctrlHeaderInts,
       int mixRate, int ctxRate, int primeFrames, int dbg, const char *stateKey),
{
    var key = UTF8ToString(stateKey);
    var audio = window[key];
    if (!audio) return 0;
    try {
        var stemBytes = stemHeaderBytes + ringFrames * 2; // int16 ring
        var stemSabs = [];
        var hdrInts = stemHeaderBytes >> 2; // 8
        for (var s = 0; s < maxStems; s++) {
            var sab = new SharedArrayBuffer(stemBytes);
            // header: writePos, readPos, ringFrames, generation, readTotal, ...
            var hdr = new Int32Array(sab, 0, hdrInts);
            hdr.fill(0);
            hdr[2] = ringFrames;
            stemSabs.push(sab);
        }
        // control SAB: header ints + maxStems*4 words (gain,pan,flags,gen).
        var ctrlBytes = (ctrlHeaderInts + maxStems * 4) * 4;
        var ctrlSab = new SharedArrayBuffer(ctrlBytes);
        var ci = new Int32Array(ctrlSab);
        ci.fill(0);
        ci[2] = mixRate; ci[3] = ctxRate;

        audio.offmain = {
            stemSabs: stemSabs,
            ctrlSab: ctrlSab,
            ringFrames: ringFrames,
            stemHeaderBytes: stemHeaderBytes,
            // typed views cached for the per-tick publish (avoid re-wrapping).
            stemHdr: stemSabs.map(function(sab){ return new Int32Array(sab, 0, stemHeaderBytes >> 2); }),
            stemPcm: stemSabs.map(function(sab){ return new Int16Array(sab, stemHeaderBytes); }),
            stemPcmU8: stemSabs.map(function(sab){ return new Uint8Array(sab, stemHeaderBytes); }),
            ctrlI: ci,
            ctrlF: new Float32Array(ctrlSab),
        };

        // Post init-offmain to the worklet now if it's connected; otherwise the
        // js_audio_init onmessage 'started' path will post it (see the pending
        // flag below). We set a pending payload either way.
        var payload = {
            type: 'init-offmain',
            stemSabs: stemSabs,
            ctrlSab: ctrlSab,
            ringFrames: ringFrames,
            stemHeaderBytes: stemHeaderBytes,
            mixRate: mixRate,
            ctxRate: ctxRate,
            primeFrames: primeFrames,
            maxStems: maxStems,
            dbg: dbg !== 0,
        };
        audio.offmainPending = payload;
        if (audio.worklet) {
            audio.worklet.port.postMessage(payload);
            audio.offmainPending = null;
        }
        return 1;
    } catch (e) {
        console.error('AudioDevice: offmain alloc failed: ' + e);
        return 0;
    }
});

// Publish a stem this tick: copy the newly-decoded PCM delta from the WASM-heap
// source ring (srcPtr = int16 mono ring, ringFrames long) into the stem SAB
// ring, set the SAB availability writePos, and write gain/pan/flags into the
// control SAB.
//   slot         : stem slot index
//   srcPtr       : byte ptr into HEAP for the producer int16 ring (mBuffer)
//   lastWrite    : the DATA frontier we last published (start of the new delta)
//   newWrite     : the producer's current DATA frontier (frames, mRingWritePos/2)
//   availWrite   : the AVAILABILITY frontier (readFrame+availFrames) mod ringFrames,
//                  what the worklet's (writePos-readPos) reads as depth
//   gain, pan    : per-stem params
//   flags        : bit0=paused, bit1=finished
//   generation   : mirrored into both SABs
EM_JS(void, js_offmain_publish_stem,
      (int slot, int srcPtr, int lastWrite, int newWrite, int availWrite,
       double gain, double pan, int flags, int generation, const char *stateKey),
{
    var key = UTF8ToString(stateKey);
    var audio = window[key];
    if (!audio || !audio.offmain) return;
    var om = audio.offmain;
    if (slot < 0 || slot >= om.stemPcm.length) return;
    var ringFrames = om.ringFrames;
    var hdr = om.stemHdr[slot];
    // BYTE-ACCURATE copy: mBuffer (srcPtr) is often at an ODD wasm-heap address
    // (the StreamReceiver object isn't 2-byte aligned), so an Int16Array view of
    // it would be misaligned -> byte-shifted garbage. Copy via HEAPU8 (byte
    // granularity) into the SAB's byte view. 1 int16 frame = 2 bytes.
    var dstU8 = om.stemPcmU8[slot];                  // Uint8Array(sab, headerBytes)
    var srcU8 = HEAPU8;                               // byte view of the wasm heap
    var srcBase = srcPtr;                            // byte offset of mBuffer

    // Copy the new DATA delta [lastWrite, newWrite) mod ringFrames into the SAB.
    var lw = lastWrite, nw = newWrite;
    var count = nw - lw;                              // frames
    if (count < 0) count += ringFrames;
    if (count > ringFrames) count = ringFrames;
    if (count > 0) {
        var startSrc = ((lw % ringFrames) + ringFrames) % ringFrames; // frame index
        var firstLen = ringFrames - startSrc;        // frames until ring end
        var n1 = (count <= firstLen) ? count : firstLen;
        // part 1: [startSrc, startSrc+n1) frames -> bytes [startSrc*2 .. )
        dstU8.set(srcU8.subarray(srcBase + startSrc * 2, srcBase + (startSrc + n1) * 2),
                  startSrc * 2);
        if (count > firstLen) {
            var n2 = count - firstLen;               // wraps to ring start
            dstU8.set(srcU8.subarray(srcBase, srcBase + n2 * 2), 0);
        }
    }
    // Publish the AVAILABILITY writePos with a release store (worklet acquire-loads).
    Atomics.store(hdr, 0, ((availWrite % ringFrames) + ringFrames) % ringFrames);
    hdr[3] = generation;

    // Control SAB per-slot params.
    var ci = om.ctrlI, cf = om.ctrlF;
    var base = 4 + slot * 4;
    cf[base + 0] = gain;
    cf[base + 1] = pan;
    Atomics.store(ci, base + 2, flags);
    Atomics.store(ci, base + 3, generation);
});

// Seed a stem's worklet readPos to a start frame (called once when a stem is
// assigned a slot, so the worklet starts at the song start). Sets activeMask bit.
EM_JS(void, js_offmain_seed_stem,
      (int slot, int startFrame, int ringFrames, int generation, const char *stateKey),
{
    var key = UTF8ToString(stateKey);
    var audio = window[key];
    if (!audio || !audio.offmain) return;
    var om = audio.offmain;
    if (slot < 0 || slot >= om.stemHdr.length) return;
    var hdr = om.stemHdr[slot];
    var rp = ((startFrame % ringFrames) + ringFrames) % ringFrames;
    // writePos == readPos initially (empty); the first publish fills it.
    Atomics.store(hdr, 1, rp);
    Atomics.store(hdr, 0, rp);
    hdr[2] = ringFrames;
    hdr[3] = generation;
    Atomics.store(hdr, 4, 0); // reset monotonic readTotal (frames consumed)
    var ci = om.ctrlI;
    var mask = Atomics.load(ci, 0);
    Atomics.store(ci, 0, mask | (1 << slot));
    Atomics.store(ci, 4 + slot * 4 + 3, generation);
});

// Free a stem slot: clear its activeMask bit so the worklet stops mixing it.
EM_JS(void, js_offmain_free_stem, (int slot, const char *stateKey), {
    var key = UTF8ToString(stateKey);
    var audio = window[key];
    if (!audio || !audio.offmain) return;
    var ci = audio.offmain.ctrlI;
    var mask = Atomics.load(ci, 0);
    Atomics.store(ci, 0, mask & ~(1 << slot));
    // mark finished flag for the slot too (advisory).
    Atomics.store(ci, 4 + slot * 4 + 2, 2);
});

// Read back the worklet's MONOTONIC readTotal (frames consumed since seed) for a
// stem slot. The pump diffs it against its last-seen value to advance the
// producer back-pressure with NO wrap ambiguity (a wrapped readPos can't tell
// "1 behind" from "1 ahead"; a monotonic counter can). Returns -1 if unavailable.
EM_JS(int, js_offmain_read_total, (int slot, const char *stateKey), {
    var key = UTF8ToString(stateKey);
    var audio = window[key];
    if (!audio || !audio.offmain) return -1;
    var om = audio.offmain;
    if (slot < 0 || slot >= om.stemHdr.length) return -1;
    return Atomics.load(om.stemHdr[slot], 4);
});

// Publish the fixed output target depth (ctx frames) to the control SAB.
EM_JS(void, js_offmain_set_target, (int targetFrames, const char *stateKey), {
    var key = UTF8ToString(stateKey);
    var audio = window[key];
    if (!audio || !audio.offmain) return;
    Atomics.store(audio.offmain.ctrlI, 1, targetFrames);
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

    // Allocate local mix + resample-output buffers
    sMixBuffer = new float[MIX_BUF_FRAMES * 2];
    sOutBuffer = new float[MIX_BUF_FRAMES * 2];

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

    // Create SharedArrayBuffer + AudioContext + AudioWorklet. The browser may
    // ignore the requested rate (mSampleRate, the mogg/decode rate) and lock the
    // AudioContext to the hardware rate — js_audio_init returns the ACTUAL rate.
    int totalBytes = HEADER_BYTES + RING_SAMPLES * sizeof(float);
    int actualRate = js_audio_init(totalBytes, mSampleRate, RING_FRAMES, kStateKey, kWorkletName);
    mDeviceSampleRate = (actualRate > 0) ? actualRate : mSampleRate;
    // Reset the linear-resampler phase (mix-rate mSampleRate -> mDeviceSampleRate).
    mResamplePos = 0.0;
    mResampleCarryN = 0;

    // ---- Off-main mix (RB3_WEB_OFFMAIN_MIX) ----
    // Read the flag ONCE. When ON, allocate the per-stem + control SABs and post
    // 'init-offmain' to the worklet; music stems will publish to these SABs and
    // the worklet mixes on the audio thread.
    //
    // DEFAULT-ON (deepring): the off-main path is verified correct — the worklet
    // mixes the decoded stem rings on the AUDIO thread, the stem rings ride ~7-8 s
    // deep (so a single main-thread freeze drains a multi-second cushion instead of
    // dropping audio), audio_verify MATCHes (chroma ~0.98, 0% clip), the output
    // music floor stays a fixed ~70 ms, and spaced stalls drop 0%. Opt OUT with
    // RB3_WEB_OFFMAIN_MIX=0 (kept for the prior main-thread-mix shipping path).
    // The residual back-to-back (interval<=stall, ~100%-duty) dropout at 400/800 ms
    // is a documented path-B (off-main DECODE) limitation, not a regression — no
    // finite ring survives a sustained 100%-duty main-thread freeze train.
    {
        const char *omEnv = getenv("RB3_WEB_OFFMAIN_MIX");
        sOffMainMix = !(omEnv && omEnv[0] == '0');
        const char *floorEnv = getenv("RB3_WEB_OFFMAIN_FLOOR_MS");
        if (floorEnv && floorEnv[0]) {
            int v = atoi(floorEnv);
            if (v >= 20 && v <= 200) sOffMainFloorMs = v;
        }
    }
    if (sOffMainMix) {
        const int arate = (mDeviceSampleRate > 0) ? mDeviceSampleRate : mSampleRate;
        // Prime cushion ~120 ms (independent of the output floor; absorbs the
        // song-start burst). Worklet keys it on the min-across-stems availability.
        int primeFrames = (int)((long long)mSampleRate * 120 / 1000); // mix-rate frames
        // Fixed output target depth in ctx frames (the floor).
        sOffMainTargetFrames = (int)((long long)arate * sOffMainFloorMs / 1000);
        const char *dbgEnv = getenv("RB3_WEB_OFFMAIN_DBG");
        int dbg = (dbgEnv && dbgEnv[0] && dbgEnv[0] != '0') ? 1 : 0;
        int ok = js_offmain_alloc(kMaxStems, kStemHeaderBytes, kStemRingFrames,
                                  kCtrlHeaderInts, mSampleRate, arate,
                                  primeFrames, dbg, kStateKey);
        if (!ok) {
            sOffMainMix = false;
            printf("AudioDevice: OFF-MAIN alloc FAILED — falling back to main-thread mix\n");
        } else {
            mMusicStems.assign(kMaxStems, nullptr);
            mStemLastWrite.assign(kMaxStems, 0);
            mStemSeeded.assign(kMaxStems, false);
            mStemLastReadTotal.assign(kMaxStems, 0);
            js_offmain_set_target(sOffMainTargetFrames, kStateKey);
            printf("AudioDevice: OFF-MAIN mix ENABLED — %d stem SABs (%d frames each ~9s), "
                   "output floor %d ms (%d ctx frames), prime %d mix frames\n",
                   kMaxStems, kStemRingFrames, sOffMainFloorMs, sOffMainTargetFrames, primeFrames);
        }
    }

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
            console.log('Audio capture started...');
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
    if (mDeviceSampleRate != mSampleRate) {
        printf("AudioDevice: initialized (web) -- mix %d Hz -> ctx %d Hz (RESAMPLING %.4fx), ring %d frames\n",
               mSampleRate, mDeviceSampleRate, (double)mSampleRate / mDeviceSampleRate, RING_FRAMES);
    } else {
        printf("AudioDevice: initialized (web) -- %d Hz, ring %d frames\n", mSampleRate, RING_FRAMES);
    }
    return true;
}

void AudioDevice::Terminate() {
    if (!mInitialized)
        return;

    static const char *kStateKey = "_" MILO_WEB_AUDIO_NS_STR "Audio";
    js_audio_terminate(kStateKey);

    delete[] sMixBuffer;
    sMixBuffer = nullptr;
    delete[] sOutBuffer;
    sOutBuffer = nullptr;
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

bool AudioDevice::OffMainMixEnabled() { return sOffMainMix; }

void AudioDevice::RegisterMusicStem(WebMusicStem *stem) {
    if (!sOffMainMix) return;
    std::lock_guard<std::mutex> lock(mMusicStemMutex);
    // Already registered? (idempotent — PlayImpl may be called more than once)
    for (size_t i = 0; i < mMusicStems.size(); i++)
        if (mMusicStems[i] == stem) return;
    // Claim the first free slot.
    for (size_t i = 0; i < mMusicStems.size(); i++) {
        if (mMusicStems[i] == nullptr) {
            mMusicStems[i] = stem;
            mStemSeeded[i] = false;
            mStemLastWrite[i] = 0;
            mStemLastReadTotal[i] = 0;
            return;
        }
    }
    // No free slot (more than kMaxStems live stems) — fall back to NOT off-main
    // for this one. It simply won't be heard; log once. (16 covers real songs.)
    printf("AudioDevice: OFF-MAIN stem pool full (%d slots) — stem dropped\n", kMaxStems);
}

void AudioDevice::UnregisterMusicStem(WebMusicStem *stem) {
    if (!sOffMainMix) return;
    static const char *kStateKey = "_" MILO_WEB_AUDIO_NS_STR "Audio";
    std::lock_guard<std::mutex> lock(mMusicStemMutex);
    for (size_t i = 0; i < mMusicStems.size(); i++) {
        if (mMusicStems[i] == stem) {
            mMusicStems[i] = nullptr;
            mStemSeeded[i] = false;
            js_offmain_free_stem((int)i, kStateKey);
            return;
        }
    }
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
        const float kLimThreshold = 0.90f, kLimReleaseMs = 80.0f;
        const float aRel = expf(-1.0f / (mSampleRate * (kLimReleaseMs / 1000.0f)));
        float env = mLimiterEnv;
        for (int f = 0; f < frameCount; f++) {
            float l = output[f * 2 + 0];
            float r = output[f * 2 + 1];
            float la = l < 0.0f ? -l : l;
            float ra = r < 0.0f ? -r : r;
            float level = la > ra ? la : ra;
            float desired = (level > kLimThreshold) ? (kLimThreshold / level) : 1.0f;
            // INSTANT attack (see AudioDevice.cpp): gain drops immediately to hold the
            // post-gain sample at the threshold so it cannot rail; one-pole release.
            if (desired < env) env = desired;
            else env = aRel * env + (1.0f - aRel) * desired;
            output[f * 2 + 0] = SoftClip(l * env);
            output[f * 2 + 1] = SoftClip(r * env);
        }
        mLimiterEnv = env;
    }
}

static int sPumpCount = 0;

// ---- Off-main pump: decode/top-up only (RB3_WEB_OFFMAIN_MIX) ----------------
// Publishes each live music stem's freshly-decoded PCM into its per-stem SAB and
// mirrors the worklet's readPos back into the producer for back-pressure. NO mix,
// NO resample, NO output-ring write for music. SFX (a second pass, flag-OFF
// style) is handled by the caller after this returns true.
void AudioDevice::PumpAudioOffMainStems() {
    static const char *kStateKey = "_" MILO_WEB_AUDIO_NS_STR "Audio";
    std::lock_guard<std::mutex> lock(mMusicStemMutex);

    int activeCount = 0;
    for (size_t i = 0; i < mMusicStems.size(); i++) {
        WebMusicStem *stem = mMusicStems[i];
        if (!stem) continue;
        if (!stem->OffMainActive()) continue;
        if (!stem->OffMainArmed()) continue;   // not yet playing (kInit prime)
        activeCount++;

        // 1) Mirror the worklet's consumed readPos back into the producer BEFORE
        //    we re-read its snapshot, so SendDoneImpl/GetPlayCursor advance and
        //    the decode pipeline keeps refilling. Skip until the slot is seeded
        //    (the worklet's readPos is meaningless before we seed song start).
        if (mStemSeeded[i]) {
            int rt = js_offmain_read_total((int)i, kStateKey);
            if (rt >= 0) {
                int delta = rt - mStemLastReadTotal[i];
                // Guard a 32-bit wrap of the monotonic counter (~13h of audio).
                if (delta < 0) delta = 0;
                if (delta > 0) stem->OffMainAdvanceConsumed(delta);
                mStemLastReadTotal[i] = rt;
            }
        }

        // 2) Snapshot the producer ring state.
        OffMainStemState st;
        stem->OffMainSnapshot(&st);

        // The AVAILABILITY frontier (WRAPPED to [0,ringFrames)): the worklet may
        // read frames up to here. The SAB PCM must be valid over [readFrame,
        // availEnd). We copy the NEW slice [lastAvailEnd, availEnd) each tick
        // (forward mod ringFrames; the per-tick advance is << ringFrames).
        int rf = st.ringFrames;
        int availEnd = st.readFrame + st.availFrames;       // unwrapped
        availEnd %= rf; if (availEnd < 0) availEnd += rf;    // wrapped

        // 3) Seed the worklet readPos at the song start on the first publish.
        if (!mStemSeeded[i]) {
            int start = st.startFrame;
            if (start < 0) continue;            // not armed yet
            js_offmain_seed_stem((int)i, start, rf, /*generation*/1, kStateKey);
            mStemSeeded[i] = true;
            mStemLastWrite[i] = st.readFrame % rf; // copy from the play start
            mStemLastReadTotal[i] = 0;             // matches the worklet's reset
        }

        // 4) Publish the newly-playable PCM slice [lastAvailEnd, availEnd) and the
        //    AVAILABILITY writePos (= availEnd), plus params.
        int flags = (st.paused ? 1 : 0) | (st.finished ? 2 : 0);
        js_offmain_publish_stem((int)i,
                                (int)(intptr_t)st.ringPcm, // byte ptr into HEAP16
                                mStemLastWrite[i], availEnd, availEnd,
                                (double)st.gain, (double)st.pan,
                                flags, /*generation*/1, kStateKey);
        mStemLastWrite[i] = availEnd;
    }

    // 5) Publish the fixed output target depth (no adaptive law).
    (void)activeCount;
    js_offmain_set_target(sOffMainTargetFrames, kStateKey);
}

void AudioDevice::PumpAudio() {
    if (!mInitialized || !sWorkletReady || !sMixBuffer)
        return;

    static const char *kStateKey = "_" MILO_WEB_AUDIO_NS_STR "Audio";

    // Check if AudioWorklet is ready (async setup)
    if (!js_audio_worklet_started(kStateKey))
        return;

    // ---- Off-main mix (RB3_WEB_OFFMAIN_MIX) ----
    // Music stems publish to per-stem SABs (worklet mixes on the audio thread);
    // we then fall through to the SFX-only second pass below (mSources now holds
    // only SFX — music stems skip AddSource, see rb3_stream_receiver_native.cpp).
    if (sOffMainMix) {
        PumpAudioOffMainStems();
        // Continue into the normal pump for SFX ONLY: mSources contains just the
        // RB3SampleInstNative one-shots when off-main. They mix into the same
        // output ring the worklet additively combines with the music mix. If
        // there are no SFX sources, the pump just writes silence frames (cheap)
        // — the worklet adds 0 and plays music. We keep the existing adaptive
        // path for the SHALLOW SFX ring so one-shots fire promptly (the SFX
        // top-up clamp is preserved). The output SAB stays the SFX bus.
        // Fall through.
    }

    // Query free space in the SAB ring buffer
    int freeFrames = js_audio_ring_free_frames(kStateKey);
    if (freeFrames <= 0)
        return;

    // --- Adaptive output-latency cap (wave-09 probe D + pressure/decay rework) ---
    // The while(freeFrames>0) loop below would otherwise fill the ENTIRE ring every
    // pump (~RING_FRAMES-1 = 32767 frames ≈ 673 ms), so every newly-added source —
    // especially a one-shot SFX — waits behind that whole queue (keydown→audio
    // ~685 ms). We cap the QUEUED depth to a target latency and ADAPT that target to
    // the worklet's underrun feedback: keep it as LOW as possible for snappy SFX, but
    // GROW it when the worklet pads silence (a main-thread stall starved the ring),
    // then ease it back toward the floor while playback stays clean. RING_FRAMES (the
    // allocation) is unchanged.
    //
    // CONTROL LAW (per ~0.5s worklet report-window; host-simulated in latency_sim.py):
    //   * TRANSIENT REJECTION — a "pressure" accumulator (fixed-point /256) rises by
    //     1.0 on any window WITH new underruns and decays ×0.5 on a clean window. We
    //     only GROW once pressure reaches 2.0 (≈2 consecutive underrun windows), so a
    //     lone load-phase stutter (pressure 1.0, decays away) NEVER grows the target.
    //   * SUSTAINED GROWTH — while pressure stays ≥2.0, each underrun window adds
    //     kGrowMs (40 ms), so a real sustained burst climbs decisively to the ceiling.
    //   * RESPONSIVE SHRINK — each clean window moves kShrinkPct (25%) of the distance
    //     (target−floor) toward the floor, but at least kShrinkMinMs (10 ms). Large
    //     values fall fast (geometric); it eases gently near the floor (no oscillation).
    //   * HEADROOM CAP — the effective max is clamped to ≤80% of RING_FRAMES (always
    //     keep ≥20% ring slack), in addition to any env max; a throttled log fires once
    //     when the target crosses into the top band near that ceiling.
    // This self-tunes to each machine — no per-device knob needed. Env overrides:
    // RB3_AUDIO_LATENCY_MS pins a FIXED target (disables adaptation, for testing);
    // RB3_AUDIO_LAT_MIN_MS / _MAX_MS set the adaptive bounds.
    const int arate = (mDeviceSampleRate > 0) ? mDeviceSampleRate
                    : (mSampleRate > 0 ? mSampleRate : 48000);
    static const int kStartMs       = 200; // initial target — primed deep so the song-start burst
                                           // (~85% of all under-run frames; the worklet starts
                                           // draining before the ring is primed) can't starve the ring.
    static const int kGrowMs        = 60;  // added per underrun window once pressure is sustained
                                           // (was 40; grow decisively so one window of stalls is enough)
    static const int kShrinkPctNum  = 12;  // clean-window multiplicative shrink: only 12% of (target-floor)
    static const int kShrinkPctDen  = 100; // (was 25%) — shrink slowly so the buffer doesn't dive back to
                                           // the floor between sporadic stalls and get re-caught (the
                                           // grow<->shrink oscillation the diagnosis logged).
    static const int kShrinkMinMs   = 5;   // ...but at least this many ms (eases near the floor)
    // HYSTERESIS: only begin shrinking after this many CONSECUTIVE clean windows
    // (~0.5s each). A lone clean window after a stall no longer immediately drains
    // the headroom — we hold the deeper buffer for a few seconds first so a
    // closely-spaced second stall is still absorbed.
    static const int kShrinkHoldWindows = 8;
    static const int kPressureOne   = 256; // 1.0 in fixed-point
    static const int kPressureGrow  = 2 * kPressureOne; // grow only at >=2.0 (sustained pressure)
    static const int kPressureMax   = 4 * kPressureOne; // saturate so a long burst can't run away
    static int sFixedFrames  = -2;   // -2 uninit; -1 adaptive; >=0 env-fixed
    static int sMinFrames    = 0;
    static int sMaxFrames    = 0;    // EFFECTIVE max (already <=80% ring)
    static int sHighBand     = 0;    // top-band threshold for the throttled "near ceiling" flag
    static int sGrowFrames   = 0;
    static int sShrinkMinF   = 0;
    static int sTargetFrames = 0;    // current adaptive target (device frames ahead of read)
    static int sPressure     = 0;    // fixed-point /256 underrun pressure accumulator
    static int sLastUnderrun = -1;   // -1 until baselined past the boot backlog
    static int sLastQuanta   = -1;   // worklet totalQuanta at last law step (edge-trigger guard)
    static bool sHighFlagged = false;// has the "near ceiling" log already fired in this excursion?
    static int sCleanRun     = 0;    // consecutive clean windows (shrink-hysteresis counter)
    if (sFixedFrames == -2) {
        const char *fenv = getenv("RB3_AUDIO_LATENCY_MS");
        int fixedMs = fenv ? atoi(fenv) : 0;
        sFixedFrames = fenv ? (int)((long long)arate * (fixedMs < 5 ? 5 : fixedMs) / 1000) : -1;
        // Floor 50 -> 140 ms: the measured main-thread stalls are p99=83 ms, so the
        // floor must clear the p99 (50 ms sat below it and under-ran on every stall)
        // while the deeper adaptive target on top absorbs the rarer max~200 ms. 140 ms
        // covers p99 with margin at roughly HALF the SFX latency of a blanket 180 ms —
        // this is a rhythm game, so a flat 180 ms floor is too costly for one-shot
        // SFX (menu/hit) that queue behind the buffered music. The real fix is to feed
        // the buffer off the main thread (so stalls don't starve it at all) — see
        // docs/native/audio-thread-2026-06-20/; this floor is the interim default.
        // Override with RB3_AUDIO_LAT_MIN_MS.
        int minMs = getenv("RB3_AUDIO_LAT_MIN_MS") ? atoi(getenv("RB3_AUDIO_LAT_MIN_MS")) : 140;
        int maxMs = getenv("RB3_AUDIO_LAT_MAX_MS") ? atoi(getenv("RB3_AUDIO_LAT_MAX_MS")) : 500;
        if (minMs < 5) minMs = 5;
        sMinFrames    = (int)((long long)arate * minMs / 1000);
        sMaxFrames    = (int)((long long)arate * maxMs / 1000);
        // HEADROOM CAP: never target more than 80% of the ring -> always keep >=20%
        // slack so the worklet has room to drain even at the highest adaptive depth.
        const int ring80 = (int)((long long)RING_FRAMES * 80 / 100);
        if (sMaxFrames > ring80)          sMaxFrames = ring80;
        if (sMaxFrames > RING_FRAMES - 1) sMaxFrames = RING_FRAMES - 1;
        sHighBand     = (int)((long long)sMaxFrames * 90 / 100); // top 10% of the effective range
        sGrowFrames   = (int)((long long)arate * kGrowMs / 1000);
        sShrinkMinF   = (int)((long long)arate * kShrinkMinMs / 1000);
        sTargetFrames = (int)((long long)arate * kStartMs / 1000);
        if (sTargetFrames < sMinFrames) sTargetFrames = sMinFrames;
        if (sTargetFrames > sMaxFrames) sTargetFrames = sMaxFrames;
    }

    int targetFrames;
    if (sFixedFrames >= 0) {
        targetFrames = sFixedFrames;                 // env-pinned: no adaptation
    } else {
        int u[4];
        // EDGE-TRIGGER: the pressure law is a PER-WINDOW law (one grow/decay step
        // per ~0.5s worklet report), but PumpAudio runs every frame (~33ms, called
        // from rb3 App.cpp). The worklet's underrun summary is replaced wholesale on
        // each 'underrun-stats' postMessage; u[2]=totalQuanta is monotonically
        // increasing and only takes a NEW value when a fresh window has been posted.
        // Gating the law on `u[2] != sLastQuanta` makes each constant (kPressureOne,
        // kPressureGrow, the >>=1 decay, the 25% shrink) mean one window — without
        // this guard a single stale window re-fired the branch ~15-30x per window
        // (~33ms apart, all citing the same stale minDepth), so 4 pumps reached
        // kPressureGrow and the target walked the whole 50->500ms floor<->ceiling
        // range in <0.4s (latency thrash). See handoff 01-audio lines 99-100.
        if (js_audio_underrun_stats(u, kStateKey) && u[2] != sLastQuanta) {
            sLastQuanta = u[2];
            // SOFT-PRESSURE input: the worklet's per-window ring low-water mark. A
            // dip to well below the target (but still >=128 frames, so the hard
            // underrun counter stays 0) is a near-miss the old law was blind to.
            // We only let it build pressure once the boot backlog has been
            // baselined (sLastUnderrun >= 0, set below), so the init literal's
            // minRingDepthFrames:0 can't false-grow at startup, and we skip it when
            // no audio is playing (sources empty -> the ring legitimately drains on
            // pause/seek, not a stall).
            int minDepth = 0;
            js_audio_min_ring_depth(&minDepth, kStateKey);
            bool sourcesActive;
            {
                std::lock_guard<std::mutex> lock(mSourceMutex);
                sourcesActive = !mSources.empty();
            }
            // Half the target: the "comfortably ahead" line. Dipping under it means
            // the pump was late enough this window that one more stall would underrun.
            // NOTE: a FULL-DRAIN window leaves minDepth==0 (no quantum saw any audio),
            // which fails (minDepth > 0) and skips soft pressure — but such a window
            // necessarily bumped the hard underrun counter (u[0]), so the hard branch
            // below covers it. softNearMiss is for the >=128-frame near-misses only.
            const bool softNearMiss = (sLastUnderrun >= 0) && sourcesActive &&
                                      (minDepth > 0) && (minDepth < sTargetFrames / 2);

            if (sLastUnderrun < 0) {
                sLastUnderrun = u[0];                 // baseline past the boot backlog (don't grow for it)
            } else if (u[0] > sLastUnderrun) {
                // New underruns this window: build pressure. Only GROW once pressure is
                // SUSTAINED (>=2 consecutive underrun windows) — a lone spike is ignored.
                sCleanRun = 0;                        // dirty window: reset shrink hysteresis
                sLastUnderrun = u[0];
                sPressure += kPressureOne;
                if (sPressure > kPressureMax) sPressure = kPressureMax;
                if (sPressure >= kPressureGrow && sTargetFrames < sMaxFrames) {
                    sTargetFrames += sGrowFrames;
                    if (sTargetFrames > sMaxFrames) sTargetFrames = sMaxFrames;
                    printf("AudioDevice: latency GROW -> %d ms (sustained underrun, pressure %d/256)\n",
                           (int)((long long)sTargetFrames * 1000 / arate), sPressure);
                }
            } else if (softNearMiss) {
                // Near-miss this window: build pressure at HALF rate (a single transient
                // dip adds 128 < kPressureGrow=512 and decays away on the next clean
                // window; ~4 sustained near-miss windows -> >=512 -> GROW within ~2s).
                // Do NOT decay/shrink on a near-miss window — this is a "dirty" window.
                sCleanRun = 0;                        // dirty window: reset shrink hysteresis
                sPressure += kPressureOne / 2;
                if (sPressure > kPressureMax) sPressure = kPressureMax;
                if (sPressure >= kPressureGrow && sTargetFrames < sMaxFrames) {
                    sTargetFrames += sGrowFrames;
                    if (sTargetFrames > sMaxFrames) sTargetFrames = sMaxFrames;
                    printf("AudioDevice: latency GROW -> %d ms (sustained near-miss, minDepth %d frames, "
                           "pressure %d/256)\n",
                           (int)((long long)sTargetFrames * 1000 / arate), minDepth, sPressure);
                }
            } else {
                // Clean window: decay pressure. Only shrink toward the floor AFTER
                // kShrinkHoldWindows consecutive clean windows (hysteresis), and then
                // only slowly (12% of the distance) — so the buffer holds its depth
                // for a few seconds after a stall instead of diving back to the floor
                // and getting re-caught by the next sporadic stall. This kills the
                // grow<->shrink oscillation the diagnosis logged.
                sPressure >>= 1;
                if (sCleanRun < kShrinkHoldWindows) {
                    sCleanRun++;
                } else if (sTargetFrames > sMinFrames) {
                    int step = (int)((long long)(sTargetFrames - sMinFrames) * kShrinkPctNum / kShrinkPctDen);
                    if (step < sShrinkMinF) step = sShrinkMinF;
                    sTargetFrames -= step;
                    if (sTargetFrames < sMinFrames) sTargetFrames = sMinFrames;
                }
            }
        }
        // THROTTLED "near ceiling" flag: fire once when we cross into the top band,
        // re-arm when we drop back below it (so it doesn't spam every pump).
        if (sTargetFrames >= sHighBand) {
            if (!sHighFlagged) {
                sHighFlagged = true;
                printf("AudioDevice: latency HIGH ~%d ms (near ceiling) — holding ring headroom\n",
                       (int)((long long)sTargetFrames * 1000 / arate));
            }
        } else {
            sHighFlagged = false;
        }
        targetFrames = sTargetFrames;
    }
    if (targetFrames > RING_FRAMES - 1) targetFrames = RING_FRAMES - 1;

    int queued = (RING_FRAMES - 1) - freeFrames;     // frames already queued ahead of read cursor
    int writable = targetFrames - queued;
    if (writable <= 0)
        return;                                       // already at/above target this pump
    if (freeFrames > writable)
        freeFrames = writable;                        // top up only to the target depth

    sPumpCount++;

    const bool resample = (mDeviceSampleRate != mSampleRate && mDeviceSampleRate > 0 && mSampleRate > 0);
    // freeFrames is counted in DEVICE-rate (output / ctx) frames. When resampling,
    // each output frame consumes (mSampleRate/mDeviceSampleRate) mix-rate frames.
    const double step = resample ? ((double)mSampleRate / (double)mDeviceSampleRate) : 1.0;

    // Mix and push in chunks (all chunk sizes below are DEVICE-rate frames).
    while (freeFrames > 0) {
        int outChunk = std::min(freeFrames, MIX_BUF_FRAMES);

        if (!resample) {
            // Fast path: ctx rate == mix rate, push mix straight through.
            MixSources(sMixBuffer, outChunk);
            FillDebugTone(sMixBuffer, outChunk, mSampleRate);

            if (sCapturing && sCaptureBuffer && sCapturePos < CAPTURE_FRAMES) {
                int framesToCapture = std::min(outChunk, CAPTURE_FRAMES - sCapturePos);
                memcpy(sCaptureBuffer + sCapturePos * 2, sMixBuffer, framesToCapture * 2 * sizeof(float));
                sCapturePos += framesToCapture;
                if (sCapturePos >= CAPTURE_FRAMES) {
                    sCapturing = false;
                    sCaptureReady = true;
                    printf("AudioDevice: capture complete (%d frames). Call %sDownloadAudio() to save.\n",
                           sCapturePos, MILO_WEB_AUDIO_NS_STR);
                }
            }
            js_audio_ring_write(sMixBuffer, outChunk, kStateKey);
            freeFrames -= outChunk;
            continue;
        }

        // Resampling path (mix rate -> device/ctx rate), continuous linear
        // interpolation. MixSources() is a DESTRUCTIVE pull: it advances the source
        // stream by exactly the frames it mixes. So we must carry EVERY mix frame that
        // was pulled but not yet fully consumed into the next chunk, stream-contiguous
        // -- carrying only one sample (the old code) silently dropped the unconsumed
        // tail frame on the ~8% of jittered chunks where the read head didn't land on
        // a frame boundary, producing a 1-sample click/crackle on music (inaudible on
        // a constant-cadence sine, which is all earlier tests exercised).
        //
        // Layout: sMixBuffer[0 .. carryN-1] = frames carried from the previous chunk
        // (stream-contiguous, already filled); MixSources fills the fresh frames after
        // them. mResamplePos in [0,1) is the read offset from index 0.
        int carryN = mResampleCarryN;
        for (int k = 0; k < carryN; k++) {
            sMixBuffer[k * 2 + 0] = mResampleCarry[k * 2 + 0];
            sMixBuffer[k * 2 + 1] = mResampleCarry[k * 2 + 1];
        }
        double sStart = mResamplePos;
        double sEnd = sStart + (double)(outChunk - 1) * step;
        // Frames needed = indices 0 .. floor(sEnd)+1 (the last output reads i0 and
        // i0+1), i.e. needTotal = floor(sEnd)+2 filled frames total.
        int needTotal = (int)sEnd + 2;
        if (needTotal > MIX_BUF_FRAMES) {
            // Cap: shrink the output chunk so the needed frames (incl. the carry) fit
            // the mix buffer. Recompute sEnd for the reduced outChunk.
            needTotal = MIX_BUF_FRAMES;
            outChunk = (int)(((double)(needTotal - 2) - sStart) / step) + 1;
            if (outChunk <= 0) break;
            sEnd = sStart + (double)(outChunk - 1) * step;
        }
        int newMix = needTotal - carryN;       // additional fresh frames to mix
        if (newMix < 1) { newMix = 1; needTotal = carryN + 1; }
        MixSources(sMixBuffer + carryN * 2, newMix); // fill [carryN .. needTotal-1]
        if (sDebugToneHz > 0.0) {
            // Overwrite only the fresh block; the carried frames keep the tone phase
            // continuous across the seam.
            FillDebugTone(sMixBuffer + carryN * 2, newMix, mSampleRate);
        }

        double s = sStart;
        for (int o = 0; o < outChunk; o++) {
            int i0 = (int)s;
            double t = s - i0;
            float l0 = sMixBuffer[i0 * 2 + 0];
            float r0 = sMixBuffer[i0 * 2 + 1];
            float l1 = sMixBuffer[(i0 + 1) * 2 + 0];
            float r1 = sMixBuffer[(i0 + 1) * 2 + 1];
            sOutBuffer[o * 2 + 0] = (float)(l0 + (l1 - l0) * t);
            sOutBuffer[o * 2 + 1] = (float)(r0 + (r1 - r0) * t);
            s += step;
        }
        // Carry ALL unconsumed frames [consumed .. needTotal-1] into the next chunk,
        // stream-contiguous. consumed = floor(s) integer frames are behind the read
        // head; the rest the next chunk still needs. leftover is 1 or 2 in practice.
        int consumed = (int)s;
        if (consumed > needTotal - 1) consumed = needTotal - 1;  // numerical guard (>=1 carry)
        if (consumed < 0) consumed = 0;
        int leftover = needTotal - consumed;
        if (leftover > kResampleCarryMax) leftover = kResampleCarryMax;
        for (int k = 0; k < leftover; k++) {
            mResampleCarry[k * 2 + 0] = sMixBuffer[(consumed + k) * 2 + 0];
            mResampleCarry[k * 2 + 1] = sMixBuffer[(consumed + k) * 2 + 1];
        }
        mResampleCarryN = leftover;
        mResamplePos = s - consumed;        // in [0,1)

        if (sCapturing && sCaptureBuffer && sCapturePos < CAPTURE_FRAMES) {
            int framesToCapture = std::min(outChunk, CAPTURE_FRAMES - sCapturePos);
            memcpy(sCaptureBuffer + sCapturePos * 2, sOutBuffer, framesToCapture * 2 * sizeof(float));
            sCapturePos += framesToCapture;
            if (sCapturePos >= CAPTURE_FRAMES) {
                sCapturing = false;
                sCaptureReady = true;
                printf("AudioDevice: capture complete (%d frames). Call %sDownloadAudio() to save.\n",
                       sCapturePos, MILO_WEB_AUDIO_NS_STR);
            }
        }
        js_audio_ring_write(sOutBuffer, outChunk, kStateKey);
        freeFrames -= outChunk;
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
    // The capture records the post-resample SAB-bound output, which is at the
    // DEVICE/ctx rate (mDeviceSampleRate). When the ctx rate differs from the mix
    // rate (the resampling path), tag the WAV with the device rate so the file
    // plays/measures at the correct pitch; otherwise it's the mix rate (==44100).
    auto &dev = AudioDevice::GetInstance();
    int capRate = dev.GetSampleRate(); // mix/engine rate (44100)
    int devRate = dev.GetDeviceSampleRate();
    if (devRate > 0) capRate = devRate;
    js_download_wav(sCaptureBuffer, sCapturePos, capRate, kWavName);
}

// Debug: set a pure-tone override (Hz) generated at the engine/mix rate. 0 = off.
// Used to deterministically verify the mix-rate -> ctx-rate resampler keeps pitch.
EMSCRIPTEN_KEEPALIVE void MILO_WEB_AUDIO_FN(debug_tone)(int hz) {
    sDebugToneHz = (double)hz;
    sDebugTonePhase = 0.0;
    printf("AudioDevice: debug tone %s (%d Hz at mix rate)\n", hz > 0 ? "ON" : "OFF", hz);
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
    {
        static const char *kStateKey = "_" MILO_WEB_AUDIO_NS_STR "Audio";
        int u[4] = {0, 0, 0, 0};
        if (js_audio_underrun_stats(u, kStateKey)) {
            printf("  worklet: underrunEvents=%d paddedFrames=%d totalQuanta=%d totalFrames=%d\n",
                   u[0], u[1], u[2], u[3]);
        } else {
            printf("  worklet: underrun stats not available yet\n");
        }
    }
#ifdef HX_WEB
    dev.DebugDumpSources();
#endif
}

} // extern "C"

#endif // __EMSCRIPTEN__
