#pragma once
//
// W0.3 per-draw state-log ring — debug/test accessor surface.
//
// The RB3 GPU backend (Rnd_Wgpu_RB3.cpp) records a structured per-draw state
// record for every main mesh it rasterizes, into a ring that is INERT and
// near-zero-cost when the RB3_DRAWLOG env flag is unset (RecordDrawLog
// early-returns on one cached-static branch, no allocation, no bind-group
// handle reads). The ring exists to catch the two historical per-draw
// regression classes mechanically:
//   1. Co-location — identical world transform across instances that should
//      differ (crowd/drum collapse) — encoded by RB3DrawRecord::world[16].
//   2. Uniform / bind-group collapse (the a0f98ad class) — draws that should
//      carry distinct per-object/material/bone uniforms end up sharing one
//      bind group — encoded by the opaque sceneBG/matBG/objBG/boneBG identity
//      tokens (never dereferenced; only compared for sharing pattern).
//
// This header lets the native test suite (rb3-tests) force recording on
// without an env var and read the captured ring, without exposing the private
// renderer internals. Diagnostic surface only — no production draw path
// references these functions.

#include <cstdint>
#include <vector>

// Per-draw record (POD pushed on the hot path — identity is opaque). Layout is
// the committed data contract shared by the JSON dump (W0.3.S1) and the golden
// comparator (W0.3.S2). Bind-group handles are OPAQUE identity tokens only:
// never dereferenced, never value-compared — dense-ified per stream at dump
// time so the JSON is run/host independent while preserving the sharing
// pattern.
struct RB3DrawRecord {
    uint64_t pipelineHash;    // PipelineKeyHash{}(key) — EXACT
    uint8_t  blend;           // (int)key.blend
    uint8_t  zMode;           // (int)key.zMode
    uint8_t  layout;          // (int)key.layout (Static/Skinned)
    uint8_t  flags;           // bit0 hasDepth, bit1 alphaCut, bit2 alphaWrite, bit3 skinned
    uint32_t targetFormat;    // (uint32)key.targetFormat
    uint32_t indexCount;      // cachedIndexCount
    uint32_t triCount;        // nf
    uint32_t vertCount;       // meshEntry.fpVerts (>=0)
    uint64_t meshNameHash;    // FNV-1a of mesh->Name() (empty -> 0) — alignment key
    float    world[16];       // obj.world, column-major — FLOAT EPS compared
    const void* sceneBG;      // mSceneBindGroup.Get() — opaque identity token
    const void* matBG;        // matBG.Get()
    const void* objBG;        // objBG.Get()
    const void* boneBG;       // boneBG.Get()
};

// Read the current frame's captured draw log (the ring the renderer fills in
// DrawMesh; cleared each BeginFrame). Empty unless recording is enabled.
const std::vector<RB3DrawRecord>& RB3DebugGetDrawLog();

// Force draw-log recording on/off without an env var (for gtests). ORed with
// the RB3_DRAWLOG env gate: recording is active if either is set.
void RB3DebugSetDrawLogEnabled(bool on);

// True if draw-log recording is currently active (env flag OR debug override).
bool RB3DebugDrawLogEnabled();
