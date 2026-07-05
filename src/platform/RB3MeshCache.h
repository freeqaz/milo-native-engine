// RB3 per-mesh GPU vertex/index buffer upload cache — extracted from
// platform/Rnd_Wgpu_RB3.cpp (W1.2). RB3-only TU: compiled into
// MILO_ENGINE_GPU_PLATFORM_SOURCES_RB3, never alongside the DC3 MeshGpuCache.
//
// This header holds the cache-entry data structure (RB3MeshEntry), the two cache
// maps (sMeshGpu keyed by drawn mesh, sGeomSyncGen the per-owner geom-generation
// counter), the owner-gen lookup, and the CleanupGpuMesh invalidation entry point.
// The per-frame CreateBuffer leak counters are DEFINED in Rnd_Wgpu_RB3.cpp (frame
// lifecycle owned by BeginFrame/EndFrame/DrawMesh); only their extern decls live
// here so the moved upload helper can bump them.
#pragma once

#include "gfx/VertexFormats.h"   // GpuVertex / GpuVertexSkinned — engine vert layouts

#include <webgpu/webgpu_cpp.h>
#include <cstdint>
#include <unordered_map>
#include <vector>

class RndMesh;
class BandRnd;  // upload helper takes it by ref (mGpu Device/Queue); defined in Rnd_Wgpu_RB3.h

// ===========================================================================
// Per-mesh GPU vertex/index buffer cache.
//
// THE FIX for the unbounded WebGPU buffer leak: before this cache, DrawMesh
// created a fresh MeshVB + MeshIB wgpu::Buffer on EVERY draw, EVERY frame, for
// EVERY mesh — with nothing reusing them. On native Dawn the transient handles
// recycle within a frame, but on browser WebGPU the GPU-process resources
// accumulate without bound (real-GPU interpose measured ~73k buffers idle at
// main_hub, growing ~1.5k/sec until the GPU process SIGSEGVs at song_select).
//
// This mirrors the existing sTexGpu texture cache (and the dc3 backend's
// MeshGpuCache::EnsureMeshUploaded): the VB/IB are uploaded ONCE per mesh and
// reused every subsequent frame. They are re-uploaded only when the geometry
// changes — detected by a fingerprint (geom owner pointer + vert/face counts +
// skinned flag) AND by RndMesh::OnSync() marking the entry dirty (the signal
// RndText / dynamic meshes already fire via RndMesh::Sync when their verts
// mutate). Skinned characters animate via the per-frame bone palette
// (mBoneRing), NOT vertex re-upload, so caching their bind-pose verts is safe.
struct RB3MeshEntry {
    // --- Geometry (uploaded once per mesh, reused every frame) ---
    wgpu::Buffer vbuf;
    wgpu::Buffer ibuf;
    uint32_t     indexCount = 0;  // == 3 * numFaces (the original, unpadded count)
    bool         skinned = false;
    bool         uploaded = false;
    // Fingerprint: the geometry source we last uploaded. A change in any field
    // (owner swapped, vert/face count changed, skinned-ness flipped) forces a
    // re-upload even without an OnSync — defends shared-geom-owner instances and
    // SetGeomOwner hot-swaps that don't route through this exact mesh's OnSync.
    const void*  ownerKey = nullptr;
    int          fpVerts = -1;
    int          fpFaces = -1;
    bool         fpSkinned = false;
    // Owner geometry-generation last uploaded. The owner's Sync()/OnSync bumps a
    // per-owner counter (sGeomSyncGen); when this stamp lags the owner's live gen
    // the geometry changed without a count change (the geom-owner-proxy case, e.g.
    // an approaching sustain Tail whose verts move but section count saturated), so
    // the buffer must re-upload. 0 = never stamped (forces an upload on first sight).
    uint32_t     fpOwnerGen = 0;

    // --- L1 vertex-unpack cache (RB3_UNPACK_CACHE) ---
    // The per-draw CPU vertex unpack (Be*/Half2Float on -O0 wasm) was the dominant
    // uncounted residue on the game_screen reveal frame (research/09). Static-mesh
    // verts have NO consumer past the upload, so when !needUpload we skip the unpack
    // entirely. Skinned meshes are different: the V24 shard guard re-reads the
    // bind-pose `gpuVertsSkinned` EVERY frame to ratio-test the live blended pose,
    // so we keep the bind verts here and the guard reads the cache when the unpack
    // is skipped. Invalidated by exactly the conditions that set `needUpload`
    // (owner/fpVerts/fpFaces/fpSkinned + the OnSync `uploaded=false` dirty signal),
    // so a stale cache can never outlive its geometry. Skinned-only ⇒ bounded
    // memory (88 B/vert × character meshes ≈ a few MB).
    std::vector<GpuVertexSkinned> cachedSkinnedVerts;

    // --- Per-DRAW (per-instance) uniform buffers + bind groups ---
    // Before this cache, DrawMesh allocated the object/bone/material uniforms out
    // of a shared per-frame RING and built a FRESH bind group against the ring
    // offset every draw. On browser WebGPU, submit-queue backpressure pins each
    // frame's bind groups (and the ring) across all in-flight command buffers, so
    // the per-frame bind-group creates accumulate unbounded alongside the VB/IB.
    //
    // We cannot collapse this to ONE persistent uniform buffer per mesh: the SAME
    // RndMesh is drawn MULTIPLE times per frame (song_select list rows, repeated
    // panel widgets) with DIFFERENT object/material/bone state, and every WebGPU
    // queue.WriteBuffer for a frame executes BEFORE that frame's single submit —
    // so a shared per-mesh buffer would render every instance with the LAST
    // instance's uniforms (the darkened-rows regression). Instead we keep a small
    // per-mesh VECTOR of uniform "slots", indexed by a per-frame occurrence
    // counter (reset to 0 the first time the mesh is drawn each frame). Slot N
    // holds the Nth-this-frame instance's uniforms. Slots are created on demand
    // (so the count is bounded by this mesh's MAX instances in any one frame) and
    // RECYCLED across frames (the index resets, the wgpu handles persist) — a
    // free-list, no per-frame buffer/bind-group creation at steady state. No
    // bind-group-LAYOUT change, so the shared dc3 backend is untouched.
    struct UniformSlot {
        wgpu::Buffer    objUB;       // sizeof(ObjectUniforms) = 128B
        wgpu::BindGroup objBG;
        wgpu::Buffer    boneUB;      // sizeof(BoneUniforms) = 2560B (skinned only)
        wgpu::BindGroup boneBG;
        wgpu::Buffer    matUB;       // sizeof(MaterialUniforms) = 192B
        wgpu::BindGroup matBG;
        // Material bind-group cache invalidation: the material bind group also
        // binds the resolved diffuse/emissive texture VIEWS, which can change when
        // a lazy texture upload completes or the material pointer is swapped.
        // Rebuild matBG only when any of these change.
        const void*     matKey = nullptr;          // last RndMat*
        void*           matDiffuseView = nullptr;  // last wgpu diffuse view handle
        void*           matEmissiveView = nullptr; // last wgpu emissive view handle
    };
    std::vector<UniformSlot> slots;
    // The frame-sequence value this mesh's slot index was last reset against, and
    // the next slot to hand out THIS frame. When DrawMesh sees a mesh whose
    // frameSeen != the global frame sequence, it resets nextSlot to 0 (lazy
    // per-frame reset — no map-wide sweep at BeginFrame) before handing out a slot.
    uint64_t frameSeen = (uint64_t)-1;
    uint32_t nextSlot  = 0;
};
extern std::unordered_map<RndMesh*, RB3MeshEntry> sMeshGpu;

// Owner geometry-generation counter — fixes the geom-owner-proxy invalidation gap.
//
// A drawn mesh's cache entry is keyed by the DRAWN mesh pointer (sMeshGpu[mesh]),
// but its geometry comes from owner = mesh->GeomOwner() (SetGeomOwner). When the
// owner's verts mutate, the owner fires RndMesh::Sync -> OnSync(owner), which sets
// `uploaded=false` only on the OWNER's entry — but the owner is NEVER drawn (it has
// no entry, or an unrelated one), so the DRAWN proxies never see the dirty signal.
// The count-based fingerprint (fpVerts/fpFaces) only catches count changes, so a
// proxy whose owner rewrites vert POSITIONS without changing counts keeps drawing a
// stale GPU buffer. This is the sustain-tail bug: an approaching Tail's owner verts
// move every frame but the section count saturates at 2, so the proxies freeze at
// the first post-saturation upload (an invisible sliver); only a held tail (count
// changes per section boundary) re-uploads.
//
// Fix: a per-owner generation counter, bumped in OnSync and stamped into the proxy
// entry. A proxy whose stamped fpOwnerGen != the owner's live gen re-uploads, even
// when counts are unchanged. O(1) — one hash lookup per draw, only for meshes whose
// owner != self (self-owned meshes already invalidate correctly via their own
// OnSync entry). A missing owner entry reads as gen 0; CleanupGpuMesh erases both
// maps, and a recycled owner pointer always re-Syncs before its first draw (gen>=1
// != stamped 0 -> forced re-upload), so pointer reuse after free is safe.
extern std::unordered_map<RndMesh*, uint32_t> sGeomSyncGen;
static inline uint32_t LookupGeomSyncGen(RndMesh* owner) {
    auto it = sGeomSyncGen.find(owner);
    return (it != sGeomSyncGen.end()) ? it->second : 0u;
}

// Per-frame GPU-resource CREATE counters — proves the leak is fixed. Incremented
// at every CreateBuffer in DrawMesh's upload path (and the per-mesh bind-group
// builds), reset in BeginFrame, logged in EndFrame under RENDER_DBG. DEFINED in
// Rnd_Wgpu_RB3.cpp (frame lifecycle); declared extern here so the moved upload
// helper (RB3EnsureMeshGpu) can bump the buffer counter.
extern int sMeshBufCreatesThisFrame;
extern int sMeshBGCreatesThisFrame;

// Drop a mesh's cached GPU buffers. Strong def displaces the weak no-op
// link-stub (native: rndobj_synth_link_stubs.s; web: missing_stubs.js). Called
// from RndMesh's HX_NATIVE destructor so freed meshes release their GPU buffers
// instead of leaking the cache slot for the lifetime of the process.
void CleanupGpuMesh(RndMesh* mesh);

// ---------------------------------------------------------------------------
// RB3UnpackMeshVerts — the per-vertex CPU unpack shared by DrawMesh and the L2
// GPU warm sweep (BandRnd::WarmGpuForDir). Reads owner->mVerts (uncompressed RB3
// Vert, Color32-packed) OR owner->mCompressedVerts (Xbox-compressed, Be*-decoded
// in RB3MeshCache.cpp), filling the static OR skinned engine layout per
// `skinned`. Returns the unpacked vert count, or -1 if the mesh has no
// geometry. Factored out so the warm sweep's pre-upload is byte-identical to
// the draw-time upload (same VB bytes -> the first real draw is a guaranteed
// cache hit). This is the dominant -O0-wasm cost class (research/09: Be*/
// Half2Float/GpuVertexSkinned family) — charge it at the caller.
//
// Note: takes `GpuVertex` (the engine layout) rather than the RB3-only alias
// `GpuVertexRB3` (Rnd_Wgpu_RB3.h) to keep this header from pulling in that TU's
// header — the two names refer to the identical type.
int RB3UnpackMeshVerts(RndMesh* owner, bool skinned,
                       std::vector<GpuVertex>& gpuVerts,
                       std::vector<GpuVertexSkinned>& gpuVertsSkinned);

// ---------------------------------------------------------------------------
// RB3EnsureMeshGpu — idempotent mesh-upload helper extracted from DrawMesh's
// needUpload block. Unpacks (RB3UnpackMeshVerts) + uploads VB/IB + stamps the
// sMeshGpu fingerprint with the SAME keys DrawMesh uses, AND populates the L1
// skinned bind-vert cache, so after a warm pass the first real draw is a cache
// hit (no re-unpack, no re-upload, warmed shard guard). Returns true iff it
// actually uploaded (cache miss). No render-pass dependency — only creates+writes
// buffers (queue ops), safe to call outside an open pass during the loading dwell.
// Used by BandRnd::WarmGpuForDir. Takes BandRnd by ref for rnd.mGpu Device/Queue;
// its DEFINITION (RB3MeshCache.cpp) includes Rnd_Wgpu_RB3.h for the full type.
bool RB3EnsureMeshGpu(BandRnd& rnd, RndMesh* mesh);
