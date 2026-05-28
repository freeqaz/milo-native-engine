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

        this.port.onmessage = (e) => {
            if (e.data.type === 'init') {
                this.sab = e.data.sab;
                this.bufFrames = e.data.bufFrames;
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

        return true;
    }
}

registerProcessor('milo-audio-processor', MiloAudioProcessor);
