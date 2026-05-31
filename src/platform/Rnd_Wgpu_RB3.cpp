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
#include "math/Mtx.h"
#include "math/Vec.h"
#include "os/Debug.h"
#include "platform/NativeSettings.h"

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

    // Choose format: always RGBA8Unorm (CPU-decompress DXT). Simple, portable,
    // works on the null backend used in headless CI.
    wgpu::TextureFormat fmt = wgpu::TextureFormat::RGBA8Unorm;
    std::vector<uint8_t> rgba((size_t)w * h * 4, 0xFF);
    uint8_t* dst = rgba.data();
    unsigned int order = bmp.Order();
    unsigned int dxt = order & 0x38;

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
    {
        wgpu::TextureDescriptor dd{};
        dd.label = "BandDepth"; dd.size = {(uint32_t)W, (uint32_t)H, 1};
        dd.format = wgpu::TextureFormat::Depth24PlusStencil8;
        dd.usage = wgpu::TextureUsage::RenderAttachment;
        dd.mipLevelCount = 1;
        mDepthTex = mGpu.Device().CreateTexture(&dd);
        mDepthView = mDepthTex.CreateView();
    }

    mSceneRing.Init(mGpu.Device(), 16 * 1024, "SceneUBO");
    mMaterialRing.Init(mGpu.Device(), 256 * 1024, "MaterialUBO");
    mObjectRing.Init(mGpu.Device(), 256 * 1024, "ObjectUBO");
    mBoneRing.Init(mGpu.Device(), 256 * 1024, "BoneUBO");

    CreateDefaultTextures();

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

    // File-static GPU cache that holds wgpu::Texture / wgpu::TextureView refs.
    // It would otherwise destruct during libc's static-destructor phase (after
    // the Vulkan ICD .so is unmapped) and drop its last refs there, leading to
    // dangling vkDestroy* calls. Drop it while Dawn is still alive.
    sTexGpu.clear();

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

void BandRnd::WriteSceneUniforms(RndCam* cam) {
    SceneUniforms s{};

    float viewProj[16];
    float camPos[3] = {0, 0, 0};

    if (getenv("RB3_RENDER_DBG")) fprintf(stderr, "[dbg] WriteSceneUniforms cam=%p\n", (void*)cam);
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

    // One directional key light + bright ambient so unlit materials still read.
    s.numLights = 1;
    s.lightDirs[0][0] = -0.4f; s.lightDirs[0][1] = -0.5f; s.lightDirs[0][2] = -0.75f; s.lightDirs[0][3] = 0;
    s.lightColors[0][0] = 1.0f; s.lightColors[0][1] = 1.0f; s.lightColors[0][2] = 1.0f; s.lightColors[0][3] = 1.0f;
    s.ambientColor[0] = s.ambientColor[1] = s.ambientColor[2] = 0.45f; s.ambientColor[3] = 1.0f;
    s.numPointLights = 0;
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
    mDrawnMeshes = 0;
    mDrawnTris = 0;
    mSceneRing.Reset();
    mMaterialRing.Reset();
    mObjectRing.Reset();
    mBoneRing.Reset();

    mFrameView = mGpu.IsHeadless() ? mGpu.AcquireHeadlessFrame() : mGpu.AcquireNextFrame();
    if (!mFrameView) { fprintf(stderr, "BandRnd: frame acquire failed\n"); return; }

    WriteSceneUniforms(cam);

    mEncoder = mGpu.Device().CreateCommandEncoder();

    wgpu::RenderPassColorAttachment colorAtt{};
    colorAtt.view = mFrameView;
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
}

void BandRnd::EndFrame() {
    if (!mGpuReady) return;
    if (mInPass) { mPass.End(); mInPass = false; }
    // Defensive: if a render-target pass was somehow left open at frame end
    // (it should always be closed by FinishDrawTarget within the same frame),
    // clear the latch so the next frame starts clean rather than carrying a
    // stale RT-redirect state into a fresh encoder.
    mRtActiveTex = nullptr;
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
                        "meshes=%d tris=%d uploaded_tex=%d\n",
                mFrameCount, camName,
                cur ? cur->WorldXfm().v.x : 0.f,
                cur ? cur->WorldXfm().v.y : 0.f,
                cur ? cur->WorldXfm().v.z : 0.f,
                mClearColor.red, mClearColor.green, mClearColor.blue,
                mDrawnMeshes, mDrawnTris, texCount);
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

wgpu::BindGroup BandRnd::MakeMaterialBindGroup(uint32_t off, RndMat* mat) {
    // Resolve the material's diffuse texture (if any) to its GPU view.
    // Triggers a lazy upload if the texture hasn't been SyncBitmap'd yet —
    // ensures texture data arrives even when the matched-fork PostLoad path
    // doesn't call SyncBitmap before the first DrawMesh.
    wgpu::TextureView diffuse = mWhiteView;
    if (mat) {
        RndTex* dt = mat->GetDiffuseTex();
        if (dt) {
            wgpu::TextureView v = GetRB3TexView(dt);
            if (!v) v = UploadRndTexIfNeeded(mGpu, dt);
            if (v) diffuse = v;
        }
    }
    wgpu::BindGroupEntry e[11] = {};
    e[0].binding = 0;  e[0].buffer = mMaterialRing.Buffer(); e[0].offset = off; e[0].size = sizeof(MaterialUniforms);
    e[1].binding = 1;  e[1].textureView = diffuse;
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
    unsigned v = __builtin_bswap32((unsigned)packed);
    out[0] = ((v >> 0) & 0xFF) / 255.0f; out[1] = ((v >> 8) & 0xFF) / 255.0f;
    out[2] = ((v >> 16) & 0xFF) / 255.0f; out[3] = ((v >> 24) & 0xFF) / 255.0f;
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
    colorAtt.view = mFrameView;
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

    // --- Unpack vertices into engine GpuVertexRB3 / GpuVertexSkinned layout ---
    std::vector<GpuVertexRB3> gpuVerts;
    std::vector<GpuVertexSkinned> gpuVertsSkinned;
    if (nv > 0) {
        // Uncompressed RB3 Vert (Color32 packed; bone data in Vert.boneWeights /
        // Vert.boneIndices).
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
        // Xbox-compressed verts (read verbatim by our HX_NATIVE Mesh.cpp branch).
        // Bone data, when present, lives at off 28 (b0 = BLENDWEIGHT, UDEC4N
        // 10-10-10-2 unsigned) and off 32 (b1 = BLENDINDICES, UBYTE4) — the D3D
        // field names are swapped vs intent (see engine VertexFormats.cpp).
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
        return; // no geometry
    }
    nv = skinned ? (int)gpuVertsSkinned.size() : (int)gpuVerts.size();
    // GEM_VTX: one-shot dump of the gem prism's unpacked local-space verts +
    // bounds + world-projected extent, to confirm the geometry is non-degenerate
    // and lands on screen.
    if (getenv("GEM_VTX")) {
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
    bool dbg = getenv("RB3_RENDER_DBG") != nullptr;
    std::vector<uint16_t> indices;
    indices.reserve(nf * 3);
    for (int i = 0; i < nf; i++) {
        indices.push_back(faces[i].v1);
        indices.push_back(faces[i].v2);
        indices.push_back(faces[i].v3);
    }

    wgpu::Buffer vbuf, ibuf;
    {
        wgpu::BufferDescriptor bd{};
        bd.label = "MeshVB";
        bd.size = skinned ? ((uint64_t)nv * sizeof(GpuVertexSkinned))
                          : ((uint64_t)nv * sizeof(GpuVertexRB3));
        bd.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        vbuf = mGpu.Device().CreateBuffer(&bd);
        mGpu.Queue().WriteBuffer(vbuf, 0,
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
        ibuf = mGpu.Device().CreateBuffer(&bd);
        mGpu.Queue().WriteBuffer(ibuf, 0, indices.data(), padded);
    }

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
    uint32_t objOff = mObjectRing.Write(mGpu.Queue(), &obj, sizeof(obj));
    wgpu::BindGroup objBG;
    {
        wgpu::BindGroupEntry e{};
        e.binding = 0; e.buffer = mObjectRing.Buffer(); e.offset = objOff; e.size = sizeof(ObjectUniforms);
        wgpu::BindGroupDescriptor bd{};
        bd.layout = mPipelines.ObjectLayout(); bd.entryCount = 1; bd.entries = &e;
        objBG = mGpu.Device().CreateBindGroup(&bd);
    }

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
        for (int b = 0; b < numBones; b++) {
            RndTransformable* bt = owner->BoneTransAt(b);
            // Identity fallback for a null/garbage bone.
            float* dst = bones.bones[b];
            for (int i = 0; i < 16; i++) dst[i] = (i % 5 == 0) ? 1.f : 0.f;
            if (!bt) { sFallbackBones++; continue; }
            const Transform& wt = bt->WorldXfm();
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
            MiloXfmToColMajor(skin, dst);
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
                if (nv > 0) {
                    const GpuVertexSkinned& gv = gpuVertsSkinned[0];
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
        int n = (int)gpuVertsSkinned.size();
        if (n >= 3) {
            // Bind-pose (local) AABB and the blended (world) AABB, the latter via
            // the EXACT 4-bone blend the shader uses.
            float lmn[3]={1e30f,1e30f,1e30f}, lmx[3]={-1e30f,-1e30f,-1e30f};
            float wmn[3]={1e30f,1e30f,1e30f}, wmx[3]={-1e30f,-1e30f,-1e30f};
            int step = n > 256 ? (n / 256) : 1; // sample to bound cost
            for (int i = 0; i < n; i += step) {
                const GpuVertexSkinned& g = gpuVertsSkinned[i];
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

    uint32_t boneOff = mBoneRing.Write(mGpu.Queue(), &bones, sizeof(bones));
    wgpu::BindGroup boneBG;
    {
        wgpu::BindGroupEntry e{};
        e.binding = 0; e.buffer = mBoneRing.Buffer(); e.offset = boneOff; e.size = sizeof(BoneUniforms);
        wgpu::BindGroupDescriptor bd{};
        bd.layout = mPipelines.BoneLayout(); bd.entryCount = 1; bd.entries = &e;
        boneBG = mGpu.Device().CreateBindGroup(&bd);
    }

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
        mu.useAlphaAsRGB = isTextMeshHeur ? 1.0f : 0.0f;
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
    uint32_t matOff = mMaterialRing.Write(mGpu.Queue(), &mu, sizeof(mu));
    wgpu::BindGroup matBG = MakeMaterialBindGroup(matOff, mat);

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

    mPass.SetPipeline(pipe);
    mPass.SetBindGroup(0, mSceneBindGroup, 0, nullptr);
    mPass.SetBindGroup(1, matBG, 0, nullptr);
    mPass.SetBindGroup(2, objBG, 0, nullptr);
    mPass.SetBindGroup(3, boneBG, 0, nullptr);
    mPass.SetVertexBuffer(0, vbuf, 0, WGPU_WHOLE_SIZE);
    mPass.SetIndexBuffer(ibuf, wgpu::IndexFormat::Uint16, 0, WGPU_WHOLE_SIZE);
    mPass.DrawIndexed((uint32_t)indices.size(), 1, 0, 0, 0);

    mDrawnMeshes++;
    mDrawnTris += nf;
}

// ===========================================================================
// Real out-of-line bodies for the matched-fork HX_NATIVE virtuals that the
// link stubs currently weak-alias. A strong def here displaces the weak alias.
// ===========================================================================

void RndMesh::DrawShowing() {
    gBandRnd.DrawMesh(this);
}

void RndMesh::OnSync(int) {
    // Geometry changed; nothing GPU-cached to invalidate in this simple backend
    // (we re-upload every draw). No-op.
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
