#pragma once

#include <webgpu/webgpu_cpp.h>

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
