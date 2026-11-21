#pragma once

#include <cmath>

// DC3's post-process noise map tied to OUTPUT ROWS (np-look23 a).
//
// The map every venue but bid and glitterati grades with is
// world/shared/noise/noise_mono.tex: 8 x 256, a per-row grain, tiled at
// base_scale (1, 3), so 768 map rows run down the screen (stationary, so the
// pattern holds still). NgPostProc::CheckNoise hands the shader that tiling
// in screen uv, which ties it to the SCREEN, not to its rows: one texel is
// 768 / H output rows tall -- 0.94 at the Xbox's own 720 rows, 1.41 at 1080 --
// and the linear filter blends two neighbouring map rows by a weight that
// drifts down the screen. That is a fine line pattern at about two texels
// (2.81 px at 1080, aliased to 2.16 px at 713 and 2.14 px at 720) under a
// slow beat. Measured on a held frame: 1.2-3.6 8-bit levels at 2.81 px at
// 1920x1080, gone (<0.1) with the noise off. Xenia's 1280x720 frame carries
// the same pattern, identical in a single frame and a 3 s mean, at 2.14-2.15
// px; its amplitude depends on the region (the noise scales with the image):
// 1.04 levels in dclive far01's floor region (x900-1080, y540-710, where the
// native frame read 0.46), 2.56 over the left half (x0-640) and 2.69 over the
// whole frame. (An earlier 1.73 here came from a region that was not
// recorded; it is withdrawn.) It is the authored tiling, beat included, not a
// native defect.
//
// RETAIL KEEPS IT. The original's behaviour is 768 map rows down any screen
// under a linear filter, with that beat, and DC3_LOOK=retail is the original
// (LookProfile.h). Re-tiling is the enhanced look's
// (LookSettings::noiseSnapsToOutputRows), and needs this shape's capability
// too (rndshape::kNoiseSnapsToOutputRows); with either off every constant and
// both shader paths are exactly the authored ones.
//
// The enhanced look keeps the map, its intensity, its stationarity and the authored
// grain density as nearly as whole output rows allow: each layer's texel
// covers k output rows, k >= 1 the whole number nearest (in log terms) the
// authored outputRows / (scale * mapH). At 1080 that ratio is 1.406, between
// 1 (29 % finer) and 2 (42 % coarser), so k = 1; at the Xbox's 720 it is
// 0.94, so k = 1 there too -- one grain row per output row, which is what
// the original showed on its own target. The seeds move by whole texels, and
// every row of a texel samples the map at one fixed place in it, so every
// output row reads the same blend at every output size and render scale: no
// beat. Horizontal tiling is untouched: 8 texels across the screen are
// magnified hundreds of times and cannot beat.
//
// Visibility. Sampled at the texel centre, every row would carry the map's
// full row-to-row contrast, where the authored tiling's drifting blend gave
// that contrast only at the peaks of its beat: the lines would be about twice
// as visible on average (4.1 vs 1.7-2.0 levels at 713-1080) and 40x at 360,
// where 2.13 map rows per output row had all but erased them (0.1 levels).
// So each layer samples a fixed distance d past its texel's centre, blending
// in the next row by d: a row-alternating map keeps (1 - 2d) of its contrast
// on every row, and d is chosen so that equals the MEAN contrast the authored
// tiling gives at that size (NoiseLineContrast: 0.50 at 713-800, 0.42 at
// 1080, 0.02 at 360). Each layer's mean over a row pair (the map's tint) is
// unchanged; the beat goes.
//
// Render scale: `outputRows` is the rows the frame LEAVES at (OutputHeight),
// not the drawn buffer's: a 2x frame draws 2 rows per output row, each pair
// reads one blend, and the box filter keeps it -- the same picture as 1x.
//
// Gates: the DC3-shape capability (rndshape::kNoiseSnapsToOutputRows) AND the
// look (LookSettings::noiseSnapsToOutputRows, Enhanced only). With either
// off, NoiseRowSnapConstants leaves every constant untouched and the shaders
// take the authored uv unchanged.

// The whole number of output rows one noise texel covers, for a layer whose
// authored tiling puts `texelRows` map rows (scale * map height) down a
// screen `outputRows` rows high.
inline int NoiseOutputRowsPerTexel(int outputRows, double texelRows) {
    if (outputRows <= 0 || !(texelRows > 0.0)) return 1;
    const double px = (double)outputRows / texelRows;
    if (px <= 1.0) return 1;
    int k = (int)std::floor(px);
    if ((double)(k + 1) / px < px / (double)k) k++;
    return k;
}

// The mean 2-row amplitude |2 v(i) - v(i-1) - v(i+1)| / 4 of a +-1
// row-alternating map tiled `texelsPerRow` map rows per output row and read
// by a linear (level-0) filter at each row's centre, averaged over the
// tiling's phase: the authored line contrast at that size, as a fraction of
// the map's own (1 = every row at full contrast). A pure function of the
// tiling; matches the measured before/after ratio at 360 / 713 / 800 / 1080
// (0.018 / 0.494 / 0.498 / 0.417 vs 0.027 / 0.49 / 0.49 / 0.41).
inline double NoiseLineContrast(double texelsPerRow) {
    if (!(texelsPerRow > 0.0)) return 1.0;
    const int kRows = 64, kPhases = 16;
    double sum = 0.0;
    for (int ph = 0; ph < kPhases; ph++) {
        double v[kRows];
        for (int i = 0; i < kRows; i++) {
            const double p = ((double)i + 0.5) * texelsPerRow + ((double)ph + 0.5) / kPhases - 0.5;
            const double n = std::floor(p), f = p - n;
            const double a = std::fmod(n, 2.0) == 0.0 ? 1.0 : -1.0;
            v[i] = a * (1.0 - f) - a * f;
        }
        double s = 0.0;
        for (int i = 1; i + 1 < kRows; i++) s += std::fabs(2.0 * v[i] - v[i - 1] - v[i + 1]) / 4.0;
        sum += s / (kRows - 2);
    }
    return sum / kPhases;
}

// The distance past a texel's centre each row samples, for a layer retiled
// to k output rows per texel: a k-row square wave reads 1 / k on the same
// measure, so (1 - 2d) / k = NoiseLineContrast(authored), d in [0, 0.5].
inline double NoiseSampleOffset(int k, double authoredTexelsPerRow) {
    double f = NoiseLineContrast(authoredTexelsPerRow) * (double)k;
    if (f > 1.0) f = 1.0;
    if (f < 0.0) f = 0.0;
    return 0.5 * (1.0 - f);
}

// The derived half of the re-tiling: a pure function of the output rows, the
// map height and the two layers' authored scales (|base y|, |top multiplier|).
// NoiseLineContrast is ~2,000 steps per layer, so PostProcPass memoises this
// (NoiseRowSnapCache) and recomputes it only when one of those changes.
struct NoiseRowSnap {
    bool applied = false;          // false: leave every constant untouched
    double rowsB = 0, rowsT = 0;   // whole-texel map rows down the screen, per layer
    double baseY = 0;              // the base layer's re-tiled |scale| (rowsB / mapH)
    float dB = 0, dT = 0;          // each layer's sample offset past its texel's centre
};

inline NoiseRowSnap NoiseRowSnapFor(int outputRows, int mapH, double base, double top) {
    NoiseRowSnap r;
    if (outputRows <= 0 || mapH <= 0 || !(base > 0.0)) return r;
    const double texB = base * mapH, texT = base * top * mapH;
    const int kb = NoiseOutputRowsPerTexel(outputRows, texB);
    const int kt = NoiseOutputRowsPerTexel(outputRows, texT);
    r.applied = true;
    r.rowsB = (double)outputRows / kb;
    r.rowsT = (double)outputRows / kt;
    r.baseY = r.rowsB / mapH;
    r.dB = (float)NoiseSampleOffset(kb, texB / outputRows);
    r.dT = (float)NoiseSampleOffset(kt, texT / outputRows);
    return r;
}

// The cheap half, per frame: re-tiles NoiseConstants' seeds / params
// (kPS_NoiseSeeds, kPS_NoiseParams: params = (base x, base y, top multiplier,
// intensity)) to `r`. snap = (1, base d, top d) when applied; (0, 0, 0) and
// every constant untouched otherwise.
inline void ApplyNoiseRowSnap(const NoiseRowSnap &r, float seeds[4], float params[4], float snap[3]) {
    snap[0] = snap[1] = snap[2] = 0.0f;
    if (!r.applied) return;
    params[1] = (float)std::copysign(r.baseY, (double)params[1]);
    params[2] = (float)std::copysign(r.rowsT / r.rowsB, (double)params[2]);
    seeds[1] = (float)(std::round((double)seeds[1] * r.rowsB) / r.rowsB);
    seeds[3] = (float)(std::round((double)seeds[3] * r.rowsT) / r.rowsT);
    snap[0] = 1.0f;
    snap[1] = r.dB;
    snap[2] = r.dT;
}

// Both halves at once (the tests' entry point): so both layers' rows are whole
// output rows of a frame `outputRows` high, for a map `mapH` texels tall.
// Every constant untouched when `enabled` is false or the size is unknown.
inline void NoiseRowSnapConstants(bool enabled, int outputRows, int mapH, float seeds[4],
                                  float params[4], float snap[3]) {
    snap[0] = snap[1] = snap[2] = 0.0f;
    if (!enabled) return;
    ApplyNoiseRowSnap(NoiseRowSnapFor(outputRows, mapH, std::fabs((double)params[1]),
                                      std::fabs((double)params[2])),
                      seeds, params, snap);
}

// PostProcPass's memo of NoiseRowSnapFor, keyed on its four inputs.
// `computes` counts the recomputations (the tests read it).
struct NoiseRowSnapCache {
    int outputRows = -1, mapH = -1;
    double base = -1.0, top = -1.0;
    NoiseRowSnap value;
    unsigned computes = 0;

    const NoiseRowSnap &Get(int rows, int h, double b, double t) {
        if (rows != outputRows || h != mapH || b != base || t != top) {
            outputRows = rows; mapH = h; base = b; top = t;
            value = NoiseRowSnapFor(rows, h, b, t);
            computes++;
        }
        return value;
    }
};
