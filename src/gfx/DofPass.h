#pragma once
#include <webgpu/webgpu_cpp.h>

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

    // When TheDOFProc is enabled: blurs `sceneView` (sceneW x sceneH, format
    // `sceneFormat`) by `depthView` (a depth-only view of the scene depth,
    // `depthSamples` samples, same size) into OutputView() and returns true.
    // Otherwise returns false and records nothing. The scene is not modified.
    bool Run(wgpu::CommandEncoder& encoder, const wgpu::TextureView& sceneView,
             wgpu::TextureFormat sceneFormat, const wgpu::TextureView& depthView,
             uint32_t depthSamples, int sceneW, int sceneH, GpuDevice& gpu);
    const wgpu::TextureView& OutputView() const { return mOutView; }
    const wgpu::Texture& OutputTexture() const { return mOutTex; }
    void Terminate();

private:
    void EnsurePipelines(wgpu::TextureFormat format, GpuDevice& gpu);
    void EnsureTextures(int w, int h, wgpu::TextureFormat format, GpuDevice& gpu);
    void Pass(wgpu::CommandEncoder& encoder, const wgpu::RenderPipeline& pipe,
              const wgpu::BindGroup& bg, const wgpu::TextureView& dst, const char* label);

    wgpu::TextureFormat mFormat = wgpu::TextureFormat::Undefined;
    wgpu::ShaderModule mShader;
    wgpu::BindGroupLayout mPassBGL;       // src, sampler, offsets
    wgpu::BindGroupLayout mCompositeBGL;  // scene, blur, depth (MS), sampler, params
    wgpu::PipelineLayout mPassPL, mCompositePL;
    wgpu::RenderPipeline mDownPipe, mBlurPipe, mCompositePipe;
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
};
