# DC3's retail light model on the native port (lane np-gfx-light, 2026-11-14)

This page describes how the Xbox 360 image lights a DC3 scene, and how the
engine's DC3 shape now reproduces it. It replaces the heuristic that
`WgpuRnd::WriteSceneUniforms` used for DC3 (`else if (env &&
EnvHasAmbientFogOwner(env))`): a 0.08 ambient floor, light priority by name,
the top 4 lights by brightness, point lights turned into directionals aimed at a
guessed scene centre, synthetic fill lights, and a total-energy cap. That arm
is still in the file for any consumer that reaches it. DC3 no longer reaches
it, and rb3-xenon never did change path (section 6).

Every claim carries a tag:

- **MEASURED** means it was read from matched dc3-decomp source (the function
  is named, 100% unless stated otherwise), from a shipped `xbox_shaders`
  permutation disassembled with xenia's microcode disassembler, or from
  shipped data.
- **INFERRED** means it was reasoned from those and not observed directly.

The disassemblies (`standard_<mask>.{vs,ps}.txt`, with the literal pool in
`.lit`) are in dc3-decomp `archive/np-gfx-2026-11-14/lanes/light/shaders/standard/`.
That directory holds the whole shipped `standard` cache, not only the
permutations cited here.

## 1. Which lights reach a draw

| what | retail | tag | source |
|---|---|---|---|
| ambient (c1) | The environ's ambient, unscaled. A material with `use_environ` off gets (1,1,1,1). | MEASURED | `NgMat::SetupAmbient`; permutation 0x10 draws `texel * c0 * c1` |
| real point lights | The **first three** `kPoint` lights of `mLightsReal`, in list order, that are Showing and have a non-zero packed colour. c62+i = (pos, 1/(falloffStart − range)), c66+i = (rgb, −range·that). Or (pos, 0) and (rgb, 1) when falloffStart ≥ range. So the light is full strength inside falloffStart and falls linearly to zero at range. | MEASURED | `NgEnviron::Select` / `SetPointLightRegisters` (Env_NG.cpp; Select is 98.6%, the residual is store order) |
| projected light | At most **one**: the first `kFakeSpot` of `mLightsReal` that `CheckProjLight` accepts goes to slot 3. c65 = −m.y, c69 = (rgb, 1), c95..c97 = `RndLight::Projection()` (4x3), tf5 = shadow map, tf10 = gobo (blend 0) or white-transparent (blend 1). | MEASURED | `NgEnviron::Select`, `SetProjLightRegisters`, `CheckProjLight` |
| approx lights | Folded into a six-face box map c80..c85 at the position `Select` was given, by `NgEnviron::UpdateApproxLighting` from `mLightsApprox` + `RndEnviron::sGlobalLighting`. Directionals and fake spots always land. Points and spots land only when there is a position: a Character passes its origin (`Character::DrawLodOrShadow`'s `RndEnvironTracker`), a WorldDir passes none. | MEASURED | BoxMap.cpp, Env_NG.cpp |
| directional "real" lights | None. `Select` notifies "Invalid real light" for anything that is neither a point nor a fake spot. | MEASURED | `NgEnviron::Select` |
| fill lights, ambient floor, energy cap, name priority | **None exist in retail.** | MEASURED (absence: none of the register writers has such a step) | as above |

**Native.** `rndshape::WriteSceneLighting` (DC3SceneLighting.cpp) writes
retailLighting 2: ambient, three points and fog. The native port already
runs DC3's own `NgEnviron::Select` and `UpdateApproxLighting`, with
`mAllowPerPixel = 1`, so the box map it uploads is retail's.
`WgpuShaderMgr` now keeps Vector4 vertex constants. Per draw,
`FillMeshApproxLighting` copies c80..c85 into `ObjectUniforms.boxLight` and
`NumLights_Approx` into `retail.y`.

## 2. How a standard draw evaluates them, and in which space

| what | retail | tag | source |
|---|---|---|---|
| shading space | Gamma space. Texels are used as stored (no gamma-signed bitmap format in `D3DFormatForBitmap`), the target is `D3DFMT_A8R8G8B8`, and no permutation decodes or encodes. The product saturates at the 8-bit write. | MEASURED | rnddx9/Rnd.cpp:351-373; Rnd_Xbox.cpp:531-562, 853-881; all permutations listed here |
| display | One gamma ramp at scan-out, `pow(i/256, g)` with `g = 0.85` from `(rnd (gamma 0.85))`. | MEASURED | `DxRnd::SetupGamma`; `system/run/config/default.dta:292` |
| diffuse | `c0 · (c1 [· vertex for prelit+environ] + box(N) + Σ points) · texel`. box(N) = Σ max(±N_axis, 0) · face, linear in each axis, from 0x100010030011 lines 13-24 (`mul_sat` of N̂ for the positive half, `maxs_sat` of −N̂ for the negative half, then one `mad r, n.axis, c80+face, ...` per face). Points use N·L with the linear falloff. | MEASURED | 0x20011, 0x30011, 0x100010030011 |
| per vertex vs per pixel | Bit 0 (`per_pixel_lit`) moves the same sum into the pixel shader. `AllowPerPixel` is set. | MEASURED | `RndShaderStandard::CalcShaderOpts` |
| specular | Bit 2 = `SpecularRGB().Pack() != 0`. c2 = (rgb, max(a, 0.5)). An energy-normalised Phong lobe on R over the box faces and the points (same as RB3's `retailSpecular`). | MEASURED (gate and c2); the lobe was INFERRED identical to RB3's, which was measured on RB3's cache | CalcShaderOpts, `NgMat::SetRegularShaderConst` |
| rim | Bit 37 with per-pixel lighting, c61 = (rgb, max(a, 0.5)). `r = pow((1 − sat(N·V))(0.5 N.z + 0.5), p) · rimColour · box(−V)`; out = `diffuse · (1 + 0.3 r) + 0.7 r`. | MEASURED | 0x2000030015 (literals 0.7 / 0.3) |
| environment map | Bits 3 and 43 (falloff). `(texel + env.rgb · env.a · w) · c0 · lighting`, R = 2(N·V)N − V, fetched as (R.x, R.z, R.y). w = 1, or 1 − sat(N·V) with falloff. Added to the texel **before** lighting. | MEASURED | 0x30018, 0x30019, 0x80000030019 (`cube r1, r1.yyxz, r1.zxyy`) |
| env spec mask | Bit 49 needs a spec map. Assumed to multiply w by `spec.a`. | INFERRED (no shipped permutation disassembled with it) | |
| emissive / glow | `oC0 = texel · (c1c0 + c0 · box) + emissive.rgb · c5.x`. c5.x = `mEmissiveMultiplier`, untinted. | MEASURED | 0x30091 |
| fog | Bit 18. `f = sat(2^(−2.88539 · clip.w · c91.y))`, c91 = (end, 1/(end − start), 0, 1), out = mix(c90, colour, f). Exponential in view depth, not linear in distance. | MEASURED | 0x70010 (vs + ps) |
| AO | Needs `!prelit && UseAO && env AOEnabled && strength > 0.003`. The shipped cache has the AO bit only on billboard permutations (e.g. 0x4002000000), none with a diffuse map. | MEASURED (cache contents), INFERRED (therefore off for every lit DC3 mesh) | |
| ambient alpha | c1.w multiplies output alpha (`muls_prev r0.w, c1.w`). | MEASURED | 0x100010030011 line 25 |

**Native.** `kGammaSpaceShading` is now on for DC3. `MatUnlit`,
`MatPrelitAmbient` and `MatRetailTerms` follow CalcShaderOpts.
`standard_wgsl.inc` adds the env map to the texel under retailLighting 2 and
skips the legacy rim and reflection adds there. The fog takes the
exponential form under `fogEnabled 2`. With `gammaShading` the output is
clamped to [0, 1]: no `compressHighlights` shoulder, no `softClipLighting`,
and no linear-to-sRGB encode, all of which retail lacks. The scan-out ramp
(0.85) is not part of the shading. Since np-gfx-post (`b3b1d66`;
dc3-backend-for-rb3-wii.md section 31), `rndshape::DisplayGamma` returns the
system config's 0.85 for dc3-decomp, and `WgpuRnd::EndDrawing` applies it once
after the UI on every output, headless included. Native captures are therefore
post-ramp, like retail hardware captures; `DC3_DISPLAY_GAMMA=off` gives the
pre-ramp frame. rb3-xenon still gets 0, so no ramp. Captures quoted in this
document before that change are pre-ramp.

## 3. Dancer floor shadows (projected_blend 1)

| what | retail | tag | source |
|---|---|---|---|
| which light | A `kFakeSpot` in the environ's real lights with `projected_blend 1` (shadow_projected), a non-zero colour, and shadow objects (`mShadowOverride`, else `mShadowObjects`). | MEASURED | `CheckProjLight` |
| the map | `NgLight::CheckShadowMap` (once per `DrawCount`, `AllowPerPixel`, `kFakeSpot`) creates two 256x256 `kRenderedNoZ` targets. `RenderShadows` clears to (0,0,0,0), sets VS c4 = the cone projection (`SetShadowTransforms`) and draws each caster that passes `SphereConeTest` in `kDrawOcclusion`. `BlurShadowRT` then runs two 5-tap passes with weights 0.1, 0.25, 0.3, 0.25, 0.1 at one-texel steps, horizontal then vertical. | MEASURED | Lit_NG.cpp |
| the receiver | Materials with `recv_proj_lights`. `CalcShaderOpts` sets `mNumProj` and `mProjLightMultiply` (blend 1). | MEASURED | Shader.cpp:720-726 |
| the maths | `uv = (c95·p, c96·p)/(c97·p)`, `s = tf5(uv).a`, 0 outside (border colour off). `oC0 = (c1c0 + box·c0·(1 − 0.75 · s · c69 · sat(c65·N))) · texel`. Ambient is not shadowed; the factor is per channel. | MEASURED | 0x100010030011 lines 3-27, c255 = (0, 1, 0.75, 0) |
| additive gobo (blend 0) | `light = (1 − shadow.a) · gobo · c69 · sat(L·N)` added to the box term. | MEASURED | 0x10030011 |

**Why native showed none.** The native shader manager discarded every matrix
constant, so `RenderShadows`' VS c4 never reached a draw. Whatever the
occlusion pass drew did not land in a map any receiver read: no receiver
sampled tf5, and the engine had no projected-light term for DC3. The generic
`ShadowPass` (an ortho depth map from the first directional, over every mesh
of the environ's dir) has nothing to do with retail. DC3's real lights hold
no directionals, so it rendered the whole venue into a map from a default
light, and the retail path never read it.

**Native now.**

1. `WriteDC3ProjShadow` finds the same light as `Select` does and builds a
   clip transform from `Projection()`'s rows r0, r1, r2:
   `(2 r0 − r2, r2 − 2 r1, 0, r2)`. The rasteriser lands each caster texel
   where a receiver's `(r0·p, r1·p)/(r2·p)` reads it. The transform goes into
   `SceneUniforms.lightViewProj`, with `shadowEnabled = 2` once a map for this
   light exists.
2. `WgpuRnd::EndDrawing` calls `CollectProjShadowCasters` and draws the
   light's casters into `ShadowPass`'s depth map at depth 0 over a clear of 1.
   This is a coverage map, which is what retail's `kRenderedNoZ` target is.
   The next frame's receivers sample it. Pointers are never kept across
   frames.
3. `dc3ProjLight` (standard_wgsl.inc) takes nine compare taps 1.5 texels of
   256 apart with (1 2 1)² weights. That approximates the two 5-tap passes
   (σ 1.06 vs 1.14 texels, INFERRED-equivalent), and the shader applies the
   factor to the box term.
4. The generic `ShadowPass::Render` pre-pass no longer runs for DC3
   (`rndshape::kGenericShadowPass`).

### The additive gobo (projected_blend 0)

| what | retail | tag | source |
|---|---|---|---|
| tf10 | The light's `GetTexture()` (the gobo), sampled at the same uv, with clamp mode 6 and a black border, so it is zero outside the cone. | MEASURED | `SetProjLightRegisters`; 0x10030011 fetch constants |
| tf5 when there are no shadow objects | `TheRnd.GetDefaultTex(kDefaultTex_FlatNormal)`. `CreateDefaultTexture` row 3 is (FF, FF, FF, 00), so alpha 0 and s = 0: the gobo is unshadowed. | MEASURED | Env_NG.cpp, Rnd.cpp |
| tf5 border | White (s = 1) for blend 0, black (s = 0) for blend 1. | MEASURED | the two permutations' fetch constants |
| the term | `(1 − s) · tf10(uv).rgb · c69 · sat(c65 · N̂)` added to the box term, inside `texel · c0 · (...)`. | MEASURED | 0x10030011 |
| shipped receivers | 385 of the cache's 4600 `standard` permutations have the projected-light bit. 192 of them are the multiply form. **None** is vertex-lit: per_pixel_lit is set on every one. | MEASURED (cache census) | `archive/.../shaders/standard/` |

**Native.** `WriteDC3ProjShadow` takes both blends. It sets
`projLightColor.w` to 1 (multiply) or 2 (additive) and keeps the gobo, which
`WgpuRnd` binds as scene binding 3 (`rndshape::SceneProjTexture()`). Lights
without shadow objects get no map, so s = 0 as in retail.

### What the venues hold (measured in a native run)

| venue | environ | projected light | blend | casters |
|---|---|---|---|---|
| streetside | `default_environment.env` | `a2_spotlight.lit`, kFakeSpot, with a texture, range 300, −y = (0.31, −0.13, 0.94) | 1 | 5 shadow objects → 87-106 meshes. The receivers are the road (`Street.mat`), the cardboard and the boombox (`boomy_main.mat`). |
| rollerrink | `FrontLeft.env` | `mirrorball_spotlight_01.lit`, colour 1.99, gobo | 0 | none |
| rollerrink | another environ | `stage_spotlight.lit`, colour 3.0 | 0 | 5 shadow objects |

On the rink, the gobo was the missing half of the floor's light. Without it
the floor measured luma 55, against retail frames at 105-136 (section 5).

**Not done.**

- The receiver's vertex-lit form scales the whole interpolated light. That is
  INFERRED, and it does not matter in practice: the census above has no
  vertex-lit receiver, so it is a native-only fallback.
- `SphereConeTest` culling of casters is skipped. It only removes casters that
  cannot land in the map.
- The casters are drawn twice per frame. DC3's own `NgLight::RenderShadows`
  still runs into its own render target, which nothing samples, alongside
  the native coverage pass. That is wasted work, not a visible difference.

## 4. Skin and hair (left for a follow-up lane)

Under retailLighting 2, skin and hair materials take `retailShade` like
everything else, which is what CalcShaderOpts does for a plain standard draw.
The dedicated variations were disassembled and saved:
`standard_0000000040020011` (shader variation 1, skin) and
`standard_0000000080020015` (variation 2, hair). Their fragment maths is not
implemented here.

## 5. Results

Captures come from `scripts/gpu/look_capture.sh` (song omg, 45 s in, every
cell gameplay = yes). Before is the baseline set at engine 99ee167 / dc3
29c8bdc3d. After is this branch: the table below is from e24e553, and 82a6b89 re-measured within 1-4 luma (rink 134.1 / 0.315, street 76.6 / 0.358). The sheets are in dc3-decomp
`archive/np-gfx-2026-11-14/lanes/light/sheets/`
(`before_after_{normal,peak}.png`). Retail frames are third-party footage and
appear only in a local sheet that is not to be shared.

The floor numbers below are the lower 40% and left 70% of the frame. Headless
captures carry no display ramp, and the retail footage is post-ramp, so each
side's own mean is quoted.

| venue (normal state) | before luma / sat | after luma / sat | retail luma / sat |
|---|---|---|---|
| rollerrink | 97.6 / 0.243 | 133.1 / 0.317 | 121-136 / 0.37-0.42 (frames 01, 02) |
| streetside | 133.4 / 0.220 | 80.2 / 0.355 | 86-88 / 0.41-0.49 (frames 02, 08) |

Against the acceptance checks:

1. **Peak streetside crowd as black silhouettes: yes.** The crowd now reads
   as silhouettes against the neon skyline. Before, it was a lit crowd.
2. **Normal streetside, soft dark shadows under the dancers and the boombox:
   yes.** They are visible on the cardboard (the lane archive's
   `evidence/streetside_shadow_zoom_feet.png`). On the dark road they are faint, as they are in retail.
   Debugging confirmed that the receivers see full coverage, with a box
   factor of 0.29.
3. **Rink with a cream floor, saturated carpet and a pool of light: the floor
   and the pool, yes.** That came from the blend-0 gobo. Saturation is still
   about 20% short of retail.

**Frame time.** Measured with `native_cpu_profile.py` (np-cpu-perf): headless,
YMCA in the rollerrink, sampled 30 s into gameplay for 10 s. Base and this
branch were run alternately, twice each, on an RTX 3090. Not measured on the
Radeon 890M or a phone. The main thread is CPU-bound in both builds (99%).

| build | gameplay fps | main-thread ms/frame |
|---|---|---|
| base (99ee167) | 119.3, 124.6 | 8.33, 7.97 |
| this branch | 104.2, 107.4 | 9.53, 9.24 |

That is about **+1.2 to 1.3 ms of CPU per frame**. Menu numbers are not
comparable: shader compilation (`[vkrt] Analysis` at 38-75%) dominated the
base runs' menu samples.

Where the time goes is INFERRED and not profiled. The caster pass goes
through `DrawMeshShadow` (Mesh_Wgpu.cpp:431). For every caster mesh, about
100 here, it does an ObjectUniforms ring write, a `CreateBindGroup` and, for
skinned meshes, `FillBoneUniforms` plus a second ring write and bind group.
Caching the bind groups with dynamic offsets is the obvious next step.
DC3's own `NgLight::RenderShadows` also still draws the same casters into a
target nothing samples (section 3).

**Remaining gap.**

- Skin is darker and greyer than retail's warm tan. Skin and hair use
  dedicated shader variations (section 4) that this lane did not implement.
- Overall saturation is still below retail.
- Venues other than these two were checked by eye only.

## 6. Consumer verification

**How the gate works.** `rndshape::kDC3RetailModel` is
`std::is_base_of_v<RndDrawable, RndEnviron>`, which is true only for dc3's
headers. Every DC3 function is a template on a dependent environ type, so
rb3-xenon's build never instantiates it. rb3 Wii's shape
(`RndShape_RB3Wii.h`) supplies:

- `kGenericShadowPass = true`
- a `CollectProjShadowCasters` that returns false
- a `SceneProjTexture` that returns nullptr

The shader's `dc3ProjLight` returns the identity unless
`projLightColor.w ≥ 0.5`, and only `WriteDC3ProjShadow` writes that field.

| consumer | check | result |
|---|---|---|
| rb3-xenon | `rb3-render` over `xbox-zip`, base engine 99ee167 vs this branch, same worktree | 11 of 11 PNGs byte-identical, at e24e553, at 82a6b89 and again at 5514b3f |
| rb3 (Wii, dc3 GPU backend, i.e. `WgpuRnd` with the RB3Wii shape) | `rb3-native` with `RB3_FIXED_CLOCK=1` and `setarch -R`, screenshots at frames 100/600/900, base vs this branch | not byte-identical **between two base runs** (mean abs diff 4.9-5.5 per pixel). The lane's difference is the same size (4.9-5.6), so it sits inside run-to-run noise. The title scene renders lit. |
| rb3 | `ctest` in a worktree build against this branch (rerun at 5514b3f) | 158 registered, 151 passed, 0 failed, 7 skipped |
| rb3 | `drawlog-golden.py --fixed-clock --canonical-order` | fails with "dump frame=61 != expected 60" on the **base** binary as well, so it predates this lane and says nothing about it |
| getenv census | `native_compat_census.py --engine-root <this branch> check` | OK, 442 flags. This lane adds no getenv. |
| dc3 | `native/tests/test_retail_lighting.cpp`, 9 tests | pass. Each sabotage listed in the dc3 commits fails it. |
| dc3 | `RehearseProjectionTest.DancerIsDrawnThroughTheProjection` | Failed under the retail model: 2-3% "character-coloured" against a 10% bar. The kept shots show the dancer drawn, saturated rather than washed out. The test's green ≥ 80 rule was measuring overexposure, so it now detects the dancer by hue. It passes on both the base and this branch, and reads 0.0% on the reveal-off shots. |

## 7. Findings handed over by other lanes

| finding | lane | state here | how |
|---|---|---|---|
| A non-prelit material with `use_environ` 0 is lit by the scene environ; retail gives it white ambient and no lights. | np-gfx-blend | **covered** | `rndshape::MatUnlit` is `!UseEnviron && !Prelit` for the DC3 shape. It applies to every draw, world and UI, through `BuildMaterialParams` and the next-pass path. Test: `MaterialWithoutEnvironIsUnlit`. |
| The mesh ambient keeps a 0.08 floor. | np-gfx-blend | **covered** | The floor lives only in the `EnvHasAmbientFogOwner` arm of `WriteSceneUniforms`, which DC3 no longer reaches. rb3-xenon still does, unchanged. Test: `AmbientAndPointLightsAreTheEnvironsOwn`, with ambient (0.02, 0.3, 0.4). |
| Particles take environ ambient × material colour (engine c6e10c5). | np-gfx-blend | **consistent** | Particles read `env->AmbientColor()` themselves. The scene ambient here is that same value, unfloored, so particles and meshes agree with retail's c1. |
| (a) Colour space: the Xbox 360 renders in gamma space. | np-gfx-hud | **covered, same conclusion** | Section 2 reaches it independently, and `kGammaSpaceShading = kDC3RetailModel` (c44d878) switches every DC3 draw, world and UI. `UiDrawTermsFor` ORs `GameRenderHook::UiShadesInGammaSpace` into that constant, so for DC3 the hook is redundant and can be retired at merge. |
| (b) Xenia headless PNGs carry an sRGB LUT. | np-gfx-hud | **not applicable** | Every retail reference used here is captured footage (`retail/INDEX.md`), not Xenia output. |
| (c) `MaterialShadesUnlit` applied only under the UI terms. | np-gfx-hud | **covered** | `MatUnlit` above is the same rule for every DC3 draw. `MaterialShadesUnlit` adds nothing for DC3 and can be retired at merge, or kept for a consumer that opts into the UI terms without the DC3 shape. |
| (d) The scene-light path drops RndLight intensity. | np-gfx-hud | **covered** | Intensity lives in the colour: `SetPackedColor` multiplies it in, and `Intensity()` is the max channel. `WriteDC3Retail` and the projected light read `GetColor()`, as `SetPointLightRegisters` does. Test: intensity 3 reaches c66. |
| (e) Retail selects the default environ every frame. | np-gfx-hud | **done** | `rndshape::kSelectDefaultEnvEachFrame` (DC3 shape only). |
