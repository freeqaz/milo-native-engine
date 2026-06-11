// BandRnd implementation — RB3 GPU backend. Selected when MILO_ENGINE_GPU_BACKEND=rb3.
// See platform/Rnd_Wgpu_RB3.h.

#include "platform/Rnd_Wgpu_RB3.h"

#include "rndobj/Cam.h"
#include "rndobj/Mesh.h"
#include "rndobj/Mat.h"
#include "rndobj/Tex.h"
#include "rndobj/Text.h"
#include "rndobj/Trans.h"
#include "rndobj/Dir.h"
#include "rndobj/Group.h"
#include "rndobj/Env.h"
#include "rndobj/Lit.h"
#include "rndobj/MultiMesh.h"
#include "rndobj/Part.h"
#include "rndobj/PostProc.h"
#include "rndobj/ColorXfm.h"
#include "math/Mtx.h"
#include "math/Vec.h"
#include "os/Debug.h"
#include "platform/NativeSettings.h"
#include "platform/FrameTraceCounters.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <vector>

// The single global renderer. TheRnd (declared extern in rndobj/Rnd.h) is a
// WEAK no-op alias in rndobj_synth_link_stubs.s; this strong definition wins.
BandRnd gBandRnd;
Rnd* TheRnd = &gBandRnd;

// Registers the legacy short-name rndobj class aliases (Tex/Text/Dir). Defined
// below; also called from the real game boot in main_native.cpp (RunGame).
void RB3RegisterLegacyRndAliases();

// Stage 2 A/B canary gate: RB3_PP_OFF=1 forces the whole postproc intermediate
// path inactive (frame renders straight to the framebuffer, no composite) — used
// to prove a postproc-active screen is pixel-identical with the grade skipped.
static bool RB3PostProcDisabled();
static bool RB3PipelinePrewarmDisabled();

// L1 vertex-unpack cache (default ON; RB3_UNPACK_CACHE_OFF=1 opts out for A/B).
// When OFF, DrawMesh unpacks every vertex on every draw (the legacy behavior) —
// used to prove the cache is visual-no-op. getenv-once latch (house style).
static bool RB3UnpackCacheOff() {
    static int s = -1;
    if (s < 0) {
        const char* e = getenv("RB3_UNPACK_CACHE_OFF");
        s = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return s != 0;
}

// ---------------------------------------------------------------------------
// VertexFormats::StaticLayout()/SkinnedLayout() — the engine's PipelineManager
// (gfx/PipelineManager.cpp) calls these, but their definitions live in
// gfx/VertexFormats.cpp, which RB3 EXCLUDES (it includes DC3's rndobj/Mesh.h for
// the Unpack* helpers we don't use). The layout fns are pure wgpu (no RndMesh),
// so we provide them here. They MUST match GpuVertexRB3 / the engine GpuVertex
// byte layout exactly (pos/norm/color/uv/tangent + skinned bone data).
// ---------------------------------------------------------------------------
namespace VertexFormats {
static wgpu::VertexAttribute MkA(wgpu::VertexFormat f, uint64_t off, uint32_t loc) {
    wgpu::VertexAttribute a{}; a.format = f; a.offset = off; a.shaderLocation = loc; return a;
}
const wgpu::VertexBufferLayout& StaticLayout() {
    static wgpu::VertexAttribute attrs[5];
    static wgpu::VertexBufferLayout layout;
    static bool inited = false;
    if (!inited) {
        attrs[0] = MkA(wgpu::VertexFormat::Float32x3, 0,  0);
        attrs[1] = MkA(wgpu::VertexFormat::Float32x3, 12, 1);
        attrs[2] = MkA(wgpu::VertexFormat::Float32x4, 24, 2);
        attrs[3] = MkA(wgpu::VertexFormat::Float32x2, 40, 3);
        attrs[4] = MkA(wgpu::VertexFormat::Float32x4, 48, 4);
        layout.arrayStride = sizeof(GpuVertexRB3);
        layout.stepMode = wgpu::VertexStepMode::Vertex;
        layout.attributeCount = 5; layout.attributes = attrs;
        inited = true;
    }
    return layout;
}
const wgpu::VertexBufferLayout& SkinnedLayout() {
    // 88-byte skinned vertex layout (matches engine GpuVertexSkinned).
    static wgpu::VertexAttribute attrs[7];
    static wgpu::VertexBufferLayout layout;
    static bool inited = false;
    if (!inited) {
        attrs[0] = MkA(wgpu::VertexFormat::Float32x3, 0,  0);
        attrs[1] = MkA(wgpu::VertexFormat::Float32x3, 12, 1);
        attrs[2] = MkA(wgpu::VertexFormat::Float32x4, 24, 2);
        attrs[3] = MkA(wgpu::VertexFormat::Float32x2, 40, 3);
        attrs[4] = MkA(wgpu::VertexFormat::Float32x4, 48, 4);
        attrs[5] = MkA(wgpu::VertexFormat::Uint8x4,   64, 5);
        attrs[6] = MkA(wgpu::VertexFormat::Float32x4, 72, 6);
        layout.arrayStride = 88;
        layout.stepMode = wgpu::VertexStepMode::Vertex;
        layout.attributeCount = 7; layout.attributes = attrs;
        inited = true;
    }
    return layout;
}
} // namespace VertexFormats

// ===========================================================================
// BandUniformRing
// ===========================================================================
void BandUniformRing::Init(wgpu::Device device, uint32_t capacity, const char* label) {
    mDevice = device;
    mLabel = label;
    wgpu::BufferDescriptor d{};
    d.label = label;
    d.size = capacity;
    d.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    mBuffer = device.CreateBuffer(&d);
    mCapacity = capacity;
    mOffset = 0;
}

uint32_t BandUniformRing::Write(wgpu::Queue queue, const void* data, uint32_t size) {
    uint32_t aligned = (size + kAlign - 1) & ~(kAlign - 1);
    if (mOffset + aligned > mCapacity) mOffset = 0; // wrap (defensive)
    uint32_t off = mOffset;
    queue.WriteBuffer(mBuffer, off, data, size);
    mOffset += aligned;
    return off;
}

// ===========================================================================
// Matrix helpers — output is 16 floats COLUMN-MAJOR (WGSL mat4x4f reads cols),
// so the WGSL expression `M * v` applies M with the math we set up here.
// ===========================================================================

// A Milo Transform is row-vector: world = v * [m | v], i.e. for a point p,
//   p_world = p.x*m.x + p.y*m.y + p.z*m.z + t  (m.x/m.y/m.z are basis ROWS).
// To use it as a column-major mat4 M with `M * p` semantics (column-vector),
// the matrix columns are the basis rows of m plus translation:
//   col0 = (m.x.x, m.x.y, m.x.z, 0)? — NO. v*M means M's ROWS are the basis.
// For M*p to equal p*MiloXfm, M must be the transpose: M's COLUMNS are m.x,m.y,m.z.
// Column-major storage of M means storing M's columns contiguously, which are
// exactly m.x, m.y, m.z, then translation as the 4th column with the 4th row
// being (0,0,0,1).
static void MiloXfmToColMajor(const Transform& x, float* out) {
    // M (column-major). Column j = basis vector along axis j of the Milo xfm.
    out[0]  = x.m.x.x; out[1]  = x.m.x.y; out[2]  = x.m.x.z; out[3]  = 0.0f;
    out[4]  = x.m.y.x; out[5]  = x.m.y.y; out[6]  = x.m.y.z; out[7]  = 0.0f;
    out[8]  = x.m.z.x; out[9]  = x.m.z.y; out[10] = x.m.z.z; out[11] = 0.0f;
    out[12] = x.v.x;   out[13] = x.v.y;   out[14] = x.v.z;   out[15] = 1.0f;
}

static void MatMul4(const float* A, const float* B, float* out) {
    // out = A * B (all column-major). out_col_j = A * B_col_j.
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 4; i++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += A[k * 4 + i] * B[j * 4 + k];
            out[j * 4 + i] = s;
        }
    }
}

// ===========================================================================
// BandRnd
// ===========================================================================

void BandRnd::InitScreenshots() {
    const char* ssDir = getenv("MILO_SCREENSHOT_DIR");
    if (!ssDir || !ssDir[0]) return;
    mShotDir = ssDir;

    const char* ssFrames = getenv("MILO_SCREENSHOT_FRAMES");
    if (!ssFrames || !ssFrames[0]) ssFrames = "5,25,50,120,200,280,360,500,700,1100,1400";
    {
        std::istringstream iss(ssFrames);
        std::string tok;
        while (std::getline(iss, tok, ',')) {
            int f = atoi(tok.c_str());
            if (f >= 0) mShotFrames.push_back(f);
        }
    }

    const char* ssNames = getenv("MILO_SCREENSHOT_NAMES");
    if (ssNames && ssNames[0]) {
        std::istringstream iss(ssNames);
        std::string tok;
        while (std::getline(iss, tok, ',')) mShotNames.push_back(tok);
    }
    // Pad names if fewer than frames.
    while ((int)mShotNames.size() < (int)mShotFrames.size())
        mShotNames.push_back("");

    mShotIndex = 0;
    printf("BandRnd: auto-screenshot — dir=%s frames=", mShotDir.c_str());
    for (int i = 0; i < (int)mShotFrames.size(); i++) {
        if (i) printf(",");
        printf("%d", mShotFrames[i]);
    }
    printf("\n");
}

void BandRnd::BeginDrawing() {
    if (!mGpuReady) return;
    // V2: mirror Rnd::BeginDrawing's mDefaultCam->Select() so the first frames
    // (intro / main_hub / quickplay) always have a non-null current cam. The
    // base Rnd::BeginDrawing is bypassed by this override, so without this the
    // hub/quickplay screens render against an identity-matrix view that puts
    // the camera at the origin looking into a white backdrop. Once a scene cam
    // calls Select(), RndCam::sCurrent overrides this and we honor it.
    if (!RndCam::sCurrent && mDefaultCam) {
        mDefaultCam->Select();
    }
    RndCam* cam = RndCam::sCurrent ? RndCam::sCurrent : mDefaultCam;
    BeginFrame(cam);
}

void BandRnd::EndDrawing() {
    if (!mGpuReady) return;
    EndFrame();

    // V4: latch the first frame on which a non-default scene cam is selected.
    // The default cam ([default cam]) sits at world (0,0,0) with identity
    // basis — capturing while it is current produces a black-top-half intro
    // because the scene's named cam hasn't been positioned yet. Once any
    // named cam Selects (RndCam::sCurrent != mDefaultCam) record the frame so
    // we can defer early scheduled shots that landed on the default-cam
    // window.
    if (mFirstSceneCamFrame < 0 && RndCam::sCurrent &&
        RndCam::sCurrent != mDefaultCam) {
        mFirstSceneCamFrame = mFrameCount;
    }

    // Auto-screenshot: capture the specified frames. A shot scheduled before
    // any scene cam selected is rescheduled to the first stable scene-cam
    // frame + 4 (4 frames of cam-update settle = ~67ms @ 60Hz).
    if (mShotIndex < (int)mShotFrames.size()) {
        int target = mShotFrames[mShotIndex];
        // Effective target = max(target, firstSceneCam + 4) once we know it.
        // If firstSceneCam unknown and target is reached, ALSO defer.
        int effectiveTarget = target;
        if (mFirstSceneCamFrame < 0) {
            // Scene cam not seen yet — drop the schedule, retry next frame.
            // Capping at target+200 so a never-arriving cam doesn't lock us
            // out completely.
            if (mFrameCount >= target && mFrameCount < target + 200) {
                mFrameCount++;
                return;
            }
            effectiveTarget = target;
        } else {
            int minTarget = mFirstSceneCamFrame + 4;
            if (target < minTarget) effectiveTarget = minTarget;
        }
        if (mFrameCount == effectiveTarget) {
            int w = mGpu.WindowWidth(), h = mGpu.WindowHeight();
            std::vector<uint8_t> pixels((size_t)w * h * 4);
            if (mGpu.ReadbackHeadlessFrame(pixels.data(), pixels.size())) {
                char path[512];
                const std::string& label = mShotNames[mShotIndex];
                if (!label.empty())
                    snprintf(path, sizeof(path), "%s/%02d_f%04d_%s.png",
                             mShotDir.c_str(), mShotIndex + 1, mFrameCount, label.c_str());
                else
                    snprintf(path, sizeof(path), "%s/%02d_f%04d.png",
                             mShotDir.c_str(), mShotIndex + 1, mFrameCount);
                if (WritePNG(path, pixels.data(), w, h))
                    printf("BandRnd: screenshot %d -> %s (target was %d)\n",
                           mFrameCount, path, target);
                else
                    fprintf(stderr, "BandRnd: WritePNG failed -> %s\n", path);
            } else {
                fprintf(stderr, "BandRnd: readback failed for frame %d "
                        "(MILO_HEADLESS required)\n", mFrameCount);
            }
            mShotIndex++;
        }
    }

    mFrameCount++;
}

void BandRnd::PreInitRender() {
    if (mPreInited) return;
    mPreInited = true;
    // Mirror the rndobj factory registration block in Rnd::PreInit (Rnd.cpp),
    // minus the GPU / overlay / console / TheRnd-singleton parts. These register
    // under the correct OBJ_CLASSNAME and read gSystemConfig — which the boot
    // path (SystemPreInit/SystemInit) has populated by the time we get here.
    RndTransformable::Init();
    RndCam::Init();
    RndMesh::Init();
    RndEnviron::Init();
    RndMat::Init();
    RndTex::Init();
    RndLight::Init();
    RndMultiMesh::Init();
    RndTransformable::Register();
    RndGroup::Init();
    RndDir::Init();

    RB3RegisterLegacyRndAliases();
    printf("BandRnd: rndobj factories registered (Trans/Cam/Mesh/Env/Mat/Tex/Light/MultiMesh/Group/Dir + Tex/Text/Dir aliases)\n");
}

// Name aliases for the legacy milo class names. RndTex/RndText/RndDir register
// under OBJ_CLASSNAME "RndTex"/"RndText"/"RndDir", but RB3's 2010-era on-disc
// milos store the old short names "Tex"/"Text"/"Dir" (the other rndobj classes —
// Mat/Mesh/Group/Cam/Trans/MultiMesh — already use bare OBJ_CLASSNAMEs, so no
// alias is needed there). Register the short Symbols to the same factory so
// NewObject("Tex"/"Text"/"Dir") resolves (otherwise "Can't make Tex"/"Text").
// Called from BOTH the synthetic render harness (PreInitRender) and the real
// game boot (RunGame, where the real Rnd::PreInit registers the RndXxx names but
// not the short aliases). RndXxx::NewObject is a valid static factory regardless
// of registration order, so calling this any time before a milo loads is safe.
void RB3RegisterLegacyRndAliases() {
    Hmx::Object::RegisterFactory(Symbol("Tex"),  RndTex::NewObject);
    Hmx::Object::RegisterFactory(Symbol("Text"), RndText::NewObject);
    Hmx::Object::RegisterFactory(Symbol("Dir"),  RndDir::NewObject);
}

// ===========================================================================
// V1: RndTex -> GPU texture side-table + upload path.
//
// RB3 doesn't link the engine's TextureConvert.cpp (MILO_ENGINE_BUILD_GPU_BACKENDS
// is OFF), so we provide a self-contained bitmap-upload implementation here.
// Supports the formats RB3 cached bitmaps actually use:
//   - DXT1 / DXT3 / DXT5 (Xbox-cached .png_xbox; mOrder & 0x38)
//   - RGBA8 / BGRA8 (bpp=32; mOrder & 1 -> RGBA, else BGRA)
//   - RGB24 (bpp=24; expanded to RGBA8 with alpha=255)
//   - Palette-indexed 8/4 bpp via RndBitmap::PixelColor() decode (slow path)
// CPU-decompresses DXT to RGBA8 to avoid the BC-feature-detection dependency
// — keeps the path identical in headless null-backend CI.
// ===========================================================================
struct RB3TexEntry {
    wgpu::Texture     tex;
    wgpu::TextureView view;
    const uint8_t*    lastPixels = nullptr;  // detect bitmap data churn
    uint32_t          fingerprint = 0;
    bool              uploaded = false;
    // RTT: when isRenderTarget, `tex`/`view` hold a RENDER_ATTACHMENT |
    // TEXTURE_BINDING texture that BandRnd paints into (instead of a
    // CPU-uploaded bitmap). The sky-dome material then samples `view` to read
    // the painted result. `uploaded` is set true once the RT texture exists so
    // GetRB3TexView/MakeMaterialBindGroup bind the painted view.
    bool              isRenderTarget = false;
};
static std::unordered_map<RndTex*, RB3TexEntry> sTexGpu;

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
static std::unordered_map<RndMesh*, RB3MeshEntry> sMeshGpu;

// Per-frame GPU-resource CREATE counter — proves the leak is fixed. Incremented
// at every CreateBuffer in DrawMesh's upload path (and the per-mesh bind-group
// builds), reset in BeginFrame, logged in EndFrame under RENDER_DBG. At steady
// state (no new geometry entering the scene) this drops to ~0.
static int sMeshBufCreatesThisFrame = 0;
static int sMeshBGCreatesThisFrame = 0;

// Monotonic frame sequence. Bumped once per BeginFrame; each mesh entry compares
// its `frameSeen` against this to lazily reset its per-frame uniform-slot index
// (RB3MeshEntry::nextSlot) the first time it is drawn each frame — no map-wide
// sweep needed. (Distinct from BandRnd::mFrameCount, which only advances on
// EndDrawing and is also used by screenshot scheduling; a dedicated global keeps
// the slot logic independent of that.)
static uint64_t sFrameSeq = 0;

// Drop a mesh's cached GPU buffers. Strong def displaces the weak no-op
// link-stub (native: rndobj_synth_link_stubs.s; web: missing_stubs.js). Called
// from RndMesh's HX_NATIVE destructor so freed meshes release their GPU buffers
// instead of leaking the cache slot for the lifetime of the process.
void CleanupGpuMesh(RndMesh* mesh) {
    sMeshGpu.erase(mesh);
}

// Drop a texture's cached GPU resources (twin of CleanupGpuMesh for sTexGpu).
// wgpu::Texture/TextureView are refcounted, so erasing the map entry releases
// them — no explicit .destroy() needed. Strong def displaces the weak no-op
// link-stub (native: rndobj_synth_link_stubs.s). Called from RndTex's HX_NATIVE
// destructor so freed textures release their GPU memory and don't leave a stale
// sTexGpu slot that a recycled RndTex* could later resurrect.
void CleanupGpuTex(RndTex* tex) {
    sTexGpu.erase(tex);
}

static uint32_t TexFingerprint(const uint8_t* p, int sz) {
    if (!p || sz < 16) return 0;
    uint32_t h = 0; int step = sz / 8; if (step < 1) step = 1;
    for (int i = 0; i < sz; i += step) h = h * 31u + p[i];
    return h;
}

static void DecodeRGB565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = ((c >> 11) & 0x1F) * 255 / 31;
    g = ((c >> 5)  & 0x3F) * 255 / 63;
    b = ((c)       & 0x1F) * 255 / 31;
}
static void DecompressDXT1Block(const uint8_t* block, uint8_t* dst, int dstStride) {
    uint16_t c0 = block[0] | (block[1] << 8);
    uint16_t c1 = block[2] | (block[3] << 8);
    uint8_t r[4], g[4], b[4], a[4];
    DecodeRGB565(c0, r[0], g[0], b[0]); a[0] = 255;
    DecodeRGB565(c1, r[1], g[1], b[1]); a[1] = 255;
    if (c0 > c1) {
        r[2] = (2*r[0]+r[1])/3; g[2] = (2*g[0]+g[1])/3; b[2] = (2*b[0]+b[1])/3; a[2] = 255;
        r[3] = (r[0]+2*r[1])/3; g[3] = (g[0]+2*g[1])/3; b[3] = (b[0]+2*b[1])/3; a[3] = 255;
    } else {
        r[2] = (r[0]+r[1])/2; g[2] = (g[0]+g[1])/2; b[2] = (b[0]+b[1])/2; a[2] = 255;
        r[3] = 0; g[3] = 0; b[3] = 0; a[3] = 0;
    }
    uint32_t indices = block[4] | (block[5] << 8) | (block[6] << 16) | (block[7] << 24);
    for (int py = 0; py < 4; py++) for (int px = 0; px < 4; px++) {
        int idx = (indices >> ((py*4+px)*2)) & 3;
        uint8_t* out = dst + py*dstStride + px*4;
        out[0]=r[idx]; out[1]=g[idx]; out[2]=b[idx]; out[3]=a[idx];
    }
}
static void DecompressDXT1(const uint8_t* src, uint8_t* dst, int w, int h) {
    int bw = (w+3)/4, bh = (h+3)/4, ds = w*4;
    for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
        uint8_t blk[64];
        DecompressDXT1Block(src, blk, 16); src += 8;
        for (int py = 0; py < 4 && by*4+py < h; py++)
            for (int px = 0; px < 4 && bx*4+px < w; px++) {
                int di = ((by*4+py)*w + (bx*4+px))*4;
                int si = (py*4+px)*4;
                std::memcpy(&dst[di], &blk[si], 4);
            }
    }
    (void)ds;
}
static void DecompressDXT3(const uint8_t* src, uint8_t* dst, int w, int h) {
    int bw = (w+3)/4, bh = (h+3)/4;
    for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
        const uint8_t* a = src; const uint8_t* c = src + 8; src += 16;
        uint8_t blk[64];
        DecompressDXT1Block(c, blk, 16);
        for (int py = 0; py < 4; py++) for (int px = 0; px < 4; px++) {
            int ai = py*4 + px;
            uint8_t ab = a[ai/2];
            uint8_t a4 = (ai & 1) ? (ab >> 4) : (ab & 0xF);
            blk[(py*4+px)*4 + 3] = (a4 << 4) | a4;
        }
        for (int py = 0; py < 4 && by*4+py < h; py++)
            for (int px = 0; px < 4 && bx*4+px < w; px++) {
                int di = ((by*4+py)*w + (bx*4+px))*4;
                int si = (py*4+px)*4;
                std::memcpy(&dst[di], &blk[si], 4);
            }
    }
}
static void DecompressDXT5AlphaBlock(const uint8_t* block, uint8_t alphas[4][4]) {
    uint8_t a0 = block[0], a1 = block[1]; uint8_t p[8];
    p[0]=a0; p[1]=a1;
    if (a0 > a1) {
        p[2]=(6*a0+1*a1)/7; p[3]=(5*a0+2*a1)/7; p[4]=(4*a0+3*a1)/7;
        p[5]=(3*a0+4*a1)/7; p[6]=(2*a0+5*a1)/7; p[7]=(1*a0+6*a1)/7;
    } else {
        p[2]=(4*a0+1*a1)/5; p[3]=(3*a0+2*a1)/5; p[4]=(2*a0+3*a1)/5;
        p[5]=(1*a0+4*a1)/5; p[6]=0; p[7]=255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)block[2+i] << (i*8);
    for (int py = 0; py < 4; py++) for (int px = 0; px < 4; px++) {
        int idx = (bits >> ((py*4+px)*3)) & 7;
        alphas[py][px] = p[idx];
    }
}
static void DecompressDXT5(const uint8_t* src, uint8_t* dst, int w, int h) {
    int bw = (w+3)/4, bh = (h+3)/4;
    for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
        const uint8_t* aBlk = src; const uint8_t* cBlk = src + 8; src += 16;
        uint8_t alphas[4][4]; DecompressDXT5AlphaBlock(aBlk, alphas);
        uint8_t blk[64];
        DecompressDXT1Block(cBlk, blk, 16);
        for (int py = 0; py < 4; py++) for (int px = 0; px < 4; px++)
            blk[(py*4+px)*4 + 3] = alphas[py][px];
        for (int py = 0; py < 4 && by*4+py < h; py++)
            for (int px = 0; px < 4 && bx*4+px < w; px++) {
                int di = ((by*4+py)*w + (bx*4+px))*4;
                int si = (py*4+px)*4;
                std::memcpy(&dst[di], &blk[si], 4);
            }
    }
}

// Xbox 360 caches store DXT block data as big-endian 16-bit words: swap pairs.
static void ByteSwapDXT16(uint8_t* data, size_t size) {
    for (size_t i = 0; i + 1 < size; i += 2) {
        uint8_t t = data[i]; data[i] = data[i+1]; data[i+1] = t;
    }
}

// Returns the GPU view for an RndTex, uploading the bitmap on first access (and
// re-uploading when the underlying pixel pointer changes). Returns an empty
// view on any failure — caller falls back to mWhiteView.
static wgpu::TextureView UploadRndTexIfNeeded(GpuDevice& gpu, RndTex* tex) {
    if (!tex) return {};
    // RTT: a render-target entry has no CPU bitmap pixels — its texture is
    // painted by BandRnd::BeginDrawTarget. Return its RT view (if created) and
    // skip the pixel-upload path entirely.
    {
        auto it = sTexGpu.find(tex);
        if (it != sTexGpu.end() && it->second.isRenderTarget)
            return it->second.view;
    }
    const RndBitmap& bmp = tex->mBitmap;
    int w = bmp.Width(), h = bmp.Height();
    int bpp = bmp.Bpp();
    const uint8_t* pixels = bmp.Pixels();
    if (w <= 0 || h <= 0 || bpp <= 0 || !pixels) return {};

    RB3TexEntry& e = sTexGpu[tex];
    int pixBytes = bmp.PixelBytes();
    uint32_t fp = TexFingerprint(pixels, pixBytes);
    if (e.uploaded && e.lastPixels == pixels && e.fingerprint == fp) {
        return e.view;
    }

    // Frame-trace: a cache MISS pays the DXT->RGBA8 CPU decode + GPU WriteTexture
    // burst (the first-draw stall class). Hits returned above and never reach
    // here, so this charges only real upload work to gTexUploadMsThisFrame.
    double ftStart = gFrameTraceActive ? FrameTraceNowMs() : 0.0;

    unsigned int order = bmp.Order();
    unsigned int dxt = order & 0x38;

    // Q4 — BC-native texture upload. When the device supports BC (texture-
    // compression-bc, requested at device creation on both native + web), upload
    // the DXT blocks directly instead of CPU-decompressing every DXT->RGBA8 at
    // first draw (the ~600 ms boot-profile burst + 4-8x more upload bytes).
    // BC1/BC2/BC3 IS the source data, so this is lossless (no quality change).
    // DXN/BC5 deliberately stays on the RGBA8 CPU path below: BC5RGUnorm returns
    // (R,G,0,1) which the DXT5nm/RGB normal-decode shader heuristic reads as a
    // flipped Z; the CPU decompress sets B=255 for correct Z=+1 (matches
    // TextureConvert::MapBitmapFormat's documented DXN carve-out).
    // Opt-out: RB3_BC_TEX_OFF=1 restores the full CPU-decompress path.
    static int sBcTexEnabled = -1;
    if (sBcTexEnabled < 0) {
        const char* e = getenv("RB3_BC_TEX_OFF");
        sBcTexEnabled = (e && e[0] && e[0] != '0') ? 0 : 1;
    }
    bool isBlockDxt = (dxt == 0x08 || dxt == 0x10 || dxt == 0x18); // DXT1/3/5
    if (sBcTexEnabled && isBlockDxt && gpu.HasBCCompression()) {
        // Block geometry: 8 bytes/block for DXT1(BC1), 16 for DXT3/5(BC2/BC3).
        int blockBytes = (dxt == 0x08) ? 8 : 16;
        int blockW = (w + 3) / 4;            // blocks per row (covers padded width)
        int blockH = (h + 3) / 4;            // block rows (covers padded height)
        uint32_t bytesPerRow = (uint32_t)(blockW * blockBytes);
        size_t uploadSize = (size_t)bytesPerRow * blockH;

        // Copy + endian-swap the BC blocks (Xbox stores them as BE 16-bit words).
        // Mirror the RGBA8 path's source size (PixelBytes()); clamp the upload to
        // whichever is smaller so a short source can't over-read the GPU copy.
        std::vector<uint8_t> work(pixels, pixels + pixBytes);
        ByteSwapDXT16(work.data(), work.size());
        if (uploadSize > (size_t)pixBytes) uploadSize = (size_t)pixBytes;

        wgpu::TextureFormat bcFmt =
            (dxt == 0x08) ? wgpu::TextureFormat::BC1RGBAUnorm :
            (dxt == 0x10) ? wgpu::TextureFormat::BC2RGBAUnorm :
                            wgpu::TextureFormat::BC3RGBAUnorm;

        wgpu::TextureDescriptor td{};
        td.label = "RB3TexBC";
        td.size = {(uint32_t)w, (uint32_t)h, 1};
        td.format = bcFmt;
        td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
        td.mipLevelCount = 1; // RB3 native bitmaps are single-level (numMips==0)
        wgpu::Texture t = gpu.Device().CreateTexture(&td);
        if (t) {
            // mipLevel 0: copy width/height == the physical mip size, so they
            // satisfy the BC block-multiple constraint even when w/h aren't /4.
            // bytesPerRow is a multiple of blockBytes (NOT rounded to 256 — that
            // 256 alignment is a COPY_BYTES_PER_ROW_ALIGNMENT rule for buffer->
            // texture command-encoder copies; Queue().WriteTexture relaxes it).
            wgpu::TexelCopyTextureInfo dstInfo{}; dstInfo.texture = t;
            wgpu::TexelCopyBufferLayout layout{};
            layout.bytesPerRow = bytesPerRow;
            layout.rowsPerImage = (uint32_t)blockH;
            wgpu::Extent3D ext = {(uint32_t)w, (uint32_t)h, 1};
            gpu.Queue().WriteTexture(&dstInfo, work.data(), uploadSize, &layout, &ext);

            e.tex = t;
            e.view = t.CreateView();
            e.lastPixels = pixels;
            e.fingerprint = fp;
            e.uploaded = true;
            if (gFrameTraceActive) {
                gTexUploadMsThisFrame += (float)(FrameTraceNowMs() - ftStart);
                gTexUploadCountThisFrame++;
            }
            return e.view;
        }
        // Texture creation failed — fall through to the RGBA8 CPU path.
    }

    // Choose format: always RGBA8Unorm (CPU-decompress DXT). Simple, portable,
    // works on the null backend used in headless CI, and the BC-unsupported /
    // DXN / RB3_BC_TEX_OFF fallback.
    wgpu::TextureFormat fmt = wgpu::TextureFormat::RGBA8Unorm;
    std::vector<uint8_t> rgba((size_t)w * h * 4, 0xFF);
    uint8_t* dst = rgba.data();

    if (dxt) {
        // Copy + endian-swap before decompress.
        std::vector<uint8_t> work(pixels, pixels + pixBytes);
        ByteSwapDXT16(work.data(), work.size());
        switch (dxt) {
            case 0x08: DecompressDXT1(work.data(), dst, w, h); break;
            case 0x10: DecompressDXT3(work.data(), dst, w, h); break;
            case 0x18: DecompressDXT5(work.data(), dst, w, h); break;
            default:
                // Unsupported DXT variant — leave as opaque white.
                break;
        }
    } else if (bpp == 32) {
        std::memcpy(dst, pixels, (size_t)w * h * 4);
        if (!(order & 1)) {
            for (int i = 0; i < w * h; i++) {
                uint8_t* p = dst + i*4;
                uint8_t t = p[0]; p[0] = p[2]; p[2] = t;
            }
        }
    } else if (bpp == 24) {
        for (int i = 0; i < w * h; i++) {
            if (order & 1) {
                dst[i*4+0] = pixels[i*3+0];
                dst[i*4+1] = pixels[i*3+1];
                dst[i*4+2] = pixels[i*3+2];
            } else {
                dst[i*4+0] = pixels[i*3+2];
                dst[i*4+1] = pixels[i*3+1];
                dst[i*4+2] = pixels[i*3+0];
            }
            dst[i*4+3] = 0xFF;
        }
    } else if (bpp == 8 || bpp == 4) {
        // Palette-indexed — RndBitmap::PixelColor handles the lookup.
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            uint8_t r, g, b, a;
            bmp.PixelColor(x, y, r, g, b, a);
            int i = (y * w + x) * 4;
            dst[i+0] = r; dst[i+1] = g; dst[i+2] = b; dst[i+3] = a;
        }
    } else {
        return {};
    }

    wgpu::TextureDescriptor td{};
    td.label = "RB3Tex";
    td.size = {(uint32_t)w, (uint32_t)h, 1};
    td.format = fmt;
    td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    td.mipLevelCount = 1;
    wgpu::Texture t = gpu.Device().CreateTexture(&td);
    if (!t) return {};

    wgpu::TexelCopyTextureInfo dstInfo{}; dstInfo.texture = t;
    wgpu::TexelCopyBufferLayout layout{}; layout.bytesPerRow = (uint32_t)(w * 4);
    wgpu::Extent3D ext = {(uint32_t)w, (uint32_t)h, 1};
    gpu.Queue().WriteTexture(&dstInfo, rgba.data(), rgba.size(), &layout, &ext);

    e.tex = t;
    e.view = t.CreateView();
    e.lastPixels = pixels;
    e.fingerprint = fp;
    e.uploaded = true;
    if (gFrameTraceActive) {
        gTexUploadMsThisFrame += (float)(FrameTraceNowMs() - ftStart);
        gTexUploadCountThisFrame++;
    }
    return e.view;
}

// Public accessor — used by MakeMaterialBindGroup to bind a material's
// diffuse texture. Returns an empty view if not yet uploaded.
static wgpu::TextureView GetRB3TexView(RndTex* tex) {
    if (!tex) return {};
    auto it = sTexGpu.find(tex);
    if (it != sTexGpu.end() && it->second.uploaded) return it->second.view;
    return {};
}

static wgpu::Texture MakeSolid(GpuDevice& gpu, wgpu::TextureFormat fmt,
                               uint8_t r, uint8_t g, uint8_t b, uint8_t a, const char* label) {
    wgpu::TextureDescriptor d{};
    d.label = label; d.size = {1, 1, 1}; d.format = fmt;
    d.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    d.mipLevelCount = 1;
    wgpu::Texture t = gpu.Device().CreateTexture(&d);
    uint8_t px[4] = {r, g, b, a};
    wgpu::TexelCopyTextureInfo dst{}; dst.texture = t;
    wgpu::TexelCopyBufferLayout lay{}; lay.bytesPerRow = 4; lay.rowsPerImage = 1;
    wgpu::Extent3D ext = {1, 1, 1};
    gpu.Queue().WriteTexture(&dst, px, 4, &lay, &ext);
    return t;
}

void BandRnd::CreateDefaultTextures() {
    mWhiteTex = MakeSolid(mGpu, mTargetFmt, 255, 255, 255, 255, "White");
    mBlackTex = MakeSolid(mGpu, wgpu::TextureFormat::RGBA8Unorm, 0, 0, 0, 255, "Black");
    mFlatNormalTex = MakeSolid(mGpu, wgpu::TextureFormat::RGBA8Unorm, 128, 128, 255, 255, "FlatNormal");
    mWhiteView = mWhiteTex.CreateView();
    mBlackView = mBlackTex.CreateView();
    mFlatNormalView = mFlatNormalTex.CreateView();

    {
        wgpu::TextureDescriptor cd{};
        cd.label = "BlackCube"; cd.size = {1, 1, 6};
        cd.format = wgpu::TextureFormat::RGBA8Unorm;
        cd.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
        cd.mipLevelCount = 1;
        mBlackCubeTex = mGpu.Device().CreateTexture(&cd);
        uint8_t px[4] = {0, 0, 0, 255};
        for (uint32_t f = 0; f < 6; f++) {
            wgpu::TexelCopyTextureInfo dst{}; dst.texture = mBlackCubeTex; dst.origin = {0, 0, f};
            wgpu::TexelCopyBufferLayout lay{}; lay.bytesPerRow = 4; lay.rowsPerImage = 1;
            wgpu::Extent3D ext = {1, 1, 1};
            mGpu.Queue().WriteTexture(&dst, px, 4, &lay, &ext);
        }
        wgpu::TextureViewDescriptor vd{}; vd.dimension = wgpu::TextureViewDimension::Cube; vd.arrayLayerCount = 6;
        mBlackCubeView = mBlackCubeTex.CreateView(&vd);
    }

    wgpu::SamplerDescriptor sd{};
    sd.magFilter = wgpu::FilterMode::Linear; sd.minFilter = wgpu::FilterMode::Linear;
    sd.addressModeU = wgpu::AddressMode::Repeat; sd.addressModeV = wgpu::AddressMode::Repeat;
    mSampler = mGpu.Device().CreateSampler(&sd);

    {
        wgpu::TextureDescriptor td{};
        td.label = "ShadowDummy"; td.size = {1, 1, 1};
        td.format = wgpu::TextureFormat::Depth24Plus;
        td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::RenderAttachment;
        td.mipLevelCount = 1;
        mShadowTex = mGpu.Device().CreateTexture(&td);
        mShadowView = mShadowTex.CreateView();
    }
    wgpu::SamplerDescriptor cs{}; cs.compare = wgpu::CompareFunction::LessEqual;
    mShadowSampler = mGpu.Device().CreateSampler(&cs);
}

// Start the async/sync GpuDevice bring-up. Returns true once the device
// request has been DISPATCHED — on native (Dawn) the device is ready
// synchronously when this returns; on web (emdawnwebgpu) the adapter/device
// request is async and mGpu.IsReady() stays false until the JS callbacks fire.
// Callers must wait on mGpu.IsReady() (polling PollEvents) before
// InitGpuResources(). Does NOT touch any GPU resources (pipelines/textures).
bool BandRnd::StartGpuInit(int width, int height, bool headless) {
    if (mGpuReady) return true;
    // W8: pull NativeSettings camera-knob seeds out of the environment so the
    // RB3 backend respects MILO_CAM_FOV_SCALE / MILO_CAM_NEAR / etc the same
    // way DC3's Rnd_Wgpu.cpp does. Without this Init() the engine struct stays
    // at its compile-time defaults regardless of env, and the FOV knob added
    // below in WriteSceneUniforms can't be tuned without a rebuild.
    NativeSettings::Get().Init();
    GpuDeviceDesc desc{};
    desc.headless = headless;
    desc.width = width;
    desc.height = height;
    desc.title = "rb3-native BandRnd";
    if (!mGpu.Init(desc)) {
        fprintf(stderr, "BandRnd: GpuDevice init dispatch FAILED\n");
        return false;
    }
    return true;
}

// Create the pipelines / depth tex / 4 uniform rings / default textures and
// latch mGpuReady. MUST be called only after mGpu.IsReady() is true (the
// device + surface exist). Idempotent: re-entry is a no-op once mGpuReady.
void BandRnd::InitGpuResources() {
    if (mGpuReady) return;

    // Pick the render-target format the pipelines must emit into. On web the
    // canvas surface format is whatever the browser preferred (Chrome → BGRA8),
    // queried by GpuDevice in the async device callback; the color attachment
    // (AcquireNextFrame) is in that format, so the pipeline targetFormat must
    // match or we hit "attachment format != pipeline" validation + swapped
    // colors. On native HEADLESS the readback target is forced RGBA8Unorm
    // (GpuDevice::AcquireHeadlessFrame), and mGpu.SurfaceFormat() is still the
    // BGRA8 default at this point (headless never calls ConfigureSurface), so
    // keep RGBA8 for headless to preserve the unchanged native PNG output.
    mTargetFmt = mGpu.IsHeadless() ? wgpu::TextureFormat::RGBA8Unorm
                                   : mGpu.SurfaceFormat();

    mPipelines.Init(&mGpu);

    const int W = mGpu.WindowWidth(), H = mGpu.WindowHeight();
    EnsureDepth(W, H);

    // 64KB = ~85 scene-uniform slots (768B aligned each). The per-environ
    // re-write (venue lighting) makes a busy world.cam frame do ~24 scene writes
    // (vs ~3-15 typical); the ring wraps to offset 0 at capacity, and since each
    // bind group pins a fixed offset, >slots writes/frame would clobber an
    // earlier draw's uniforms mid-frame (all queue writes land before submit).
    // 85 slots >> the measured 24 worst-case keeps every frame's offsets
    // distinct with wide margin for busier venues. (Was 16KB = ~21 slots.)
    mSceneRing.Init(mGpu.Device(), 64 * 1024, "SceneUBO");
    mMaterialRing.Init(mGpu.Device(), 256 * 1024, "MaterialUBO");
    mObjectRing.Init(mGpu.Device(), 256 * 1024, "ObjectUBO");
    mBoneRing.Init(mGpu.Device(), 256 * 1024, "BoneUBO");

    CreateDefaultTextures();

    // V2 bloom: grab the default sampler (pipelines/textures are lazily built on
    // first BloomPass::Run, sized to the scene). Cheap; safe to Init unconditionally.
    mBloom.Init(mGpu);
    // P1 highway bloom: its own BloomPass instance + own mip chain. Init() is
    // MANDATORY even though pipelines/textures build lazily on first Run() —
    // BloomPass::Run does NOT build mDefaultSampler (only Init() does); a null
    // sampler yields an invalid command buffer that discards the whole frame.
    // mDefaultSampler is per-instance, so mBloom.Init() does not cover this one.
    mHaloBloom.Init(mGpu);

    mGpuReady = true;
    printf("BandRnd: GPU ready (%dx%d, fmt=%d, %s)\n", W, H, (int)mTargetFmt,
           mGpu.IsHeadless() ? "headless" : "windowed");
}

// Monolithic init — used by native callers (main_native.cpp, rb3_render_mesh.cpp).
// On native Dawn the device is ready synchronously after StartGpuInit, so this
// stays a single blocking call. The web boot machine instead drives
// StartGpuInit / poll-IsReady / InitGpuResources across separate frames.
bool BandRnd::InitGpu(int width, int height, bool headless) {
    if (mGpuReady) return true;
    if (!StartGpuInit(width, height, headless)) return false;
    if (!mGpu.IsReady()) {
        fprintf(stderr, "BandRnd: GpuDevice not ready after sync init\n");
        return false;
    }
    InitGpuResources();
    return mGpuReady;
}

// Tear down all WebGPU handles owned by gBandRnd. Must run BEFORE libc's static
// destructor phase — the Vulkan ICD's shared lib gets unmapped on exit() and any
// wgpu::~ObjectBase that drops the last ref calls into Dawn → vkDestroy*, which
// then jumps to a freed VMA (faulting at a libvulkan ICD address). Registered as
// a TheDebug exit callback (see BandRndShutdownExitCallback in rb3_band_rnd.cpp,
// installed from main_native's RunGame after InitGpu) so it runs from within
// Debug::Exit, ahead of exit(0).
void BandRnd::Shutdown() {
    if (!mGpuReady) return;

    // File-static GPU caches that hold wgpu::Texture / wgpu::TextureView /
    // wgpu::Buffer / wgpu::BindGroup refs. They would otherwise destruct during
    // libc's static-destructor phase (after the Vulkan ICD .so is unmapped) and
    // drop their last refs there, leading to dangling vkDestroy* calls. Drop them
    // while Dawn is still alive.
    sTexGpu.clear();
    sMeshGpu.clear();

    // Release uniform-ring buffers (refs on wgpu::Buffer / wgpu::Device).
    mSceneRing.Release();
    mMaterialRing.Release();
    mObjectRing.Release();
    mBoneRing.Release();

    // Per-frame scratch handles.
    mPass = nullptr;
    mEncoder = nullptr;
    mFrameView = nullptr;
    mSceneBindGroup = nullptr;
    mInPass = false;
    mRtActiveTex = nullptr;

    // Default textures + samplers + views.
    mShadowSampler = nullptr;
    mSampler = nullptr;
    mShadowView = nullptr;
    mBlackCubeView = nullptr;
    mFlatNormalView = nullptr;
    mBlackView = nullptr;
    mWhiteView = nullptr;
    mShadowTex = nullptr;
    mBlackCubeTex = nullptr;
    mFlatNormalTex = nullptr;
    mBlackTex = nullptr;
    mWhiteTex = nullptr;

    // Depth attachments.
    mDepthView = nullptr;
    mDepthTex = nullptr;

    // Shared 2D quad pipeline infra (DrawRect / Stage-2 composite). Drop the
    // cached pipelines first, then the shader/layouts/buffers.
    mQuadPipelines.clear();
    mQuadShader = nullptr;
    mQuadRectBGL = nullptr;
    mQuadRectPL = nullptr;
    mQuadVertexBuffer = nullptr;
    mRectUB = nullptr;
    mQuadReady = false;

    // P1 highway-halo bloom infra (drop refs before Dawn teardown).
    mHaloDraws.clear();
    mHaloBloom.Terminate();
    mHaloAddPipeline = nullptr;
    mHaloBlitPL = nullptr;
    mHaloBlitBGL = nullptr;
    mHaloBlitShader = nullptr;
    mHaloView = nullptr;
    mHaloTex = nullptr;
    mHaloWidth = 0;
    mHaloHeight = 0;
    mHaloBlitReady = false;

    // Stage-2 postproc composite infra.
    mBloom.Terminate();
    mQuadPostPipeline = nullptr;
    mQuadPostShader = nullptr;
    mQuadPostBGL = nullptr;
    mQuadPostPL = nullptr;
    mPostProcUB = nullptr;
    mIntermediateView = nullptr;
    mIntermediateTex = nullptr;
    mIntermediateWidth = 0;
    mIntermediateHeight = 0;
    mPostProcFlushed = false;

    // Pipeline manager: drops the pipeline+shader caches + layouts.
    mPipelines.Terminate();

    // Finally tear down the GpuDevice (releases device/adapter/instance/surface
    // in safe order, terminates GLFW).
    mGpu.Shutdown();

    mGpuReady = false;
    printf("BandRnd: Shutdown complete\n");
}

static void BandRndShutdownExitCallback() {
    gBandRnd.Shutdown();
}

void RB3RegisterBandRndShutdown() {
    TheDebug.AddExitCallback(BandRndShutdownExitCallback);
}

// Venue colour/point lighting (P4): read the venue's per-environ RndEnviron
// lights into the scene uniforms under world.cam (the venue scopes ~20 environs
// to mesh groups per frame; see the per-environ re-write in DrawMesh). DEFAULT-ON
// — opt out via RB3_VENUE_LIGHT_OFF=1 (mirrors RB3_TRACK_LIGHT_OFF). This drives
// the moody, coloured, stage-lit venue look (matching retail's dark backdrop so
// the highway pops) instead of the old flat one-white-directional + 0.45-grey
// flood. Used by both WriteSceneUniforms (gate the read) and DrawMesh (gate the
// per-environ re-write), so they stay in lock-step. game.cam (the gameplay
// highway) and cams that aren't world.cam are byte-identical to before.
static bool sVenueLightEnabled() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("RB3_VENUE_LIGHT_OFF"); v = (e && e[0] && e[0] != '0') ? 0 : 1; }
    return v != 0;
}

void BandRnd::WriteSceneUniforms(RndCam* cam) {
    SceneUniforms s{};

    float viewProj[16];
    float camPos[3] = {0, 0, 0};

    if (getenv("RB3_RENDER_DBG")) fprintf(stderr, "[dbg] WriteSceneUniforms cam=%p\n", (void*)cam);
    if (getenv("RB3_LIGHT_PROBE") && cam) {
        fprintf(stderr, "[LIGHT_PROBE] WriteSceneUniforms cam='%s' near=%.1f far=%.1f\n",
                cam->Name() ? cam->Name() : "<noname>", cam->NearPlane(), cam->FarPlane());
    }
    if (cam) {
        cam->UpdateLocal();          // refresh mLocalProjectXfm + mWorldProjectXfm
        const Transform& world = cam->WorldXfm();
        camPos[0] = world.v.x; camPos[1] = world.v.y; camPos[2] = world.v.z;

        // View matrix: world -> camera-local. Build it DIRECTLY from the camera's
        // world-space basis (rows of WorldXfm.m: x=right, y=forward/depth, z=up)
        // and eye (WorldXfm.v), so camera-local coords are
        //   x' = dot(p-eye, right), y' = dot(p-eye, fwd), z' = dot(p-eye, up).
        // y' (depth) is then POSITIVE for points in front of the camera. (Going
        // through RB3's mInvWorldXfm + a transpose interpretation flipped the
        // depth sign, projecting everything behind the camera — clip.w < 0.)
        const Vector3& right = world.m.x;
        const Vector3& fwd   = world.m.y;
        const Vector3& up    = world.m.z;
        const Vector3& eye   = world.v;
        float view[16] = {0};
        // Column-major: column j holds the world-axis component for output row.
        // out.x' = right·p - right·eye, etc.
        view[0]  = right.x; view[4]  = right.y; view[8]  = right.z; view[12] = -(right.x*eye.x + right.y*eye.y + right.z*eye.z);
        view[1]  = fwd.x;   view[5]  = fwd.y;   view[9]  = fwd.z;   view[13] = -(fwd.x*eye.x   + fwd.y*eye.y   + fwd.z*eye.z);
        view[2]  = up.x;    view[6]  = up.y;    view[10] = up.z;    view[14] = -(up.x*eye.x    + up.y*eye.y    + up.z*eye.z);
        view[3]  = 0;       view[7]  = 0;       view[11] = 0;       view[15] = 1.0f;

        // Perspective: Milo camera-local axes are X=right, Y=forward(depth),
        // Z=up. Build a column-major clip matrix mapping that to WebGPU clip
        // (x right, y up, z in [0,1]) from YFov / aspect / near / far.
        float yfov = cam->YFov();
        if (yfov <= 0.0001f) yfov = 0.9f;
        float n = cam->NearPlane() > 0 ? cam->NearPlane() : 0.1f;
        float f = cam->FarPlane()  > n ? cam->FarPlane()  : (n + 1000.0f);
        float aspect = (float)mGpu.WindowWidth() / (float)mGpu.WindowHeight();

        // NativeSettings::fovScale — runtime knob to scale the effective FOV.
        // fovScale > 1.0 narrows the FOV (zoom in, things get bigger);
        // fovScale < 1.0 widens it (zoom out, things get smaller). Default 1.0
        // (no-op). Defined in include/platform/NativeSettings.h, exposed via the
        // HTTP settings endpoint and the ImGui DebugPanel. Was previously dead;
        // wired here as part of the W8 investigation into menu-UI vertical
        // sizing, so tooling can A/B FOV from the browser without rebuild.
        // Applies as: tanHalf_effective = tanHalf / fovScale → sy = fovScale/tanHalf.
        float fovScale = NativeSettings::Get().fovScale;
        if (fovScale < 0.01f) fovScale = 1.0f;  // safety clamp

        float tanHalf = tanf(yfov * 0.5f);
        float sy = fovScale / tanHalf;    // vertical scale (FOV-scaled)
        float sx = sy / aspect;           // horizontal scale (keeps aspect)

        // Column-major perspective P. Camera-local p=(x,y,z): x=right, y=depth, z=up.
        //   clip.x =  sx * x
        //   clip.y =  sy * z          (Milo Z-up -> clip Y-up)
        //   clip.z =  f/(f-n) * (y - n)   (so z in [0,1], y=n -> 0, y=f -> 1)
        //   clip.w =  y               (perspective divide by depth)
        float P[16] = {0};
        // column 0 (multiplies x): contributes to clip.x
        P[0] = sx;
        // column 1 (multiplies y): contributes to clip.z and clip.w
        P[1 * 4 + 2] = f / (f - n);
        P[1 * 4 + 3] = 1.0f;
        // column 2 (multiplies z): contributes to clip.y
        P[2 * 4 + 1] = sy;
        // column 3 (constant / w=1): clip.z offset
        P[3 * 4 + 2] = -(f * n) / (f - n);

        MatMul4(P, view, viewProj);
        std::memcpy(s.view, view, sizeof(view));
    } else {
        for (int i = 0; i < 16; i++) viewProj[i] = (i % 5 == 0) ? 1.f : 0.f;
        std::memcpy(s.view, viewProj, sizeof(viewProj));
    }

    std::memcpy(s.viewProj, viewProj, sizeof(viewProj));
    s.cameraPos[0] = camPos[0]; s.cameraPos[1] = camPos[1]; s.cameraPos[2] = camPos[2];

    // Lighting. Default (non-venue cams): one white directional + 0.45 grey ambient
    // so unlit materials read on every screen. VENUE PATH (world.cam, DEFAULT-ON):
    // read the CURRENT RndEnviron's real lights — directional (type 1) + the POINT
    // lights that actually illuminate each group (the *_silhouette.lit spots, the
    // coloured theater/street/foreground accents) — so the backdrop gets the moody,
    // coloured, stage-lit look that matches retail (dark venue so the highway pops)
    // instead of a flat grey flood. The venue scopes ~20 environs to mesh groups
    // per world.cam frame; DrawMesh re-writes these uniforms on each environ change
    // (RndEnviron::sCurrent), so the coloured lights land on their own group rather
    // than one environ lighting the whole venue. The shader implements point lights
    // (computePointLight range falloff). Iterate mLightsApprox ONLY (never
    // ObjDirItr<RndLight> — WASM hang); guard owner ptrs (AmbientColor()/GetColor()
    // deref them). Scoped to world.cam so game.cam (the Track-A highway look) and
    // menu cams are byte-identical to the old single-directional default. Opt out
    // via RB3_VENUE_LIGHT_OFF=1 (see sVenueLightEnabled). Verified clean across
    // gameplay (clear win), song-select (unchanged) and the menu hub (moodier);
    // ring-buffer sized for the extra per-environ writes (see InitGpuResources).
    const char* camNm = cam ? cam->Name() : nullptr;
    RndEnviron* venv = RndEnviron::sCurrent;
    if (sVenueLightEnabled() && camNm && std::strcmp(camNm, "world.cam") == 0 && venv && venv->mAmbientFogOwner) {
        // Ambient: a near-white ambient is the engine's degenerate default (not an
        // authored flood), so pull it down — the point/dir lights provide the real
        // illumination and a low ambient keeps the dark-venue contrast. Floor so
        // nothing crushes to pure black.
        const Hmx::Color& amb = venv->AmbientColor();
        float ar = amb.red, ag = amb.green, ab = amb.blue;
        if (std::max(ar, std::max(ag, ab)) > 0.85f) { ar *= 0.25f; ag *= 0.25f; ab *= 0.25f; }
        s.ambientColor[0] = std::max(ar, 0.07f);
        s.ambientColor[1] = std::max(ag, 0.07f);
        s.ambientColor[2] = std::max(ab, 0.07f);
        s.ambientColor[3] = 1.0f;
        // RB3_VENUE_PROBE: dump what each DISTINCT venue RndEnviron contains (once
        // per env name), so tuning is grounded in real light data not static
        // analysis. With per-environ re-writes this fires for every env group.
        static int sVenueProbe = -1;
        if (sVenueProbe < 0) { const char* e = getenv("RB3_VENUE_PROBE"); sVenueProbe = (e && e[0] && e[0] != '0') ? 1 : 0; }
        static std::unordered_map<std::string,int> sProbedEnvs;
        const char* envNm = venv->Name() ? venv->Name() : "<noname>";
        bool probe = sVenueProbe && (sProbedEnvs[envNm]++ == 0);
        if (probe) {
            int total = 0, showing = 0;
            for (ObjPtrList<RndLight>::iterator pit = venv->mLightsApprox.begin();
                 pit != venv->mLightsApprox.end(); ++pit) { total++; if (*pit && (*pit)->Showing()) showing++; }
            fprintf(stderr, "[VENUE_PROBE] env=%s ambRaw=(%.2f,%.2f,%.2f) ambAdj=(%.2f,%.2f,%.2f) numApprox=%d showing=%d\n",
                    venv->Name() ? venv->Name() : "<noname>",
                    amb.red, amb.green, amb.blue, s.ambientColor[0], s.ambientColor[1], s.ambientColor[2], total, showing);
        }
        int dl = 0, pl = 0;
        for (ObjPtrList<RndLight>::iterator it = venv->mLightsApprox.begin();
             it != venv->mLightsApprox.end() && (dl < 4 || pl < 4); ++it) {
            RndLight* L = *it;
            if (!L || !L->mColorOwner || !L->Showing()) continue;
            const Hmx::Color& lc = L->GetColor();
            if (probe) {
                const Vector3& lp = L->WorldXfm().v;
                fprintf(stderr, "[VENUE_PROBE]   light '%s' type=%d color=(%.2f,%.2f,%.2f) range=%.1f pos=(%.1f,%.1f,%.1f)\n",
                        L->Name() ? L->Name() : "<noname>", (int)L->GetType(),
                        lc.red, lc.green, lc.blue, L->Range(), lp.x, lp.y, lp.z);
            }
            if (lc.red + lc.green + lc.blue <= 0.01f) continue;     // skip off/black lights
            int ty = (int)L->GetType();                            // 0=point, 1=directional, 2=spot
            if (ty == 1 && dl < 4) {
                const Vector3& d = L->WorldXfm().m.y;
                s.lightDirs[dl][0] = d.x; s.lightDirs[dl][1] = d.y; s.lightDirs[dl][2] = d.z; s.lightDirs[dl][3] = 0;
                s.lightColors[dl][0] = std::min(lc.red, 1.5f);
                s.lightColors[dl][1] = std::min(lc.green, 1.5f);
                s.lightColors[dl][2] = std::min(lc.blue, 1.5f);
                s.lightColors[dl][3] = 1.0f;
                dl++;
            } else if (ty == 0 && pl < 4) {
                const Vector3& p = L->WorldXfm().v;                // point light WORLD POSITION
                s.pointLightPos[pl][0] = p.x; s.pointLightPos[pl][1] = p.y; s.pointLightPos[pl][2] = p.z; s.pointLightPos[pl][3] = 0;
                s.pointLightColors[pl][0] = std::min(lc.red, 1.8f);
                s.pointLightColors[pl][1] = std::min(lc.green, 1.8f);
                s.pointLightColors[pl][2] = std::min(lc.blue, 1.8f);
                s.pointLightColors[pl][3] = 1.0f;
                s.pointLightRanges[pl] = L->Range() > 0.f ? L->Range() : 100.f;
                pl++;
            }
        }
        if (dl == 0 && pl == 0) {
            // Env has NO real lights (ambient-only, e.g. sky.env) — soft default
            // key so geometry still has form. NOT added when the env has point
            // lights (e.g. theater.env's coloured stage spots), else a grey key
            // would wash the authored colour out.
            s.lightDirs[0][0] = -0.4f; s.lightDirs[0][1] = -0.5f; s.lightDirs[0][2] = -0.75f; s.lightDirs[0][3] = 0;
            s.lightColors[0][0] = 0.6f; s.lightColors[0][1] = 0.6f; s.lightColors[0][2] = 0.6f; s.lightColors[0][3] = 1.0f;
            dl = 1;
        }
        s.numLights = dl;
        s.numPointLights = pl;
    } else {
        s.numLights = 1;
        s.lightDirs[0][0] = -0.4f; s.lightDirs[0][1] = -0.5f; s.lightDirs[0][2] = -0.75f; s.lightDirs[0][3] = 0;
        s.lightColors[0][0] = 1.0f; s.lightColors[0][1] = 1.0f; s.lightColors[0][2] = 1.0f; s.lightColors[0][3] = 1.0f;
        s.ambientColor[0] = s.ambientColor[1] = s.ambientColor[2] = 0.45f; s.ambientColor[3] = 1.0f;
        s.numPointLights = 0;
    }
    s.fogEnabled = 0;
    s.shadowEnabled = 0;
    s.numProjLights = 0;

    mSceneOffset = mSceneRing.Write(mGpu.Queue(), &s, sizeof(s));

    wgpu::BindGroupEntry e[5] = {};
    e[0].binding = 0; e[0].buffer = mSceneRing.Buffer(); e[0].offset = mSceneOffset; e[0].size = sizeof(SceneUniforms);
    e[1].binding = 1; e[1].textureView = mShadowView;
    e[2].binding = 2; e[2].sampler = mShadowSampler;
    e[3].binding = 3; e[3].textureView = mWhiteView;
    e[4].binding = 4; e[4].sampler = mSampler;
    wgpu::BindGroupDescriptor bd{};
    bd.layout = mPipelines.SceneLayout();
    bd.entryCount = 5; bd.entries = e;
    mSceneBindGroup = mGpu.Device().CreateBindGroup(&bd);
    mLastSceneCam = cam;
    if (cam) {
        const Transform& cw = cam->WorldXfm();
        mLastSceneCamPose[0] = cw.v.x; mLastSceneCamPose[1] = cw.v.y; mLastSceneCamPose[2] = cw.v.z;
        mLastSceneCamPose[3] = cw.m.y.x; mLastSceneCamPose[4] = cw.m.y.y; mLastSceneCamPose[5] = cw.m.y.z;
    }
}

void BandRnd::BeginFrame(RndCam* cam) {
    if (!mGpuReady) return;

    // A5 pipeline pre-warm: one-shot, on the first rendered frame after the GPU
    // is ready (well before the splash->hub venue-build frame). mTargetFmt was
    // latched in InitGpuResources and mRtFmt is a fixed member, so both keys
    // match what DrawMesh will later request. Runs OUTSIDE the render pass (it
    // only touches the pipeline cache / device, never the encoder), so it is
    // safe here at the top of BeginFrame. Charges its compile cost to this idle
    // frame's pipeMs counter instead of the transition spike. Inert when
    // RB3_PIPELINE_PREWARM_OFF=1.
    if (!mPipelinesPrewarmed && !RB3PipelinePrewarmDisabled()) {
        mPipelinesPrewarmed = true;
        static bool sPrewarmDbg = getenv("RB3_PREWARM_DBG") != nullptr;
        double wall0 = (sPrewarmDbg || gFrameTraceActive) ? FrameTraceNowMs() : 0.0;
        int created = mPipelines.PreWarm(mTargetFmt, mRtFmt);
        if (gFrameTraceActive) {
            gPipelineCreateMsThisFrame += (float)(FrameTraceNowMs() - wall0);
            gPipelineCreateCountThisFrame += created;
        }
        if (sPrewarmDbg)
            fprintf(stderr, "[A5] pipeline pre-warm: created %d pipelines in "
                    "%.1f ms (mainFmt=%d rtFmt=%d)\n",
                    created, FrameTraceNowMs() - wall0,
                    (int)mTargetFmt, (int)mRtFmt);
    }

    mDrawnMeshes = 0;
    mDrawnTris = 0;
    // Per-frame GPU-resource CREATE counters — proves the mesh leak is fixed.
    // At steady state (no new geometry entering the scene) these drop to ~0.
    sMeshBufCreatesThisFrame = 0;
    sMeshBGCreatesThisFrame = 0;
    // Advance the frame sequence so each mesh lazily resets its per-frame uniform
    // slot index (RB3MeshEntry::nextSlot) the first time it draws this frame.
    sFrameSeq++;
    mSceneRing.Reset();
    mMaterialRing.Reset();
    mObjectRing.Reset();
    mBoneRing.Reset();
    // P1 highway bloom: drop last frame's captured replay records. Keeps the
    // per-draw bind-group handles (refcounted) valid only through THIS frame's
    // EndFrame; menus / world.cam frames leave it empty so CompositeHaloBloom is
    // a no-op. Inert allocation when RB3_HIGHWAY_BLOOM_OFF=1 (vector stays empty).
    mHaloDraws.clear();

    mFrameView = mGpu.IsHeadless() ? mGpu.AcquireHeadlessFrame() : mGpu.AcquireNextFrame();
    if (!mFrameView) { fprintf(stderr, "BandRnd: frame acquire failed\n"); return; }

    // Size the depth (and postproc intermediate / halo) attachments to the LIVE
    // color target every frame. On web the browser auto-resizes the canvas
    // backing store on a window/devtools resize, so the swapchain texture
    // AcquireNextFrame() just returned can differ from the boot size, while
    // WindowWidth/Height (only moved by an explicit ResizeSurface the web path
    // never calls) go stale. Reading the actual color-texture size keeps every
    // render pass' attachments the same size — otherwise BandMainPass aborts with
    // "depth stencil attachment size does not match the other attachments".
    // Headless has no swapchain texture, so fall back to WindowWidth/Height
    // (fixed for headless captures, so depth stays at the init size).
    int fbW = mGpu.WindowWidth(), fbH = mGpu.WindowHeight();
    if (!mGpu.IsHeadless()) {
        wgpu::Texture& surf = mGpu.SurfaceTexture();
        if (surf) {
            uint32_t sw = surf.GetWidth(), sh = surf.GetHeight();
            if (sw > 0 && sh > 0) { fbW = (int)sw; fbH = (int)sh; }
        }
    }
    EnsureDepth(fbW, fbH);

    WriteSceneUniforms(cam);

    mEncoder = mGpu.Device().CreateCommandEncoder();

    // Stage 2: if a post-process is selected (e.g. song_select's B+W_film02.pp),
    // render the whole frame into an offscreen intermediate and grade it onto the
    // framebuffer in EndFrame. When no postproc is active the path is inert —
    // the main pass draws straight into mFrameView, exactly as before (canary).
    bool hasPP = !RB3PostProcDisabled() && RndPostProc::Current() != nullptr;
    mPostProcFlushed = false;
    mRenderedToIntermediate = false;
    wgpu::TextureView mainTarget = mFrameView;
    if (hasPP) {
        EnsureIntermediate(fbW, fbH);
        if (mIntermediateView) { mainTarget = mIntermediateView; mRenderedToIntermediate = true; }
    }

    wgpu::RenderPassColorAttachment colorAtt{};
    colorAtt.view = mainTarget;
    colorAtt.loadOp = wgpu::LoadOp::Clear;
    colorAtt.storeOp = wgpu::StoreOp::Store;
    colorAtt.clearValue = {(double)mClearColor.red, (double)mClearColor.green,
                           (double)mClearColor.blue, 1.0};

    wgpu::RenderPassDepthStencilAttachment depthAtt{};
    depthAtt.view = mDepthView;
    depthAtt.depthLoadOp = wgpu::LoadOp::Clear; depthAtt.depthStoreOp = wgpu::StoreOp::Store;
    depthAtt.depthClearValue = 1.0f;
    depthAtt.stencilLoadOp = wgpu::LoadOp::Clear; depthAtt.stencilStoreOp = wgpu::StoreOp::Store;
    depthAtt.stencilClearValue = 0;

    wgpu::RenderPassDescriptor rp{};
    rp.label = "BandMainPass";
    rp.colorAttachmentCount = 1; rp.colorAttachments = &colorAtt;
    rp.depthStencilAttachment = &depthAtt;

    mPass = mEncoder.BeginRenderPass(&rp);
    mInPass = true;
    mPass.SetBindGroup(0, mSceneBindGroup, 0, nullptr);

    // Dispatch the engine's pre-clear render-to-texture pass once the main pass is
    // open. The shared Rnd::DrawPreClear() iterates the registered pre-clear
    // drawables (TexRenderer, TexMovie-to-tex, and crucially OutfitConfig — whose
    // DrawPreClear drives MatSwap::Compose, the ONLY caller of the DrawRect
    // RTT-outfit-tint branch). The mid-frame RTT suspend/resume contract
    // (BeginDrawTarget/EndDrawTarget) handles running these after the main clear:
    // each RT compose suspends the freshly-cleared main pass and resumes it with
    // LoadOp::Load.
    //
    // ON BY DEFAULT so character-outfit RTT tints actually render. Verified
    // regression-safe across boot/main_hub/song_select/gameplay (no Dawn aborts,
    // no artifacts; OutfitConfig::Compose composes its tints into the outfit RTT
    // diffuse). Opt-out RB3_NO_PRECLEAR=1 restores the byte-identical no-pre-clear
    // loop (the original BandRnd behaviour) for A/B / debugging.
    {
        static int s = -1;
        if (s < 0) { const char* e = getenv("RB3_NO_PRECLEAR"); s = (e && e[0] && e[0] != '0') ? 0 : 1; }
        if (s) Rnd::DrawPreClear();
    }
}

void BandRnd::EndFrame() {
    if (!mGpuReady) return;
    if (mInPass) { mPass.End(); mInPass = false; }
    // Defensive: if a render-target pass was somehow left open at frame end
    // (it should always be closed by FinishDrawTarget within the same frame),
    // clear the latch so the next frame starts clean rather than carrying a
    // stale RT-redirect state into a fresh encoder.
    mRtActiveTex = nullptr;

    // Stage 2: if the frame was rendered into the offscreen intermediate (a
    // postproc is selected), grade-composite it onto the real framebuffer view
    // now — same encoder, before Finish(). Fires exactly once per frame.
    //
    // Tier 2: when the venue was composited MID-FRAME (DoPostProcess / EndWorld
    // fired before the HUD/track panel drew), mPostProcFlushed is already true and
    // the framebuffer holds graded-venue + ungraded-HUD — DO NOT composite again
    // (that would re-grade the HUD). The !mPostProcFlushed guard makes this path
    // exclusive with the mid-frame flush. Screens with no postprocs_before_draw
    // panel (song_select 2D composite, menus) never flush mid-frame, so they take
    // THIS path unchanged (canary preserved).
    if (mRenderedToIntermediate && mIntermediateView && RndPostProc::Current() &&
        !mPostProcFlushed && !RB3PostProcDisabled()) {
        RunPostProcComposite(mFrameView);
        mPostProcFlushed = true;
    }

    // P1 additive-halo-only highway gem bloom (Design B). After the main pass +
    // the grade block, before Finish(): replay this frame's captured halo-source
    // draws into a transparent buffer, bloom it, and ADDITIVE-blit the halo onto
    // mFrameView (LoadOp::Load). The base highway is never redirected. Inert when
    // RB3_HIGHWAY_BLOOM_OFF=1 (mHaloDraws empty) or off a gameplay frame.
    if (HighwayBloomEnabled() && !mHaloDraws.empty() && mFrameView)
        CompositeHaloBloom();

    wgpu::CommandBuffer cmd = mEncoder.Finish();
    mGpu.Queue().Submit(1, &cmd);
    mFrameView = nullptr;
    // V3-extension: per-frame render diagnostics gated on RENDER_DBG /
    // RB3_RENDER_DBG. Logs camera name + draw count + tri count + clear color
    // every N frames so the gameplay-scene red-screen can be triaged against
    // "what was the renderer actually doing on this frame".
    static int sDbgEvery = -1;
    if (sDbgEvery < 0) {
        const char* s = getenv("RENDER_DBG");
        if (!s) s = getenv("RB3_RENDER_DBG");
        sDbgEvery = (s && s[0]) ? atoi(s) : 0;
        if (sDbgEvery <= 0 && s && s[0]) sDbgEvery = 1;
    }
    if (sDbgEvery > 0 && (mFrameCount % sDbgEvery) == 0) {
        RndCam* cur = RndCam::sCurrent;
        const char* camName = (cur && cur->Name()) ? cur->Name() : "<none>";
        int texCount = (int)sTexGpu.size();
        fprintf(stderr, "[render f%d] cam=%s pos=(%.2f,%.2f,%.2f) clear=(%.2f,%.2f,%.2f) "
                        "meshes=%d tris=%d uploaded_tex=%d cached_meshes=%d "
                        "buf_creates=%d bg_creates=%d\n",
                mFrameCount, camName,
                cur ? cur->WorldXfm().v.x : 0.f,
                cur ? cur->WorldXfm().v.y : 0.f,
                cur ? cur->WorldXfm().v.z : 0.f,
                mClearColor.red, mClearColor.green, mClearColor.blue,
                mDrawnMeshes, mDrawnTris, texCount, (int)sMeshGpu.size(),
                sMeshBufCreatesThisFrame, sMeshBGCreatesThisFrame);
    }
    // Default (no RENDER_DBG / RB3_RENDER_DBG): stay silent. Synchronous
    // console.log() in the browser is extremely expensive; a per-frame tally
    // here flooded the JS console (~60 msgs/s) and tanked menu FPS.
}

// Build a material bind group against an explicit buffer (used for pre-warm).
wgpu::BindGroup BandRnd::MakeMaterialBindGroupRaw(wgpu::Buffer buf, uint32_t off) {
    wgpu::BindGroupEntry e[11] = {};
    e[0].binding = 0;  e[0].buffer = buf; e[0].offset = off; e[0].size = sizeof(MaterialUniforms);
    e[1].binding = 1;  e[1].textureView = mWhiteView;
    e[2].binding = 2;  e[2].sampler = mSampler;
    e[3].binding = 3;  e[3].textureView = mFlatNormalView;
    e[4].binding = 4;  e[4].textureView = mBlackView;
    e[5].binding = 5;  e[5].textureView = mBlackView;
    e[6].binding = 6;  e[6].textureView = mBlackView;
    e[7].binding = 7;  e[7].sampler = mSampler;
    e[8].binding = 8;  e[8].textureView = mBlackCubeView;
    e[9].binding = 9;  e[9].sampler = mSampler;
    e[10].binding = 10; e[10].textureView = mFlatNormalView;
    wgpu::BindGroupDescriptor bd{};
    bd.layout = mPipelines.MaterialLayout();
    bd.entryCount = 11; bd.entries = e;
    return mGpu.Device().CreateBindGroup(&bd);
}

// Resolve a material's diffuse + emissive GPU texture views (performing a lazy
// upload if needed). Pulled out of bind-group construction so DrawMesh can probe
// for a late texture arrival WITHOUT creating a throwaway bind group every frame.
void BandRnd::ResolveMaterialViews(RndMat* mat, wgpu::TextureView& diffuse,
                                   wgpu::TextureView& emissive) {
    diffuse = mWhiteView;
    emissive = mBlackView;
    if (mat) {
        RndTex* dt = mat->GetDiffuseTex();
        if (dt) {
            wgpu::TextureView v = GetRB3TexView(dt);
            if (!v) v = UploadRndTexIfNeeded(mGpu, dt);
            if (v) diffuse = v;
        }
        RndTex* et = (RndTex*)mat->mEmissiveMap;
        if (et) {
            wgpu::TextureView v = GetRB3TexView(et);
            if (!v) v = UploadRndTexIfNeeded(mGpu, et);
            if (v) emissive = v;
        }
    }
}

// Per-mesh-cache material bind group: identical texture/sampler wiring to
// MakeMaterialBindGroup, but binds the uniform at offset 0 of the mesh's OWN
// persistent matUB (not a per-frame ring offset) and takes already-resolved
// diffuse/emissive views (resolved by ResolveMaterialViews above) so DrawMesh
// can cache the bind group and rebuild it only when those views (or the material
// pointer) actually change.
wgpu::BindGroup BandRnd::MakeMaterialBindGroupCached(wgpu::Buffer buf,
                                                     wgpu::TextureView diffuse,
                                                     wgpu::TextureView emissive) {
    wgpu::BindGroupEntry e[11] = {};
    e[0].binding = 0;  e[0].buffer = buf; e[0].offset = 0; e[0].size = sizeof(MaterialUniforms);
    e[1].binding = 1;  e[1].textureView = diffuse;
    e[2].binding = 2;  e[2].sampler = mSampler;
    e[3].binding = 3;  e[3].textureView = mFlatNormalView;
    e[4].binding = 4;  e[4].textureView = mBlackView;
    e[5].binding = 5;  e[5].textureView = emissive;
    e[6].binding = 6;  e[6].textureView = mBlackView;
    e[7].binding = 7;  e[7].sampler = mSampler;
    e[8].binding = 8;  e[8].textureView = mBlackCubeView;
    e[9].binding = 9;  e[9].sampler = mSampler;
    e[10].binding = 10; e[10].textureView = mFlatNormalView;
    wgpu::BindGroupDescriptor bd{};
    bd.layout = mPipelines.MaterialLayout();
    bd.entryCount = 11; bd.entries = e;
    return mGpu.Device().CreateBindGroup(&bd);
}

wgpu::BindGroup BandRnd::MakeMaterialBindGroup(uint32_t off, RndMat* mat) {
    // Resolve the material's diffuse texture (if any) to its GPU view.
    // Triggers a lazy upload if the texture hasn't been SyncBitmap'd yet —
    // ensures texture data arrives even when the matched-fork PostLoad path
    // doesn't call SyncBitmap before the first DrawMesh.
    wgpu::TextureView diffuse = mWhiteView;
    wgpu::TextureView emissive = mBlackView;
    if (mat) {
        RndTex* dt = mat->GetDiffuseTex();
        if (dt) {
            wgpu::TextureView v = GetRB3TexView(dt);
            if (!v) v = UploadRndTexIfNeeded(mGpu, dt);
            if (v) diffuse = v;
        }
        // Track-A glow: resolve the emissive (self-illumination) map so the shader's
        // emissive term has real data. Mirrors the diffuse path (texture cache +
        // lazy upload). Inert unless DrawMesh sets emissiveMultiplier > 0.
        RndTex* et = (RndTex*)mat->mEmissiveMap;
        if (et) {
            wgpu::TextureView v = GetRB3TexView(et);
            if (!v) v = UploadRndTexIfNeeded(mGpu, et);
            if (v) emissive = v;
        }
    }
    wgpu::BindGroupEntry e[11] = {};
    e[0].binding = 0;  e[0].buffer = mMaterialRing.Buffer(); e[0].offset = off; e[0].size = sizeof(MaterialUniforms);
    e[1].binding = 1;  e[1].textureView = diffuse;
    e[2].binding = 2;  e[2].sampler = mSampler;
    e[3].binding = 3;  e[3].textureView = mFlatNormalView;
    e[4].binding = 4;  e[4].textureView = mBlackView;
    e[5].binding = 5;  e[5].textureView = emissive;
    e[6].binding = 6;  e[6].textureView = mBlackView;
    e[7].binding = 7;  e[7].sampler = mSampler;
    e[8].binding = 8;  e[8].textureView = mBlackCubeView;
    e[9].binding = 9;  e[9].sampler = mSampler;
    e[10].binding = 10; e[10].textureView = mFlatNormalView;
    wgpu::BindGroupDescriptor bd{};
    bd.layout = mPipelines.MaterialLayout();
    bd.entryCount = 11; bd.entries = e;
    return mGpu.Device().CreateBindGroup(&bd);
}

// --- Xbox 360 compressed vertex unpacking (36 bytes/vert, big-endian) ---
// Mirrors milo-native-engine gfx/VertexFormats.cpp UnpackCompressedVertices
// (which lives in the rndobj-coupled TU RB3 excludes). The D3D vertex decl is:
//   pos   = FLOAT3   POSITION  (3 BE floats, off 0)
//   color = D3DCOLOR COLOR     (packed, off 12)
//   uv    = FLOAT16_2 TEXCOORD (off 16)
//   norm  = DEC4N    NORMAL    (10-10-10-2, off 20)
//   tan   = DEC4N    TANGENT   (off 24); bone data off 28/32.
struct XboxCVert { int pos[3]; int color; int uv; int norm; int tan; int b0; int b1; };
static_assert(sizeof(XboxCVert) == 36, "XboxCVert must be 36 bytes");

static float BeFloat(int bits) {
    unsigned v = __builtin_bswap32((unsigned)bits); float f; std::memcpy(&f, &v, 4); return f;
}
static float Half2Float(unsigned short h) {
    unsigned sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    unsigned f;
    if (exp == 0) { if (mant == 0) f = sign << 31; else { float val = (float)mant / 1024.0f * (1.0f/16384.0f); return sign ? -val : val; } }
    else if (exp == 0x1F) f = (sign << 31) | 0x7F800000 | (mant << 13);
    else f = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
    float r; std::memcpy(&r, &f, 4); return r;
}
static void BeUV(int packed, float out[2]) {
    unsigned v = __builtin_bswap32((unsigned)packed);
    out[0] = Half2Float((v >> 16) & 0xFFFF); out[1] = Half2Float(v & 0xFFFF);
}
static void BeColor(int packed, float out[4]) {
    // RB3's compressed vertex colour is a D3DCOLOR (D3DDECLTYPE_D3DCOLOR), ARGB
    // packed as 0xAARRGGBB. The asset is big-endian (Xbox 360): 4 bytes on disc
    // are [AA, RR, GG, BB]; read as a little-endian int then bswap32 restores
    // the natural 0xAARRGGBB, so R = (v>>16), G = (v>>8), B = (v>>0), A = (v>>24).
    // (The prior mapping read R from the low byte, swapping R<->B; this only
    // affected meshes that actually use vertex colour — i.e. PRELIT meshes,
    // since the shader now gates vertex-colour application on RndMat::mPreLit.)
    unsigned v = __builtin_bswap32((unsigned)packed);
    out[0] = ((v >> 16) & 0xFF) / 255.0f; // R
    out[1] = ((v >> 8)  & 0xFF) / 255.0f; // G
    out[2] = ((v >> 0)  & 0xFF) / 255.0f; // B
    out[3] = ((v >> 24) & 0xFF) / 255.0f; // A
}
static void BeDec4n(int packed, float out[3]) {
    unsigned v = __builtin_bswap32((unsigned)packed);
    int ix = (int)(v << 22) >> 22, iy = (int)(v << 12) >> 22, iz = (int)(v << 2) >> 22;
    out[0] = ix / 511.0f; out[1] = iy / 511.0f; out[2] = iz / 511.0f;
}
// UDEC4N: 10-10-10-2 UNSIGNED normalized (bone weights, BLENDWEIGHT slot).
static void BeUDec4n(int packed, float out[4]) {
    unsigned v = __builtin_bswap32((unsigned)packed);
    out[0] = (v & 0x3FF) / 1023.0f;
    out[1] = ((v >> 10) & 0x3FF) / 1023.0f;
    out[2] = ((v >> 20) & 0x3FF) / 1023.0f;
    out[3] = ((v >> 30) & 0x3) / 3.0f;
}
// UBYTE4: four packed bone indices (BLENDINDICES slot). The blob is big-endian
// on disc; bswap restores Xbox byte order (idx0 in the low byte).
static void BeUByte4(int packed, uint8_t out[4]) {
    unsigned v = __builtin_bswap32((unsigned)packed);
    out[0] = v & 0xFF; out[1] = (v >> 8) & 0xFF;
    out[2] = (v >> 16) & 0xFF; out[3] = (v >> 24) & 0xFF;
}

// ---------------------------------------------------------------------------
// RB3UnpackMeshVerts — the per-vertex CPU unpack shared by DrawMesh and the L2
// GPU warm sweep (BandRnd::WarmGpuForDir). Reads owner->mVerts (uncompressed RB3
// Vert, Color32-packed) OR owner->mCompressedVerts (Xbox-compressed, Be*-decoded),
// filling the static OR skinned engine layout per `skinned`. Returns the unpacked
// vert count, or -1 if the mesh has no geometry. Factored out so the warm sweep's
// pre-upload is byte-identical to the draw-time upload (same VB bytes -> the first
// real draw is a guaranteed cache hit). This is the dominant -O0-wasm cost class
// (research/09: Be*/Half2Float/GpuVertexSkinned family) — charge it at the caller.
// ---------------------------------------------------------------------------
static int RB3UnpackMeshVerts(RndMesh* owner, bool skinned,
                              std::vector<GpuVertexRB3>& gpuVerts,
                              std::vector<GpuVertexSkinned>& gpuVertsSkinned) {
    RndMesh::VertVector& verts = owner->mVerts;
    int nv = verts.size();
    if (nv > 0) {
        if (skinned) {
            gpuVertsSkinned.resize(nv);
            for (int i = 0; i < nv; i++) {
                const RndMesh::Vert& v = verts[i];
                GpuVertexSkinned& g = gpuVertsSkinned[i];
                g.pos[0] = v.pos.x; g.pos[1] = v.pos.y; g.pos[2] = v.pos.z;
                g.norm[0] = v.norm.x; g.norm[1] = v.norm.y; g.norm[2] = v.norm.z;
                g.color[0] = v.color.fr(); g.color[1] = v.color.fg();
                g.color[2] = v.color.fb(); g.color[3] = v.color.fa();
                g.uv[0] = v.uv.x; g.uv[1] = v.uv.y;
                g.boneWeights[0] = v.boneWeights.GetX(); g.boneWeights[1] = v.boneWeights.GetY();
                g.boneWeights[2] = v.boneWeights.GetZ(); g.boneWeights[3] = v.boneWeights.GetW();
                g.boneIndices[0] = (uint8_t)v.boneIndices[0]; g.boneIndices[1] = (uint8_t)v.boneIndices[1];
                g.boneIndices[2] = (uint8_t)v.boneIndices[2]; g.boneIndices[3] = (uint8_t)v.boneIndices[3];
                g.pad = 0.0f;
                g.tangent[0] = 1.0f; g.tangent[1] = 0; g.tangent[2] = 0; g.tangent[3] = 1.0f;
            }
        } else {
            gpuVerts.resize(nv);
            for (int i = 0; i < nv; i++) {
                const RndMesh::Vert& v = verts[i];
                GpuVertexRB3& g = gpuVerts[i];
                g.pos[0] = v.pos.x; g.pos[1] = v.pos.y; g.pos[2] = v.pos.z;
                g.norm[0] = v.norm.x; g.norm[1] = v.norm.y; g.norm[2] = v.norm.z;
                g.color[0] = v.color.fr(); g.color[1] = v.color.fg();
                g.color[2] = v.color.fb(); g.color[3] = v.color.fa();
                g.uv[0] = v.uv.x; g.uv[1] = v.uv.y;
                g.tangent[0] = 1.0f; g.tangent[1] = 0; g.tangent[2] = 0; g.tangent[3] = 1.0f;
            }
        }
    } else if (owner->mCompressedVerts && owner->mNumCompressedVerts > 0) {
        nv = (int)owner->mNumCompressedVerts;
        const XboxCVert* cv = (const XboxCVert*)owner->mCompressedVerts;
        if (skinned) {
            gpuVertsSkinned.resize(nv);
            for (int i = 0; i < nv; i++) {
                GpuVertexSkinned& g = gpuVertsSkinned[i];
                g.pos[0] = BeFloat(cv[i].pos[0]); g.pos[1] = BeFloat(cv[i].pos[1]); g.pos[2] = BeFloat(cv[i].pos[2]);
                BeColor(cv[i].color, g.color);
                BeUV(cv[i].uv, g.uv);
                BeDec4n(cv[i].norm, g.norm);
                BeUDec4n(cv[i].b0, g.boneWeights);   // BLENDWEIGHT (UDEC4N)
                BeUByte4(cv[i].b1, g.boneIndices);   // BLENDINDICES (UBYTE4)
                g.pad = 0.0f;
                float t3[3]; BeDec4n(cv[i].tan, t3);
                g.tangent[0] = t3[0]; g.tangent[1] = t3[1]; g.tangent[2] = t3[2]; g.tangent[3] = 1.0f;
            }
        } else {
            gpuVerts.resize(nv);
            for (int i = 0; i < nv; i++) {
                GpuVertexRB3& g = gpuVerts[i];
                g.pos[0] = BeFloat(cv[i].pos[0]); g.pos[1] = BeFloat(cv[i].pos[1]); g.pos[2] = BeFloat(cv[i].pos[2]);
                BeColor(cv[i].color, g.color);
                BeUV(cv[i].uv, g.uv);
                BeDec4n(cv[i].norm, g.norm);
                float t3[3]; BeDec4n(cv[i].tan, t3);
                g.tangent[0] = t3[0]; g.tangent[1] = t3[1]; g.tangent[2] = t3[2]; g.tangent[3] = 1.0f;
            }
        }
    } else {
        return -1; // no geometry
    }
    return skinned ? (int)gpuVertsSkinned.size() : (int)gpuVerts.size();
}

// ===========================================================================
// Render-to-texture (RTT).
//
// BeginDrawTarget suspends the main pass and opens a fresh pass that draws into
// a per-RndTex RGBA8 render target (no depth, transparent clear). EndDrawTarget
// closes that pass and re-opens the main pass (preserving its contents via
// LoadOp::Load). The begin hook is driven lazily from DrawMesh (the shared
// rndobj/Cam.cpp only fires the END hook, FinishDrawTarget); the end hook is
// driven from RndTex::FinishDrawTarget below.
//
// Disable with RB3_RTT_OFF=1 — then DrawMesh never redirects, so a render
// target tex (e.g. clouds_rnd.tex) is never painted and the sky-dome material
// samples an empty view (the prior static-sky behaviour).
// ===========================================================================
static bool RB3RttDisabled() {
    static int s = -1;
    if (s < 0) { const char* e = getenv("RB3_RTT_OFF"); s = (e && e[0] && e[0] != '0') ? 1 : 0; }
    return s != 0;
}

void BandRnd::BeginDrawTarget(RndTex* tex) {
    if (!mGpuReady || !tex || RB3RttDisabled()) return;
    if (mRtActiveTex == tex) return;        // already redirected to this target
    // Nested RT (already painting another target) is unsupported here — bail
    // rather than corrupt the pass nesting. (RB3's clouds path never nests.)
    if (mRtActiveTex) return;

    int w = tex->Width(), h = tex->Height();
    if (w <= 0 || h <= 0) return;

    // Lazily create the RT texture + view ONCE, stored in the same side-table
    // the diffuse-bind path reads (so the sky-dome material samples it).
    RB3TexEntry& e = sTexGpu[tex];
    if (!e.isRenderTarget || !e.tex) {
        wgpu::TextureDescriptor td{};
        td.label = "RB3RenderTarget";
        td.size = {(uint32_t)w, (uint32_t)h, 1};
        td.format = mRtFmt;   // RGBA8Unorm
        td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
        td.mipLevelCount = 1;
        wgpu::Texture t = mGpu.Device().CreateTexture(&td);
        if (!t) return;
        e.tex = t;
        e.view = t.CreateView();
        e.isRenderTarget = true;
        e.uploaded = true;        // make GetRB3TexView/UploadRndTexIfNeeded bind it
        e.lastPixels = nullptr;
        e.fingerprint = 0;
        if (getenv("RB3_RENDER_DBG"))
            fprintf(stderr, "[dbg] RTT created %dx%d for tex '%s'\n",
                    w, h, tex->Name() ? tex->Name() : "?");
    }

    // Suspend the main pass.
    if (mInPass) { mPass.End(); mInPass = false; }

    // Begin a new pass into the RT view: clear transparent, store, NO depth.
    wgpu::RenderPassColorAttachment colorAtt{};
    colorAtt.view = e.view;
    colorAtt.loadOp = wgpu::LoadOp::Clear;
    colorAtt.storeOp = wgpu::StoreOp::Store;
    colorAtt.clearValue = {0.0, 0.0, 0.0, 0.0};

    wgpu::RenderPassDescriptor rp{};
    rp.label = "BandRTTPass";
    rp.colorAttachmentCount = 1; rp.colorAttachments = &colorAtt;
    rp.depthStencilAttachment = nullptr;

    mPass = mEncoder.BeginRenderPass(&rp);
    mInPass = true;
    mPass.SetBindGroup(0, mSceneBindGroup, 0, nullptr);
    mRtActiveTex = tex;
}

void BandRnd::EndDrawTarget() {
    if (!mGpuReady || !mRtActiveTex) return;

    // Close the RT pass.
    if (mInPass) { mPass.End(); mInPass = false; }
    mRtActiveTex = nullptr;

    // Re-open the MAIN pass, PRESERVING whatever was already drawn this frame
    // (LoadOp::Load on both color and depth — the RT pass ran mid-frame).
    if (!mFrameView) return;   // frame already torn down (defensive)
    wgpu::RenderPassColorAttachment colorAtt{};
    // Stage 2: when a postproc is active the main pass renders into the
    // intermediate, so a mid-frame RTT (e.g. the clouds/sky-dome RndCam::TargetTex
    // user) must RESUME into the intermediate too — not the framebuffer — or the
    // post-RTT scene draws would land outside the graded path. MainColorTarget()
    // returns mIntermediateView under a postproc, else mFrameView (unchanged).
    colorAtt.view = MainColorTarget();
    colorAtt.loadOp = wgpu::LoadOp::Load;
    colorAtt.storeOp = wgpu::StoreOp::Store;

    wgpu::RenderPassDepthStencilAttachment depthAtt{};
    depthAtt.view = mDepthView;
    depthAtt.depthLoadOp = wgpu::LoadOp::Load; depthAtt.depthStoreOp = wgpu::StoreOp::Store;
    // Even with LoadOp::Load (no clear), WebGPU/Dawn validates that
    // depthClearValue is finite in [0,1] on beginRenderPass. The wgpu C++
    // struct defaults this field to NaN (kDepthClearValueUndefined), which
    // fails validation and aborts the resumed main pass — freezing the web
    // render loop. Set the standard depth clear (1.0) so it is valid.
    depthAtt.depthClearValue = 1.0f;
    depthAtt.stencilLoadOp = wgpu::LoadOp::Load; depthAtt.stencilStoreOp = wgpu::StoreOp::Store;
    depthAtt.stencilClearValue = 0;

    wgpu::RenderPassDescriptor rp{};
    rp.label = "BandMainPassResume";
    rp.colorAttachmentCount = 1; rp.colorAttachments = &colorAtt;
    rp.depthStencilAttachment = &depthAtt;

    mPass = mEncoder.BeginRenderPass(&rp);
    mInPass = true;
    // The cam was restored to the prior scene cam (current->Select()) after the
    // RT draw; force a scene-uniform re-write on the next DrawMesh by clearing
    // the staleness latch, then bind the existing scene group for now.
    mPass.SetBindGroup(0, mSceneBindGroup, 0, nullptr);
    mLastSceneCam = nullptr;   // next DrawMesh re-resolves the active cam
}

// ===========================================================================
// Tier 2 mid-frame layering — grade the VENUE, draw highway+HUD over it ungraded
// (this supersedes the concurrent depth-only ClearDepthForOverlay from engine
//  6498fab: the Tier 2 ClearDepthForOverlay below does the venue-grade flush AND
//  the original depth-clear as its fallback — see its body.)
//
// Retail layers the post-process as a fullscreen TEV blit of the world RTT inside
// EndWorld (WiiRnd::DoPostProcess), AFTER the venue scene and BEFORE the HUD/track
// panel. We reproduce that: when EndWorld fires (PanelDir::DrawShowing, once per
// frame, via the engine's mWorldEnded latch), the fully-rendered venue
// intermediate is graded onto the framebuffer here, then the main pass RESUMES
// targeting the framebuffer — color LoadOp::Load keeps the graded venue, depth
// LoadOp::Clear lets the highway/gems/HUD composite ON TOP, UNGRADED. Reuses the
// EndDrawTarget suspend/resume machinery (no new GPU plumbing).
// ===========================================================================
void BandRnd::FlushPostProcMidFrame() {
    if (!mGpuReady || mPostProcFlushed || !mRenderedToIntermediate) return;
    if (RB3PostProcDisabled() || !RndPostProc::Current() || !mIntermediateView) return;
    if (!mFrameView) return;                 // frame already torn down (defensive)
    if (mRtActiveTex) return;                // never flush while a mid-frame RTT pass is open

    // 1. Close the main (intermediate) pass so the venue is fully written.
    if (mInPass) { mPass.End(); mInPass = false; }

    // 2. Grade the intermediate onto the framebuffer (runs bloom + composite; opens
    //    and closes its own render pass against mFrameView).
    RunPostProcComposite(mFrameView);
    mPostProcFlushed = true;

    // 3. Re-open the main pass targeting the FRAMEBUFFER. Color LoadOp::Load keeps
    //    the graded venue; depth LoadOp::Clear resets z so the highway/gems/HUD
    //    (drawn with their own game.cam) composite on top instead of being occluded
    //    by venue geometry depth. (Same suspend/resume contract as EndDrawTarget;
    //    depthClearValue must be finite for Dawn validation even with Load.)
    wgpu::RenderPassColorAttachment colorAtt{};
    colorAtt.view = mFrameView;
    colorAtt.loadOp = wgpu::LoadOp::Load;    // preserve the graded venue blit
    colorAtt.storeOp = wgpu::StoreOp::Store;

    wgpu::RenderPassDepthStencilAttachment depthAtt{};
    depthAtt.view = mDepthView;
    depthAtt.depthLoadOp = wgpu::LoadOp::Clear; depthAtt.depthStoreOp = wgpu::StoreOp::Store;
    depthAtt.depthClearValue = 1.0f;
    depthAtt.stencilLoadOp = wgpu::LoadOp::Clear; depthAtt.stencilStoreOp = wgpu::StoreOp::Store;
    depthAtt.stencilClearValue = 0;

    wgpu::RenderPassDescriptor rp{};
    rp.label = "BandMainPassPostGrade";
    rp.colorAttachmentCount = 1; rp.colorAttachments = &colorAtt;
    rp.depthStencilAttachment = &depthAtt;

    mPass = mEncoder.BeginRenderPass(&rp);
    mInPass = true;
    mPass.SetBindGroup(0, mSceneBindGroup, 0, nullptr);
    mLastSceneCam = nullptr;   // next DrawMesh re-resolves the active cam

    if (getenv("RB3_RENDER_DBG") || getenv("RB3_TIER2_DBG"))
        fprintf(stderr, "[RB3_TIER2_DBG] mid-frame venue composite flushed f%d "
                "meshesDrawnSoFar=%d (highway/HUD draws ungraded over graded venue)\n",
                mFrameCount, mDrawnMeshes);
}

// ===========================================================================
// P1 additive-halo-only highway gem bloom (DEFAULT-ON; opt out RB3_HIGHWAY_BLOOM_OFF).
// DESIGN B: capture-and-replay, NOT the rejected redirect (Design A). The live
// highway pass (game.cam) is NEVER touched — DrawMesh only CAPTURES a per-draw
// replay record for each halo-source mesh (the live pose-baked scene bind group
// handle + mat/obj/bone bind groups + vbuf/ibuf). At EndFrame, CompositeHaloBloom
// replays those draws into a transparent buffer, blooms it, and ADDITIVE-blits
// ONLY the blurred halo onto mFrameView. The base track is unaffected, so
// RB3_HIGHWAY_BLOOM_BLEND=0 is visually identical to OFF (negative control).
//
// When RB3_HIGHWAY_BLOOM_OFF=1, HighwayBloomEnabled() returns false and every site
// (the DrawMesh capture, the EndFrame composite) is a no-op — byte-identical to
// the pre-bloom path. The halo is confined to the emissive gem cores + now-bar
// (IsHaloSourceMat): the full-quad track surface and HUD meter-glass are excluded
// so the dark track + HUD are never washed.
// ===========================================================================
bool BandRnd::HighwayBloomEnabled() {
    // DEFAULT-ON (gems/now-bar additive bloom — retail-accurate, confined to the
    // emissive gem cores + strike line, never the track surface or HUD). Opt out
    // via RB3_HIGHWAY_BLOOM_OFF=1 (mirrors RB3_TRACK_LIGHT_OFF / RB3_VENUE_LIGHT_OFF).
    static int s = -1;
    if (s < 0) { const char* e = getenv("RB3_HIGHWAY_BLOOM_OFF"); s = (e && e[0] && e[0] != '0') ? 0 : 1; }
    return s != 0;
}

// Halo-source classifier. The halo must be CONFINED to the small, bright emissive
// meshes — the gem cores (prism_mat, emisMap=prism_gem_emissive, mult 1.0) and the
// now-bar/strike (gem_smasher_glow, emisMap + mult 0.90). Two exclusions keep it
// from washing the scene (the first attempt did both):
//   - surface.mat (the highway watermark) is also emissive but is a FULL QUAD;
//     blooming a full quad washes the whole track + lifts the black point. Exclude
//     it by name — it's the one full-plane emissive on the highway.
//   - The additive-blend test (kBlendAdd/kBlendSrcAlphaAdd) was dropped: its only
//     unique catches were the HUD overdrive/streak meter-glass lenses, which bloom
//     into the HUD. The now-bar is already selected by its emissive map, so the
//     blend test added only spill.
// Safe only under the game.cam guard at the call site (other cams never reach this).
bool BandRnd::IsHaloSourceMat(RndMat* mat) {
    if (!mat) return false;
    if ((RndTex*)mat->mEmissiveMap == nullptr || mat->mEmissiveMultiplier <= 0.0f) return false;
    const char* mn = mat->Name();
    if (mn && std::strstr(mn, "surface")) return false;   // full-quad track plane — would wash
    return true;
}

// (Re)create the halo replay target at w x h. Same format/usage as
// EnsureIntermediate (mTargetFmt, RenderAttachment | TextureBinding) so the
// replay can render into it and CompositeHaloBloom can sample it. Sized to the
// window (matches mDepthView), NOT mIntermediate*. Early-out if unchanged.
void BandRnd::EnsureHaloTarget(int w, int h) {
    if (!mGpuReady || w <= 0 || h <= 0) return;
    if (mHaloTex && mHaloView && mHaloWidth == w && mHaloHeight == h) return;
    wgpu::TextureDescriptor td{};
    td.label = "RB3HaloBloomTarget";
    td.size = {(uint32_t)w, (uint32_t)h, 1};
    td.format = mTargetFmt;
    td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
    td.mipLevelCount = 1;
    wgpu::Texture t = mGpu.Device().CreateTexture(&td);
    if (!t) return;
    mHaloTex = t;
    mHaloView = t.CreateView();
    mHaloWidth = w;
    mHaloHeight = h;
}

// A minimal WGSL module: vs_fullscreen (fullscreen triangle, no vbuf) + fs_blit
// (plain textureSample). Builds ONLY the ADDITIVE pipeline (color & alpha
// One/One) — the premultiplied-OVER pipeline of the rejected Design A is dropped
// (we additively lay ONLY the halo over the untouched framebuffer).
static const char* kRB3HaloBlitShaderSource = R"WGSL(
struct VOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

@group(0) @binding(0) var srcTex: texture_2d<f32>;
@group(0) @binding(1) var srcSampler: sampler;

@vertex fn vs_fullscreen(@builtin(vertex_index) idx: u32) -> VOut {
    var out: VOut;
    let x = f32(i32(idx & 1u)) * 4.0 - 1.0;
    let y = f32(i32(idx >> 1u)) * 4.0 - 1.0;
    out.pos = vec4f(x, y, 0.0, 1.0);
    out.uv = vec2f((x + 1.0) * 0.5, (1.0 - y) * 0.5);
    return out;
}

@fragment fn fs_blit(in: VOut) -> @location(0) vec4f {
    return textureSample(srcTex, srcSampler, in.uv);
}
)WGSL";

void BandRnd::EnsureHaloBlitPipeline() {
    if (mHaloBlitReady) return;
    auto& dev = mGpu.Device();

    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = kRB3HaloBlitShaderSource;
    wgpu::ShaderModuleDescriptor smDesc{};
    smDesc.nextInChain = &wgsl;
    mHaloBlitShader = dev.CreateShaderModule(&smDesc);

    // group 0: srcTex@0 (Float, 2D), sampler@1 (Filtering)
    wgpu::BindGroupLayoutEntry entries[2] = {};
    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Fragment;
    entries[0].texture.sampleType = wgpu::TextureSampleType::Float;
    entries[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    entries[1].binding = 1;
    entries[1].visibility = wgpu::ShaderStage::Fragment;
    entries[1].sampler.type = wgpu::SamplerBindingType::Filtering;

    wgpu::BindGroupLayoutDescriptor bglDesc{};
    bglDesc.entryCount = 2;
    bglDesc.entries = entries;
    mHaloBlitBGL = dev.CreateBindGroupLayout(&bglDesc);

    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &mHaloBlitBGL;
    mHaloBlitPL = dev.CreatePipelineLayout(&plDesc);

    // ONLY the additive pipeline (color One/One, alpha One/One).
    wgpu::BlendState bs{};
    bs.color.srcFactor = wgpu::BlendFactor::One;
    bs.color.dstFactor = wgpu::BlendFactor::One;
    bs.color.operation = wgpu::BlendOperation::Add;
    bs.alpha.srcFactor = wgpu::BlendFactor::One;
    bs.alpha.dstFactor = wgpu::BlendFactor::One;
    bs.alpha.operation = wgpu::BlendOperation::Add;

    wgpu::ColorTargetState ct{};
    ct.format = mTargetFmt;
    ct.blend = &bs;
    ct.writeMask = wgpu::ColorWriteMask::All;

    wgpu::FragmentState frag{};
    frag.module = mHaloBlitShader;
    frag.entryPoint = "fs_blit";
    frag.targetCount = 1;
    frag.targets = &ct;

    wgpu::RenderPipelineDescriptor pd{};
    pd.layout = mHaloBlitPL;
    pd.vertex.module = mHaloBlitShader;
    pd.vertex.entryPoint = "vs_fullscreen";
    pd.fragment = &frag;
    pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    pd.depthStencil = nullptr;
    pd.multisample.count = 1;
    mHaloAddPipeline = mGpu.Device().CreateRenderPipeline(&pd);

    mHaloBlitReady = true;
}

// EndFrame composite (Design B): replay the captured halo-source draws into the
// transparent halo buffer, bloom it, and ADDITIVE-blit ONLY the blurred halo
// onto mFrameView (LoadOp::Load — base track preserved). Same encoder, after
// mPass.End() and before mEncoder.Finish(). Clears mHaloDraws (keeps capacity).
void BandRnd::CompositeHaloBloom() {
    if (!mGpuReady || mHaloDraws.empty() || !mFrameView) { mHaloDraws.clear(); return; }

    // Read tunables up front so the neg-control (BLEND=0) can short-circuit to a
    // TRUE no-op. The engine's BloomPass does NOT scale its threshold/blur output
    // by the Run `intensity` arg (only the upsample mip-merge uses a SEPARATE
    // weight), so OutputView() is non-zero even at intensity=0 — i.e. passing
    // blend=0 to Run() would still add the base-mip halo. To honor the contract
    // "RB3_HIGHWAY_BLOOM_BLEND=0 is visually identical to OFF", skip the replay +
    // bloom + composite entirely when blend<=0 (touches NOTHING on mFrameView).
    float thresh = 0.55f;
    float blend = 0.7f;
    {
        const char* t = getenv("RB3_HIGHWAY_BLOOM_THRESH");
        if (t && t[0]) thresh = (float)atof(t);
        const char* b = getenv("RB3_HIGHWAY_BLOOM_BLEND");
        if (b && b[0]) blend = (float)atof(b);
    }
    if (blend <= 0.0f) {
        if (getenv("RB3_RENDER_DBG") || getenv("RB3_TIER2_DBG"))
            fprintf(stderr, "[RB3_HALOBLOOM] f%d blend=0 -> no-op (identical to OFF)\n",
                    mFrameCount);
        mHaloDraws.clear();
        return;
    }

    // Match mDepthView's live size: the replay pass below pairs mHaloView with
    // mDepthView, so the halo target must be the same size as the depth buffer
    // (WindowWidth/Height can be stale after a web canvas resize — see BeginFrame).
    int W = mDepthWidth, H = mDepthHeight;
    if (W <= 0 || H <= 0) { W = mGpu.WindowWidth(); H = mGpu.WindowHeight(); }
    EnsureHaloTarget(W, H);
    EnsureHaloBlitPipeline();
    if (!mHaloView || !mHaloAddPipeline || !mHaloBlitBGL || !mDepthView) {
        mHaloDraws.clear();
        return;
    }

    // (b) Replay pass: clear the halo buffer TRANSPARENT (only halo-source pixels
    //     become non-zero) and clear depth (the captured draws were authored with
    //     depth on). Replay each captured draw verbatim against its pose-baked
    //     scene bind group — NO dynamic offset (the bind group pins mSceneOffset
    //     directly, so dynamicOffsetCount must be 0 to match the live draw).
    {
        wgpu::RenderPassColorAttachment colorAtt{};
        colorAtt.view = mHaloView;
        colorAtt.loadOp = wgpu::LoadOp::Clear;
        colorAtt.storeOp = wgpu::StoreOp::Store;
        colorAtt.clearValue = {0, 0, 0, 0};

        wgpu::RenderPassDepthStencilAttachment depthAtt{};
        depthAtt.view = mDepthView;
        depthAtt.depthLoadOp = wgpu::LoadOp::Clear; depthAtt.depthStoreOp = wgpu::StoreOp::Store;
        depthAtt.depthClearValue = 1.0f;
        depthAtt.stencilLoadOp = wgpu::LoadOp::Clear; depthAtt.stencilStoreOp = wgpu::StoreOp::Store;
        depthAtt.stencilClearValue = 0;

        wgpu::RenderPassDescriptor rp{};
        rp.label = "BandHaloReplay";
        rp.colorAttachmentCount = 1; rp.colorAttachments = &colorAtt;
        rp.depthStencilAttachment = &depthAtt;

        wgpu::RenderPassEncoder pass = mEncoder.BeginRenderPass(&rp);
        for (const HaloDraw& d : mHaloDraws) {
            pass.SetPipeline(d.pipe);
            pass.SetBindGroup(0, d.scene, 0, nullptr);
            pass.SetBindGroup(1, d.mat, 0, nullptr);
            pass.SetBindGroup(2, d.obj, 0, nullptr);
            pass.SetBindGroup(3, d.bone, 0, nullptr);
            pass.SetVertexBuffer(0, d.vbuf, 0, WGPU_WHOLE_SIZE);
            pass.SetIndexBuffer(d.ibuf, wgpu::IndexFormat::Uint16, 0, WGPU_WHOLE_SIZE);
            pass.DrawIndexed(d.indexCount, 1, 0, 0, 0);
        }
        pass.End();
    }

    // (c) Bloom the halo buffer (its own BloomPass instance / mip chain).
    wgpu::TextureView haloView;
    {
        Hmx::Color tint(1.f, 1.f, 1.f, 1.f);
        mHaloBloom.Run(mEncoder, mHaloView, mHaloWidth, mHaloHeight,
                       blend, thresh, tint, mGpu);
        if (mHaloBloom.HasOutput()) haloView = mHaloBloom.OutputView();
    }

    // (d) ADDITIVE composite ONLY: one fullscreen pass on mFrameView
    //     (LoadOp::Load → keep the base frame), no depth. Add ONLY the blurred
    //     halo. NO over-draw, NO blit of mHaloView itself. With blend==0 the
    //     bloom output is black → this is a no-op → visually identical to OFF.
    {
        wgpu::RenderPassColorAttachment colorAtt{};
        colorAtt.view = mFrameView;
        colorAtt.loadOp = wgpu::LoadOp::Load;
        colorAtt.storeOp = wgpu::StoreOp::Store;

        wgpu::RenderPassDescriptor rp{};
        rp.label = "BandHaloBloomComposite";
        rp.colorAttachmentCount = 1; rp.colorAttachments = &colorAtt;
        rp.depthStencilAttachment = nullptr;

        wgpu::RenderPassEncoder pass = mEncoder.BeginRenderPass(&rp);
        if (haloView) {
            wgpu::BindGroupEntry bge[2] = {};
            bge[0].binding = 0; bge[0].textureView = haloView;
            bge[1].binding = 1; bge[1].sampler = mSampler;
            wgpu::BindGroupDescriptor bgd{};
            bgd.layout = mHaloBlitBGL; bgd.entryCount = 2; bgd.entries = bge;
            wgpu::BindGroup bg = mGpu.Device().CreateBindGroup(&bgd);
            pass.SetPipeline(mHaloAddPipeline);
            pass.SetBindGroup(0, bg, 0, nullptr);
            pass.Draw(3);
        }
        pass.End();
    }

    if (getenv("RB3_RENDER_DBG") || getenv("RB3_TIER2_DBG"))
        fprintf(stderr, "[RB3_HALOBLOOM] f%d %dx%d draws=%zu thresh=%.2f blend=%.2f halo=%d\n",
                mFrameCount, mHaloWidth, mHaloHeight, mHaloDraws.size(), thresh, blend,
                haloView != nullptr);

    // (e) Done — keep capacity for next frame.
    mHaloDraws.clear();
}

void BandRnd::DoPostProcess() {
    // Preserve the base post-processor bookkeeping (mPostProcessors is empty on
    // the native backend, but DoWorldEnd/DoPost state stays consistent), then run
    // the mid-frame venue grade composite. Fires once per frame via the
    // mWorldEnded latch in Rnd::EndWorld (the caller).
    Rnd::DoPostProcess();
    if (getenv("RB3_TIER2_DBG"))
        fprintf(stderr, "[RB3_TIER2_DBG] DoPostProcess f%d meshes=%d hasPP=%d flushed=%d toInt=%d\n",
                mFrameCount, mDrawnMeshes, RndPostProc::Current() != nullptr,
                mPostProcFlushed, mRenderedToIntermediate);
    FlushPostProcMidFrame();
}

void BandRnd::ClearDepthForOverlay() {
    if (!mGpuReady) return;
    if (getenv("RB3_TIER2_DBG"))
        fprintf(stderr, "[RB3_TIER2_DBG] ClearDepthForOverlay f%d meshes=%d flushed=%d toInt=%d\n",
                mFrameCount, mDrawnMeshes, mPostProcFlushed, mRenderedToIntermediate);
    // TrackPanel::Draw calls this at the venue->highway boundary. If the venue
    // composite hasn't flushed yet (e.g. a screen whose panel ordering didn't fire
    // EndWorld before the track panel), run it now — FlushPostProcMidFrame both
    // composites the venue and clears depth as part of resuming into the
    // framebuffer, which is exactly this method's intent.
    if (!mPostProcFlushed && mRenderedToIntermediate && !RB3PostProcDisabled() &&
        RndPostProc::Current() && mIntermediateView) {
        FlushPostProcMidFrame();
        return;
    }
    // Otherwise just clear depth (color preserved) in the CURRENT pass so the
    // highway/gems composite over whatever is already drawn (the original
    // note-highway depth fix). Suspend + resume the current target with color
    // LoadOp::Load + depth LoadOp::Clear.
    if (!mInPass || mRtActiveTex) return;    // nothing open / RTT pass — skip
    wgpu::TextureView dst = MainColorTarget();
    if (!dst) return;
    mPass.End(); mInPass = false;

    wgpu::RenderPassColorAttachment colorAtt{};
    colorAtt.view = dst;
    colorAtt.loadOp = wgpu::LoadOp::Load;
    colorAtt.storeOp = wgpu::StoreOp::Store;

    wgpu::RenderPassDepthStencilAttachment depthAtt{};
    depthAtt.view = mDepthView;
    depthAtt.depthLoadOp = wgpu::LoadOp::Clear; depthAtt.depthStoreOp = wgpu::StoreOp::Store;
    depthAtt.depthClearValue = 1.0f;
    depthAtt.stencilLoadOp = wgpu::LoadOp::Clear; depthAtt.stencilStoreOp = wgpu::StoreOp::Store;
    depthAtt.stencilClearValue = 0;

    wgpu::RenderPassDescriptor rp{};
    rp.label = "BandMainPassDepthClear";
    rp.colorAttachmentCount = 1; rp.colorAttachments = &colorAtt;
    rp.depthStencilAttachment = &depthAtt;

    mPass = mEncoder.BeginRenderPass(&rp);
    mInPass = true;
    mPass.SetBindGroup(0, mSceneBindGroup, 0, nullptr);
    mLastSceneCam = nullptr;
}

// ===========================================================================
// Stage 2 postproc grade — PORTED VERBATIM from milo-native-engine
// src/gfx/PostProcPass.cpp:9-166 (uniform struct + WGSL). Lives in its own
// module (vs_fullscreen/fs_postproc) — see Rnd_Wgpu_RB3.h note on why it can't
// share kRB3QuadShaderSource (binding-2 type clash + bloomTex@3).
//
// B+W_film02.pp is a pure grade (saturation -40, contrast +10, black-lift) +
// bloom + vignette + noise — no refract/DOF/chromatic. v1 binds the black
// default to bloomTex@3 and skips the blur (the grade alone removes the smear);
// the screen-blend bloom term is then a no-op (bloom == black).
// ===========================================================================
struct PostProcUniforms {
    float contrast;
    float brightness;
    float saturation;
    float vignetteIntensity;
    float vignetteColor[4];
    float chromaticOffset;
    float chromaticSharpen;
    float posterLevels;
    float posterMin;
    float levelInLo[4];
    float levelInHi[4];
    float levelOutLo[4];
    float levelOutHi[4];
    float bloomIntensity;
    float noiseIntensity;
    float noiseMidtone;
    float flickerMul;
    float bloomColor[4];
    float time;
    // Repurposed tail pad (keeps the struct at 160B so the layout/static_assert
    // are preserved): tiled-noise-texture controls. noiseHasMap selects the
    // textured grain path (1.0) vs the procedural hash fallback (0.0).
    float noiseScaleX;   // was _pad0 — mNoiseBaseScale.x (X tiling)
    float noiseScaleY;   // was _pad1 — mNoiseBaseScale.y (Y tiling)
    float noiseHasMap;   // was _pad2 — 1.0 if a noise bitmap is bound
};
static_assert(sizeof(PostProcUniforms) == 160, "PostProcUniforms must be 160 bytes");

static const char* kRB3PostProcShaderSource = R"WGSL(
struct PostProcUB {
    contrast: f32,
    brightness: f32,
    saturation: f32,
    vignetteIntensity: f32,
    vignetteColor: vec4f,
    chromaticOffset: f32,
    chromaticSharpen: f32,
    posterLevels: f32,
    posterMin: f32,
    levelInLo: vec4f,
    levelInHi: vec4f,
    levelOutLo: vec4f,
    levelOutHi: vec4f,
    bloomIntensity: f32,
    noiseIntensity: f32,
    noiseMidtone: f32,
    flickerMul: f32,
    bloomColor: vec4f,
    time: f32,
    noiseScaleX: f32,
    noiseScaleY: f32,
    noiseHasMap: f32,
};

@group(0) @binding(0) var sceneTex: texture_2d<f32>;
@group(0) @binding(1) var sceneSampler: sampler;
@group(0) @binding(2) var<uniform> pp: PostProcUB;
@group(0) @binding(3) var bloomTex: texture_2d<f32>;
@group(0) @binding(4) var noiseTex: texture_2d<f32>;

// RB3's grain is a *map-domain* gain (mNoiseIntensity ~3.0 on the menu/world
// postprocs is a texel-space multiplier, NOT a screen-space add). Applying the
// raw (n-0.5)*intensity term washed the whole frame gray (the v1 failure mode).
// kNoiseGain attenuates the per-pixel luminance deviation down to film-grain
// magnitude. With gain 0.04 and clamped intensity 3.0 the per-pixel swing is
// ~±0.06 (~±15/255) — a fine, visible, ZERO-MEAN grain (mean luminance
// unchanged → no gray wash, the v1 failure mode). Verified on the static
// song_select menu: grain ON vs OFF shifts mean luminance by <0.5/255 and
// saturation by <0.002 while raising midtone high-frequency variance, matching
// the Wii's midtone-concentrated grain. (0.03 was nearly imperceptible; 0.05
// reads slightly heavy — 0.04 is the balance, with margin on the no-wash gate.)
const kNoiseGain: f32 = 0.04;

struct VOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

@vertex fn vs_fullscreen(@builtin(vertex_index) idx: u32) -> VOut {
    var out: VOut;
    let x = f32(i32(idx & 1u)) * 4.0 - 1.0;
    let y = f32(i32(idx >> 1u)) * 4.0 - 1.0;
    out.pos = vec4f(x, y, 0.0, 1.0);
    out.uv = vec2f((x + 1.0) * 0.5, (1.0 - y) * 0.5);
    return out;
}

@fragment fn fs_postproc(in: VOut) -> @location(0) vec4f {
    let texSize = vec2f(textureDimensions(sceneTex));

    var color: vec3f;
    if (pp.chromaticOffset > 0.0) {
        let offset = pp.chromaticOffset / texSize;
        let r = textureSample(sceneTex, sceneSampler, in.uv + vec2f(offset.x, 0.0)).r;
        let g = textureSample(sceneTex, sceneSampler, in.uv).g;
        let b = textureSample(sceneTex, sceneSampler, in.uv - vec2f(offset.x, 0.0)).b;
        if (pp.chromaticSharpen > 0.5) {
            let center = textureSample(sceneTex, sceneSampler, in.uv).rgb;
            let blur = vec3f(r, g, b);
            color = center + (center - blur) * 1.5;
        } else {
            color = vec3f(r, g, b);
        }
    } else {
        color = textureSample(sceneTex, sceneSampler, in.uv).rgb;
    }

    let inRange = max(pp.levelInHi.rgb - pp.levelInLo.rgb, vec3f(0.001));
    let normalized = clamp((color - pp.levelInLo.rgb) / inRange, vec3f(0.0), vec3f(1.0));
    color = mix(pp.levelOutLo.rgb, pp.levelOutHi.rgb, normalized);

    // Match Xbox's non-linear contrast formula (from RndColorXfm::AdjustContrast)
    var contrastMul: f32;
    let contrastNorm = pp.contrast / 100.0;
    if (contrastNorm > 0.0) {
        contrastMul = 1.0 / (contrastNorm * -0.9921875 + 1.0);
    } else {
        contrastMul = -(contrastNorm * -0.992126 - 1.0);
    }
    let contrastOff = (1.0 - contrastMul) * 0.5;
    color = color * contrastMul + contrastOff;
    // Brightness: match Xbox formula
    let brightnessAdj = (pp.brightness + 100.0) / 200.0 - 0.5;
    color = color + brightnessAdj;

    let luma = dot(color, vec3f(0.2126, 0.7152, 0.0722));
    color = mix(vec3f(luma), color, 1.0 + pp.saturation / 100.0);

    if (pp.posterLevels > 1.0) {
        let levels = pp.posterLevels;
        let intensity = max(max(color.r, color.g), color.b);
        if (intensity >= pp.posterMin) {
            color = floor(color * levels + 0.5) / levels;
        }
    }

    if (pp.vignetteIntensity > 0.0) {
        let center = in.uv - 0.5;
        let dist = length(center) * 1.414;
        let vig = 1.0 - smoothstep(0.4, 1.0, dist) * pp.vignetteIntensity;
        color = mix(pp.vignetteColor.rgb, color, vig);
    }

    // Flicker: time-based brightness modulation
    if (pp.flickerMul != 1.0) {
        color = color * pp.flickerMul;
    }

    // Noise/grain: tiled noise TEXTURE (the real RB3 path) with a procedural
    // hash fallback for postprocs that set an intensity but ship no bitmap.
    if (pp.noiseIntensity != 0.0) {
        var n: f32;
        if (pp.noiseHasMap > 0.5) {
            // Tiled noise bitmap. mNoiseBaseScale is the X/Y tiling count; sample
            // by screen-UV * scale (Repeat sampler) so it reads as fine,
            // stationary film grain — NOT a moving wash (Wii mNoiseStationary).
            let nuv = in.uv * vec2f(pp.noiseScaleX, pp.noiseScaleY);
            n = textureSample(noiseTex, sceneSampler, nuv).r;
        } else {
            // Procedural fallback (no bitmap on this postproc).
            let px = in.uv * vec2f(textureDimensions(sceneTex));
            n = fract(sin(dot(px + pp.time * 43.17, vec2f(12.9898, 78.233))) * 43758.5453);
        }
        // KEY no-gray-wash fix: scale the per-pixel deviation DOWN (kNoiseGain)
        // and clamp the map-domain intensity. Zero-mean about 0.5 → mean
        // luminance unchanged. The fallback shares this attenuation so it can
        // never reproduce the v1 intensity-3.0 blizzard either.
        let grain = (n - 0.5) * min(abs(pp.noiseIntensity), 3.0) * kNoiseGain;
        if (pp.noiseMidtone > 0.5) {
            // Overlay-style midtone weight: pins grain OFF at black (luma 0) and
            // white (luma 1), peaks at luma 0.5 — keeps shadows/highlights clean.
            let nl = dot(color, vec3f(0.2126, 0.7152, 0.0722));
            let midtoneMask = 4.0 * nl * (1.0 - nl);
            color = color + grain * midtoneMask;
        } else {
            color = color + grain;
        }
    }

    if (pp.bloomIntensity > 0.0) {
        let bloom = textureSample(bloomTex, sceneSampler, in.uv).rgb;
        // Clamp intensity to prevent overpowering bloom from aggressive game data
        let clampedIntensity = min(pp.bloomIntensity, 1.0);
        let bloomContrib = bloom * clampedIntensity * pp.bloomColor.rgb;
        // ADDITIVE blend (not screen): screen-blend `1-(1-c)*(1-b*k)` lifts DARK
        // pixels hardest (the (1-c) term is large there) → the whole dark
        // background washes milky-bright (the v1-bloom failure mode on the dark-blue
        // menu). Additive `c + b*k` instead lifts every pixel by the SAME absolute
        // amount, so a tight highlight halo reads as a glow while the dark
        // background stays dark. bloomColor.a carries the blend factor (CPU-set;
        // see RunPostProcComposite) so it's tunable without a uniform-layout change.
        color = color + bloomContrib * pp.bloomColor.a;
    }

    return vec4f(clamp(color, vec3f(0.0), vec3f(1.0)), 1.0);
}
)WGSL";

// ===========================================================================
// Stage 2: postproc render-to-texture composite (§4 of the RTT plan).
//
// MainColorTarget(): the color attachment the main scene draws into. When a
// postproc is active the scene renders into the offscreen intermediate (so it
// can be graded as a whole); otherwise straight into the framebuffer (the
// default, canary-preserving path). RTT-resume (EndDrawTarget) routes through
// this so a mid-frame RndCam::TargetTex draw resumes into the right surface.
// ===========================================================================
wgpu::TextureView BandRnd::MainColorTarget() {
    // Tier 2: once the mid-frame venue composite has flushed onto the framebuffer,
    // the main pass renders into the FRAMEBUFFER (the graded venue is already
    // there). Any further mid-frame RTT resume after the flush must therefore
    // resume into mFrameView, not the now-stale intermediate.
    if (mPostProcFlushed)
        return mFrameView;
    if (!RB3PostProcDisabled() && RndPostProc::Current() && mIntermediateView && mRenderedToIntermediate)
        return mIntermediateView;
    return mFrameView;
}

// (Re)create the offscreen intermediate at w x h using mTargetFmt (RGBA8
// headless / BGRA8 windowed — NEVER hardcoded). usage RenderAttachment (the
// scene renders into it) | TextureBinding (the composite samples it). Recreate
// on size change.
void BandRnd::EnsureIntermediate(int w, int h) {
    if (!mGpuReady || w <= 0 || h <= 0) return;
    if (mIntermediateTex && mIntermediateView &&
        mIntermediateWidth == w && mIntermediateHeight == h) {
        return;  // already sized correctly
    }
    wgpu::TextureDescriptor td{};
    td.label = "RB3PostProcIntermediate";
    td.size = {(uint32_t)w, (uint32_t)h, 1};
    td.format = mTargetFmt;   // matches the framebuffer the composite writes to
    td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
    td.mipLevelCount = 1;
    wgpu::Texture t = mGpu.Device().CreateTexture(&td);
    if (!t) return;
    mIntermediateTex = t;
    mIntermediateView = t.CreateView();
    mIntermediateWidth = w;
    mIntermediateHeight = h;
}

// (Re)create the "BandDepth" depth/stencil attachment at w x h. Recreated only on
// a size change. BeginFrame sizes this to the ACTUAL acquired color texture each
// frame: on web the browser auto-resizes the canvas backing store (and the
// swapchain texture AcquireNextFrame returns) on a window/devtools resize, while
// GpuDevice::WindowWidth/Height only moves on an explicit ResizeSurface the web
// path never calls. Sizing depth off the live color texture keeps the two in
// lockstep on every platform — without a stale mismatch aborting BandMainPass
// ("depth stencil attachment size does not match the other attachments").
void BandRnd::EnsureDepth(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (mDepthTex && mDepthView && mDepthWidth == w && mDepthHeight == h)
        return;  // already sized correctly
    wgpu::TextureDescriptor dd{};
    dd.label = "BandDepth";
    dd.size = {(uint32_t)w, (uint32_t)h, 1};
    dd.format = wgpu::TextureFormat::Depth24PlusStencil8;
    dd.usage = wgpu::TextureUsage::RenderAttachment;
    dd.mipLevelCount = 1;
    wgpu::Texture t = mGpu.Device().CreateTexture(&dd);
    if (!t) return;
    mDepthTex = t;
    mDepthView = t.CreateView();
    mDepthWidth = w;
    mDepthHeight = h;
}

// Grade the intermediate onto `dst` (the framebuffer): a single fullscreen
// triangle (vs_fullscreen, no vbuf) running fs_postproc — ported verbatim from
// gfx/PostProcPass.cpp. No blend, no depth, LoadOp::Clear (full-screen
// overwrite — no Load reliance, web BGRA8 safe). Reads grade params from
// RndPostProc::Current() via the HX_NATIVE accessors. Bloom is v1-skipped:
// mBlackView is bound to bloomTex@3, so the screen-blend bloom term is a no-op.
static bool RB3PostProcDisabled() {
    static int s = -1;
    if (s < 0) { const char* e = getenv("RB3_PP_OFF"); s = (e && e[0] && e[0] != '0') ? 1 : 0; }
    return s != 0;
}

// A5 pipeline pre-warm (default ON, opt-out RB3_PIPELINE_PREWARM_OFF). When ON,
// the first few rendered frames eagerly create the enumerable RB3 draw-time
// pipeline set so the splash->main_hub venue-build frame (native f≈13: pipeMs
// 87 ms / pipeN 13; web: ~120 ms async pipeline-compile residue) finds them all
// cache-hit. Visual no-op: pre-creating a cache entry the real draw would have
// created anyway cannot change pixels — only WHEN the compile is paid.
static bool RB3PipelinePrewarmDisabled() {
    static int s = -1;
    if (s < 0) {
        const char* e = getenv("RB3_PIPELINE_PREWARM_OFF");
        s = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return s != 0;
}

void BandRnd::RunPostProcComposite(wgpu::TextureView dst) {
    if (!mGpuReady || !dst || !mIntermediateView) return;
    RndPostProc* pp = RndPostProc::Current();
    if (!pp) return;

    EnsureQuadPipeline();
    if (!mQuadPostShader || !mPostProcUB) return;

    // --- Fill the uniform block (ported from PostProcPass::Run :260-303) ---
    PostProcUniforms uni{};
    const RndColorXfm& cxfm = pp->GetColorXfm();
    uni.contrast = cxfm.mContrast;
    uni.brightness = cxfm.mBrightness;
    uni.saturation = cxfm.mSaturation;
    uni.vignetteIntensity = pp->GetVignetteIntensity();
    const Hmx::Color& vc = pp->GetVignetteColor();
    uni.vignetteColor[0] = vc.red;   uni.vignetteColor[1] = vc.green;
    uni.vignetteColor[2] = vc.blue;  uni.vignetteColor[3] = vc.alpha;
    uni.chromaticOffset = pp->GetChromaticAberrationOffset();
    uni.chromaticSharpen = pp->GetChromaticSharpen() ? 1.0f : 0.0f;
    uni.posterLevels = pp->GetPosterLevels();
    uni.posterMin = pp->GetPosterMin();

    uni.levelInLo[0] = cxfm.mLevelInLo.red;   uni.levelInLo[1] = cxfm.mLevelInLo.green;
    uni.levelInLo[2] = cxfm.mLevelInLo.blue;  uni.levelInLo[3] = 0;
    uni.levelInHi[0] = cxfm.mLevelInHi.red;   uni.levelInHi[1] = cxfm.mLevelInHi.green;
    uni.levelInHi[2] = cxfm.mLevelInHi.blue;  uni.levelInHi[3] = 1;
    uni.levelOutLo[0] = cxfm.mLevelOutLo.red; uni.levelOutLo[1] = cxfm.mLevelOutLo.green;
    uni.levelOutLo[2] = cxfm.mLevelOutLo.blue; uni.levelOutLo[3] = 0;
    uni.levelOutHi[0] = cxfm.mLevelOutHi.red; uni.levelOutHi[1] = cxfm.mLevelOutHi.green;
    uni.levelOutHi[2] = cxfm.mLevelOutHi.blue; uni.levelOutHi[3] = 1;

    // V2 BLOOM. Run the threshold/blur/upsample mip chain on the intermediate
    // (the fully-rendered, pre-grade scene) and additive-blend its OutputView()
    // into bloomTex@3 (the shader branch at kRB3PostProcShaderSource's
    // `bloomIntensity > 0.0`). Mirrors gfx/PostProcPass.cpp:251-257 / :330-331
    // (DC3's path) but with two corrections for the rb3 backend:
    //
    //   THRESHOLD SCALE. RndPostProc::mBloomThreshold is in the Wii's pre-tonemap
    //   luminance scale (default 4.0; world.pp + subwayhangout.pp both ship 10.0),
    //   NOT the [0,1] normalized luma our composite operates in. PostProcPass only
    //   FLOORS it (max(thr,0.7)), so a raw 10.0 passes straight through and
    //   fs_bloom_threshold's `luma - 10 + knee` is negative for every SDR pixel →
    //   bloom NEVER fires (verified: world.pp raw thr=10 → zero visible bloom). And
    //   fs_bloom_threshold's soft knee is `knee = threshold*0.5`, so the EFFECTIVE
    //   onset is threshold*0.5 — a nominal 0.9 actually blooms everything above 0.45
    //   (washes the whole frame, verified). So we IGNORE the inflated Wii value and
    //   pass a FIXED normalized cutoff: kBloomThreshold 1.8 → onset 0.9, i.e. only
    //   the brightest ~few-% highlights bloom (ground-truth SP/venue frames bloom
    //   only the brightest ~2%, p99 luma ~0.75). Combined with the small additive
    //   blend (bloomColor.a, set below) this is a tight halo, never a wash/blowout.
    //
    // The mip chain records its own render passes into mEncoder; this runs AFTER
    // the main pass closed (EndFrame ends mPass before calling us) and BEFORE the
    // composite's BeginRenderPass below, so the intermediate is fully written and
    // the bloom textures are ready when the composite samples them.
    static const float kBloomThreshold = 1.8f;   // → fs_bloom_threshold onset ~0.9
    float bloomIntensity = std::min(pp->GetBloomIntensity(), 1.0f);
    float bloomThreshold = kBloomThreshold;
    wgpu::TextureView bloomView = mBlackView;   // inert default (branch won't sample)
    {
        // RB3_BLOOM_OFF: A/B isolation — disable ONLY the bloom term while keeping
        // the rest of the composite byte-identical (so a same-scene bloom-on vs
        // bloom-off diff measures exactly the bloom, not the grade/grain).
        static int s = -1;
        if (s < 0) { const char* e = getenv("RB3_BLOOM_OFF"); s = (e && e[0] && e[0] != '0') ? 1 : 0; }
        if (s) bloomIntensity = 0.0f;
    }
    // RB3_BLOOM_THRESH / RB3_BLOOM_SCALE: tuning overrides (sweep without rebuild).
    {
        const char* t = getenv("RB3_BLOOM_THRESH");
        if (t && t[0]) bloomThreshold = (float)atof(t);
        const char* sc = getenv("RB3_BLOOM_SCALE");
        if (sc && sc[0]) bloomIntensity *= (float)atof(sc);
    }
    if (bloomIntensity > 0.0f) {
        mBloom.Run(mEncoder, mIntermediateView, mIntermediateWidth, mIntermediateHeight,
                   bloomIntensity, bloomThreshold, pp->GetBloomColor(), mGpu);
        if (mBloom.HasOutput()) bloomView = mBloom.OutputView();
    }
    uni.bloomIntensity = bloomIntensity;
    const Hmx::Color& bc = pp->GetBloomColor();
    uni.bloomColor[0] = bc.red; uni.bloomColor[1] = bc.green;
    uni.bloomColor[2] = bc.blue;
    // bloomColor.a = additive blend factor for the composite (see the bloom branch
    // in kRB3PostProcShaderSource). Kept low so only true highlights produce a
    // visible halo and the dark background never washes. RB3_BLOOM_BLEND overrides.
    {
        // 0.02: at threshold-onset 0.9 the bloom output still has wide low-frequency
        // spread (the BloomPass mip chain blurs highlights broadly), so the additive
        // factor must stay small — verified the dark menu background lifts <1.5/255
        // (no wash) while bright text/album-art/SP highlights gain a visible halo.
        float blend = 0.02f;
        const char* b = getenv("RB3_BLOOM_BLEND");
        if (b && b[0]) blend = (float)atof(b);
        uni.bloomColor[3] = blend;
    }

    uni.time = (float)mFrameCount;
    // V2 NOISE GRAIN. RB3's postproc noise is a TILED NOISE TEXTURE (mNoiseMap +
    // mNoiseBaseScale tiling, midtone-overlay blended) — a SUBTLE film grain on
    // the Wii. v1 zeroed it because the procedural hash fallback at RB3's real
    // intensity (~3.0) washed the frame gray. We now bind the real bitmap and
    // attenuate the deviation (kNoiseGain, in the shader) so it is grain, not a
    // wash. A postproc with intensity!=0 but no bitmap falls back to the
    // (same-attenuated) procedural path. With intensity==0 the branch is skipped
    // entirely → still an exact identity passthrough for neutral env postprocs,
    // keeping the no-postproc/zero-noise canary pixel-clean.
    RndTex* noiseMap = pp->GetNoiseMap();
    wgpu::TextureView noiseView;
    if (noiseMap) noiseView = UploadRndTexIfNeeded(mGpu, noiseMap);
    if (noiseView) {
        uni.noiseHasMap = 1.0f;
        const Vector2& ns = pp->GetNoiseBaseScale();
        uni.noiseScaleX = ns.x;
        uni.noiseScaleY = ns.y;
    } else {
        uni.noiseHasMap = 0.0f;       // procedural fallback (still attenuated)
        uni.noiseScaleX = 0.0f;
        uni.noiseScaleY = 0.0f;
        noiseView = mBlackView;       // a valid view must be bound regardless
    }
    uni.noiseIntensity = pp->GetNoiseIntensity();
    uni.noiseMidtone = pp->GetNoiseMidtone() ? 1.0f : 0.0f;
    // RB3_NOISE_OFF: A/B isolation — disable ONLY the grain term while keeping
    // the rest of the composite byte-identical (so a same-scene grain-on vs
    // grain-off diff measures exactly the grain, not scene motion / the grade).
    {
        static int s = -1;
        if (s < 0) { const char* e = getenv("RB3_NOISE_OFF"); s = (e && e[0] && e[0] != '0') ? 1 : 0; }
        if (s) uni.noiseIntensity = 0.0f;
    }

    // flicker disabled (separate follow-up).
    uni.flickerMul = 1.0f;

    mGpu.Queue().WriteBuffer(mPostProcUB, 0, &uni, sizeof(uni));

    // --- Pipeline (cached): format mTargetFmt, no blend, no depth ---
    if (!mQuadPostPipeline) {
        wgpu::ColorTargetState ct{};
        ct.format = mTargetFmt;
        ct.writeMask = wgpu::ColorWriteMask::All;   // no blend (opaque overwrite)

        wgpu::FragmentState frag{};
        frag.module = mQuadPostShader;
        frag.entryPoint = "fs_postproc";
        frag.targetCount = 1;
        frag.targets = &ct;

        wgpu::RenderPipelineDescriptor pd{};
        pd.layout = mQuadPostPL;
        pd.vertex.module = mQuadPostShader;
        pd.vertex.entryPoint = "vs_fullscreen";
        pd.fragment = &frag;
        pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        pd.depthStencil = nullptr;   // composite has no depth attachment
        pd.multisample.count = 1;    // rb3 backend is single-sampled
        mQuadPostPipeline = mGpu.Device().CreateRenderPipeline(&pd);
    }
    if (!mQuadPostPipeline) return;

    wgpu::BindGroupEntry bge[5] = {};
    bge[0].binding = 0; bge[0].textureView = mIntermediateView;
    bge[1].binding = 1; bge[1].sampler = mSampler;
    bge[2].binding = 2; bge[2].buffer = mPostProcUB; bge[2].offset = 0; bge[2].size = sizeof(PostProcUniforms);
    bge[3].binding = 3; bge[3].textureView = bloomView;    // V2 bloom output (or black when inactive)
    bge[4].binding = 4; bge[4].textureView = noiseView;    // V2 tiled noise (or black fallback)
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = mQuadPostBGL;
    bgd.entryCount = 5;
    bgd.entries = bge;
    wgpu::BindGroup bg = mGpu.Device().CreateBindGroup(&bgd);

    wgpu::RenderPassColorAttachment colorAtt{};
    colorAtt.view = dst;
    colorAtt.loadOp = wgpu::LoadOp::Clear;     // full-screen overwrite (no Load)
    colorAtt.storeOp = wgpu::StoreOp::Store;
    colorAtt.clearValue = {0, 0, 0, 1};

    wgpu::RenderPassDescriptor rp{};
    rp.label = "BandPostProcComposite";
    rp.colorAttachmentCount = 1; rp.colorAttachments = &colorAtt;
    rp.depthStencilAttachment = nullptr;

    wgpu::RenderPassEncoder pass = mEncoder.BeginRenderPass(&rp);
    pass.SetPipeline(mQuadPostPipeline);
    pass.SetBindGroup(0, bg, 0, nullptr);
    pass.Draw(3);
    pass.End();

    // RB3_RENDER_DBG: prove the composite fires ONLY on postproc screens
    // (song_select / etched), NEVER on plain gameplay or main_hub. Log on every
    // CHANGE of the active postproc object (name + full grade) plus a periodic
    // heartbeat, so the verify can confirm B+W_film02 is the active grade.
    if (getenv("RB3_RENDER_DBG")) {
        static RndPostProc* sLastPP = nullptr;
        RndCam* cur = RndCam::sCurrent;
        const char* camName = (cur && cur->Name()) ? cur->Name() : "<none>";
        if (pp != sLastPP) {
            sLastPP = pp;
            // Log the active postproc + its grade whenever Current() changes —
            // proves the composite fires ONLY on postproc-active screens, and
            // which grade (name + sat/contrast/levels/vignette) is applied.
            fprintf(stderr,
                "[RB3_RENDER_DBG] postproc composite active f%d pp='%s' cam=%s sat=%.1f "
                "contrast=%.1f bright=%.1f vignette=%.2f outLo=(%.3f,%.3f,%.3f) %dx%d "
                "noise[int=%.2f midtone=%.0f hasMap=%.0f scale=(%.1f,%.1f)] "
                "bloom[int=%.2f thresh=%.2f color=(%.2f,%.2f,%.2f) raw=%.2f rawThr=%.2f]\n",
                mFrameCount, pp->Name() ? pp->Name() : "?", camName,
                uni.saturation, uni.contrast, uni.brightness, uni.vignetteIntensity,
                uni.levelOutLo[0], uni.levelOutLo[1], uni.levelOutLo[2],
                mIntermediateWidth, mIntermediateHeight,
                uni.noiseIntensity, uni.noiseMidtone, uni.noiseHasMap,
                uni.noiseScaleX, uni.noiseScaleY,
                uni.bloomIntensity, bloomThreshold,
                uni.bloomColor[0], uni.bloomColor[1], uni.bloomColor[2],
                pp->GetBloomIntensity(), pp->GetBloomThreshold());
        }
    }
}

// ===========================================================================
// Shared 2D quad pipeline infra (§3 of the RTT engine plan).
//
// ONE WGSL module holds every quad entry point so Stage 2's postproc composite
// reuses the same shader handle:
//   - vs_rect            : explicit 6-vertex NDC quad (positions mapped CPU-side
//                          in DrawRect; passes uv + per-vertex color through).
//   - fs_rect            : textured/color-modulated rect — tex*mod*vtxColor, with
//                          colorMod==kColorModAlphaUnpackModulate(2) sampling the
//                          diffuse's ALPHA as a grayscale mask (v1 approx).
//   - fs_rect_notex      : mod*vtxColor (base layer has a null diffuse).
//   - vs_fullscreen      : Stage-2 fullscreen-triangle (no vbuf) — added later.
//   - fs_postproc        : Stage-2 grade fragment — added later.
//
// RectUB (32B, group 0 binding 2): mod (vec4) + flags (uvec4; only .x =
// colorMod is read). mod = mat->GetColor() * paramColor — the KEY divergence
// from dc3's DrawRect2D (which ignores matColor and would yield NO tint here,
// because Compose passes a white param color and sets the real tint via
// sMat->SetColor()).
// ===========================================================================
static const char* kRB3QuadShaderSource = R"WGSL(
struct VertexRect {
    @location(0) pos: vec2f,
    @location(1) uv: vec2f,
    @location(2) color: vec4f,
};

struct VSOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
    @location(1) color: vec4f,
};

struct RectUB {
    modColor: vec4f,
    flags: vec4u,   // flags.x = colorMod
};

@group(0) @binding(0) var rectTex: texture_2d<f32>;
@group(0) @binding(1) var rectSampler: sampler;
@group(0) @binding(2) var<uniform> rectUB: RectUB;

@vertex fn vs_rect(in: VertexRect) -> VSOut {
    var out: VSOut;
    out.pos = vec4f(in.pos, 0.0, 1.0);
    out.uv = in.uv;
    out.color = in.color;
    return out;
}

@fragment fn fs_rect(in: VSOut) -> @location(0) vec4f {
    let tex = textureSample(rectTex, rectSampler, in.uv);
    // colorMod == kColorModAlphaUnpackModulate (2): treat the diffuse alpha as a
    // grayscale mask (v1 approximation of the Wii alpha-unpack-modulate path).
    var src = tex;
    if (rectUB.flags.x == 2u) {
        src = vec4f(tex.a, tex.a, tex.a, tex.a);
    }
    return src * rectUB.modColor * in.color;
}

@fragment fn fs_rect_notex(in: VSOut) -> @location(0) vec4f {
    return rectUB.modColor * in.color;
}
)WGSL";

// CPU mirror of the 32-byte RectUB (matches the WGSL struct std140 layout:
// vec4 + uvec4 = 16 + 16 = 32 bytes).
struct RB3RectUB {
    float mod[4];
    uint32_t flags[4];
};

// CPU mirror of the per-vertex 2D quad layout (matches vs_rect inputs).
struct RB3RectVertex {
    float pos[2];
    float uv[2];
    float color[4];
};

void BandRnd::EnsureQuadPipeline() {
    if (mQuadReady) return;
    auto& dev = mGpu.Device();

    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = kRB3QuadShaderSource;
    wgpu::ShaderModuleDescriptor smDesc{};
    smDesc.nextInChain = &wgsl;
    mQuadShader = dev.CreateShaderModule(&smDesc);

    // Rect bind-group layout: tex@0, sampler@1, RectUB@2.
    wgpu::BindGroupLayoutEntry entries[3] = {};
    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Fragment;
    entries[0].texture.sampleType = wgpu::TextureSampleType::Float;
    entries[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    entries[1].binding = 1;
    entries[1].visibility = wgpu::ShaderStage::Fragment;
    entries[1].sampler.type = wgpu::SamplerBindingType::Filtering;
    entries[2].binding = 2;
    entries[2].visibility = wgpu::ShaderStage::Fragment;
    entries[2].buffer.type = wgpu::BufferBindingType::Uniform;
    entries[2].buffer.minBindingSize = sizeof(RB3RectUB);

    wgpu::BindGroupLayoutDescriptor bglDesc{};
    bglDesc.entryCount = 3;
    bglDesc.entries = entries;
    mQuadRectBGL = dev.CreateBindGroupLayout(&bglDesc);

    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &mQuadRectBGL;
    mQuadRectPL = dev.CreatePipelineLayout(&plDesc);

    wgpu::BufferDescriptor vbDesc{};
    vbDesc.size = 6 * sizeof(RB3RectVertex);
    vbDesc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
    mQuadVertexBuffer = dev.CreateBuffer(&vbDesc);

    wgpu::BufferDescriptor ubDesc{};
    ubDesc.size = sizeof(RB3RectUB);
    ubDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    mRectUB = dev.CreateBuffer(&ubDesc);

    // --- Stage 2: postproc grade module + bind-group layout + UB ---
    wgpu::ShaderSourceWGSL ppWgsl{};
    ppWgsl.code = kRB3PostProcShaderSource;
    wgpu::ShaderModuleDescriptor ppSmDesc{};
    ppSmDesc.nextInChain = &ppWgsl;
    mQuadPostShader = dev.CreateShaderModule(&ppSmDesc);

    // sceneTex@0, sampler@1, PostProcUB@2 (160B, minBindingSize=160),
    // bloomTex@3, noiseTex@4 (V2 tiled grain).
    wgpu::BindGroupLayoutEntry ppEntries[5] = {};
    ppEntries[0].binding = 0;
    ppEntries[0].visibility = wgpu::ShaderStage::Fragment;
    ppEntries[0].texture.sampleType = wgpu::TextureSampleType::Float;
    ppEntries[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    ppEntries[1].binding = 1;
    ppEntries[1].visibility = wgpu::ShaderStage::Fragment;
    ppEntries[1].sampler.type = wgpu::SamplerBindingType::Filtering;
    ppEntries[2].binding = 2;
    ppEntries[2].visibility = wgpu::ShaderStage::Fragment;
    ppEntries[2].buffer.type = wgpu::BufferBindingType::Uniform;
    ppEntries[2].buffer.minBindingSize = sizeof(PostProcUniforms);  // 160
    ppEntries[3].binding = 3;
    ppEntries[3].visibility = wgpu::ShaderStage::Fragment;
    ppEntries[3].texture.sampleType = wgpu::TextureSampleType::Float;
    ppEntries[3].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    ppEntries[4].binding = 4;
    ppEntries[4].visibility = wgpu::ShaderStage::Fragment;
    ppEntries[4].texture.sampleType = wgpu::TextureSampleType::Float;
    ppEntries[4].texture.viewDimension = wgpu::TextureViewDimension::e2D;

    wgpu::BindGroupLayoutDescriptor ppBglDesc{};
    ppBglDesc.entryCount = 5;
    ppBglDesc.entries = ppEntries;
    mQuadPostBGL = dev.CreateBindGroupLayout(&ppBglDesc);

    wgpu::PipelineLayoutDescriptor ppPlDesc{};
    ppPlDesc.bindGroupLayoutCount = 1;
    ppPlDesc.bindGroupLayouts = &mQuadPostBGL;
    mQuadPostPL = dev.CreatePipelineLayout(&ppPlDesc);

    wgpu::BufferDescriptor ppUbDesc{};
    ppUbDesc.size = sizeof(PostProcUniforms);
    ppUbDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    mPostProcUB = dev.CreateBuffer(&ppUbDesc);

    mQuadReady = true;
}

// Get-or-create a rect RenderPipeline keyed on (format, blend, hasDepth, isPost,
// hasTex). The composite (Stage 2) and repeated DrawRect calls share the cache
// so we never CreateRenderPipeline per invocation. `notex` selects the
// fs_rect_notex entry (base layer, null diffuse).
static uint64_t RB3QuadPipeKey(wgpu::TextureFormat fmt, WgpuBlend blend,
                               bool hasDepth, bool isPost, bool notex) {
    return ((uint64_t)(uint32_t)fmt << 8) | ((uint64_t)(uint32_t)blend << 3) |
           ((uint64_t)(hasDepth ? 1 : 0) << 2) | ((uint64_t)(isPost ? 1 : 0) << 1) |
           (uint64_t)(notex ? 1 : 0);
}

void BandRnd::DrawRect(const Hmx::Rect& rect, const Hmx::Color& paramColor,
                       RndMat* mat, const Hmx::Color* topRight,
                       const Hmx::Color* botLeft) {
    if (!mGpuReady || !mInPass) return;

    // CRITICAL RTT begin-hook (mirrors DrawMesh ~:1188): Compose calls DrawRect
    // BEFORE any DrawMesh, so the lazy begin-redirect that DrawMesh normally
    // performs has not run yet. If the current cam targets an RTT tex we haven't
    // redirected to, open the RT pass now — otherwise the outfit tint paints the
    // MAIN framebuffer instead of the RTT diffuse texture.
    if (!RB3RttDisabled() && RndCam::sCurrent) {
        RndTex* tt = RndCam::sCurrent->TargetTex();
        if (tt && tt != mRtActiveTex) BeginDrawTarget(tt);
    }
    if (!mInPass) return;   // BeginDrawTarget bailed and left no open pass

    EnsureQuadPipeline();
    if (!mQuadShader || !mQuadVertexBuffer || !mRectUB) return;

    // Rect is absolute Rnd-PIXEL space (e.g. 0..Width x 0..Height). Map to NDC
    // via TheRnd->Width()/Height() — NOT the GPU framebuffer size.
    float w = (float)Width();
    float h = (float)Height();
    if (w <= 0.0f || h <= 0.0f) return;

    float x0 = rect.x / w * 2.0f - 1.0f;
    float y0 = 1.0f - rect.y / h * 2.0f;
    float x1 = (rect.x + rect.w) / w * 2.0f - 1.0f;
    float y1 = 1.0f - (rect.y + rect.h) / h * 2.0f;

    // Per-vertex color carries the optional top-right / bottom-left gradient
    // (Compose always passes white + null gradients; the real tint is the UB
    // mod). cTL = paramColor; cBR averaged.
    float cTL[4] = { paramColor.red, paramColor.green, paramColor.blue, paramColor.alpha };
    float cTR[4], cBL[4], cBR[4];
    if (topRight) { cTR[0]=topRight->red; cTR[1]=topRight->green; cTR[2]=topRight->blue; cTR[3]=topRight->alpha; }
    else          { std::memcpy(cTR, cTL, sizeof(cTL)); }
    if (botLeft)  { cBL[0]=botLeft->red;  cBL[1]=botLeft->green;  cBL[2]=botLeft->blue;  cBL[3]=botLeft->alpha; }
    else          { std::memcpy(cBL, cTL, sizeof(cTL)); }
    for (int i = 0; i < 4; i++) cBR[i] = (cTR[i] + cBL[i]) * 0.5f;

    RB3RectVertex verts[6] = {
        {{x0, y0}, {0, 0}, {cTL[0], cTL[1], cTL[2], cTL[3]}},
        {{x0, y1}, {0, 1}, {cBL[0], cBL[1], cBL[2], cBL[3]}},
        {{x1, y0}, {1, 0}, {cTR[0], cTR[1], cTR[2], cTR[3]}},
        {{x1, y0}, {1, 0}, {cTR[0], cTR[1], cTR[2], cTR[3]}},
        {{x0, y1}, {0, 1}, {cBL[0], cBL[1], cBL[2], cBL[3]}},
        {{x1, y1}, {1, 1}, {cBR[0], cBR[1], cBR[2], cBR[3]}},
    };
    mGpu.Queue().WriteBuffer(mQuadVertexBuffer, 0, verts, sizeof(verts));

    // Modulation = mat->GetColor() * paramColor. THE key DC3 divergence:
    // Compose passes a white paramColor and sets the real tint via
    // sMat->SetColor(), so the modulation MUST fold mat->GetColor().
    int colorMod = 0;
    Hmx::Color matCol(1.0f, 1.0f, 1.0f, 1.0f);
    if (mat) {
        matCol = mat->GetColor();
        colorMod = (int)mat->mColorModFlags;
    }
    RB3RectUB ub{};
    ub.mod[0] = matCol.red   * paramColor.red;
    ub.mod[1] = matCol.green * paramColor.green;
    ub.mod[2] = matCol.blue  * paramColor.blue;
    ub.mod[3] = matCol.alpha * paramColor.alpha;
    ub.flags[0] = (uint32_t)colorMod;
    mGpu.Queue().WriteBuffer(mRectUB, 0, &ub, sizeof(ub));

    // Diffuse: GetRB3TexView(mat->GetDiffuseTex()), uploading on first use, with
    // mWhiteView fallback. The base layer has a null diffuse → fs_rect_notex.
    bool hasTex = false;
    wgpu::TextureView texView;
    RndTex* diffuse = mat ? mat->GetDiffuseTex() : nullptr;
    if (diffuse) {
        texView = GetRB3TexView(diffuse);
        if (!texView) texView = UploadRndTexIfNeeded(mGpu, diffuse);
        if (texView) hasTex = true;
    }
    if (!hasTex) texView = mWhiteView;

    // Blend via the shared MapBlend; target format/depth per the ACTIVE pass.
    WgpuBlend blend = WgpuBlend::Src;
    if (mat) {
        int b = (int)mat->GetBlend();
        if (b >= 0 && b <= 10) blend = (WgpuBlend)b;
    }
    bool rtPass = (mRtActiveTex != nullptr);
    wgpu::TextureFormat fmt = rtPass ? mRtFmt : mTargetFmt;   // NEVER hardcode RGBA8
    bool hasDepth = !rtPass;   // RT pass: no depth; main pass: depth-disabled D24S8

    uint64_t pkey = RB3QuadPipeKey(fmt, blend, hasDepth, /*isPost*/ false, /*notex*/ !hasTex);
    wgpu::RenderPipeline pipe;
    {
        auto it = mQuadPipelines.find(pkey);
        if (it != mQuadPipelines.end()) {
            pipe = it->second;
        } else {
            wgpu::BlendState bs = mPipelines.MapBlend(blend);
            wgpu::ColorTargetState ct{};
            ct.format = fmt;
            ct.blend = &bs;
            ct.writeMask = wgpu::ColorWriteMask::All;

            wgpu::FragmentState frag{};
            frag.module = mQuadShader;
            frag.entryPoint = hasTex ? "fs_rect" : "fs_rect_notex";
            frag.targetCount = 1;
            frag.targets = &ct;

            wgpu::VertexAttribute attrs[3] = {};
            attrs[0].format = wgpu::VertexFormat::Float32x2; attrs[0].offset = 0;  attrs[0].shaderLocation = 0;
            attrs[1].format = wgpu::VertexFormat::Float32x2; attrs[1].offset = 8;  attrs[1].shaderLocation = 1;
            attrs[2].format = wgpu::VertexFormat::Float32x4; attrs[2].offset = 16; attrs[2].shaderLocation = 2;
            wgpu::VertexBufferLayout vbl{};
            vbl.arrayStride = sizeof(RB3RectVertex);
            vbl.stepMode = wgpu::VertexStepMode::Vertex;
            vbl.attributeCount = 3;
            vbl.attributes = attrs;

            // Main pass attaches a Depth24PlusStencil8 buffer; the pipeline must
            // declare a matching depth-stencil state. Disable depth entirely
            // (compare Always, write false) so the 2D quad always paints.
            wgpu::DepthStencilState ds{};
            ds.format = wgpu::TextureFormat::Depth24PlusStencil8;
            ds.depthWriteEnabled = wgpu::OptionalBool::False;
            ds.depthCompare = wgpu::CompareFunction::Always;

            wgpu::RenderPipelineDescriptor pd{};
            pd.layout = mQuadRectPL;
            pd.vertex.module = mQuadShader;
            pd.vertex.entryPoint = "vs_rect";
            pd.vertex.bufferCount = 1;
            pd.vertex.buffers = &vbl;
            pd.fragment = &frag;
            pd.depthStencil = hasDepth ? &ds : nullptr;
            pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
            pd.multisample.count = 1;   // rb3 backend is single-sampled

            pipe = mGpu.Device().CreateRenderPipeline(&pd);
            mQuadPipelines[pkey] = pipe;
        }
    }
    if (!pipe) return;

    wgpu::BindGroupEntry bge[3] = {};
    bge[0].binding = 0; bge[0].textureView = texView;
    bge[1].binding = 1; bge[1].sampler = mSampler;
    bge[2].binding = 2; bge[2].buffer = mRectUB; bge[2].offset = 0; bge[2].size = sizeof(RB3RectUB);
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = mQuadRectBGL;
    bgd.entryCount = 3;
    bgd.entries = bge;
    wgpu::BindGroup bg = mGpu.Device().CreateBindGroup(&bgd);

    mPass.SetPipeline(pipe);
    mPass.SetBindGroup(0, bg, 0, nullptr);
    mPass.SetVertexBuffer(0, mQuadVertexBuffer, 0, sizeof(verts));
    mPass.Draw(6);

    // CRITICAL: restore the SCENE bind group at group 0 — DrawRect rebinds
    // group 0 to its own 2D layout, and the next DrawMesh aborts in Dawn
    // (bind-group/layout mismatch) unless we put the scene group back.
    mPass.SetBindGroup(0, mSceneBindGroup, 0, nullptr);

    // One-shot RB3_DRAWRECT_DBG: report the rect, modulation color, diffuse
    // name, colorMod, and whether the RT redirect was active (verification
    // fallback when the live outfit-compose path is hard to frame).
    if (getenv("RB3_DRAWRECT_DBG")) {
        // Cap per kind (main-pass vs RTT) so a per-frame full-screen background
        // rect (e.g. movie.tex, rtActive=0) can't drown out the rarer outfit
        // RTT-compose rects (rtActive=1, the path this stage targets).
        static int sShotsMain = 0, sShotsRtt = 0;
        int& cnt = rtPass ? sShotsRtt : sShotsMain;
        if (cnt++ < 12) {
            const char* dn = diffuse ? (diffuse->Name() ? diffuse->Name() : "?") : "<null>";
            fprintf(stderr,
                "[RB3_DRAWRECT_DBG] rect=(%.1f,%.1f,%.1f,%.1f) mod=(%.3f,%.3f,%.3f,%.3f) "
                "matCol=(%.3f,%.3f,%.3f,%.3f) diffuse='%s' colorMod=%d rtActive=%d fmt=%d hasDepth=%d\n",
                rect.x, rect.y, rect.w, rect.h,
                ub.mod[0], ub.mod[1], ub.mod[2], ub.mod[3],
                matCol.red, matCol.green, matCol.blue, matCol.alpha,
                dn, colorMod, rtPass ? 1 : 0, (int)fmt, hasDepth ? 1 : 0);
        }
    }
}

void BandRnd::DrawMesh(RndMesh* mesh) {
    if (!mGpuReady || !mInPass || !mesh) return;

    // RTT begin hook: the shared rndobj/Cam.cpp only fires the END side
    // (FinishDrawTarget). When the current cam has a TargetTex set and we have
    // not yet redirected to it, start the RT pass so this + subsequent draws
    // land in the target texture instead of the main framebuffer.
    if (!RB3RttDisabled() && RndCam::sCurrent) {
        RndTex* tt = RndCam::sCurrent->TargetTex();
        if (tt && tt != mRtActiveTex) BeginDrawTarget(tt);
    }

    RndMesh* owner = mesh->GeomOwner();
    if (!owner) owner = mesh;
    if (getenv("RB3_RENDER_DBG")) fprintf(stderr, "[dbg] DrawMesh '%s' owner=%p\n",
        mesh->Name() ? mesh->Name() : "?", (void*)owner);

    // CAM_DBG: log which camera is current when each gameplay mesh draws, plus
    // its world position. Reveals whether the gem highway (prism_gem / surface
    // / smasher) draws under game.cam (down-the-highway) or some other cam, and
    // where each gem sits in world space. Gated on CAM_DBG; throttled per name.
    if (getenv("CAM_DBG")) {
        const char* mn = mesh->Name() ? mesh->Name() : "?";
        bool key = std::strstr(mn, "prism_gem") || std::strstr(mn, "gem_smasher") ||
                   std::strstr(mn, "surface");
        if (key) {
            static std::unordered_map<std::string, int> sSeen;
            int& n = sSeen[mn];
            if ((n++ % 240) == 0) {
                RndCam* cur = RndCam::sCurrent;
                const Transform& wx = mesh->WorldXfm();
                ObjectDir* dir = mesh->Dir();
                // Project the mesh origin to NDC to see WHERE on screen it lands.
                float ndcx = 0, ndcy = 0, depth = 0;
                if (cur) {
                    const Transform& cw = cur->WorldXfm();
                    Vector3 d(wx.v.x - cw.v.x, wx.v.y - cw.v.y, wx.v.z - cw.v.z);
                    float rx = cw.m.x.x*d.x + cw.m.x.y*d.y + cw.m.x.z*d.z;
                    float fy = cw.m.y.x*d.x + cw.m.y.y*d.y + cw.m.y.z*d.z;
                    float uz = cw.m.z.x*d.x + cw.m.z.y*d.y + cw.m.z.z*d.z;
                    depth = fy;
                    float yfov = cur->YFov() > 0.0001f ? cur->YFov() : 0.9f;
                    float aspect = (float)mGpu.WindowWidth() / (float)mGpu.WindowHeight();
                    float th = tanf(yfov * 0.5f);
                    if (fy > 0.001f) { ndcx = (rx/(th*aspect))/fy; ndcy = (uz/th)/fy; }
                }
                fprintf(stderr, "[CAM_DBG] f=%d mesh='%s' dir='%s' cam='%s' camPos=(%.1f,%.1f,%.1f) "
                                "meshPos=(%.1f,%.1f,%.1f) depth=%.1f ndc=(%.3f,%.3f)\n",
                        mFrameCount, mn, (dir && dir->Name()) ? dir->Name() : "<nodir>",
                        (cur && cur->Name()) ? cur->Name() : "<none>",
                        cur ? cur->WorldXfm().v.x : 0.f,
                        cur ? cur->WorldXfm().v.y : 0.f,
                        cur ? cur->WorldXfm().v.z : 0.f,
                        wx.v.x, wx.v.y, wx.v.z, depth, ndcx, ndcy);
            }
        }
    }

    // V2: re-write scene uniforms whenever RndCam::sCurrent has changed since
    // the last write. Panels Select() their scene cam during draw — without
    // this, every mesh would use the cam that was current at BeginDrawing
    // (typically the previous frame's last cam, or the default cam on frame 0),
    // so the geometry projects against the wrong view matrix.
    //
    // V13: also re-write when the SAME cam object has been re-posed mid-frame.
    // TrackDir::DrawShowing scrolls the highway by repeatedly SetWorldXfm'ing
    // game.cam (stationary tf50 for back/middle layers, then tf80 = tf50 + the
    // scroll `mult` for the moving/gem layers) and Select()ing it again — but
    // the pointer doesn't change, so a pointer-only check kept the stationary
    // viewProj live for the gem draws. The scrolled gems sit ~+mult further up
    // the highway, which under the stationary pose is far beyond the far plane
    // (depth ~470 vs far=226) → frustum-clipped → invisible. Comparing the
    // cam's world pose (eye + forward) catches the re-pose so the gems project
    // against the scrolled camera they were authored to descend toward.
    bool camChanged = RndCam::sCurrent && RndCam::sCurrent != mLastSceneCam;
    if (RndCam::sCurrent && !camChanged) {
        const Transform& cw = RndCam::sCurrent->WorldXfm();
        const float* p = mLastSceneCamPose;
        if (cw.v.x != p[0] || cw.v.y != p[1] || cw.v.z != p[2] ||
            cw.m.y.x != p[3] || cw.m.y.y != p[4] || cw.m.y.z != p[5])
            camChanged = true;
    }
    if (camChanged) {
        WriteSceneUniforms(RndCam::sCurrent);
        mPass.SetBindGroup(0, mSceneBindGroup, 0, nullptr);
        const Transform& cw = RndCam::sCurrent->WorldXfm();
        mLastSceneCamPose[0] = cw.v.x; mLastSceneCamPose[1] = cw.v.y; mLastSceneCamPose[2] = cw.v.z;
        mLastSceneCamPose[3] = cw.m.y.x; mLastSceneCamPose[4] = cw.m.y.y; mLastSceneCamPose[5] = cw.m.y.z;
        mLastSceneEnv = (void*)RndEnviron::sCurrent;
    }
    // Per-environ re-write: the venue scopes many RndEnvirons to mesh groups
    // within one world.cam frame; without this, every venue mesh is lit by the
    // single environ that was current at the last CAMERA write. Only matters
    // when venue lighting is on and the active cam is the venue (world.cam) — the
    // gameplay highway (game.cam) and menu cams keep their single per-cam write,
    // so they're byte-identical to before. Gated by RB3_VENUE_LIGHT.
    else if (sVenueLightEnabled() && RndCam::sCurrent &&
             RndCam::sCurrent->Name() && std::strcmp(RndCam::sCurrent->Name(), "world.cam") == 0 &&
             (void*)RndEnviron::sCurrent != mLastSceneEnv) {
        WriteSceneUniforms(RndCam::sCurrent);
        mPass.SetBindGroup(0, mSceneBindGroup, 0, nullptr);
        mLastSceneEnv = (void*)RndEnviron::sCurrent;
    }

    RndMesh::VertVector& verts = owner->mVerts;
    std::vector<RndMesh::Face>& faces = owner->mFaces;
    int nv = verts.size();
    int nf = (int)faces.size();
    if (nf <= 0) return;

    // V14: skinned-mesh detection. The smasher / strike-plate (gem_smasher_*,
    // gem_mash0..5) and other rig meshes carry a non-empty mBones list with
    // per-vertex bone weights + indices. GPU-capture (GPU_CAPTURE.md) of the
    // gameplay window showed every pipeline at stride=64 (static layout), i.e.
    // vs_skinned was never selected and the bone palette was identity — so the
    // smasher deformed to a degenerate pose and only the fallback glyph showed.
    // When skinned, we unpack into the 88-byte GpuVertexSkinned layout (bone
    // indices+weights), fill the real bone palette (BoneOffsetAt * boneWorldXfm),
    // and select VertexLayoutType::Skinned so vs_skinned blends the verts.
    bool skinned = owner->IsSkinned();

    // DIAG: skip-skinned / skip-static draw bisection.
    if (skinned && getenv("RB3_SKIP_SKINNED")) return;
    if (!skinned && getenv("RB3_SKIP_STATIC")) return;

    // --- Per-mesh GPU cache: decide whether VB/IB need (re)uploading ---
    // Reuse the cached vertex/index buffers unless the geometry actually changed.
    // `uploaded` is cleared by RndMesh::OnSync() (the dirty signal dynamic meshes
    // fire via RndMesh::Sync — RndText sub-meshes call mesh->Sync() from
    // RndText::SyncMeshes every time their glyphs change) and by a fingerprint
    // mismatch (owner swap, vert/face-count change, skinned flip). We rely on that
    // OnSync dirty signal for text rather than force-re-uploading every text mesh
    // each frame: on browser WebGPU, submit-queue backpressure pins each frame's
    // freshly-created VB/IB across all in-flight command buffers, so a per-frame
    // text re-upload still accumulates unboundedly (measured 1k -> 22k MeshVB at
    // song_select before this change). Trusting OnSync drops that to ~0.
    // Opt-out (RB3_NO_MESH_CACHE=1) restores the legacy per-draw upload for A/B.
    static int sMeshCacheOff = -1;
    if (sMeshCacheOff < 0) {
        const char* e = getenv("RB3_NO_MESH_CACHE");
        sMeshCacheOff = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    // Stable source-vertex-count key: uncompressed meshes carry verts in
    // owner->mVerts (nv = verts.size()); Xbox-compressed meshes carry them in
    // owner->mCompressedVerts (verts.size() == 0). The unpack path below
    // reassigns `nv` to the compressed count, so fingerprint against a value
    // that is identical pre- and post-unpack — otherwise compressed meshes would
    // re-upload every frame and the cache would be a no-op for most scene geom.
    int fpVertsKey = (nv > 0) ? nv : (int)owner->mNumCompressedVerts;
    RB3MeshEntry& meshEntry = sMeshGpu[mesh];
    bool needUpload = sMeshCacheOff || !meshEntry.uploaded ||
                      meshEntry.ownerKey != (const void*)owner ||
                      meshEntry.fpVerts != fpVertsKey || meshEntry.fpFaces != nf ||
                      meshEntry.fpSkinned != skinned;

    // SKIN_PROBE: ground-truth diagnostic for character skinning. Logs, once per
    // unique mesh name, whether the INSTANCE vs the GEOM-OWNER carries the bones,
    // which path the mesh takes, face count, and the bone palette source object.
    if (getenv("SKIN_PROBE")) {
        const char* mn = mesh->Name() ? mesh->Name() : "?";
        static std::unordered_map<std::string, int> sProbeSeen;
        if (sProbeSeen[mn]++ == 0) {
            fprintf(stderr,
                "[SKIN_PROBE] mesh='%s' owner='%s' (same=%d) "
                "mesh.IsSkinned=%d mesh.NumBones=%d  owner.IsSkinned=%d owner.NumBones=%d  "
                "nf=%d nv=%d -> %s\n",
                mn, owner->Name() ? owner->Name() : "?", (owner == mesh),
                mesh->IsSkinned(), mesh->NumBones(),
                owner->IsSkinned(), owner->NumBones(),
                nf, nv, skinned ? "SKINNED-PATH" : "STATIC-PATH");
        }
    }

    // --- L1 vertex-unpack cache: decide whether to (re)unpack this draw ---
    // The per-draw CPU unpack below (Be*/Half2Float helpers) was the dominant
    // UNCOUNTED residue on the game_screen reveal frame (research/09): on -O0 wasm
    // it runs per-vertex through tiny helper calls for all 113 reveal-frame meshes,
    // plus per skinned char mesh on every steady frame for the shard guard. We now
    // skip it when the cache is valid:
    //   * STATIC meshes — the unpacked verts have NO consumer past the GPU upload
    //     (the local-sphere recompute + VB write both sit inside `if (needUpload)`),
    //     so when !needUpload we skip the unpack entirely.
    //   * SKINNED meshes — the V24 shard guard re-reads the bind-pose verts EVERY
    //     frame, so we cache them in meshEntry.cachedSkinnedVerts and the guard
    //     reads the cache (via skinnedView) when the unpack is skipped.
    // Invalidation is exactly `needUpload` (owner/fpVerts/fpFaces/fpSkinned + the
    // OnSync `uploaded=false` dirty signal RndText/dynamic meshes fire), so a stale
    // cached vert can never outlive its geometry generation. Opt out via
    // RB3_UNPACK_CACHE_OFF=1 (legacy unconditional per-draw unpack, for A/B).
    bool cacheOn = !RB3UnpackCacheOff();
    bool haveSkinnedCache = cacheOn && meshEntry.cachedSkinnedVerts.size() > 0;
    // Skip when: cache active, geometry unchanged this frame, and (for skinned) we
    // actually have a populated bind-vert cache to read from.
    bool skipUnpack = cacheOn && !needUpload && (!skinned || haveSkinnedCache);

    // Frame-trace: charge the CPU unpack cost (the reveal-frame residue) to its
    // own counter so attribution is pinned by a number, not just the profile. When
    // skipped this stays 0 — that's the L1 win, made visible.
    double ftUnpackStart = gFrameTraceActive ? FrameTraceNowMs() : 0.0;

    // --- Unpack vertices into engine GpuVertexRB3 / GpuVertexSkinned layout ---
    // Runs only on a cache miss (needUpload / cache off). The GPU UPLOAD (the leaky
    // part) is gated on needUpload below; this CPU unpack is now gated on the same
    // condition (plus the skinned shard-guard's cache read).
    std::vector<GpuVertexRB3> gpuVerts;
    std::vector<GpuVertexSkinned> gpuVertsSkinned;
    if (skipUnpack) {
        // Cache hit: nothing to unpack. nv must still reflect the source vert count
        // (used by the needUpload-gated VB size + debug dumps). Static reuse needs
        // no vert data downstream; skinned reuse reads meshEntry.cachedSkinnedVerts.
        nv = skinned ? (int)meshEntry.cachedSkinnedVerts.size() : fpVertsKey;
    } else {
        // Cache miss / cache off — do the real per-vertex unpack (shared with the
        // L2 warm sweep via RB3UnpackMeshVerts, so warm-then-draw is byte-identical).
        nv = RB3UnpackMeshVerts(owner, skinned, gpuVerts, gpuVertsSkinned);
        if (nv < 0) return; // no geometry

        // VERT_PROBE: dump uncompressed-skinned bind verts (pos bounds + a few
        // samples w/ weights+indices) once per mesh, to ground-truth the band-
        // character geometry that takes the uncompressed path.
        if (skinned && getenv("VERT_PROBE") && mesh->Name()) {
            static std::unordered_map<std::string,int> sVP;
            const char* mn = mesh->Name();
            if (sVP[mn]++ == 0) {
                float mn3[3]={1e30f,1e30f,1e30f}, mx3[3]={-1e30f,-1e30f,-1e30f};
                for (int i=0;i<nv;i++){ for(int k=0;k<3;k++){ float p=gpuVertsSkinned[i].pos[k];
                    if(p<mn3[k])mn3[k]=p; if(p>mx3[k])mx3[k]=p; } }
                fprintf(stderr,"[VERT_PROBE] mesh='%s' nv=%d posBounds min(%.1f,%.1f,%.1f) max(%.1f,%.1f,%.1f) span(%.1f,%.1f,%.1f)\n",
                    mn, nv, mn3[0],mn3[1],mn3[2], mx3[0],mx3[1],mx3[2],
                    mx3[0]-mn3[0],mx3[1]-mn3[1],mx3[2]-mn3[2]);
                for (int i=0;i<nv && i<6;i++){ const GpuVertexSkinned& g=gpuVertsSkinned[i];
                    fprintf(stderr,"   v%d pos(%.2f,%.2f,%.2f) w(%.3f,%.3f,%.3f,%.3f sum=%.3f) idx(%d,%d,%d,%d)\n",
                        i, g.pos[0],g.pos[1],g.pos[2], g.boneWeights[0],g.boneWeights[1],g.boneWeights[2],g.boneWeights[3],
                        g.boneWeights[0]+g.boneWeights[1]+g.boneWeights[2]+g.boneWeights[3],
                        g.boneIndices[0],g.boneIndices[1],g.boneIndices[2],g.boneIndices[3]); }
            }
        }
    }

    // Charge the CPU unpack cost (0 on a cache hit — the L1 win, made visible).
    if (gFrameTraceActive && !skipUnpack) {
        gVertUnpackMsThisFrame += (float)(FrameTraceNowMs() - ftUnpackStart);
        gVertUnpackCountThisFrame++;
    }

    // Populate / refresh the skinned bind-vert cache on a real unpack so subsequent
    // frames' shard guard reads it instead of re-unpacking. Static meshes need no
    // cache (no consumer past upload). A move avoids a copy; gpuVertsSkinned is not
    // read again after this point on the unpack path (the shard guard reads
    // skinnedView, which we bind to the cache below).
    if (cacheOn && skinned && !skipUnpack)
        meshEntry.cachedSkinnedVerts = gpuVertsSkinned;

    // The shard guard + SMASH_DBG read bind-pose skinned verts EVERY frame. Point
    // them at the freshly-unpacked locals on a miss, or at the cache on a hit, so a
    // skipped unpack still feeds the guard identical data.
    const std::vector<GpuVertexSkinned>& skinnedView =
        skipUnpack ? meshEntry.cachedSkinnedVerts : gpuVertsSkinned;

    // GEM_VTX: one-shot dump of the gem prism's unpacked local-space verts +
    // bounds + world-projected extent, to confirm the geometry is non-degenerate
    // and lands on screen. Reads the local gpuVerts, so only meaningful when the
    // unpack actually ran this draw (one-shot per mesh — first draw is a miss).
    if (getenv("GEM_VTX") && !skipUnpack && !skinned) {
        const char* mn = mesh->Name() ? mesh->Name() : "?";
        if (std::strstr(mn, "prism_gem")) {
            static std::unordered_map<std::string, int> sVtxSeen;
            if (sVtxSeen[mn]++ == 0) {
                float mn3[3] = {1e9f,1e9f,1e9f}, mx3[3] = {-1e9f,-1e9f,-1e9f};
                for (int i = 0; i < nv; i++)
                    for (int k = 0; k < 3; k++) {
                        if (gpuVerts[i].pos[k] < mn3[k]) mn3[k] = gpuVerts[i].pos[k];
                        if (gpuVerts[i].pos[k] > mx3[k]) mx3[k] = gpuVerts[i].pos[k];
                    }
                fprintf(stderr, "[GEM_VTX] mesh='%s' nv=%d nf=%d localBox min(%.2f,%.2f,%.2f) max(%.2f,%.2f,%.2f)\n",
                        mn, nv, nf, mn3[0],mn3[1],mn3[2], mx3[0],mx3[1],mx3[2]);
                for (int i = 0; i < nv && i < 3; i++)
                    fprintf(stderr, "[GEM_VTX]   v%d pos(%.2f,%.2f,%.2f) col(%.2f,%.2f,%.2f,%.2f) norm(%.2f,%.2f,%.2f) uv(%.2f,%.2f)\n",
                            i, gpuVerts[i].pos[0],gpuVerts[i].pos[1],gpuVerts[i].pos[2],
                            gpuVerts[i].color[0],gpuVerts[i].color[1],gpuVerts[i].color[2],gpuVerts[i].color[3],
                            gpuVerts[i].norm[0],gpuVerts[i].norm[1],gpuVerts[i].norm[2],
                            gpuVerts[i].uv[0],gpuVerts[i].uv[1]);
                RndMat* m2 = mesh->Mat();
                if (m2) {
                    const Hmx::Color& c = m2->GetColor();
                    fprintf(stderr, "[GEM_VTX]   mat color(%.2f,%.2f,%.2f,%.2f) blend=%d zmode=%d intensify=%d prelit=%d alphaCut=%d hasDiffuse=%d\n",
                            c.red,c.green,c.blue,c.alpha, (int)m2->GetBlend(), (int)m2->GetZMode(),
                            m2->mIntensify, m2->mPreLit, m2->mAlphaCut, m2->GetDiffuseTex()?1:0);
                } else {
                    fprintf(stderr, "[GEM_VTX]   mat=NULL\n");
                }
                const Transform& wx2 = mesh->WorldXfm();
                fprintf(stderr, "[GEM_VTX]   worldXfm basis x(%.2f,%.2f,%.2f) y(%.2f,%.2f,%.2f) z(%.2f,%.2f,%.2f) v(%.1f,%.1f,%.1f)\n",
                        wx2.m.x.x,wx2.m.x.y,wx2.m.x.z, wx2.m.y.x,wx2.m.y.y,wx2.m.y.z,
                        wx2.m.z.x,wx2.m.z.y,wx2.m.z.z, wx2.v.x,wx2.v.y,wx2.v.z);
            }
        }
    }
  // --- GPU upload (the leaky part): only on cache miss / dirty / fingerprint
  // change. The unpacked verts above are always available to the shard guard. ---
  // Frame-trace: the VB/IB CreateBuffer+WriteBuffer burst (first-draw mesh upload
  // stall) charges to gMeshUploadMsThisFrame; cached reuse below is free.
  double ftMeshStart = gFrameTraceActive ? FrameTraceNowMs() : 0.0;
  if (needUpload) {
    // Recompute a tight LOCAL bounding sphere from the just-unpacked positions
    // (once per geometry generation, gated on needUpload). Xbox-compressed venue
    // meshes carry no CPU verts, so RndMesh::UpdateSphere can't fix their stale /
    // wrong-centered baked sphere — the only place real local positions exist is
    // right here. Static meshes only: skinned meshes keep their zero sphere
    // (RndMesh::UpdateSphere sets s.Zero() => never culled, which is correct).
    // The LOCAL sphere is transformed by the mesh's WorldXfm at the cull test
    // (RndDrawable::MakeWorldSphere), so animated static venue meshes still cull
    // correctly. Used by the world.cam-scoped RB3VenueFrustumCull (Draw.cpp).
    if (!skinned && nv > 0) {
        float mn3[3] = { 1e30f, 1e30f, 1e30f }, mx3[3] = { -1e30f, -1e30f, -1e30f };
        for (int i = 0; i < nv; i++)
            for (int k = 0; k < 3; k++) {
                float p = gpuVerts[i].pos[k];
                if (p < mn3[k]) mn3[k] = p;
                if (p > mx3[k]) mx3[k] = p;
            }
        Vector3 center((mn3[0] + mx3[0]) * 0.5f, (mn3[1] + mx3[1]) * 0.5f,
                       (mn3[2] + mx3[2]) * 0.5f);
        float dx = mx3[0] - mn3[0], dy = mx3[1] - mn3[1], dz = mx3[2] - mn3[2];
        float radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
        Sphere localSphere;
        localSphere.Set(center, radius);
        mesh->SetSphere(localSphere);
    }

    std::vector<uint16_t> indices;
    indices.reserve(nf * 3);
    for (int i = 0; i < nf; i++) {
        indices.push_back(faces[i].v1);
        indices.push_back(faces[i].v2);
        indices.push_back(faces[i].v3);
    }

    {
        wgpu::BufferDescriptor bd{};
        bd.label = "MeshVB";
        bd.size = skinned ? ((uint64_t)nv * sizeof(GpuVertexSkinned))
                          : ((uint64_t)nv * sizeof(GpuVertexRB3));
        bd.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        meshEntry.vbuf = mGpu.Device().CreateBuffer(&bd);
        sMeshBufCreatesThisFrame++;
        mGpu.Queue().WriteBuffer(meshEntry.vbuf, 0,
                                 skinned ? (const void*)gpuVertsSkinned.data()
                                         : (const void*)gpuVerts.data(),
                                 bd.size);
    }
    {
        uint64_t isz = indices.size() * sizeof(uint16_t);
        // index buffer size must be a multiple of 4 (WebGPU WriteBuffer requirement).
        // Resize the source vector to cover the padded byte count so WriteBuffer
        // does not over-read past valid data.  The extra element (at most 1 u16)
        // is zero-initialised by resize and never referenced by DrawIndexed.
        uint64_t padded = (isz + 3) & ~3ull;
        indices.resize(padded / sizeof(uint16_t), 0);
        wgpu::BufferDescriptor bd{};
        bd.label = "MeshIB"; bd.size = padded;
        bd.usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
        meshEntry.ibuf = mGpu.Device().CreateBuffer(&bd);
        sMeshBufCreatesThisFrame++;
        mGpu.Queue().WriteBuffer(meshEntry.ibuf, 0, indices.data(), padded);
    }

    // Stamp the cache fingerprint so subsequent frames reuse these buffers.
    // indexCount is the UNPADDED count (3*nf); DrawIndexed draws exactly that
    // (the original code passed the padded size, whose trailing partial triangle
    // the rasterizer always dropped — identical output, fewer fetched indices).
    meshEntry.indexCount = (uint32_t)(nf * 3);
    meshEntry.skinned    = skinned;
    meshEntry.ownerKey   = (const void*)owner;
    meshEntry.fpVerts    = fpVertsKey;
    meshEntry.fpFaces    = nf;
    meshEntry.fpSkinned  = skinned;
    meshEntry.uploaded   = true;
    if (gFrameTraceActive) {
        gMeshUploadMsThisFrame += (float)(FrameTraceNowMs() - ftMeshStart);
        gMeshUploadCountThisFrame++;
    }
  } // if (needUpload)

    // Cached buffers reused this frame (no GPU work when !needUpload).
    wgpu::Buffer vbuf = meshEntry.vbuf;
    wgpu::Buffer ibuf = meshEntry.ibuf;
    uint32_t cachedIndexCount = meshEntry.indexCount;
    if (!vbuf || !ibuf) return;  // upload failed / no geometry

    // --- Claim this draw's per-INSTANCE uniform slot ---
    // The SAME RndMesh draws multiple times per frame with different obj/mat/bone
    // state (song_select rows, repeated panel widgets). Each instance needs its
    // OWN uniform buffer + bind group, because every queue.WriteBuffer for the
    // frame runs before the single submit — a per-mesh shared buffer would render
    // every instance with the LAST instance's uniforms (the darkened-rows
    // regression). Lazily reset the per-frame slot index on this mesh's first draw
    // this frame, then hand out the next slot (growing the per-mesh vector only
    // when this frame's instance count exceeds any prior frame's — bounded by the
    // mesh's max instances/frame, recycled across frames).
    //
    // RB3_NO_MESH_CACHE=1 fully restores legacy per-draw behavior: a fresh
    // FUNCTION-LOCAL slot whose wgpu handles release when DrawMesh returns (RAII),
    // exactly like the original code's transient per-draw buffers + bind groups
    // (and reproducing the legacy leak under web submit backpressure — that is the
    // opt-out's explicit A/B tradeoff). Correctness holds in BOTH modes: a slot is
    // never shared between two draws.
    RB3MeshEntry::UniformSlot localSlot;
    RB3MeshEntry::UniformSlot* slotPtr;
    if (sMeshCacheOff) {
        slotPtr = &localSlot;
    } else {
        if (meshEntry.frameSeen != sFrameSeq) {
            meshEntry.frameSeen = sFrameSeq;
            meshEntry.nextSlot = 0;
        }
        uint32_t slotIdx = meshEntry.nextSlot++;
        if (slotIdx >= meshEntry.slots.size()) meshEntry.slots.resize(slotIdx + 1);
        slotPtr = &meshEntry.slots[slotIdx];
        // SLOT_PROBE: surface meshes drawn MORE THAN ONCE per frame (slotIdx>0) —
        // the multi-instance case the per-instance slots exist to keep correct.
        // Throttled per mesh-name occurrence-count so it doesn't spam.
        if (slotIdx > 0 && getenv("SLOT_PROBE")) {
            const char* mn = mesh->Name() ? mesh->Name() : "<noname>";
            fprintf(stderr, "[SLOT_PROBE] f=%llu mesh='%s' slotIdx=%u (drawn %u+ times this frame)\n",
                    (unsigned long long)sFrameSeq, mn, slotIdx, slotIdx + 1);
        }
    }
    RB3MeshEntry::UniformSlot& slot = *slotPtr;

    // --- Object uniforms: world transform of the mesh ---
    // For a SKINNED mesh the bone palette (below) already composes the bone's
    // world transform, so the blended vertex is in world space; the object
    // world matrix must be IDENTITY to avoid double-transforming. (Mirrors
    // DC3's Mesh_Wgpu.cpp skinned path.) Static meshes use the mesh WorldXfm.
    ObjectUniforms obj{};
    if (skinned) {
        for (int i = 0; i < 16; i++) obj.world[i] = (i % 5 == 0) ? 1.f : 0.f;
    } else {
        MiloXfmToColMajor(mesh->WorldXfm(), obj.world);
    }
    // worldInvTranspose: for unscaled rigid xfm, the rotation part suffices.
    // Use the world rotation as-is (good enough for normals on rigid meshes).
    std::memcpy(obj.worldInvTranspose, obj.world, sizeof(obj.world));
    // Per-(mesh,instance) persistent object uniform buffer + bind group (created
    // once per slot, reused across frames). The world transform changes per frame
    // (animation / panel pose) AND per instance (each list row poses the shared
    // mesh differently), so we WriteBuffer it each draw — but into THIS instance's
    // OWN buffer, so no new GPU resource is created at steady state. Replaces the
    // old per-draw ring-offset bind group that accumulated unbounded under web
    // submit-queue backpressure.
    if (!slot.objUB) {
        wgpu::BufferDescriptor ubd{};
        ubd.label = "MeshObjUB";
        ubd.size = sizeof(ObjectUniforms);
        ubd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        slot.objUB = mGpu.Device().CreateBuffer(&ubd);
        sMeshBufCreatesThisFrame++;
        wgpu::BindGroupEntry e{};
        e.binding = 0; e.buffer = slot.objUB; e.offset = 0; e.size = sizeof(ObjectUniforms);
        wgpu::BindGroupDescriptor bd{};
        bd.layout = mPipelines.ObjectLayout(); bd.entryCount = 1; bd.entries = &e;
        slot.objBG = mGpu.Device().CreateBindGroup(&bd);
        sMeshBGCreatesThisFrame++;
    }
    mGpu.Queue().WriteBuffer(slot.objUB, 0, &obj, sizeof(obj));
    wgpu::BindGroup objBG = slot.objBG;

    // --- Bone uniforms ---
    // Static: identity palette (vs_main ignores it; bound only to satisfy the
    // pipeline layout). Skinned: the real palette — for each bone i the skin
    // matrix is BoneOffsetAt(i) * boneTrans->WorldXfm(), composed in Milo
    // row-vector convention and stored column-major for the WGSL `M * v` form,
    // so vs_skinned's weighted sum yields world-space positions directly.
    BoneUniforms bones{};
    int sFallbackBones = 0; // SHARD_DBG: count identity-substituted (runaway) bones
    if (skinned) {
        int numBones = owner->NumBones();
        if (numBones > kMaxBones) numBones = kMaxBones;
        // BONE_PROBE: once, for the first body-sized skinned mesh (>=20 bones),
        // dump each bone's local-rotation orthonormality (det), world pose, the
        // inverse-bind offset, and the composed skin determinant. Localizes
        // whether deformation is the local pose, the hierarchy, or the offset.
        static bool sBoneProbeDone = false;
        const char* probeName = getenv("BONE_PROBE_NAME");
        bool nameMatch = mesh->Name() && (probeName ? (strstr(mesh->Name(), probeName) != nullptr) :
                         (strstr(mesh->Name(), "plaidshirt") ||
                         strstr(mesh->Name(), "trackjacket") || strstr(mesh->Name(), "shirt") ||
                         strstr(mesh->Name(), "jacket") || strstr(mesh->Name(), "vestdenim")));
        bool doBoneProbe = getenv("BONE_PROBE") && !sBoneProbeDone &&
                           nameMatch && owner->NumBones() >= 8 && mesh->Name();
        auto det3 = [](const Hmx::Matrix3& m) {
            return m.x.x*(m.y.y*m.z.z - m.y.z*m.z.y)
                 - m.x.y*(m.y.x*m.z.z - m.y.z*m.z.x)
                 + m.x.z*(m.y.x*m.z.y - m.y.y*m.z.x);
        };
        if (doBoneProbe) {
            sBoneProbeDone = true;
            fprintf(stderr, "\n=== BONE_PROBE mesh='%s' numBones=%d (actual=%d) ===\n",
                    mesh->Name(), numBones, owner->NumBones());
        }
        // RB3_RECOMPUTE_OFFSETS (test): once per skinned mesh, recompute each
        // bone's inverse-bind offset from the LIVE skeleton pose:
        //   mOffset = meshWorldXfm * inverse(boneWorldXfm)
        // i.e. SetBone(i, bone, true). This makes skin = identity at the current
        // (neutral) pose BY CONSTRUCTION, regardless of the authored offset. Used
        // to prove whether the deformation is an authored-offset/bind mismatch
        // (recompute fixes it) vs a runtime-pose bug (recompute can't help).
        // NOTE: this is a DIAGNOSTIC, not a fix. It recomputes against the LIVE
        // (already idle-animating) pose at first draw, which is NOT the bind pose
        // — so it visually WORSENS playback (bakes a mid-idle pose; later frames
        // fling worse). It DOES prove the root cause under RB3_NO_CLIP=1
        // RB3_NO_IK=1 (static bind): then trackjacket's skinPos collapses to 0,
        // confirming an authored-offset-vs-skeleton-bind mismatch. The real fix is
        // per-character skeleton instancing (see CHAR_SKINNING_DEFORM_INVESTIGATION.md).
        if (getenv("RB3_RECOMPUTE_OFFSETS")) {
            static std::unordered_map<void*,int> sRecomp;
            if (sRecomp[(void*)owner]++ == 0) {
                for (int b = 0; b < numBones; b++)
                    owner->SetBone(b, owner->BoneTransAt(b), true);
            }
        }
        // SKEL_REBAKE pre-pass (wave-06): scoped, one-time inverse-bind rebake for the
        // female band outfit (trackjacket) whose female-baked offsets land on the
        // shared MALE-bind band skeleton. The whole native band skeleton is PROVABLY
        // STATIC (XBONE_TRACK: bone_R-upperArm worldPos byte-identical across 1424
        // draws over a whole song, clip+IK on) — no member's Poll animates the shared
        // char/main/skeleton.milo magnet, so there is no live female pose to bind to (a
        // SyncObjects rebind is a no-op: Find(boneName) returns the SAME magnet) and no
        // recompute trap (the magnet never moves). The 3 males look correct only because
        // their outfit invBind already matches the static male bind. We bring the female
        // to that SAME quality by rebaking HER outfit's inverse-bind offsets against the
        // static skeleton pose (mOffset = meshWorld * inverse(boneWorld), SetBone(b,bone,
        // true)) so her skin composes to identity -> a coherent, correctly-posed arm,
        // not the clamp's authored-T-pose stub. Triggered only when the mesh actually
        // shards (worst bone's mesh-local skin > 12u); clean meshes (males) never rebake.
        // One-time per mesh (mNativeBonesRebound flag, which also tells the clamp to skip
        // this mesh). Default-on; opt-out RB3_NO_SKEL_REBAKE=1.
        {
            static int sRebake = -1;
            if (sRebake < 0) sRebake = getenv("RB3_NO_SKEL_REBAKE") ? 0 : 1;
            // MESH-LEVEL DYNAMIC EXCLUSION: face / hair / fingernail outfit meshes are
            // skinned to bones driven every frame by CharFaceServo / CharHair /
            // CharIKFingers, so a one-time static rebake of those does not stick (the
            // bones move) and would only swap the shipped clamp's coherent bind pose for
            // a re-flinging one (regression). Leave those meshes to the per-frame fling
            // clamp (the shipped backstop). Only the STATIC arm/twist/torso/leg outfit
            // meshes (trackjacket, buttflappants, ...) are rebaked — their bind mismatch
            // on the provably-static band skeleton is the real ~20u fling, and a static
            // rebake there is a permanent correction. (The 650u goatee/hair flings stay
            // exactly as the shipped clamp left them.)
            const char* mn0 = mesh->Name();
            bool dynamicMesh = mn0 && (strstr(mn0, "facehair") || strstr(mn0, "goatee") ||
                strstr(mn0, "hair") || strstr(mn0, "bedhead") || strstr(mn0, "blownback") ||
                strstr(mn0, "mohawk") || strstr(mn0, "fingernails") ||
                strstr(mn0, "eyebrow") || strstr(mn0, "tongue") || strstr(mn0, "facial"));
            if (sRebake && !dynamicMesh && numBones >= 8 && !mesh->mNativeBonesRebound &&
                (!owner || !owner->mNativeBonesRebound)) {
                const Transform& mw = mesh->WorldXfm();
                Transform invMw; Invert(mw, invMw);
                float worst2 = 0.f; int worstB = -1;
                for (int b = 0; b < numBones; b++) {
                    RndTransformable* bt = owner->BoneTransAt(b);
                    if (!bt) continue;
                    const Transform& wt = bt->WorldXfm();
                    if (!(std::fabs(wt.v.x) < 1e5f && std::fabs(wt.v.y) < 1e5f &&
                          std::fabs(wt.v.z) < 1e5f)) continue;
                    Transform skin; Multiply(owner->BoneOffsetAt(b), wt, skin);
                    Transform local; Multiply(skin, invMw, local);
                    float ml2 = local.v.x*local.v.x + local.v.y*local.v.y +
                                local.v.z*local.v.z;
                    if (ml2 > worst2) { worst2 = ml2; worstB = b; }
                }
                // BAND-ONLY scope: only the on-stage band outfit meshes bind to the
                // STATIC, shared char/main/skeleton_unshared.milo magnet (a root dir,
                // never animated). The crowd/extras clap/lighter/fist/body meshes bind
                // their OWN per-character skeletons (char/crowd/*, char/extras/*) which
                // DO animate — rebaking those against a live pose would FREEZE the
                // animation mid-motion (the recompute trap). Gate the rebake on the
                // worst bone's owning dir being skeleton_unshared.milo so only the band
                // (static-skeleton) meshes are touched.
                RndTransformable* wbone = (worstB >= 0) ? owner->BoneTransAt(worstB) : 0;
                ObjectDir* wdir = wbone ? wbone->Dir() : 0;
                bool bandStatic = wdir && !wdir->mStoredFile.empty() &&
                    strstr(wdir->mStoredFile.c_str(), "skeleton_unshared.milo") != 0;
                if (worst2 > 144.0f && bandStatic) {
                    // Rebake ONLY the individual flung bones (mesh-local skin > 12u) to
                    // the current pose. On the band the upper-body arm/twist chain is
                    // STATIC (XBONE_TRACK), so its rebaked offset is a permanent, stable
                    // correction -> coherent posed female arm. The female hair/face/
                    // finger chains (bone_hair*, bone_*lid*, finger bones) ARE driven on
                    // top by CharHair/CharFaceServo/CharIKFingers, so a rebake of those
                    // is only momentarily valid; we DELIBERATELY do NOT set
                    // mNativeBonesRebound, so the per-frame fling clamp below stays LIVE
                    // and catches any bone that drifts off its rebaked pose (the dynamic
                    // chains), clamping them to bind exactly as the shipped backstop did,
                    // while the static arm renders correctly posed. Clean bones (the 3
                    // males, and the static bones already at I) are never touched.
                    Transform mwInv; { const Transform& mw2 = mesh->WorldXfm();
                        Invert(mw2, mwInv); }
                    int reb = 0;
                    for (int b = 0; b < numBones; b++) {
                        RndTransformable* bt = owner->BoneTransAt(b);
                        if (!bt) continue;
                        // DYNAMIC-CHAIN EXCLUSION: the female hair / facial / finger
                        // bones are driven every frame by CharHair / CharFaceServo /
                        // CharIKFingers ON TOP of the static base skeleton, so a one-time
                        // static rebake of those does not stick (they move). Leave them to
                        // the per-frame fling clamp (clamps to bind, the shipped backstop)
                        // — exactly the baseline behaviour, no regression. We rebake ONLY
                        // the STATIC arm/twist/torso/leg chain (the real ~20u bind
                        // mismatch), which is provably static so its correction is
                        // permanent. Identify dynamic bones by name.
                        const char* bn = bt->Name();
                        if (bn && (strstr(bn, "hair") || strstr(bn, "-lid") ||
                                   strstr(bn, "_lid") || strstr(bn, "jaw") ||
                                   strstr(bn, "lip") || strstr(bn, "brow") ||
                                   strstr(bn, "eye") || strstr(bn, "mouth") ||
                                   strstr(bn, "cheek") || strstr(bn, "nose") ||
                                   strstr(bn, "tongue") || strstr(bn, "goatee") ||
                                   strstr(bn, "index") || strstr(bn, "middle") ||
                                   strstr(bn, "pinky") || strstr(bn, "ring") ||
                                   strstr(bn, "thumb") || strstr(bn, "finger")))
                            continue;
                        const Transform& wt2 = bt->WorldXfm();
                        if (!(std::fabs(wt2.v.x) < 1e5f && std::fabs(wt2.v.y) < 1e5f &&
                              std::fabs(wt2.v.z) < 1e5f)) continue;
                        Transform sk2; Multiply(owner->BoneOffsetAt(b), wt2, sk2);
                        Transform lo2; Multiply(sk2, mwInv, lo2);
                        float bml2 = lo2.v.x*lo2.v.x + lo2.v.y*lo2.v.y + lo2.v.z*lo2.v.z;
                        if (bml2 > 144.0f) { owner->SetBone(b, bt, true); reb++; }
                    }
                    if (getenv("SKEL_REBAKE_PROBE")) {
                        fprintf(stderr,
                            "[SKEL_REBAKE] mesh='%s' worstBone='%s' boneDir='%s' meshLocal=%.1fu -> rebaked %d flung bones (clamp stays live)\n",
                            mesh->Name()?mesh->Name():"?",
                            (wbone && wbone->Name())?wbone->Name():"?",
                            wdir?wdir->mStoredFile.c_str():"-", std::sqrt(worst2), reb);
                    }
                }
            }
        }
        // BISECT: force identity palette to test mesh/weights/indices vs posing.
        static int sBonesIdentity = -1;
        if (sBonesIdentity < 0) sBonesIdentity = getenv("RB3_BONES_IDENTITY") ? 1 : 0;
        for (int b = 0; b < numBones; b++) {
            RndTransformable* bt = owner->BoneTransAt(b);
            // Identity fallback for a null/garbage bone.
            float* dst = bones.bones[b];
            for (int i = 0; i < 16; i++) dst[i] = (i % 5 == 0) ? 1.f : 0.f;
            if (sBonesIdentity) continue; // TRUE identity palette -> worldPos == bindPos
            if (!bt) { sFallbackBones++;
                if (doBoneProbe) fprintf(stderr, "  bone[%d] NULL\n", b);
                continue; }
            const Transform& wt = bt->WorldXfm();
            if (doBoneProbe) {
                const Transform& lx = bt->LocalXfm();
                const Transform& off = owner->BoneOffsetAt(b);
                Transform sk; Multiply(off, wt, sk);
                // PTR_PROBE: dump bone object pointer + its owning ObjectDir to test
                // whether trackjacket's bones are the SHARED skeleton bones (remapped
                // at merge) or trackjacket's own un-posed bind-pose copies.
                ObjectDir* bdir = bt->Dir();
                ObjectDir* mdir = mesh->Dir();
                fprintf(stderr,
                    "  bone[%d] '%s' ptr=%p dir='%s' meshPtr=%p meshDir='%s'\n",
                    b, bt->Name() ? bt->Name() : "?", (void*)bt,
                    bdir && bdir->Name() ? bdir->Name() : "-",
                    (void*)mesh, mdir && mdir->Name() ? mdir->Name() : "-");
                fprintf(stderr,
                    "  bone[%d] '%s' parent='%s'\n"
                    "    localPos=(%.2f,%.2f,%.2f) localRot=[%.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f]\n"
                    "    worldPos=(%.2f,%.2f,%.2f) worldRot=[%.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f]\n"
                    "    offDet=%.4f offPos=(%.2f,%.2f,%.2f) skinPos=(%.2f,%.2f,%.2f)\n",
                    b, bt->Name() ? bt->Name() : "?",
                    bt->TransParent() && bt->TransParent()->Name() ? bt->TransParent()->Name() : "-",
                    lx.v.x, lx.v.y, lx.v.z,
                    lx.m.x.x, lx.m.x.y, lx.m.x.z, lx.m.y.x, lx.m.y.y, lx.m.y.z, lx.m.z.x, lx.m.z.y, lx.m.z.z,
                    wt.v.x, wt.v.y, wt.v.z,
                    wt.m.x.x, wt.m.x.y, wt.m.x.z, wt.m.y.x, wt.m.y.y, wt.m.y.z, wt.m.z.x, wt.m.z.y, wt.m.z.z,
                    det3(off.m), off.v.x, off.v.y, off.v.z, sk.v.x, sk.v.y, sk.v.z);
                // Q4 EXT: dump the composed SKIN rotation (what the GPU palette
                // gets). At bind pose this should be ~identity (det 1, diagonal 1).
                float sd = det3(sk.m);
                fprintf(stderr,
                    "    skinDet=%.4f skinRot=[%.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f]\n"
                    "    offRot=[%.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f]\n",
                    sd,
                    sk.m.x.x, sk.m.x.y, sk.m.x.z, sk.m.y.x, sk.m.y.y, sk.m.y.z, sk.m.z.x, sk.m.z.y, sk.m.z.z,
                    off.m.x.x, off.m.x.y, off.m.x.z, off.m.y.x, off.m.y.y, off.m.y.z, off.m.z.x, off.m.z.y, off.m.z.z);
            }
            if (!(std::fabs(wt.v.x) < 1e5f && std::fabs(wt.v.y) < 1e5f &&
                  std::fabs(wt.v.z) < 1e5f)) {
                sFallbackBones++;
                continue; // keep identity for runaway/NaN bone world xfm
            }
            Transform skin;
            Multiply(owner->BoneOffsetAt(b), wt, skin);
            // V24: validate the COMPOSED skin matrix, not just the bone's world
            // translation. A bone whose WorldXfm has a non-finite / runaway
            // ROTATION or SCALE (.m) passes the translation-only guard above, but
            // `Multiply(offset, wt)` then yields a skin matrix whose rotation
            // basis or translation flings any vertex weighted to it far across
            // the scene — drawing as a thin teal/green triangular shard/fan
            // (the crowd/extras servo skeletons can momentarily produce one).
            // Reject the whole composed matrix (rotation rows + translation) if
            // any element is non-finite or absurdly large, falling back to
            // identity so the vertex stays at its bind pose instead of slivering.
            {
                const float* row[4] = { &skin.m.x.x, &skin.m.y.x, &skin.m.z.x, &skin.v.x };
                bool bad = false;
                // Transform is row-major Vector3 rows (4 floats stride incl pad);
                // check the 3x3 rotation basis + the translation explicitly.
                float chk[12] = {
                    skin.m.x.x, skin.m.x.y, skin.m.x.z,
                    skin.m.y.x, skin.m.y.y, skin.m.y.z,
                    skin.m.z.x, skin.m.z.y, skin.m.z.z,
                    skin.v.x,   skin.v.y,   skin.v.z };
                (void)row;
                for (int k = 0; k < 12; k++) {
                    float a = chk[k];
                    if (!(a == a) || std::fabs(a) > 1e5f) { bad = true; break; }
                }
                if (bad) { sFallbackBones++; continue; } // keep identity
            }
            // CROWD/EXTRAS skin fling clamp (backstop). The crowd and extras bind their
            // skin meshes to separate char/crowd & char/extras skeletons whose bind does
            // not match their meshes' inverse-bind offsets under the native load, so their
            // clapping/lighter hand poses fling vertices thousands of units into shards.
            // This block detects a bone whose composed skin, in the MESH's own frame, is
            // far off bind (skin * inverse(meshWorld) translation > 12u) and falls it back
            // to identity so its weighted vertices stay at their authored model-space bind
            // instead of shattering. Gated to multi-bone SKIN meshes (NumBones>=8) so
            // single-bone props (e.g. the mic) are never clamped. Opt-out: RB3_NO_SKIN_CLAMP=1.
            //
            // The BAND members are NOT handled here — the faithful fix lives in DECOMP
            // (BandCharacter::RebindOutfitBonesToOwnSkeleton): the female member's outfit
            // skin bones are repointed to her own gender-posed live skeleton, so her arms
            // compose correctly AND animate. Those rebound meshes set
            // RndMesh::mNativeBonesRebound and are SKIPPED below (this heuristic would
            // wrongly freeze a correctly-animating arm to bind). See
            // docs/native/CHAR_SKINNING_DEFORM_INVESTIGATION.md.
            {
                static int sSkinClamp = -1;
                if (sSkinClamp < 0) sSkinClamp = getenv("RB3_NO_SKIN_CLAMP") ? 0 : 1;
                // Skip the clamp for band-member outfit meshes that have been
                // rebound to the member's own gender-posed skeleton
                // (BandCharacter::RebindOutfitBonesToOwnSkeleton). Those are now
                // correctly bound and ANIMATE — the mesh-local heuristic below
                // would otherwise freeze a swinging arm to bind. The clamp stays
                // active for the crowd/extras' separate broken skeletons. Test the
                // DRAWN mesh's flag, never the GeomOwner's (band outfit skin meshes
                // are self-owned; using GeomOwner would mis-flag crowd meshes that
                // share char_shared geometry).
                bool reboundSkip = mesh->mNativeBonesRebound;
                // DRAW-TIME authoritative AFTER measure for the wave-08 rebind: for a
                // rebound band mesh, report the worst bone's |skinWorld - boneWorld|
                // delta — how far the composed skin places vertices from the bone
                // itself. Clean skinning keeps this within limb/joint extent (~40-65u,
                // MEASURED); a broken bind flings it to hundreds. (A skinned mesh's own
                // WorldXfm is identity here, so a "mesh-local" measure would just read
                // back the character's far-from-origin world position — misleading.)
                // Gated REBIND_DRAW_SKINPOS=1. Measured at the exact palette-compose.
                if (reboundSkip && numBones >= 8 && getenv("REBIND_DRAW_SKINPOS") &&
                    mesh->Name()) {
                    float dx = skin.v.x - wt.v.x, dy = skin.v.y - wt.v.y,
                          dz = skin.v.z - wt.v.z;
                    float delta = std::sqrt(dx*dx + dy*dy + dz*dz);
                    static std::unordered_map<std::string,float> sMax;
                    std::string key = mesh->Name();
                    if (delta > sMax[key]) {
                        sMax[key] = delta;
                        fprintf(stderr,
                            "[REBIND_DRAW_SKINPOS] mesh='%s' bone='%s' "
                            "skinToBoneDelta(max)=%.3fu (clean<~65u; fling=hundreds)\n",
                            mesh->Name(), bt->Name()?bt->Name():"?", delta);
                    }
                    // also flag ANY individual bone that flings past 120u (a real shard)
                    if (delta > 120.f && getenv("REBIND_DRAW_FLING")) {
                        static int sFc = 0;
                        if (sFc++ < 60)
                            fprintf(stderr,
                                "[REBIND_DRAW_FLING] mesh='%s' bone='%s' delta=%.1fu "
                                "skinDet=%.3f\n", mesh->Name(),
                                bt->Name()?bt->Name():"?", delta,
                                skin.m.x.x*(skin.m.y.y*skin.m.z.z-skin.m.y.z*skin.m.z.y)
                              - skin.m.x.y*(skin.m.y.x*skin.m.z.z-skin.m.y.z*skin.m.z.x)
                              + skin.m.x.z*(skin.m.y.x*skin.m.z.y-skin.m.y.y*skin.m.z.x));
                    }
                }
                if (sSkinClamp && !reboundSkip && numBones >= 8) {
                    // skin in mesh-local space = skin * inverse(meshWorld); its
                    // translation magnitude is the per-bone bind mismatch (~0 clean).
                    const Transform& mw = mesh->WorldXfm();
                    Transform invMw; Invert(mw, invMw);
                    Transform local; Multiply(skin, invMw, local);
                    float ml2 = local.v.x*local.v.x + local.v.y*local.v.y + local.v.z*local.v.z;
                    // Threshold 12u: the female static bind mismatch is ~20u, while a
                    // legitimately animated (clean) arm bone displaces only a few u in
                    // mesh space, so 12u cleanly separates a shard from real motion.
                    if (ml2 > 144.0f) { // > 12u bind mismatch in mesh space -> would shard
                        sFallbackBones++;
                        if (getenv("SKIN_CLAMP_PROBE")) {
                            static std::unordered_map<std::string,int> sCl;
                            std::string key = std::string(mesh->Name()?mesh->Name():"?");
                            if (sCl[key]++ % 240 == 0)
                                fprintf(stderr, "[SKIN_CLAMP] mesh='%s' bone='%s' meshLocal=%.1fu (clamped to bind)\n",
                                    mesh->Name()?mesh->Name():"?", bt->Name()?bt->Name():"?",
                                    std::sqrt(ml2));
                        }
                        continue; // keep identity for this bone (vertices stay at bind)
                    }
                }
            }
            // SHARD_CATCH: report any bone whose composed skin translation is far
            // from origin (>8u) — that's a vertex-flinging shard. Prints mesh+bone
            // name so we can see WHICH bone breaks during normal play.
            if (getenv("SHARD_CATCH")) {
                float sp = std::sqrt(skin.v.x*skin.v.x+skin.v.y*skin.v.y+skin.v.z*skin.v.z);
                if (sp > 8.0f) {
                    static std::unordered_map<std::string,int> sSC;
                    std::string key = std::string(mesh->Name()?mesh->Name():"?") + "/" +
                                      std::string(bt->Name()?bt->Name():"?");
                    if (sSC[key]++ % 120 == 0)
                        fprintf(stderr,"[SHARD_CATCH] mesh='%s' bone[%d]='%s' skinPos=(%.1f,%.1f,%.1f) |%.1f| parent='%s'\n",
                            mesh->Name()?mesh->Name():"?", b, bt->Name()?bt->Name():"?",
                            skin.v.x,skin.v.y,skin.v.z, sp,
                            bt->TransParent()&&bt->TransParent()->Name()?bt->TransParent()->Name():"-");
                }
            }
            MiloXfmToColMajor(skin, dst);
        }
        // XBONE_TRACK: print the named bone's worldPos for the trackjacket mesh on
        // EVERY draw, to settle whether the shared band skeleton magnet ANIMATES
        // (worldPos varies frame to frame) or is STATIC (constant). Decides whether
        // a constant per-bone correction would yield a frozen vs trackable female.
        if (const char* xt = getenv("XBONE_TRACK")) {
            if (mesh->Name() && strstr(mesh->Name(), "trackjacket")) {
                for (int b = 0; b < numBones; b++) {
                    RndTransformable* bt = owner->BoneTransAt(b);
                    if (!bt || !bt->Name() || strstr(bt->Name(), xt) == nullptr) continue;
                    const Transform& wt = bt->WorldXfm();
                    static int sN = 0;
                    fprintf(stderr, "[XBONE_TRACK] #%d bone='%s' worldPos=(%.4f,%.4f,%.4f) worldRot.x=(%.4f,%.4f,%.4f)\n",
                        sN++, bt->Name(), wt.v.x, wt.v.y, wt.v.z, wt.m.x.x, wt.m.x.y, wt.m.x.z);
                    break;
                }
            }
        }
        // XBONE: cross-mesh single-bone probe. For env XBONE=<bonename>, dump that
        // named bone's POINTER + worldRot + THIS mesh's offset/skinPos, once per
        // (mesh,bone) pair. Lets us compare the SAME bone object across two outfit
        // meshes (vestdenim clean vs trackjacket flung) in ONE run, to see if the
        // bone pointer / worldRot is shared and only the offset differs.
        if (const char* xb = getenv("XBONE")) {
            for (int b = 0; b < numBones; b++) {
                RndTransformable* bt = owner->BoneTransAt(b);
                if (!bt || !bt->Name() || strstr(bt->Name(), xb) == nullptr) continue;
                static std::unordered_map<std::string,int> sXB;
                std::string key = std::string(mesh->Name()?mesh->Name():"?") + "@" + bt->Name();
                if (sXB[key]++ != 0) continue;
                const Transform& wt = bt->WorldXfm();
                const Transform& off = owner->BoneOffsetAt(b);
                Transform sk; Multiply(off, wt, sk);
                // Walk the bone's TransParent chain to the root, to identify which
                // character/skeleton instance this bone belongs to.
                RndTransformable* root = bt; int depth = 0;
                while (root->TransParent() && depth++ < 64) root = root->TransParent();
                ObjectDir* bdir = bt->Dir();
                ObjectDir* bparent = bdir ? bdir->Dir() : nullptr;
                fprintf(stderr,
                    "[XBONE] showing=%d rootBone='%s' rootPtr=%p boneDirPtr=%p boneDirFile='%s' parentDir=%p parentName='%s'\n",
                    (int)mesh->Showing(), root->Name()?root->Name():"?", (void*)root,
                    (void*)bdir,
                    (bdir && bdir->mStoredFile.c_str())?bdir->mStoredFile.c_str():"-",
                    (void*)bparent, (bparent && bparent->Name())?bparent->Name():"-");
                fprintf(stderr,
                    "[XBONE] mesh='%s' bone='%s' bonePtr=%p meshPtr=%p worldPos=(%.1f,%.1f,%.1f) meshDir='%s'\n"
                    "    worldRot=[%.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f]\n"
                    "    offRot=[%.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f]\n"
                    "    skinPos=(%.2f,%.2f,%.2f)\n",
                    mesh->Name()?mesh->Name():"?", bt->Name(), (void*)bt, (void*)mesh,
                    wt.v.x, wt.v.y, wt.v.z,
                    mesh->Dir() && mesh->Dir()->Name() ? mesh->Dir()->Name() : "-",
                    wt.m.x.x, wt.m.x.y, wt.m.x.z, wt.m.y.x, wt.m.y.y, wt.m.y.z, wt.m.z.x, wt.m.z.y, wt.m.z.z,
                    off.m.x.x, off.m.x.y, off.m.x.z, off.m.y.x, off.m.y.y, off.m.y.z, off.m.z.x, off.m.z.y, off.m.z.z,
                    sk.v.x, sk.v.y, sk.v.z);
            }
        }
        // SKEW_PROBE: under NO_CLIP+NO_IK (bind pose) every skin matrix should be
        // identity. Report any skinned mesh whose worst bone deviates from
        // identity (max |element - I|), to find the mesh whose bind skinning is
        // broken (the static deformer). One line per mesh, throttled.
        if (getenv("SKEW_PROBE") && mesh->Name()) {
            float worst = 0.f; int worstB = -1;
            for (int b = 0; b < numBones; b++) {
                const float* d = bones.bones[b];
                float idn[16]; for (int i=0;i<16;i++) idn[i]=(i%5==0)?1.f:0.f;
                float mx = 0.f;
                for (int i=0;i<12;i++){ float e=std::fabs(d[i]-idn[i]); if(e>mx)mx=e; }
                if (mx>worst){ worst=mx; worstB=b; }
            }
            static std::unordered_map<std::string,int> sSK;
            const char* mn = mesh->Name();
            if (worst > 0.05f && sSK[mn]++ % 120 == 0) {
                RndTransformable* wb = (worstB>=0)?owner->BoneTransAt(worstB):nullptr;
                fprintf(stderr,"[SKEW_PROBE] mesh='%s' numBones=%d worstDev=%.3f worstBone[%d]='%s'\n",
                    mn, numBones, worst, worstB, (wb&&wb->Name())?wb->Name():"?");
            }
        }
        for (int b = numBones; b < kMaxBones; b++)
            for (int i = 0; i < 16; i++) bones.bones[b][i] = (i % 5 == 0) ? 1.f : 0.f;
        // V26: SHARD_BONE_DBG — runs INDEPENDENT of the V24 guard (so it works
        // with SHARD_GUARD_OFF=1). For a skinned mesh whose bone WORLD translations
        // spread far apart (a candidate shard), report the OUTLIER bone (farthest
        // from the per-mesh centroid) + its name/world/local. This localizes the
        // exact bone that produces a bad pose; it was the instrument that traced the
        // V26 root cause to the upper-arm chain (the MakeRotQuat sqrt(2) bug). NOTE:
        // a high spread is NOT proof of a shard for meshes weighted across BOTH
        // hands (fingernails) or wide crowd batches — those legitimately span far;
        // the rendered SHARD_RATIO (bind-vs-world AABB) is the truer metric.
        if (getenv("SHARD_BONE_DBG") && numBones >= 2) {
            float cx=0.f, cy=0.f, cz=0.f; int cn=0;
            for (int b=0;b<numBones;b++){ RndTransformable* bt=owner->BoneTransAt(b);
                if(!bt) continue; const Transform& w=bt->WorldXfm();
                if(std::fabs(w.v.x)<1e5f&&std::fabs(w.v.y)<1e5f&&std::fabs(w.v.z)<1e5f){
                    cx+=w.v.x; cy+=w.v.y; cz+=w.v.z; cn++; } }
            if (cn>0){ cx/=cn; cy/=cn; cz/=cn;
                int worst=-1; float worstD=0.f;
                for (int b=0;b<numBones;b++){ RndTransformable* bt=owner->BoneTransAt(b);
                    if(!bt) continue; const Transform& w=bt->WorldXfm();
                    float dx=w.v.x-cx, dy=w.v.y-cy, dz=w.v.z-cz;
                    float d=sqrtf(dx*dx+dy*dy+dz*dz);
                    if(d>worstD){ worstD=d; worst=b; } }
                if (worst>=0 && worstD>40.f) {
                    RndTransformable* bw=owner->BoneTransAt(worst);
                    const Transform& w=bw->WorldXfm();
                    const Transform& lx=bw->LocalXfm();
                    const char* mn = mesh->Name() ? mesh->Name() : "?";
                    static std::unordered_map<std::string,int> sBoneSeen;
                    if (sBoneSeen[mn]++ % 60 == 0)
                        fprintf(stderr, "[SHARD_BONE] mesh='%s' f=%d worstBone[%d]='%s' "
                            "dist=%.1f wv=(%.2f,%.2f,%.2f) lv=(%.3f,%.3f,%.3f)\n",
                            mn, mFrameCount, worst, bw->Name()?bw->Name():"?", worstD,
                            w.v.x,w.v.y,w.v.z, lx.v.x,lx.v.y,lx.v.z);
                }
            }
        }
        if (getenv("SMASH_DBG")) {
            const char* mn = mesh->Name() ? mesh->Name() : "?";
            static std::unordered_map<std::string,int> sSeen;
            if (sSeen[mn]++ == 0) {
                const Transform& b0 = owner->BoneOffsetAt(0);
                RndTransformable* bt0 = owner->BoneTransAt(0);
                fprintf(stderr, "[SMASH_DBG] skinned mesh='%s' nv=%d nf=%d numBones=%d "
                        "src=%s bone0='%s' bone0World=(%.2f,%.2f,%.2f) bone0Off.v=(%.2f,%.2f,%.2f)\n",
                        mn, nv, nf, numBones,
                        (verts.size() > 0) ? "verts" : "compressed",
                        bt0 && bt0->Name() ? bt0->Name() : "<null>",
                        bt0 ? bt0->WorldXfm().v.x : 0.f, bt0 ? bt0->WorldXfm().v.y : 0.f,
                        bt0 ? bt0->WorldXfm().v.z : 0.f, b0.v.x, b0.v.y, b0.v.z);
                if (!skinnedView.empty()) {
                    const GpuVertexSkinned& gv = skinnedView[0];
                    fprintf(stderr, "[SMASH_DBG]   v0 pos(%.2f,%.2f,%.2f) w(%.2f,%.2f,%.2f,%.2f) idx(%u,%u,%u,%u)\n",
                            gv.pos[0],gv.pos[1],gv.pos[2], gv.boneWeights[0],gv.boneWeights[1],
                            gv.boneWeights[2],gv.boneWeights[3], gv.boneIndices[0],gv.boneIndices[1],
                            gv.boneIndices[2],gv.boneIndices[3]);
                }
            }
        }
    } else {
        for (int b = 0; b < kMaxBones; b++)
            for (int i = 0; i < 16; i++) bones.bones[b][i] = (i % 5 == 0) ? 1.f : 0.f;
    }

    // V24: degenerate-skinned-triangle (teal shard) guard. The crowd / extras
    // characters' servo skeletons can momentarily produce a FINITE-but-wrong bone
    // pose (within the per-bone [-1e5,1e5] guard above, so not rejected there) for
    // a held prop / appendage — a vertex weighted to that bone flings a few-to-tens
    // of units away from the rest of the body, drawing the thin teal/green/yellow
    // triangular slivers + fans seen scattered through the crowd and above the
    // highway. They are SKINNED geometry (confirmed by bisection: skipping all
    // skinned meshes removes them). Detect it by comparing the mesh's ACTUAL
    // blended world extent to its bind-pose (local) extent: if a skinned mesh
    // blows up to many times its authored size, its pose is broken this frame —
    // skip drawing it (a transient dropped frame of a small crowd prop is
    // invisible; the alternative is a screen-crossing shard). Bind-pose props are
    // small (~1-10u), so a >6x blow-up with a >20u span is unambiguous. Opt out
    // via SHARD_GUARD_OFF for A/B.
    // V26: the ratio is now also computed when the guard is OFF (gated on
    // SHARD_RATIO_DBG) so the post-fix residual can be measured with the guard
    // disabled; the DROP itself still only fires when SHARD_GUARD_OFF is unset.
    if (skinned && (!getenv("SHARD_GUARD_OFF") || getenv("SHARD_RATIO_DBG"))) {
        bool guardActive = !getenv("SHARD_GUARD_OFF");
        // Read bind verts through skinnedView so a cache-skipped unpack still
        // ratio-tests the same bind-pose data (cache == this draw's would-be unpack).
        int n = (int)skinnedView.size();
        if (n >= 3) {
            // Bind-pose (local) AABB and the blended (world) AABB, the latter via
            // the EXACT 4-bone blend the shader uses.
            float lmn[3]={1e30f,1e30f,1e30f}, lmx[3]={-1e30f,-1e30f,-1e30f};
            float wmn[3]={1e30f,1e30f,1e30f}, wmx[3]={-1e30f,-1e30f,-1e30f};
            int step = n > 256 ? (n / 256) : 1; // sample to bound cost
            for (int i = 0; i < n; i += step) {
                const GpuVertexSkinned& g = skinnedView[i];
                float lx=g.pos[0], ly=g.pos[1], lz=g.pos[2];
                if(lx<lmn[0])lmn[0]=lx; if(lx>lmx[0])lmx[0]=lx;
                if(ly<lmn[1])lmn[1]=ly; if(ly>lmx[1])lmx[1]=ly;
                if(lz<lmn[2])lmn[2]=lz; if(lz>lmx[2])lmx[2]=lz;
                float wsum=0.f, ox=0.f,oy=0.f,oz=0.f;
                for (int k=0;k<4;k++){
                    int bi=g.boneIndices[k]; if(bi<0||bi>=kMaxBones)bi=0;
                    float w=g.boneWeights[k]; wsum+=w;
                    const float* m=bones.bones[bi];
                    ox+=w*(m[0]*lx+m[4]*ly+m[8]*lz+m[12]);
                    oy+=w*(m[1]*lx+m[5]*ly+m[9]*lz+m[13]);
                    oz+=w*(m[2]*lx+m[6]*ly+m[10]*lz+m[14]);
                }
                if (wsum < 0.01f) { const float* m=bones.bones[0];
                    ox=m[0]*lx+m[4]*ly+m[8]*lz+m[12];
                    oy=m[1]*lx+m[5]*ly+m[9]*lz+m[13];
                    oz=m[2]*lx+m[6]*ly+m[10]*lz+m[14]; }
                if(ox<wmn[0])wmn[0]=ox; if(ox>wmx[0])wmx[0]=ox;
                if(oy<wmn[1])wmn[1]=oy; if(oy>wmx[1])wmx[1]=oy;
                if(oz<wmn[2])wmn[2]=oz; if(oz>wmx[2])wmx[2]=oz;
            }
            float lext = sqrtf((lmx[0]-lmn[0])*(lmx[0]-lmn[0])+(lmx[1]-lmn[1])*(lmx[1]-lmn[1])+(lmx[2]-lmn[2])*(lmx[2]-lmn[2]));
            float wext = sqrtf((wmx[0]-wmn[0])*(wmx[0]-wmn[0])+(wmx[1]-wmn[1])*(wmx[1]-wmn[1])+(wmx[2]-wmn[2])*(wmx[2]-wmn[2]));
            // RATIO test (blended-extent / bind-extent). Measuring every skinned
            // mesh over the song shows a roughly bimodal split: correctly-posed
            // meshes (crowd bodies, extras bodies, hair, mic stand, animated
            // limbs) sit at ratio ~1.0-1.9; the shard-producing exploded poses
            // jump to ~2.0-12x (e.g. 51squier_strings 35u->100u, fingernails
            // 36u->312u, extras' heads 15u->200u, an exploded crowd body
            // 87u->305u). A 2.0x threshold drops the prominent screen-crossing
            // teal shards while keeping every legitimately-posed/animated mesh —
            // crucially NO crowd-body or band-player BODY is dropped (verified).
            // A pure max-triangle-edge test and an absolute world-span cap were
            // both tried and REJECTED: crowd-row meshes are large/batched and
            // legitimately have long edges + big AABBs, so those tests gutted the
            // crowd. The ratio is the only metric that separates cleanly. Opt out:
            // SHARD_GUARD_OFF. Residual: a few small held-prop slivers (lighter/
            // clap, ratio <2.0) can still flicker for a frame — see VENUE_RENDER
            // V24 (a full fix needs the CharServo skeleton-math root-cause).
            bool degenerate = (wext > 15.f) && (lext > 0.001f) && (wext > 2.0f * lext);
            // SHARD_RATIO_DBG: log EVERY skinned mesh's bind/world extent + ratio,
            // throttled per pointer, to see which slivers slip the threshold.
            if (getenv("SHARD_RATIO_DBG") && wext > 8.f) {
                const char* mn = mesh->Name() ? mesh->Name() : "?";
                static std::unordered_map<const void*,int> sR;
                if (sR[(const void*)mesh]++ % 60 == 0)
                    fprintf(stderr, "[SHARD_RATIO] mesh='%s' bindExt=%.2f worldExt=%.2f ratio=%.2f%s\n",
                        mn, lext, wext, wext/(lext+1e-6f), degenerate?" DROP":"");
            }
            if (degenerate && guardActive) {
                if (getenv("SHARD_DBG")) {
                    const char* mn = mesh->Name() ? mesh->Name() : "?";
                    fprintf(stderr, "[SHARD_GUARD] dropped degenerate skinned mesh='%s' "
                        "bindExt=%.2f worldExt=%.2f ratio=%.1f f=%d\n",
                        mn, lext, wext, wext/(lext+1e-6f), mFrameCount);
                }
                mDrawnMeshes++; // count it as handled; just don't emit the shard
                return;
            }
        }
    }

    // Per-(mesh,instance) persistent bone uniform buffer + bind group (created
    // once per slot, reused across frames). The bone palette is this mesh's
    // per-frame ANIMATION data — it changes every frame for skinned meshes (static
    // meshes get a constant identity palette) — so we WriteBuffer it each draw, but
    // into THIS instance's OWN buffer. The bind group is built once per slot. (Was
    // a per-draw ring-offset bind group → unbounded under web submit-queue
    // backpressure.)
    if (!slot.boneUB) {
        wgpu::BufferDescriptor ubd{};
        ubd.label = "MeshBoneUB";
        ubd.size = sizeof(BoneUniforms);
        ubd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        slot.boneUB = mGpu.Device().CreateBuffer(&ubd);
        sMeshBufCreatesThisFrame++;
        wgpu::BindGroupEntry e{};
        e.binding = 0; e.buffer = slot.boneUB; e.offset = 0; e.size = sizeof(BoneUniforms);
        wgpu::BindGroupDescriptor bd{};
        bd.layout = mPipelines.BoneLayout(); bd.entryCount = 1; bd.entries = &e;
        slot.boneBG = mGpu.Device().CreateBindGroup(&bd);
        sMeshBGCreatesThisFrame++;
    }
    mGpu.Queue().WriteBuffer(slot.boneUB, 0, &bones, sizeof(bones));
    wgpu::BindGroup boneBG = slot.boneBG;

    // --- Material uniforms ---
    RndMat* mat = mesh->Mat();
    MaterialUniforms mu{};
    // W5 text-mesh heuristic — mirror the DC3 draw path (Mesh_Wgpu.cpp:188).
    // RndText::UpdateMesh / RndText::CreateLines build per-font sub-meshes via
    // Hmx::Object::New<RndMesh>() (src/system/rndobj/Text.cpp:1766) and never
    // assign a Name, so an empty first byte is a reliable text-mesh
    // discriminator. Every gameplay/scene mesh in RB3 has a non-empty Name(),
    // so the predicate only fires on RndText sub-meshes. This is the same
    // condition MaterialSetup::BuildMaterialParams uses to set
    // useAlphaAsRGB + prelit for text on the DC3 draw path; without it, RB3
    // font atlases (DXT5 with glyphs in alpha, RGB == 0) collapse to black in
    // the shader's non-text diffuse-sampling branch (standard_wgsl.inc:631),
    // which is why menu / song-list / HUD text was invisible in the W4
    // baseline screenshots. See docs/plans/web-port/W5_TEXT_RENDERING.md.
    bool isTextMeshHeur = mesh->Name() && mesh->Name()[0] == '\0';
    // W7 Phase 3 Tier 1 — broaden the "looks like UI text" predicate to also
    // catch dim NAMED meshes that the W5/Phase-1 empty-name discriminator
    // misses. Static analysis (W5 doc + Text.cpp:1766) confirms RndText
    // sub-meshes created via Hmx::Object::New<RndMesh>() ARE caught, but
    // the W5p3 Tier-2 attempt (engine 08b3932) lifted those colours to
    // max(0.6, c) and pixel output was BYTE-IDENTICAL to baseline, proving
    // the residual song-row title + HUD-digit dimness is NOT on the
    // empty-name path. The static trace identifies the missed widgets:
    //
    //   * BandScoreboard digit slots — src/system/bandobj/BandScoreboard.cpp
    //     loads NAMED `num%d.mesh`, `thousands_comma.mesh`,
    //     `millions_comma.mesh`, `%d_source.mesh` from the .milo
    //     (BandScoreboard::SetupScore, lines 56-93). These are textured
    //     digit-sprite quads using a normal RGB diffuse — NOT an alpha-only
    //     font atlas — so useAlphaAsRGB would WRONGLY zero their RGB. We
    //     therefore only apply the colour FLOOR for the broader set;
    //     useAlphaAsRGB / prelit / zMode stay gated on the original
    //     empty-name discriminator (correct for the per-glyph quads of
    //     RndText::UpdateMesh).
    //   * UILabel `*.lbl`-named widgets — drawn through UILabel::DrawShowing
    //     which rewrites the font material's colour per draw from
    //     data-driven UIColors (W5 Phase 3 root cause). The glyph quads
    //     themselves are unnamed (already caught) but in some draw paths
    //     the widget mesh carries the dim mat colour without a sub-mesh.
    //   * Font/label materials by name — RB3 ships fonts as `*font*` /
    //     `*label*` materials; catching the mat-name pattern adds a third
    //     safety net for any text path that escapes both above heuristics.
    //
    // Predicate is conservative: pure name pattern matches, no nullptr
    // deref (Name() pointers and mat pointer all guarded), and the lift
    // uses std::max() so already-bright text (the W5-Phase-1 wins:
    // news-ticker / FRIEND-RANKINGS / CHOOSE-INSTRUMENT) is unchanged.
    const char* meshName = mesh->Name();
    const char* matName = (mat && mat->Name()) ? mat->Name() : "";
    bool isLikelyUiText = isTextMeshHeur;
    if (!isLikelyUiText && meshName && meshName[0]) {
        // BandScoreboard digit/source meshes — see BandScoreboard.cpp:79-91.
        if ((meshName[0] == 'n' && std::strncmp(meshName, "num", 3) == 0) ||
            std::strstr(meshName, "_source.mesh") ||
            std::strstr(meshName, "_comma.mesh")) {
            isLikelyUiText = true;
        }
        // Generic UILabel `*.lbl`-suffixed widgets.
        else if (std::strstr(meshName, ".lbl")) {
            isLikelyUiText = true;
        }
    }
    if (!isLikelyUiText && matName[0]) {
        // Font / label material name patterns — third safety net.
        if (std::strstr(matName, "font") || std::strstr(matName, "label")) {
            isLikelyUiText = true;
        }
    }
    if (mat) {
        const Hmx::Color& c = mat->GetColor();
        mu.color[0] = c.red; mu.color[1] = c.green; mu.color[2] = c.blue; mu.color[3] = c.alpha;
        // W7 Phase 3 Tier 1 — colour floor for UI-text-class meshes.
        // The Phase 3 static trace established that for some screens the
        // shipping .milo data drives the font material's GetColor() to
        // (0.20..0.50, ..., 1.0) — fine on a 480i CRT (retail target)
        // but invisible on a 1280x720 canvas alpha-blended over near-
        // black UI panels. Lift each channel to >= 0.6, preserving alpha
        // (used downstream as the fringe-fade input). No-op for already-
        // bright labels — max() guards regressions on the news-ticker /
        // FRIEND-RANKINGS / CHOOSE-INSTRUMENT text the W5 Phase 1 fix
        // recovered.
        if (isLikelyUiText) {
            mu.color[0] = std::max(0.6f, mu.color[0]);
            mu.color[1] = std::max(0.6f, mu.color[1]);
            mu.color[2] = std::max(0.6f, mu.color[2]);
        }
        mu.alphaThreshold = mat->mAlphaCut ? (mat->mAlphaThresh / 255.0f) : 0.0f;
        // V1: enable texture sampling if the material has a valid diffuse map.
        // We attempt a lazy upload here so meshes whose textures load mid-frame
        // still pick them up on subsequent draws (the MakeMaterialBindGroup
        // call below uploads + binds the matching view).
        RndTex* dt = mat->GetDiffuseTex();
        bool hasTex = false;
        if (dt) {
            wgpu::TextureView v = GetRB3TexView(dt);
            if (!v) v = UploadRndTexIfNeeded(mGpu, dt);
            if (v) hasTex = true;
        }
        mu.useTexture = hasTex ? 1.0f : 0.0f;
        mu.intensify = mat->mIntensify ? 2.0f : 1.0f;
        // W5 Phase 1: force-prelit text so font glyph quads pick up the
        // material colour directly without lighting attenuation.
        mu.prelit = (mat->mPreLit || isTextMeshHeur) ? 1.0f : 0.0f;
        // W5 Phase 1: route font atlas glyph alpha into RGB. Mirrors the
        // shader's text branch (standard_wgsl.inc, useAlphaAsRGB == 1.0):
        //   baseColor.rgb *= diffuseSample.a;
        // instead of the default
        //   baseColor.rgb *= diffuseSample.rgb;
        // which is the actual root cause of W5's black-on-dark text.
        //
        // EXCEPTION — colour-icon glyph fonts (the overshell instrument-icon
        // font instrument_icons_small*, whose material name contains "icon"):
        // these are unnamed RndText glyph submeshes (so isTextMeshHeur fires)
        // but their atlas holds real RGB artwork (the guitar/bass/drums/vocals/
        // keys rings) with alpha as a solid circular cell MASK. useAlphaAsRGB
        // would discard the RGB artwork and multiply the (white) base colour by
        // the solid-circle alpha -> a SOLID WHITE CIRCLE (the overshell
        // white-blob bug). Letter fonts (Pentatonic_*) carry no "icon" in their
        // material name, so they keep the alpha->RGB glyph path.
        bool isColorIconFont = matName[0] && std::strstr(matName, "icon") != nullptr;
        mu.useAlphaAsRGB = (isTextMeshHeur && !isColorIconFont) ? 1.0f : 0.0f;
        // W9 tail-color fix — apply the material's texture-coordinate transform.
        // RB3 sustain "tail" materials (tail_green.mat, tail_red.mat, ...) all
        // share one diffuse atlas (gem_tails.tex, an 8bpp DXT5 128x128 image of
        // vertically-striped fret colours) and a WHITE base mColor. Each tail
        // mesh has a CONSTANT u (~0.008) and full-height v; the per-fret colour
        // is selected entirely by the material's TexXfm U-translation
        // (TexXfm().v.x: 0.0 green, -0.115 red, -0.335 blue, ...), which shifts
        // the sample column onto a different coloured strip. Without writing
        // texGenMode + texXfmRow0/1 the shader takes its kTexGenNone branch (raw
        // u==0.008 for every material) and all tails sample the same column ->
        // uniformly white. This mirrors the DC3 draw path
        // (MaterialSetup::BuildMaterialParams, MaterialSetup.cpp:182-190).
        TexGen texGen = mat->mTexGen;
        mu.texGenMode = (float)texGen;
        if (texGen == kTexGenXfm || texGen == kTexGenXfmOrigin ||
            texGen == kTexGenProjected) {
            const Transform& txf = mat->TexXfm();
            mu.texXfmRow0[0] = txf.m.x.x; mu.texXfmRow0[1] = txf.m.x.y;
            mu.texXfmRow0[2] = txf.v.x;   mu.texXfmRow0[3] = txf.v.z;
            mu.texXfmRow1[0] = txf.m.y.x; mu.texXfmRow1[1] = txf.m.y.y;
            mu.texXfmRow1[2] = txf.v.y;   mu.texXfmRow1[3] = 0.0f;
        }
        // W9 tail-color fix (part 2): drive the per-fret colour from the tail
        // MATERIAL NAME and let the shared gem_tails.tex atlas supply only the
        // luminance/shape. The atlas is one image of vertically-striped fret
        // colours selected per material by a TexXfm U-offset; replicating the
        // Wii GX texture-matrix + sampler-wrap convention exactly so each fret
        // lands on its own strip is brittle (some frets wrap onto the atlas's
        // blank/white strip and render white). Because every tail mesh samples
        // a single near-constant texel column, the atlas effectively contributes
        // a vertical luminance ramp; multiplying it by the fret colour derived
        // from the material name (tail_green/red/yellow/blue/orange/...) yields
        // a correctly-tinted, shaped tail without depending on the fragile
        // strip-UV mapping. tail_bonus/star and tail_white stay white by name.
        {
            const char* mn = mat->Name();
            const char* tc = mn ? std::strstr(mn, "tail_") : nullptr;
            if (tc) {
                tc += 5; // past "tail_"
                struct { const char* name; float r,g,b; } kFret[] = {
                    {"green",  0.18f, 0.85f, 0.20f},
                    {"red",    0.90f, 0.16f, 0.13f},
                    {"yellow", 0.95f, 0.85f, 0.10f},
                    {"blue",   0.13f, 0.55f, 0.92f},
                    {"orange", 0.95f, 0.50f, 0.08f},
                    {"purple", 0.62f, 0.20f, 0.85f},
                };
                for (auto& f : kFret) {
                    size_t L = std::strlen(f.name);
                    if (std::strncmp(tc, f.name, L) == 0 && tc[L] == '.') {
                        // Drive the fret colour directly and drop the atlas tint
                        // (useTexture=0): the atlas only contributes the fragile
                        // strip selection we can't reproduce, so a solid fret
                        // colour is both correct and clean. The tail's vertex
                        // alpha + SrcAlphaAdd blend still give it the lit look.
                        mu.color[0] = f.r; mu.color[1] = f.g; mu.color[2] = f.b;
                        mu.useTexture = 0.0f;
                        break;
                    }
                }
                // tail_white / tail_bonus(star) / tail_chord / tail_miss keep the
                // material's own (white/grey) colour — correct for star power etc.
            }
        }
        // CHAR_DBG: one-shot per skinned mesh — report whether the character
        // outfit material resolved a diffuse texture (and what kind), to tell
        // "untextured because no tex bound" from "untextured because the
        // render-to-texture outfit composite never painted the target".
        if (skinned && getenv("CHAR_DBG")) {
            const char* mn = mesh->Name() ? mesh->Name() : "(null)";
            static std::unordered_map<std::string,int> sCharDbgSeen;
            if (sCharDbgSeen[mn]++ == 0) {
                int texType = dt ? (int)dt->GetType() : -1;
                fprintf(stderr,
                    "CHAR_DBG: skinned mesh '%s' mat='%s' diffuse=%s type=0x%x hasTexView=%d "
                    "color=(%.2f,%.2f,%.2f) bones=%d\n",
                    mn,
                    mat->Name() ? mat->Name() : "(null)",
                    dt ? (dt->Name() ? dt->Name() : "(unnamed)") : "(null)",
                    texType, hasTex ? 1 : 0,
                    c.red, c.green, c.blue, owner->NumBones());
            }
        }
    } else {
        mu.color[0] = mu.color[1] = mu.color[2] = mu.color[3] = 1.0f;
        mu.useTexture = 0.0f; mu.intensify = 1.0f; mu.prelit = 0.0f;
    }
    if (getenv("RB3_LIGHT_PROBE")) {
        const char* mn = mesh->Name() ? mesh->Name() : "<noname>";
        static std::unordered_map<std::string,int> sLightProbeSeen;
        if (sLightProbeSeen[mn]++ == 0) {
            RndCam* pc = RndCam::sCurrent;
            RndEnviron* pe = RndEnviron::sCurrent;
            RndTex* em = mat ? (RndTex*)mat->mEmissiveMap : nullptr;
            RndTex* dt = mat ? mat->GetDiffuseTex() : nullptr;
            Hmx::Color mc = mat ? mat->GetColor() : Hmx::Color(1,1,1,1);
            fprintf(stderr,
                "[LIGHT_PROBE] mesh='%s' cam='%s' env='%s' prelit=%d blend=%d color=(%.2f,%.2f,%.2f,%.2f) mat='%s' diff=%s emisMul=%.2f emisMap=%s\n",
                mn, (pc && pc->Name()) ? pc->Name() : "<none>",
                (pe && pe->Name()) ? pe->Name() : "<none>",
                mat ? (mat->mPreLit ? 1 : 0) : -1,
                mat ? (int)mat->GetBlend() : -1,
                mc.red, mc.green, mc.blue, mc.alpha,
                (mat && mat->Name()) ? mat->Name() : "<nomat>",
                dt ? (dt->Name() ? dt->Name() : "<unnamed>") : "null",
                mat ? mat->mEmissiveMultiplier : 0.f,
                em ? (em->Name() ? em->Name() : "<unnamed>") : "null");
        }
    }
    // Track-A glow: the gameplay-track look, applied ONLY under game.cam
    // (near30/far224) so the venue/band/crowd (drawn under world.cam) are untouched.
    // Parts:
    //  (1) Darken the prelit highway SURFACE so the prelit gems/lanes pop against a
    //      near-black track (retail's highway is dark, not the native mid-gray).
    //      surface.mat is prelit white; scale its base down (its bass-clef watermark
    //      survives via the emissive term below).
    //  (2) Re-enable material EMISSIVE (this backend's DrawMesh dropped it). Set the
    //      multiplier here (guarded by emissive-map presence — many mats have mult>0
    //      but a null map); the map texture itself is bound in MakeMaterialBindGroup.
    // Default-ON (the dark-track look is the correct native gameplay appearance);
    // RB3_TRACK_LIGHT_OFF=1 is the opt-out escape hatch for A/B (mirrors RB3_BLOOM_OFF).
    static int sTrackLight = -1;
    if (sTrackLight < 0) { const char* e = getenv("RB3_TRACK_LIGHT_OFF"); sTrackLight = (e && e[0] && e[0] != '0') ? 0 : 1; }
    if (sTrackLight && mat) {
        RndCam* pc = RndCam::sCurrent;
        if (pc && pc->Name() && std::strcmp(pc->Name(), "game.cam") == 0) {
            const char* mname = mat->Name() ? mat->Name() : "";
            if (std::strcmp(mname, "surface.mat") == 0) {
                mu.color[0] *= 0.12f; mu.color[1] *= 0.12f; mu.color[2] *= 0.12f;
            }
            // Lit lanes: the track rails (rails.tex) are non-prelit, so they're
            // dimmed by the flat ambient and read as near-black separators on the
            // now-dark surface — the opposite of retail's bright glowing lane
            // dividers. Force them prelit so rails.tex shows at full authored
            // brightness (self-lit lanes), matching the dark-highway/bright-lane look.
            else if (std::strcmp(mname, "rails.mat") == 0) {
                mu.prelit = 1.0f;
                // rails.tex is authored bright-white; retail's lane dividers are a
                // cooler blue-white (measured normalized ~0.58/0.70/1.00 — blue
                // dominant, red suppressed). Apply a per-channel cool tint whose mean
                // is ~0.7, so overall lane brightness matches the prior flat ×0.7 but
                // the hue shifts toward retail and the gems stay the focal point.
                mu.color[0] *= 0.53f; mu.color[1] *= 0.64f; mu.color[2] *= 0.92f;
            }
            RndTex* emTex = (RndTex*)mat->mEmissiveMap;
            mu.emissiveMultiplier = emTex ? mat->mEmissiveMultiplier : 0.0f;
            // Brighter "now bar": the additive strike-line glow (gem_smasher_glow,
            // square_smasher_bright_*.tex, ships emissive mul 0.90) is dimmer/narrower
            // than retail's luminous now bar — boost its emissive contribution.
            if (std::strcmp(mname, "gem_smasher_glow.mat") == 0) {
                mu.emissiveMultiplier *= 2.0f;
            }
            // SP "peak state" blue track overlay (peakstate_plane, spotlight_*_track.tex
            // filigree) fades in via PropAnim once the streak hits 4x, but draws faint
            // (gray base color × blue diffuse, alpha-blended). Brighten its color so the
            // blue reads as a glow over the dark track (retail's SP track is vividly
            // blue); the anim's alpha still drives the fade-in.
            if (std::strstr(mname, "peakstate") != nullptr) {
                mu.color[0] *= 2.0f; mu.color[1] *= 2.0f; mu.color[2] *= 2.0f;
            }
        }
    }
    // GEM_FORCE: debug — render prism gems as opaque bright magenta with depth
    // disabled, to disambiguate "geometry missing/off-screen/clipped" from
    // "material/alpha/occlusion makes it invisible". (Used in V13 to prove the
    // gems were frustum-clipped by a stale stationary-camera viewProj rather
    // than a material problem; left gated for future gem-render triage.)
    bool gemForce = false;
    if (getenv("GEM_FORCE")) {
        const char* mn = mesh->Name() ? mesh->Name() : "?";
        if (std::strstr(mn, "prism_gem")) {
            gemForce = true;
            mu.color[0] = 1.0f; mu.color[1] = 0.0f; mu.color[2] = 1.0f; mu.color[3] = 1.0f;
            mu.useTexture = 0.0f; mu.intensify = 1.0f; mu.prelit = 1.0f;
            mu.alphaThreshold = 0.0f;
        }
    }
    // Per-(mesh,instance) persistent material uniform buffer + cached bind group.
    // The material uniforms (mu: animated colour/emissive/etc.) change per frame
    // AND per instance (the same shared mesh tints differently per list row) so we
    // WriteBuffer them each draw into THIS instance's OWN buffer. The bind group
    // also binds the resolved diffuse/emissive texture VIEWS, which change only
    // when a lazy texture upload completes or the material pointer is swapped — so
    // we rebuild it only when those handles change, not every frame. (Was a
    // per-draw ring-offset bind group → unbounded under web submit-queue
    // backpressure.)
    if (!slot.matUB) {
        wgpu::BufferDescriptor ubd{};
        ubd.label = "MeshMatUB";
        ubd.size = sizeof(MaterialUniforms);
        ubd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        slot.matUB = mGpu.Device().CreateBuffer(&ubd);
        sMeshBufCreatesThisFrame++;
    }
    mGpu.Queue().WriteBuffer(slot.matUB, 0, &mu, sizeof(mu));
    {
        // Resolve the current diffuse/emissive views (cheap: a texture-cache hit
        // after the first frame; lazily uploads on a late arrival) WITHOUT
        // building a bind group, then rebuild the cached bind group only when the
        // material pointer or a resolved view handle actually changed. At steady
        // state this creates ZERO bind groups per frame.
        wgpu::TextureView diffuse, emissive;
        ResolveMaterialViews(mat, diffuse, emissive);
        if (!slot.matBG || slot.matKey != (const void*)mat ||
            slot.matDiffuseView != diffuse.Get() ||
            slot.matEmissiveView != emissive.Get()) {
            slot.matBG = MakeMaterialBindGroupCached(slot.matUB, diffuse, emissive);
            sMeshBGCreatesThisFrame++;
            slot.matKey = (const void*)mat;
            slot.matDiffuseView = diffuse.Get();
            slot.matEmissiveView = emissive.Get();
        }
    }
    wgpu::BindGroup matBG = slot.matBG;

    // --- Pipeline state from material (V4: honor material blend/zmode) ---
    // RndMat::Blend enum constants (kBlendDest..kPreMultAlpha = 0..7) match
    // WgpuBlend enum constants 1:1 — direct cast is safe. Same for ZMode
    // (kZModeDisable..kZModeDecal = 0..4 match WgpuZMode::Disable..Decal).
    // V3 always used Src/Normal which made text/UI render opaque (text glyph
    // quads have a font-atlas alpha texture but kBlendSrc writes that alpha
    // directly to FB → no compositing → text reads as flat low-contrast
    // sprites instead of legible characters). Mapping material blend lets text
    // (SrcAlpha) properly composite over the underlying geometry.
    PipelineKey key{};
    key.shaderType = 0;
    WgpuBlend blendMode = WgpuBlend::Src;
    WgpuZMode zMode = WgpuZMode::Normal;
    if (mat) {
        // Defensive clamp — Blend is bit-field, malformed milos could yield
        // out-of-range values.
        int b = (int)mat->GetBlend();
        if (b >= 0 && b <= 7) blendMode = (WgpuBlend)b;
        int z = (int)mat->GetZMode();
        if (z >= 0 && z <= 4) zMode = (WgpuZMode)z;
    }
    // W5 Phase 2: force depth-disabled rendering for text meshes so HUD /
    // menu labels composite over the 3D scene regardless of panel transform
    // Z (mirrors Mesh_Wgpu.cpp:202 on the DC3 draw path). Without this,
    // text behind a closer 3D panel would get z-occluded and disappear.
    if (isTextMeshHeur) zMode = WgpuZMode::Disable;
    if (gemForce) { blendMode = WgpuBlend::Src; zMode = WgpuZMode::Disable; }
    key.blend = blendMode;
    key.zMode = zMode;
    key.cull = WgpuCull::None;        // draw both sides (RB3 winding varies)
    key.stencil = WgpuStencil::Ignore;
    // V14: skinned meshes use the 88-byte SkinnedLayout + vs_skinned entry point.
    key.layout = skinned ? VertexLayoutType::Skinned : VertexLayoutType::Static;
    key.targetFormat = mTargetFmt;
    key.sampleCount = 1;
    key.hasDepth = true;
    key.alphaCut = mat ? mat->mAlphaCut : false;
    // RTT: the render-target pass is an RGBA8 color attachment with NO depth.
    // Select an RT-compatible pipeline variant (matching target format, no
    // depth-stencil, and alpha writes enabled so the painted target carries a
    // real alpha channel — the sky-dome material composites the clouds via it).
    bool rtPass = (mRtActiveTex != nullptr);
    if (rtPass) {
        key.targetFormat = mRtFmt;
        key.hasDepth = false;
    }
    // Mask alpha writes so the framebuffer alpha stays at the clear value (1.0).
    // RB3 materials often have color.alpha=0 for UI/text fill quads — with Src
    // blending (One/Zero) that would propagate fragment alpha=0 to the
    // framebuffer, which the PNG readback then encodes as RGBA(0,0,0,0). The
    // PNG viewer renders that as transparent / "blank white", which gave the
    // illusion that frames 04/05 (main_hub/quickplay) drew nothing — they were
    // drawing geometry, but every pixel ended up with alpha=0. Writing only
    // RGB keeps the readback opaque.
    //
    // RTT exception: the render-target pass MUST write alpha — the sky-dome
    // material samples the painted target and composites the clouds via its
    // alpha, so masking alpha would leave the target opaque-everywhere.
    key.alphaWrite = rtPass ? true : false;

    wgpu::RenderPipeline pipe = mPipelines.GetPipeline(key);
    if (!pipe) return;

    // P1 highway bloom CAPTURE (Design B): under the live game.cam highway pass,
    // record a verbatim replay of each halo-source draw — pipeline, the LIVE
    // pose-baked mSceneBindGroup HANDLE (NOT mSceneOffset: game.cam is re-posed
    // mid-frame, so replaying against the single final pose would mis-place every
    // halo; capturing the per-draw bind-group handle replays each source against
    // its authored pose), the mat/obj/bone bind groups, and the vbuf/ibuf/count.
    // No GPU work here — a leaf push onto mHaloDraws, consumed in EndFrame's
    // CompositeHaloBloom. Inert when RB3_HIGHWAY_BLOOM_OFF=1.
    if (HighwayBloomEnabled() && RndCam::sCurrent && RndCam::sCurrent->Name() &&
        std::strcmp(RndCam::sCurrent->Name(), "game.cam") == 0 && IsHaloSourceMat(mat)) {
        if (mHaloDraws.capacity() == 0) mHaloDraws.reserve(16);
        // Capture this instance's obj/bone/mat bind groups + buffers. Each draw
        // claims its OWN per-(mesh,instance) uniform slot, so even a mesh drawn
        // multiple times per frame never overwrites a captured slot before
        // EndFrame's replay — the captured handles are valid (slots are only
        // recycled at the NEXT BeginFrame, which also clears mHaloDraws).
        mHaloDraws.push_back({pipe, mSceneBindGroup, matBG, objBG, boneBG,
                              vbuf, ibuf, cachedIndexCount});
    }

    mPass.SetPipeline(pipe);
    mPass.SetBindGroup(0, mSceneBindGroup, 0, nullptr);
    mPass.SetBindGroup(1, matBG, 0, nullptr);
    mPass.SetBindGroup(2, objBG, 0, nullptr);
    mPass.SetBindGroup(3, boneBG, 0, nullptr);
    mPass.SetVertexBuffer(0, vbuf, 0, WGPU_WHOLE_SIZE);
    mPass.SetIndexBuffer(ibuf, wgpu::IndexFormat::Uint16, 0, WGPU_WHOLE_SIZE);
    mPass.DrawIndexed(cachedIndexCount, 1, 0, 0, 0);

    mDrawnMeshes++;
    mDrawnTris += nf;
}

// ---------------------------------------------------------------------------
// RB3EnsureMeshGpu — idempotent mesh-upload helper extracted from DrawMesh's
// needUpload block. Unpacks (shared RB3UnpackMeshVerts) + uploads VB/IB + stamps
// the sMeshGpu fingerprint with the SAME keys DrawMesh uses, AND populates the L1
// skinned bind-vert cache. So after a warm pass, the first real draw of this mesh
// sees needUpload==false (geometry-buffer reuse), skipUnpack==true (no re-unpack),
// and the skinned shard guard reads the warmed cache — zero reveal-frame work.
// Returns true iff it actually uploaded (cache miss). No render-pass dependency:
// it only creates+writes buffers (queue ops), so it is safe to call outside an
// open pass during the loading dwell. Used by BandRnd::WarmGpuForDir.
// ---------------------------------------------------------------------------
static bool RB3EnsureMeshGpu(BandRnd& rnd, RndMesh* mesh) {
    if (!mesh) return false;
    RndMesh* owner = mesh->GeomOwner();
    if (!owner) owner = mesh;
    std::vector<RndMesh::Face>& faces = owner->mFaces;
    int nf = (int)faces.size();
    if (nf <= 0) return false;

    bool skinned = owner->IsSkinned();
    int nvSrc = owner->mVerts.size();
    int fpVertsKey = (nvSrc > 0) ? nvSrc : (int)owner->mNumCompressedVerts;

    RB3MeshEntry& meshEntry = sMeshGpu[mesh];
    bool needUpload = !meshEntry.uploaded ||
                      meshEntry.ownerKey != (const void*)owner ||
                      meshEntry.fpVerts != fpVertsKey || meshEntry.fpFaces != nf ||
                      meshEntry.fpSkinned != skinned;
    // Already resident with the warm L1 caches in place -> nothing to do. (For
    // skinned meshes, only consider it warm once the bind-vert cache is populated,
    // so the warmed first-draw shard guard has data.)
    if (!needUpload && (!skinned || !meshEntry.cachedSkinnedVerts.empty()))
        return false;

    std::vector<GpuVertexRB3> gpuVerts;
    std::vector<GpuVertexSkinned> gpuVertsSkinned;
    int nv = RB3UnpackMeshVerts(owner, skinned, gpuVerts, gpuVertsSkinned);
    if (nv < 0) return false;

    // L1: warm the skinned bind-vert cache (read every frame by the shard guard).
    if (skinned)
        meshEntry.cachedSkinnedVerts = gpuVertsSkinned;

    // If the GPU buffers are already current (only the skinned cache was missing),
    // refresh the cache above and stop — don't recreate identical buffers.
    if (!needUpload)
        return false;

    // Local bounding sphere for static meshes (mirrors DrawMesh's needUpload arm —
    // compressed venue meshes have no other place to recompute it).
    if (!skinned && nv > 0) {
        float mn3[3] = { 1e30f, 1e30f, 1e30f }, mx3[3] = { -1e30f, -1e30f, -1e30f };
        for (int i = 0; i < nv; i++)
            for (int k = 0; k < 3; k++) {
                float p = gpuVerts[i].pos[k];
                if (p < mn3[k]) mn3[k] = p;
                if (p > mx3[k]) mx3[k] = p;
            }
        Vector3 center((mn3[0] + mx3[0]) * 0.5f, (mn3[1] + mx3[1]) * 0.5f,
                       (mn3[2] + mx3[2]) * 0.5f);
        float dx = mx3[0] - mn3[0], dy = mx3[1] - mn3[1], dz = mx3[2] - mn3[2];
        float radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
        Sphere localSphere;
        localSphere.Set(center, radius);
        mesh->SetSphere(localSphere);
    }

    std::vector<uint16_t> indices;
    indices.reserve(nf * 3);
    for (int i = 0; i < nf; i++) {
        indices.push_back(faces[i].v1);
        indices.push_back(faces[i].v2);
        indices.push_back(faces[i].v3);
    }
    {
        wgpu::BufferDescriptor bd{};
        bd.label = "MeshVB";
        bd.size = skinned ? ((uint64_t)nv * sizeof(GpuVertexSkinned))
                          : ((uint64_t)nv * sizeof(GpuVertexRB3));
        bd.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        meshEntry.vbuf = rnd.mGpu.Device().CreateBuffer(&bd);
        sMeshBufCreatesThisFrame++;
        rnd.mGpu.Queue().WriteBuffer(meshEntry.vbuf, 0,
                                     skinned ? (const void*)gpuVertsSkinned.data()
                                             : (const void*)gpuVerts.data(),
                                     bd.size);
    }
    {
        uint64_t isz = indices.size() * sizeof(uint16_t);
        uint64_t padded = (isz + 3) & ~3ull;
        indices.resize(padded / sizeof(uint16_t), 0);
        wgpu::BufferDescriptor bd{};
        bd.label = "MeshIB"; bd.size = padded;
        bd.usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
        meshEntry.ibuf = rnd.mGpu.Device().CreateBuffer(&bd);
        sMeshBufCreatesThisFrame++;
        rnd.mGpu.Queue().WriteBuffer(meshEntry.ibuf, 0, indices.data(), padded);
    }
    meshEntry.indexCount = (uint32_t)(nf * 3);
    meshEntry.skinned    = skinned;
    meshEntry.ownerKey   = (const void*)owner;
    meshEntry.fpVerts    = fpVertsKey;
    meshEntry.fpFaces    = nf;
    meshEntry.fpSkinned  = skinned;
    meshEntry.uploaded   = true;
    return true;
}

// L2 GPU warm sweep — see Rnd_Wgpu_RB3.h. Walks `root` (incl. subdirs) via
// ObjDirItr, pushing each not-yet-resident RndTex through UploadRndTexIfNeeded and
// each RndMesh through RB3EnsureMeshGpu (same cache keys DrawMesh uses), spending
// at most budgetMs of wall time per call. Returns #uploaded; 0 == fully warm. The
// ObjDirItr<RndLight> per-frame hang noted elsewhere does not apply: this runs at
// most a few times during the (idle) loading dwell, not per draw.
int BandRnd::WarmGpuForDir(ObjectDir* root, float budgetMs) {
    if (!mGpuReady || !root) return 0;
    int uploaded = 0;
    double t0 = FrameTraceNowMs();
    // Textures first (so a later mesh's material bind-time view lookup hits the
    // same per-RndTex cache), then meshes. Count only REAL uploads (a tex not yet
    // resident) so the return value is a true "remaining work" signal and reaches 0
    // once `root` is fully warm — `uploaded==0` is the dwell driver's done test.
    for (ObjDirItr<RndTex> it(root, true); it != nullptr; ++it) {
        RndTex* tex = it;
        auto cached = sTexGpu.find(tex);
        bool wasResident = (cached != sTexGpu.end() && cached->second.uploaded);
        UploadRndTexIfNeeded(mGpu, tex);
        if (!wasResident) {
            auto now = sTexGpu.find(tex);
            if (now != sTexGpu.end() && now->second.uploaded) ++uploaded;
        }
        if (budgetMs > 0.f && (FrameTraceNowMs() - t0) >= budgetMs)
            return uploaded;
    }
    for (ObjDirItr<RndMesh> it(root, true); it != nullptr; ++it) {
        if (RB3EnsureMeshGpu(*this, it)) ++uploaded;
        if (budgetMs > 0.f && (FrameTraceNowMs() - t0) >= budgetMs)
            return uploaded;
    }
    return uploaded;
}

// ===========================================================================
// Real out-of-line bodies for the matched-fork HX_NATIVE virtuals that the
// link stubs currently weak-alias. A strong def here displaces the weak alias.
// ===========================================================================

void RndMesh::DrawShowing() {
    gBandRnd.DrawMesh(this);
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
}

// RndTex render-target entry points.
//
// MakeDrawTarget (BEGIN): the shared rndobj/Cam.cpp never calls this — the
// begin-side redirect lived in the per-platform Wii/Xenon RndCam this backend
// lacks — so BandRnd::DrawMesh hooks the begin lazily instead. Kept a no-op.
//
// FinishDrawTarget (END): fired by RndCam::SetTargetTex(nullptr)/Select() when
// the current cam's target tex is torn down (RndTexRenderer::DrawToTexture).
// Close the RT pass and re-open the main pass — but only if THIS tex is the
// one we redirected to (guards against a spurious end for an untracked target).
void RndTex::MakeDrawTarget() {}
void RndTex::FinishDrawTarget() {
    if (gBandRnd.mRtActiveTex == this) gBandRnd.EndDrawTarget();
}

// V1: SyncBitmap uploads the bitmap data to a GPU texture (CPU-decompresses
// DXT to RGBA8). The result is stashed in sTexGpu keyed by `this` and bound
// in MakeMaterialBindGroup as the diffuse slot when the material references
// this texture. PresyncBitmap delegates to SyncBitmap so either entry-point
// the matched-fork loader chooses (PreLoad vs PostLoad) does the right thing.
void RndTex::SyncBitmap() {
    if (!gBandRnd.mGpuReady) return;
    UploadRndTexIfNeeded(gBandRnd.mGpu, this);
}
void RndTex::PresyncBitmap() {
    if (!gBandRnd.mGpuReady) return;
    UploadRndTexIfNeeded(gBandRnd.mGpu, this);
}

// ===========================================================================
// Particle billboard renderer (A1 fix-B).
//
// On the RB3 BandRnd backend, RndParticleSys::DrawShowing (matched fork,
// HX_NATIVE) calls the free `DrawParticlesBillboard(RndParticleSys*)`, which
// was a weak no-op stub (rndobj_synth_link_stubs.s) — particles never drew.
// This file provides a STRONG def that forwards to BandRnd::DrawParticles,
// generating camera-facing quads per active particle. Mirrors the dc3
// Part_Wgpu.cpp geometry/shader and BandRnd::DrawRect's resource pattern.
//
// RB3 divergences from the dc3 reference:
//   - RB3's RndParticleSys has NO UV tiling (no NumTilesAcross/Down, no
//     per-particle mCurrentTileIndex), so each quad samples the full texture
//     (UV 0..1).
//   - Positions are stored in the system's RELATIVE frame; world pos =
//     mRelativeXfm * p->pos (Multiply(localPos, sys->RelativeXfm(), worldPos)).
//     Absolute systems keep mRelativeXfm == identity (venue particles
//     unaffected); relative-motion smasher FX need it or they cluster at the
//     world origin instead of the strike line.
// ===========================================================================

namespace {
struct RB3ParticleVertex {
    float pos[3];
    float uv[2];
    float color[4];
};

// WGSL: vs projects world→clip via the SHARED scene group-0 viewProj; fs
// samples tex × per-vertex color with an alpha discard. Group 0 reuses the
// main renderer's SceneLayout (5 bindings) so mSceneBindGroup binds directly —
// the vs only reads binding 0 (viewProj), the unused shadow/white entries are
// allowed by WebGPU bind-group/layout compatibility.
const char* kRB3ParticleShaderSource = R"WGSL(
struct SceneUB {
    viewProj: mat4x4f,
};
@group(0) @binding(0) var<uniform> scene: SceneUB;

@group(1) @binding(0) var particleTex: texture_2d<f32>;
@group(1) @binding(1) var particleSampler: sampler;

struct VIn {
    @location(0) pos: vec3f,
    @location(1) uv: vec2f,
    @location(2) color: vec4f,
};
struct VOut {
    @builtin(position) clip: vec4f,
    @location(0) uv: vec2f,
    @location(1) color: vec4f,
};

@vertex fn vs_particle(in: VIn) -> VOut {
    var out: VOut;
    out.clip = scene.viewProj * vec4f(in.pos, 1.0);
    out.uv = in.uv;
    out.color = in.color;
    return out;
}

@fragment fn fs_particle(in: VOut) -> @location(0) vec4f {
    let tex = textureSample(particleTex, particleSampler, in.uv);
    let c = tex * in.color;
    if (c.a < 0.004) { discard; }
    return c;
}
)WGSL";

uint64_t RB3PartPipeKey(wgpu::TextureFormat fmt, WgpuBlend blend, bool hasDepth) {
    return ((uint64_t)(uint32_t)fmt << 8) | ((uint64_t)(uint32_t)blend << 1) |
           (uint64_t)(hasDepth ? 1 : 0);
}
} // namespace

void BandRnd::EnsureParticlePipeline() {
    if (mPartReady) return;

    wgpu::ShaderSourceWGSL src;
    src.code = kRB3ParticleShaderSource;
    wgpu::ShaderModuleDescriptor smDesc{};
    smDesc.nextInChain = &src;
    mPartShader = mGpu.Device().CreateShaderModule(&smDesc);

    // Group 1: diffuse tex + sampler.
    wgpu::BindGroupLayoutEntry e1[2] = {};
    e1[0].binding = 0;
    e1[0].visibility = wgpu::ShaderStage::Fragment;
    e1[0].texture.sampleType = wgpu::TextureSampleType::Float;
    e1[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    e1[1].binding = 1;
    e1[1].visibility = wgpu::ShaderStage::Fragment;
    e1[1].sampler.type = wgpu::SamplerBindingType::Filtering;
    wgpu::BindGroupLayoutDescriptor bgl1Desc{};
    bgl1Desc.entryCount = 2;
    bgl1Desc.entries = e1;
    mPartTexBGL = mGpu.Device().CreateBindGroupLayout(&bgl1Desc);

    // Group 0 reuses the shared scene layout (mSceneBindGroup is compatible).
    wgpu::BindGroupLayout layouts[2] = { mPipelines.SceneLayout(), mPartTexBGL };
    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.bindGroupLayoutCount = 2;
    plDesc.bindGroupLayouts = layouts;
    mPartPL = mGpu.Device().CreatePipelineLayout(&plDesc);

    mPartReady = true;
}

void BandRnd::DrawParticles(RndParticleSys* sys) {
    if (!mGpuReady || !mInPass || !sys) return;

    RndParticle* head = sys->ActiveParticles();
    if (!head) return;
    RndMat* mat = sys->GetMat();
    if (!mat) return;

    // Camera axes for billboarding. Milo convention (per CAM_DBG in DrawMesh):
    // m.x = right, m.y = forward, m.z = up.
    RndCam* cam = RndCam::Current();
    if (!cam) return;
    const Transform& camXfm = cam->WorldXfm();
    float rx = camXfm.m.x.x, ry = camXfm.m.x.y, rz = camXfm.m.x.z;  // right
    float ux = camXfm.m.z.x, uy = camXfm.m.z.y, uz = camXfm.m.z.z;  // up

    const Transform& relXfm = sys->RelativeXfm();

    // Build the vertex stream (4 verts/particle: TL, BL, TR, BR).
    static std::vector<RB3ParticleVertex> sVerts;   // reused across calls
    sVerts.clear();
    sVerts.reserve(256);

    Vector3 worldPos;
    for (RndParticle* p = head; p; p = p->Next()) {
        // Relative-frame → world. Absolute systems keep relXfm == identity.
        Multiply(p->Pos3(), relXfm, worldPos);

        float halfSize = p->size * 0.5f;
        float srx = rx * halfSize, sry = ry * halfSize, srz = rz * halfSize;
        float sux = ux * halfSize, suy = uy * halfSize, suz = uz * halfSize;

        // Optional per-particle rotation about the view axis.
        if (p->angle != 0.0f) {
            float cosA = cosf(p->angle);
            float sinA = sinf(p->angle);
            float nrx = srx * cosA + sux * sinA;
            float nry = sry * cosA + suy * sinA;
            float nrz = srz * cosA + suz * sinA;
            float nux = -srx * sinA + sux * cosA;
            float nuy = -sry * sinA + suy * cosA;
            float nuz = -srz * sinA + suz * cosA;
            srx = nrx; sry = nry; srz = nrz;
            sux = nux; suy = nuy; suz = nuz;
        }

        float cx = worldPos.x, cy = worldPos.y, cz = worldPos.z;
        float cr = p->col.red, cg = p->col.green, cb = p->col.blue, ca = p->col.alpha;

        RB3ParticleVertex v;
        v.color[0] = cr; v.color[1] = cg; v.color[2] = cb; v.color[3] = ca;

        // TL: -right +up
        v.pos[0] = cx - srx + sux; v.pos[1] = cy - sry + suy; v.pos[2] = cz - srz + suz;
        v.uv[0] = 0.0f; v.uv[1] = 0.0f; sVerts.push_back(v);
        // BL: -right -up
        v.pos[0] = cx - srx - sux; v.pos[1] = cy - sry - suy; v.pos[2] = cz - srz - suz;
        v.uv[0] = 0.0f; v.uv[1] = 1.0f; sVerts.push_back(v);
        // TR: +right +up
        v.pos[0] = cx + srx + sux; v.pos[1] = cy + sry + suy; v.pos[2] = cz + srz + suz;
        v.uv[0] = 1.0f; v.uv[1] = 0.0f; sVerts.push_back(v);
        // BR: +right -up
        v.pos[0] = cx + srx - sux; v.pos[1] = cy + sry - suy; v.pos[2] = cz + srz - suz;
        v.uv[0] = 1.0f; v.uv[1] = 1.0f; sVerts.push_back(v);
    }

    int numParticles = (int)sVerts.size() / 4;
    if (numParticles == 0) return;

    EnsureParticlePipeline();

    // Grow the dynamic VB/IB on demand. IB indices are static per slot
    // (0,1,2, 2,1,3 per quad) so we only rewrite them when the buffer grows.
    int neededVerts = numParticles * 4;
    int neededIndices = numParticles * 6;
    if (neededVerts > mPartVBCapacity) {
        int cap = neededVerts < 256 ? 256 : neededVerts;
        wgpu::BufferDescriptor desc{};
        desc.size = (uint64_t)cap * sizeof(RB3ParticleVertex);
        desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        mPartVB = mGpu.Device().CreateBuffer(&desc);
        mPartVBCapacity = cap;
    }
    if (neededIndices > mPartIBCapacity) {
        int quads = (mPartVBCapacity / 4);
        int cap = quads * 6;
        std::vector<uint16_t> indices(cap);
        for (int i = 0; i < quads; i++) {
            int base = i * 4;
            int idx = i * 6;
            indices[idx + 0] = (uint16_t)(base + 0);
            indices[idx + 1] = (uint16_t)(base + 1);
            indices[idx + 2] = (uint16_t)(base + 2);
            indices[idx + 3] = (uint16_t)(base + 2);
            indices[idx + 4] = (uint16_t)(base + 1);
            indices[idx + 5] = (uint16_t)(base + 3);
        }
        wgpu::BufferDescriptor desc{};
        desc.size = (uint64_t)cap * sizeof(uint16_t);
        desc.usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
        mPartIB = mGpu.Device().CreateBuffer(&desc);
        mGpu.Queue().WriteBuffer(mPartIB, 0, indices.data(), cap * sizeof(uint16_t));
        mPartIBCapacity = cap;
    }
    if (!mPartVB || !mPartIB) return;

    mGpu.Queue().WriteBuffer(mPartVB, 0, sVerts.data(),
                             sVerts.size() * sizeof(RB3ParticleVertex));

    // Blend from the system material. Target format/depth per the ACTIVE pass
    // (RT pass: mRtFmt, no depth; main pass: mTargetFmt, D24S8 depth — read-only
    // LessEqual so particles z-test against the scene but don't write depth, so
    // overlapping additive sprites composite correctly).
    WgpuBlend blend = WgpuBlend::SrcAlpha;
    {
        int b = (int)mat->GetBlend();
        if (b >= 0 && b <= 10) blend = (WgpuBlend)b;
    }
    bool rtPass = (mRtActiveTex != nullptr);
    wgpu::TextureFormat fmt = rtPass ? mRtFmt : mTargetFmt;
    bool hasDepth = !rtPass;

    uint64_t pkey = RB3PartPipeKey(fmt, blend, hasDepth);
    wgpu::RenderPipeline pipe;
    {
        auto it = mPartPipelines.find(pkey);
        if (it != mPartPipelines.end()) {
            pipe = it->second;
        } else {
            wgpu::BlendState bs = mPipelines.MapBlend(blend);
            wgpu::ColorTargetState ct{};
            ct.format = fmt;
            ct.blend = &bs;
            ct.writeMask = wgpu::ColorWriteMask::All;

            wgpu::FragmentState frag{};
            frag.module = mPartShader;
            frag.entryPoint = "fs_particle";
            frag.targetCount = 1;
            frag.targets = &ct;

            wgpu::VertexAttribute attrs[3] = {};
            attrs[0].format = wgpu::VertexFormat::Float32x3; attrs[0].offset = 0;  attrs[0].shaderLocation = 0;
            attrs[1].format = wgpu::VertexFormat::Float32x2; attrs[1].offset = 12; attrs[1].shaderLocation = 1;
            attrs[2].format = wgpu::VertexFormat::Float32x4; attrs[2].offset = 20; attrs[2].shaderLocation = 2;
            wgpu::VertexBufferLayout vbl{};
            vbl.arrayStride = sizeof(RB3ParticleVertex);
            vbl.stepMode = wgpu::VertexStepMode::Vertex;
            vbl.attributeCount = 3;
            vbl.attributes = attrs;

            // Main pass has a D24S8 depth buffer; test (LessEqual) but never
            // write — particles are translucent and order-independent under
            // additive/alpha blend.
            wgpu::DepthStencilState ds{};
            ds.format = wgpu::TextureFormat::Depth24PlusStencil8;
            ds.depthWriteEnabled = wgpu::OptionalBool::False;
            ds.depthCompare = wgpu::CompareFunction::LessEqual;

            wgpu::RenderPipelineDescriptor pd{};
            pd.layout = mPartPL;
            pd.vertex.module = mPartShader;
            pd.vertex.entryPoint = "vs_particle";
            pd.vertex.bufferCount = 1;
            pd.vertex.buffers = &vbl;
            pd.fragment = &frag;
            pd.depthStencil = hasDepth ? &ds : nullptr;
            pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
            pd.primitive.cullMode = wgpu::CullMode::None;
            pd.multisample.count = 1;   // rb3 backend is single-sampled

            pipe = mGpu.Device().CreateRenderPipeline(&pd);
            mPartPipelines[pkey] = pipe;
        }
    }
    if (!pipe) return;

    // Diffuse tex (mWhiteView fallback) → group 1.
    wgpu::TextureView texView;
    RndTex* diffuse = mat->GetDiffuseTex();
    if (diffuse) {
        texView = GetRB3TexView(diffuse);
        if (!texView) texView = UploadRndTexIfNeeded(mGpu, diffuse);
    }
    if (!texView) texView = mWhiteView;

    wgpu::BindGroupEntry bge[2] = {};
    bge[0].binding = 0; bge[0].textureView = texView;
    bge[1].binding = 1; bge[1].sampler = mSampler;
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = mPartTexBGL;
    bgd.entryCount = 2;
    bgd.entries = bge;
    wgpu::BindGroup texBG = mGpu.Device().CreateBindGroup(&bgd);

    mPass.SetPipeline(pipe);
    mPass.SetBindGroup(0, mSceneBindGroup, 0, nullptr);
    mPass.SetBindGroup(1, texBG, 0, nullptr);
    mPass.SetVertexBuffer(0, mPartVB, 0, sVerts.size() * sizeof(RB3ParticleVertex));
    mPass.SetIndexBuffer(mPartIB, wgpu::IndexFormat::Uint16, 0,
                         (uint64_t)neededIndices * sizeof(uint16_t));
    mPass.DrawIndexed(neededIndices);

    // Restore the scene bind group at group 0 for the next DrawMesh (mirrors
    // DrawRect — though we already bound mSceneBindGroup at group 0 here, the
    // next DrawMesh re-binds anyway; explicit for symmetry/safety).
    mPass.SetBindGroup(0, mSceneBindGroup, 0, nullptr);
}

// Strong def displacing the weak no-op stub
// (_Z22DrawParticlesBillboardP14RndParticleSys in rndobj_synth_link_stubs.s).
// Called by RndParticleSys::DrawShowing (matched fork, HX_NATIVE).
void DrawParticlesBillboard(RndParticleSys* sys) {
    gBandRnd.DrawParticles(sys);
}
