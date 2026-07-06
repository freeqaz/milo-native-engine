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
#include "platform/RB3MeshCache.h"       // W1.2: RB3MeshEntry cache data + maps + invalidation
#include "platform/RB3MaterialBinder.h"  // W1.3: cross-TU tex-helper decls + material binder iface
#include "platform/RB3PostProc.h"        // W1.4: cross-TU RB3PostProcDisabled decl + postproc TU iface
#include "platform/RB3Quad.h"            // W1.4: cross-TU RB3RttDisabled decl + quad/DrawRect TU iface
#include "platform/FrameTraceCounters.h"
#include "platform/RB3TexSharpenDebug.h"
#include "platform/RB3TexSharpen.h"     // RB3SharpenReuploadTex / RB3SharpenTexFingerprint defs
#include "platform/GameRenderHook.h"    // W1.7: frame-pass hook dispatch (DrawGameOverlay / RenderCharacterImpostors)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
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
// (RB3PostProcDisabled is declared in platform/RB3PostProc.h — W1.4 de-static.)
static bool RB3PipelinePrewarmDisabled();
static bool RB3PipelinePrewarmNoChunk();
static int RB3PipelinePrewarmPerFrame();

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

// (BandUniformRing removed in W1.5 — the four rings now use the shared
//  gfx/UniformRingBuffer.h; overflow behavior unified to Grow. See W1.5.S2.)

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
    // Dimensions of the GPU texture last created for this entry. The churn path
    // (UploadRndTexIfNeeded) re-reads tex->mBitmap's W/H each call and recreates
    // `tex` at whatever the current bitmap size is — there is NO same-size
    // assumption — so when the progressive-sharpen manager swaps a stripped
    // bitmap up to full-res these grow accordingly. Recorded for the sharpen
    // recreate-at-new-size diagnostic (RB3DebugGetTexGpuInfo) and harmless
    // otherwise. (-1 == never created.)
    int               lastW = -1;
    int               lastH = -1;
    // RTT: when isRenderTarget, `tex`/`view` hold a RENDER_ATTACHMENT |
    // TEXTURE_BINDING texture that BandRnd paints into (instead of a
    // CPU-uploaded bitmap). The sky-dome material then samples `view` to read
    // the painted result. `uploaded` is set true once the RT texture exists so
    // GetRB3TexView/MakeMaterialBindGroup bind the painted view.
    bool              isRenderTarget = false;
};
static std::unordered_map<RndTex*, RB3TexEntry> sTexGpu;

// Monotonic count of GPU-texture (re)creations in UploadRndTexIfNeeded. Bumped
// on every CreateTexture there — first upload AND every churn-driven recreate
// (incl. the progressive-sharpen swap-to-full-res). Read by the sharpen
// recreate-at-new-size diagnostic to confirm a swap actually re-created the
// texture rather than silently reusing the old (smaller) one. Diagnostic-only.
static uint64_t sTexRecreateCount = 0;

// ===========================================================================
// Per-mesh GPU vertex/index buffer cache.
//
// MOVED (W1.2): struct RB3MeshEntry (+ nested UniformSlot), the sMeshGpu /
// sGeomSyncGen maps, LookupGeomSyncGen, CleanupGpuMesh, and RndMesh::OnSync now
// live in platform/RB3MeshCache.{h,cpp} (included above). Behavior-identical
// relocation — the maps went file-static -> external linkage (unique names,
// RB3-only TU). The per-frame CreateBuffer counters below stay DEFINED here
// (frame lifecycle owned by BeginFrame/EndFrame/DrawMesh); RB3MeshCache.h only
// declares them extern for the moved upload helper.
// ===========================================================================

// Per-frame GPU-resource CREATE counter — proves the leak is fixed. Incremented
// at every CreateBuffer in DrawMesh's upload path (and the per-mesh bind-group
// builds), reset in BeginFrame, logged in EndFrame under RENDER_DBG. At steady
// state (no new geometry entering the scene) this drops to ~0. External linkage
// (declared extern in RB3MeshCache.h) so the moved RB3EnsureMeshGpu can bump it.
int sMeshBufCreatesThisFrame = 0;
int sMeshBGCreatesThisFrame = 0;

// Monotonic frame sequence. Bumped once per BeginFrame; each mesh entry compares
// its `frameSeen` against this to lazily reset its per-frame uniform-slot index
// (RB3MeshEntry::nextSlot) the first time it is drawn each frame — no map-wide
// sweep needed. (Distinct from BandRnd::mFrameCount, which only advances on
// EndDrawing and is also used by screenshot scheduling; a dedicated global keeps
// the slot logic independent of that.)
static uint64_t sFrameSeq = 0;

// CleanupGpuMesh MOVED (W1.2) to platform/RB3MeshCache.cpp (co-located with the
// sMeshGpu/sGeomSyncGen maps it erases). Declared in platform/RB3MeshCache.h.

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
wgpu::TextureView UploadRndTexIfNeeded(GpuDevice& gpu, RndTex* tex) {
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
            e.lastW = w; e.lastH = h;
            sTexRecreateCount++;
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
    e.lastW = w; e.lastH = h;
    sTexRecreateCount++;
    if (gFrameTraceActive) {
        gTexUploadMsThisFrame += (float)(FrameTraceNowMs() - ftStart);
        gTexUploadCountThisFrame++;
    }
    return e.view;
}

// ===========================================================================
// Progressive-texture-sharpen recreate-at-new-size diagnostic (research/13 T0).
//
// The whole progressive-sharpen design rests on one engine assumption: swapping
// a stripped (half-res) RndBitmap up to full-res + dirtying the churn key makes
// UploadRndTexIfNeeded RECREATE the GPU texture at the NEW (larger) size and
// publish a NEW view — with NO same-size assert. These native-only helpers let a
// test drive that path directly (RB3DebugUploadTex) and inspect what happened
// (RB3DebugGetTexGpuInfo) without exposing sTexGpu. Diagnostic surface only; not
// referenced by any production draw path.
// ===========================================================================
bool RB3DebugUploadTex(RndTex* tex) {
    if (!tex) return false;
    // Exact production path: ResolveMaterialViews / WarmGpuForDir both reach the
    // GPU texture through UploadRndTexIfNeeded with the engine's GpuDevice.
    wgpu::TextureView v = UploadRndTexIfNeeded(gBandRnd.Gpu(), tex);
    return v != nullptr;
}

RB3TexGpuInfo RB3DebugGetTexGpuInfo(RndTex* tex) {
    RB3TexGpuInfo info{};
    info.globalRecreateCount = sTexRecreateCount;
    auto it = sTexGpu.find(tex);
    if (it != sTexGpu.end()) {
        const RB3TexEntry& e = it->second;
        info.present = true;
        info.uploaded = e.uploaded;
        info.texW = e.lastW;
        info.texH = e.lastH;
        info.viewPtr = (const void*)e.view.Get();
        info.texPtr = (const void*)e.tex.Get();
    }
    return info;
}

// ===========================================================================
// Progressive-texture-sharpen PRODUCTION helpers (research/13 T1).
//
// The sharpen manager (RB3TexSharpen.cpp, a native-only TU) owns the state
// machine: parse the .sharpen sidecar, match its entries to the venue's loaded
// RndTex objects by a recomputed TexFingerprint, swap each matched RndBitmap up
// to full-res, then RE-INVOKE the upload so UploadRndTexIfNeeded recreates the
// GPU texture at the new (larger) size and publishes a new view. These two
// helpers are the manager's ONLY contact with this TU's private sTexGpu cache +
// the upload path:
//
//   RB3SharpenTexFingerprint — recompute the SAME TexFingerprint the upload path
//     keys on, over the tex's CURRENT live pixels. This is the robust match key:
//     the sidecar stores the fingerprint of the STRIPPED base (mip[levels]); the
//     loaded stripped bitmap's pixels reproduce it byte-for-byte, so a sidecar
//     entry is matched to its RndTex purely by value (no name/order assumption).
//
//   RB3SharpenReuploadTex — drive the production UploadRndTexIfNeeded for `tex`
//     (the same call ResolveMaterialViews / WarmGpuForDir make). After the manager
//     swaps mPixels/W/H, both churn keys (pixel pointer + fingerprint) differ, so
//     this is a cache MISS → recreate at the new size → NEW e.view. The cached
//     DrawMesh material bind group then rebuilds AUTOMATICALLY: its existing key
//     `slot.matDiffuseView != diffuse.Get()` sees the new view handle that
//     ResolveMaterialViews now returns (GetRB3TexView yields the fresh e.view).
//     No new matBG invalidation key is required — the view-handle compare IS the
//     invalidation (proven by test_texsharpen + the gate write-up). The OLD
//     wgpu::Texture/View are refcounted-released when e.tex/e.view are overwritten
//     in UploadRndTexIfNeeded, so no use-after-free: a matBG still binding the old
//     view keeps it alive until that matBG is itself rebuilt on the next draw.
//
// Returns the recreate? — true iff this call recreated (cache-miss) vs hit cache.
// The manager uses that to charge real work to its per-frame budget.
uint32_t RB3SharpenTexFingerprint(const RndTex* tex) {
    if (!tex) return 0;
    const RndBitmap& bmp = tex->mBitmap;
    const uint8_t* pixels = bmp.Pixels();
    int pixBytes = bmp.PixelBytes();
    if (!pixels || pixBytes < 16) return 0;
    return TexFingerprint(pixels, pixBytes);
}

bool RB3SharpenReuploadTex(RndTex* tex) {
    if (!tex || !gBandRnd.mGpuReady) return false;
    uint64_t before = sTexRecreateCount;
    wgpu::TextureView v = UploadRndTexIfNeeded(gBandRnd.Gpu(), tex);
    (void)v;
    return sTexRecreateCount != before; // true == a recreate happened
}

// Public accessor — used by MakeMaterialBindGroup to bind a material's
// diffuse texture. Returns an empty view if not yet uploaded.
wgpu::TextureView GetRB3TexView(RndTex* tex) {
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
    mHaloBlendBuf = nullptr;
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

// render-polish 2026-06-11 (menu-contrast, wave-5 "Fix 3"): the venue-light
// heuristic's floor lighting (the ambient floor, the near-white-ambient clamp,
// and the no-light grey key) was tuned conservatively-bright so nothing crushed
// to black — but on the menu hub it lifts the *unlit-by-design* (ue=1) brick /
// sidewalk / band-outfit geometry to a flat grey, killing the contrast vs retail
// (loop-wide 3x3 contrast ~2.6:1 vs retail ~10:1; dark cells 0.16 vs 0.035).
// Pulling these floors DOWN lets the rich authored point lights (lamppole/road/
// theater spots, colors 1-3.0 with quadratic range falloff) carry the
// illumination while the far-from-light zones go dark — exactly retail's dark
// backdrop + bright-neon-hotspot look. The point/neon emissive (mUseEnviron==0
// unlit mats + emissive maps) is UNAFFECTED — it bypasses this lighting path
// entirely (register colour / self-illum), so the signs/marquee still pop.
// Tunable for A/B; defaults are the tuned values. Applies to the venue path
// (world.cam, RB3_VENUE_LIGHT on) only, so game.cam (highway) is byte-identical.
static float sVenueEnvFloat(const char* env, float def) {
    const char* e = getenv(env);
    if (!e || !e[0]) return def;
    float v = (float)atof(e);
    return (v >= 0.f) ? v : def;
}
static float sVenueAmbientFloor() { static float v = sVenueEnvFloat("RB3_VENUE_AMBIENT_FLOOR", 0.008f); return v; }
static float sVenueAmbientClamp() { static float v = sVenueEnvFloat("RB3_VENUE_AMBIENT_CLAMP", 0.09f);  return v; }
static float sVenueGreyKey()      { static float v = sVenueEnvFloat("RB3_VENUE_GREY_KEY",      0.22f);  return v; }

// C8 face-shading fix (2026-07-02, impl-c8-shading). On Wii, RndEnviron splits
// its lights: mLightsReal drive GX directional/point HARDWARE shading, while
// mLightsApprox are the "fake" lights folded through BoxMapLighting into GX
// AMBIENT (Env.cpp: IsFake() == in mLightsApprox; UpdateApproxLighting feeds
// them to ApplyApproxLighting as an ambient box). Native's WriteSceneUniforms
// historically promoted mLightsApprox to full Lambert directionals — fine for
// the venue-geometry environs (the converge-2026-06-20 backdrop tuning is built
// on it), but WRONG for CHARACTER environs (chars.env / char.env / *_char.env):
// there the approx set is rim.lit + the four white *_silhouette.lit spots, so
// band flesh gets a flat frontal white flood (over-bright + shadowless) instead
// of the dim, single, distance-attenuated warm key (main.lit, the front-of-house
// real light) the Wii shades faces with. GT (Dolphin) faces are dim + rim-lit
// with a real shadow side. Fix: for character environs that actually carry a
// usable real key, shade from mLightsReal and demote mLightsApprox to a modest
// ambient fill; character environs with an EMPTY real list, and every non-char
// environ, keep the legacy approx-promotion untouched (so the venue backdrop and
// the arena tight-spot band key from converge STEP 1 do not regress). RB3-only
// TU → DC3 byte-identical; world.cam venue path only → game.cam/menu untouched.
// Default-ON, full clean revert via RB3_CHAR_REAL_LIGHT_OFF=1 (no rebuild).
static bool sCharRealLight() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("RB3_CHAR_REAL_LIGHT_OFF"); v = (e && e[0] && e[0] != '0') ? 0 : 1; }
    return v != 0;
}
// Coefficient folding the demoted mLightsApprox (rim/silhouette) into ambient as
// an AVERAGE (venue-stable — not a per-light sum that re-flattens), and the
// per-channel clamp on the resulting char ambient. Tuned against the Dolphin GT
// face oracle: keep faces dim so the real key carries the directional shading.
static float sCharApproxAmbient() { static float v = sVenueEnvFloat("RB3_CHAR_APPROX_AMBIENT", 0.11f); return v; }
static float sCharAmbientMax()    { static float v = sVenueEnvFloat("RB3_CHAR_AMBIENT_MAX",    0.14f); return v; }

// render-polish 2026-06-19 (lighting-polish wrap-up, sub-item 2 "venue song-start
// exposure"): the native lit sum (ambient + Sum point/dir diffuse) runs HOTTER than
// the Wii GX backdrop on disco-lit venues (small_club), so the song-start lighting
// reveal reads as a flat over-bright PINK field (RB3_PP_OFF=1 raw lit-path: mlum
// 0.667, dark-cell 0.42 — nothing dark) that the wave-4 softClipLighting + wave-5
// fs_postproc clip only BOUND, not cure. On GX the rasterised channel color is
// clamped to [0,1] BEFORE the TEV texture multiply, so stage lights can only TINT
// a surface; our native path sums up to 4 point + 4 dir authored lights (raw color
// components reach 3.0, e.g. lamppole.lit (3.0,0.14,0)) into the lit term unbounded.
// Scale the LIGHT contribution (point + dir colors + the no-light grey key) down
// toward the GX-clamped look so the soft-clip/postproc BACKSTOP instead of doing the
// work. Ambient is NOT scaled (already floored low); this only tames the bright
// stage-light pile-up. The scale is applied BEFORE the existing per-channel clamp so
// a raw-3.0 light still clamps but a moderate light scales proportionally. The same
// lever darkens the menu-hub point-light mid-bleed (raises contrast — the floor
// lever is exhausted, the residual was bright-side) and softens the endgame disco
// peak (green and pink alike) for free. world.cam venue path ONLY → game.cam highway
// byte-identical. 1.0 = old behavior (clean full revert, no rebuild).
static float sVenuePointExposure() { static float v = sVenueEnvFloat("RB3_VENUE_POINT_EXPOSURE", 0.70f); return v; }
static float sVenueDirExposure()   { static float v = sVenueEnvFloat("RB3_VENUE_DIR_EXPOSURE",   0.80f); return v; }

// converge-2026-06-20 lighting STEP 1 (GAP 2, arena_02 band near-black): the
// shader's legacy point-light falloff is saturate(1 - d/range)^2 — a HARD cutoff
// that is exactly 0 at d>=range. arena_02 authors per-station white key spots
// (*_silhouette.lit, type=0 point, range=55) that sit 70-103u from the band roots
// → the squared curve extinguishes the ONLY key and the band falls to near-black.
// The Wii GX ground truth (rndwii/Lit.cpp:36-44, GXInitLightAttn k0=1,k1=1/range,
// k2=0) is the inverse-linear law 1/(1 + d/range): 0.5 at d==range, long tail, so
// the same spots deliver a real white key 70-100u away → spotlit-dim band, not
// black. Selected per-draw via SceneUniforms.pointFalloffMode (default 0 = the
// exact legacy curve; 1 = the GX law). We set mode 1 ONLY on the world.cam venue
// path here, so DC3 + game.cam (highway) + menu cams + every non-venue draw stay
// byte-identical (mode 0). DEFAULT-ON for RB3's venue path; full clean revert via
// RB3_VENUE_POINT_FALLOFF_LEGACY=1 (no rebuild). Directionals + large-range points
// are essentially unchanged near d<<range, so directional-lit venues (festival,
// clubs) don't move; the change restores only the tight-spot band key (arena).
static bool sVenuePointFalloffGx() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("RB3_VENUE_POINT_FALLOFF_LEGACY"); v = (e && e[0] && e[0] != '0') ? 0 : 1; }
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
        // Aspect + far plane: a cam rendering into an off-screen render target
        // (clouds RTT, the 2D bowl-imposter crowd) is authored to the TARGET's
        // aspect, not the window's. Using the window aspect (~1.78 16:9) for a
        // square (1.0) RT cam throws the projected geometry outside the RT's NDC →
        // empty target. Derive the aspect from the cam's TargetTex when present
        // (= RndCam::SetFrustum's kAspect for the imposter cam, Crowd.cpp), else
        // the window aspect for the normal on-screen scene cam. This also corrects
        // the clouds RTT projection.
        float aspect;
        RndTex* rtAspectTex = cam->TargetTex();
        if (rtAspectTex && rtAspectTex->Width() > 0 && rtAspectTex->Height() > 0) {
            aspect = (float)rtAspectTex->Width() / (float)rtAspectTex->Height();
            // The 2D bowl-imposter crowd re-poses each archetype char to the WORLD
            // ORIGIN and views it from ~dist away (Crowd.cpp), but SetFrustum
            // inherits the venue world.cam FAR plane (~224). The char sits at
            // camera-local depth ~600-1600 ≫ far → every triangle lands behind the
            // z=1 far clip plane and is clip-volume-culled (a BLACK RT, even with
            // no depth attachment) — the same failure documented for scrolled
            // highway gems below. Widen the RT cam's far plane so the standoff char
            // is inside the clip volume. RT-cam-scoped → main pass + clouds RT (its
            // own far already encompasses the sky dome) are unchanged.
            if (f < 8000.0f) f = 8000.0f;
        } else {
            aspect = (float)mGpu.WindowWidth() / (float)mGpu.WindowHeight();
        }

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
        // STEP 1 (GAP 2): use the GX-faithful inverse-linear point falloff for the
        // venue path so tight range-55 silhouette spots reach the band (default 0 =
        // legacy squared-cutoff everywhere else; DC3 + game.cam never set this).
        s.pointFalloffMode = sVenuePointFalloffGx() ? 1.0f : 0.0f;
        // Ambient: a near-white ambient is the engine's degenerate default (not an
        // authored flood), so pull it down — the point/dir lights provide the real
        // illumination and a low ambient keeps the dark-venue contrast. Floor so
        // nothing crushes to pure black.
        const Hmx::Color& amb = venv->AmbientColor();
        float ar = amb.red, ag = amb.green, ab = amb.blue;
        // Near-white ambient is the engine's degenerate default (e.g. env='' that
        // scopes the band outfits at ambRaw=1,1,1), not an authored flood — clamp
        // it down hard so it doesn't grey-wash the walking band / props.
        const float clamp = sVenueAmbientClamp();
        if (std::max(ar, std::max(ag, ab)) > 0.85f) { ar *= clamp; ag *= clamp; ab *= clamp; }
        // Floor so nothing crushes to pure black, but low enough that the
        // far-from-light venue geometry reaches retail's deep blacks.
        const float floor = sVenueAmbientFloor();
        s.ambientColor[0] = std::max(ar, floor);
        s.ambientColor[1] = std::max(ag, floor);
        s.ambientColor[2] = std::max(ab, floor);
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
        // C8 face-shading: choose the direct-shading light list. Character
        // environs (chars.env / char.env / *_char.env) with a usable real key
        // shade from mLightsReal (Wii's GX hardware lights) so band faces get the
        // dim, single, distance-attenuated key (main.lit) instead of the flat
        // white *_silhouette flood from the approx set; the approx lights are then
        // demoted to a modest ambient fill below. Every other environ — and any
        // char environ whose real list is empty — keeps the legacy approx
        // promotion, leaving the venue backdrop tuning + arena spot-key untouched.
        bool useReal = false;
        if (sCharRealLight() && std::strstr(envNm, "char") != nullptr) {
            for (ObjPtrList<RndLight>::iterator rit = venv->mLightsReal.begin();
                 rit != venv->mLightsReal.end(); ++rit) {
                RndLight* RL = *rit;
                if (!RL || !RL->mColorOwner || !RL->Showing()) continue;
                const Hmx::Color& rc = RL->GetColor();
                if (rc.red + rc.green + rc.blue > 0.01f) { useReal = true; break; }
            }
        }
        ObjPtrList<RndLight>& litList = useReal ? venv->mLightsReal : venv->mLightsApprox;
        if (probe) fprintf(stderr, "[CHAR_REAL] env=%s isChar=%d useReal=%d\n",
                           envNm, (int)(std::strstr(envNm, "char") != nullptr), (int)useReal);
        int dl = 0, pl = 0;
        for (ObjPtrList<RndLight>::iterator it = litList.begin();
             it != litList.end() && (dl < 4 || pl < 4); ++it) {
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
                const float de = sVenueDirExposure();
                s.lightColors[dl][0] = std::min(lc.red * de, 1.5f);
                s.lightColors[dl][1] = std::min(lc.green * de, 1.5f);
                s.lightColors[dl][2] = std::min(lc.blue * de, 1.5f);
                s.lightColors[dl][3] = 1.0f;
                dl++;
            } else if (ty == 0 && pl < 4) {
                const Vector3& p = L->WorldXfm().v;                // point light WORLD POSITION
                s.pointLightPos[pl][0] = p.x; s.pointLightPos[pl][1] = p.y; s.pointLightPos[pl][2] = p.z; s.pointLightPos[pl][3] = 0;
                const float pe = sVenuePointExposure();
                s.pointLightColors[pl][0] = std::min(lc.red * pe, 1.8f);
                s.pointLightColors[pl][1] = std::min(lc.green * pe, 1.8f);
                s.pointLightColors[pl][2] = std::min(lc.blue * pe, 1.8f);
                s.pointLightColors[pl][3] = 1.0f;
                s.pointLightRanges[pl] = L->Range() > 0.f ? L->Range() : 100.f;
                pl++;
            }
        }
        if (useReal) {
            // Demote the character env's approx set (rim + *_silhouette spots) to
            // a modest ambient FILL so the shadow side of faces reads without a
            // frontal flood. Average (not sum) keeps this venue-stable — summing N
            // white spots would just re-flatten the face. Clamped low so the real
            // key (main.lit) carries the directional shading and faces stay
            // GT-dim. Folds on top of the already-clamped env ambient set above.
            float fr = 0, fg = 0, fb = 0; int fn = 0;
            for (ObjPtrList<RndLight>::iterator ait = venv->mLightsApprox.begin();
                 ait != venv->mLightsApprox.end(); ++ait) {
                RndLight* AL = *ait;
                if (!AL || !AL->mColorOwner || !AL->Showing()) continue;
                const Hmx::Color& ac = AL->GetColor();
                if (ac.red + ac.green + ac.blue <= 0.01f) continue;
                fr += ac.red; fg += ac.green; fb += ac.blue; fn++;
            }
            if (fn > 0) {
                const float k = sCharApproxAmbient() / (float)fn;   // average
                const float mx = sCharAmbientMax();
                s.ambientColor[0] = std::min(s.ambientColor[0] + fr * k, mx);
                s.ambientColor[1] = std::min(s.ambientColor[1] + fg * k, mx);
                s.ambientColor[2] = std::min(s.ambientColor[2] + fb * k, mx);
            }
        }
        if (dl == 0 && pl == 0) {
            // Env has NO real lights (ambient-only, e.g. sky.env) — soft default
            // key so geometry still has form. NOT added when the env has point
            // lights (e.g. theater.env's coloured stage spots), else a grey key
            // would wash the authored colour out.
            s.lightDirs[0][0] = -0.4f; s.lightDirs[0][1] = -0.5f; s.lightDirs[0][2] = -0.75f; s.lightDirs[0][3] = 0;
            // Dim the ambient-only-env grey key in lockstep with the dir exposure so
            // the no-light fallback (sky/back_left/road) is darkened too — helps the
            // menu-hub contrast (sub-item 1) for free.
            const float grey = sVenueGreyKey() * sVenueDirExposure();
            s.lightColors[0][0] = grey; s.lightColors[0][1] = grey; s.lightColors[0][2] = grey; s.lightColors[0][3] = 1.0f;
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
        static bool sPrewarmDbg = getenv("RB3_PREWARM_DBG") != nullptr;
        double wall0 = (sPrewarmDbg || gFrameTraceActive) ? FrameTraceNowMs() : 0.0;
        if (RB3PipelinePrewarmNoChunk()) {
            // Legacy one-shot: all ~240 compiles in this frame (the Track-C spike).
            mPipelinesPrewarmed = true;
            int created = mPipelines.PreWarm(mTargetFmt, mRtFmt);
            if (gFrameTraceActive) {
                gPipelineCreateMsThisFrame += (float)(FrameTraceNowMs() - wall0);
                gPipelineCreateCountThisFrame += created;
            }
            if (sPrewarmDbg)
                fprintf(stderr, "[A5] pipeline pre-warm (one-shot): created %d in "
                        "%.1f ms (mainFmt=%d rtFmt=%d)\n",
                        created, FrameTraceNowMs() - wall0,
                        (int)mTargetFmt, (int)mRtFmt);
        } else {
            // Track-C: create a small per-frame COUNT of pipelines, spread across
            // frames so each end-of-frame Dawn flush compiles only this chunk.
            int before = mPipelines.CachedPipelineCount();
            int remaining = mPipelines.PreWarmStep(
                mTargetFmt, mRtFmt, RB3PipelinePrewarmPerFrame());
            int created = mPipelines.CachedPipelineCount() - before;
            if (gFrameTraceActive) {
                gPipelineCreateMsThisFrame += (float)(FrameTraceNowMs() - wall0);
                gPipelineCreateCountThisFrame += created;
            }
            if (remaining <= 0) {
                mPipelinesPrewarmed = true;  // fully warm — stop stepping
                if (sPrewarmDbg)
                    fprintf(stderr, "[A5] pipeline pre-warm (chunked): complete, "
                            "%d cached (mainFmt=%d rtFmt=%d)\n",
                            mPipelines.CachedPipelineCount(),
                            (int)mTargetFmt, (int)mRtFmt);
            } else if (sPrewarmDbg && created > 0) {
                fprintf(stderr, "[A5] pipeline pre-warm (chunked): +%d this frame "
                        "in %.1f ms, %d remaining\n",
                        created, FrameTraceNowMs() - wall0, remaining);
            }
        }
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
    // W0.3 per-draw state-log ring: drop last frame's records. Stays empty (no
    // allocation) unless RB3_DRAWLOG recording is active; reserve(512) is done
    // lazily on first push in RecordDrawLog.
    mDrawLog.clear();

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

    // W1.7: game-supplied off-screen render passes (RB3's analog of DC3's
    // per-HamCharacter impostor RTTs). Dispatched here — encoder open, right
    // after the pre-clear RTT passes — mirroring DC3 Rnd_Wgpu.cpp, which calls
    // RenderCharacterImpostors after DrawPreClear() and before the main frame
    // pass. RB3's BeginFrame opens the main pass earlier (before DrawPreClear),
    // so the hook runs with the main pass already active; the impl is
    // responsible for opening/closing its own RTT passes (suspend/resume the
    // main pass) exactly as DC3's does. No-op today (BandRenderHook has no
    // impostor loop wired on native), so output is byte-identical.
    if (GameRenderHook* hook = GetGameRenderHook()) {
        hook->RenderCharacterImpostors(this);
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

    // W1.7: game-supplied HUD/overlay draw pass (RB3's analog of DC3's
    // HamDirector overlay). Dispatched here — after the venue/post-proc + halo
    // composites, encoder still open, before Finish() — mirroring DC3
    // Rnd_Wgpu.cpp, which calls DrawGameOverlay once the post-processed venue is
    // resolved into the framebuffer. Unlike DC3, RB3 draws its HUD/track panel
    // inline in the main pass and does NOT open a dedicated 1x no-depth overlay
    // pass here; if a future RB3 overlay needs one, the impl opens its own pass
    // via the renderer API (as RenderCharacterImpostors does for RTTs). No-op
    // today (BandRenderHook issues no overlay), so output is byte-identical.
    if (GameRenderHook* hook = GetGameRenderHook()) {
        hook->DrawGameOverlay(this);
    }

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

    // W0.3 per-draw state-log ring: dump this frame's captured records to JSON.
    // Guarded by DrawLogOn() so it is a no-op (one branch) when RB3_DRAWLOG is
    // off. DumpDrawLog itself no-ops when no dump path is configured (the debug
    // setter can enable recording for gtests without wanting a file written).
    if (DrawLogOn())
        DumpDrawLog();
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

// Xbox 360 compressed vertex unpacking (XboxCVert/Be*/RB3UnpackMeshVerts)
// MOVED (W1.2) to platform/RB3MeshCache.cpp. XboxCVert + the Be* decode
// helpers stay file-static there (no external consumer); RB3UnpackMeshVerts
// is declared in platform/RB3MeshCache.h (included above).

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
bool RB3RttDisabled() {
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
    // Force the next DrawMesh to RE-RESOLVE and re-write the scene uniforms for the
    // camera that draws into this RT. Without this, the DrawMesh camChanged check
    // (pointer + pose) can MISS the RT cam when it equals mLastSceneCam (e.g. the
    // 2D bowl-imposter crowd re-Selects the SAME gImpostorCamera object for every
    // archetype, and its re-posed v/forward can coincide with mLastSceneCamPose) —
    // so the geometry would project against a STALE (main scene cam) view/proj and
    // land outside the RT (an empty target). Mirrors EndDrawTarget's reset for the
    // reverse direction; harmless for the clouds RTT (whose cam already differs).
    mLastSceneCam = nullptr;
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

// Track-C fix: spread the pre-warm pipeline-compile burst across frames instead of
// one ~590 ms (web) / ~700 ms (native) blocking frame that guarantees an audio
// under-run. Default ON (chunked); RB3_PIPELINE_PREWARM_NOCHUNK=1 restores the
// legacy one-shot for A/B. RB3_PIPELINE_PREWARM_PER_FRAME sets how many pipelines
// are created per frame (default 12 → the 240-key set warms over ~20 frames /
// ~0.33 s of the idle splash dwell, each frame's Dawn flush carrying only ~12
// compiles instead of all 240). COUNT not time: CreateRenderPipeline is async
// over the Dawn wire, so the real GPU-process compile happens at the per-rAF
// flush, not in the wasm call — a wall-time budget can't see it, but bounding the
// per-flush pipeline COUNT bounds the resulting GPUTask.
static bool RB3PipelinePrewarmNoChunk() {
    static int s = -1;
    if (s < 0) {
        const char* e = getenv("RB3_PIPELINE_PREWARM_NOCHUNK");
        s = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return s != 0;
}
static int RB3PipelinePrewarmPerFrame() {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("RB3_PIPELINE_PREWARM_PER_FRAME");
        v = (e && e[0]) ? atoi(e) : 12;
        if (v < 1) v = 1;
    }
    return v;
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
    // RB3_HEADMAT_DBG (C8 head-invisible triage, temporary): name EVERY mesh
    // that reaches DrawMesh with empty geometry, once per instance.
    if (nf <= 0) {
        if (getenv("RB3_HEADMAT_DBG")) {
            static std::unordered_map<const void*, int> sSeen;
            if (sSeen[(const void*)mesh]++ == 0) {
                Hmx::Object* dirObj = mesh->Dir();
                fprintf(stderr, "[HEADMAT] EMPTY mesh='%s' mesh=%p owner=%p dir='%s' nf=%d nv=%d\n",
                        mesh->Name() ? mesh->Name() : "?", (void*)mesh, (void*)owner,
                        (dirObj && dirObj->Name()) ? dirObj->Name() : "-", nf, nv);
            }
        }
        return;
    }

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

    // HUB_BAR_PROBE: one-shot diagnostic for the focused-menu highlight bar
    // (Defect 2) — confirms it is a skinned mesh and reports its (correct) mesh
    // WorldXfm vs the (origin) bone palette. Render-inert, env-gated.
    if (getenv("HUB_BAR_PROBE") && mesh->Name() &&
        (strstr(mesh->Name(), "highlight_main") ||
         strstr(mesh->Name(), "highlight_pattern"))) {
        static std::unordered_map<std::string,int> sHBd;
        std::string key = mesh->Name();
        if (sHBd[key]++ % 240 == 0)
            fprintf(stderr, "[HUB_BAR_DRAW] mesh='%s' skinned=%d numBones(owner)=%d "
                "meshWorld.v=(%.2f,%.2f,%.2f) showing=%d\n",
                mesh->Name(), (int)skinned, owner?owner->NumBones():-1,
                mesh->WorldXfm().v.x, mesh->WorldXfm().v.y, mesh->WorldXfm().v.z,
                (int)mesh->Showing());
    }

    // DIAG: skip-skinned / skip-static draw bisection.
    if (skinned && getenv("RB3_SKIP_SKINNED")) return;
    if (!skinned && getenv("RB3_SKIP_STATIC")) return;
    // DIAG: RB3_ISOLATE_MESH=<substr> — draw ONLY meshes whose name contains
    // <substr>, so every pixel in a capture is attributable to that mesh
    // (slab/artifact attribution that survives camera-loop misalignment).
    {
        static const char* sIso = getenv("RB3_ISOLATE_MESH");
        if (sIso && sIso[0]) {
            const char* nm = mesh->Name() ? mesh->Name() : "";
            if (!std::strstr(nm, sIso)) return;
        }
    }

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
    // Owner-generation re-upload: a geom-owner proxy (owner != mesh) shares geometry
    // with a never-drawn owner. The owner fires OnSync(owner) when its verts mutate,
    // which can't touch THIS proxy's `uploaded` flag, and the count fingerprint can't
    // see same-count position changes (the saturated sustain-tail case). Compare the
    // proxy's stamped owner-gen against the owner's live gen so those changes force a
    // re-upload. Self-owned meshes (owner == mesh) already invalidate via their own
    // OnSync entry, so we skip the lookup for them (no behavior change there).
    bool ownerGenStale = (owner != mesh) &&
                         (meshEntry.fpOwnerGen != LookupGeomSyncGen(owner));
    bool needUpload = sMeshCacheOff || !meshEntry.uploaded ||
                      meshEntry.ownerKey != (const void*)owner ||
                      meshEntry.fpVerts != fpVertsKey || meshEntry.fpFaces != nf ||
                      meshEntry.fpSkinned != skinned || ownerGenStale;

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
        if (nv < 0) {
            if (getenv("RB3_HEADMAT_DBG") && mesh->Name()
                && std::strcmp(mesh->Name(), "head.mesh") == 0) {
                static std::unordered_map<const void*, int> sSeen;
                if (sSeen[(const void*)mesh]++ == 0)
                    fprintf(stderr, "[HEADMAT] mesh='head.mesh' mesh=%p EARLY-OUT unpack nv<0\n",
                            (void*)mesh);
            }
            return; // no geometry
        }

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

    // MESH_DUMP=<substr>: one-shot decoded-geometry diagnostic for any mesh whose
    // name contains <substr>. Dumps vert/face counts, index range + OOB/degenerate
    // face counts, position bounds + NaN/Inf census, and the top triangles by
    // local-space area (with their indices + decoded positions). Built to diagnose
    // the neon_arcade "green slab" class of decode bug; reads the freshly-unpacked
    // verts, so only fires on a cache miss (the first draw always is).
    static std::unordered_map<std::string, std::array<float,6>> sMdBounds; // name -> local bbox
    static const char* sMeshDumpEnv = getenv("MESH_DUMP");
    {
        const char* md = sMeshDumpEnv;
        const char* mdn = mesh->Name() ? mesh->Name() : "?";
        if (md && md[0] && std::strstr(mdn, md)) {
            // Throttled screen-footprint line (every draw is too chatty): project
            // the cached local bbox corners through WorldXfm + the current cam,
            // print the NDC rectangle the mesh covers. Catches "mesh fills the
            // frame in shot N" with the exact camera pose.
            auto bit = sMdBounds.find(mdn);
            if (bit != sMdBounds.end() && RndCam::sCurrent) {
                static std::unordered_map<std::string, int> sMdTick;
                if ((sMdTick[mdn]++ % 7) == 0) {
                    RndCam* cur = RndCam::sCurrent;
                    const Transform& cw = cur->WorldXfm();
                    const Transform& mw = mesh->WorldXfm();
                    const std::array<float,6>& bb = bit->second;
                    float yfov = cur->YFov() > 0.0001f ? cur->YFov() : 0.9f;
                    float aspect = (float)mGpu.WindowWidth() / (float)mGpu.WindowHeight();
                    float th = tanf(yfov * 0.5f);
                    float nxMin = 1e9f, nxMax = -1e9f, nyMin = 1e9f, nyMax = -1e9f;
                    float dMin = 1e9f, dMax = -1e9f; int behind = 0;
                    for (int ci = 0; ci < 8; ci++) {
                        float lx = (ci & 1) ? bb[3] : bb[0];
                        float ly = (ci & 2) ? bb[4] : bb[1];
                        float lz = (ci & 4) ? bb[5] : bb[2];
                        // world = M * local + v
                        float wxp = mw.m.x.x*lx + mw.m.y.x*ly + mw.m.z.x*lz + mw.v.x;
                        float wyp = mw.m.x.y*lx + mw.m.y.y*ly + mw.m.z.y*lz + mw.v.y;
                        float wzp = mw.m.x.z*lx + mw.m.y.z*ly + mw.m.z.z*lz + mw.v.z;
                        float dx = wxp - cw.v.x, dy = wyp - cw.v.y, dz = wzp - cw.v.z;
                        float rx = cw.m.x.x*dx + cw.m.x.y*dy + cw.m.x.z*dz;
                        float fy = cw.m.y.x*dx + cw.m.y.y*dy + cw.m.y.z*dz;
                        float uz = cw.m.z.x*dx + cw.m.z.y*dy + cw.m.z.z*dz;
                        if (fy < dMin) dMin = fy; if (fy > dMax) dMax = fy;
                        if (fy <= 0.001f) { behind++; continue; }
                        float nx = (rx/(th*aspect))/fy, ny = (uz/th)/fy;
                        if (nx < nxMin) nxMin = nx; if (nx > nxMax) nxMax = nx;
                        if (ny < nyMin) nyMin = ny; if (ny > nyMax) nyMax = ny;
                    }
                    fprintf(stderr,
                        "[MESH_FOOT] f=%d mesh='%s'@%p owner=%p cam='%s' camPos(%.1f,%.1f,%.1f) depth[%.1f,%.1f] "
                        "behind=%d ndcX[%.2f,%.2f] ndcY[%.2f,%.2f] rtt='%s' lastSceneCam='%s' sceneOff=%u\n",
                        mFrameCount, mdn, (void*)mesh, (void*)owner, cur->Name() ? cur->Name() : "?",
                        cw.v.x, cw.v.y, cw.v.z, dMin, dMax, behind,
                        nxMin, nxMax, nyMin, nyMax,
                        mRtActiveTex ? (mRtActiveTex->Name() ? mRtActiveTex->Name() : "?") : "none",
                        (mLastSceneCam && mLastSceneCam->Name()) ? mLastSceneCam->Name() : "?",
                        mSceneOffset);
                }
            }
        }
    }
    if (!skipUnpack) {
        const char* md = sMeshDumpEnv;
        const char* mdn = mesh->Name() ? mesh->Name() : "?";
        if (md && md[0] && std::strstr(mdn, md)) {
            static std::unordered_map<std::string, int> sMdSeen;
            if (sMdSeen[mdn]++ == 0) {
                const float* posOf;
                auto posAt = [&](int i) -> const float* {
                    return skinned ? skinnedView[i].pos : gpuVerts[i].pos;
                };
                (void)posOf;
                int nanCt = 0, infCt = 0;
                float mn3[3] = {1e30f,1e30f,1e30f}, mx3[3] = {-1e30f,-1e30f,-1e30f};
                for (int i = 0; i < nv; i++) {
                    const float* p = posAt(i);
                    for (int k = 0; k < 3; k++) {
                        if (std::isnan(p[k])) nanCt++;
                        else if (std::isinf(p[k])) infCt++;
                        else { if (p[k] < mn3[k]) mn3[k] = p[k]; if (p[k] > mx3[k]) mx3[k] = p[k]; }
                    }
                }
                int oobFaces = 0, degenFaces = 0; unsigned maxIdx = 0, minIdx = 0xFFFFFFFFu;
                for (int i = 0; i < nf; i++) {
                    unsigned a = faces[i].v1, b = faces[i].v2, c = faces[i].v3;
                    if (a > maxIdx) maxIdx = a; if (b > maxIdx) maxIdx = b; if (c > maxIdx) maxIdx = c;
                    if (a < minIdx) minIdx = a; if (b < minIdx) minIdx = b; if (c < minIdx) minIdx = c;
                    if (a >= (unsigned)nv || b >= (unsigned)nv || c >= (unsigned)nv) oobFaces++;
                    if (a == b || b == c || a == c) degenFaces++;
                }
                // top-8 triangles by local-space area
                struct TriA { float area; int f; };
                TriA top[8]; int topN = 0; double totalArea = 0;
                for (int i = 0; i < nf; i++) {
                    unsigned a = faces[i].v1, b = faces[i].v2, c = faces[i].v3;
                    if (a >= (unsigned)nv || b >= (unsigned)nv || c >= (unsigned)nv) continue;
                    const float *pa = posAt((int)a), *pb = posAt((int)b), *pc = posAt((int)c);
                    float u[3] = {pb[0]-pa[0], pb[1]-pa[1], pb[2]-pa[2]};
                    float w[3] = {pc[0]-pa[0], pc[1]-pa[1], pc[2]-pa[2]};
                    float cx = u[1]*w[2]-u[2]*w[1], cy = u[2]*w[0]-u[0]*w[2], cz = u[0]*w[1]-u[1]*w[0];
                    float area = 0.5f * std::sqrt(cx*cx + cy*cy + cz*cz);
                    if (std::isnan(area) || std::isinf(area)) continue;
                    totalArea += area;
                    if (topN < 8) { top[topN].area = area; top[topN].f = i; topN++; }
                    else {
                        int mi = 0;
                        for (int k = 1; k < 8; k++) if (top[k].area < top[mi].area) mi = k;
                        if (area > top[mi].area) { top[mi].area = area; top[mi].f = i; }
                    }
                }
                fprintf(stderr,
                    "[MESH_DUMP] mesh='%s' owner='%s' skinned=%d nv=%d nf=%d src=%s\n"
                    "[MESH_DUMP]   idx min=%u max=%u oobFaces=%d degenFaces=%d nan=%d inf=%d\n"
                    "[MESH_DUMP]   posBounds min(%.2f,%.2f,%.2f) max(%.2f,%.2f,%.2f) totalArea=%.1f\n",
                    mdn, owner->Name() ? owner->Name() : "?", (int)skinned, nv, nf,
                    (owner->mVerts.size() > 0) ? "mVerts" : "compressed",
                    minIdx, maxIdx, oobFaces, degenFaces, nanCt, infCt,
                    mn3[0],mn3[1],mn3[2], mx3[0],mx3[1],mx3[2], totalArea);
                for (int k = 0; k < topN; k++) {
                    // selection-sort print order: largest first
                    int bi = k;
                    for (int j = k+1; j < topN; j++) if (top[j].area > top[bi].area) bi = j;
                    TriA t = top[bi]; top[bi] = top[k]; top[k] = t;
                    unsigned a = faces[top[k].f].v1, b = faces[top[k].f].v2, c = faces[top[k].f].v3;
                    const float *pa = posAt((int)a), *pb = posAt((int)b), *pc = posAt((int)c);
                    fprintf(stderr,
                        "[MESH_DUMP]   tri f=%d area=%.1f idx(%u,%u,%u) A(%.2f,%.2f,%.2f) B(%.2f,%.2f,%.2f) C(%.2f,%.2f,%.2f)\n",
                        top[k].f, top[k].area, a, b, c,
                        pa[0],pa[1],pa[2], pb[0],pb[1],pb[2], pc[0],pc[1],pc[2]);
                }
                const Transform& mdwx = mesh->WorldXfm();
                fprintf(stderr,
                    "[MESH_DUMP]   worldXfm x(%.2f,%.2f,%.2f) y(%.2f,%.2f,%.2f) z(%.2f,%.2f,%.2f) v(%.1f,%.1f,%.1f)\n",
                    mdwx.m.x.x,mdwx.m.x.y,mdwx.m.x.z, mdwx.m.y.x,mdwx.m.y.y,mdwx.m.y.z,
                    mdwx.m.z.x,mdwx.m.z.y,mdwx.m.z.z, mdwx.v.x,mdwx.v.y,mdwx.v.z);
                sMdBounds[mdn] = {mn3[0],mn3[1],mn3[2],mx3[0],mx3[1],mx3[2]};
            }
        }
    }

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
    meshEntry.fpOwnerGen = LookupGeomSyncGen(owner);
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
    if (!vbuf || !ibuf) {
        if (getenv("RB3_HEADMAT_DBG") && mesh->Name()
            && std::strcmp(mesh->Name(), "head.mesh") == 0) {
            static std::unordered_map<const void*, int> sSeen;
            if (sSeen[(const void*)mesh]++ == 0)
                fprintf(stderr, "[HEADMAT] mesh='head.mesh' mesh=%p EARLY-OUT vbuf=%d ibuf=%d\n",
                        (void*)mesh, (int)!!vbuf, (int)!!ibuf);
        }
        return;  // upload failed / no geometry
    }

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
    // Per-draw geometric / draw-guard policy (W1.7 B1–B5). The RB3 asset-name
    // branches that used to decide these placements/guards inline are relocated
    // to the game side (rb3/native/src/rb3_render_hook.cpp, BandRenderHook::
    // QueryDrawGeomPolicy + the skel/shard name classifiers). The ENGINE keeps
    // ALL matrix / palette math below unchanged; the hook returns only the
    // DECISION and owns the RB3_* opt-out flag reads, so every relocation stays
    // byte-identical. Fetched once per DrawMesh (defaults to "no override" when
    // no hook is registered). `outWorld16` is unused today — the engine keeps the
    // hub-bar matrix math itself so no float ordering crosses the seam.
    DrawGeomPolicy geomPolicy;
    GameRenderHook* geomHook = GetGameRenderHook();
    if (geomHook)
        geomPolicy = geomHook->QueryDrawGeomPolicy(mesh, nullptr);
    // HUB MENU HIGHLIGHT BAR placement fix (Defect 2 of the hub-highlight pair; see
    // docs/native/render-polish-2026-06-11/task-hub-bar-placement-impl.md).
    //
    // The focused-menu-item yellow bar (`highlight_main.mesh` / `highlight_pattern
    // .mesh`) is a SKINNED UI mesh whose 4 corner bones carry the bar quad as their
    // LOCAL transforms (UILabel::UpdateAndDrawHighlightMesh -> botleft/topright->
    // SetLocalPos(...)), with the per-focus world PLACEMENT set on the mesh via
    // mLabelDir->SetWorldXfm(WorldXfm()) at UILabel.cpp:334. On native, those corner
    // bones' transform PARENT (`pentatonic_display`) stays at the ORIGIN — the
    // SetWorldXfm lands on the mesh/labelDir but NOT on the bones' parent chain — so
    // the skinned bone palette (BoneOffsetAt * boneWorld) places the bar at the
    // ORIGIN. The skinned path normally forces obj.world=identity (correct for
    // CHARACTER skeletons whose bones already hold WORLD coords), so the bar renders
    // at screen-CENTRE instead of behind the focused item.
    //
    // The palette already orients + sizes the bar correctly (isolated, the bar is a
    // clean full-height rounded rect — only its POSITION is wrong). The missing
    // piece is purely the label TRANSLATION. So for these named UI bar meshes,
    // inject the mesh WorldXfm's TRANSLATION into obj.world (rotation kept identity:
    // the FULL meshWorld would double-apply the model->world rotation the palette
    // already encoded and SKEW the bar). Matches retail: the bar sits behind the
    // focused hub item and tracks DUP/DDOWN. Same SPECIFIC mesh-name scope as the
    // Defect-1 colour fix, so the overshell choose-difficulty highlight
    // (`highlight.mesh`) and gameplay/HUD meshes are untouched.
    // Opt-out: RB3_NO_HUB_BAR_PLACEMENT_FIX (now read inside the hook). The
    // name match + flag decision is relocated; the engine keeps the `skinned`
    // gate here so the value is byte-identical to the prior inline computation.
    bool hubBarPlacement = skinned && geomPolicy.hubBarPlacement;
    // SCROLLBAR THUMB placement fix (Bug 1b, ui-bugs wave 2). Same family as the
    // hub highlight bar above: the scrollbar's red thumb (`scrollbar.mesh`) is a
    // SKINNED UI mesh in the SHARED ui/resource/scrollbar_display.milo dir. Every
    // ScrollbarDisplay component draws that ONE shared dir after
    // pDir->SetWorldXfm(WorldXfm()) (ScrollbarDisplay::DrawShowing). The static
    // sibling `scrollbar_bg.mesh` (the track) follows the freshly-set dir world to
    // the list's right edge (world ~168), but the thumb's bones do NOT re-inherit
    // that world (their local sub-tree is not re-dirtied by the parent SetWorldXfm),
    // so the skinned palette (BoneOffset * boneWorld) places the thumb at the
    // ORIGIN → it renders at screen centre (~x=640) instead of on the track.
    //
    // The bg track draws immediately BEFORE the thumb in the same pDir->Draw()
    // (mDraws order), and the bg MESH's own WorldXfm carries the full correct
    // placement (translation + the authored x-scale). So we STASH the bg mesh's
    // WorldXfm and, when the thumb draws, use it as the thumb's obj.world instead
    // of identity. The thumb bones already encode the thumb's position/size WITHIN
    // the track (origin-relative, incl. the live scroll offset via their local z),
    // so worldPos = bgPlacement * boneMatrix * vertex lands the thumb on the track
    // and tracks scrolling. Scoped to the two named scrollbar meshes — characters,
    // the hub bar, and all other skinned meshes are untouched. Wii path is
    // unaffected (native-only draw path). Opt-out RB3_SCROLLBAR_THUMB_FIX_OFF=1.
    // The bg-track world cache + the `skinned && have` guard stay in the engine
    // (state/math); the name match (`scrollbar_bg.mesh`/`scrollbar.mesh`) and the
    // RB3_SCROLLBAR_THUMB_FIX_OFF flag are relocated to the hook
    // (geomPolicy.scrollbarBg / .scrollbarThumb). Mutually exclusive by name, so
    // the prior `else if` is preserved by testing scrollbarBg then scrollbarThumb.
    static Transform sScrollbarPlacement; // last bg-track world (shared 1-widget)
    static bool sHaveScrollbarPlacement = false;
    if (geomPolicy.scrollbarBg) {
        sScrollbarPlacement = mesh->WorldXfm();
        sHaveScrollbarPlacement = true;
    }
    bool scrollbarThumb = geomPolicy.scrollbarThumb && skinned && sHaveScrollbarPlacement;
    if (skinned && scrollbarThumb) {
        MiloXfmToColMajor(sScrollbarPlacement, obj.world);
    } else if (skinned && hubBarPlacement) {
        // identity rotation + label translation (column-major; translation in [12..14])
        for (int i = 0; i < 16; i++) obj.world[i] = (i % 5 == 0) ? 1.f : 0.f;
        const Vector3& mwv = mesh->WorldXfm().v;
        obj.world[12] = mwv.x; obj.world[13] = mwv.y; obj.world[14] = mwv.z;
    } else if (skinned) {
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
        // IK diagnosis (render-polish wave-4): allow delaying the one-shot to a min
        // frame so we capture the leg chain AFTER animation flings it (frame ~121+).
        static int sBoneProbeMinFrame = -2;
        if (sBoneProbeMinFrame == -2) { const char* e = getenv("BONE_PROBE_MINFRAME");
            sBoneProbeMinFrame = e ? atoi(e) : -1; }
        bool doBoneProbe = getenv("BONE_PROBE") && !sBoneProbeDone &&
                           nameMatch && owner->NumBones() >= 8 && mesh->Name() &&
                           (sBoneProbeMinFrame < 0 || (int)mFrameCount >= sBoneProbeMinFrame);
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
            // W1.7 B3: the RB3_NO_SKEL_REBAKE flag + the face/hair/fingernail mesh-name
            // exclusion are relocated to the hook: geomPolicy.skelRebakeMesh ==
            // (rebake enabled) && !(dynamic mesh name). The band-skeleton dir test and
            // the per-bone dynamic-chain exclusion are asked of the hook below, keeping
            // all rebake math (Invert/Multiply/SetBone) in the engine.
            if (geomPolicy.skelRebakeMesh && numBones >= 8 && !mesh->mNativeBonesRebound &&
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
                // W1.7 B3: the skeleton_unshared.milo dir-name test is relocated to
                // the hook (IsBandMemberSkeletonFile); the engine keeps the worst-bone
                // selection + the stored-file null/empty guards.
                bool bandStatic = wdir && !wdir->mStoredFile.empty() && geomHook &&
                    geomHook->IsBandMemberSkeletonFile(wdir->mStoredFile.c_str());
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
                        // W1.7 B3: the dynamic-chain bone-name test is relocated to
                        // the hook (IsRebakeDynamicBone); the engine keeps the loop.
                        const char* bn = bt->Name();
                        if (bn && geomHook && geomHook->IsRebakeDynamicBone(bn))
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
        // render-polish wave-5 (pose-fling) — STALE-WORLDXFM-CACHE FIX.
        //
        // ROOT CAUSE (rb3 docs/native/render-polish-2026-06-11/task-pose-fling-impl.md):
        // a band member's per-member skeleton LEAF bones (ankle/toe/finger) carry a
        // STALE mWorldXfm cache. The bone LOCAL transforms are correct (orthonormal,
        // sane bone-offset lengths) and the parent (knee/thigh/pelvis) worlds are
        // correct, but the leaf's cached world was composed against an EARLIER flung
        // intermediate pose and never re-read after the parent was corrected by a later
        // pose pass — so RndTransformable::WorldXfm() returns the stale value (dirty bit
        // already cleared). A leaf ankle then reads world Z=-33 (below floor) off a knee
        // that is itself at Z=+25, an impossible >2x AABB jump that the V24 shard guard
        // (below) correctly refuses to draw -> legwear/footwear/fingernails/gloves
        // guard-dropped (the "pose fling"). PROVEN: a forced top-down WorldXfm_Force of
        // the leg chain snaps the ankle from Z=-33 to the correct Z=+4 (CHAIN_FORCE
        // probe). This is NATIVE-specific: it does not reproduce on Wii (single 32-bit
        // pose pass per frame); on native the band skeleton is re-posed across the
        // reload-re-entrant + IK passes and a WorldXfm() read between them caches a
        // pre-final intermediate.
        //
        // FIX: before reading the bone palette, force a fresh top-down WorldXfm recompute
        // of every bone this mesh references, by walking each bone's TransParent chain to
        // the root and forcing root->leaf (WorldXfm_Force composes against the parent's
        // already-forced world). Idempotent + cheap (short shared chains; visited-set
        // dedups across bones of the same mesh). Skinned-meshes only, so crowd/extras
        // skeletons get the same correctness pass — but they were already coherent (their
        // single pose pass leaves no stale leaf), so it is a no-op for them. Opt-out
        // RB3_NO_SKEL_WORLDFIX=1.
        {
            static int sWorldFixOff = -1;
            if (sWorldFixOff < 0) sWorldFixOff = getenv("RB3_NO_SKEL_WORLDFIX") ? 1 : 0;
            if (!sWorldFixOff) {
                static std::unordered_set<RndTransformable*> sForced;
                sForced.clear();
                RndTransformable* chain[64];
                for (int b = 0; b < numBones; b++) {
                    RndTransformable* bt = owner->BoneTransAt(b);
                    if (!bt) continue;
                    // collect leaf->root, stop at an already-forced node (its ancestors
                    // are forced too)
                    int nc = 0;
                    for (RndTransformable* n = bt; n && nc < 64; n = n->TransParent()) {
                        if (sForced.count(n)) break;
                        chain[nc++] = n;
                    }
                    // force root->leaf so each composes against a fresh parent world
                    for (int s = nc - 1; s >= 0; s--) {
                        chain[s]->DirtyLocalXfm();   // mark dirty (re-arm the cache)
                        chain[s]->WorldXfm_Force();  // recompute against fresh parent
                        sForced.insert(chain[s]);
                    }
                }
            }
        }
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
            // HUB_BAR_PROBE (per-bone): the hub highlight bar's corner bones carry
            // the bar quad as their LOCAL xfm but their TransParent (pentatonic_
            // display) stays at the ORIGIN — so boneWorld + composed skin land near
            // origin, not at the focused label (whose world is only on the mesh
            // WorldXfm). This is why the placement fix injects the mesh translation
            // into obj.world. Render-inert, env-gated.
            if (getenv("HUB_BAR_PROBE") && mesh->Name() &&
                (strstr(mesh->Name(), "highlight_main") ||
                 strstr(mesh->Name(), "highlight_pattern"))) {
                static std::unordered_map<std::string,int> sHB;
                std::string key = std::string(mesh->Name()) + "@b" + std::to_string(b);
                if (sHB[key]++ % 240 == 0) {
                    RndTransformable* par = bt->TransParent();
                    fprintf(stderr,
                        "[HUB_BAR] mesh='%s' b=%d bone='%s' parent='%s'(w=%.1f,%.1f,%.1f) "
                        "boneWorld.v=(%.1f,%.1f,%.1f) skin.v=(%.1f,%.1f,%.1f)\n",
                        mesh->Name(), b, bt->Name()?bt->Name():"?",
                        par&&par->Name()?par->Name():"-",
                        par?par->WorldXfm().v.x:0, par?par->WorldXfm().v.y:0,
                        par?par->WorldXfm().v.z:0,
                        wt.v.x, wt.v.y, wt.v.z, skin.v.x, skin.v.y, skin.v.z);
                }
            }
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
        // === C8 rotation-basis deep-dive probe (scout-c8, render-polish 2026-06-11) ===
        // C8_PROBE=<substr|*> selects skinned meshes by name; samples every
        // C8_EVERY frames (default 45) per mesh. Discriminator outputs:
        //   [C8_MESH]  header: dir, geomOwner, src format, rebound flags.
        //   [C8_SLOT]  per bone slot: name/ptr/dir, world.v, |dWorld| since last
        //              sample (MOVING vs STATIC), skin.v, bindPos =
        //              inverse(BoneOffsetAt(b)).v, skinDet, world rot row0.
        //   [C8_VERT]  bone-locality audit: per sampled vert, dist(bind vert pos,
        //              bindPos[dominant bone]). Authored data is bone-local
        //              (small); large = vert indexes the WRONG bone.
        //   [C8_SMEAR] worst blended-world outlier vert vs blended centroid, with
        //              its 4 (idx,weight) pairs + each slot's skin.v.
        {
            static const char* sC8 = getenv("C8_PROBE");
            static int sC8Every = -1;
            if (sC8Every < 0) {
                const char* e = getenv("C8_EVERY");
                sC8Every = (e && atoi(e) > 0) ? atoi(e) : 45;
            }
            const char* c8mn = mesh->Name() ? mesh->Name() : "";
            // C8_PROBE accepts a comma-separated substring list (or '*').
            bool c8match = false;
            if (sC8 && sC8[0]) {
                if (sC8[0] == '*') c8match = true;
                else {
                    const char* p = sC8;
                    while (*p && !c8match) {
                        const char* q = strchr(p, ',');
                        std::string tok(p, q ? (size_t)(q - p) : strlen(p));
                        if (!tok.empty() && strstr(c8mn, tok.c_str())) c8match = true;
                        p = q ? q + 1 : p + strlen(p);
                    }
                }
            }
            if (c8match && numBones >= 2 && !skinnedView.empty()) {
                static std::unordered_map<const void*, int> sCnt;
                static std::unordered_map<const void*, std::vector<float> > sLastW;
                int tick = sCnt[(const void*)mesh]++;
                if (tick % sC8Every == 0) {
                    int sampleNo = tick / sC8Every;
                    // bind world origin per slot = inverse(offset).v
                    std::vector<float> bindP((size_t)numBones * 3, 0.f);
                    for (int b = 0; b < numBones; b++) {
                        Transform inv; Invert(owner->BoneOffsetAt(b), inv);
                        bindP[b*3+0] = inv.v.x; bindP[b*3+1] = inv.v.y; bindP[b*3+2] = inv.v.z;
                    }
                    std::vector<float>& lastW = sLastW[(const void*)mesh];
                    bool haveLast = (int)lastW.size() == numBones * 3;
                    if (!haveLast) lastW.resize((size_t)numBones * 3, 0.f);
                    RndMesh* go2 = mesh->GeomOwner();
                    Hmx::Object* mdirObj = mesh->Dir();
                    fprintf(stderr,
                        "[C8_MESH] f=%d s=%d mesh='%s' dir='%s' owner='%s' nb=%d nv=%d "
                        "src=%s rebound=%d/%d\n",
                        mFrameCount, sampleNo, c8mn,
                        (mdirObj && mdirObj->Name()) ? mdirObj->Name() : "-",
                        (go2 && go2 != mesh && go2->Name()) ? go2->Name() : "self",
                        numBones, (int)skinnedView.size(),
                        (owner->mVerts.size() > 0) ? "verts" : "compressed",
                        (int)mesh->mNativeBonesRebound,
                        (int)(owner ? owner->mNativeBonesRebound : 0));
                    int nMoving = 0, nStatic = 0, nNull = 0;
                    bool slotTable = (sampleNo <= 2) || (sampleNo % 10 == 0);
                    for (int b = 0; b < numBones; b++) {
                        RndTransformable* bt = owner->BoneTransAt(b);
                        if (!bt) { nNull++; continue; }
                        const Transform& w = bt->WorldXfm();
                        float dW = -1.f;
                        if (haveLast) {
                            float dx = w.v.x - lastW[b*3+0], dy = w.v.y - lastW[b*3+1],
                                  dz = w.v.z - lastW[b*3+2];
                            dW = sqrtf(dx*dx + dy*dy + dz*dz);
                            if (dW > 0.01f) nMoving++; else nStatic++;
                        }
                        const float* cm = bones.bones[b]; // col-major skin
                        if (slotTable) {
                            ObjectDir* bdir = bt->Dir();
                            Transform sk; Multiply(owner->BoneOffsetAt(b), w, sk);
                            float sd = sk.m.x.x*(sk.m.y.y*sk.m.z.z - sk.m.y.z*sk.m.z.y)
                                     - sk.m.x.y*(sk.m.y.x*sk.m.z.z - sk.m.y.z*sk.m.z.x)
                                     + sk.m.x.z*(sk.m.y.x*sk.m.z.y - sk.m.y.y*sk.m.z.x);
                            // instance attribution: walk TransParent to the root —
                            // identifies WHICH character/rig instance owns the bone.
                            RndTransformable* root = bt;
                            int guard = 0;
                            while (root->TransParent() && guard++ < 64)
                                root = root->TransParent();
                            fprintf(stderr,
                                "[C8_SLOT] f=%d mesh='%s' b=%d '%s' ptr=%p bdir='%s' "
                                "bfile='%s' root='%s'@%p "
                                "w=(%.1f,%.1f,%.1f) dW=%.2f%s skin.v=(%.1f,%.1f,%.1f) "
                                "bind=(%.1f,%.1f,%.1f) skinDet=%.3f wrow0=(%.2f,%.2f,%.2f)\n",
                                mFrameCount, c8mn, b, bt->Name() ? bt->Name() : "?", (void*)bt,
                                (bdir && bdir->Name()) ? bdir->Name() : "-",
                                bdir ? bdir->mStoredFile.c_str() : "-",
                                (root && root->Name()) ? root->Name() : "?", (void*)root,
                                w.v.x, w.v.y, w.v.z, dW,
                                (dW < 0.f ? "(first)" : (dW > 0.01f ? " MOVING" : " STATIC")),
                                cm[12], cm[13], cm[14],
                                bindP[b*3+0], bindP[b*3+1], bindP[b*3+2], sd,
                                w.m.x.x, w.m.x.y, w.m.x.z);
                            // CHAIN_PROBE (render-polish wave-5 pose-fling diagnosis):
                            // when set to a bone-name substring, walk THIS bone's
                            // TransParent chain from leaf -> root and dump each link's
                            // LOCAL transform translation magnitude + rotation det +
                            // world translation. A faithful skeleton has small, ~constant
                            // bone-LOCAL .v (bind offsets, e.g. femur ~17u). The fling
                            // ancestor is the link whose LOCAL .v (or .v.y in particular)
                            // is huge or whose det != 1 / row lengths != 1. Render-inert.
                            const char* chainSel = getenv("CHAIN_PROBE");
                            if (chainSel && chainSel[0] && bt->Name() &&
                                strstr(bt->Name(), chainSel)) {
                                // CHAIN_FORCE: dirty + recompute the whole chain top-down
                                // before sampling. If the flung world snaps to the correct
                                // (manualW) value, the bug is a STALE WorldXfm cache (dirty
                                // bit not set / not propagated), NOT a bad local pose.
                                if (getenv("CHAIN_FORCE")) {
                                    // collect chain leaf->root
                                    RndTransformable* stack[40]; int ns=0;
                                    for (RndTransformable* n=bt; n && ns<40; n=n->TransParent())
                                        stack[ns++]=n;
                                    // force-recompute root->leaf
                                    for (int s=ns-1; s>=0; s--) {
                                        stack[s]->DirtyLocalXfm(); // mark dirty
                                        stack[s]->WorldXfm_Force();
                                    }
                                }
                                // CHAIN_PROPTEST: dirty the PARENT (knee) and check whether
                                // the child (ankle) becomes dirty. If the ankle stays clean,
                                // the parent->child dirty-cache linkage is SEVERED (the bug).
                                if (getenv("CHAIN_PROPTEST")) {
                                    RndTransformable* leaf = bt;            // ankle
                                    RndTransformable* par = leaf->TransParent(); // knee
                                    if (par) {
                                        bool leafBefore = leaf->Dirty();
                                        par->DirtyLocalXfm();   // dirty the knee (propagates)
                                        bool leafAfter = leaf->Dirty();
                                        fprintf(stderr,
                                            "[CHAIN_PROPTEST] leaf='%s' par='%s' leafDirtyBefore=%d "
                                            "leafDirtyAfterParentDirty=%d %s\n",
                                            leaf->Name()?leaf->Name():"?",
                                            par->Name()?par->Name():"?",
                                            (int)leafBefore, (int)leafAfter,
                                            leafAfter ? "(propagation OK)" : "*** LINKAGE SEVERED ***");
                                    }
                                }
                                RndTransformable* node = bt;
                                int g2 = 0;
                                while (node && g2++ < 40) {
                                    // capture dirty BEFORE any WorldXfm() (which clears it)
                                    bool wasDirty = node->Dirty();
                                    const Transform& L = node->LocalXfm();
                                    const Transform& W = node->WorldXfm();
                                    float ldet = L.m.x.x*(L.m.y.y*L.m.z.z - L.m.y.z*L.m.z.y)
                                               - L.m.x.y*(L.m.y.x*L.m.z.z - L.m.y.z*L.m.z.x)
                                               + L.m.x.z*(L.m.y.x*L.m.z.y - L.m.y.y*L.m.z.x);
                                    float rx = sqrtf(L.m.x.x*L.m.x.x+L.m.x.y*L.m.x.y+L.m.x.z*L.m.x.z);
                                    float ry = sqrtf(L.m.y.x*L.m.y.x+L.m.y.y*L.m.y.y+L.m.y.z*L.m.y.z);
                                    float rz = sqrtf(L.m.z.x*L.m.z.x+L.m.z.y*L.m.z.y+L.m.z.z*L.m.z.z);
                                    float lvmag = sqrtf(L.v.x*L.v.x+L.v.y*L.v.y+L.v.z*L.v.z);
                                    fprintf(stderr,
                                        "[CHAIN] f=%d g=%d '%s' Lv=(%.2f,%.2f,%.2f)|%.1f| "
                                        "Ldet=%.3f Lrow=(%.2f,%.2f,%.2f) Wv=(%.1f,%.1f,%.1f)\n",
                                        mFrameCount, g2, node->Name()?node->Name():"?",
                                        L.v.x, L.v.y, L.v.z, lvmag, ldet, rx, ry, rz,
                                        W.v.x, W.v.y, W.v.z);
                                    if (getenv("CHAIN_MTX"))
                                        fprintf(stderr,
                                            "[CHAIN_MTX]   Lm.x=(%.3f,%.3f,%.3f) Lm.y=(%.3f,%.3f,%.3f) "
                                            "Lm.z=(%.3f,%.3f,%.3f)\n",
                                            L.m.x.x, L.m.x.y, L.m.x.z, L.m.y.x, L.m.y.y, L.m.y.z,
                                            L.m.z.x, L.m.z.y, L.m.z.z);
                                    // CHAIN_COMPOSE: manually compose parent.world o this.local
                                    // and compare to this.WorldXfm(). A mismatch proves a stale
                                    // cache / wrong-parent / unexpected-constraint bug. Also dump
                                    // TransConstraint(). Render-inert.
                                    if (getenv("CHAIN_COMPOSE")) {
                                        RndTransformable* par = node->TransParent();
                                        int con = (int)node->TransConstraint();
                                        fprintf(stderr, "[CHAIN_DIRTY]   '%s' wasDirtyPreRead=%d\n",
                                                node->Name()?node->Name():"?", (int)wasDirty);
                                        if (par) {
                                            Transform comp;
                                            Multiply(L, par->WorldXfm(), comp);
                                            float dvx = comp.v.x - W.v.x, dvy = comp.v.y - W.v.y,
                                                  dvz = comp.v.z - W.v.z;
                                            float dvmag = sqrtf(dvx*dvx+dvy*dvy+dvz*dvz);
                                            fprintf(stderr,
                                                "[CHAIN_COMPOSE]   con=%d parW=(%.1f,%.1f,%.1f) "
                                                "manualW=(%.1f,%.1f,%.1f) cacheW=(%.1f,%.1f,%.1f) "
                                                "dMag=%.2f%s\n",
                                                con, par->WorldXfm().v.x, par->WorldXfm().v.y,
                                                par->WorldXfm().v.z, comp.v.x, comp.v.y, comp.v.z,
                                                W.v.x, W.v.y, W.v.z, dvmag,
                                                dvmag > 1.0f ? " *** CACHE MISMATCH ***" : "");
                                        } else {
                                            fprintf(stderr, "[CHAIN_COMPOSE]   con=%d (root)\n", con);
                                        }
                                    }
                                    node = node->TransParent();
                                }
                            }
                        }
                        lastW[b*3+0] = w.v.x; lastW[b*3+1] = w.v.y; lastW[b*3+2] = w.v.z;
                    }
                    // vert audits over sampled verts
                    int n2 = (int)skinnedView.size();
                    int step2 = n2 > 512 ? (n2 / 512) : 1;
                    double sumLoc = 0; float maxLoc = 0; int cntLoc = 0, farLoc = 0;
                    int maxLocVert = -1;
                    double cx = 0, cy = 0, cz = 0; int cn = 0;
                    std::vector<float> bl;
                    std::vector<int> blIdx;
                    bl.reserve(((size_t)(n2 / step2) + 2) * 3);
                    blIdx.reserve((n2 / step2) + 2);
                    for (int i = 0; i < n2; i += step2) {
                        const GpuVertexSkinned& g = skinnedView[i];
                        float lx = g.pos[0], ly = g.pos[1], lz = g.pos[2];
                        float wsum = 0, ox = 0, oy = 0, oz = 0;
                        int domB = -1; float domW = -1.f;
                        for (int k = 0; k < 4; k++) {
                            int bi = g.boneIndices[k]; if (bi < 0 || bi >= kMaxBones) bi = 0;
                            float wgt = g.boneWeights[k]; wsum += wgt;
                            if (wgt > domW) { domW = wgt; domB = bi; }
                            const float* m = bones.bones[bi];
                            ox += wgt*(m[0]*lx + m[4]*ly + m[8]*lz + m[12]);
                            oy += wgt*(m[1]*lx + m[5]*ly + m[9]*lz + m[13]);
                            oz += wgt*(m[2]*lx + m[6]*ly + m[10]*lz + m[14]);
                        }
                        if (wsum < 0.01f) continue;
                        ox /= wsum; oy /= wsum; oz /= wsum;
                        bl.push_back(ox); bl.push_back(oy); bl.push_back(oz);
                        blIdx.push_back(i);
                        cx += ox; cy += oy; cz += oz; cn++;
                        if (domB >= 0 && domB < numBones && domW > 0.3f) {
                            float dx = lx - bindP[domB*3+0], dy = ly - bindP[domB*3+1],
                                  dz = lz - bindP[domB*3+2];
                            float d = sqrtf(dx*dx + dy*dy + dz*dz);
                            sumLoc += d; cntLoc++;
                            if (d > maxLoc) { maxLoc = d; maxLocVert = i; }
                            if (d > 30.f) farLoc++;
                        }
                    }
                    if (cn > 0) { cx /= cn; cy /= cn; cz /= cn; }
                    fprintf(stderr,
                        "[C8_VERT] f=%d mesh='%s' locality: avg=%.1f max=%.1f far(>30u)=%d/%d "
                        "maxVert=%d | slots: moving=%d static=%d null=%d\n",
                        mFrameCount, c8mn, cntLoc ? (float)(sumLoc / cntLoc) : -1.f, maxLoc,
                        farLoc, cntLoc, maxLocVert, nMoving, nStatic, nNull);
                    float worstD = -1.f; int worstSl = -1;
                    for (int sl = 0; sl < (int)blIdx.size(); sl++) {
                        float dx = bl[sl*3+0]-(float)cx, dy = bl[sl*3+1]-(float)cy,
                              dz = bl[sl*3+2]-(float)cz;
                        float d = sqrtf(dx*dx + dy*dy + dz*dz);
                        if (d > worstD) { worstD = d; worstSl = sl; }
                    }
                    if (worstSl >= 0) {
                        int vi = blIdx[worstSl];
                        const GpuVertexSkinned& g = skinnedView[vi];
                        fprintf(stderr,
                            "[C8_SMEAR] f=%d mesh='%s' centroid=(%.1f,%.1f,%.1f) worstVert=%d "
                            "dev=%.1f bind=(%.1f,%.1f,%.1f) blended=(%.1f,%.1f,%.1f)\n",
                            mFrameCount, c8mn, (float)cx, (float)cy, (float)cz, vi, worstD,
                            g.pos[0], g.pos[1], g.pos[2],
                            bl[worstSl*3+0], bl[worstSl*3+1], bl[worstSl*3+2]);
                        for (int k = 0; k < 4; k++) {
                            int bi = g.boneIndices[k]; if (bi < 0 || bi >= kMaxBones) bi = 0;
                            if (g.boneWeights[k] < 0.01f) continue;
                            const float* m = bones.bones[bi];
                            RndTransformable* bt = (bi < numBones) ? owner->BoneTransAt(bi) : nullptr;
                            fprintf(stderr,
                                "[C8_SMEAR]   k=%d bi=%d w=%.2f bone='%s' skin.v=(%.1f,%.1f,%.1f) "
                                "bind=(%.1f,%.1f,%.1f)\n",
                                k, bi, g.boneWeights[k],
                                (bt && bt->Name()) ? bt->Name() : "?",
                                m[12], m[13], m[14],
                                (bi < numBones) ? bindP[bi*3+0] : 0.f,
                                (bi < numBones) ? bindP[bi*3+1] : 0.f,
                                (bi < numBones) ? bindP[bi*3+2] : 0.f);
                        }
                    }
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
        // render-polish 2026-06-11 (char-render): OPT-IN guard exemption for
        // rebound meshes (RB3_GUARD_EXEMPT_REBOUND=1), default OFF. EXPERIMENT
        // OUTCOME: pairing this with the decomp own==bound rest-rebake
        // (RB3_BOUND_REBAKE=1) anchors translation (skin-to-bone deltas <=92u, no
        // flings, no mixed anchors) but the native rotation-basis divergence
        // remains — far-from-bone verts smear by R*sin(theta) to persistent
        // 200-460u extents (gloves/fingernails/jackets vs ~70u character) and
        // exempt meshes drew as full-screen slabs. The 2.0x ratio guard is
        // CORRECT about those poses; keep dropping them by default until the
        // pose-pipeline basis root-cause (C8) is fixed. Kept opt-in for the next
        // iteration's A/B.
        static int sExemptRebound = -1;
        if (sExemptRebound < 0)
            sExemptRebound = getenv("RB3_GUARD_EXEMPT_REBOUND") ? 1 : 0;
        if (sExemptRebound && (mesh->mNativeBonesRebound ||
                               (owner && owner->mNativeBonesRebound)))
            guardActive = false;
        // HUB / focused-menu highlight-bar shard-guard exemption (Defect 3 of the
        // hub-highlight family; see also the Defect-1 colour fix ~L5620 and the
        // Defect-2 placement fix ~L4504). The focused-item yellow bar
        // (`highlight_main.mesh` / `highlight_pattern.mesh`, the LabelShrinkWrapper /
        // UILabel highlight quad) is a SKINNED quad whose 4 corner bones are
        // DELIBERATELY spread to shrink-wrap the focused label's text (UILabel::
        // UpdateAndDrawHighlightMesh -> corner->SetLocalPos from the text width). Its
        // bind-pose AABB is tiny, so for a WIDE label the blended world extent is many
        // times the bind extent — exactly the ratio the V24 shard guard treats as a
        // runaway crowd/character pose and DROPS. That is a false positive here: the
        // stretch is the mesh's whole purpose, and the bones are finite/sane. The
        // symptom is that the bar renders for NARROW items (QUICKPLAY, ratio < 2x) but
        // VANISHES for WIDE ones (START A ROAD CHALLENGE, ratio > 2x) — the "highlight
        // disappears on the bottom/long menu entry" bug. These meshes are already
        // scoped by name for the placement + colour fixes; exempt them here too. They
        // are screen-space UI overlays that cannot produce a scene-crossing shard.
        // Opt-out: RB3_NO_HUB_BAR_SHARD_EXEMPT=1 (restores the drop for A/B).
        {
            static int sHlShardExemptOff = -1;
            if (sHlShardExemptOff < 0)
                sHlShardExemptOff = getenv("RB3_NO_HUB_BAR_SHARD_EXEMPT") ? 1 : 0;
            if (!sHlShardExemptOff && mesh->Name() &&
                (std::strncmp(mesh->Name(), "highlight_main", 14) == 0 ||
                 std::strncmp(mesh->Name(), "highlight_pattern", 17) == 0))
                guardActive = false;
        }
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
            // IK_SHARD_VERT (render-polish wave-4 IK diagnosis): localize the single
            // worst-flung vertex of a candidate-shard mesh and attribute it to its
            // dominant bone, then report that bone's composed-skin rotation row vs the
            // bone's WORLD rotation row. The earlier per-bone-ORIGIN measures
            // (REBIND_DRAW_SKINPOS, C8_SLOT skin.v) only test translation; a vertex at
            // radius R from the bone with a rotation-basis error theta flings by
            // R*sin(theta), invisible to an origin-only metric. This finds the bone
            // whose basis flings the far verts. Gated; render-inert.
            if (getenv("IK_SHARD_VERT") && wext > 60.f) {
                static const char* sSel = getenv("IK_SHARD_VERT");
                const char* mn0 = mesh->Name() ? mesh->Name() : "?";
                bool match = (sSel[0]=='*' && sSel[1]==0);
                if (!match && mn0) { char buf[256]; std::strncpy(buf,sSel,255); buf[255]=0;
                    for(char*tok=std::strtok(buf,",");tok;tok=std::strtok(nullptr,","))
                        if(std::strstr(mn0,tok)){match=true;break;} }
                if (match) {
                    float wcx=0.5f*(wmn[0]+wmx[0]),wcy=0.5f*(wmn[1]+wmx[1]),wcz=0.5f*(wmn[2]+wmx[2]);
                    int worstI=-1, worstBone=-1; float worstD=0.f, worstW=0.f;
                    float wlx=0,wly=0,wlz=0;
                    for (int i=0;i<n;i+=step){
                        const GpuVertexSkinned& g = skinnedView[i];
                        float lx=g.pos[0],ly=g.pos[1],lz=g.pos[2];
                        float ox=0,oy=0,oz=0,wsum=0; int dom=-1; float domW=0;
                        for(int k=0;k<4;k++){int bi=g.boneIndices[k];if(bi<0||bi>=kMaxBones)bi=0;
                            float w=g.boneWeights[k];wsum+=w;if(w>domW){domW=w;dom=bi;}
                            const float* m=bones.bones[bi];
                            ox+=w*(m[0]*lx+m[4]*ly+m[8]*lz+m[12]);
                            oy+=w*(m[1]*lx+m[5]*ly+m[9]*lz+m[13]);
                            oz+=w*(m[2]*lx+m[6]*ly+m[10]*lz+m[14]);}
                        float dx=ox-wcx,dy=oy-wcy,dz=oz-wcz;float d=sqrtf(dx*dx+dy*dy+dz*dz);
                        if(d>worstD){worstD=d;worstI=i;worstBone=dom;worstW=domW;
                            wlx=lx;wly=ly;wlz=lz;}
                    }
                    if (worstBone>=0 && owner) {
                        RndTransformable* wb = (worstBone<owner->NumBones())?owner->BoneTransAt(worstBone):nullptr;
                        const Transform& off = owner->BoneOffsetAt(worstBone);
                        const Transform& wt = wb?wb->WorldXfm():off;
                        // radius of the bind vert from this bone's bind origin (model space)
                        // = |off^-1 applied... | -> use |vert - (-off.v)| approximated by |vert|
                        // The composed skin row0 vs bone world row0 tells if the basis rotated.
                        Transform sk; Multiply(off, wt, sk);
                        float rdx=wlx-(-off.v.x),rdy=wly-(-off.v.y),rdz=wlz-(-off.v.z);
                        float R=sqrtf(rdx*rdx+rdy*rdy+rdz*rdz);
                        static std::unordered_map<std::string,int> sV;
                        std::string key=mn0;
                        if (sV[key]++ % 30 == 0)
                            fprintf(stderr,
                                "[IK_SHARD_VERT] mesh='%s' wext=%.0f worstVtx=%d domBone[%d]='%s' w=%.2f "
                                "vertR=%.1f vertDevFromCentroid=%.0f bindVert=(%.1f,%.1f,%.1f)\n"
                                "    boneWorld.v=(%.1f,%.1f,%.1f) boneWorldRow0=(%.2f,%.2f,%.2f) "
                                "skinRow0=(%.2f,%.2f,%.2f) off.v=(%.1f,%.1f,%.1f)\n",
                                mn0, wext, worstI, worstBone, (wb&&wb->Name())?wb->Name():"?", worstW,
                                R, worstD, wlx,wly,wlz,
                                wt.v.x,wt.v.y,wt.v.z, wt.m.x.x,wt.m.x.y,wt.m.x.z,
                                sk.m.x.x,sk.m.x.y,sk.m.x.z, off.v.x,off.v.y,off.v.z);
                    }
                }
            }
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
            // render-polish wrap-up (pose-footwear-shard): the wave-5 WorldXfm
            // recompose (L4104-4156) repaired the BAND pose — its garment bones now
            // read SANE world coords (C8_PROBE: dropped footwear bones at-floor,
            // skinDet=1.0). The residual band-garment drops are a FALSE POSITIVE in
            // the fixed 2.0x ratio: a SMALL-bind garment (boot/glove/legwear, bind
            // ~12-25u; fingernails ~36u) legitimately spans 2.0-3.5x its tiny bind
            // AABB when the limb it skins curls hard, WITHOUT the world extent ever
            // becoming geometrically impossible (measured 25-85u across ALL band
            // footwear/gloves/legwear). A genuine band tear produces a far larger
            // world span (the wave-5 BEFORE leg fling reached hundreds of u, below
            // floor). The clean discriminator is the bone's owning skeleton, NOT the
            // ratio or the world extent: band garments resolve to the static shared
            // band skeleton (char/char/main/skeleton_unshared.milo, root=playerN),
            // while crowd/extras true shards bind char/extras|crowd/*.milo with a
            // ~200u skin-vs-bind smear, and instruments/UI bind prop/UI dirs. Same
            // detector as the wave-6 rebake (L4042). So: for BAND-member garments
            // ONLY, relax to a wider ratio cap (4.0x; deep curl tops out ~3.5x,
            // true tears jump >4.4x) PLUS an absolute world-extent cap (110u; > any
            // measured band garment of 85u, far below any real fling) as a backstop
            // so the relaxation can never pass a truly exploded band mesh. Crowd /
            // extras / instrument / UI keep the proven, crowd-safe 2.0x EXACTLY.
            // Native-only file -> Wii byte-identical + DC3-inert by construction.
            bool bandMember = false;
            int nb = owner ? owner->NumBones() : 0;
            for (int bm = 0; bm < nb && !bandMember; bm++) {
                RndTransformable* bbt = owner->BoneTransAt(bm);
                ObjectDir* bbd = bbt ? bbt->Dir() : 0;
                if (bbd && !bbd->mStoredFile.empty() &&
                    strstr(bbd->mStoredFile.c_str(), "skeleton_unshared.milo") != 0)
                    bandMember = true;
            }
            bool degenerate;
            if (bandMember) {
                // Three caps, all env-tunable for A/B (no rebuild); defaults from the
                // measured envelope. (1) ratio cap 4.0x: deep limb curl on a normal
                // garment tops out ~3.5x, true tears jump >4.4x. (2) absolute world
                // cap 110u: every legit band garment measured <=85u world, while a
                // real band tear spans 85-400u (the wave-5 BEFORE/RB3_NO_SKEL_WORLDFIX
                // control: min real tear 44.9u, typical 120-400u) -> 110u is a clean
                // backstop. (3) world FLOOR 40u below which the ratio test is SKIPPED:
                // a tiny-bind SUBMESH (a glove fingertip submesh binds ~3.8u; a finger
                // curl moves it to ~19u world = ratio 5x but geometrically sane) would
                // false-trip the 4x ratio. The control proves NO real band tear is
                // <44.9u world, so a 40u floor lets micro-bind submeshes through on a
                // normal curl while every genuine tear (>=44.9u) still hits the ratio
                // and/or world cap. Equivalent to the item's "verts within N units of
                // bind" sanity check, expressed on the already-computed world extent.
                static const char* sBWC = getenv("RB3_BAND_SHARD_WORLDCAP");
                static const char* sBRC = getenv("RB3_BAND_SHARD_RATIOCAP");
                static const char* sBWF = getenv("RB3_BAND_SHARD_WORLDFLOOR");
                static const float kBandWorldCap = sBWC ? (float)atof(sBWC) : 110.f;
                static const float kBandRatioCap = sBRC ? (float)atof(sBRC) : 4.0f;
                static const float kBandWorldFloor = sBWF ? (float)atof(sBWF) : 40.f;
                bool ratioBad = (wext > kBandWorldFloor) && (wext > kBandRatioCap * lext);
                degenerate = (wext > 15.f) && (lext > 0.001f) &&
                             (ratioBad || wext > kBandWorldCap);
            } else {
                degenerate = (wext > 15.f) && (lext > 0.001f) && (wext > 2.0f * lext);
            }
            // SHARD_RATIO_DBG: log EVERY skinned mesh's bind/world extent + ratio,
            // throttled per pointer, to see which slivers slip the threshold.
            if (getenv("SHARD_RATIO_DBG") && wext > 8.f) {
                const char* mn = mesh->Name() ? mesh->Name() : "?";
                static std::unordered_map<const void*,int> sR;
                if (sR[(const void*)mesh]++ % 60 == 0)
                    fprintf(stderr, "[SHARD_RATIO] mesh='%s' bindExt=%.2f worldExt=%.2f ratio=%.2f %s%s\n",
                        mn, lext, wext, wext/(lext+1e-6f),
                        bandMember?"band":"other", degenerate?" DROP":"");
            }
            if (degenerate && guardActive) {
                if (getenv("SHARD_DBG")) {
                    const char* mn = mesh->Name() ? mesh->Name() : "?";
                    // render-polish 2026-06-11 (char-render): attribute the drop to
                    // its owning dir + first-bone world position, so band-member
                    // drops (player_*0 dirs at the stage) separate from venue
                    // extras/crowd/preview chars sharing outfit mesh NAMES.
                    Hmx::Object* dirObj = mesh->Dir();
                    const char* dn = (dirObj && dirObj->Name()) ? dirObj->Name() : "-";
                    RndTransformable* b0 =
                        (owner && owner->NumBones() > 0) ? owner->BoneTransAt(0) : nullptr;
                    float bx = b0 ? b0->WorldXfm().v.x : 0.f,
                          by = b0 ? b0->WorldXfm().v.y : 0.f,
                          bz = b0 ? b0->WorldXfm().v.z : 0.f;
                    fprintf(stderr, "[SHARD_GUARD] dropped degenerate skinned mesh='%s' "
                        "bindExt=%.2f worldExt=%.2f ratio=%.1f f=%d dir='%s' "
                        "bone0=(%.1f,%.1f,%.1f)\n",
                        mn, lext, wext, wext/(lext+1e-6f), mFrameCount, dn, bx, by, bz);
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
    // RB3_HEADMAT_DBG: catch head.mesh reaching the material block with a NULL
    // mat (C8 head-invisible triage). Temporary probe.
    if (getenv("RB3_HEADMAT_DBG") && !mat && mesh->Name()
        && std::strcmp(mesh->Name(), "head.mesh") == 0) {
        static std::unordered_map<const void*, int> sNullSeen;
        if (sNullSeen[(const void*)mesh]++ == 0)
            fprintf(stderr, "[HEADMAT] mesh='head.mesh' owner=%p MAT=NULL\n", (void*)mesh);
    }
    // W1.3: material -> MaterialUniforms fill moved VERBATIM into RB3MaterialBinder.cpp
    // (RB3BuildMaterialUniforms). The bind-group plumbing + PipelineKey blend/zmode
    // below are unchanged and consume mu / isTextMeshHeur / gemForce from the unpack.
    RB3MaterialBindResult matRes = RB3BuildMaterialUniforms(mesh, mat, skinned, owner, mGpu);
    MaterialUniforms& mu = matRes.mu;
    bool isTextMeshHeur = matRes.isTextMeshHeur;
    bool gemForce = matRes.gemForce;
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

    // W0.3 per-draw state-log ring. INERT when RB3_DRAWLOG is off: DrawLogOn()
    // is a cached-static branch, so this compiles to one predicted test + return
    // on the hot path (no allocation, no .Get() calls) and leaves rendered output
    // byte-identical. When on, captures pipeline id, blend/zmode/layout/format/
    // flags, the column-major world xfm, the four opaque scene/mat/obj/bone
    // bind-group identity tokens, index/tri/vert counts, and the mesh-name hash.
    if (DrawLogOn())
        RecordDrawLog(key, obj.world, mSceneBindGroup.Get(), matBG.Get(),
                      objBG.Get(), boneBG.Get(), cachedIndexCount, (uint32_t)nf,
                      (uint32_t)(meshEntry.fpVerts > 0 ? meshEntry.fpVerts : 0),
                      skinned, mesh->Name());
}

// ===========================================================================
// W0.3 per-draw state-log ring — record / dump / debug accessors.
//
// ADDITIVE regression-net infra. INERT and near-zero-cost when RB3_DRAWLOG is
// unset (DrawLogOn() is a cached-static branch). Records a structured per-draw
// state snapshot from DrawMesh so the golden test (W0.3.S2/S3) can catch the
// two historical per-draw regression classes mechanically: co-location
// (identical world xfm across instances that should differ) and uniform/
// bind-group collapse (the a0f98ad class — distinct draws sharing one bind
// group). Bind-group handles are stored as OPAQUE identity tokens only (never
// dereferenced), dense-ified per stream at dump time.
// ===========================================================================

// FNV-1a of a NUL-terminated string (empty/NULL -> 0). Stable across runs/hosts.
static uint64_t RB3DrawLogFnv1a(const char* s) {
    if (!s || !s[0]) return 0;
    uint64_t h = 1469598103934665603ULL;  // FNV offset basis
    for (; *s; ++s) {
        h ^= (uint64_t)(unsigned char)*s;
        h *= 1099511628211ULL;            // FNV prime
    }
    return h;
}

bool BandRnd::DrawLogOn() const {
    // Cached env gate: getenv once, then a plain int test on the hot path. ORed
    // with the debug override so gtests can force recording without an env var.
    static int sEnabled = -1;
    if (sEnabled < 0) {
        const char* e = getenv("RB3_DRAWLOG");
        sEnabled = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return sEnabled != 0 || mDrawLogForced;
}

void BandRnd::RecordDrawLog(const PipelineKey& key, const float world[16],
                            const void* sceneBG, const void* matBG,
                            const void* objBG, const void* boneBG, uint32_t idx,
                            uint32_t tris, uint32_t verts, bool skinned,
                            const char* name) {
    if (mDrawLog.capacity() == 0) mDrawLog.reserve(512);
    RB3DrawRecord r;
    r.pipelineHash = (uint64_t)PipelineKeyHash{}(key);
    r.blend        = (uint8_t)(int)key.blend;
    r.zMode        = (uint8_t)(int)key.zMode;
    r.layout       = (uint8_t)(int)key.layout;
    r.flags        = (uint8_t)((key.hasDepth   ? 1u : 0u)       |
                               (key.alphaCut   ? 2u : 0u)       |
                               (key.alphaWrite ? 4u : 0u)       |
                               (skinned        ? 8u : 0u));
    r.targetFormat = (uint32_t)key.targetFormat;
    r.indexCount   = idx;
    r.triCount     = tris;
    r.vertCount    = verts;
    r.meshNameHash = RB3DrawLogFnv1a(name);
    for (int i = 0; i < 16; ++i) r.world[i] = world[i];
    r.sceneBG = sceneBG;
    r.matBG   = matBG;
    r.objBG   = objBG;
    r.boneBG  = boneBG;
    mDrawLog.push_back(r);
}

void BandRnd::DumpDrawLog() {
    // A dump is written only when RB3_DRAWLOG_DUMP is set. The debug setter can
    // enable recording (for gtests reading RB3DebugGetDrawLog) without wanting a
    // file, so absent a path this is a no-op even when DrawLogOn() is true.
    const char* path = getenv("RB3_DRAWLOG_DUMP");
    if (!path || !path[0]) return;

    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "BandRnd: draw-log dump: cannot open %s\n", path);
        return;
    }

    // Dense-ify each bind-group stream INDEPENDENTLY: assign 0-based ids in
    // first-seen order across the frame. This erases raw pointers (run/host
    // independent) while preserving the sharing pattern the golden compares.
    std::unordered_map<const void*, int> sceneIds, matIds, objIds, boneIds;
    auto denseId = [](std::unordered_map<const void*, int>& m, const void* p) -> int {
        auto it = m.find(p);
        if (it != m.end()) return it->second;
        int id = (int)m.size();
        m.emplace(p, id);
        return id;
    };

    fprintf(f, "{ \"frame\": %d, \"count\": %d,\n  \"draws\": [", mFrameCount,
            (int)mDrawLog.size());
    for (size_t i = 0; i < mDrawLog.size(); ++i) {
        const RB3DrawRecord& r = mDrawLog[i];
        int sceneId = denseId(sceneIds, r.sceneBG);
        int matId   = denseId(matIds,   r.matBG);
        int objId   = denseId(objIds,   r.objBG);
        int boneId  = denseId(boneIds,  r.boneBG);
        fprintf(f, "%s\n    { \"i\":%d, \"name\":\"0x%llx\", \"pipe\":\"0x%llx\", "
                   "\"blend\":%d, \"zmode\":%d, \"layout\":%d, \"fmt\":%u, "
                   "\"hasDepth\":%s, \"alphaCut\":%s, \"alphaWrite\":%s, \"skinned\":%s, "
                   "\"idx\":%u, \"tris\":%u, \"verts\":%u, "
                   "\"scene\":%d, \"mat\":%d, \"obj\":%d, \"bone\":%d,\n"
                   "      \"world\":[",
                (i == 0 ? "" : ","),
                (int)i,
                (unsigned long long)r.meshNameHash,
                (unsigned long long)r.pipelineHash,
                (int)r.blend, (int)r.zMode, (int)r.layout, (unsigned)r.targetFormat,
                (r.flags & 1) ? "true" : "false",
                (r.flags & 2) ? "true" : "false",
                (r.flags & 4) ? "true" : "false",
                (r.flags & 8) ? "true" : "false",
                (unsigned)r.indexCount, (unsigned)r.triCount, (unsigned)r.vertCount,
                sceneId, matId, objId, boneId);
        for (int e = 0; e < 16; ++e)
            fprintf(f, "%s%.6g", (e == 0 ? "" : ","), (double)r.world[e]);
        fprintf(f, "] }");
    }
    fprintf(f, "%s] }\n", mDrawLog.empty() ? "" : "\n  ");
    fclose(f);
}

// --- rb3-tests debug accessors (declared in RB3DrawLogDebug.h) ---------------
// Operate on the single global renderer gBandRnd. mDrawLog / mDrawLogForced are
// public members, so no friend access is needed.
const std::vector<RB3DrawRecord>& RB3DebugGetDrawLog() {
    return gBandRnd.mDrawLog;
}
void RB3DebugSetDrawLogEnabled(bool on) {
    gBandRnd.mDrawLogForced = on;
}
bool RB3DebugDrawLogEnabled() {
    return gBandRnd.DrawLogOn();
}

// RB3EnsureMeshGpu — MOVED to platform/RB3MeshCache.cpp (W1.2.S3, pure MOVE).
// The idempotent VB/IB upload helper used by BandRnd::WarmGpuForDir now lives in
// the RB3MeshCache TU next to the sMeshGpu cache + RB3UnpackMeshVerts it drives;
// its declaration is in platform/RB3MeshCache.h (bool RB3EnsureMeshGpu(BandRnd&,
// RndMesh*)). WarmGpuForDir's call below resolves through that header unchanged.

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

// RndMesh::OnSync MOVED (W1.2) to platform/RB3MeshCache.cpp (co-located with the
// sMeshGpu/sGeomSyncGen maps it mutates).

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
const char* kRB3ParticleShaderSource =
#include "gfx/Shaders/rb3_particle.wgsl.inc"
;

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
    float fx = camXfm.m.y.x, fy = camXfm.m.y.y, fz = camXfm.m.y.z;  // forward
    float cpx = camXfm.v.x, cpy = camXfm.v.y, cpz = camXfm.v.z;     // cam pos

    const Transform& relXfm = sys->RelativeXfm();

    // Material register color modulates the rasterized particle (Milo material
    // model: the per-particle ramp color is the RASTER color; the material's
    // RGBA register then multiplies it — same as a mesh's mat->color × texture).
    // The fog/cloud systems in the menu shell carry authored alpha dampeners
    // (fog_thin.mat=0.10, fog.mat=0.50, cloud_a01.mat=0.48) that scale their
    // density down to a thin atmospheric haze. The original DC3-derived billboard
    // path dropped this term (c = tex × p->col only), so RB3's menu street-fog —
    // resurrected by the Wii-matched InitParticle sim fix — rendered at up to 10×
    // its intended opacity (a green-grey full-frame wash). Folding matColor in
    // restores the authored thinness; for the typical mat (color 1,1,1,1) it is a
    // no-op, so gameplay venue FX / A1 hit-flames are unchanged. Opt-out:
    // RB3_PART_MATCOLOR_OFF=1 for A/B.
    float mcr = 1.0f, mcg = 1.0f, mcb = 1.0f, mca = 1.0f;
    static const bool sPartMatColorOff = getenv("RB3_PART_MATCOLOR_OFF") != nullptr;
    if (!sPartMatColorOff) {
        const Hmx::Color& mc = mat->GetColor();
        mcr = mc.red; mcg = mc.green; mcb = mc.blue; mca = mc.alpha;
    }
    // Atmospheric-haze thinning. The shell street-fog (fog.mat a=0.50,
    // fog_thin.mat a=0.10) is authored heavier on the Wii data path than the
    // retail (360/PS3) reference's thin haze — a full-frame green-grey wash once
    // the Wii-matched sim revived these systems. We pull the dampened-alpha
    // (mca < 1) systems toward the retail look with an additional alpha scale,
    // scoped so the common mca==1 venue FX / A1 hit-flames are untouched.
    // Tunable (RB3_PART_HAZE_SCALE, default 0.35); opt-out RB3_PART_HAZE_OFF=1.
    static const bool sHazeOff = getenv("RB3_PART_HAZE_OFF") != nullptr;
    static const float sHazeScale = []{
        const char* e = getenv("RB3_PART_HAZE_SCALE");
        return e ? (float)atof(e) : 0.35f;
    }();
    bool isHaze = (!sHazeOff && !sPartMatColorOff && mca < 0.999f);
    if (isHaze) {
        mca *= sHazeScale;
    }
    // Near-camera fade for haze systems. A large soft fog sprite (size up to
    // ~130 u here) whose centre is close to / behind the camera fills the whole
    // frame as the menu camera shots dolly THROUGH the street-fog volume — the
    // single worst-case wash (one camera-shot still smothered the frame at 60%+
    // even after the alpha thinning above). Retail's same walking-band shot keeps
    // the band readable against deep blacks, so the on-screen fog must fade as the
    // camera enters it. We fade each haze particle by how far its centre sits in
    // front of the camera relative to its own half-size: full alpha once it is
    // >= kFar half-sizes ahead, fading to 0 at/behind the camera. Only touches
    // dampened-alpha haze systems (gameplay FX / A1 flames have mca==1 → skipped).
    // Opt-out RB3_PART_NEARFADE_OFF=1.
    static const bool sNearFadeOff = getenv("RB3_PART_NEARFADE_OFF") != nullptr;

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
        float cr = p->col.red * mcr, cg = p->col.green * mcg,
              cb = p->col.blue * mcb, ca = p->col.alpha * mca;

        // Near-camera fade (haze systems only): forward distance from camera to
        // this particle's centre, measured in units of its half-size. Fade to 0
        // by the time the centre is at/behind the camera, full strength once it
        // is kFar half-sizes ahead. Cheap dot product against the cam forward.
        if (isHaze && !sNearFadeOff) {
            float dForward = (cx - cpx) * fx + (cy - cpy) * fy + (cz - cpz) * fz;
            float hs = halfSize > 1.0f ? halfSize : 1.0f;
            const float kFar = 2.0f;   // full alpha once >= 2 half-sizes ahead
            float t = dForward / (hs * kFar);
            if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
            ca *= t;
        }

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

    // PART_PROBE: per-system diagnostic (throttled). Dumps the system + material
    // identity, blend, particle count, and the first few particles' color/size/
    // world pos — for attributing full-screen billboard washes (green-slab class)
    // to their emitting system.
    static const bool sPartProbe = getenv("PART_PROBE") != nullptr;
    if (sPartProbe) {
        static std::unordered_map<std::string, int> sPP;
        const char* sn = sys->Name() ? sys->Name() : "?";
        if ((sPP[sn]++ % 120) == 0) {
            RndTex* dt = mat->GetDiffuseTex();
            const Hmx::Color& mc = mat->GetColor();
            fprintf(stderr,
                "[PART_PROBE] f=%d sys='%s' mat='%s' tex='%s' matColor(%.2f,%.2f,%.2f,%.2f) "
                "blend=%d n=%d cam='%s' calcFrame=%.1f frameDrive=%d\n",
                mFrameCount, sn, mat->Name() ? mat->Name() : "?",
                dt ? (dt->Name() ? dt->Name() : "?") : "none",
                mc.red, mc.green, mc.blue, mc.alpha,
                (int)mat->GetBlend(), numParticles,
                (cam->Name() ? cam->Name() : "?"),
                sys->CalcFrame(), -1);
            int shown = 0;
            for (RndParticle* p = head; p && shown < 3; p = p->Next(), shown++) {
                Vector3 wp; Multiply(p->Pos3(), relXfm, wp);
                fprintf(stderr,
                    "[PART_PROBE]   p%d col(%.2f,%.2f,%.2f,%.2f) colVel(%.2f,%.2f,%.2f,%.2f) vel(%.2f,%.2f,%.2f) "
                    "size=%.2f sizeVel=%.3f pos(%.1f,%.1f,%.1f) birth=%.1f death=%.1f\n",
                    shown, p->col.red, p->col.green, p->col.blue, p->col.alpha,
                    p->colVel.red, p->colVel.green, p->colVel.blue, p->colVel.alpha,
                    p->vel.x, p->vel.y, p->vel.z,
                    p->size, p->sizeVel, wp.x, wp.y, wp.z, p->birthFrame, p->deathFrame);
            }
        }
    }

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
