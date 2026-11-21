#pragma once
#include <webgpu/webgpu_cpp.h>
#include "gfx/BindGroupCount.h"
#include "gfx/BindGroupCache.h"

class GpuDevice;

// Depth of field, ported from retail NgDOFProc::DoPost (RB3 Xbox 360) and the
// shaders it drives:
//   1. downsample_4x: the scene, 4x4 box, into a quarter-size target.
//   2. blur (8-tap permutation): SetVHBlurWeights' "horizontal" tap set,
//      quarter A -> quarter B, then its "vertical" set, B -> A. Each pass is
//      the mean of 8 bilinear taps at uv + offset_i; there is no centre tap.
//   3. postprocess's depth-of-field bit (0x8): per pixel,
//        t = z * range - scale * range,  range = 1 / (scale - bias)
//        f = sat(min(max(|t|, minBlur), maxBlur))
//        out = sat(mix(scene, blur, f))
//      where z is the pixel's depth, read once (textureLoad, no sampler), and
//      scale / bias are the projected depths of the focal plane and of
//      focalPlane * (1 - blurDepth), as NgDOFProc::Set computes them.
// Retail runs reverse-Z and its shader reads 1 - z; this renderer's depth is
// already 0 at the near plane, so z is used as read.
class DofPass {
public:
    // The CPU half of NgDOFProc::Set + DoPost: the four constants retail loads
    // into c24 (range, -scale * range, minBlur, maxBlur). zNear/zFar are the
    // camera's ZRange.
    static void RetailConstants(float focalPlane, float blurDepth, float maxBlur, float minBlur,
                                float nearPlane, float farPlane, float zNear, float zFar,
                                float out[4]);
    // SetVHBlurWeights' tap offsets in UV units for a blurW x blurH target:
    // vertical = false gives the horizontal pass, true the vertical one.
    static void RetailBlurOffsets(bool vertical, int blurW, int blurH, float blurWidthScale,
                                  float out[8][2]);
    // The offsets Run uses for a sceneW x sceneH scene: RetailBlurOffsets for
    // its quarter-size blur target -- the quarter of the LOGICAL 1280x720 frame
    // when the scene is drawn at another size (renderScale = drawn / logical,
    // GpuDevice::RenderScaleFor: a render scale, the output's own resolution,
    // or both; below 1 for an output smaller than 720p), so the blur spans the
    // part of the screen it does on the original.
    static void SceneBlurOffsets(bool vertical, int sceneW, int sceneH, float renderScale,
                                 float blurWidthScale, float out[8][2]);

    // When TheDOFProc is enabled: blurs `sceneView` (sceneW x sceneH, format
    // `sceneFormat`) by `depthView` (a depth-only view of the scene depth,
    // `depthSamples` samples, same size) into OutputView() and returns true.
    // Otherwise returns false and records nothing. The scene is not modified.
    // `enhanced` (np-gfx-post round 10, DC3_LOOK=enhanced's LookSettings::
    // postDofFilter, the world grade only): the downsample as an 8x8 tent and
    // the composite's blur read through a cubic B-spline instead of bilinear,
    // so the quarter-size blur shows no texel creases; the Poisson taps are
    // scaled by EnhancedTapScale so the blur's spread stays retail's.
    bool Run(wgpu::CommandEncoder& encoder, const wgpu::TextureView& sceneView,
             wgpu::TextureFormat sceneFormat, const wgpu::TextureView& depthView,
             uint32_t depthSamples, int sceneW, int sceneH, GpuDevice& gpu,
             bool enhanced = false, bool depthWeighted = false,
             int depthWeightLoads = kDepthWeightLoads16);
    // np-look23 (c), with `enhanced` only (LookSettings::postDofDepthWeight):
    // the downsample weights each of the tent's 16 bilinear taps by the blur
    // factor squared at the heavier texel of its pair (premultiplied, the
    // weight in alpha, RGBA16Float quarter targets) and
    // the composite divides it back out, so an in-focus figure's colour no
    // longer spreads into the blurred background beside it (retail's bright
    // fringe, which Xenia and the hardware show). Retail and `enhanced`
    // without it: exactly as before.
    static constexpr wgpu::TextureFormat kDepthWeightFormat = wgpu::TextureFormat::RGBA16Float;
    // np-look23 (c2), inspection only (LookSettings::postDofDepthWeightLoads):
    // how the depth-weighted downsample gathers its weight, so each can be
    // timed. Depth loads per quarter texel:
    //   16  one per bilinear tap (fs_down_tent_dw, the default);
    //    4  one per 2x2 group of the taps, at the group's centre texel, shared
    //       by its four colour taps (fs_down_tent_dw4);
    //    1  one at the block's centre: the plain tent times that one weight
    //       (fs_down_tent_dw1);
    //    0  none: a half-size pre-pass writes the blur factor squared, one
    //       load per half-size texel (fs_dw_factor_half), and the downsample
    //       samples it bilinearly at each tap (fs_down_tent_dwh).
    // 4, 1 and 0 compile their pipelines on first use.
    static constexpr int kDepthWeightLoads16 = 16;
    static constexpr int kDepthWeightLoads4 = 4;
    static constexpr int kDepthWeightLoads1 = 1;
    static constexpr int kDepthWeightHalfFactor = 0;
    static constexpr wgpu::TextureFormat kDepthWeightFactorFormat = wgpu::TextureFormat::R16Float;
    // The gather the last depth-weighted Run took (tests).
    int LastRunDepthWeightLoads() const { return mLastDwLoads; }
    // Whether the last Run took the depth-weighted path (tests).
    bool LastRunDepthWeighted() const { return mLastDepthWeighted; }
    // The enhanced path's tap scale per axis: the tent adds 1.5 scene texel^2
    // (3/32 quarter texel^2) and the B-spline 1/6 quarter texel^2 of variance
    // over the box and bilinear, taken back out of the two Poisson passes'
    // variance `v` (quarter texel^2): sqrt(1 - (3/32 + 1/6) / v), in [0.5, 1].
    static float EnhancedTapScale(float v);
    const wgpu::TextureView& OutputView() const { return mOutView; }
    const wgpu::Texture& OutputTexture() const { return mOutTex; }
    // The composite's four constants (range, -scale * range, minBlur,
    // maxBlur) the last Run wrote: which camera's planes they came from is
    // the rndshape seam's choice (DofSetCamPlanes).
    const float* LastCompositeConstants() const { return mLastConstants; }
    void Terminate();
    // Compiles the three pipelines for a `format` scene now, so the first
    // Run() that finds the depth of field on does not (they used to compile on
    // the first song frame, with the spot beam and point-test pipelines:
    // docs/native/dc3-backend-for-rb3-wii.md section 38.2).
    // `enhanced` (LookSettings::postDofFilter at Init) also compiles the tent
    // downsample and B-spline composite.
    // `depthWeighted` (LookSettings::postDofDepthWeight too) also compiles
    // the depth-weighted downsample, blur and composite.
    void Prewarm(wgpu::TextureFormat format, GpuDevice& gpu, bool enhanced = false,
                 bool depthWeighted = false) {
        EnsurePipelines(format, gpu);
        if (enhanced) EnsureEnhancedPipelines(gpu);
        if (enhanced && depthWeighted) EnsureDepthWeightPipelines(gpu);
    }
    bool EnhancedReady() const { return (bool)mDownTentPipe && (bool)mCompositeBSplinePipe; }
    // Bind groups from `cache` (null: four created per run); dc3 PLAN H-66.
    void SetBindGroupCache(BindGroupCache* cache) { mBindGroups = cache; }

private:
    void EnsurePipelines(wgpu::TextureFormat format, GpuDevice& gpu);
    void EnsureEnhancedPipelines(GpuDevice& gpu);
    wgpu::RenderPipeline MakePipe(const wgpu::PipelineLayout& layout, const char* fs, const char* label,
                                  GpuDevice& gpu,
                                  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined);
    void EnsureDepthWeightPipelines(GpuDevice& gpu);
    // np-look23 (c2): the downsample pipeline for `loads` (compiled on first use).
    const wgpu::RenderPipeline& DepthWeightDownPipe(int loads, GpuDevice& gpu);
    void EnsureDepthWeightTextures(int w, int h, GpuDevice& gpu);
    void EnsureTextures(int w, int h, wgpu::TextureFormat format, GpuDevice& gpu);
    void Pass(wgpu::CommandEncoder& encoder, const wgpu::RenderPipeline& pipe,
              const wgpu::BindGroup& bg, const wgpu::TextureView& dst, const char* label);

    wgpu::BindGroup Group(wgpu::Device& dev, const wgpu::BindGroupDescriptor& d) {
        return mBindGroups ? mBindGroups->GetOrCreate(dev, d) : CreateCountedBindGroup(dev, &d);
    }
    BindGroupCache* mBindGroups = nullptr;
    wgpu::TextureFormat mFormat = wgpu::TextureFormat::Undefined;
    wgpu::ShaderModule mShader;
    wgpu::BindGroupLayout mPassBGL;       // src, sampler, offsets
    wgpu::BindGroupLayout mCompositeBGL;  // scene, blur, depth (MS), sampler, params
    wgpu::PipelineLayout mPassPL, mCompositePL;
    wgpu::RenderPipeline mDownPipe, mBlurPipe, mCompositePipe;
    wgpu::RenderPipeline mDownTentPipe, mCompositeBSplinePipe;   // `enhanced`: Prewarm or first use
    // np-look23 (c), `depthWeighted`: compiled on first use.
    wgpu::BindGroupLayout mDownDwBGL;     // src, sampler, depth (MS), params
    wgpu::PipelineLayout mDownDwPL;
    wgpu::RenderPipeline mDownDwPipe, mBlurDwPipe, mCompositeDwPipe;
    wgpu::Texture mDw[2];
    wgpu::TextureView mDwView[2];
    int mDwW = 0, mDwH = 0;
    bool mLastDepthWeighted = false;
    // np-look23 (c2): the 4-, 1- and half-factor gathers, on first use.
    wgpu::RenderPipeline mDownDw4Pipe, mDownDw1Pipe, mDownDwHPipe, mFactorHalfPipe;
    wgpu::BindGroupLayout mFactorHalfBGL;   // depth (MS), params
    wgpu::BindGroupLayout mDownDwHBGL;      // src, sampler, factor
    wgpu::PipelineLayout mFactorHalfPL, mDownDwHPL;
    wgpu::Texture mDwFactor;
    wgpu::TextureView mDwFactorView;
    int mDwFactorW = 0, mDwFactorH = 0;
    int mLastDwLoads = kDepthWeightLoads16;
    wgpu::Sampler mSampler;               // linear, clamp
    // One buffer per pass: Queue::WriteBuffer lands at Submit, ahead of every
    // pass in the frame, so passes sharing a buffer would read the same data.
    wgpu::Buffer mBlurUB[2];
    wgpu::Buffer mCompositeUB;

    wgpu::Texture mQuarter[2];
    wgpu::TextureView mQuarterView[2];
    wgpu::Texture mOutTex;
    wgpu::TextureView mOutView;
    int mW = 0, mH = 0;
    bool mWarnedSamples = false;
    float mLastConstants[4] = {};
};
