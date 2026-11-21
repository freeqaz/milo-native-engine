// DC3 Native Port — WebGPU Texture Upload
// Provides real RndTex::PresyncBitmap() / SyncBitmap() implementations
// that upload bitmap data to GPU textures via TextureConvert.
// Overrides weak stubs in engine_stubs_generated.cpp.

#include "platform/Rnd_Wgpu.h"
#include "platform/TexGpu.h"
#include "gfx/TextureConvert.h"
#include <algorithm>
#include "rndobj/Tex.h"
#include "rndobj/CubeTex.h"
#include "rndobj/Bitmap.h"
#include "platform/rndshape/RndShape.h"
#include "platform/LookProfile.h"

#include "gfx/Screenshot.h"
#include "gfx/FrameCost.h"

#include <algorithm>
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
    // A self-shadow map's depth as a binding (depth aspect only); null for
    // every other target (rndshape::kSelfShadowTexType).
    wgpu::TextureView depthSampleView;
    bool uploaded = false;
    bool renderTarget = false;
    const uint8_t* lastPixelPtr = nullptr;  // detect bitmap data changes
    uint32_t pixelFingerprint = 0;          // quick content check
    // Bitmap size the texture was last created at (-1 == never created from a
    // bitmap). PresyncBitmap re-reads the bitmap every time it re-creates, so a
    // bitmap swapped to a new size gets a texture of the new size.
    int lastW = -1;
    int lastH = -1;
    // The bitmap texture is shared with every other RndTex whose bitmap is
    // byte-identical (sSharedTex below); `shareKey` names the entry.
    bool shared = false;
    uint64_t shareKey = 0;
    // Bytes of the bitmap texture this holder created (0 for a share hit, a
    // render target, or once the texture moved into sSharedTex).
    uint64_t bytes = 0;
};

static std::unordered_map<RndTex*, GpuTexData> sTexGpuData;

// Byte-identical bitmaps share one GPU texture (milo-native-engine
// docs/native/dc3-backend-for-rb3-wii.md section 38.3). A flashcard, a results
// row, a crowd member's eyes are copies of one object -- RndTex::Copy, or the
// same .milo loaded again -- each with its own copy of the same pixels, and
// each became its own texture on first draw.
//
// What makes it safe:
//  - Only kRegular textures share, and never one that is or was a render
//    target. A bitmap texture is TextureBinding|CopyDst and nothing writes it
//    after CreateFromBitmap: render targets get their own texture
//    (EnsureRenderTargetData, which also lets go of a shared one), and
//    UploadRGBAToRndTex writes only those. A holder whose bitmap changes is
//    re-created (PresyncBitmap) and lets go of the shared entry; it never
//    writes into it.
//  - The key covers everything CreateFromBitmap is given or reads: the
//    RndTex type, the format the bitmap maps to, numMips and the level cap
//    passed in, size, bpp, order, and every level's bytes and palette (the
//    whole chain, not only the levels the cap uploads). A hit must then match
//    the reference holder on those inputs and byte for byte, and the
//    reference holder must still hold the bitmap it uploaded (same pixel
//    pointer and fingerprint). So a 64-bit collision costs a create, never a
//    wrong texture. Anything that one day changes what CreateFromBitmap is
//    given per texture (a per-class level cap, say) must join UploadInputs.
//  - The view is the texture's default view; samplers are per material, so
//    no sampler state is shared.
//  - Entries are refcounted: the view leaves the bind-group cache only when
//    the last holder lets go, and when the reference holder goes another
//    holder becomes the reference.
struct UploadInputs {
    int type = 0;
    int format = 0;     // wgpu::TextureFormat from MapBitmapFormat
    int numMips = 0;    // as passed to CreateFromBitmap
    int maxLevels = 0;  // as passed to CreateFromBitmap
    bool operator==(const UploadInputs& o) const {
        return type == o.type && format == o.format && numMips == o.numMips &&
               maxLevels == o.maxLevels;
    }
};

struct SharedTex {
    wgpu::Texture texture;
    wgpu::TextureView view;
    const RndTex* owner = nullptr;   // the bitmap a hit is compared against
    UploadInputs inputs;
    int refs = 0;
    uint64_t bytes = 0;  // the texture's, counted once for every holder
};
static std::unordered_map<uint64_t, SharedTex> sSharedTex;
static unsigned long long sTexShareCount = 0;

// GPU memory of a bitmap texture: every mip level of its format (BC1 8 bytes a
// 4x4 block, BC2/BC3 16, otherwise 4 bytes a texel).
static uint64_t BitmapTextureBytes(const wgpu::Texture& t) {
    if (!t) return 0;
    const wgpu::TextureFormat f = t.GetFormat();
    const bool bc1 = f == wgpu::TextureFormat::BC1RGBAUnorm;
    const bool bc = bc1 || f == wgpu::TextureFormat::BC2RGBAUnorm || f == wgpu::TextureFormat::BC3RGBAUnorm;
    uint64_t total = 0;
    uint32_t w = t.GetWidth(), h = t.GetHeight();
    for (uint32_t level = 0; level < t.GetMipLevelCount(); level++) {
        if (bc) total += (uint64_t)((w + 3) / 4) * ((h + 3) / 4) * (bc1 ? 8 : 16);
        else total += (uint64_t)w * h * 4;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return total * t.GetDepthOrArrayLayers();
}

// Live bitmap-texture bytes (each distinct texture once) and the highest they
// have been. Memory only grows at a create, so the peak is taken there.
static uint64_t sBitmapBytesPeak = 0;
uint64_t GpuTexBitmapBytes() {
    uint64_t total = 0;
    for (const auto& kv : sTexGpuData)
        if (!kv.second.shared && !kv.second.renderTarget) total += kv.second.bytes;
    for (const auto& kv : sSharedTex) total += kv.second.bytes;
    return total;
}
uint64_t GpuTexBitmapBytesPeak() { return sBitmapBytesPeak; }

static uint64_t HashBytes(uint64_t h, const void* data, size_t n) {
    // FNV-1a over 8-byte words, then the tail.
    const uint8_t* p = (const uint8_t*)data;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001b3ull;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

static uint64_t HashInt(uint64_t h, long long v) { return HashBytes(h, &v, sizeof(v)); }

static uint64_t BitmapShareKey(const RndBitmap& bmp, const UploadInputs& in) {
    uint64_t h = 0xcbf29ce484222325ull;
    h = HashInt(h, in.type);
    h = HashInt(h, in.format);
    h = HashInt(h, in.numMips);
    h = HashInt(h, in.maxLevels);
    h = HashInt(h, (long long)bmp.Order());
    h = HashInt(h, bmp.Bpp());
    for (const RndBitmap* m = &bmp; m; m = m->nextMip()) {
        h = HashInt(h, m->Width());
        h = HashInt(h, m->Height());
        h = HashInt(h, m->PixelBytes());
        h = HashInt(h, m->PaletteBytes());
        if (m->Pixels()) h = HashBytes(h, m->Pixels(), (size_t)m->PixelBytes());
        if (m->Palette()) h = HashBytes(h, m->Palette(), (size_t)m->PaletteBytes());
    }
    return h;
}

// The whole mip chain, byte for byte.
static bool SameBitmapBytes(const RndBitmap& a, const RndBitmap& b) {
    if (a.Bpp() != b.Bpp() || a.Order() != b.Order()) return false;
    const RndBitmap* x = &a;
    const RndBitmap* y = &b;
    for (; x && y; x = x->nextMip(), y = y->nextMip()) {
        if (x->Width() != y->Width() || x->Height() != y->Height() ||
            x->PixelBytes() != y->PixelBytes() || x->PaletteBytes() != y->PaletteBytes())
            return false;
        if (!x->Pixels() || !y->Pixels()) return false;
        if (memcmp(x->Pixels(), y->Pixels(), (size_t)x->PixelBytes()) != 0) return false;
        if ((x->Palette() == nullptr) != (y->Palette() == nullptr)) return false;
        if (x->Palette() && memcmp(x->Palette(), y->Palette(), (size_t)x->PaletteBytes()) != 0)
            return false;
    }
    return x == nullptr && y == nullptr;
}

// Drops `d`'s hold on its bitmap texture. The view leaves the bind-group cache
// only when the last RndTex sharing it lets go.
static void ReleaseBitmapTexture(const RndTex* tex, GpuTexData& d) {
    if (d.shared) {
        auto s = sSharedTex.find(d.shareKey);
        d.shared = false;
        if (s != sSharedTex.end()) {
            if (--s->second.refs > 0) {
                if (s->second.owner == tex) {
                    // Another holder becomes the reference bitmap.
                    s->second.owner = nullptr;
                    for (auto& kv : sTexGpuData)
                        if (kv.first != tex && kv.second.shared && kv.second.shareKey == d.shareKey) {
                            s->second.owner = kv.first;
                            break;
                        }
                }
                return;
            }
            sSharedTex.erase(s);
        }
    }
    if (gWgpuRnd && d.view) gWgpuRnd->ForgetTextureView(d.view);
}

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
    // Named for GPU captures and the MILO_GPU_TIMERS target list.
    const std::string label = std::string("RndTex:") + (tex->Name() ? tex->Name() : "");
    // If the existing texture was uploaded as BC compressed (from PresyncBitmap),
    // it lacks RenderAttachment usage and must be replaced with a proper RGBA target.
    bool needColor = !data.texture || !data.view || (data.texture && !data.renderTarget);
    bool needDepth = NeedsDepthTarget(tex) && (!data.depthTexture || !data.depthView);
    if (needColor) {
        // The old view (a BC upload being replaced) leaves the bind-group cache.
        if (data.shared) ReleaseBitmapTexture(tex, data);
        else gWgpuRnd->ForgetTextureView(data.view);
        data.texture = TextureConvert::CreateRenderTarget(
            gWgpuRnd->Gpu(), texW, texH, ChooseRenderTargetFormat(tex), label.c_str()
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
        const bool sampled = rndshape::kSelfShadowMap
            && (int)tex->GetType() == rndshape::kSelfShadowTexType;
        data.depthTexture = TextureConvert::CreateDepthTarget(
            gWgpuRnd->Gpu(), texW, texH, (label + " depth").c_str(), sampled
        );
        data.depthView = data.depthTexture.CreateView();
        data.depthSampleView = nullptr;
        if (sampled) {
            wgpu::TextureViewDescriptor vd{};
            vd.label = "SelfShadowMap";
            vd.aspect = wgpu::TextureAspect::DepthOnly;
            data.depthSampleView = data.depthTexture.CreateView(&vd);
        }
    } else if (!NeedsDepthTarget(tex)) {
        data.depthTexture = nullptr;
        data.depthView = nullptr;
        data.depthSampleView = nullptr;
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

wgpu::TextureView GetGpuTexDepthSampleView(RndTex* tex) {
    if (!tex || !tex->IsRenderTarget()) return wgpu::TextureView();
    GpuTexData* rt = EnsureRenderTargetData(tex);
    return rt ? rt->depthSampleView : wgpu::TextureView();
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
    framecost::Scope cost(framecost::kTexture);
    int numMips = 0;
    RndBitmap* mip = mBitmap.nextMip();
    while (mip) {
        numMips++;
        mip = mip->nextMip();
    }

    // The consumer's loader decides how many of those levels reach the GPU:
    // DC3's DxTex::SyncBitmap creates a regular texture with
    // min(NumMips()+1, 2) levels (rndshape::kBitmapMaxMipLevels; 0 = all); the
    // look profile can raise it (LookSettings::bitmapMipLevels).
    // The other bitmapped kinds DC3 creates with one level (kRegularLinear,
    // movie, scratch) carry no mips, so the cap only ever bites on kRegular;
    // render targets and cube maps do not come through here.
    // A live kRegular RndTex with this exact bitmap, given the same
    // CreateFromBitmap inputs, already has the texture: share it. (Not when
    // the reference holder is this texture: its bitmap is what changed.)
    // Levels uploaded: the look profile's (milo::Look().bitmapMipLevels; a
    // look switch by inspection token does not re-upload, so textures keep the
    // startup look's count, and ApplyLookSampling clamps world samplers to the
    // switched look's count where that is lower -- np-look26 f,
    // milo::WorldTexLodMaxLevels). It is in the key all the same, so two holders
    // given different level counts can never share. The text/HUD clamp to two
    // levels is sampler state of the draw (MaterialSetup ApplyLookSampling,
    // lodMaxClamp), not of the texture, so a glyph page and a world texture of
    // the same bytes can share one texture and still sample as their class does.
    const int maxLevels = milo::Look().bitmapMipLevels;
    const bool shareable = GetType() == RndTex::kRegular && !IsRenderTarget() &&
                           !(it != sTexGpuData.end() && it->second.renderTarget);
    UploadInputs inputs;
    inputs.type = (int)GetType();
    inputs.format =
        (int)TextureConvert::MapBitmapFormat(mBitmap, gWgpuRnd->Gpu().HasBCCompression());
    inputs.numMips = numMips;
    inputs.maxLevels = maxLevels;
    const uint64_t shareKey = shareable ? BitmapShareKey(mBitmap, inputs) : 0;
    GpuTexData data;
    auto shared = shareable ? sSharedTex.find(shareKey) : sSharedTex.end();
    bool hit = false;
    if (shared != sSharedTex.end() && shared->second.owner && shared->second.owner != this &&
        shared->second.inputs == inputs) {
        // The reference holder must still hold the bitmap it uploaded.
        const RndTex* owner = shared->second.owner;
        auto od = sTexGpuData.find(const_cast<RndTex*>(owner));
        hit = od != sTexGpuData.end() && od->second.shared && od->second.shareKey == shareKey &&
              od->second.lastPixelPtr == owner->mBitmap.Pixels() &&
              od->second.pixelFingerprint ==
                  PixelFingerprint(owner->mBitmap.Pixels(), owner->mBitmap.PixelBytes()) &&
              SameBitmapBytes(mBitmap, owner->mBitmap);
    }
    if (hit) {
        data.texture = shared->second.texture;
        data.view = shared->second.view;
        shared->second.refs++;
        data.shared = true;
        data.shareKey = shareKey;
        sTexShareCount++;
    } else {
        wgpu::Texture gpuTex = TextureConvert::CreateFromBitmap(
            gWgpuRnd->Gpu(), mBitmap, numMips, maxLevels);
        if (!gpuTex) {
            sPresyncCreateFail++;
            static int sFailLog = 0;
            if (sFailLog < 10) {
                sFailLog++;
                fprintf(stderr,
                        "Tex_Wgpu: failed to create GPU texture for '%s' (%dx%d, %d bpp, %d mips)\n",
                        Name(), mBitmap.Width(), mBitmap.Height(), mBitmap.Bpp(), numMips);
            }
            return;
        }
        data.texture = gpuTex;
        data.view = gpuTex.CreateView();
        data.bytes = BitmapTextureBytes(gpuTex);
        sTexCreateCount++;
    }
    sPresyncOk++;
    data.uploaded = true;
    data.lastPixelPtr = curPixels;
    data.pixelFingerprint = PixelFingerprint(curPixels, mBitmap.PixelBytes());
    data.lastW = mBitmap.Width();
    data.lastH = mBitmap.Height();

    // A re-upload lets go of the old texture (its view leaves the bind-group
    // cache once nothing shares it).
    if (it != sTexGpuData.end()) ReleaseBitmapTexture(this, it->second);
    if (!hit && shareable && sSharedTex.find(shareKey) == sSharedTex.end()) {
        // The first holder of these bytes: later twins share it.
        SharedTex& e = sSharedTex[shareKey];
        e.texture = data.texture;
        e.view = data.view;
        e.owner = this;
        e.inputs = inputs;
        e.refs = 1;
        e.bytes = data.bytes;
        data.bytes = 0;
        data.shared = true;
        data.shareKey = shareKey;
    }
    sTexGpuData[this] = data;
    if (!hit) sBitmapBytesPeak = std::max(sBitmapBytesPeak, GpuTexBitmapBytes());
}

uint32_t GpuTexPixelFingerprint(const uint8_t* pixels, int size) {
    return PixelFingerprint(pixels, size);
}

wgpu::Texture GetGpuTexTexture(RndTex* tex) {
    auto it = sTexGpuData.find(tex);
    return it == sTexGpuData.end() ? wgpu::Texture() : it->second.texture;
}

GpuTexDebugInfo GetGpuTexDebugInfo(RndTex* tex) {
    GpuTexDebugInfo info;
    info.createCount = sTexCreateCount;
    info.shareCount = sTexShareCount;
    auto it = sTexGpuData.find(tex);
    if (it != sTexGpuData.end()) {
        const GpuTexData& d = it->second;
        info.present = true;
        info.uploaded = d.uploaded;
        info.width = d.lastW;
        info.height = d.lastH;
        info.view = (const void*)d.view.Get();
        info.texture = (const void*)d.texture.Get();
        if (d.texture) info.mipLevels = (int)d.texture.GetMipLevelCount();
        info.shared = d.shared;
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
    auto it = sTexGpuData.find(tex);
    if (it == sTexGpuData.end()) return;
    // Cached bind groups pin the view (and through it the texture); drop them
    // with the texture so a dead RndTex's GPU memory is not held (once no twin
    // shares it).
    if (it->second.shared) ReleaseBitmapTexture(tex, it->second);
    else if (gWgpuRnd) gWgpuRnd->ForgetTextureView(it->second.view);
    sTexGpuData.erase(it);
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

int GetGpuCubeTexMipLevels(RndCubeTex* cubeTex) {
    auto it = sCubeTexGpuData.find(cubeTex);
    if (it == sCubeTexGpuData.end() || !it->second.uploaded || !it->second.texture) return -1;
    return (int)it->second.texture.GetMipLevelCount();
}

bool ReadbackGpuRenderTarget(RndTex* tex, std::vector<uint8_t>& texels, int& w, int& h) {
    if (!gWgpuRnd || !tex) return false;
    auto it = sTexGpuData.find(tex);
    if (it == sTexGpuData.end() || !it->second.renderTarget || !it->second.texture) return false;
    const wgpu::Texture& t = it->second.texture;
    w = (int)t.GetWidth();
    h = (int)t.GetHeight();
    texels.resize((size_t)w * h * 4);
    return gWgpuRnd->Gpu().ReadbackTexture(t, w, h, texels.data(), texels.size());
}

// Debug aid: write every live render target's colour contents to
// <dir>/rt_<frame>_<texname>.png. Driven from WgpuRnd::MaybeCaptureFrame when
// MILO_DUMP_RT is set, so the dump lands next to that frame's screenshot.
void DumpGpuRenderTargets(const char* dir, int frame) {
    if (!gWgpuRnd || !dir) return;
    for (auto& kv : sTexGpuData) {
        RndTex* tex = kv.first;
        int w = 0, h = 0;
        std::vector<uint8_t> px;
        if (!ReadbackGpuRenderTarget(tex, px, w, h)) continue;
        char name[128];
        snprintf(name, sizeof(name), "%s", tex->Name() ? tex->Name() : "anon");
        for (char* c = name; *c; ++c) if (*c == '/' || *c == ' ') *c = '_';
        char path[600];
        snprintf(path, sizeof(path), "%s/rt_%05d_%s.png", dir, frame, name);
        WritePNG(path, px.data(), w, h);
    }
}

void ClearGpuTexCaches() {
    if (gWgpuRnd) gWgpuRnd->BindGroups().Clear();
    sTexGpuData.clear();
    sSharedTex.clear();
    sCubeTexGpuData.clear();
}

size_t GpuTexCount() {
    // Distinct live GPU textures: a shared texture counts once, however many
    // RndTexes hold it (every holder with `shared` set has its entry in
    // sSharedTex).
    size_t own = 0;
    for (const auto& kv : sTexGpuData)
        if (!kv.second.shared) own++;
    return own + sSharedTex.size() + sCubeTexGpuData.size();
}

size_t GpuTexHolderCount() { return sTexGpuData.size() + sCubeTexGpuData.size(); }

unsigned long long GpuTexShareCount() { return sTexShareCount; }

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

    // Look profile: the DC3 profiles upload the cube's whole mip chain, as
    // DxCubeTex::Sync does; Native keeps the base level only.
    wgpu::Texture gpuTex = TextureConvert::CreateCubeFromBitmaps(
        gWgpuRnd->Gpu(), faces, 6, milo::Look().cubeMipChain);
    if (!gpuTex) return wgpu::TextureView();

    // Create cube texture view
    wgpu::TextureViewDescriptor viewDesc{};
    viewDesc.dimension = wgpu::TextureViewDimension::Cube;
    viewDesc.arrayLayerCount = 6;
    viewDesc.baseArrayLayer = 0;
    viewDesc.mipLevelCount = gpuTex.GetMipLevelCount();
    viewDesc.baseMipLevel = 0;

    GpuCubeTexData data;
    data.texture = gpuTex;
    data.view = gpuTex.CreateView(&viewDesc);
    data.uploaded = true;
    sCubeTexGpuData[cubeTex] = data;

    return data.view;
}
