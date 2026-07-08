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
#include <string>
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
    const void* sceneBG;      // mActiveScene.group.Get() — opaque identity token
    const void* matBG;        // matBG.Get()
    const void* objBG;        // objBG.Get()
    const void* boneBG;       // boneBG.Get()
};

// ---------------------------------------------------------------------------
// W17 R3-UIDUMP — provenance sidecar (RB3_DRAWLOG_PROV).
//
// RB3DrawRecord above is a COMMITTED byte-identical data contract (the 792-draw
// golden diffs it directly), so the human-readable names/rect/pass metadata the
// UI-forensics instrument needs live in a PARALLEL vector, one RB3DrawProv per
// RB3DrawRecord at the SAME index. Filled only when RB3_DRAWLOG_PROV is set
// (which also forces the ring on, so the two vectors stay index-aligned). When
// prov is off the vector stays empty and the drawlog JSON is byte-identical.
// ---------------------------------------------------------------------------
// T2-WORLDROI (Wave 19): one on-screen bone SEGMENT sub-rect. `bone`=bone Name();
// `rect`=x,y,w,h px bbox of the bone-world point (and its palette-member parent
// endpoint) projected through the world-cam viewProj. Only present on rectKind==3
// (skinned-pose) rows. Localizes which BONE a pixel ROI hit. This axis is SPATIAL
// (which mesh/bone/owner drew a pixel) — NOT the frame-assignment TIMING axis (T1)
// nor R4's ledger `order` axis; never conflate them.
struct RB3ProvBoneRect {
    std::string bone;
    float       rect[4];
};

struct RB3DrawProv {
    std::string meshName;        // mesh->Name()      ("" for internal glyph meshes)
    std::string matName;         // mat->Name()       ("" if no material)
    std::string camName;         // RndCam::sCurrent->Name()
    std::string transParent;     // mesh->TransParent() ? ->Name() : ""
    std::string scopePanel;      // top PANEL scope-stack entry ("" if none)
    std::string scopeOwner;      // top OWNER scope-stack entry (text/label/widget)
    float       matColor[4];     // authored mat->GetColor() at draw time
    float       boundColor[4];   // effective post-binder mu.color (UI floor applied)
    float       rect[4];         // projected screen bbox x,y,w,h in px; w<0 => degenerate
    uint8_t     rectKind;        // 0 exact-verts, 1 sphere-fallback, 2 unavailable, 3 skinned-pose bbox
    uint16_t    passIdx;         // frame-monotonic render-pass sequence number
    uint8_t     passDepthLoadOp; // 0=Clear,1=Load,2=none  at THIS pass's open
    // T2-WORLDROI: skinned-pose provenance (rectKind==3 only; empty/0 otherwise).
    int         boneFallback = 0;            // # bones rendering at BIND (null+nonfinite+clamped);
                                             // when >0 the rect unions the bind-pose sphere extent.
    std::vector<RB3ProvBoneRect> boneRects;  // per-bone screen sub-rects (rectKind==3 only)
};

// Current frame's provenance sidecar (index-aligned with RB3DebugGetDrawLog()).
// Empty unless RB3_DRAWLOG_PROV is set.
const std::vector<RB3DrawProv>& RB3DebugGetDrawProv();

// True if the provenance sidecar is active (RB3_DRAWLOG_PROV env; cached).
bool RB3DrawProvEnabled();

// Provenance scope stack (engine-owned, game-fed). Push/pop are NO-OPS on a
// single cached-static branch unless RB3_DRAWLOG_PROV is set — so the game-side
// `#ifdef HX_NATIVE` hooks cost one predicted branch when prov is off. kind:
// 0=panel, 1=owner (text/label/widget). RecordDrawLog snapshots the current
// top-of-stack panel + owner into the sidecar. The `name` pointer is copied into
// a std::string at push time (caller need not keep it alive).
void RB3DrawScopePush(int kind, const char* name);
void RB3DrawScopePop(int kind);
struct RB3DrawScopeGuard {
    int  kind;
    bool active;
    RB3DrawScopeGuard(int k, const char* name) : kind(k), active(false) {
        if (name) { RB3DrawScopePush(k, name); active = true; }
    }
    ~RB3DrawScopeGuard() { if (active) RB3DrawScopePop(kind); }
    RB3DrawScopeGuard(const RB3DrawScopeGuard&) = delete;
    RB3DrawScopeGuard& operator=(const RB3DrawScopeGuard&) = delete;
};

// Read the current frame's captured draw log (the ring the renderer fills in
// DrawMesh; cleared each BeginFrame). Empty unless recording is enabled.
const std::vector<RB3DrawRecord>& RB3DebugGetDrawLog();

// Force draw-log recording on/off without an env var (for gtests). ORed with
// the RB3_DRAWLOG env gate: recording is active if either is set.
void RB3DebugSetDrawLogEnabled(bool on);

// True if draw-log recording is currently active (env flag OR debug override).
bool RB3DebugDrawLogEnabled();
