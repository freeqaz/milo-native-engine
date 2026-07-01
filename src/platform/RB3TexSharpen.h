#pragma once
//
// Progressive-texture-sharpen manager (research/13 T1) — native-only.
//
// A4 ships web venues with their top texture mip STRIPPED (half-res base) so the
// venue reaches gameplay fast on a slow link. This manager makes that strip
// PROGRESSIVE: once gameplay is running (off the critical path), the rb3 glue
// background-fetches the venue's `.sharpen` sidecar (the discarded full-res
// top-mip bytes) and hands it here; this manager matches each sidecar entry to a
// loaded RndTex by a recomputed TexFingerprint, swaps the RndBitmap back up to
// full resolution, and re-invokes the GPU upload so the texture is RECREATED at
// full size — live, in-session, spread over a few textures per frame so the
// recreate/upload bursts never hitch.
//
// The mechanism rests on the engine churn path (UploadRndTexIfNeeded,
// Rnd_Wgpu_RB3.cpp): swapping a bitmap's pixel pointer + size + dirtying its
// fingerprint is a cache MISS → recreate at the new size → new view → the cached
// material bind group rebuilds automatically (its existing view-handle compare
// detects the new view). Proven by native/tests/test_texsharpen.cpp (the T0 gate)
// against the real GPU.
//
// SPLIT OF CONCERNS. This TU is GPU/RndTex-focused and takes the sidecar BYTES
// already loaded. The async low-priority FETCH (web), gameplay-gating, and the
// local-file/MEMFS read live in the rb3 native/web glue (it owns the asset roots
// + WebAssets) — so the `__EMSCRIPTEN__` web arm stays out of the engine. On
// native the sidecar is a local file (no network). Flag: RB3_PROGRESSIVE_SHARPEN
// (default ON; opt-out keeps the A4 stripped-stays-stripped behavior). getenv-once.
//
// All of this is HX_NATIVE / native-only — no matched-Wii-TU behavior change.

class ObjectDir;
class RndTex;

#include <cstdint>

// ---------------------------------------------------------------------------
// Engine churn-path hooks (defined in Rnd_Wgpu_RB3.cpp, which owns sTexGpu).
// The manager's only contact with the texture cache + upload path.
// ---------------------------------------------------------------------------

// Recompute the TexFingerprint UploadRndTexIfNeeded keys on, over `tex`'s CURRENT
// live pixels (PixelBytes() bytes). Returns 0 for <16-byte / null pixel buffers
// (same as the upload path). The sharpen match key: equals the sidecar entry's
// stripped_fp for the texture that loaded the stripped mip.
uint32_t RB3SharpenTexFingerprint(const RndTex* tex);

// Drive the production UploadRndTexIfNeeded for `tex` (same call the draw path
// makes). After a bitmap swap this is a cache miss → recreate at the new size →
// new view. Returns true iff a recreate actually happened (vs a cache hit), so
// the scheduler can charge real work to its per-frame budget. No-op (false) if
// the GPU isn't ready.
bool RB3SharpenReuploadTex(RndTex* tex);

// ---------------------------------------------------------------------------
// Flag.
// ---------------------------------------------------------------------------

// RB3_PROGRESSIVE_SHARPEN gate (getenv-once). Default ON. Set
// RB3_PROGRESSIVE_SHARPEN=0 to keep the A4 stripped venue stripped (opt-out).
bool RB3ProgressiveSharpenEnabled();

// ---------------------------------------------------------------------------
// Per-frame textures-to-sharpen budget (getenv-once, RB3_SHARPEN_PER_FRAME).
// Small by default (4) so the recreate+upload bursts spread over steady gameplay
// frames instead of hitching. The rb3 glue passes this to the poll.
// ---------------------------------------------------------------------------
int RB3SharpenPerFrame();

// ---------------------------------------------------------------------------
// Manager API — driven by the rb3 native/web glue.
// ---------------------------------------------------------------------------

// Load a freshly-fetched `.sharpen` sidecar for `venueDir` and match its entries
// to that venue's loaded RndTex objects (walk ObjDirItr<RndTex>, key by recomputed
// TexFingerprint, with the sidecar index + best-effort name as secondary). `bytes`
// is the SHRP blob; `len` its length. Returns the number of entries matched to a
// loaded texture (0 == nothing to sharpen / bad blob / flag off). Idempotent-ish:
// re-loading replaces the session. Does NOT swap anything yet — RB3SharpenStep
// does the incremental work. No-op if the flag is off or `venueDir` is null.
int RB3SharpenLoadSidecar(ObjectDir* venueDir, const uint8_t* bytes, uint32_t len);

// Sharpen up to `maxThisFrame` not-yet-sharpened matched textures: reconstruct
// the full-res base level, swap the RndBitmap (W/H/rowBytes/pixels), and re-invoke
// the upload so the GPU texture is recreated at full size. Returns the number
// actually sharpened this call (0 == session complete or none pending). Call once
// per gameplay frame, OUTSIDE an open render pass. Cheap once complete.
int RB3SharpenStep(int maxThisFrame);

// True once every matched texture has been sharpened (session complete) — the
// glue can stop fetching/polling. False while work remains or no session loaded.
bool RB3SharpenComplete();

// Clear the active session (e.g. on song unload / venue teardown) so a freed
// venueDir's RndTex pointers are never touched again. Idempotent. Frees the
// owned full-res pixel buffers ONLY for textures not yet swapped-in (a swapped-in
// buffer is now owned by the live RndBitmap and must outlive the session).
void RB3SharpenReset();

// Diagnostics for the native harness gate (sTexGpu growth is also observable via
// RB3DebugGetTexGpuInfo). 0/0 == no active matched session.
struct RB3SharpenStatus {
    bool     active = false;        // a session is loaded + matched
    int      matched = 0;           // sidecar entries matched to a loaded RndTex
    int      sharpened = 0;         // matched textures swapped up to full-res so far
    int      total = 0;             // sidecar entry count
    uint64_t bytesUpgraded = 0;     // sum of full-res top-mip bytes applied
};
RB3SharpenStatus RB3SharpenGetStatus();
