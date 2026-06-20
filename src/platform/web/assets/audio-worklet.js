// Milo AudioWorklet Processor
// Reads mixed stereo PCM from a SharedArrayBuffer ring buffer
// and outputs to the browser's audio output.
//
// Ring buffer layout (SharedArrayBuffer):
//   Int32[0] = writePos (frames, updated by WASM main thread)
//   Int32[1] = readPos  (frames, updated by this worklet)
//   Float32[2..] = interleaved stereo PCM data (L,R,L,R,...)
//
// The registered processor name is a fixed engine-wide string
// ('milo-audio-processor'); the per-consumer namespace prefix
// (MILO_WEB_AUDIO_NS) only governs window globals + C-exported
// symbol names, not this processor identity.

class MiloAudioProcessor extends AudioWorkletProcessor {
    constructor() {
        super();
        this.sab = null;
        this.cursors = null;
        this.data = null;
        this.bufFrames = 0;

        // Underrun instrumentation (additive, backward-compatible). Counts
        // render quanta that could NOT be fully served from the ring (the
        // for-loop below pads them with silence == the audible "static"), the
        // total silence-padded frames, and the total quanta processed. We
        // postMessage a running summary to the main thread roughly every ~0.5s
        // of audio so a headless harness can poll window[...].underruns and
        // compute underruns-per-second during sustained gameplay. ~0 == the
        // ring is keeping up (dropout fixed); many == still starving.
        this.underrunEvents = 0;   // quanta with a silence pad
        this.underrunFrames = 0;   // total silence-padded frames
        this.totalQuanta = 0;      // total process() calls served
        this.totalFrames = 0;      // total frames requested
        // Per-window low-water mark: the SMALLEST ring depth (`available` frames
        // ahead of the read cursor) observed across the quanta in the current
        // ~0.5s report window. A dip from a deep buffer to a near-miss (ring < the
        // adaptive target but still >= 128 frames) registers ZERO underrunEvents,
        // so this is the early-warning signal the pump's adaptive law gates on.
        // Reset to bufFrames each window after posting (so it can only fall).
        this.minRingDepthThisWindow = this.bufFrames;
        this._reportAccum = 0;

        // ---- Graceful under-run concealment (defense-in-depth) ----
        // When the ring runs dry we used to hard-step the output to 0 (and back
        // to a live sample on recovery) — that discontinuity is the audible
        // "click". Instead we HOLD the last delivered sample and linearly RAMP it
        // to zero over ~kFadeFrames frames, then on recovery RAMP back up from 0
        // to the live signal. The dip is the same length but it's now a smooth
        // de-zipper instead of a step, so a brief stall becomes an inaudible
        // smear rather than a pop. This conceals the symptom; the pump's deeper
        // buffer + adaptive law (AudioDevice_Web.cpp) attacks the cause.
        this.lastL = 0;            // last sample actually delivered (for hold/ramp)
        this.lastR = 0;
        this.fadeGain = 1.0;       // 1.0 = playing live, 0.0 = fully faded out
        // ~3 ms at the worklet rate (sampleRate is the global ctx rate). A few ms
        // is short enough to be perceptually invisible yet long enough to avoid a
        // hard step. Recomputed in 'init' once the real rate is known.
        this.fadeFrames = Math.max(64, Math.round(sampleRate * 0.003));
        this.fadeStep = 1.0 / this.fadeFrames;

        // ---- Start-up prime gate (song-start burst was ~85% of all under-runs) ----
        // The worklet connects and begins draining the moment its addModule promise
        // resolves — which can be BEFORE the main-thread pump has filled the ring,
        // and the two biggest main-thread stalls land in the first second. So until
        // the ring has first reached `primeFrames` of buffered audio we OUTPUT
        // SILENCE (no read-cursor advance) and let the producer build the cushion.
        // Once primed we never re-arm, so this is purely a one-time start-up cushion
        // and steady-state latency is unaffected.
        this.primed = false;
        this.primeFrames = Math.round(sampleRate * 0.12); // ~120 ms cushion before first audio

        this.port.onmessage = (e) => {
            if (e.data.type === 'init') {
                this.sab = e.data.sab;
                this.bufFrames = e.data.bufFrames;
                // Seed the low-water mark now that bufFrames is known (the ctor
                // ran before 'init', so it was 0 there).
                this.minRingDepthThisWindow = this.bufFrames;
                this.cursors = new Int32Array(this.sab, 0, 2);
                // PCM data starts after 8-byte header (2 x Int32)
                this.data = new Float32Array(this.sab, 8);
                // Clamp the prime cushion to the ring so it can always be reached.
                if (this.primeFrames > this.bufFrames - 1)
                    this.primeFrames = (this.bufFrames - 1) | 0;
            }
        };
    }

    process(inputs, outputs, parameters) {
        if (!this.data) return true;

        const output = outputs[0];
        if (!output || output.length < 2) return true;

        const left = output[0];
        const right = output[1];
        const frames = left.length; // 128 frames per render quantum

        const readPos = Atomics.load(this.cursors, 1);
        const writePos = Atomics.load(this.cursors, 0);
        const bufFrames = this.bufFrames;

        let available = writePos - readPos;
        if (available < 0) available += bufFrames;

        // Start-up prime gate: hold silence until the ring first reaches the prime
        // cushion. NOT counted as an under-run (this is the intended boot cushion,
        // not a starvation glitch) and the read cursor does not advance, so no audio
        // is dropped — we simply wait for the producer to fill it. fadeGain stays at
        // its initial 1.0 so the first real samples start cleanly (data ramps up via
        // the recovery path only after a real dip, not here).
        if (!this.primed) {
            if (available < this.primeFrames) {
                for (let i = 0; i < frames; i++) { left[i] = 0; right[i] = 0; }
                this.totalQuanta++;
                this.totalFrames += frames;
                this._reportAccum += frames;
                if (this._reportAccum >= (sampleRate >> 1)) {
                    this._reportAccum = 0;
                    this.port.postMessage({
                        type: 'underrun-stats',
                        underrunEvents: this.underrunEvents,
                        underrunFrames: this.underrunFrames,
                        totalQuanta: this.totalQuanta,
                        totalFrames: this.totalFrames,
                        minRingDepthFrames: this.minRingDepthThisWindow
                    });
                    this.minRingDepthThisWindow = this.bufFrames;
                }
                return true;
            }
            this.primed = true;
        }

        // Per-window ring low-water mark (the dip the underrun counter misses).
        if (available < this.minRingDepthThisWindow) this.minRingDepthThisWindow = available;

        const toRead = Math.min(frames, available);

        const fadeStep = this.fadeStep;
        let g = this.fadeGain;
        let lastL = this.lastL;
        let lastR = this.lastR;

        let rp = readPos;
        for (let i = 0; i < toRead; i++) {
            const idx = (rp % bufFrames) * 2;
            const sL = this.data[idx];
            const sR = this.data[idx + 1];
            // Ramp the fade-gain back UP to 1.0 on recovery (de-zipper the
            // re-entry so the return from a concealed dip is also click-free).
            if (g < 1.0) { g += fadeStep; if (g > 1.0) g = 1.0; }
            left[i] = sL * g;
            right[i] = sR * g;
            lastL = sL;
            lastR = sR;
            rp++;
        }

        // Under-run concealment: instead of a hard step to 0, HOLD the last
        // delivered sample and RAMP the gain down to 0 over ~fadeFrames. Once
        // faded, this emits true silence (lastL*0). When data returns, the read
        // loop above ramps the gain back up. A short stall is a smooth smear, not
        // a pop. (g persists across quanta via this.fadeGain.)
        for (let i = toRead; i < frames; i++) {
            if (g > 0.0) { g -= fadeStep; if (g < 0.0) g = 0.0; }
            left[i] = lastL * g;
            right[i] = lastR * g;
        }

        this.fadeGain = g;
        this.lastL = lastL;
        this.lastR = lastR;

        Atomics.store(this.cursors, 1, rp % bufFrames);

        // ---- underrun instrumentation (additive) ----
        this.totalQuanta++;
        this.totalFrames += frames;
        const padded = frames - toRead;
        if (padded > 0) {
            this.underrunEvents++;
            this.underrunFrames += padded;
        }
        // Report ~ every 0.5s of audio (sampleRate is the worklet-global ctx
        // rate). DC3 and any other consumer simply ignore these extra messages.
        this._reportAccum += frames;
        if (this._reportAccum >= (sampleRate >> 1)) {
            this._reportAccum = 0;
            this.port.postMessage({
                type: 'underrun-stats',
                underrunEvents: this.underrunEvents,
                underrunFrames: this.underrunFrames,
                totalQuanta: this.totalQuanta,
                totalFrames: this.totalFrames,
                // Additive: smallest ring depth seen this window (frames). Ignored
                // by consumers that read named fields; the pump's adaptive law uses
                // it to grow on near-misses before they become audible underruns.
                minRingDepthFrames: this.minRingDepthThisWindow
            });
            // Re-arm the low-water mark for the next window (alongside _reportAccum).
            this.minRingDepthThisWindow = this.bufFrames;
        }

        return true;
    }
}

registerProcessor('milo-audio-processor', MiloAudioProcessor);
