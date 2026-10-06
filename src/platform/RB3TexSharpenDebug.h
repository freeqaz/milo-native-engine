#pragma once
//
// Progressive-texture-sharpen recreate-at-new-size diagnostic (research/13 T0).
//
// The progressive-sharpen design (load a half-res stripped venue to reach
// gameplay fast, then background-fetch the full-res top-mips and sharpen each
// texture live in-session) rests on ONE engine assumption: swap a stripped
// RndBitmap up to full resolution + dirty the churn key (pixel pointer /
// fingerprint) and UploadRndTexIfNeeded (Rnd_Wgpu_RB3.cpp) will RECREATE the GPU
// texture at the NEW (larger) size and publish a NEW view — with no same-size
// assert and no fixed-size assumption.
//
// These helpers (defined by the GPU backend: Rnd_Wgpu_RB3.cpp for the rb3
// flavor, rndshape/RB3WiiTexSharpen.cpp over Tex_Wgpu's cache for dc3) let the native test suite drive that exact path and observe the result
// without exposing the internal cache. They are diagnostic surface only — no
// production draw path references them.

class RndTex;

// Per-RndTex GPU-cache snapshot for the recreate diagnostic.
struct RB3TexGpuInfo {
    bool        present = false;            // an sTexGpu entry exists for this tex
    bool        uploaded = false;          // its GPU texture/view is live
    int         texW = -1;                 // width of the GPU texture last created
    int         texH = -1;                 // height of the GPU texture last created
    const void* viewPtr = nullptr;         // wgpu::TextureView handle (identity)
    const void* texPtr = nullptr;          // wgpu::Texture handle (identity)
    unsigned long long globalRecreateCount = 0; // monotonic CreateTexture count
};

// Drive the production upload/churn path for `tex` through the engine GpuDevice
// (the same UploadRndTexIfNeeded that ResolveMaterialViews / WarmGpuForDir use).
// Returns true if the texture is resident afterward.
bool RB3DebugUploadTex(RndTex* tex);

// Snapshot the GPU-cache state for `tex` (size of the last-created texture, the
// current view/texture handles, and the global recreate counter).
RB3TexGpuInfo RB3DebugGetTexGpuInfo(RndTex* tex);

// While set, RB3SharpenReuploadTex reports the GPU as not ready (returns false
// without uploading), so a test can drive the sharpen manager's retry path
// without tearing the device down. Off by default; no production path sets it.
void RB3DebugSetSharpenGpuUnavailable(bool on);
