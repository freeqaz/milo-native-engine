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
             bool enhanced = false, bool depthWeighted = false, bool nearField = false,
             bool nearSpread = false, bool edge = false);
    // np-look51, with `enhanced` only (LookSettings::postDofEdge, the edge
    // rule): the composite that runs reads every depth sample of the pixel
    // (depthSamples) for its mix amount, not sample 0 alone: the smallest
    // factor of all the samples, raised to the largest of those in front of
    // the focus (and the near field, with
    // `nearField`, the largest of the samples'). So a pixel an in-focus figure
    // partly covers keeps its resolved anti-aliased colour, where sample 0
    // alone kept it whole or replaced it whole by the blur (a one-sample
    // stair-stepped outline in close-ups), and a pixel a blurred near object
    // touches stays blurred. A pixel whose samples share one depth gets
    // today's value. A separate entry point per composite
    // (..._edge, DofComposite...Edge), so with `edge` off the composite that
    // runs is the one before np-look51; the retail composite never takes it.
    // np-look36, with `depthWeighted` only (LookSettings::postDofNearField):
    // the depth weight keeps an in-focus figure out of the blur on BOTH sides
    // of the focal plane, so an out-of-focus object in front of a dancer kept
    // a hard cut-out edge. The factor pre-pass also writes the near field
    // (out of focus, in front of the focus; G), the downsample writes a second
    // quarter target (the depth-blind tent, the near coverage in alpha), the
    // blurs carry both, and the composite takes the depth-blind blur where the
    // centre is in the near field. `nearSpread` (postDofNearSpread, a uniform)
    // also mixes the near object's blur over the pixels beside it by the
    // blurred near coverage. Behind the focus and in focus: the depth-weighted
    // result to the pixel.
    // np-look23 (c), with `enhanced` only (LookSettings::postDofDepthWeight):
    // a half-size pre-pass writes the blur factor squared (one depth load per
    // half-size texel, R16Float; DofDepthWeightFactorHalf), the downsample
    // weights each of the tent's 16 bilinear taps by it, sampled bilinearly
    // at the tap (premultiplied, the weight in alpha, RGBA16Float quarter
    // targets; DofDownsampleTentDepthWeightHalf), and the composite divides it
    // back out, so an in-focus figure's colour no longer spreads into the
    // blurred background beside it (retail's bright fringe, which Xenia and
    // the hardware show). A depth load per tap in the downsample itself cost
    // +0.84 ms at 1920x1080 on a Radeon 890M (np-look23 c2); this +0.25 ms.
    // Retail and `enhanced` without it: exactly as before.
    static constexpr wgpu::TextureFormat kDepthWeightFormat = wgpu::TextureFormat::RGBA16Float;
    // The half factor target: the factor squared (R16Float), and with the near
    // field (np-look36) the near field beside it (RG16Float). Without the near
    // field the pre-pass, its target and every pass after it are exactly what
    // they were before np-look36: the same shaders, formats and reads.
    static constexpr wgpu::TextureFormat kDepthWeightFactorFormat = wgpu::TextureFormat::R16Float;
    static constexpr wgpu::TextureFormat kDepthWeightFactorNearFormat = wgpu::TextureFormat::RG16Float;
    // Whether the last Run took the depth-weighted path (tests).
    bool LastRunDepthWeighted() const { return mLastDepthWeighted; }
    bool LastRunNearField() const { return mLastNearField; }
    bool LastRunEdge() const { return mLastEdge; }
    // The half factor target's own format, as the device made it (tests).
    wgpu::TextureFormat FactorTargetFormat() const {
        return mDwFactor ? mDwFactor.GetFormat() : wgpu::TextureFormat::Undefined;
    }
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
    // the depth-weighted factor pre-pass, downsample, blur and composite.
    // `nearField` (postDofNearField too): the near field's three as well.
    // `edge` (postDofEdge, with `enhanced`): the edge rule's composite for the
    // same path.
    void Prewarm(wgpu::TextureFormat format, GpuDevice& gpu, bool enhanced = false,
                 bool depthWeighted = false, bool nearField = false, bool edge = false) {
        EnsurePipelines(format, gpu);
        if (enhanced) EnsureEnhancedPipelines(gpu);
        if (enhanced && depthWeighted) EnsureDepthWeightPipelines(gpu);
        if (enhanced && depthWeighted && nearField) EnsureNearPipelines(gpu);
        if (enhanced && edge) EnsureEdgePipelines(gpu, depthWeighted, depthWeighted && nearField);
    }
    bool EnhancedReady() const { return (bool)mDownTentPipe && (bool)mCompositeBSplinePipe; }
    // Bind groups from `cache` (null: four created per run); dc3 PLAN H-66.
    void SetBindGroupCache(BindGroupCache* cache) { mBindGroups = cache; }

private:
    void EnsurePipelines(wgpu::TextureFormat format, GpuDevice& gpu);
    void EnsureEnhancedPipelines(GpuDevice& gpu);
    wgpu::RenderPipeline MakePipe(const wgpu::PipelineLayout& layout, const char* fs, const char* label,
                                  GpuDevice& gpu,
                                  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined,
                                  uint32_t targets = 1);
    void EnsureNearPipelines(GpuDevice& gpu);
    void EnsureEdgePipelines(GpuDevice& gpu, bool depthWeighted, bool nearField);
    void EnsureNearTextures(int w, int h, GpuDevice& gpu);
    void EnsureDepthWeightPipelines(GpuDevice& gpu);
    void EnsureDepthWeightTextures(int w, int h, GpuDevice& gpu, bool nearField = false);
    void EnsureTextures(int w, int h, wgpu::TextureFormat format, GpuDevice& gpu);
    void Pass(wgpu::CommandEncoder& encoder, const wgpu::RenderPipeline& pipe,
              const wgpu::BindGroup& bg, const wgpu::TextureView& dst, const char* label,
              const wgpu::TextureView* dst2 = nullptr);

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
    // np-look23 (c), `depthWeighted`: Prewarm or first use.
    wgpu::BindGroupLayout mFactorHalfBGL;   // depth (MS), params
    wgpu::BindGroupLayout mDownDwBGL;       // src, sampler, factor
    wgpu::PipelineLayout mFactorHalfPL, mDownDwPL;
    wgpu::RenderPipeline mFactorHalfPipe, mDownDwPipe, mBlurDwPipe, mCompositeDwPipe;
    wgpu::Texture mDw[2];
    wgpu::TextureView mDwView[2];
    int mDwW = 0, mDwH = 0;
    bool mLastDepthWeighted = false;
    wgpu::Texture mDwFactor;                // the half-size factor, ((w+1)>>1) x ((h+1)>>1)
    wgpu::TextureView mDwFactorView;
    int mDwFactorW = 0, mDwFactorH = 0;
    // np-look36, `nearField`: Prewarm or first use.
    wgpu::BindGroupLayout mBlurNearBGL;      // src, sampler, offsets, src2
    wgpu::BindGroupLayout mCompositeNearBGL; // the composite's + the near quarter
    wgpu::PipelineLayout mBlurNearPL, mCompositeNearPL;
    wgpu::RenderPipeline mFactorHalfNearPipe, mDownNearPipe, mBlurNearPipe, mCompositeNearPipe;
    wgpu::Texture mNear[2];
    wgpu::TextureView mNearView[2];
    int mNearW = 0, mNearH = 0;
    bool mLastNearField = false;
    // Whether the half factor target was made with the near field's channel.
    bool mDwFactorNear = false;
    // np-look51, `edge`: the edge rule's composite per path, Prewarm or first use.
    wgpu::RenderPipeline mCompositeBSplineEdgePipe, mCompositeDwEdgePipe, mCompositeNearEdgePipe;
    bool mLastEdge = false;
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
