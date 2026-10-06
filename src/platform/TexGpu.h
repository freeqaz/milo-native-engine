#pragma once

#include <webgpu/webgpu_cpp.h>
#include <cstdint>

class RndTex;
class RndCubeTex;

// GPU texture view accessors (defined in Tex_Wgpu.cpp)
wgpu::TextureView GetGpuTexView(RndTex* tex);
wgpu::TextureView GetGpuTexDepthView(RndTex* tex);
wgpu::TextureView GetGpuCubeTexView(RndCubeTex* cubeTex);

// Upload raw RGBA pixel data to a render-target RndTex's GPU texture
void UploadRGBAToRndTex(RndTex* tex, const uint8_t* rgba, int w, int h);

// Choose render target format matching the surface's sRGB-ness
wgpu::TextureFormat ChooseRenderTargetFormat(RndTex* tex);

// Check if a texture has a proper renderable GPU backing (RGBA, not compressed)
bool IsGpuTexRenderable(RndTex* tex);

// Release every cached GPU texture (2D and cube). WgpuRnd::Terminate calls it
// before the device shuts down: the caches are file-scope statics, and a
// consumer that leaves through exit() rather than _exit() would otherwise drop
// the last device reference from a static destructor, after the Vulkan loader
// has started tearing down (RB3-Wii's Debug::Exit takes that path).
void ClearGpuTexCaches();

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
    const void* view = nullptr;    // wgpu::TextureView handle (identity only)
    const void* texture = nullptr; // wgpu::Texture handle (identity only)
    unsigned long long createCount = 0;
};
GpuTexDebugInfo GetGpuTexDebugInfo(RndTex* tex);
