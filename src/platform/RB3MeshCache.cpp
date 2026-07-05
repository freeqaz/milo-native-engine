// RB3 per-mesh GPU vertex/index buffer upload cache — extracted from
// platform/Rnd_Wgpu_RB3.cpp (W1.2). See platform/RB3MeshCache.h for the design
// notes. RB3-only TU (MILO_ENGINE_GPU_PLATFORM_SOURCES_RB3); never compiled
// alongside the DC3 MeshGpuCache (which also defines a CleanupGpuMesh).

#include "platform/RB3MeshCache.h"

#include "rndobj/Mesh.h"

// The cache map (keyed by drawn mesh). Definition; declared extern in the header.
std::unordered_map<RndMesh*, RB3MeshEntry> sMeshGpu;

// The per-owner geom-generation counter. Definition; declared extern in the header.
std::unordered_map<RndMesh*, uint32_t> sGeomSyncGen;

// Drop a mesh's cached GPU buffers. Strong def displaces the weak no-op
// link-stub (native: rndobj_synth_link_stubs.s; web: missing_stubs.js). Called
// from RndMesh's HX_NATIVE destructor so freed meshes release their GPU buffers
// instead of leaking the cache slot for the lifetime of the process.
void CleanupGpuMesh(RndMesh* mesh) {
    sMeshGpu.erase(mesh);
    // A freed mesh may have been someone's geometry owner; drop its generation
    // counter too so the map doesn't grow unbounded. A recycled pointer re-Syncs
    // before its next draw (gen>=1 != any stamped 0), so this is safe.
    sGeomSyncGen.erase(mesh);
}

void RndMesh::OnSync(int) {
    // Geometry changed — mark this mesh's cached GPU vertex/index buffers dirty so
    // the next DrawMesh re-uploads them. This is the dirty signal dynamic meshes
    // (RndText sub-meshes, ribbons, procedural geometry) fire via RndMesh::Sync
    // when their verts mutate; without it the per-mesh GPU cache (sMeshGpu) would
    // keep drawing stale geometry. Clearing `uploaded` keeps the existing
    // wgpu::Buffers (re-`WriteBuffer`'d in place on re-upload if sizes match, or
    // recreated if vert/face counts changed) — no leak.
    auto it = sMeshGpu.find(this);
    if (it != sMeshGpu.end()) it->second.uploaded = false;
    // Bump this mesh's geometry generation. When `this` is a GEOMETRY OWNER for one
    // or more drawn proxies (SetGeomOwner), those proxies are keyed by their OWN
    // pointer in sMeshGpu and never see the `uploaded=false` above. Their DrawMesh
    // compares their stamped fpOwnerGen against this live gen and re-uploads when it
    // lags — this is what makes an approaching sustain Tail's tube (owner verts move
    // every frame, section count saturated -> count fingerprint unchanged) render
    // before it is held. Self-owned meshes also bump it, harmlessly: their own entry
    // already invalidated via `uploaded=false`, and DrawMesh skips the owner-gen
    // check when owner == mesh, so the stamp simply tracks along.
    ++sGeomSyncGen[this];
}
