// DC3's Xbox pan law, folded to the native stereo output.
//
// The image pans every voice onto the 5.1 mastering voice in
// Voice::UpdateMix (dc3-decomp src/system/synth_xbox/Voice.cpp):
//
//  * a mono voice is panned with constant power around the 5.1 ring. Voice::
//    SetPan folds the pan into -4..4, and the ring is cut into six arcs
//    (SL -3 .. -5/3, FL -1 .. SL -3, FL -1 .. C 0, C 0 .. FR 1, FR 1 .. SR 3,
//    SR 3 .. SL 5), each a cos/sin pair at mVolume;
//  * a stereo voice is not ring-panned: left goes to FL at mVolume*cos(a) and
//    right to FR at mVolume*sin(a), a = (pan + 1) / 2 * pi/2, so at pan 0 both
//    are 3 dB down.
//
// Native mixes to stereo, so the 5.1 levels are folded with the game's own
// downmix, the one Synth360::Poll uses for its DML/DMR meter buses:
//   L = FL + C*g(-3 dB) + SL*g(-1.2 dB) + SR*g(-6.2 dB),  R the mirror.
// A hard-left mono voice is FL at mVolume (the old native law, vol*(1 - pan),
// played it at twice that), and a centred one is C, 3 dB down on each side.
//
// Used by StreamReceiverNative (song stems) and SampleInstNative (samples),
// both DC3-only. PLAN H-115: the boombox EQ meters this mix, and the old law
// read about 0.13 (5 dB of its 40 dB window) above the image.

#pragma once

#include <cmath>

namespace ImagePan {

// Synth360::Poll's downmix gains: DbToRatio(-3), (-1.2), (-6.2).
inline float GainCenter() { return std::pow(10.0f, -3.0f / 20.0f); }
inline float GainSide() { return std::pow(10.0f, -1.2f / 20.0f); }
inline float GainRear() { return std::pow(10.0f, -6.2f / 20.0f); }

// Voice::SetPan: Mod(pan + 4, 8) - 4.
inline float FoldPan(float pan) {
    float m = std::fmod(pan + 4.0f, 8.0f);
    if (m < 0.0f) m += 8.0f;
    return m - 4.0f;
}

// Mono voice: stereo gains for `vol` at `pan`.
inline void Mono(float vol, float pan, float &left, float &right) {
    const float p = FoldPan(pan);
    // 5.1 slots as the image orders them: 0 FL, 1 FR, 2 C, 3 LFE, 4 SL, 5 SR.
    int lo, hi;
    float loPan, hiPan;
    if (p < -3.0f) { lo = 4; loPan = -3.0f; hi = 5; hiPan = -5.0f; }
    else if (p < -1.0f) { lo = 0; loPan = -1.0f; hi = 4; hiPan = -3.0f; }
    else if (p < 0.0f) { lo = 0; loPan = -1.0f; hi = 2; hiPan = 0.0f; }
    else if (p < 1.0f) { lo = 2; loPan = 0.0f; hi = 1; hiPan = 1.0f; }
    else if (p < 3.0f) { lo = 1; loPan = 1.0f; hi = 5; hiPan = 3.0f; }
    else { lo = 5; loPan = 3.0f; hi = 4; hiPan = 5.0f; }
    const float angle = (p - loPan) / (hiPan - loPan) * 1.5707964f;
    float lv[6] = { 0, 0, 0, 0, 0, 0 };
    lv[lo] = vol * std::cos(angle);
    lv[hi] = vol * std::sin(angle);
    const float gc = GainCenter(), gs = GainSide(), gr = GainRear();
    left = lv[0] + lv[2] * gc + lv[4] * gs + lv[5] * gr;
    right = lv[1] + lv[2] * gc + lv[4] * gr + lv[5] * gs;
}

// Stereo voice: gains for its left and right channels at `pan`.
inline void Stereo(float vol, float pan, float &left, float &right) {
    const float angle = ((FoldPan(pan) + 1.0f) * 0.5f) * 1.5707964f;
    left = vol * std::cos(angle);
    right = vol * std::sin(angle);
}

} // namespace ImagePan
