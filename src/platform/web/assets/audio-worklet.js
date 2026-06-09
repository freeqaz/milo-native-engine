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

        // Per-window ring low-water mark (the dip the underrun counter misses).
        if (available < this.minRingDepthThisWindow) this.minRingDepthThisWindow = available;

        const toRead = Math.min(frames, available);

        let rp = readPos;
        for (let i = 0; i < toRead; i++) {
            const idx = (rp % bufFrames) * 2;
            left[i] = this.data[idx];
            right[i] = this.data[idx + 1];
            rp++;
        }

        // Pad with silence on underrun
        for (let i = toRead; i < frames; i++) {
            left[i] = 0;
            right[i] = 0;
        }

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
