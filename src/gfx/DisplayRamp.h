// DisplayRamp — a display gamma ramp applied to the finished frame, the way
// the Xbox 360 scans its front buffer out through the table set with
// D3DDevice_SetGammaRamp.
//
// RB3 sets that table in DxRnd::SetupGamma (rb3-xenon rnddx9/Rnd_Xbox.cpp):
// for i in 0..255, entry = (u16)(pow(i / 256, g) * 1024) * 64, the same for
// red, green and blue, with g from the system config's (rnd (gamma g)). The
// pass evaluates that formula per pixel, so a frame byte i reads back as the
// ramp entry divided by 65535. It runs after everything the frame draws,
// UI included, since the ramp is a property of the display, not of any draw.
//
// Rndobj-free: the caller (WgpuRnd::EndDrawing) supplies g through
// rndshape::DisplayGamma(); 0 means no ramp and the pass never runs.
#pragma once
#include <webgpu/webgpu_cpp.h>

class GpuDevice;

class DisplayRamp {
public:
    // Rewrites `frameTex` (whose 1x view is `frameView`) through the ramp.
    // The texture must allow CopySrc; returns false, doing nothing, when it
    // does not.
    bool Apply(wgpu::CommandEncoder& encoder, const wgpu::Texture& frameTex,
               const wgpu::TextureView& frameView, float gamma, GpuDevice& gpu);

private:
    wgpu::ShaderModule mShader;
    wgpu::BindGroupLayout mBGL;
    wgpu::RenderPipeline mPipe;
    wgpu::TextureFormat mFormat = wgpu::TextureFormat::Undefined;
    float mGamma = 0.0f;
    wgpu::Texture mScratch;
    wgpu::TextureView mScratchView;
    uint32_t mW = 0, mH = 0;
};
