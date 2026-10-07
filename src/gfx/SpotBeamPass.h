// SpotBeamPass — retail NgSpotlightDrawer's volumetric beams on WebGPU.
//
// Ported from RB3 Xbox 360 retail: the CPU side from rb3-xenon
// world/SpotlightDrawer_NG.cpp (RenderScene, RenderBeams, RenderCone,
// RenderConeDefs, SetupXSection, RenderSphere, RenderSheet, BlurRT,
// SetupForPostProcess), the GPU side from the shipped `xbox_shaders`
// (tools/rb3-dc3-parity/xobx.py dump, xenia-gpu-shader-compiler ucode):
//
//   depthvolume  option bits (ShaderMgr.unk1c << 1): 0 cone, 2 sheet, 4 sphere.
//                Drawn on each spotlight's NG shaft mesh into a half-size
//                A8R8G8B8 target cleared to (0, 0, 0, 1), blend ONE/ONE add,
//                no depth test (RndShaderDepthVolume::Select). The cone culls
//                CCW (cull override 3: the far side of the shaft draws), the
//                sheet and sphere cull nothing (override 1).
//     cone.ps    intersects the ray eye -> pixel with the cone (apex c25,
//                axis c26, cos^2 of the half angle c28.w), clips it by the
//                scene's linear depth and the pixel's own distance, and
//                writes  c90.rgb * 0.004 * |viewdepth(exit) - viewdepth(enter)|
//                * mean((1 - s)^2) * xsection,  s = axial distance / total
//                length; xsection = (1 - c86.x) + c86.x * tex11(|u|, 0).r with
//                u the pixel's place between the silhouette planes c87/c88.
//     sheet.ps   c90.rgb * c91.w * xsection(uv) * vertex alpha * visible /
//                |dot(ray, c91.xyz) + 0.01|.
//     sphere.ps  c90.rgb * 0.625 * (min(back, scene) - min(front, scene)), with
//                front/back = view depth -/+ tex11(uv).r * c91.w.
//     The fog terms (c127.z/w, set by SetupFogDensityState) are zero while the
//     beams draw, so the density map does not reach them.
//   blur         option 0x10000 (5 taps, weights 0.1 0.25 0.3 0.25 0.1). Retail
//                runs it twice, offsets (+-1, +-2 texels) along x then y:
//                sSeparateBlurPasses is 1 and sBlurAmount 1.0 in retail data
//                (0x82C711C4 / 0x82C711C0).
//   postprocess  option bit 51: after the bloom screen blend,
//                  c += beam.rgb * (fog.r * c127.y + c127.x) * c91.x
//                with c91.x = mIntensity * 32 and c127 = (base, smoke * (1 -
//                base)) from SetupFogDensityMap (percent / 100). That add lives
//                in gfx/RB3RetailPost; this pass hands it the blurred target.
//
// Rndobj-free: the backend resolves meshes, textures and the camera.
#pragma once
#include <webgpu/webgpu_cpp.h>

#include <cstdint>

#include "platform/SpotBeamHook.h"

class GpuDevice;

class SpotBeamPass {
public:
    // The camera the beams render with (retail mSpotCam).
    struct Camera {
        float pos[3];       // WorldXfm().v
        float fwd[3];       // WorldXfm().m.y (Milo cameras look down +y)
        float nearPlane, farPlane;
        float zRange[2];    // RndCam::mZRange
        float viewProj[16]; // the scene's view-projection, row-major (pos * VP)
    };

    // The pixel-shader constants retail uploads for one beam. Only those the
    // depthvolume shaders read are kept; register numbers in the comments.
    struct Constants {
        int shape = 0;           // 0 cone, 1 sheet, 2 sphere (ShaderMgr.unk1c)
        float c10[4] = {};       // eye position, 1
        float c25[4] = {};       // cone apex, 1 / total length
        float c26[4] = {};       // cone axis, total length
        float c27[4] = {};       // eye - apex, 1
        float c28[4] = {};       // cone quadric; .w = cos^2(half angle)
        float c30[4] = {};       // view plane: fwd, -dot(fwd, eye)
        float c86[4] = {};       // cross-section visibility
        float c87[4] = {};       // right silhouette plane
        float c88[4] = {};       // left silhouette plane
        float c89[4] = {};       // GetDepthRangeValues: near, far, zratio, zratio * zNear
        float c90[4] = {};       // beam colour
        float c91[4] = {};       // cone: (0, half distance, 0, 1/far); sheet: (m.z, sSheetW);
                                 // sphere: (0, 0, 0.625, top radius)
        float c127[4] = {};      // SetupFogDensityState: (0, 1/far, 0, 0)
        float c9[4] = {};        // RenderScene: sFogScale
    };

    // RenderBeams' per-beam setup. False when retail would not draw the beam
    // (no length) or the inputs give non-finite constants.
    static bool BeamConstants(const NativeSpotBeam& beam, const Camera& cam, Constants& out);
    // SetupForPostProcess + SetupFogDensityMap: the composite's c91.x and c127.xy.
    static void CompositeConstants(const NativeSpotBeamFrame& frame, float out[3]);
    // BlurRT(amountX, amountY)'s tap offsets for a w x h target, in UV units.
    static void BlurOffsets(float amountX, float amountY, int w, int h, float out[5][2]);
    static const float kBlurWeights[5];

    struct Draw {
        Constants k;
        float meshXfm[12];        // rows m.x, m.y, m.z, v
        wgpu::Buffer vertexBuffer;
        uint32_t vertexStride = 0; // 64 or 88 (gfx/VertexFormats.h)
        uint64_t vertexBytes = 0;
        wgpu::Buffer indexBuffer;  // uint16 triangle list
        uint32_t indexCount = 0;
        wgpu::TextureView xsection; // null: white
    };

    // Renders the beams into a half-size target (sceneW/2 x sceneH/2), reading
    // `depthView` (a depth-only, `depthSamples`-sampled view of the scene depth,
    // sceneW x sceneH), and blurs it. Returns false, recording nothing, when
    // there is nothing to draw or the depth is single-sampled. On true,
    // OutputView() holds the blurred beams.
    bool Run(wgpu::CommandEncoder& encoder, const Camera& cam, const Draw* draws, size_t count,
             const wgpu::TextureView& depthView, uint32_t depthSamples, int sceneW, int sceneH,
             GpuDevice& gpu);
    const wgpu::TextureView& OutputView() const { return mView[0]; }
    const wgpu::Texture& OutputTexture() const { return mTex[0]; }  // RGBA8Unorm, readable
    void Terminate();

private:
    bool EnsurePipelines(GpuDevice& gpu);
    void EnsureTargets(int w, int h, GpuDevice& gpu);

    wgpu::ShaderModule mShader;
    wgpu::BindGroupLayout mBeamBGL;   // uniforms, depth (MS), xsection, sampler
    wgpu::BindGroupLayout mBlurBGL;   // uniforms, src, sampler
    wgpu::PipelineLayout mBeamPL, mBlurPL;
    // [shape][stride 64/88]
    wgpu::RenderPipeline mBeamPipe[3][2];
    wgpu::RenderPipeline mBlurPipe;
    wgpu::Sampler mLinear;
    wgpu::Texture mWhiteTex;
    wgpu::TextureView mWhiteView;

    wgpu::Texture mTex[2];
    wgpu::TextureView mView[2];
    int mW = 0, mH = 0;
    bool mReady = false;
    bool mFailed = false;
    bool mWarnedSamples = false;
};
