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
// the same pattern (2.14 px, 1.73 levels, identical in a single frame and a
// 3 s mean): it is the authored tiling, not a native defect.
//
// Both looks keep the map, its intensity, its stationarity and the authored
// grain density as nearly as whole output rows allow: each layer's texel
// covers k output rows, k >= 1 the whole number nearest (in log terms) the
// authored outputRows / (scale * mapH). At 1080 that ratio is 1.406, between
// 1 (29 % finer) and 2 (42 % coarser), so k = 1; at the Xbox's 720 it is
// 0.94, so k = 1 there too -- one grain row per output row, which is what
// the original showed on its own target. The seeds move by whole texels, and
// the shaders sample each row at its texel's centre (so the linear filter
// never blends two map rows), so every output row reads exactly one texel at
// every output size and render scale. Horizontal tiling is untouched: 8
// texels across the screen are magnified hundreds of times and cannot beat.

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

// Re-tiles NoiseConstants' seeds / params (kPS_NoiseSeeds, kPS_NoiseParams:
// params = (base x, base y, top multiplier, intensity)) so both layers' rows
// are whole output rows of a frame `outputRows` high, for a map `mapH` texels
// tall. No-op when either is unknown.
inline void SnapNoiseToOutputRows(int outputRows, int mapH, float seeds[4], float params[4]) {
    if (outputRows <= 0 || mapH <= 0 || params[1] == 0.0f) return;
    const double base = std::fabs((double)params[1]);
    const double top = std::fabs((double)params[2]);
    const int kb = NoiseOutputRowsPerTexel(outputRows, base * mapH);
    const int kt = NoiseOutputRowsPerTexel(outputRows, base * top * mapH);
    const double rowsB = (double)outputRows / kb;
    const double rowsT = (double)outputRows / kt;
    params[1] = (float)std::copysign(rowsB / mapH, (double)params[1]);
    params[2] = (float)std::copysign(rowsT / rowsB, (double)params[2]);
    seeds[1] = (float)(std::round((double)seeds[1] * rowsB) / rowsB);
    seeds[3] = (float)(std::round((double)seeds[3] * rowsT) / rowsT);
}
