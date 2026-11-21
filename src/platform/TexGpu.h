#pragma once

#include <webgpu/webgpu_cpp.h>
#include <cstdint>
#include <vector>

class RndTex;
class RndCubeTex;

// GPU texture view accessors (defined in Tex_Wgpu.cpp)
wgpu::TextureView GetGpuTexView(RndTex* tex);
wgpu::TextureView GetGpuTexDepthView(RndTex* tex);
// The depth of a self-shadow map target as a binding (depth aspect); null for
// any other texture (rndshape::kSelfShadowTexType).
wgpu::TextureView GetGpuTexDepthSampleView(RndTex* tex);
wgpu::TextureView GetGpuCubeTexView(RndCubeTex* cubeTex);

// The GPU texture behind a texture's view (null when it has none), for tests
// that read one back. Byte-identical bitmaps share one (Tex_Wgpu.cpp).
wgpu::Texture GetGpuTexTexture(RndTex* tex);

// Upload raw RGBA pixel data to a render-target RndTex's GPU texture
void UploadRGBAToRndTex(RndTex* tex, const uint8_t* rgba, int w, int h);

// Choose render target format matching the surface's sRGB-ness
wgpu::TextureFormat ChooseRenderTargetFormat(RndTex* tex);

// np-look47: what a texture pass into `tex` draws into. samples > 1: draw into
// msaaView / msaaDepthView and resolve into resolveView. gpuWidth x gpuHeight
// is the attachments' size, which under LookSettings::texPassScale may exceed
// tex->Width() x Height(). passClears: the pass clears colour, so the 4x pair
// may be made now (a pass resumed with a load never makes one);
// passClears && passClearsDepth (or no depth): the target may also be re-made
// at the size the look asks for now -- the one place a target's GPU size
// changes. False when the texture has no render target.
struct GpuTexPassTarget {
    wgpu::TextureView resolveView;
    wgpu::TextureView depthView;
    wgpu::TextureView msaaView;
    wgpu::TextureView msaaDepthView;
    uint32_t gpuWidth = 0;
    uint32_t gpuHeight = 0;
    uint32_t samples = 1;
    // The kept answer to "is this target drawn like the frame" (np-look47),
    // decided at the target's last clearing pass.
    bool likeFrame = false;
};
bool GetGpuTexPassTarget(RndTex* tex, GpuTexPassTarget& out, bool passClears = true,
                         bool passClearsDepth = true);
// np-look47 step 2, tests: the output rows the size rule reads in place of
// GpuDevice::OutputHeight() (0 = the device's).
void SetTexPassOutputRowsForTest(int rows);
// The area-weighted box filter a scaled target's readback is brought back to
// its own size with (RGBA8, sw x sh -> dw x dh).
void BoxFilterRGBA8(const uint8_t* src, int sw, int sh, std::vector<uint8_t>& dst, int dw, int dh);

// np-look47: the consumer's half of "a texture pass is drawn like the frame".
// The engine half is fixed (RndTex type exactly kRendered, no mips); the
// predicate answers the rest (dc3: the target is the output of texture
// renderers that all draw a character). Asked only where a pass that clears
// the target begins; the answer is kept until the target's next clearing
// pass, so a sampling call never re-asks it. No predicate registered (rb3,
// rb3-xenon): no target is drawn like the frame. nullptr unregisters.
using TexPassLikeFrameFn = bool (*)(RndTex*);
void SetTexPassLikeFramePredicate(TexPassLikeFrameFn fn);
TexPassLikeFrameFn GetTexPassLikeFramePredicate();

// Check if a texture has a proper renderable GPU backing (RGBA, not compressed)
bool IsGpuTexRenderable(RndTex* tex);

// Release every cached GPU texture (2D and cube). WgpuRnd::Terminate calls it
// before the device shuts down: the caches are file-scope statics, and a
// consumer that leaves through exit() rather than _exit() would otherwise drop
// the last device reference from a static destructor, after the Vulkan loader
// has started tearing down (RB3-Wii's Debug::Exit takes that path).
void ClearGpuTexCaches();

// For the [GPUCOUNT] log line. GpuTexCount: distinct live GPU textures (2D +
// cube; a texture shared by byte-identical bitmaps counts once).
// GpuTexHolderCount: RndTexes holding one. GpuTexShareCount: monotonic count of
// uploads that shared a live texture instead of creating one.
size_t GpuTexCount();
size_t GpuTexHolderCount();
unsigned long long GpuTexShareCount();
// Bytes of the live bitmap textures (PresyncBitmap's; a shared texture once;
// render targets and cube maps not counted), and the most they have been.
uint64_t GpuTexBitmapBytes();
uint64_t GpuTexBitmapBytesPeak();

// The content fingerprint RndTex::PresyncBitmap keys re-creation on (with the
// pixel pointer): 8 evenly spaced bytes folded as h = h*31 + byte; 0 for a null
// or <16-byte buffer.
uint32_t GpuTexPixelFingerprint(const uint8_t* pixels, int size);

// Snapshot of one texture's GPU cache entry, plus the global count of textures
// PresyncBitmap has created from bitmaps (first uploads and re-creates). Used to
// tell a re-create from a cache hit; no draw path reads it.
struct GpuTexDebugInfo {
    bool present = false;          // a cache entry exists for the texture
    bool uploaded = false;         // its texture and view are live
    int width = -1;                // bitmap size the texture was created at
    int height = -1;
    int mipLevels = -1;            // levels of the GPU texture (-1 = none)
    const void* view = nullptr;    // wgpu::TextureView handle (identity only)
    const void* texture = nullptr; // wgpu::Texture handle (identity only)
    unsigned long long createCount = 0;
    // Uploads that found a live texture with a byte-identical bitmap and
    // shared it instead of creating one (Tex_Wgpu.cpp sSharedTex).
    unsigned long long shareCount = 0;
    bool shared = false;           // this texture's GPU texture is shared
};
GpuTexDebugInfo GetGpuTexDebugInfo(RndTex* tex);
// Mip levels of a cube texture's GPU copy (-1 = not uploaded). Debug/tests.
int GetGpuCubeTexMipLevels(RndCubeTex* cubeTex);
