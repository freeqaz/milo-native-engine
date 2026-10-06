// RB3RetailPost — Rock Band 3's Xbox 360 post-process chain (bloom + colour
// transform), for RB3 content on the dc3 backend.
//
// Ported from the retail shaders, not approximated: the shader container
// `xbox_shaders` (XOBX v1, parsed per DxShaderMgr::LoadShaderFile) was
// disassembled with xenia-gpu-shader-compiler. The pieces, and the NgPostProc
// code that drives them (rb3-xenon rndobj/PostProc_NG.cpp, DoBloom /
// SetBloomColor / ModulateColorXfm):
//
//   bloom mask   standard.ps with mPseudoHDR (bit 22, set for materials whose
//                NgMat::AllowHDR() holds) writes  a = dot(rgb, c7.rgb)  where
//                c7 = (0.3, 0.59, 0.11) * s, s = threshold > 1 ? 1/threshold : 1
//   bloom.ps     4 taps of the scene, each rgb * a, averaged; into a target a
//                quarter of the scene in each axis
//   blur         15 taps, offsets -6.5 .. 7.5 texels, Gaussian weights
//                (SetBloomBlurWeights), horizontal then vertical
//   downsample_4x  4x4 box, into the next set (another quarter per axis); three
//                sets in all (w/4, w/16, w/64)
//   postprocess.ps (bloom bit 4, colour-xfm bit 21):
//                b   = tex(set0) + tex(set1) + tex(set2)
//                c   = 1 - (1 - b * c6) * (1 - scene)        screen blend
//                out = sat(dp4(c92..c94, (c, 1)))           colour transform
//                with c6 = bloomColor.rgb * BloomIntensity() and c92..94 the
//                RndColorXfm matrix (3x3 scaled by the flicker modulation).
//
// The scene's alpha IS the bloom mask, as on retail: the dc3 standard shader
// writes the luma mask for the materials rndshape::BloomMaskScale admits, every
// other draw leaves alpha alone, and the frame clears it to 0.
//
// Rndobj-free: the caller (gfx/PostProcPass, through rndshape::FillRetailPost)
// fills RetailPostParams from the consumer's RndPostProc.
#pragma once
#include <webgpu/webgpu_cpp.h>

class GpuDevice;

struct RetailPostParams {
    bool bloom = false;           // NgPostProc::DoBloom's doBloom
    float bloomColor[3] = {0, 0, 0};   // c6: bloom colour * BloomIntensity()
    float bloomLumaScale = 1.0f;  // c7 scale: threshold > 1 ? 1/threshold : 1
                                  // (applied in the scene pass; informational here)
    // Colour transform, rows of the 3x4 matrix applied as out = M * (rgb, 1)
    // (identity when the postproc has no colour adjustment).
    float xfm[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    // Effects kept from the generic composite (gfx/PostProcPass), applied after
    // the transform.
    float vignetteIntensity = 0.0f;
    float vignetteColor[4] = {0, 0, 0, 0};
    float posterLevels = 0.0f;
    float posterMin = 1.0f;
    float chromaticOffset = 0.0f;
    float chromaticSharpen = 0.0f;
    float noiseIntensity = 0.0f;
    float noiseMidtone = 0.0f;
    float time = 0.0f;
    int debugView = 0;            // 1: show the bloom mask, 2: show the bloom term
};

class RB3RetailPost {
public:
    void Init(GpuDevice& gpu);
    // Grades `sceneView` (sceneW x sceneH) onto `frameView` (cleared, full
    // overwrite, surface format).
    void Run(wgpu::CommandEncoder& encoder, const wgpu::TextureView& sceneView,
             int sceneW, int sceneH, const wgpu::TextureView& frameView,
             const RetailPostParams& p, GpuDevice& gpu);
    void Terminate();

private:
    static constexpr int kSets = 3;
    void EnsurePipelines(GpuDevice& gpu);
    void EnsureTextures(int sceneW, int sceneH, GpuDevice& gpu);
    void Pass(wgpu::CommandEncoder& encoder, const wgpu::RenderPipeline& pipe,
              const wgpu::TextureView& dst, const wgpu::TextureView& src,
              const float* uni, size_t uniBytes, GpuDevice& gpu);

    wgpu::Texture mTex[kSets][2];
    wgpu::TextureView mView[kSets][2];
    int mW[kSets] = {}, mH[kSets] = {};

    wgpu::ShaderModule mShader;
    wgpu::BindGroupLayout mPassBGL;       // src tex, sampler, uniform
    wgpu::BindGroupLayout mCompositeBGL;  // scene, sampler, uniform, set0..2
    wgpu::PipelineLayout mPassPL, mCompositePL;
    wgpu::RenderPipeline mBloomDownPipe;  // scene -> set0 (rgb * mask)
    wgpu::RenderPipeline mDown4Pipe;      // set k -> set k+1
    wgpu::RenderPipeline mBlurPipe;       // 15-tap separable
    wgpu::RenderPipeline mCompositePipe;
    wgpu::Sampler mSampler;               // linear, clamp
    // One 256-byte slot per pass. Queue::WriteBuffer runs at Submit, ahead of
    // the frame's passes, so passes sharing a slot would all read the last data.
    static constexpr int kSlots = 64;
    static constexpr int kSlotStride = 256;
    wgpu::Buffer mUniforms;
    int mSlot = 0;
    wgpu::Texture mBlackTex;
    wgpu::TextureView mBlackView;
    wgpu::TextureFormat mFrameFormat = wgpu::TextureFormat::Undefined;
    bool mReady = false;
};
