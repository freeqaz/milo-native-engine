// DC3 Native Port — WebGPU Texture Upload
// Provides real RndTex::PresyncBitmap() / SyncBitmap() implementations
// that upload bitmap data to GPU textures via TextureConvert.
// Overrides weak stubs in engine_stubs_generated.cpp.

#include "platform/Rnd_Wgpu.h"
#include "platform/TexGpu.h"
#include "gfx/TextureConvert.h"
#include "rndobj/Tex.h"
#include "rndobj/CubeTex.h"
#include "rndobj/Bitmap.h"
#include "platform/rndshape/RndShape.h"

#include "gfx/Screenshot.h"

#include <unordered_map>
#include <vector>
#include <cstdio>
#include <cstring>

// ============================================================================
// GPU texture side table — maps RndTex* to GPU resources
// ============================================================================

struct GpuTexData {
    wgpu::Texture texture;
    wgpu::TextureView view;
    wgpu::Texture depthTexture;
    wgpu::TextureView depthView;
    bool uploaded = false;
    bool renderTarget = false;
    const uint8_t* lastPixelPtr = nullptr;  // detect bitmap data changes
    uint32_t pixelFingerprint = 0;          // quick content check
    // Bitmap size the texture was last created at (-1 == never created from a
    // bitmap). PresyncBitmap re-reads the bitmap every time it re-creates, so a
    // bitmap swapped to a new size gets a texture of the new size.
    int lastW = -1;
    int lastH = -1;
};

static std::unordered_map<RndTex*, GpuTexData> sTexGpuData;

// Monotonic count of GPU textures PresyncBitmap has created from a bitmap: the
// first upload and every re-create after the pixel pointer or content changed.
// Read through GetGpuTexDebugInfo, so a caller can tell a re-create from a
// cache hit.
static unsigned long long sTexCreateCount = 0;

static bool NeedsDepthTarget(RndTex* tex) {
    if (!tex) return false;
    RndTex::Type type = tex->GetType();
    return (type & RndTex::kRendered) && !(type & 0x20) && type != RndTex::kDepthVolumeMap;
}

static wgpu::TextureFormat ChooseRenderTargetFormat(RndTex* tex) {
    if (tex && tex->GetType() == RndTex::kDepthVolumeMap)
        return wgpu::TextureFormat::RGBA8Unorm;
    // Match the surface format's sRGB-ness. If the surface is non-sRGB
    // (e.g., RGBA8Unorm on web), render targets must also be non-sRGB,
    // otherwise sRGB→linear→non-sRGB causes darkening.
    if (gWgpuRnd) {
        wgpu::TextureFormat sf = gWgpuRnd->Gpu().SurfaceFormat();
        if (sf == wgpu::TextureFormat::RGBA8Unorm ||
            sf == wgpu::TextureFormat::BGRA8Unorm)
            return wgpu::TextureFormat::RGBA8Unorm;
    }
    return wgpu::TextureFormat::RGBA8UnormSrgb;
}

static GpuTexData* EnsureRenderTargetData(RndTex* tex) {
    if (!tex || !gWgpuRnd || !gWgpuRnd->Gpu().IsReady()) return nullptr;
    if (!tex->IsRenderTarget()) return nullptr;

    // Render target textures may have 0 dimensions in .milo files when their
    // actual size is set at runtime via DTA scripts (set_bitmap/set_rendered).
    // On native, those scripts may not run, so use a reasonable default.
    int texW = tex->Width() > 0 ? tex->Width() : 256;
    int texH = tex->Height() > 0 ? tex->Height() : 256;

    GpuTexData& data = sTexGpuData[tex];
    // If the existing texture was uploaded as BC compressed (from PresyncBitmap),
    // it lacks RenderAttachment usage and must be replaced with a proper RGBA target.
    bool needColor = !data.texture || !data.view || (data.texture && !data.renderTarget);
    bool needDepth = NeedsDepthTarget(tex) && (!data.depthTexture || !data.depthView);
    if (needColor) {
        data.texture = TextureConvert::CreateRenderTarget(
            gWgpuRnd->Gpu(), texW, texH, ChooseRenderTargetFormat(tex)
        );
        data.view = data.texture.CreateView();

        // Clear to opaque black — WebGPU spec allows undefined initial contents,
        // which browsers often display as purple/magenta. Alpha must be 255
        // so meshes using SrcAlpha blending don't become transparent (showing
        // white canvas background on web).
        int w = texW, h = texH;
        size_t sz = (size_t)w * h * 4;
        std::vector<uint8_t> black(sz, 0);
        for (size_t i = 3; i < sz; i += 4) black[i] = 255; // set alpha to opaque
        wgpu::TexelCopyTextureInfo dest{};
        dest.texture = data.texture;
        wgpu::TexelCopyBufferLayout layout{};
        layout.bytesPerRow = (uint32_t)(w * 4);
        wgpu::Extent3D extent{(uint32_t)w, (uint32_t)h, 1};
        gWgpuRnd->Gpu().Queue().WriteTexture(&dest, black.data(), sz, &layout, &extent);
    }
    if (needDepth) {
        data.depthTexture = TextureConvert::CreateDepthTarget(
            gWgpuRnd->Gpu(), texW, texH
        );
        data.depthView = data.depthTexture.CreateView();
    } else if (!NeedsDepthTarget(tex)) {
        data.depthTexture = nullptr;
        data.depthView = nullptr;
    }
    data.uploaded = true;
    data.renderTarget = true;
    return &data;
}

// Public accessor — used by Mesh_Wgpu.cpp to get texture view for binding
wgpu::TextureView GetGpuTexView(RndTex* tex) {
    if (!tex) return wgpu::TextureView();
    if (tex->IsRenderTarget()) {
        GpuTexData* rt = EnsureRenderTargetData(tex);
        if (rt) {
            return rt->view;
        }
    }
    auto it = sTexGpuData.find(tex);
    if (it != sTexGpuData.end() && it->second.uploaded) {
        return it->second.view;
    }
    return wgpu::TextureView();
}

wgpu::TextureView GetGpuTexDepthView(RndTex* tex) {
    if (!tex || !tex->IsRenderTarget()) return wgpu::TextureView();
    GpuTexData* rt = EnsureRenderTargetData(tex);
    return rt ? rt->depthView : wgpu::TextureView();
}

bool IsGpuTexRenderable(RndTex* tex) {
    if (!tex) return false;
    auto it = sTexGpuData.find(tex);
    return it != sTexGpuData.end() && it->second.renderTarget;
}

// ============================================================================
// RndTex::PresyncBitmap — create GPU texture from bitmap data
// ============================================================================

// Quick fingerprint of pixel data to detect content changes
static uint32_t PixelFingerprint(const uint8_t* pixels, int size) {
    if (!pixels || size < 16) return 0;
    // Sample a few positions across the data
    uint32_t h = 0;
    int step = size / 8;
    if (step < 1) step = 1;
    for (int i = 0; i < size; i += step) {
        h = h * 31 + pixels[i];
    }
    return h;
}

static int sPresyncCalls = 0;
static int sPresyncNoGpu = 0;
static int sPresyncNoBitmap = 0;
static int sPresyncNoPixels = 0;
static int sPresyncAlreadyDone = 0;
static int sPresyncCreateFail = 0;
static int sPresyncOk = 0;

void RndTex_PrintPresyncStats() {
#ifdef DEBUG_LOGS
    printf("DC3 TexPresync: calls=%d noGpu=%d noBitmap=%d noPixels=%d done=%d fail=%d ok=%d total_uploaded=%d\n",
           sPresyncCalls, sPresyncNoGpu, sPresyncNoBitmap, sPresyncNoPixels,
           sPresyncAlreadyDone, sPresyncCreateFail, sPresyncOk, (int)sTexGpuData.size());
#endif
}

void RndTex::PresyncBitmap() {
    sPresyncCalls++;
    if (!gWgpuRnd) { sPresyncNoGpu++; return; }
    if (!gWgpuRnd->Gpu().IsReady()) { sPresyncNoGpu++; return; }

    // Only process regular textures with bitmap data
    if (mBitmap.Width() <= 0 || mBitmap.Height() <= 0 || mBitmap.Bpp() <= 0) {
        sPresyncNoBitmap++;
        return;
    }
    const uint8_t* curPixels = mBitmap.Pixels();
    if (!curPixels) {
        sPresyncNoPixels++;
        return;
    }

    // Check if already uploaded AND bitmap data hasn't changed.
    // Font textures may be uploaded before their data is loaded from
    // the .milo file — the bitmap has valid dimensions but placeholder
    // pixel data. When the real data loads, we need to re-upload.
    auto it = sTexGpuData.find(this);
    if (it != sTexGpuData.end() && it->second.uploaded) {
        uint32_t fp = PixelFingerprint(curPixels, mBitmap.PixelBytes());
        if (it->second.lastPixelPtr == curPixels && it->second.pixelFingerprint == fp) {
            sPresyncAlreadyDone++;
            return; // Same data, skip
        }
        // Data changed — re-upload
    }

    // Create GPU texture from Milo bitmap
    int numMips = 0;
    RndBitmap* mip = mBitmap.nextMip();
    while (mip) {
        numMips++;
        mip = mip->nextMip();
    }

    wgpu::Texture gpuTex = TextureConvert::CreateFromBitmap(
        gWgpuRnd->Gpu(), mBitmap, numMips);

    if (!gpuTex) {
        sPresyncCreateFail++;
        static int sFailLog = 0;
        if (sFailLog < 10) {
            sFailLog++;
            fprintf(stderr, "Tex_Wgpu: failed to create GPU texture for '%s' (%dx%d, %d bpp, %d mips)\n",
                    Name(), mBitmap.Width(), mBitmap.Height(), mBitmap.Bpp(), numMips);
        }
        return;
    }
    sPresyncOk++;

    GpuTexData data;
    data.texture = gpuTex;
    data.view = gpuTex.CreateView();
    data.uploaded = true;
    data.lastPixelPtr = curPixels;
    data.pixelFingerprint = PixelFingerprint(curPixels, mBitmap.PixelBytes());
    data.lastW = mBitmap.Width();
    data.lastH = mBitmap.Height();
    sTexCreateCount++;

    sTexGpuData[this] = data;
}

uint32_t GpuTexPixelFingerprint(const uint8_t* pixels, int size) {
    return PixelFingerprint(pixels, size);
}

GpuTexDebugInfo GetGpuTexDebugInfo(RndTex* tex) {
    GpuTexDebugInfo info;
    info.createCount = sTexCreateCount;
    auto it = sTexGpuData.find(tex);
    if (it != sTexGpuData.end()) {
        const GpuTexData& d = it->second;
        info.present = true;
        info.uploaded = d.uploaded;
        info.width = d.lastW;
        info.height = d.lastH;
        info.view = (const void*)d.view.Get();
        info.texture = (const void*)d.texture.Get();
    }
    return info;
}

// ============================================================================
// RndTex::SyncBitmap — finalize texture upload (Tier 1: no-op, done in Presync)
// ============================================================================

void RndTex::SyncBitmap() {
    // In Tier 1, all upload work is done in PresyncBitmap
}

void RndTex::MakeDrawTarget() {
    if (!gWgpuRnd) return;
    EnsureRenderTargetData(this);
    gWgpuRnd->SelectRenderTarget(this);
}

void RndTex::FinishDrawTarget() {
    if (!gWgpuRnd) return;
    gWgpuRnd->FinishRenderTarget(this);
}

// ============================================================================
// Cleanup — called when a RndTex is destroyed
// ============================================================================
// Note: RndTex destructor doesn't call us directly yet.
// For Tier 1, leaked GPU textures are acceptable (cleaned up at shutdown).
// TODO: Hook into RndTex destructor or add ref-counting.

void CleanupGpuTex(RndTex* tex) {
    sTexGpuData.erase(tex);
}

void UploadRGBAToRndTex(RndTex* tex, const uint8_t* rgba, int w, int h) {
    if (!tex || !rgba || !gWgpuRnd || !gWgpuRnd->Gpu().IsReady()) return;
    GpuTexData* rtData = EnsureRenderTargetData(tex);
    if (!rtData || !rtData->texture) return;

    wgpu::TexelCopyTextureInfo dest{};
    dest.texture = rtData->texture;
    dest.mipLevel = 0;
    dest.origin = {0, 0, 0};

    wgpu::TexelCopyBufferLayout layout{};
    layout.bytesPerRow = (uint32_t)(w * 4);

    wgpu::Extent3D size{(uint32_t)w, (uint32_t)h, 1};
    gWgpuRnd->Gpu().Queue().WriteTexture(&dest, rgba, (size_t)(w * h * 4), &layout, &size);
}

// ============================================================================
// Cube texture GPU side table — maps RndCubeTex* to GPU resources
// ============================================================================

struct GpuCubeTexData {
    wgpu::Texture texture;
    wgpu::TextureView view;
    bool uploaded = false;
};

static std::unordered_map<RndCubeTex*, GpuCubeTexData> sCubeTexGpuData;

// Debug aid: write every live render target's colour contents to
// <dir>/rt_<frame>_<texname>.png. Driven from WgpuRnd::MaybeCaptureFrame when
// MILO_DUMP_RT is set, so the dump lands next to that frame's screenshot.
void DumpGpuRenderTargets(const char* dir, int frame) {
    if (!gWgpuRnd || !dir) return;
    for (auto& kv : sTexGpuData) {
        RndTex* tex = kv.first;
        GpuTexData& d = kv.second;
        if (!d.renderTarget || !d.texture) continue;
        int w = (int)d.texture.GetWidth(), h = (int)d.texture.GetHeight();
        std::vector<uint8_t> px((size_t)w * h * 4);
        if (!gWgpuRnd->Gpu().ReadbackTexture(d.texture, w, h, px.data(), px.size())) continue;
        char name[128];
        snprintf(name, sizeof(name), "%s", tex->Name() ? tex->Name() : "anon");
        for (char* c = name; *c; ++c) if (*c == '/' || *c == ' ') *c = '_';
        char path[600];
        snprintf(path, sizeof(path), "%s/rt_%05d_%s.png", dir, frame, name);
        WritePNG(path, px.data(), w, h);
    }
}

void ClearGpuTexCaches() {
    sTexGpuData.clear();
    sCubeTexGpuData.clear();
}

wgpu::TextureView GetGpuCubeTexView(RndCubeTex* cubeTex) {
    if (!cubeTex || !gWgpuRnd) return wgpu::TextureView();

    auto it = sCubeTexGpuData.find(cubeTex);
    if (it != sCubeTexGpuData.end() && it->second.uploaded) {
        return it->second.view;
    }

    // Lazy upload: gather 6 face bitmaps and create cube texture
    RndBitmap* faces[6];
    bool allValid = true;
    for (int i = 0; i < 6; i++) {
        faces[i] = rndshape::CubeFaceBitmap(cubeTex, i);
        if (!faces[i] || faces[i]->Width() <= 0 || faces[i]->Height() <= 0 || !faces[i]->Pixels()) {
            allValid = false;
        }
    }
    if (!allValid) return wgpu::TextureView();

    wgpu::Texture gpuTex = TextureConvert::CreateCubeFromBitmaps(
        gWgpuRnd->Gpu(), faces, 6);
    if (!gpuTex) return wgpu::TextureView();

    // Create cube texture view
    wgpu::TextureViewDescriptor viewDesc{};
    viewDesc.dimension = wgpu::TextureViewDimension::Cube;
    viewDesc.arrayLayerCount = 6;
    viewDesc.baseArrayLayer = 0;
    viewDesc.mipLevelCount = 1;
    viewDesc.baseMipLevel = 0;

    GpuCubeTexData data;
    data.texture = gpuTex;
    data.view = gpuTex.CreateView(&viewDesc);
    data.uploaded = true;
    sCubeTexGpuData[cubeTex] = data;

    return data.view;
}
