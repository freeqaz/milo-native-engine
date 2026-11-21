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
// The console does not scan that table out as given. D3DDevice_SetGammaRamp
// (the XDK's D3D, statically linked: dc3-decomp build/373307D9/asm/xdk/d3d9i/
// state.s and gamma.s) first runs D3D::GammaRampCorrected(hw, ramp, 0):
// hw[i] = (table[ramp[i] >> 6] & 0x3FF) << 6, table = GetCurrentGammaTable(0),
// a 1024-entry 10-bit table built from the kernel's VdGetCurrentDisplayGamma
// display type: for code k, x = k / 1023, table[k] = round(1023 *
// Encode(sRGBdecode(x))), Encode the display's curve -- type 1 (sRGB, a VGA
// cable) sRGB encode, which round-trips to the identity at all 1024 codes;
// type 2 (TV, HDMI or component: BT.709, Xenia's default) the BT.709 OETF;
// type 3 pow(x, 1 / power); any other type TV. The D3D code is decoded in
// dc3-decomp archive/np-gfx-2026-11-14/lanes/sharp/item31 (README, gamma_ref.py).
//
// Display type sRGB (and every RB3 shape) runs the ramp formula per pixel,
// exactly as before the table existed. Any other type runs a 256-entry lookup
// of the corrected entries (a 256 x 1 texture), built on the CPU in the console's float order
// (HardwareTable below); one pass either way.
//
// Rndobj-free: the caller (WgpuRnd::EndDrawing) supplies g through
// rndshape::DisplayGamma() and the type through rndshape::DisplayGammaType();
// g = 0 means no ramp and the pass never runs.
#pragma once
#include <webgpu/webgpu_cpp.h>
#include <cstdint>
#include "gfx/BindGroupCache.h"

class GpuDevice;

class DisplayRamp {
public:
    // VdGetCurrentDisplayGamma's display types (D3D::GetCurrentGammaTable).
    static constexpr int kDisplaySRGB = 1;    // VGA: the ramp scans out as given
    static constexpr int kDisplayTV = 2;      // HDMI / component: BT.709
    static constexpr int kDisplayPower = 3;   // pow(x, 1 / power)
    static constexpr float kDefaultPower = 2.22222233f;   // Xenia's kernel_display_gamma_power

    // DxRnd::SetupGamma's entry for frame byte i >> 6: (u16)(pow(i / 256, g) * 1024).
    static uint16_t RampCode(float gamma, int i);
    // D3D::GetCurrentGammaTable(0) for a display type: 1024 10-bit codes.
    static void DisplayTable(int type, float power, uint16_t out[1024]);
    // What the console scans out for frame byte i: the corrected entry,
    // DisplayTable(type)[RampCode(g, i)], a 10-bit code.
    static void HardwareTable(float gamma, int type, float power, uint16_t out[256]);

    // Rewrites `frameTex` (whose 1x view is `frameView`) through the ramp
    // under display `type`. The texture must allow CopySrc; returns false,
    // doing nothing, when it does not.
    bool Apply(wgpu::CommandEncoder& encoder, const wgpu::Texture& frameTex,
               const wgpu::TextureView& frameView, float gamma, GpuDevice& gpu,
               int type = kDisplaySRGB, float power = kDefaultPower);
    // Bind group from `cache` (null: one created per frame); dc3 PLAN H-66.
    void SetBindGroupCache(BindGroupCache* cache) { mBindGroups = cache; }

private:
    BindGroupCache* mBindGroups = nullptr;
    wgpu::ShaderModule mShader;
    wgpu::BindGroupLayout mBGL;
    wgpu::RenderPipeline mPipe;
    wgpu::TextureFormat mFormat = wgpu::TextureFormat::Undefined;
    float mGamma = 0.0f;
    bool mTablePipe = false;        // mPipe is fs_table (else fs_ramp)
    int mTableType = 0;             // what mLut holds
    float mTableGamma = 0.0f, mTablePower = 0.0f;
    wgpu::Texture mLut;             // 256 x 1 R32Float: the corrected entries x 64
    wgpu::TextureView mLutView;
    wgpu::Texture mScratch;
    wgpu::TextureView mScratchView;
    uint32_t mW = 0, mH = 0;
};
