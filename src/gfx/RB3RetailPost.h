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
    int debugView = 0;            // 1: show the bloom mask, 2: show the bloom term,
                                  // 3: show the spotlight beam target
    // NgSpotlightDrawer's beams (postprocess.ps option bit 51), added after the
    // screen blend and before the colour transform:
    //   c += beam.rgb * (fog.r * spotSmoke + spotBase) * spotScale
    // spotBeams is gfx/SpotBeamPass's blurred target (null: no beams), spotFog
    // its tf5 fog density texture (null: black). See gfx/SpotBeamPass.h.
    wgpu::TextureView spotBeams;
    wgpu::TextureView spotFog;
    float spotScale = 0.0f;       // c91.x: SpotDrawParams::mIntensity * 32
    float spotBase = 0.0f;        // c127.x: mBaseIntensity / 100
    float spotSmoke = 0.0f;       // c127.y: mSmokeIntensity / 100 * (1 - c127.x)

    // ---- np-gfx-post: DC3 (rndshape::kDC3RetailPost) ----------------------
    // dc3-decomp's NgPostProc::DoBloom and DC3's postprocess.ps, which differ
    // from RB3's in order and in three effects. Left at these defaults, RB3's
    // chain is unchanged (separate shader module and pipelines).
    //   composite  scene (or chroma: red at (u - ox, v), green at
    //              (u + ox/2, v + 0.866 oy), blue at (u + ox/2, v - 0.866 oy);
    //              sharpen: 4 scene - those three taps), screen-blended with
    //              the bloom sets, then the texture noise (overlay into the
    //              UNCLAMPED screen result), then dp4 against the colour
    //              matrix, then the vignette:
    //                w = sat((|uv - 0.5|^2 + I) * 3.724 - 4.655)^2
    //                out = sat(c + w * (vignetteColor - c))
    //              No posterize (no shipped DC3 post-process sets it).
    //   streak     DoBloom's streak arm (mBloomStreak && !mBloomGlare): set0
    //              = bloom.ps into w/4, three 7-tap streak passes along the
    //              rotated horizontal; set1 = bloom.ps straight from the scene
    //              into w/16 (4 bilinear taps at +-2 scene texels, so it
    //              samples a quarter of each 16x16 block, as retail), three
    //              passes along the rotated vertical; set2 black.
    //              SetBloomBlurWeightsStreak (dc3-decomp rndobj/Utl.cpp).
    bool dc3 = false;
    bool streak = false;
    float streakAttenuation = 0.0f;   // mBloomStreakAttenuation
    float streakAngle = 0.0f;         // mBloomStreakAngle, degrees
    float screenAspect = 0.5625f;     // TheRnd.Height() / Width() (yRatio)
    // NgPostProc::CheckNoise: mNoiseMap on sampler 13 (null: no noise), c112
    // seeds, c113 = (baseScale.xy, topScale or 1 when stationary, intensity).
    wgpu::TextureView noiseMap;
    float noiseSeeds[4] = {0, 0, 0, 0};
    float noiseParams[4] = {0, 0, 0, 0};
    // NgPostProc::CheckMotionBlur (postprocess.ps bit 24): when
    // RndPostProc::DoMotionBlur() holds, c105 = (weight.rgb * blend,
    // weight.a) and the composite mixes in tf14, DxRnd's mPostProcessBuffer
    // (the previous frame's post-process output, resolved by
    // FinishPostProcess -> SavePostBuffer before the UI draws over it):
    //   r   = sat(c105.w * (prev.a - scene.a) / prev.a)      (0 when prev.a = 0)
    //   k   = max(r, c105.rgb)
    //   out = sat(c + (prev.rgb - c) * k)    c = the unclamped graded colour
    //   a   = sat(scene.a + r * (prev.a - scene.a))   (fed back as next prev.a)
    // Last in the shader, after the vignette. Off: no texture, no extra target,
    // the composite unchanged. On, the composite writes a second target that
    // the next frame reads (RB3RetailPost keeps two, ping-ponged).
    bool motionBlur = false;
    float motionBlurParams[4] = {0, 0, 0, 0};   // c105
    // ---- end np-gfx-post ----
};

class RB3RetailPost {
public:
    void Init(GpuDevice& gpu);
    // Grades `sceneView` (sceneW x sceneH) onto `frameView` (cleared, full
    // overwrite, surface format). Bloom is built from `bloomSrcView` (same
    // size), which differs from the scene only when depth of field ran.
    void Run(wgpu::CommandEncoder& encoder, const wgpu::TextureView& sceneView,
             const wgpu::TextureView& bloomSrcView,
             int sceneW, int sceneH, const wgpu::TextureView& frameView,
             const RetailPostParams& p, GpuDevice& gpu);
    // Draws `src` (frame-sized, frame format) over the whole current render
    // pass, whose attachments are `samples`-sampled with depth format
    // `depthFmt` (Undefined: no depth). Neither depth nor stencil is touched.
    void Blit(wgpu::RenderPassEncoder& pass, const wgpu::TextureView& src,
              uint32_t samples, wgpu::TextureFormat depthFmt, GpuDevice& gpu);
    void Terminate();

private:
    static constexpr int kSets = 3;
    void EnsurePipelines(GpuDevice& gpu);
    // np-gfx-post: the DC3 chain (RetailPostParams::dc3).
    void EnsureDC3Pipelines(GpuDevice& gpu);
    void RunDC3Bloom(wgpu::CommandEncoder& encoder, const wgpu::TextureView& bloomSrc,
                     int sceneW, int sceneH, const RetailPostParams& p, GpuDevice& gpu);
    void RunDC3Composite(wgpu::CommandEncoder& encoder, const wgpu::TextureView& sceneView,
                         int sceneW, int sceneH, const wgpu::TextureView& frameView,
                         const RetailPostParams& p, GpuDevice& gpu);
    void PassDC3(wgpu::CommandEncoder& encoder, const wgpu::RenderPipeline& pipe,
                 const wgpu::TextureView& dst, const wgpu::TextureView& src,
                 const float* uni, size_t uniBytes, GpuDevice& gpu);
    void EnsureTextures(int sceneW, int sceneH, GpuDevice& gpu);
    void Pass(wgpu::CommandEncoder& encoder, const wgpu::RenderPipeline& pipe,
              const wgpu::TextureView& dst, const wgpu::TextureView& src,
              const float* uni, size_t uniBytes, GpuDevice& gpu);

    wgpu::Texture mTex[kSets][2];
    wgpu::TextureView mView[kSets][2];
    int mW[kSets] = {}, mH[kSets] = {};
    // The bloom sets' format. RB3: RGBA16Float. DC3 (np-gfx-post): RGBA8Unorm,
    // as NgPostProc::BloomTextureSet::AllocateTextures makes them
    // (SetBitmap(w, h, TheRnd.Bpp() = 32, kRenderedNoZ): A8R8G8B8), so every
    // bloom pass saturates at 1 as retail's EDRAM resolve does. Switched by
    // EnsureDC3Pipelines, which rebuilds the pipelines and sets that use it.
    wgpu::TextureFormat mBloomFormat = wgpu::TextureFormat::RGBA16Float;

    wgpu::ShaderModule mShader;
    wgpu::BindGroupLayout mPassBGL;       // src tex, sampler, uniform
    wgpu::BindGroupLayout mCompositeBGL;  // scene, sampler, uniform, set0..2, beams, fog
    wgpu::PipelineLayout mPassPL, mCompositePL;
    wgpu::RenderPipeline mBloomDownPipe;  // scene -> set0 (rgb * mask)
    wgpu::RenderPipeline mDown4Pipe;      // set k -> set k+1
    wgpu::RenderPipeline mBlurPipe;       // 15-tap separable
    wgpu::RenderPipeline mCompositePipe;
    wgpu::RenderPipeline mBlitPipe;       // fs_copy, keyed on samples + depth
    uint32_t mBlitSamples = 0;
    wgpu::TextureFormat mBlitDepth = wgpu::TextureFormat::Undefined;
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

    // np-gfx-post: DC3 module (kDC3Shader), its own layouts and pipelines.
    wgpu::ShaderModule mDC3Shader;
    wgpu::BindGroupLayout mDC3PassBGL;        // src tex, sampler, taps uniform
    wgpu::BindGroupLayout mDC3CompositeBGL;   // scene, sampler, uniform, set0..2, noise, noise sampler
    wgpu::PipelineLayout mDC3PassPL, mDC3CompositePL;
    wgpu::RenderPipeline mDC3BloomTapsPipe;   // bloom.ps, 4 bilinear taps (the w/16 streak set)
    wgpu::RenderPipeline mDC3StreakPipe;      // 7-tap streak blur
    wgpu::RenderPipeline mDC3CompositePipe;
    wgpu::RenderPipeline mDC3CompositeMBPipe; // + the motion-blur feedback target
    // NgPostProc::CheckMotionBlur's tf14 (DxRnd::mPostProcessBuffer, A8R8G8B8):
    // the composite reads one and writes the other. Allocated on the first
    // motion-blur frame only. mPrevRun is the mRunIndex that last wrote
    // mPrev[mPrevCur]; the feedback is valid only on the very next Run.
    wgpu::Texture mPrevTex[2];
    wgpu::TextureView mPrevView[2];
    int mPrevW = 0, mPrevH = 0, mPrevCur = 0;
    uint64_t mRunIndex = 0, mPrevRun = ~0ull;
    wgpu::Sampler mNoiseSampler;             // linear, wrap (CheckNoise: filter 1, clamp 0)
    wgpu::TextureFormat mDC3FrameFormat = wgpu::TextureFormat::Undefined;
    bool mDC3Ready = false;
};
