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

        // ---- Off-main mix mode (RB3_WEB_OFFMAIN_MIX, set by 'init-offmain') ----
        // When ON the worklet MIXES N per-stem int16 rings on the audio thread
        // (int16->float + vol/pan + additive sum + limiter + 44100->ctx resampler
        // with carry), plus the SFX output ring additively. State below is the
        // audio-thread-owned port of MixSources/limiter/resampler from
        // AudioDevice_Web.cpp. See docs/native/audio-thread-2026-06-20/.
        this.offmain = false;
        this.stemHdr = null;   // [Int32Array(sab,0,4)] per stem (writePos,readPos,ringFrames,gen)
        this.stemPcm = null;   // [Int16Array(sab,header)] per stem (mono ring)
        this.ctrlI = null;     // Int32Array control (activeMask,target,mixRate,ctxRate, per-slot...)
        this.ctrlF = null;     // Float32Array control (gain,pan per slot)
        this.maxStems = 0;
        this.stemRingFrames = 0;
        this.mixRate = 44100;
        this.ctxRate = sampleRate;
        this.step = 1.0;       // mixRate/ctxRate (mix frames per ctx out frame)
        // resampler carry state — audio-thread-owned (was mResamplePos/mResampleCarry*)
        this.resamplePos = 0.0;
        this.kResampleCarryMax = 8;
        // mix-rate stereo bus scratch: carry frames + this-quantum frames.
        // 128-frame quantum * worst step (<=2) + carry + slop. 512 frames is ample.
        this.busBuf = new Float32Array(512 * 2);
        this.resampleCarry = new Float32Array(this.kResampleCarryMax * 2);
        this.resampleCarryN = 0;
        // limiter env — audio-thread-owned (was mLimiterEnv). 1.0 = no reduction.
        this.limiterEnv = 1.0;
        // per-stem read cursors are kept in the SAB header (slot index 1) so the
        // pump can mirror them back; we cache the read base locally too.
        this.stemPrimedFrames = 0; // prime cushion (mix-rate frames), set in init
        // SFX output ring (the existing pre-mixed ring) — additively combined.
        this.sfxData = null;       // Float32Array(sfxSab, 8) stereo interleaved
        this.sfxCursors = null;    // Int32Array(sfxSab,0,2)
        this.sfxBufFrames = 0;

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
                // The existing output ring doubles as the SFX bus in off-main mode.
                this.sfxData = this.data;
                this.sfxCursors = this.cursors;
                this.sfxBufFrames = this.bufFrames;
            } else if (e.data.type === 'init-offmain') {
                this.offmain = true;
                this.maxStems = e.data.maxStems | 0;
                this.stemRingFrames = e.data.ringFrames | 0;
                this.mixRate = e.data.mixRate | 0;
                this.ctxRate = e.data.ctxRate | 0;
                this.step = this.mixRate / this.ctxRate;
                const hb = e.data.stemHeaderBytes | 0;
                // header is hb/4 int32 words (writePos,readPos,ringFrames,gen,readTotal,...)
                this.stemHdr = e.data.stemSabs.map((sab) => new Int32Array(sab, 0, hb >> 2));
                this.stemPcm = e.data.stemSabs.map((sab) => new Int16Array(sab, hb));
                this.ctrlI = new Int32Array(e.data.ctrlSab);
                this.ctrlF = new Float32Array(e.data.ctrlSab);
                // prime cushion: e.data.primeFrames is in MIX-rate frames.
                this.stemPrimedFrames = e.data.primeFrames | 0;
                if (this.stemPrimedFrames > this.stemRingFrames - 1)
                    this.stemPrimedFrames = (this.stemRingFrames - 1) | 0;
                // re-arm prime gate for the stem-ring model.
                this.primed = false;
                this.minRingDepthThisWindow = this.stemRingFrames;
            }
        };
    }

    // ---- ported DSP helpers (match AudioDevice_Web.cpp exactly) ----
    _softClip(x) {
        const kSoftKnee = 0.95;
        const a = x < 0 ? -x : x;
        if (a <= kSoftKnee) return x;
        const shaped = kSoftKnee + (1.0 - kSoftKnee) *
            Math.tanh((a - kSoftKnee) / (1.0 - kSoftKnee));
        return x < 0 ? -shaped : shaped;
    }

    _reportOffMain(minAvailCtxFrames) {
        if (minAvailCtxFrames < this.minRingDepthThisWindow)
            this.minRingDepthThisWindow = minAvailCtxFrames;
        this._reportAccum += 128;
        if (this._reportAccum >= (sampleRate >> 1)) {
            this._reportAccum = 0;
            this.port.postMessage({
                type: 'underrun-stats',
                underrunEvents: this.underrunEvents,
                underrunFrames: this.underrunFrames,
                totalQuanta: this.totalQuanta,
                totalFrames: this.totalFrames,
                minRingDepthFrames: this.minRingDepthThisWindow,
            });
            this.minRingDepthThisWindow = this.stemRingFrames;
        }
    }

    // Off-main mix: read N per-stem int16 rings (mix rate) + the SFX output ring,
    // mix on the AUDIO THREAD, resample mix->ctx with carry, limit, output. This
    // is the audio-thread port of AudioDevice_Web.cpp MixSources+limiter+resampler.
    _processOffMain(outputs) {
        const output = outputs[0];
        if (!output || output.length < 2 || !this.ctrlI) return true;
        const left = output[0], right = output[1];
        const frames = left.length;                // 128 ctx-rate frames
        const ringFrames = this.stemRingFrames;
        const mask = Atomics.load(this.ctrlI, 0);
        const step = this.step;
        const ctrlF = this.ctrlF, ctrlI = this.ctrlI;
        const stemHdr = this.stemHdr, stemPcm = this.stemPcm;

        // Build the list of active slots + their availability (mix-rate frames).
        // available_s = (writePos_s - readPos_s) mod ringFrames. The hungriest
        // active stem gates the prime gate + under-run; a momentarily-short stem
        // contributes silence for its missing tail WITHOUT zeroing the others.
        let anyActive = false;
        let minAvail = 0x7fffffff;                 // mix-rate frames (min across stems)
        const active = [];                         // {slot, rp, avail, rf, gain, pan}
        for (let s = 0; s < this.maxStems; s++) {
            if ((mask & (1 << s)) === 0) continue;
            const hdr = stemHdr[s];
            const wp = Atomics.load(hdr, 0);
            const rp = Atomics.load(hdr, 1);
            // PER-STEM ring length: stems can have different mNumBuffers -> different
            // ring sizes. Use the SAB header's ringFrames (index 2), NOT the global.
            const rfS = Atomics.load(hdr, 2) || this.stemRingFrames;
            let avail = wp - rp; if (avail < 0) avail += rfS;
            const flags = Atomics.load(ctrlI, 4 + s * 4 + 2);
            const paused = (flags & 1) !== 0;
            anyActive = true;
            if (avail < minAvail) minAvail = avail;
            active.push({
                slot: s, rp: rp, avail: avail, rf: rfS, paused: paused,
                gain: ctrlF[4 + s * 4 + 0], pan: ctrlF[4 + s * 4 + 1],
            });
        }
        if (!anyActive) minAvail = 0;

        // --- SFX output ring (additive; benign if empty: just no SFX) ---
        let sfxRp = 0, sfxAvail = 0;
        const sfxData = this.sfxData, sfxBuf = this.sfxBufFrames;
        if (sfxData && this.sfxCursors) {
            const swp = Atomics.load(this.sfxCursors, 0);
            sfxRp = Atomics.load(this.sfxCursors, 1);
            sfxAvail = swp - sfxRp; if (sfxAvail < 0) sfxAvail += sfxBuf;
        }

        if (!anyActive) {
            // No music stems. Still drain the SFX ring so menu SFX play. The SFX
            // ring is ALREADY at ctx rate (pump resampled it), so 1:1.
            this._drainSfxOnly(left, right, frames, sfxData, sfxBuf, sfxRp, sfxAvail);
            this.totalQuanta++; this.totalFrames += frames;
            this._reportOffMain(this.stemRingFrames);
            return true;
        }

        // --- start-up prime gate (keyed on hungriest stem, mix-rate frames) ---
        if (!this.primed) {
            if (minAvail < this.stemPrimedFrames) {
                for (let i = 0; i < frames; i++) { left[i] = 0; right[i] = 0; }
                // still play SFX during the music prime cushion.
                this._addSfx(left, right, frames, sfxData, sfxBuf, sfxRp, sfxAvail);
                this.totalQuanta++; this.totalFrames += frames;
                this._reportOffMain(this.stemRingFrames);
                return true;
            }
            this.primed = true;
        }

        // ====================================================================
        // EQUAL-RATE FAST PATH (ctx == mix, the common case — browser honored
        // 44100). No carry, no resample: mix 1:1 to output, limiter at ctx rate.
        // This is the exact path the resampling code reduces to at step==1, but
        // written without the carry bookkeeping (which is only meaningful when
        // ctx != mix). Matches the C++ `!resample` fast path.
        // ====================================================================
        if (this.step === 1.0) {
            const invScale = 1.0 / 32768.0;
            const aRel = Math.exp(-1.0 / (this.ctxRate * (80.0 / 1000.0)));
            const kLimThreshold = 0.90;
            let env = this.limiterEnv;
            // hungriest stem gates how many real frames we can emit; the rest is
            // a true under-run (all stems sample-locked, so this is the stall case).
            const real = Math.min(frames, minAvail);
            let sfxIdx = sfxRp;
            for (let o = 0; o < frames; o++) {
                let L = 0.0, R = 0.0;
                if (o < real) {
                    for (let a = 0; a < active.length; a++) {
                        const st = active[a];
                        if (st.paused) continue;
                        if (o >= st.avail) continue;       // per-stem short -> 0
                        const idx = (st.rp + o) % st.rf;
                        const sample = stemPcm[st.slot][idx] * invScale;
                        let pan = st.pan; if (pan < -1) pan = -1; else if (pan > 1) pan = 1;
                        const vl = st.gain * (pan <= 0 ? 1.0 : 1.0 - pan);
                        const vr = st.gain * (pan >= 0 ? 1.0 : 1.0 + pan);
                        L += sample * vl; R += sample * vr;
                    }
                }
                // SFX additive (already ctx rate; benign if empty).
                if (o < sfxAvail && sfxData) {
                    const si = (sfxIdx % sfxBuf) * 2;
                    L += sfxData[si]; R += sfxData[si + 1];
                    sfxIdx++;
                }
                // master limiter (ctx rate) — exact port of MixSources.
                const la = L < 0 ? -L : L, ra = R < 0 ? -R : R;
                const level = la > ra ? la : ra;
                const desired = (level > kLimThreshold) ? (kLimThreshold / level) : 1.0;
                if (desired < env) env = desired;
                else env = aRel * env + (1.0 - aRel) * desired;
                left[o] = this._softClip(L * env);
                right[o] = this._softClip(R * env);
            }
            this.limiterEnv = env;
            // advance every active stem by the real frames emitted (lockstep),
            // and bump the MONOTONIC readTotal (hdr[4]) so the pump can advance
            // the producer back-pressure without wrap ambiguity.
            for (let a = 0; a < active.length; a++) {
                const st = active[a];
                const hdr = stemHdr[st.slot];
                Atomics.store(hdr, 1, (st.rp + real) % st.rf);
                Atomics.store(hdr, 4, (Atomics.load(hdr, 4) + real) | 0);
            }
            if (sfxData && this.sfxCursors) {
                const sfxUsed = Math.min(frames, sfxAvail);
                Atomics.store(this.sfxCursors, 1, (sfxRp + sfxUsed) % sfxBuf);
            }
            this.totalQuanta++; this.totalFrames += frames;
            const padded = frames - real;
            if (padded > 0) { this.underrunEvents++; this.underrunFrames += padded; }
            this._reportOffMain(minAvail);          // mix==ctx so frames are 1:1
            return true;
        }

        // --- how many mix-rate bus frames does this ctx quantum need? ---
        // resamplePos in [0,1) read offset from busBuf[0]; the last output reads
        // i0 and i0+1, so needTotal = floor(resamplePos + (frames-1)*step) + 2.
        const carryN = this.resampleCarryN;
        const sStart = this.resamplePos;
        const sEnd = sStart + (frames - 1) * step;
        let needTotal = (sEnd | 0) + 2;            // total mix-rate bus frames needed
        const busCap = (this.busBuf.length >> 1);
        if (needTotal > busCap) needTotal = busCap;
        const newMix = needTotal - carryN;          // fresh mix-rate frames to build

        const bus = this.busBuf;
        // carry frames from the previous quantum (stream-contiguous at bus[0..carryN-1]).
        for (let k = 0; k < carryN; k++) {
            bus[k * 2 + 0] = this.resampleCarry[k * 2 + 0];
            bus[k * 2 + 1] = this.resampleCarry[k * 2 + 1];
        }

        // --- additive per-stem mix into the mix-rate stereo bus ---
        // INVARIANT: at quantum start, stem rp == the stem index of bus[0]. The
        // carried frames bus[0..carryN-1] are stem [rp, rp+carryN); fresh frame f
        // goes to bus[carryN+f] == stem(rp + carryN + f). After the quantum the
        // stem advances by `consumed` (the bus frames the resampler used from
        // bus[0]), so next bus[0] == stem(rp + consumed) — see the advance below.
        // A stem short for a frame contributes 0 (its own dropout), never starving
        // the others.
        const invScale = 1.0 / 32768.0;
        for (let f = 0; f < newMix; f++) {
            let accL = 0.0, accR = 0.0;
            const busOff = carryN + f;              // bus slot for this fresh frame
            for (let a = 0; a < active.length; a++) {
                const st = active[a];
                if (st.paused) continue;
                if (busOff >= st.avail) continue;   // this stem is short here -> 0
                const idx = (st.rp + busOff) % st.rf;
                const sample = stemPcm[st.slot][idx] * invScale;
                // ComputePanGains (matches rb3_stream_receiver_native.cpp)
                let pan = st.pan; if (pan < -1) pan = -1; else if (pan > 1) pan = 1;
                const vl = st.gain * (pan <= 0 ? 1.0 : 1.0 - pan);
                const vr = st.gain * (pan >= 0 ? 1.0 : 1.0 + pan);
                accL += sample * vl;
                accR += sample * vr;
            }
            bus[busOff * 2 + 0] = accL;
            bus[busOff * 2 + 1] = accR;
        }

        // --- resample mix-rate bus -> ctx rate (linear, carry-all) ---
        const aRel = Math.exp(-1.0 / (this.ctxRate * (80.0 / 1000.0))); // ctx-rate release
        const kLimThreshold = 0.90;
        let env = this.limiterEnv;
        let s = sStart;
        // Per-stem under-run: how many bus frames were actually fillable (== minAvail
        // measured from rp). Frames past minAvail were 0 for the hungriest stem.
        // We still emit them (held by the per-stem 0), so true under-run = the
        // quantum needed bus frames beyond what the hungriest stem had.
        let underBusFrames = needTotal - carryN - Math.max(0, Math.min(newMix, minAvail));
        // SFX additive read pointer (ctx-rate, 1:1).
        let sfxIdx = sfxRp;
        for (let o = 0; o < frames; o++) {
            const i0 = s | 0;
            const t = s - i0;
            let l0 = bus[i0 * 2 + 0], r0 = bus[i0 * 2 + 1];
            let l1 = bus[(i0 + 1) * 2 + 0], r1 = bus[(i0 + 1) * 2 + 1];
            let L = l0 + (l1 - l0) * t;
            let R = r0 + (r1 - r0) * t;
            // additively combine the SFX ring (already ctx rate).
            if (o < sfxAvail && sfxData) {
                const si = (sfxIdx % sfxBuf) * 2;
                L += sfxData[si]; R += sfxData[si + 1];
                sfxIdx++;
            }
            // master limiter (post-resample, ctx rate) — port of MixSources.
            const la = L < 0 ? -L : L, ra = R < 0 ? -R : R;
            const level = la > ra ? la : ra;
            const desired = (level > kLimThreshold) ? (kLimThreshold / level) : 1.0;
            if (desired < env) env = desired;             // INSTANT attack
            else env = aRel * env + (1.0 - aRel) * desired; // one-pole release
            left[o] = this._softClip(L * env);
            right[o] = this._softClip(R * env);
            s += step;
        }
        this.limiterEnv = env;

        // --- carry ALL unconsumed bus frames into the next quantum ---
        let consumed = s | 0;
        if (consumed > needTotal - 1) consumed = needTotal - 1;
        if (consumed < 0) consumed = 0;
        let leftover = needTotal - consumed;
        if (leftover > this.kResampleCarryMax) leftover = this.kResampleCarryMax;
        for (let k = 0; k < leftover; k++) {
            this.resampleCarry[k * 2 + 0] = bus[(consumed + k) * 2 + 0];
            this.resampleCarry[k * 2 + 1] = bus[(consumed + k) * 2 + 1];
        }
        this.resampleCarryN = leftover;
        this.resamplePos = s - consumed;            // [0,1)

        // --- advance each stem's readPos by the bus frames consumed ---
        // next bus[0] == current bus[consumed] == stem(rp + consumed), so every
        // active stem advances by `consumed` to keep rp == bus[0]'s stem index
        // (the invariant the mix relies on). CLAMP to minAvail so a stall (all
        // stems frozen together) never advances rp past the producer writePos —
        // the carry/resamplePos guard already absorbs the sub-frame remainder.
        const stemAdv = Math.min(consumed, minAvail);
        for (let a = 0; a < active.length; a++) {
            const st = active[a];
            const hdr = stemHdr[st.slot];
            Atomics.store(hdr, 1, (st.rp + stemAdv) % st.rf);
            Atomics.store(hdr, 4, (Atomics.load(hdr, 4) + stemAdv) | 0);
        }
        // advance the SFX ring readPos by the ctx frames it actually supplied.
        if (sfxData && this.sfxCursors) {
            const sfxUsed = Math.min(frames, sfxAvail);
            Atomics.store(this.sfxCursors, 1, (sfxRp + sfxUsed) % sfxBuf);
        }

        // --- instrumentation ---
        this.totalQuanta++; this.totalFrames += frames;
        if (underBusFrames > 0) {
            // hungriest stem couldn't fill the whole quantum's bus need.
            this.underrunEvents++;
            // express padded frames in ctx frames (bus frames / step).
            this.underrunFrames += Math.round(underBusFrames / step);
        }
        // report min available in ctx-equivalent frames (bench converts via ctxRate).
        const minAvailCtx = Math.round(minAvail / step);
        this._reportOffMain(minAvailCtx);
        return true;
    }

    // SFX-only drain when no music stems are active (1:1, ctx rate). The SFX ring
    // is the existing pre-mixed output ring (already resampled by the pump).
    _drainSfxOnly(left, right, frames, sfxData, sfxBuf, sfxRp, sfxAvail) {
        const n = Math.min(frames, sfxAvail);
        for (let i = 0; i < n; i++) {
            const si = ((sfxRp + i) % sfxBuf) * 2;
            left[i] = sfxData ? sfxData[si] : 0;
            right[i] = sfxData ? sfxData[si + 1] : 0;
        }
        for (let i = n; i < frames; i++) { left[i] = 0; right[i] = 0; }
        if (sfxData && this.sfxCursors)
            Atomics.store(this.sfxCursors, 1, (sfxRp + n) % sfxBuf);
    }

    // Additively layer the SFX ring onto an already-filled (music) output, during
    // the music prime cushion (music is silent but SFX should still fire).
    _addSfx(left, right, frames, sfxData, sfxBuf, sfxRp, sfxAvail) {
        if (!sfxData || !this.sfxCursors) return;
        const n = Math.min(frames, sfxAvail);
        for (let i = 0; i < n; i++) {
            const si = ((sfxRp + i) % sfxBuf) * 2;
            left[i] += sfxData[si];
            right[i] += sfxData[si + 1];
        }
        Atomics.store(this.sfxCursors, 1, (sfxRp + n) % sfxBuf);
    }

    process(inputs, outputs, parameters) {
        if (this.offmain) return this._processOffMain(outputs);
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
