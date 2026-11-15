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
| approx lights | Folded into a six-face box map c80..c85 at the position `Select` was given, by `NgEnviron::UpdateApproxLighting` from `mLightsApprox` + `RndEnviron::sGlobalLighting`. Directionals and fake spots always land. Points and spots land only when there is a position. A Character passes its origin (`Character::DrawLodOrShadow`'s `RndEnvironTracker`), and so does every RndDir with its own environ: `RndDir::DrawShowing` reselects `mEnv` with `&WorldXfm().v` (Dir.cpp:312). Static venue geometry sits in such dirs, so its approx points do land (round 3 probe: every rink floor/wall mesh drew with a position). Draws get no position only where an environ is selected with a null one: `Crowd` (its batched draws), `Spotlight`, `TexRenderer`, `SkeletonViz` and `HamListRibbon`. A null *environ* changes nothing (`RndEnvironTracker` selects only a non-null one), so `RndGroup::DrawShowing` keeps the enclosing dir's environ and position. (Round 1 said "a WorldDir passes none"; that was wrong.) | MEASURED | BoxMap.cpp, Env_NG.cpp, Dir.cpp, Group.cpp |
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
   factor to the box term. (Item 11, §14: to the box term *and the point
   lights*; with point lights the shipped permutation scales their sum.)
4. The generic `ShadowPass::Render` pre-pass no longer runs for DC3
   (`rndshape::kGenericShadowPass`).
5. (Round 3.) Every projected light with shadow objects gets a map, as on
   retail, where each `NgLight` owns its own target. Up to four lights a
   frame share `ShadowPass`'s 1024² depth texture as a 2x2 atlas of 512²
   tiles; a light's tile is its index in the frame that drew it, and
   `shadowEnabled = 2 + tile`. `dc3ProjTap` clamps every tap half a texel
   inside the tile. Houseparty is the venue that needed it: `a2_spotlight.lit`
   (blend 0, gobo, five shadow objects) and `ShadowDirLight01.lit` (blend 1,
   `Shadows.env`, the dancers' floor shadow) are both live in one frame, and
   with one map the first to publish took it and the floor shadow drew none.
6. (Round 3.) The caster pass is self-contained (`ShadowPass::RenderCasters`):
   its own dynamic-offset layouts, one `WriteBuffer` per frame for all object
   slots (256 B each), bone palettes (2560 B per skinned caster) and the four
   256 B clip slots, and bind groups created once (again only when a buffer
   grows). Before, `DrawMeshShadow` created one or two bind groups per caster
   per frame, and `CreateBindGroup` was 20% of the pass. The generic
   `Render()`/`DrawMeshShadow` path is unchanged and still serves rb3-xenon.
   Tests: `ProjShadowCasterPassTest` (GPU readback of the map) and
   `RetailLightingTest.TwoShadowLightsInOneFrameEachGetATile`.

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

**Round 2: what casts in the streetside normal state.** MEASURED by a probe
on `CollectProjShadowCasters` (lane archive `round2/shadows/streetside_casters.txt`):
`a2_spotlight.lit`'s shadow list is `backup0`, `backup1`, `player0`,
`player1` and `score_star_display`, the last being the boombox group. That
expands to 87-106 meshes. **The light cones are not in the list**, so retail
draws no shadow for them either. A false-colour render of the receivers'
coverage (`round2/shadows/proj_footprint_falsecolour.png`) shows the lead
dancer's, the right dancer's and the boombox's footprints on the cardboard
and the road. The integration tree's frame has them as well
(`round2/shadows/shadow_int_vs_dbg.png`). On the dark road they are faint,
because the factor scales the diffuse light (`1 − 0.75 s`), never the
ambient, as in retail. (Before item 11 native scaled only the box term; §14.)

**Not done.**

- The receiver's vertex-lit form scales the whole interpolated light. That is
  INFERRED, and it does not matter in practice: the census above has no
  vertex-lit receiver, so it is a native-only fallback.
- `SphereConeTest` culling of casters is skipped. It only removes casters that
  cannot land in the map. (The other cull, nested draws against the
  camera's frustum, is reproduced since item 12: §15.5.)
- (Done in round 3.) The casters were drawn twice per frame: DC3's own
  `NgLight::RenderShadows` drew them into a 256x256 target plus two blur
  passes, and nothing sampled it (its only reader, `SetPConstant(PShaderConstant,
  RndTex *)`, is empty in `WgpuRnd`). dc3-decomp's `Lit_NG.cpp` now returns
  before that draw on native; `RetailLightingTest.NgLightRenderShadowsDraws-
  NoCastersOnNative` guards it.

## 4. Skin, hair and the rim (round 2)

`RndMat::shader_variation` picks a different `standard` permutation:
`mCustomVariation` is always copied into the options (`CalcShaderOpts`,
Shader.cpp 550-640). Round 1 ran skin and hair through the plain maths. The
dancers' materials use five per-pixel permutations, and each of them was
disassembled, written out as a readable model, and checked against an
emulation of the shipped microcode:

| permutation | used by (rasa05, the OMG lead) | model vs microcode, worst relative error over 2,500 random pixels |
|---|---|---|
| `0000032040035037` skin, rim, rim-under, normal + spec maps | `rasa05_skin`, `rasa05_head` | 7.5e-8 |
| `0000030040031037` skin, normal + spec maps, no rim | (skin without a rim colour) | 7.5e-8 |
| `0000030080031017` hair, spec map | `rasa05_hair`, eyebrows | 5.8e-8 |
| `0000032000031037` no variation, rim, normal + spec maps | `rasa05_jacket`, `rasa05_pants` | 1.9e-8 |
| `0000030000031017` no variation, spec map | | 2.0e-8 |

MEASURED. The tools are in dc3-decomp `scripts/gpu/xenos_ucode/`:

- `ucode_emu.py` executes xenia's ucode listing (vector and co-issued scalar
  ALU ops, `_prev` chains, `tfetch`).
- `char_models.py` holds the models and the check
  (`python3 char_models.py` prints the table above).
- `gen_char_shading_cases.py` writes the test table.

The listings and literal tables are in the lane archive under
`round2/ucode/` and `shaders/standard/`. **There is no cloth variation.** The
enum is none, skin, hair and world projection, and clothes are `none`.

Notation:

- Constants: c0 is the material colour, c1 the ambient, c2 the specular
  (rgb, power), c14.x = 1 − de_normal, c19 the second specular colour, c61
  the rim (rgb, power), c80..c85 the box faces. Point light i has direction
  L_i, colour c_i and falloff a_i (section 1).
- p = max(spec-map a · c2.w, 0.5), and n(p) = (p + 2)/2π.
- V points to the camera, unnormalised in the interpolator.
- Nk is the normal-mapped normal with de_normal applied. Nf is the same normal
  at full strength.

**Skin** (`dc3SkinLight`):

- Light is `(1 + rim · box(−V)) · box(Nk) + Σ a_i c_i · sat((Nk·L_i)·(0.55, 0.6, 0.65) + (0.45, 0.4, 0.35))`.
  This is a per-channel wrap: red wraps furthest past the terminator, which
  makes the warm shadow side.
- rim = `((1 − sat(V·Nk))(0.5 Nk.z + 0.5))^c61.w`.
- Colour is `texel · c0 · (light + c1) · (1 + c61.rgb · back)`. The
  back-light sheen is `back = Σ a_i c_i sat(−L_i·V) (1 − sat(V·Nf)²)^c61.w`.
- Specular is `spec-map · c2 · (Σ a_i c_i · 4(1 − sat(V·Nf)²)^0.7 · n(p) · sat(V·R_i)^p + rimf² · n(p) · boxPow(Re, p))`, where:
  - R_i = reflect(−L_i, Nf) and Re = reflect(−V, Nf);
  - rimf = (1 − sat(V·Nf))(0.5 Nf.z + 0.5);
  - boxPow(R, p) = Σ sat(±R_axis)^p · face.

  It uses the full-strength normal and a Fresnel-like `4(1 − cos²)^0.7`
  weight.
- With no rim colour (bit 37 clear) the rim and the sheen are absent.

**Hair** (`dc3HairLight`):

- The shipped permutation has no normal map. N is the normalised
  interpolated normal and n is the raw one.
- Light is `box(N) + Σ a_i c_i · sat((N·L_i)·(0.75, 0.8, 0.85) + (0.25, 0.2, 0.15))`.
- Re = reflect(−V, N). The strand is taken as horizontal:
  t = Re·(ẑ × n) = n.x Re.y − n.y Re.x.
- With s = |1 − t²| and l1 = s^(4p), the anisotropic lobe is
  `aniso = c2 · l1 + c19 · s^(2p) · (1 − l1)`.
- Specular is `spec-map · (Σ a_i c_i sat(L_i·Re) n(p) aniso + (0.25 + (1 − sat(V·N))(0.5 N.z + 0.5)) n(p) boxPow(Re, p))`.
  The box reflection is tinted by the map only, not by c2.
- Colour is `texel · c0 · (light + c1) + specular`.

**The plain rim** (round-1 defect, now fixed): the rim also takes the point
lights from behind,
`X = ((1 − sat(N·V))(0.5 N.z + 0.5))^p · box(−V) + Σ a_i c_i sat(−L_i·V) (1 − sat(N·V)²)^p (0.5 N.z + 0.5)`,
and out = `d · (1 + 0.3 c61·X) + 0.7 c61·X + spec`. Round 1 had only the box
half (relative error 0.89 against the microcode; 2e-8 with the point half).

**Native.**

- `retailShade` now fetches the textures and calls `retailPixelShade`, which
  takes `dc3SkinLight` / `dc3HairLight` for variations 1 and 2 under
  retailLighting 2 and the plain path otherwise.
- The projected-light factor and the AO factors apply as on the plain path.
  The box term is scaled by `proj.mult`, and `proj.add` is added.
- Vertex-lit skin and hair keep the plain vertex form. Every shipped skin and
  hair permutation is per pixel.
- The rim's point half is DC3-only, gated on retailLighting 2.
- `native/tests/test_retail_char_shading.cpp` runs the engine's own
  `retailPixelShade` in a compute shader on 48 random pixels: 12 each of skin,
  skin without rim, hair and plain + rim. It compares the result with the
  emulated microcode's output. A second case checks that RB3's form
  (retailLighting 1) keeps the plain path.

**Not measurable from the cache.** The cache holds no skin+rim permutation
without rim-under, and no hair+rim permutation. Both are therefore taken as
the measured forms: `rimLightUnder` is not read, and hair ignores the rim.
INFERRED.

## 4a. Peak state and Studio 675 (round 2)

**What lights the dancers in peak.** MEASURED by a probe on the environs a
draw selects (lane archive `round2/environs/streetside_environ_changes.txt`):

- The dancers draw with their own character environs, `player0.env`,
  `player1.env`, `backup0.env` and `backup1.env`. Each has ambient 0.012, and
  one real point light, `<who>_area1_01.lit` (range 400, full strength to the
  edge). The players also carry `battle_p<n>_spot.lit`, whose colour is 0 in
  this song.
- The point light's colour is animated by the light preset: (1.15, 0.93,
  0.66) in the normal state, falling to (0.87, 0.73, 0.64) in peak.
- `boombox.env` uses `player0_area1_01.lit`. `crowd.env` has `crowd1` and
  `crowd2`. `MASTER.env` has ambient (0.082, 0.024, 0) and `default_stage01`
  in white.
- There is no other character light. The rim comes from the material: c61 on
  rasa05's jacket, pants, skin and head is (1, 0.502, 0.753, 50), and hair
  has none. It is lit by the box map and, since round 2, by the point light
  from behind (section 4).

**The stripes.** They are present in every native peak capture, uncapped and
with the frame rate pinned to 20 fps (`round2/stripes/`). The stripe meshes
(`peak_line*.mesh`) draw with an animated material alpha. FrameCapture over
1000 frames reads `peak_line1` from 0.57 down to 0.00, and the ten lines fade
in a ramp. No lighting rule touches them. INFERRED: a frame without stripes
was taken low in that cycle.

**Too dark.** The same song in the same venue (OMG at DCI, retail `dci/05`
and `dci/06`) is a different section with a blue preset, where native's
capture is magenta. Luma on a 640x360 resize:

| frame | whole frame | p90 | lower-left floor | centre (dancer) |
|---|---|---|---|---|
| retail dci/05 | 44.2 | 120.1 | 35.6 | 63.9 |
| retail dci/06 | 45.8 | 131.1 | 30.1 | 48.5 |
| native peak_dci | 25.8 | 83.4 | 14.9 | 30.5 |
| native with `pow(x, 0.85)` | 31.1 | 90.1 | 19.0 | 37.3 |

The display ramp, which belongs to np-gfx-post, explains about a quarter of
the gap. The rest is INFERRED to be the preset (retail's blue state lights
the floor) and the missing glow. **Not settled.**

**Studio 675 (dclive): an authored state, not a colour-order error.**
Whole-frame mean hue, MEASURED (`round2/dclive/measurements.txt`):

| frame | hue | saturation |
|---|---|---|
| native normal | 23.0 | 0.32 |
| retail warm state (01, 05, 06, 07, 14) | 25.8-28.7 | 0.26-0.36 |
| retail pink state (09, 12, 13) | 358.9-6.6 | 0.30-0.37 |
| retail blue state (02, 03, 08, 10) | 212-233 | |

Native sits in the warm state, about 4° redder than retail. A red/blue swap
would move it to about 200°. No packed channel order is involved anyway:
light colours are float `Hmx::Color`, and lit DC3 draws do not read vertex
colour, which is ARGB in any case (`dc3-backend-for-rb3-wii.md` section
13.1).

The hard red outline on the lead dancer is the game's wrong-limb feedback,
not lighting. Retail `dci/04` (an arm) and `dci/07` (the whole body, at the
end pose) show the same outline. The capture route does not dance, so every
limb is wrong.

What is still different: native's floor is whiter and brighter, the back of
the stage is darker, and the boombox reads silver where retail's warm frames
show it gold. Retail's pink frame 09 shows it silver too, so the boombox
colour is INFERRED to follow the light or the song.

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

**Round 2 captures.** Before is the 5664e9d merge build and after is
5dcd3a2. Both are six venues and both states, with every cell gameplay =
yes. The sheets are in the lane archive under
`round2/sheets/<venue>_before_after.png`, the close-ups under `round2/skin/`,
and the streetside floor crop is `round2/shadows/streetside_normal_feet_after_r2.png`.

- By eye, skin now reads warmer and lighter on the shadow side, from the
  per-channel wrap. The jackets' rim now includes the point light from
  behind (section 4).
- The streetside crop shows the lead's leg shadow and the boombox's shadow
  on the cardboard.
- The camera shot differs between the two runs, so floor luma is not
  comparable frame to frame. The shading itself is checked by the microcode
  test, not by these pictures.

**Frame time, round 2 (streetside normal state).** Three builds from engine
5dcd3a2:

- `full`;
- `noskin`, with the skin and hair branch disabled;
- `noshadow`, where `WriteDC3ProjShadow` returns early and
  `CollectDC3Casters` returns false.

They ran interleaved, three rounds each, on OMG, sampling 10 s from 20 s into
gameplay, on an RTX 3090. The box was loaded (31-41 on 32 cores) and the GPU
was shared with other lanes. GPU time is a per-process `nvidia-smi pmon` SM%
times the frame time, so it is an estimate.
Data: dc3-decomp lane archive `round2/frametime_ab.txt`.

| cost | GPU ms/frame, per round | CPU ms/frame |
|---|---|---|
| projected shadows (native coverage pass + receiver taps) | 0.32, 0.18, 0.29 | about 0.27 (perf: `ShadowPass::RenderCasters` 5.3% + `CollectProjShadowCasters` 1.5% of a 3.95 ms frame) |
| skin and hair maths | −0.01, +0.09, −0.05, i.e. within noise | none expected (WGSL only) |
| DC3's own `NgLight::RenderShadows` into a target nothing samples | | about 0.20 (`NgLight::CheckShadowMap` 5.0%) |

The paired CPU deltas (−0.26 to −0.83 ms) are not usable. The box got faster over
the run (the full build read 6.68, then 4.24, then 3.77 ms), so whichever
build ran later looks cheaper. `noskin` changes only WGSL and still read
0.26-0.54 ms cheaper.

**Remaining gap.**

- Skin and hair now run the shipped variations (section 4). What is left of
  the skin gap is the light reaching it: the character environ's point and
  box map, and the missing scan-out ramp (np-gfx-post).
- Overall saturation is still below retail.
- Peak at DCI reads about 0.6x retail's luma on the same song (section 4a).
  Of that, the ramp accounts for about a quarter; the rest is not settled.
- Venues other than streetside, rink, DCI and Studio 675 were checked by eye
  only.
- Not done: removing DC3's own unsampled `NgLight::RenderShadows` pass
  (about 0.2 ms CPU), and caching the caster pass's bind groups.

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
| rb3-xenon, round 2 | `rb3-render` over `xbox-zip`, engine 5664e9d (round-2 base) vs 41760b4 | 11 of 11 PNGs byte-identical |
| rb3, round 2 | `ctest` in the lane's rb3 worktree (rb3 1dee2c1f7) against 41760b4 | 158 registered, 1 failed, 7 skipped. The failure is `native_compat_census.check`: that worktree's ledger is stale against the np-gfx merge's flags. The same check fails against the base engine 5664e9d, and rb3 main's copy passes. |
| dc3, round 2 | `scripts/native_test.sh` at dc3 30ce069ac / engine 9929879 | 895 registered, 825 executed, 824 passed, 1 failed, 70 skipped (budget 70). `RetailCharShadingTest` 2/2 pass. The failure is `LargestSongFlowTest.OmgPreloadsAndReachesGameplay`: in the full suite the engine hit its 3000-frame cap in the intro. It passes alone (2 of 2 runs), main 442e1aeff raises the cap, and the integration gate gate-int2 failed it the same way. |
| getenv census, round 2 | rb3 main's `native_compat_census.py --engine-root <this branch> check` | OK, 446 flags, the same as on the base 5664e9d. The 4 above round 1's 442 came in with the np-gfx merge, and `git diff 5664e9d HEAD` has no `getenv`. |
| rb3-xenon, round 3 | `rb3-render` over `xbox-zip`, engine 0ec9eab (round-3 base) vs 30ae8c0, then vs 3c8c49e | 11 of 11 PNGs byte-identical, both times |
| rb3, round 3 | `ctest` in the lane's rb3 worktree (rb3 1dee2c1f7) against 3c8c49e | 158 registered, 150 passed, 1 failed, 7 skipped. The failure is `native_compat_census.check` (stale ledger in that worktree), the same as against the base engine. |
| getenv census, round 3 | `git diff 0ec9eab 3c8c49e` and `git diff b2bdfdb0e dfb5f897c` | no new `getenv`. The one `getenv` line in the dc3 diff is an existing `MILO_LIB` read that moved within a test file. |
| dc3, round 3 | `scripts/native_test.sh` at dc3 dfb5f897c / engine 3c8c49e | 934 registered, 864 executed, 864 passed, 0 failed, 70 skipped (budget 70) |
| dc3 | `native/tests/test_retail_lighting.cpp`, 9 tests | pass. Each sabotage listed in the dc3 commits fails it. |
| dc3 | `RehearseProjectionTest.DancerIsDrawnThroughTheProjection` | Failed under the retail model: 2-3% "character-coloured" against a 10% bar. The kept shots show the dancer drawn, saturated rather than washed out. The test's green ≥ 80 rule was measuring overexposure, so it now detects the dancer by hue. It passes on both the base and this branch, and reads 0.0% on the reveal-off shots. |

## 7. Findings handed over by other lanes

| finding | lane | state here | how |
|---|---|---|---|
| A non-prelit material with `use_environ` 0 is lit by the scene environ; retail gives it white ambient and no lights. | np-gfx-blend | **covered** | `rndshape::MatUnlit` is `!UseEnviron && !Prelit` for the DC3 shape. It applies to every draw, world and UI, through `BuildMaterialParams` and the next-pass path. Test: `MaterialWithoutEnvironIsUnlit`. |
| The mesh ambient keeps a 0.08 floor. | np-gfx-blend | **covered** | The floor lives only in the `EnvHasAmbientFogOwner` arm of `WriteSceneUniforms`, which DC3 no longer reaches. rb3-xenon still does, unchanged. Test: `AmbientAndPointLightsAreTheEnvironsOwn`, with ambient (0.02, 0.3, 0.4). |
| Particles take environ ambient × material colour (engine c6e10c5). | np-gfx-blend | **consistent** | Particles read `env->AmbientColor()` themselves. The scene ambient here is that same value, unfloored, so particles and meshes agree with retail's c1. |
| (a) Colour space: the Xbox 360 renders in gamma space. | np-gfx-hud | **covered, same conclusion** | Section 2 reaches it independently, and `kGammaSpaceShading = kDC3RetailModel` (c44d878) switches every DC3 draw, world and UI. `UiDrawTermsFor` ORs `GameRenderHook::UiShadesInGammaSpace` into that constant, so for DC3 the hook is redundant and can be retired at merge. |
| `NativeUiMaterialTerms.WorldPhaseUnchanged` expected the world to stay linear. | np-gfx-hud (test) | **test updated** (dc3 30ce069ac) | It was written before the world switched to gamma space, and it failed in the integration gate for that reason alone. It now expects `gammaShading` true in the world phase and still expects `retailMaterialTerms` false. |
| (b) Xenia headless PNGs carry an sRGB LUT. | np-gfx-hud | **not applicable** | Every retail reference used here is captured footage (`retail/INDEX.md`), not Xenia output. |
| (c) `MaterialShadesUnlit` applied only under the UI terms. | np-gfx-hud | **covered** | `MatUnlit` above is the same rule for every DC3 draw. `MaterialShadesUnlit` adds nothing for DC3 and can be retired at merge, or kept for a consumer that opts into the UI terms without the DC3 shape. |
| (d) The scene-light path drops RndLight intensity. | np-gfx-hud | **covered** | Intensity lives in the colour: `SetPackedColor` multiplies it in, and `Intensity()` is the max channel. `WriteDC3Retail` and the projected light read `GetColor()`, as `SetPointLightRegisters` does. Test: intensity 3 reaches c66. |
| (e) Retail selects the default environ every frame. | np-gfx-hud | **done** | `rndshape::kSelectDefaultEnvEachFrame` (DC3 shape only). |

## 8. Round 3

### 8.1 The rink floor pool: a gobo that never reached the GPU

**What lights the dance floor (MEASURED, paused native session).** The floor
round the dancers is `StageCarpet.mesh` (`StageCarpet.mat`, environ
`Stage_Carpet.env`), not the `Rink*.mesh` wood floor: hiding StageCarpet.mesh
drops the floor band from 194 to 65-80 luma. `Stage_Carpet.env` holds ambient
(0.227, 0.310, 0.439) and three real lights: `Stage.lit` and `Stage1.lit`
(points, range 400, falloff_start 0) and `stage_spotlight.lit` (fake spot,
projected_blend 0, gobo `light_glow01.tex`, colour 3.0, top/bottom radius
100/200, range 175, five shadow objects). Light-by-light, on one paused frame
(`{game_panel set_paused_except_sound TRUE}`; lights hidden with their
`set_showing` handler -- `showing` is not a RndLight property, and setting it
is silently a no-op):

| state | floor centre | floor edges | centre/edge |
|---|---|---|---|
| all on | 193.5 | 179.2 | 1.08 |
| `stage_spotlight` hidden | 91.6 | 77.4 | 1.18 |
| `Stage.lit` hidden | 185.5 | 173.6 | 1.07 |
| `Stage1.lit` hidden | 188.9 | 176.4 | 1.07 |
| `StageCarpet_lightCatcher.mesh` hidden | 209.7 | 187.3 | 1.12 |
| `mirrorball_spotlight_01` hidden | 87.6 | 79.4 | 1.10 |
| all three carpet lights hidden | 35.5 | 38.2 | 0.93 |
| retail rollerrink/01, 09, 12 | 130.0, 162.6, 90.4 | 86.9, 95.9, 77.5 | 1.50, 1.70, 1.17 |

(Luma of rows 480-720; centre = columns 480-800, edges = 0-200 and
1080-1280. Native includes the retail post chain, as the footage does.)

So the spot is most of the floor, and it lit the floor flat. **The term:**
textures are uploaded when a material binds them (`PresyncBitmap` in
MaterialSetup), and a gobo is bound by a light only. `light_glow01.tex` and
`discoBallReflection.tex` are referenced by no material, so
`WriteSceneUniforms` got a null view for slot 3 and bound the white default.
Every projected_blend 0 light then lit its whole cone at full colour. MEASURED:
on the paused frame, setting the spot's texture to `discoBallReflection.tex`
and back changed **no pixel** (all three frames identical to the last digit),
while halving its radii did change the frame. `light_glow01.tex` is a radial
glow (64x64; 0.51 at the centre, 0.19 at 0.3 of the width, 0.08 at 0.4, 0 at
the edge), so with it the spot is a pool round the dancers, as in retail.
Engine 3c8c49e uploads the gobo in `WriteSceneUniforms`; test
`ProjShadowCasterPassTest.GoboOnlyALightBindsReachesSlot3` (sabotage: drop the
`PresyncBitmap` call; slot 3 falls back to white and the test fails).

Every blend-0 light in the six venues went through the same white fallback:
rollerrink's `stage_spotlight` and both mirrorballs, and `a2_spotlight` in
houseparty and dclive (8.3).

After 3c8c49e, the same probe on a paused frame (another session, so another
shot): swapping the texture now changes the frame (floor 133.0 -> 138.8), and
the floor reads

| | floor centre | floor edges | centre/edge | frame |
|---|---|---|---|---|
| before (paused, 30ae8c0) | 170.2-193.5 | 175.2-179.2 | 0.95-1.08 | 119-123 |
| after (paused, 3c8c49e) | 133.0 | 103.6 | 1.28 | 83.4 |
| retail rollerrink/01, 09, 12 | 90-163 | 78-96 | 1.17-1.70 | 77-93 |

The pool is back round the dancers, the edges and the stage steps are darker,
and the frame mean is inside retail's range. The remaining gap: retail's
centre/edge runs higher (1.5-1.7 on 01 and 09). Not tuned; the shots differ
(camera, dancers, and the mirrorball gobos move), and the post chain is the
post lane's.

**The light catchers (MEASURED from the assets).** Two Multiply materials
modulate the rink by a light: `StageCarpet_lightCatcher.mat` on
`StageCarpet_lightCatcher.mesh` over the dance floor, and
`Rink_lightCatcher.mat` on `Rink_overlay.mesh` and `Rink{1..5}_overlay.mesh`,
which copy the `Rink*.mesh` wood floor (+0.1 z) round the outside of the
rink. Both are blend 6 (Multiply), untextured, white, `use_environ`,
per-pixel lit, receive projected lights. Their environs hold ambient 0.365 and
one mirrorball fake spot (colour 1.99, gobo `discoBallReflection.tex`, range
500). Retail draws Multiply as src ZERO, dest SRCCOLOR (`NgMat::SetBasicState`,
Mat_NG.cpp) with fade (1,1,1,1) and lights the material like any other, so the
floor is multiplied by what the catcher receives: 0.365 away from the gobo,
up to 1 under it.

Native forced every lit Multiply material prelit (`kHeuristicMultiplyPrelit`),
so a white catcher multiplied by 1. Engine 30ae8c0 adds
`rndshape::kMultiplyBlendIsLit` (true only for the DC3 retail model) and keeps
those materials lit in the primary and next-pass builder; rb3-xenon and
RB3-Wii keep the heuristic. Test `NativeUiMaterialBuilderTest.MultiplyMaterial-
IsLitUnderTheDC3Model`; sabotage `kMultiplyBlendIsLit = false` fails all three
assertions. Until the gobo fix the catcher's own gobo was white too, so it
received about 0.365 + 1.99 everywhere in its cone and clamped to 1: hiding it
moved the paused floor by only 194 to 210.

The eight lit Multiply materials the gate reaches (round-3 material census):

| material | venue | texture | z mode |
|---|---|---|---|
| `Rink_lightCatcher` | rollerrink | none (white) | normal |
| `StageCarpet_lightCatcher` | rollerrink | none (white) | normal |
| `DLV_PeakTransSetFloor01` | dclive | `dlv_peakfloor_diff`, colour 0 | |
| `DLV_PeakTransStage01` | dclive | `dlv_peakfloor_diff`, colour 0 | |
| `Building2_shadows` | streetside | `building2_shadow.tex` | decal |
| `building1_shadows` | streetside | `building1_shadow.tex` | decal |
| `THR_carpetShadow` | throneroom | `thr_carpetshadow.tex` | transparent |
| `THR_throneShadow` | throneroom | `thr_throneshadow.tex` | transparent |

### 8.2 The boombox colour is a profile unlock, not a light

MEASURED from the data:

- `ui/global.dta`, `initialize_gameplay_data`:
  `{hamprovider set golden_boomy {profile_mgr is_content_unlocked boomygold}}`.
- `config/metagame_rank.dta:369`: `Golden_Boomy` rewards `(unlock boomygold)`.
- `world/shared/gen/chars_base.milo_xbox`, `score_star_display`'s
  `Flow1.flow`: a FlowSwitchCase on `hamprovider golden_boomy` plays
  `boom_unlock.anim`, which swaps the boombox to `boomy_main_unlock.mat`
  (diffuse `boomy_main_unlock.tex`, specular (0.79, 0.48, 0), no emissive)
  from `boomy_main.mat` (`boomy_main.tex`, specular 0.23 grey, emissive
  `boomy_ems.tex`).
- `config/flow.dta` declares `golden_boomy` ("Is golden boomy unlocked?",
  default FALSE). No other data file sets it: the only writer is the line in
  `initialize_gameplay_data` above. `Golden_Boomy` is the last reward in
  `metagame_rank.dta` ("You unlocked the Golden Boombox!").
- Retail footage, by capture (INFERRED that it is the recording profile, from
  the split): every Zero Conflictz capture shows the gold/orange boombox
  (dci/12, streetside/03, rollerrink/09, dclive/04); every GleasonRyan, Venus
  Vinnycakes and TopherFloyd capture shows the silver one (dci/05, dci/06,
  rollerrink/01, dclive/11). Same venue, same crew, different colour, so it
  is neither light nor crew, and the split runs by uploader, not by song or
  difficulty (rollerrink/01 is Hard and silver).

Native: `{hamprovider set golden_boomy TRUE}` during gameplay turns it gold,
so the renderer draws both. Native's dev profile has not unlocked
`boomygold`, so the silver one is the right default for it. **Open, not a
renderer item**: if a gold default is wanted, it belongs to the
profile/unlock flow.

### 8.3 Projected shadows on the six venues

From the round-3 native probe of each venue's environs (MEASURED in a native
run; the light data is the shipped data):

| venue | retail projected light with shadow casters | native draws it |
|---|---|---|
| streetside | `a2_spotlight.lit`, blend 1, 5 shadow objects | yes |
| throneroom | `shadow.lit`, blend 1 | yes |
| houseparty | `ShadowDirLight01.lit` blend 1 and `a2_spotlight.lit` blend 0 (gobo), both with casters | both since round 3 (atlas); before, only `a2_spotlight` |
| rollerrink | `stage_spotlight` blend 0 (gobo), 5 casters; the mirrorballs are blend 0 with no casters | yes |
| dclive | `a2_spotlight` blend 0 (gobo), 5 objects | yes |
| dci | none | nothing to draw |

### 8.4 What the model adds per draw

Per draw (`ObjectUniforms`, 256 B slot): `boxLight[6]` (c80..c85, the box
map at the draw's position, 96 B) and `retail` (.x baked AO, .y approx lights
queued, .z receives projected lights). Per scene (`SceneUniforms`, written
when the environ changes): ambient (c1), up to 4 point lights (c62.., c66..,
of which retail fills three), `projLightDir` / `projLightColor` (.w 1 =
multiply shadow, 2 = additive gobo), `projLightProjRow0/1`, `lightViewProj`,
`shadowEnabled` (2 + atlas tile), `retailLighting` = 2, and fog.

### 8.5 Retired hooks

`GameRenderHook::UiShadesInGammaSpace` and `MaterialShadesUnlit` are gone
(engine 1dd04b7). For DC3 both were already implied by the shape:
`kGammaSpaceShading` and `rndshape::MatUnlit`. No other consumer overrode
either hook. rb3-xenon's 11 renders are byte-identical across the change.
The heuristic arm's comment in `WriteSceneUniforms` now says which shapes
reach it.

### 8.6 Depth test of the world (z mode)

MEASURED from the image: `NgMat`'s `tf2cf` table at 0x8208A0F8
(`build/373307D9/asm/system/rnddx9/Mat.s:134`, normal bank
`[7, 1, 3, 2, 5, 4, 6, 0]`). `kZModeNormal` (TestFunc 1) is D3DCMP_LESS,
which is what native's `Less` already uses. Transparent and decal (TestFunc 2)
are LESSEQUAL. Nothing to change here. Out of lane:
dc3-decomp's `src/system/rnddx9/RenderState.cpp` names indices 2 and 3 the
other way round (EQUAL / LESSEQUAL); the data is right and the names are not.

### 8.7 Saturation and brightness that belong to the post chain

Same frames with `DC3_RETAIL_POST=0` and with the post chain (native, before
the gobo fix): rink floor 138 -> 205 luma, DCI normal frame 36 -> 72, dclive
floor with post (213, 178, 175). The chain roughly doubles the darker scenes.
The lighting is not tuned against that; the numbers go to the post lane.

### 8.8 The character's own lights in peak (item 2)

MEASURED (round-3 environ probe, streetside peak): `player0.env` ambient
0.027; real `player0_area1_01.lit`, a point (1.30, 1.09, 0.96) with range 400
that follows the character; approx lights a rim directional (3.77, 4.29, 4.98)
and `area1_02`, a point (0.24, 0.10, 0.46) with range 250. Native fills the
point into c62/c66 (three slots) and the approx lights into the box map at the
character's origin, as `NgEnviron::UpdateApproxLighting` does, and the rim
term (c61) lights the dancers' edges in the peak captures. A numeric
comparison with streetside/03 and dclive/04, 11 is confounded: retail's
dancers wear white or light outfits and native's scripted route dresses them
in dark suits, so the lit side's luma says more about the cloth than the
light. Not tuned.

**DCI peak, against retail dci/05, 06 (the post lane's pointer: retail's
room and suits read blue/violet, native's dancers neutral grey).** MEASURED,
live native session in peak (choreo performer, 57 Perfect, evidence
`round3/dci_peak_charlights/`), sampled every 5 s for 70 s:

- `player0_area1_01` (key), `_02` and `_rim`, and the backups' copies
  (`color_owner` is player0's), all carry `animate_color_from_preset` 0, so
  no LightPreset drives them. Their colour comes from `env_char_lighting.anim`
  (a PropAnim under `environments_master.anim` in `world/dci/gen/dci.milo`),
  whose frame is the venue's environment state.
- Native sits at frame 100 from 10 s on: key (1.40, 1.17, 1.03) =
  1.6 x (0.871, 0.733, 0.643), rim (4.98, 1.65, 5.00) = 5 x (0.996, 0.329,
  1.0), fill (0.24, 0.11, 0.46). These are the anim's frame-100 keys exactly.
- The anim's keys, decoded from the milo (MEASURED): the key light is warm at
  frames 50, 100, 150 and 200 ((1, 0.84, 0.74), (0.87, 0.73, 0.64)) and blue
  only at 220 (0.18, 0.25, 0.39) and violet at 240 (0.48, 0.22, 0.49). The
  peak flow in the same file (`peak_sky_frame`, `peak_ribbon_*`) sends
  `environments_master` to 50 or 100; 170 and 250 are sent by other triggers
  (250 is `Frame250_DangerRedAlert`, campaign).

So in both peak frames retail's own character key is warm, as native's is:
the blue on retail's suits is not the character lights, and nothing here
changed. INFERRED where it comes from: the same `environments_master` keys
also drive `dci_performance_high.pp` (`bloom_color`, `bloom_threshold`,
`bloom_intensity`, `saturation`, `lightness`), and the post lane's own
numbers put most of native's blue in the bloom (full chain rgb 47.6, 33.5,
102.9 vs no-bloom 38.0, 23.8, 46.4; retail dci/05 54.2, 33.3, 124.1). The
remaining blue deficit (about 20 levels of B) goes back to the post lane with
those keys as the lead.

### 8.9 Frame time, round 3

Round-3 base (engine 0ec9eab, dc3 b2bdfdb0e) against the whole round (engine
3c8c49e, dc3 dfb5f897c): OMG, headless, 10 s sampled 20 s into gameplay,
builds interleaved, three rounds, RTX 3090. The box was heavily loaded
(load average 46-55 on 32 cores), and four of the nine base runs and one of
the gobo runs read about twice their neighbours, so read medians only. Data:
the lane archive's `round3/frametime/`.

| venue | main-thread ms/frame, base (median) | round 3 (median) | GPU estimate ms, base / round 3 |
|---|---|---|---|
| streetside | 4.26, 4.65, 10.47 (4.65) | 4.35, 3.67, 4.10 (4.10) | 0.83 / 0.80 |
| houseparty | 9.87, 3.68, 4.51 (4.51) | 2.52, 3.60, 3.41 (3.41) | 0.89 / 0.54 |
| rollerrink | 4.13, 10.65, 5.30 (5.30) | 4.13, 6.44, 4.38 (4.38) | 0.68 / 0.73 |

So round 3 costs nothing measurable and probably saves CPU. The `perf`
profile (round-3 build, streetside, 3.13 ms frame) is the firmer evidence:

- `NgLight::CheckShadowMap` is down from 5.0% of the frame (round 2) to
  0.05%: DC3's unread shadow render is gone.
- The caster pass is `ShadowPass::RenderCasters` 2.5% plus
  `CollectProjShadowCasterSets` 2.1%.
- The gobo upload is not visible. `RndTex::PresyncBitmap` is 1.2% of the
  frame, all of it from `BuildMaterialParams`; no sample has
  `WriteSceneUniforms` as its caller.
- The next shadow-shaped cost is not this lane's: `RndShadowMap::PrepShadow`,
  3.5%, under `Character::DrawLodOrShadow`.

## 9. Round 4: the peak state

### 9.1 Intensify doubles alpha in the world too (streetside's peak stripes)

**Retail, MEASURED.**

- `NgMat::SetRegularShaderConst` sets c5 = (emissive multiplier,
  intensify + 1, bloom multiplier, 0).
- Every shipped `standard` permutation with option bit 53 (`mIntensify`)
  scales the whole texel, alpha included, before it meets the interpolated
  colour. `standard_0020000000020010.ps` (xenia-gpu-shader-compiler ucode
  disassembly) does `mul r0, r0, c5.yyyy`, then `mul oC0.___w, r0.wwww,
  r2.wwww`. The same key without bit 53 (`standard_0000000000020010.ps`)
  writes `r0.w * r2.w`.
- The world draws with the same shader as the UI. Nothing in `NgMat` takes
  the phase into account.

**Streetside's stripes, MEASURED in a live native session (awesome state,
`/api/dta/eval`).**

- `streetside_peek_pink1..16.mat` are blend 3 (SrcAlpha), intensify,
  alpha_write, and lit (`PeakCity.env`). Their colour is (1, 0, 0.5).
- The peak animation keys their alpha from 0.57 (`pink1`) down to 0.025.
  The milo stores 0.
- The red that fills the gaps on retail streetside/03 is not a material
  colour. It is `streetside_performance_awesome.pp`'s bloom:
  `bloom_color` (0.91, 0, 0) × intensity 1.4, which HamDirector copies into
  the force post-proc slot. The stripes' alpha (alpha_write; blend MAX on the
  alpha channel) is that bloom's mask.
- `peakOrange.lit` cannot be the source. Its range is 400, and the stripes
  are about 1900 units from it.
- Every environ has tone map and colour adjust off.

**Native before.** `MaterialAlphaScale` doubled alpha in the UI phase only, so
the stripes drew at half their opacity and wrote half the bloom mask (stripes
read 0.5 in the `DC3_RETAIL_POST=mask` view).

**Fix.** `rndshape::kIntensifyScalesAlpha`, which is `kDC3RetailModel` in
`RndShape_DC3.h` and false in `RndShape_RB3Wii.h`. `MaterialAlphaScale`
returns 2 for an intensify material in any phase when it is true.

- rb3-xenon keeps the UI-only rule (the flag is false there).
- Test: dc3 `NativeUiMaterialAssetTest.PeakStripeIntensifyDoublesWorldAlpha`
  loads `streetside_peek_pink1.mat` and checks the world builders' alpha.
  Three older expectations moved with it.
- Sabotage: `kIntensifyScalesAlpha = false` fails four tests (measured).

**Result.** Measured at matched game time (awesome state, choreo performer,
1 s apart).

- The stripes are opaque and the gaps fill with red bloom, as on retail.
- Mean frame rgb over six shots, base vs fix: (40.4, 15.4, 49.1) to
  (76.6, 13.9, 55.5). Red almost doubles, which is the bloom's colour.
- Other venues: every intensify world material is now twice as opaque.
  Captured base vs fix at dci, dclive, houseparty and rollerrink, peak and
  normal. The frame means moved by at most +16 levels: dclive peak blue,
  75 to 96; DCI normal blue, 80 to 93. Nothing broke visually. The cause is
  the shader's own rule, so it was not tuned per venue.

**Remaining gap, MEASURED, pixel columns.** Retail streetside/03 against
native after the fix, same state, different crew.

| | retail | native |
|---|---|---|
| lower stripe | (255, 41..143, 255) | (204..216, 0, 228..250) |
| gap between stripes | (255, 40, 0) | (108..131, 0, 0) |
| crowd silhouette, darkest 30% | (157..185, 26..29, 3..5) | (50..53, 0, 1.5) |

- Native's raw scene (`DC3_RETAIL_POST=raw`) has the crowd at 2-16 per
  channel, so the crowd is lit about black, as retail's is.
- The difference is downstream of lighting:
  - bloom strength or extent: retail's red bloom is about 2-3× native's over
    the same black;
  - the colour transform: native's green is 0 across the lower frame, where
    retail keeps 30-140.
- Both belong to the post lane's code (`RB3RetailPostDC3.cpp`) and are handed
  to it (section 9.6).

### 9.2 The dancers in peak (item 2)

**The "dark grey" dancers were mostly the outfit.** MEASURED, streetside
awesome, live native session. OMG's scripted route dresses the lead and the
crew in the DCI suits (dark cloth). Setting the lead to Mo
(`{{gamedata player 0} set_character_outfit mo01}` at the multiuser screen)
gives a lit lead and Bodie-type backups. Next to retail streetside/03 and 04,
these read brightly lit and close in structure. Retail is warmer and redder
over the dancers, which is the red bloom of section 9.1: the bloom mask
covers the dancers as well as the stripes.

**The lights, MEASURED, `player0.env` in awesome:**

- ambient 0.012;
- key `player0_area1_01.lit`: a point, (0.871, 0.733, 0.643), range 400,
  which follows the character;
- rim: approx, type 1, (4.82, 1.88, 5.0), that is 5 × the anim key;
- fill: (0.243, 0.106, 0.459);
- `battle_p0_spot`: black in this state;
- box map at the character's origin: maximum about (4.4, 1.7, …).

The box map is what the decomp's `NgEnviron::UpdateApproxLighting` computes
from those approx lights. The animation frame is the venue's peak frame, 100,
as in section 8.8. Nothing the native port adds or drops was found on the
dancers, so no lighting change was made. The remaining warmth is the bloom
strength (section 9.6). DCI and houseparty were not re-measured this round;
section 8.8 covers DCI.

### 9.3 The peak mirror floor (item 3)

**What the reflection draws, MEASURED (streetside awesome, live native
session, `get_array` on the `WorldReflection`).**

- `peak_reflection.refl`: showing 1, vertical_stretch 1, no hide or show
  list. It draws the four characters (as `lod_chars`), `score_star_display`,
  `peak_BlueLine`, `peak_sky_1..3`, `peak_city_close`, `peak_city` and all
  sixteen `peak_line*` meshes. **So the reflected pass includes the stripes.**
- `bid_reflect.refl`: draws only `score_star_display`.
- The stripes appear in two environs: `PeakCity.env` (the direct draw) and
  `peak_reflection.env` (the reflected pass), with the same blend 3
  (SrcAlpha) material.
- `peak_reflection.env`: ambient (0.22, 0.216, 0.255), box map zero.
  `PeakCity.env`: ambient (0.325, 0.271, 0.737) plus box (0, 0.24, 0.55). So
  the data itself lights the reflected stripes more dimly.
- The road (`Street.mat`, the `alleyRoad*` and `mainRoad*` meshes): showing in
  peak, blend 1 (opaque), alpha 1, per-pixel lit, specular. Its environ is
  `default_environment.env`, ambient (0.082, 0.024, 0).

**The floor, MEASURED, pixel samples.** Retail streetside/03 against native
after the 9.1 fix:

| | retail | native |
|---|---|---|
| reflected stripes | (105..150, 24..31, 48..59) | (60..72, 0..8, 38..46) |
| gap between reflected stripes | (75..125, 12, 0), red bloom | (55, 25, 33) |
| far floor | black | (25, 23, 32); raw scene (22, 22, 40) |

The reflection's strength relative to the direct draw roughly matches: the
blue channel of reflected over direct stripes is about 0.22 on retail and
0.19 native. The reflected stripes lack red for the same reason the direct
ones do, so the bloom (section 9.6) covers most of it. `WorldReflection::
DrawShowing` draws the mirrored world straight into the frame with a mirrored
camera, and native follows it, flipping only the cull mode
(`Mesh_Wgpu.cpp`), so no reflection-strength term was missing.

Left open (INFERRED, not fixed): native's far floor has a small blue lift,
(22, 22, 40) raw, where retail is black. Its likely source is the lit road
plus the reflection showing through it. How retail orders the opaque road
against the reflected pass was not settled this round (the draw-order query
failed: `iterate` over meshes raises MILO_FAIL in the eval server).

### 9.4 The crowd in peak (item 4)

MEASURED, streetside awesome. `crowd.env`:

- ambient (0.082, 0.024, 0);
- box about (0, 0.063);
- `sunlight_crowd`: a green directional, 0.118;
- `crowd1` and `crowd2`: points, about 0.06.

In the raw scene (`DC3_RETAIL_POST=raw`) the crowd's pixels are 2-16 per
channel: lit about black, which is what retail's silhouettes need. The
"texture colour" in the brief's sheet was the high state or the pre-9.1
frame. After 9.1 the crowd reads red-black (50..53, 0, 1.5) through the
post chain, where retail reads (157..185, 26..29, 3..5). That is a black
crowd under a red bloom 2-3× stronger than native's, so it goes to the post
lane (section 9.6). Nothing in the lighting was changed for it.

### 9.5 Studio 675 (dclive) dancer shadows in the normal state (item 5)

**Which light casts them, MEASURED (live native session, dclive, `get_array`).**

- `a2_spotlight.lit` is dclive's only light with shadow objects. Its casters
  are `backup0`, `backup1`, `player0`, `player1` (HamCharacter) and
  `boombox.grp`.
- It is a fake spot (light_type 2) with projected_blend 0: the additive
  gobo. Its texture is `light_glow01.tex`, colour white, intensity 1,
  topradius 100, botradius 300, range 1000. It is its own colour_owner.
- `StageFloor.env`'s real lights are `DStagePointLight01.lit` and
  `a2_spotlight.lit`, with no approx lights.
- There is no shadow-casting projected_blend 1 light in dclive, so its
  "shadow" is the gobo the casters block: `(1 - s) · gobo · c69 · sat(N·L)`
  (section 3). It is not a multiply, so a shadow can be no darker than the
  gobo is bright where it falls.

**Whether it is selected and whether it lands, MEASURED.**

- Native selects it, as in 8.3.
- At its authored intensity 1, setting it to 0 changes the floor by roughly
  25-40 levels of R. The dancers move between the two shots, so this is
  approximate.
- Raised to 6 (`{a2_spotlight.lit set intensity 6}`), the floor shows the
  pool, and a soft dark shadow lies behind the lead's feet. That is the coverage term at work: the casters reach the
  map and the `(1 - s)` lands on the floor.
- Evidence: `round4/item5_dclive_shadow/gobo_x6_feet_zoom.png`,
  `gobo_x1_vs_off.png`.

**What is left, INFERRED, not fixed.** The mechanism works. What differs is
how much the gobo stands out from the floor's other light: at intensity 1
native's shadow is too faint to see against the lit floor, while retail
dclive/01 and 14 show soft smudges. Three leads, none tested:

1. The floor's other light is higher on native. The stage-floor meshes and
   their materials were not identified this round. `curved_floor.mat`, for
   example, is prelit and srcAlpha 0.85, so for a floor like it the prelit
   and environ terms set the base.
2. The venue's environment frame differs between the captures: retail's
   "warm" frames against native's normal state.
3. Retail's bloom amplifies the contrast. The lit floor blooms, and the
   shadowed part writes less mask.

Raising the light's intensity would be tuning, so nothing was changed.

### 9.6 Handed to the post lane

Every remaining peak gap measured in 9.1-9.4 sits after lighting. The
evidence is the raw scene (`DC3_RETAIL_POST=raw`), where the lit values
already have the right shape: black crowd, opaque stripes, a dim reflection.
These are leads with numbers, not a diagnosis of the post code.

1. **Red bloom strength or extent, streetside awesome.** The data is
   `streetside_performance_awesome.pp`: `bloom_color` (0.91, 0, 0) ×
   intensity 1.4. Since 9.1, native's mask is right: the stripes' alpha is
   0.57 × 2, clamped. Where retail reads (255, 40, 0) in the gaps between
   stripes, native reads (108..131, 0, 0). Over the crowd, retail reads
   (157..185, 26..29, 3..5) and native (50..53, 0, 1.5). On the floor
   between reflected stripes, retail reads (75..125, 12, 0) and native
   (55, 25, 33). Retail's bloom over the same black is about 2-3× native's,
   or it spreads further.
2. **Green crushed to 0, streetside awesome.** Across the lower frame,
   native's G is 0 where retail keeps 30-140: retail's stripes read
   (255, 41..143, 255), native's (204..216, 0, 228..250). The stripe
   material's colour is (1, 0, 0.5), and native's raw scene has the stripes
   at a median G of 10 (MEASURED, 5,212 stripe pixels). Lighting cannot
   multiply green into a green-0 material, so retail's 41..143 must come
   from the post chain. Retail's stripes clip at 255 in R and B, which
   points at a lift or a highlight roll-off toward white that native lacks.
   INFERRED, not tested.
3. **Warmth over the dancers in peak** (9.2) is the same red bloom as item 1
   over the dancers. It is not a character-light term.

**Revised after 9.7 (Xenia).** Items 1 and 2 were measured against retail
video, not against the debug XEX. Against Xenia running that XEX with the same
DTA values, native's post-on streetside stripes and gaps fall inside Xenia's
range, and Xenia's stripe green is 0 as well. Items 1 and 2 are **withdrawn as
post-lane leads**; the video's extra red and green are a capture difference
(state, exposure or encoder), not a native defect. Item 3 goes with them. The
lead that replaces them is the rink floor (9.7.2): raw lighting matches Xenia
and the post chain does not.

### 9.7 Against Xenia ground truth

The coordinator's Xenia frames (`archive/np-gfx-2026-11-14/xenia/INDEX.md`)
run the original debug XEX. They are pre-ramp, so native is compared with
`DC3_DISPLAY_GAMMA=off`. Two tools made the comparison raw against raw:

- `{rnd toggle_all_postprocs}` (`Rnd.cpp`, `mDisablePostProc`) turns the whole
  post chain off. It works in Xenia (sent over the DTA channel) and in native
  (`/api/dta/eval`), so both sides can be read with lighting only.
- The same DTA probes, sent to both, read the data each side is using.

Numbers are in 640×360 space, Rec.709 luma, averaged over three frames per
condition. Evidence: `round4/item1_stripes/xenia_toggle/`,
`round4/item6_rink_floor/` (Xenia frames stay in `archive/…/xenia/`).

#### 9.7.1 Streetside peak: matched (MEASURED)

**The inputs are identical** (Xenia probe vs native probe, awesome state):

- `PeakCity.env` ambient 12338515 (0xBBGGRR: 0.325, 0.271, 0.737) on both.
- `streetside_peek_pink1.mat` colour 8388863 (1, 0, 0.5), blend 3, intensify 1.
  Its alpha is beat-keyed: 0.563 in the Xenia frame, 0.57 in native.
- `streetside_performance_awesome.pp`: bloom_color 232 (0.91, 0, 0), intensity
  1.4, threshold 8.
- `peak2`, `peakOrange`, `peakDirectional` and `crowd.env`: the same values.

**Postprocs off, raw lighting:**

| | Xenia | native (engine e391f95) |
|---|---|---|
| stripe median rgb | (132..151, 0..1, 165..196) | (145..175, 4..29, 178..227) |
| stripe R/B | 0.70..0.90 | 0.79..0.81 |
| stripe region mean | (34..44, 11..13, 39..65) | (39..55, 11..25, 49..68) |
| whole-frame mean | (34..37, 15..18, 44..58) | (35..45, 17..24, 51..62) |

**Post on** (native engine e391f95, before the noise upload `ecd579f`):

| | Xenia | native |
|---|---|---|
| stripe region mean | (126..134, 8..10, 59..74) | (146, 13, 71) and (94, 5, 47) |
| stripe median | (210..226, 0, 215..227) | (226, 18, 215) |
| whole-frame mean | (87..96, 11..19, 56..66) | (97, 18, 63) and (60, 11, 51) |

Native's two post-on shots differ because the stripes' alpha pulses on the
beat. Before 9.1 the brief's stripe sample read (34, 1, 34) against Xenia's
(206, 1, 72); the single-frame r4 sample afterwards was (139, 7, 109). The
spread between those and the table above is camera, box and the beat.

**So 9.1 closes item 1.** The fix (`kIntensifyScalesAlpha`, engine `d854f7e`)
is generic: every intensify world material on DC3 now draws at twice its
alpha, which includes DCI's peak stripe panels. That is reported to the post
lane, which owns DCI peak, through the coordinator, so it does not chase the
same deficit through the bloom.

**Per stage, on the merged build (engine 17ba5d4, with the post lane's noise
upload `ecd579f`), MEASURED.** The coordinator asked for this after the post
lane found DCI's peak deficit was the noise overlay that never ran: the
streetside awesome post-proc authors noise too (`noise_intensity` 1,
stationary). The same DTA state files were set on
`streetside_performance_awesome.pp` on both sides, cycled once a second in the
forced awesome state (Xenia frames labelled by the last applied state). Scene
box luma and RGB, then the magenta stripe pixels (R>90, B>90, G<60):

| stage | Xenia scene (RGB) | native scene (RGB) | Xenia stripes | native stripes |
|---|---|---|---|---|
| full | 33.3 (92, 14, 55) | 34.0 (87, 16, 60) | 13.4% (197, 14, 201) | 12.2% (190, 9, 201) |
| noise off | 25.5 (73, 10, 44) | 24.0 (65, 10, 45) | 11.4% (177, 11, 192) | 10.5% (177, 8, 186) |
| bloom off | 22.8 (37, 15, 56) | 24.1 (38, 16, 61) | 8.0% (166, 15, 220) | 9.8% (172, 7, 223) |
| bloom, noise off | 17.0 (32, 9, 48) | 17.4 (29, 11, 47) | 9.5% (144, 6, 206) | 8.2% (145, 9, 208) |
| all off | 20.3 (34, 14, 48) | 21.5 (32, 16, 48) | 8.2% (135, 13, 203) | 8.1% (143, 8, 191) |

n = 17..21 frames per stage per side. Every stage agrees within a few levels,
and so does each stage's step: the bloom takes red from about 37 to 92 on
Xenia and 38 to 87 on native, and the noise adds about 8 scene luma on both.
**So the red is not lost at any stage on the merged build.** The brief's
(206, 1, 72) against (34, 1, 34) was a pre-fix native frame (before
`d854f7e`, and before `ecd579f`), single shot, different camera.

Evidence: `round4/item1_stripes/stage_decomposition_m3/` (stats.txt, state
files, `tools/stripe_stats.py`, native frames; Xenia frames stay private).

An earlier single-run reading had native's raw stripes a little greener than
Xenia's (median G 4..29 against 0..1). The per-stage run above does not
reproduce it (all off: G 8 native, 13 Xenia), so it is not carried as a gap.

#### 9.7.2 Rink normal floor: the deficit is the bloom stage (MEASURED)

**The inputs are identical** (Xenia vs native probes, normal state):

- `Stage_Carpet.env` ambient 7360314; its real lights `Stage.lit` 7524062 and
  `Stage1.lit` 13091466.
- `stage_spotlight.lit`: white, intensity 3, top/bottom radius 100/200, range
  175, gobo `light_glow01`.
- `StageCarpet_lightCatcher.mesh`: blend 6, white. `Rink_lightCatcher.env`
  ambient 6118749. The mirror-ball spots are white.

**Floor band luma (paused native; Xenia three frames per condition):**

| condition | Xenia | native |
|---|---|---|
| post on | 148.7..155.6 | 114.4..119.5 |
| postprocs off | 113.9..116.5 | 113.7..118.1 |
| off, light catcher hidden | | 120.8 |
| off, stage spotlight hidden | | 78.1 |

Whole-frame means agree: Xenia post on (115..118, 85..89, 73), off
(83..90, 60..62, 49..52); native post on (96, 71, 59), off (87, 62, 49).

- Raw lighting matches within a few levels. The −19% is all post chain:
  Xenia's chain raises the floor by about +35 luma, native's by about +1.
- The gobo pool (8.1) is in place: hiding the spot drops the floor by about
  40 levels. The light catcher costs about 3. Neither is the gap.
- The data active on that frame is `rollerrink_performance_high.pp`: bloom
  colour (0.97, 0.84, 0.62), threshold 8, intensity 1.6, no streak;
  saturation −10, contrast 5, colorxfm on, vignette on. A warm bloom that
  lifts a mid-grey floor by +35 needs a threshold or extraction that lets
  ~115-luma pixels through. That is a lead for the post lane, not a
  diagnosis.
- Caveats: the camera shots differ slightly (native's paused frame against
  Xenia's far shots), and native draws the gameplay feedback ring and flame
  under the lead where Xenia shows only a faint ring. The flame is gameplay
  state (scoring), not lighting. Both are outside the floor band measured.

**Which stage, on the merged build (engine 17ba5d4, MEASURED).** The post
lane's per-stage method (`lanes/post/round4/tools`): the same DTA state files
set on `rollerrink_performance_high.pp` on both sides, cycled once a second
through the high state; Xenia frames labelled by the last applied state. Scene
box and the lower third of the frame (the floor), luma, 640×360:

| stage | Xenia scene / floor | native scene / floor |
|---|---|---|
| full chain | 97.7 / 151.0 (n 14) | 79.8 / 118.8 (n 17) |
| bloom off | 72.9 / 119.1 | 67.7 / 114.9 |
| vignette off | 99.5 / 157.5 | 81.9 / 122.1 |
| colour matrix off | 99.2 / 149.3 | 81.9 / 118.1 |
| all off | 77.0 / 122.2 | 71.8 / 118.3 |

- With bloom off the two agree within 5 levels, and the vignette and the
  colour matrix move both sides by the same few levels.
- **The bloom is the stage that differs.** On Xenia it adds +24.8 to the scene
  and +32 to the floor; on native +12.1 and +3.9. The same data reaches both:
  colour (0.97, 0.84, 0.62), threshold 8, intensity 1.6, no streak.
- The floor is mid-grey (~115-120 luma raw). Native's bloom keeps far less of
  that level than Xenia's does, while on streetside (9.7.1), where the bloom
  source is bright saturated stripes, the two agree. So the lead is how the
  native bloom extracts mid-tones (the threshold or the extraction curve), not
  its strength. INFERRED; not tested in the post code, which this lane does
  not own.
- The rink's normal state (`high`) has noise 0, so engine `ecd579f` (the
  noise upload) does not touch this gap. Re-measured on the merged build:
  native's paused toggle reads the same as before (lower third 110.1 post,
  107.3 off).

Evidence: `round4/item6_rink_floor/stage_decomposition_m3/` (stats.txt, the
state files, native frames; the first Xenia attempt stalled before gameplay
and was stopped, the second, `xrr2`, is the one measured).

Handed to the post lane: **rollerrink high: the bloom adds +32 floor luma on
Xenia and +4 on native; raw lighting, grade and vignette match.**

#### 9.7.3 Flare-lit regions are not comparable against default Xenia

The post lane measured (round 4) that the Xenia fork answers every occlusion
query with `query_occlusion_fake_sample_count = 1000`, so `RndFlare` draws
large flares at about 1/80 strength there; native matches Xenia with the count
raised to 1e6, and the hardware footage. Any bulb, lamp or glow region in this
section's Xenia frames is therefore not ground truth. That covers dclive's
bulb wall (the brief's −47%, ranked weak), which this lane did not measure.

### 9.8 Verification and frame time, round 4

Final heads: engine `np-gfx-light` a9b72e5 plus this doc commit (code identical
to 17ba5d4, the merge of np-gfx c123b34), dc3 `np-gfx-light` pinned to it.

- dc3 `scripts/native_test.sh` at dc3 c4262a3a5 (engine a9b72e5): registered
  1010, executed 940, passed 940, failed 0, skipped 70, budget 70, rc 0.
- rb3-xenon `rb3-render`, engine np-gfx c123b34 against this branch: 11 of 11
  frames byte-identical. `kIntensifyScalesAlpha` is false in
  `RndShape_RB3Wii.h`, so rb3-xenon keeps the UI-only alpha rule.
- rb3 (tree 1dee2c1f7) built against this branch: 157 of 158 pass. The one
  failure, `native_compat_census.check`, is that tree's own ledger being
  stale: it fails the same way against the base engine c123b34, and rb3
  main's census script reads 454 flags, regen clean, on both engines. An
  earlier run also failed `PointTestPassTest.CollectThroughTakesOnlyBatchesUpToSeq`
  once while the dc3 gate ran on the same GPU; it passed 15 of 15 alone and
  in the final run (it times a busy GPU job).
- getenv census: `git diff c123b34 HEAD -- src include` adds no `getenv`.

**Frame time, MEASURED** (`round4/frametime/`). OMG, headless, 10 s sampled
20 s into gameplay, 3 rounds interleaved, RTX 3090, load average 26-38 on 32
cores. Same base, the flag alone: `kIntensifyScalesAlpha = false` against the
branch (engine e391f95's code; rebuilt from source it is byte-identical).

| venue | flag off: main ms / GPU est ms (median) | branch |
|---|---|---|
| streetside | 3.94 / 0.75 | 3.99 / 0.76 |
| houseparty | 3.36 / 0.70 | 3.34 / 0.52 |
| rollerrink | 3.99 / 0.61 | 3.67 / 0.67 |

No difference beyond the run-to-run spread, as expected: the change is a
uniform value, with no new draw, pass or bind group. A first A/B against the
pre-merge base binary showed +0.2..0.45 ms of GPU estimate; that binary
predates the np-gfx 34cb235 merge (velocity blur), so it compared two code
bases, not this change.

## 10. Round 5: dclive's dancer shadows, and streetside's peak sky

Ground truth for this round is Xenia running the debug XEX, compared against
native with `DC3_DISPLAY_GAMMA=off`; the HUD anchor (rows 104:180, columns
1150:1230) reads luma 21.9 on both in every pair below. Xenia's peak state is
forced with `{handle (game_panel force_venue_state awesome)}` at song time 12
s, both sides pause at 30 s, `force_shot area1_far02.shot` and
`disable_picking TRUE`, and every toggle is a DTA eval followed by a capture
(native) or a MARK interval (Xenia, frames averaged per label). Evidence:
`archive/np-gfx-2026-11-14/lanes/light/round5/` in dc3 (gitignored).

### 10.1 Studio 675 (dclive) dancer floor shadows: not a native defect

MEASURED. Protocol `dl_common.txt` (native `n3/`, Xenia `x1/`): raw and
post, the gobo hidden (`a2_spotlight.lit set_showing FALSE`), the stage
floor hidden (`DLV_StageFloor01/02.mesh`), and the shadow casters cleared
(`a2_spotlight.lit clear (shadow_objects)`). Luma:

| measure | Xenia | native |
|---|---|---|
| gobo pool, raw | 163.6 | 166.1 |
| gobo term (R1 − R_noGobo) | 43.4 | 41.3 |
| shadow depth (R_noShadow − R1) | 21.2 | 23.6 |
| floor outside the pool, raw | 118.0 | 119.9 |
| gobo pool, post | 201.6 | 203.0 |
| shadow pixels, post | 167.7 | 166.5 |
| floor band with the floor hidden | 64.5 | 64.4 |

Every term agrees within a few luma: the floor's base, the gobo's
contribution, the shadow depth and the post chain. The probes (env ambients,
`DStagePointLight01`, `a2_spotlight` colour, intensity and caster count, the
transparent stage material) read the same on both. The darker smudges in the
retail footage (`dclive/01`, `dclive/14`) are therefore not this state; they
are INFERRED to come from a different light preset or camera, which this round
did not chase. Nothing changed. Sheets: `x1_sheet.png`, `n3_sheet.png`,
`x1t_shadow.png` / `n3t_shadow.png`, `dl_floor_x_vs_n.png`.

### 10.2 Streetside peak: the sky is screen-aligned

**Decomposition (MEASURED, protocols `ss_*`, `ss2_*`; native `n5`, `n8`,
Xenia `x2`, `x3`).** Native and Xenia read identical inputs: env ambients,
fog, the floor and sky materials, the stripe lines' showing state, tex_xfm,
tex_gen and tex_wrap.

- The road is not drawn in peak: `default_environment.grp` is hidden, so
  hiding `Street.mat`'s meshes changes nothing on either side. The road is
  ruled out.
- Order: `peak_reflection.refl` (WorldReflection::DrawShowing, mirrored camera,
  draw mode 8), then the black placeholder at 50%, then the direct scene. The
  floor is the reflection times 0.5; with the reflection off it is black on
  both.
- With the sky hidden (`peak_sky_1 set_showing FALSE`), native matches Xenia
  within 0.5 luma everywhere on the floor. The whole floor difference was the
  sky.

**Cause (MEASURED).** `sky_peak_1.mat` and `sky_peak_2.mat` read
`screen_aligned 1` (`peak_street.mat` 0). That is ShaderOptions bit 13
(`RndShaderStandard::CalcShaderOpts`), and the shipped permutation
`standard_0000000000002010.vs` (xenia-gpu-shader-compiler ucode,
`round5/shdis-Lbmu/`) replaces the vertex UV with the vertex's own screen
position before the texgen rows:

```
rcp r0.x, r2.w ; mul r0.xy, r2.xy, r0.x ; mad r0.xy, r0.xy, 0.5, 0.5 ; subsc r0.z, 1, r0.y
dp2add o0.x, r0.zx, c20.yx, c20.w ; dp2add o0.y, r0.zx, c21.yx, c21.w
```

`standard_0000000000000010.vs` feeds the vertex UV to the same two dp2adds.
A ruler test on Xenia confirms it: the sky equals the texture sampled at
u = screen x / 1280 (`ss5_*`, `x6/`, `pred/x6_ruler*.png`). Native ignored
the flag and mapped the cylinder's own UVs (u = 0.5 + angle/180 around, v
down its 6124-unit height), so:

- direct, it showed the texture's starfield where retail shows the
  searchlight fan;
- reflected, it sampled stars and beams where retail samples the texture's
  bottom third, navy (24, 25, 54). Navy times the reflection ambient
  (0.22, 0.216, 0.255) is (5.3, 5.4, 13.8); Xenia reads (5.2, 5.6, 13.2) on
  the near floor. That was round 4's "blue lift" (section 9.3).

**Fix (engine 2e26676, comment 141d468).** `rndshape::MatScreenAligned`
(RndShape_DC3.h: the material's GetScreenAligned under the DC3 retail model,
false for rb3-xenon; RndShape_RB3Wii.h: false) sets bit 9 of `colorMod.w` in
both material builders (`FillScreenAligned`, after FillColorMod), and
`vs_main` / `vs_skinned` pass `texGenSourceUV(vertex.uv, clipPos)` to
computeTexGenUV. No new uniform, bind group, buffer or getenv.

**Test (dc3 bb3c4d9ed, `native/tests/test_screen_aligned_texgen.cpp`).** The
flag through both builders, and the engine's own WGSL on a GPU against the
microcode's arithmetic for screen-aligned and plain materials. Five sabotages,
each caught: the WGSL always returning the vertex UV; no FillScreenAligned for
the primary; none for a NextPass; v not flipped; MatScreenAligned false
(`round5/sabotage_summary.txt`).

**Before and after (raw, paused, R1 = peak with post off; `x3` Xenia, `n8`
before, `n19` after):**

| region | Xenia | native before | native after |
|---|---|---|---|
| near floor L (640:715, 0:230), RGB | (5.1, 2.8, 9.3) | (16.6, 16.1, 25.1) | (5.0, 2.8, 9.2) |
| far floor L (470:540, 0:230), RGB | (15.1, 3.1, 19.2) | (20.7, 10.7, 27.5) | (14.4, 2.9, 18.7) |
| floor band (440:715, 0:1050), luma | 12.8 | 16.2 | 12.5 |
| sky top (50:120, 300:900), luma | 62.0 | 32.2 | 63.4 |
| sky term (R1 − R_noSky) vs Xenia, mean abs, sky | — | 11.4 | 2.1 |
| sky term vs Xenia, mean abs, floor | — | 3.9 | 0.1 |
| whole R1 vs Xenia, mean abs, floor | — | 6.5 | 2.7 |

The remaining whole-frame floor difference is the dancers (the two sides
pause on different poses), not the sky. Sheets:
`pred/r5_R1_before_after.png`, `pred/r5_skyterm_x3_n8_n19.png`.

### 10.3 Finding, not fixed: tex_xfm translation x has the wrong sign

MEASURED with the sky's own material (`ss4_*`, Xenia `x5`, native after the
fix `n20`): setting `(tex_xfm x) 0.25` on `sky_peak_1.mat` moves the sky's
features 319 px right on Xenia and 307 px left on native (1280 × 0.25 = 320).
ty, tex_gen 0 and tex_wrap agree (sky-top luma within 2 on every probe).
Native's FillTexGen puts `xfm.v.x` into the row as `+tx`. The retail rows
come from `MakeTex3` (100% in report.json), which for an identity rotation
gives u − tx, v + ty. Not fixed here: streetside's sky has tx 0, the general
form under rotation needs MakeTex3's rows derived properly, and rb3-xenon
shares the code, so a fix needs the DC3 gate and its own test. Any DC3
material that scrolls its texture in x through tex_xfm (an anim on
`tex_xfm`) scrolls the wrong way on native.

### 10.4 Tooling notes from this round

- Light presets and anims overwrite DTA sets of env ambients and light
  colour or intensity within a frame or two, `animate_from_preset` or not;
  measure by hiding objects (`set_showing FALSE`) instead.
- `force_shot <name>.shot` plus `{$hamdirector set disable_picking TRUE}`
  holds one camera on both sides.
- Iterate with the class name: `{$W iterate Mat $m ...}`. DTA loops are
  `{do ($n $t) {set $n 0} ...}`.
- tex_xfm keys are `(tex_xfm x|y|z|yaw|...)`.
- Xenia frames carry a debug GLITCH banner in rows 25-39; measure below row
  45.
- zsh does not word-split `$VAR` in `for`; generate protocol files in Python.
- The native eval server listens on the sweep's port base + 1.
- `native_song_sweep.py` needs `--stall 1400` for a paused session, or its
  hang detector kills it.
- Retail shader binaries with a constant prefix (first bytes zero) need
  `tail -c +65` before `xenia-gpu-shader-compiler --shader_input_type=vs
  --shader_output_type=ucode` will disassemble them.
- Native `set use_environ` / `set prelit` on a material had no visible
  effect in a live session while tex_xfm and tex_gen sets did; INFERRED that
  those two are not re-read into the material uniforms after load. Not chased.

### 10.5 The Xenia MAX-alpha caveat (post lane §35), checked against this round

Xenia applies MIN/MAX alpha blending without the blend factors, so a
Multiply draw that writes the bloom mask over-blooms the floor under it on
Xenia. Neither item here depends on it:

- Every comparison in 10.1 and 10.2 that decides a cause is raw (post off on
  both sides, `{rnd toggle_all_postprocs}`), where bloom does not run.
- No Multiply material that writes alpha covers either floor. dclive's
  Multiply materials are `DLV_PeakTransSetFloor01.mat` and
  `DLV_PeakTransStage01.mat` (peak only, and `alpha_write` 0); streetside's
  are `Building2_shadows.mat` and `building1_shadows.mat` (`alpha_write` 0).
  (`milo-mat-export`; `round5/dclive_mats/`, `matx-9s4K/`.)
- dclive's post-on pool and shadow pixels agree within 1.4 luma anyway.

### 10.6 DCI normal `floor_r` (handed over by the post lane): one-frame noise

The post lane read `floor_r` 40.1 (and 41.4 on another build) against Xenia
42.3..44.9 (mean 43.4, n=14), each from one native frame at camera
correlation 0.46-0.48. MEASURED with the post lane's own scorer
(`p55_series.py ... 0.45 normal`, full chain, `DC3_DISPLAY_GAMMA=off`, this
branch with the screen-aligned fix, which DCI does not use):

- The Xenia reference camera is native's `P1_area1_far03.shot`
  (`cur_shot` at the matching frames), not a `near01` shot: forcing
  `P1_area1_near01.shot` or `area1_near01.shot` gives correlation 0.17 at
  best.
- On that camera `floor_r` swings 24.8..51.6 across frames with framing and
  dancer reflections; the region is mirror floor.
- Native frames at correlation >= 0.45, from three sessions (director's own
  cuts `n21`, `n22`; `P1_area1_far03` forced, `n23`): 41.3, 41.8, 42.0, 42.0,
  42.5, 42.9, 43.5, 43.8, mean 42.5 (n=8), against Xenia's mean 43.4.

So native sits about one level under Xenia's mean, overlapping its range,
and at the 42.4 the Xenia lane measured before round 4, so nothing points
at round 4's intensify change. Not chased further.
Evidence: `round5/dci_floor_r/` (`dci_shots.png` shows the reference beside
the forced near01 shots).

### 10.7 Verification, round 5

Heads: engine `np-gfx-light` f1ef982 plus this doc commit (np-gfx 51fe8b2
merged), dc3 `np-gfx-light` 6ff058d59 (np-gfx 8c7e3242c merged; the engine
pin stays np-gfx's 845adf8).

- dc3 `scripts/native_test.sh` at 6ff058d59: registered 1054, executed 984,
  passed 984, failed 0, skipped 70, budget 70, rc 0. The three
  `ScreenAlignedTexGen*` tests ran (not skipped).
- rb3-xenon `rb3-render`, engine 989cd30 (this branch before the fix)
  against 2e26676: 11 of 11 frames byte-identical, all gates passed on both.
- rb3 (tree 1dee2c1f7) built against the fix: 157 of 158 pass; the one
  failure is the same stale `native_compat_census.check` ledger as round 4.
- No new render pass, bind group, buffer or getenv.

## 11. Round 6: texture transforms and the vertex-shader feature bits

Same protocol as section 10 (Xenia debug XEX vs native with
`DC3_DISPLAY_GAMMA=off`, both paused at 30 s in streetside's peak state on
`area1_far02.shot`, postprocs toggled off). Evidence:
`archive/np-gfx-2026-11-14/lanes/light/round6/` in dc3 (gitignored).

### 11.1 tex_xfm: retail builds the texgen rows with MakeTex3

For tex_gen Xfm and XfmOrigin, NgMat::SetupShader builds the vertex shader's
texgen matrix with `MakeTex3(mTexXfm, centred, m)` (dc3
`src/system/rndobj/Mat_NG.cpp`) and uploads it to c20/c21. Every shipped
standard permutation has texGen 0 and evaluates (xenia ucode of
`standard_0000000000000010.vs`)

    u' = u * M.x.x + v * M.y.x + M.w.x
    v' = u * M.x.y + v * M.y.y + M.w.y

MakeTex3 negates the off-diagonals (`out.x = (m.x.x, -m.x.y)`,
`out.y = (-m.y.x, m.y.y)`) and, for the centred mode (Xfm), applies the
translation BEFORE the matrix with x negated:
`w = out.x * (-tx - 0.5) + out.y * (ty - 0.5) + 0.5`. The origin mode
(XfmOrigin) uses `w = (tx, ty)`. The native port put the Transform's rows in
directly, which gets four things wrong:

- the x translation moved the image the wrong way (a +0.1 x moved the sky
  127 px right on Xenia and 126 px left on native);
- a scaled scrolling map scrolled at 1/scale of retail's rate (the
  translation is applied before the scale);
- a rotation combined with a translation rotated the wrong way round its
  centre, and a rotation with non-uniform scale came out sheared;
- reflections (det < 0, e.g. `cubez2.mat` / `starz2.mat`) flipped the other
  axis.

A pure scale and a pure rotation with no translation come out the same both
ways. The fix is `rndshape::MatRetailTexXfmRows` (DC3 shape; the RB3Wii shape
returns false and keeps the old rows), called by `FillTexGen` for the primary
material and every NextPass. It writes MakeTex3's rows rewritten into the form
the shader's two existing branches read, so the WGSL is unchanged. Test:
dc3 `native/tests/test_retail_tex_xfm.cpp`, which links the image's own
MakeTex3 as the oracle (CPU builder rows, translation signs, and the engine's
WGSL on the GPU); 7 of 7 sabotages caught.

MEASURED, the shift probe on `sky_peak_1.mat` (screen-aligned, shipped tex_xfm
y 0.2). Each probe changes one tex_xfm property; "dcorr" is the correlation
between native's and Xenia's difference images (probe minus base, the same
session on each side, rows 50:400, columns 0:1040), so it does not depend on
the two runs pausing on the same frame:

| probe | Xenia dx | native after | native before | dcorr after | dcorr before |
|---|---|---|---|---|---|
| x 0 -> 0.1 | +127 px | +127 px | -126 px | +0.949 | +0.588 |
| y 0 -> 0.1 | -- | -- | -- | +0.974 | +0.974 |
| y 0.1 -> 0.2 | -- | -- | -- | +0.896 | +0.911 |
| yaw 0 -> 20 (y 0.2) | -- | -- | -- | +0.960 | +0.555 |
| x_scale 2 on yaw 20 | -- | -- | -- | +0.967 | +0.070 |
| x_scale 2 + x 0.1 | -- | -- | -- | +0.976 | +0.579 |

The y sign was already right (a y change brightens or darkens the sky
gradient rather than translating it, so there is no dx; the two builds agree
with Xenia equally). The rotation direction and the scale-times-translation
order were wrong before and match after. The absolute frame-to-frame
correlation between the native and Xenia frames is not comparable between the
two native builds: their sessions paused 0.16 s apart (30.19 s vs 30.04 s) and
the dancers' poses differ.

Shipped use (MEASURED, `milo-mat-export` over 1,161 world/ui milos plus 187
char milos; 15,757 materials). Only two tex_gen modes are used: Xfm
everywhere and one XfmOrigin (`ui/resource`). Non-identity xfm per area,
among others: world/shared 576 of 1,594, dci 65/147, dclive 49/140,
houseparty 36/141, rollerrink 34/151, streetside 40/131, throneroom 37/104,
ui/hud 85/550, ui/party_mode 24/229, ui/background 12/12, char 5/1,413.
383 static rows (109 distinct names) sample a different UV under the fix
(more than 1e-4, wrap-aware). Animation is almost all PropAnim: 4,294
material PropKeys key tex_xfm (x_scale 2,314, y 1,135, x 573, y_scale 237);
the only two RndMatAnims carry no transform keys. The most visible:

- HUD meters that fill by x_scale with an x offset: `battle_meter` juice
  (x_scale up to 13,664, x -0.49 -> -0.46), `mini_game_flashcard` bars,
  `practice_options` `meter_juice` (x -0.175 -> 0.15), `point_chase`
  result fills: on retail the offset is scaled, so fills move at retail's
  rate and from the authored side.
- `score_star` `star_wipe` (y_scale 0.5, y -0.83 -> -1.31): half native's
  old rate.
- `phrase_meter` `ring_01..12` (x_scale 0.01, x -0.55, clamp): retail
  samples u near 0.5, native the clamped edge.
- `flashcard_bg_2ndpass.mat` (every move card; `beat.anim` keys x 0 -> 2):
  the sheen sweeps the other way.
- UI `background_left/right`, `bg_eq` (beat anim x ~6.2, x_scale 0.3), the
  party_mode sheens and crew_bg scroll.
- Venues: streetside `Building1_brick.mat` (scale 1.5, x .07, y 1.15),
  `peak_spiral` `spiral_01/02` (x_scale 5, x .6), throneroom `peak_carpet_*`
  and `SkyStars.mat`, `projection_noise` (scale 4.3, y .25) in every venue,
  `curved_floor_refract`, `DCI_ElevatorLazer` (x 0 -> 2, reversed),
  `DLV_Refraction` and `timeywimey_refraction` (rotated with non-uniform
  scale).

NOT MEASURED against Xenia: none of the nine affected streetside materials
changes a pixel on `area1_far02.shot` in the peak state (each set to zero
translation: 0 changed pixels beyond the session's own drift), and the HUD
materials (flashcards, phrase meter) live in panel subdirectories that
`find_obj $W` does not reach. A before/after on a shipped material needs a
shot that frames it (or a milo-viewer comparison, which has no Xenia side).

### 11.2 The vertex-shader feature bits the shipped cache uses

MEASURED from the 4,600 standard keys in `xbox_shaders`: for each option bit,
the key pairs that differ only in that bit, and whether their vertex / pixel
microcode differs. texGen is 0 in all 4,600 keys (no Sphere, Projected or
Environ permutation exists, and no shipped material uses those modes).
Bits whose VS differs:

| bit | name | pairs | VS differs | native |
|---|---|---|---|---|
| 0 | perPixelLighting | 231 | 231 | yes (sections 2-5) |
| 2 | specular | 401 | 54 | yes (retailSpec; per-vertex spec) |
| 3 | environMap | 380 | 70 | yes (reflection in the FS) |
| 5 | normalMap | 833 | 833 | yes (tangent frame) |
| 8 | prelit | 130 | 130 | yes |
| 12 | skinned | 1,162 | 1,162 | yes (vs_skinned) |
| 13 | screenAligned | 50 | 50 | yes (round 5) |
| 17 | approxLights | 110 | 110 | yes |
| 18 | fog | 182 | 182 | yes |
| 19 | shadowBuffer | 1,210 | 1,210 | yes (ShadowPass) |
| 25 | billboard | 12 | 6 (with point lights only) | billboards yes; their point-light normal not checked |
| 38 | enableAO | 12 | 10 | yes (retailAOFactors) |
| 40-41 | numPoint | 1,798 | 272 | yes (retailPoint*) |
| 46 | refractWorld | 10 | 10 | yes (FillRefract) |
| 54 | flipNormal | 260 | 260 | **no -> fixed, 11.3** |
| 60 | shockwave | 2,296 | 2,296 | no; never active (below) |

Every other bit present changes the pixel shader only. Absent from the cache
entirely: extrude, velocity, fitToSpline, splinePulse, furDetail,
softDepthBlend, noiseMidtone, spotlight. The cache looks like a generated
permutation set (shockwave, shadowBuffer and pseudoHDR each have a twin for
nearly every key), so a bit being in the cache does not prove it is used.

- shockwave (bit 60) needs `RndShockwave::sSelected`, i.e. a Shockwave
  object. MEASURED: a grep of every decompressed shipped milo (5,399 files)
  finds no `Shockwave` class entry (the `shockwave.mesh/.mat/.tex` objects in
  `phrase_meter`, `chars_base`, `bustamove`, `boxyman` and
  `timey_wimey_elements` are ordinary meshes), and nothing in the source
  creates one. Not visible; no fix.
- fitToSpline / splinePulse (bits 55-56) need `RndSpline::sGlobalDefaultSpline`,
  set by a Spline loaded with its sync flag. Spline objects exist only in
  `ui/campaign` (`campaign_songselect`, `campaign_master_quest_song_select`);
  the cache has no permutation with these bits, so INFERRED the flag is not
  set in shipped data. Native has no implementation.

### 11.3 flip_normal

RndShaderStandard::CalcShaderOpts sets bit 54 inside its per-pixel block (the
material uses the environment, is per-pixel lit, and lights exist) when
`GetCull() == kCullBackwards`. In the ucode pairs
(`standard_0040000080020015.vs` vs `..._0000000080020015.vs`,
`standard_0040000000020037.vs` vs `..._0000000000020037.vs`) the only change
is the world normal negated in the vertex shader; with a normal map the
bitangent crossed from it flips too, and the tangent does not. Native had no
flip, so a backwards-culled lit surface was shaded from behind.

Fix: `RetailMatTerms::flipNormal` (set by the DC3 shape inside the per-pixel
block), carried in colorMod.w bit 10 (set after FillColorMod assigns w), and
`vsOrientNormal` in both vertex shaders. The RB3Wii shape never sets the
flag. Test: dc3 `native/tests/test_retail_flip_normal.cpp`, 8 of 8 sabotages
caught. Shipped use (MEASURED, census): 87 materials, all in `char/main` --
every dancer's and backup dancer's `*_hair_backface(s).mat`, plus feather,
visor and no-cull backfaces (19 with a normal map). No world or UI material
qualifies (the one world material culled backwards, `basket_cull.mat`, is
per-vertex lit).

NOT MEASURED against Xenia: the streetside crops of the three dancers' heads
(Xenia, native without, native with) show no regression, but two native
sessions never pause on the same frame (30.14 s vs 30.19 s here), so the hair
backfaces' change cannot be separated from the pose change, and there is no
DTA switch that removes the flip alone (changing `cull` also changes which
faces are drawn).

### 11.4 use_environ / prelit set through DTA on a live material

Not a native cache. `SYNC_MAT_PROP` asks `RndMat::IsEditable(<prop>_edit_action)`
first, and a material with a MetaMaterial refuses any property the
MetaMaterial does not mark editable (value 2), returning true without
setting it and printing one `MILO_NOTIFY_ONCE` ("Unable to set property
use_environ in Mat sky_peak_1.mat ... Not allowed by MetaMaterial
v_basic_scrn_algn.mmat"). Retail does the same. MEASURED live: on
`sky_peak_1.mat`, `set use_environ` and `set prelit` read back unchanged;
after `{$mat set metamaterial ""}` the same sets read back changed. For a
live A/B probe, clear the metamaterial first (or set the MetaMaterial's
`<prop>_edit_action` to 2). tex_xfm is editable under the venue
MetaMaterials, which is why the tex_xfm probes above worked.

### 11.5 What is left, ranked by where it shows

1. The shipped HUD and menu tex_xfm cases (11.1) have no before/after
   against Xenia yet: the flashcard sheen, phrase-meter rings, score-star wipe
   and the meter fills live in panel subdirectories that `find_obj $W` does
   not reach, and the venue cases are not framed by the peak-state probe
   shot. Needs a probe that addresses panel dirs, or a shot per venue.
2. Billboards lit by point lights (bit 25 changes the VS only when numPoint >
   0; 6 pairs): native draws billboards but its point-light normal for them
   is unchecked against the ucode. Venue particles/billboards near point
   lights.
3. fitToSpline / splinePulse: no native implementation; INFERRED unused
   (no cache permutation; Splines only in two campaign UI milos).
4. shockwave: no native implementation; MEASURED never active.
5. tex_gen Sphere / Projected / Environ: native approximations unchanged;
   MEASURED unused by shipped materials and absent from the cache.

### 11.6 Verification, round 6

Heads: engine `np-gfx-light` 65c12c7 (code at 9b98a27; np-gfx ab2be8a merged),
dc3 `np-gfx-light` d596f3c16 (np-gfx fd56094e0 merged; the engine pin stays
570205a).

- dc3 `scripts/native_test.sh` at d596f3c16 against engine 65c12c7:
  registered 1060, executed 990, passed 990, failed 0, skipped 70, budget 70,
  rc 0.
- rb3-xenon `rb3-render` (tree a3ddb13ea), engine 65c12c7 against np-gfx
  ab2be8a: 11 of 11 frames byte-identical, all gates passed on both.
- rb3 main a49c0afce built against engine 65c12c7: 158 of 158 pass,
  `native_compat_census.check` included.
- No new render pass, bind group, buffer or getenv.

## 12. Round 7: the residuals that remain once post is off

Round 7 took the census rows that were still open with post off, plus one
mechanism question (whether hidden spotlights still light on native). Each
row was screened against the original under Xenia. Evidence, sheets and tools
are in dc3-decomp
`archive/np-gfx-2026-11-14/lanes/light/round7/` (`README.txt`,
`census_b5_b6_m_max_xenia.txt`, `sheets/`, `evidence/`).

### 12.1 Screening method

- **Frozen, lit, post off.** Both sides pause at song second 20 and force the
  venue state at 12 s. Both pin the shot with `force_shot` and
  `disable_picking TRUE`. A beat-driven `PropAnim` is frozen with
  `stop_animation` and `set_frame` on that anim only. Never freeze every
  PropAnim: freezing them all at frame 3 hides whole venues.
- `{rnd toggle_all_postprocs}` turns post off **and clears the spotlights**,
  so "raw" equals "raw with spots hidden".
- Native runs with `DC3_DISPLAY_GAMMA=off` and the still performer. A Xenia
  label is the mean of the frames between its MARK and `end_` MARK (3 s, about
  180 frames). A native label is one frame.
- Box values are mean luma (Rec.709 on stored sRGB) in 640x360 coordinates.
  The noise floor is 3.4% luma / dE76 2.2.
- **Control for the Xenia rule.** A native build with
  `rndshape::kAlphaBlendMax = true` (RndShape_DC3.h) takes Xenia's
  factorless-MAX destination alpha. Retail applies each draw's own blend
  equation. The bloom mask reads destination alpha, so a post-on gap that
  closes under this build is Xenia's, not ours (post lane: `dc3-backend-for-rb3-wii.md`
  §37). Destination alpha does not reach a post-off frame, so this build
  cannot explain a post-off gap.
- **Two rules for reading a census box**, both learned this round:
  1. A box that contains a dancer or crowd silhouette is not a lighting
     measurement. Poses differ between runs, and between native runs of the
     same binary. Check the frame before believing the number. The houseparty
     steps read −8% and then +9% across builds of the same family. The
     throneroom throne box read +1% to +25%: in far01 it holds the lead
     dancer's arm.
  2. Re-read a post-on gap with post off before calling it lighting.
     Streetside's brick and graffiti were −22% and −16% post-on, but only −2%
     and −3% post-off.

### 12.2 Results

| row | what the original does | result | tag |
|---|---|---|---|
| throneroom peak teal pillars, −12% raw at full alpha, −25% at alpha 0.5 | `NgMat::SetBasicState` (Mat_NG.cpp): alpha_cut is `SetAlphaTestEnable` plus `SetAlphaFunc(5 = GREATER, threshold)` and nothing else. Nothing calls `AlphaToMaskEnable`. `peak_columns_1..16.mat` are SrcAlpha, intensify, alpha_cut at threshold 0 | **Fixed by the coverage rule** (np-menutext, `MaterialCutCoverage` / `MaterialAlphaThreshold`). Native had made every cut alpha-to-coverage on the 4x pass, so the columns drew at about alpha², in a screen-door. On np-menutext 68522e9, frozen raw: 53.0 vs Xenia 52.9 at full alpha, 39.0 vs 38.9 at alpha 0.5, under both `DC3_LOOK=retail` and `enhanced`. Clearing alpha_cut live moves native b5 46.4 → 52.2; Xenia does not move (52.9 → 52.9) | MEASURED |
| throneroom peak wings / blue_light_l, −14..−20% census | — | **Dissolved: chase phase plus the row above.** Frozen on np-menutext: wing_l −4%, wing_r −3%, blue_light_l +1% | MEASURED |
| throneroom normal throne, −12% | — | **Dissolved as a box artifact.** The box is the lead dancer's arm. It reads +1%, +12%, +21% and −2% against Xenia across four builds | MEASURED |
| dclive peak sky, −11..−20% | `DLV_PeakSkyPano.mat` contributes the same on both sides: 11.2 vs 10.8 and 12.8 vs 12.0. Its environ share is −3.5 vs −3.4 | **Post share dissolved as Xenia factorless MAX** (post on: sky_l −14.5% → −9.5%, sky_r −14.1% → −7.3%, scene −13.4% → −3.3%). **Post-off −10% / −10.5% open.** It is unchanged by the MAX build, and its cause lies outside the sky material (INFERRED) | MEASURED / open |
| houseparty peak neon steps, +16% | stair contributions match. The step-pixel p90 is 233.9 vs 234.6 | **Dissolved as occlusion** (rule 1). Census −8%..+9% depending on the run | MEASURED |
| streetside normal fence, −20% raw | — | **Dissolved as occlusion** (crowd in the box). On clean fence pixels raw is −2..+3%. Far post-on −5.6% → −0.6% under the MAX build | MEASURED |
| streetside normal brick / graffiti, −22% / −16% | spots contribute the same on both sides: brick 1.1 vs 1.0, graffiti 10.0 vs 9.9 | **Dissolved as Xenia factorless MAX.** Post-off −2% / −3%. Post-on far −21.9% / −15.4% → −2.2% / −2.2%, near −19.4% / −13.1% → −3.2% / −4.1%. Streetside peak does the same (−11.5% / −13.3% → +2.7% / +1.7%) | MEASURED |
| do hidden spotlights still light on native? | `SpotlightDrawer` clears `sLights` every frame and never adds a hidden Spotlight | **No.** Hiding every showing Spotlight removes the same light on both sides: houseparty fridge 29.9 vs 29.2, wood floor 6.6 vs 6.4; with post off 0.4 vs 0.5 and 2.1 vs 1.9 | MEASURED |
| houseparty fridge, +18..+20% | — | **Open.** It is present with spots hidden and with post off, so it is neither spot light nor the MAX rule | MEASURED / open |
| rollerrink peak booth_top, −10% | — | **Dissolved: within noise.** Post-on −4.6% (before) and +3.9% (np-menutext); post-off −2..+3% | MEASURED |

### 12.3 Cost and verification

- The round-7 engine change (alpha cut as a test with no coverage) cost
  nothing on the GPU (`MILO_GPU_TIMERS=1`, throneroom peak):
  MainPassResume 0.399 → 0.385 ms, gpu_sum 0.661 → 0.669 ms. It was reverted
  in favour of np-menutext's rule, which covers the same case.
- That lane's two extra tests went to np-menutext:
  - both material builders (primary and pass) hand the shader the strict test;
  - the real `peak_columns_1.mat` gets no coverage in either look.
  Each is shown able to fail: a builder put back on threshold/255 left
  np-menutext's own tests green and turned these red.

## 13. Round 8: the two post-off residuals, and the enhanced-lighting survey

Round 8 closed the two rows that §12 left open. Both were bisected by hiding
draws, on both sides, at the same frozen frame, using the method of §12.1.
Evidence and tools are in dc3-decomp
`archive/np-gfx-2026-11-14/lanes/light/round8/`.

### 13.1 Houseparty fridge, +18%: a Xenia occlusion-query artifact

**Masked measurement.** Dancer poses differ between the two sides, so box
means were contaminated. The comparison (`maskcmp.py`) uses only the pixels
that the fridge's own lights toggle on both sides.

| state | native | Xenia | gap |
|---|---|---|---|
| base | 105.1 | 79.3 | +32% |
| `point_test FALSE` on fridge.flare, fridge1.flare, tv.flare | 109.8 | 106.4 | +3% |
| flares hidden | 82.1 | 78.9 | +4% |

**What the original does.** `RndFlare::DrawShowing` (Flare.cpp) handles an
area-tested flare like this:
1. With `mPointTest`, it queues an occlusion query through `TheRnd.TestPoint`.
2. It scales strength by `mOcclusionResult / (rect width x height in pixels)`,
   times the step ratio.

**What each side counts.**
- Native's `PointTestPass` counts real samples, as the hardware does.
- Xenia writes a fixed 1000 samples for every ZPASS query:
  `query_occlusion_fake_sample_count = 1000`, in
  `command_processor.cc` `ExecutePacketType3_EVENT_WRITE_ZPD`.
- So on Xenia, a flare whose rect is much larger than 1000 pixels draws at
  close to zero strength.

**The lighting itself matches.** Specular is 31.5 vs 32.5, and
FridgePointLight2 dominates on both sides.

**Tag: Xenia artifact (MEASURED).** The control is `point_test FALSE`: it
takes the flares off the query path, and the gap falls from +32% to +3%.

**Rule for every venue.** Xenia under-draws every area-tested flare. Leave
such flares out of parity boxes, or compare them with `point_test FALSE` on
both sides.

### 13.2 dclive peak sky, −10%: the texture term of the peak stage cubes

**The deficit is in the cube textures.** It lies entirely in the texture term
of `DLV_PeakStageCube01/02/03`.
- With the cube texture removed, both sides agree: 17.7 vs 17.1 (+3%).
- With the texture, native is low in all 5 frozen pairs: −17% to −41%, about
  −30% typical.

**Lighting is not the cause.**
- Each of the noenv, noa0 and nor0 runs leaves the gap in place.
- Setting the alpha or tex_gen properties has no effect on either side. A
  MatAnim probably rewrites them every frame.

**Why the texture term is fragile.**
- The textures are rgb=1 with stripes in alpha.
- They sit under an animated `tex_xfm`: Cube01 y_scale 20, Cube02 x_scale 5,
  Cube03 y_scale 5.
- The scroll phase differs a little between the two sides (Cube02 y 3.57 vs
  3.73).
- Retail caps a texture at 2 mip levels (`DxTex::SyncBitmap`, mirrored by
  native `kBitmapMaxMipLevels`), so the stripes are heavily minified.
- The mean therefore depends on filtering and aliasing. Native samples with
  `maxAnisotropy = 1`. The retail fetch constant's anisotropy is not known.

**Tag: cause with a number (MEASURED); the filter mechanism is INFERRED.**
This is handed to the texture-filtering (sharpness) work.

The census shares for refraction (−26%) and line fade (−37%) may be the same
class. They are not tested.

### 13.3 Other findings

- **`MILO_WIDTH=1920 MILO_HEIGHT=1080` is broken for DC3.** The world renders
  into a 1280x720 rectangle at the top left. The round-8 survey ran at 720p.
- **An out-of-range `(lights_real i)` crashes Xenia, and SIGSEGVs native DTA
  eval.** The cause is generic. `PropSync(ObjPtrList<T>&, ...)`
  (dc3 `src/system/obj/PropSync_p.h`) advances the list iterator `prop->Int(i)`
  times with no bound, then dereferences it. The asserts there (0x154, 0x15E)
  do not check the index, so the debug original has nothing to catch this, and
  crashes the same way under Xenia. Not fixed: the code matches the original.
  Probe only the indices that exist.
- `(tex_xfm m x x)` asserts in PropSync: a Matrix3 path needs its last element.
  Use the named paths instead: `x`, `y`, `z`, `x_scale`, `y_scale`, `z_scale`,
  `pitch`, `roll`, `yaw`.

### 13.4 Enhanced-lighting survey (`DC3_LOOK=enhanced`)

**Material counts.** Every lit material carries a metamaterial, and vertex
lighting dominates.

| venue | lit | total |
|---|---|---|
| dclive | 75 | 92 |
| houseparty | 82 | 92 |
| throneroom | 54 | 77 |
| streetside | 69 | 104 |

**Cost baseline.** On the RTX 3090 (`MILO_GPU_TIMERS=1`):
- MainPassResume: 0.34–0.50 ms
- whole frame: under 1 ms
- DisplayRamp: 0.018 ms
- ProjShadowCasters: 0.02 ms

| candidate | evidence (4x crops) | verdict |
|---|---|---|
| 8-bit banding in dark gradients | **Dissolved: no gap and no banding.** (1) The 1060 vs 1576 distinct-colour count measured hue spread, not precision. Per channel, native has as many levels or more (blue 97 vs 81; red 51 vs 94 because Xenia's red spans 3–96 against native's 0–64). (2) The lines in the 4x crop are dclive's own post scanlines, at the same amplitude on both sides: row period 2.2 px, 1.35 vs 1.33 blue LSB. They are gone post-off on both sides. (3) The original renders the world into A8R8G8B8 EDRAM (`CreateBackBuffers`, Rnd_Xbox.cpp, 2x MSAA) and resolves into A8R8G8B8 pre/post buffers, so it is 8-bit end to end, like native's surface-format scene target. A wider target would not be a parity fix, and the post lane's composite dither already covers the final 8-bit write | cut (round 8, with the coordinator) |
| per-pixel lighting for vertex-lit materials (faceting, Mach bands) | metamaterial cleared and `per_pixel_lit` set where a material has no normal or spec map. dclive, throneroom, streetside: no visible change. houseparty: +12% mean luma, and 27% of pixels move by more than 8 (floor and stair stripes) | cut: breaks the luma rule |
| specular aliasing | streetside and houseparty near shots: no shimmer difference against Xenia; the visible difference is floor-texture sharpness | cut |
| projected shadow resolution and filtering | native already uses a 512² tile with 9 taps, against retail's 256² with two 5-tap blurs. Edges are equally soft, and the shadow is a blob by design | cut |
| env and cube maps | — | sharpness lane (this includes §13.2) |
| hard rim | retail's rim is a smooth pow, and no hard edge shows in any crop | cut |
| light caps | — | excluded: raising them changes exposure |

### 13.5 Reading Xenia: three rules from the post lane's round 10

The post lane closed its residual C (dclive normal, colour difference 8–15
with post on) in round 10 part 3: dc3-backend-for-rb3-wii.md §37.10,
"Residual C: closed". Evidence is in dc3-decomp
`archive/np-gfx-2026-11-14/lanes/post/round10/p3c/`. Three measurement rules
came out of it. They apply to any native-vs-Xenia census, lighting included:

1. **Run native with `DC3_DISPLAY_GAMMA=off`.** §12.1 already does this. The
   rule is here because the default is wrong for a census: native captures
   are POST-ramp (the engine logs "DC3 display ramp: gamma 0.850 ... captures
   are POST-ramp"), while Xenia's front buffer is pre-ramp. With the ramp on,
   a correct grade reads as a contrast error of the exact form
   R(k·R⁻¹(x) + off) − x, with R(x) = 255·(x/256)^0.85. Part 3c's first native
   run read −9.4 levels at input 11 against Xenia's −6.0, exactly that
   formula. Raw boxes read native +10–16% brighter in the same run.
2. **On a venue with strobing content, a gap from frames not matched in song
   time is not a measurement.** The post lane's round 10 read native's
   no-bloom grade ~15% brighter on unmatched frames. Paused at matched song
   times (±0.05 s), the same comparison is −1.0..+2.6%, under the floor. Match
   song time first (pause both sides at the same `{taskmgr seconds}`), and
   treat a strobing board as its own question.
3. **Test a grade term with per-pixel transfer curves.** Install a copy of
   the proc as the post-proc override, with only one term active (here the
   others neutral and bloom intensity 0). Freeze the frame, then take out − in
   per input bin, per channel, against the post-off frame. Each pixel is
   compared with itself, so it needs no registration and ignores content
   differences such as poses, flares and strobes. Then compare the curve with
   the term's formula on both sides. Contrast matched k·x + off to 0.03
   levels on both, and saturation and the full grade matched to ≤ 0.3. Tool:
   `p3c/tools/xfer3c.py`.

## 14. Item 11: dancer floor shadows against Xenia (np-shadow)

Lane archive: dc3-decomp `archive/np-gfx-2026-11-14/lanes/sharp/item11/`
(`step2/` the first measurement, `step3/` this section). Method, both sides
paused at song second 20 of omg, the same forced shot, post off
(`rnd toggle_all_postprocs`), native at `DC3_DISPLAY_GAMMA=off`. The shadow
is the frame with casters minus the frame after `clear (shadow_objects)` on
the shadow light, in luma. Mask = pixels darkened by more than 8.7 (the 3.4%
floor). Ratio = mean luma inside the mask with casters over without. Tool:
`step3/tools/shcmp.py`.

### 14.1 Casters: Character::DrawLod's occlusion rule

Retail's `RenderShadows` draws each shadow object with `DrawShowing` in
`kDrawOcclusion`. For a `Character` that is `DrawLod`'s occlusion branch.
It draws nothing unless `floor_shadow` is set and the draw mode has the opaque
bit. Otherwise it calls `DrawLodOrShadow(lod, 4)`: the character's `shadow`
group if it holds anything, else `DrawOpaque`, which is the character's own
drawables plus the current LOD's opaque group. Native used to expand every
mesh under the character: all LODs, the shadow meshes on top of the body,
hidden props and translucent groups. Native now follows retail
(`DC3SceneLighting.cpp` `AddOcclusionDraw`, behind the rndshape seam). Two
exceptions:

- rb3-xenon's character has a different shape, so the rule compiles out
  there (`kDC3RetailModelFor`).
- The draw's own camera-frustum cull was not reproduced in item 11; item
  12 added it (§15.5).

Test: `RetailLightingTest.CharacterCastsWhatDrawLodDraws`.

### 14.2 Caster skinning: the wedges

The map was read back on the GPU (an experiment hook, not committed). On
throneroom the dancers' legs were in place, but every upper body was
smeared into a wedge reaching toward one point. That point is where the
world origin projects. The cause was `ShadowPass`'s skinned caster shader.
It summed `bone * p * w` without normalizing the weights, while the main
pass divides by their sum (`normalizeBoneWeights`). The `*_lod` shadow-group
meshes, which item 14.1 made the casters, are drawn by no other pass. Their
weights do not sum to one, so their vertices were pulled toward the origin.
`RenderCasters` now uses `vs_caster_skinned`, which normalizes and binds an
all-zero set to the first bone. rb3-xenon's generic `vs_shadow_skinned` is
unchanged. Tests: `ProjShadowCasterPassTest.SkinnedCasterWeightsAreNormalized`
and `ProjShadowCasterPassTest.SkinnedCasterWithNoWeightsFollowsItsFirstBone`.

The throneroom cone is the longest of the venues: range 1000, top/bottom
100/300, so a slope of 0.2. Its virtual apex sits at yL = −500, behind the
light. The cone was *not* the problem. At the dancers (yL ≈ 205) the cone
is ~282 units across. That is 0.9 retail texels per unit (1.8 on native's
512 tile). The blur's 1.5/256 step is ~1.65 units. The map is a coverage
map, so depth precision does not enter. After the fix the tile shows four
clean silhouettes (`step3/tile_tr_before_after.png`). With 14.1 alone, the
wedges were still there.

### 14.3 The shadow scales the point lights too

Even with a clean map, the throneroom floor shadow drew at about a third of
retail's depth. The relative darkening per channel in the dancers' shadow
was Xenia 0.175 / 0.174 / 0.232 against native 0.053 / 0.078 / 0.114. The
floor's colour without the shadow matched within 1 level on both. The
floor is lit mostly by point lights: hiding the type-0 lights takes the
red channel from 80 to 27. standard.ps `0000110010030011` is the multiply
form with one point light (`scripts/gpu/xenos_ucode/dis.sh`). It puts the
point term into the same register as the six box faces (lines 22-33), then
multiplies the sum by `c0 (1 − 0.75 s c69 sat(c65 · N))` (lines 15-25, 34),
and only then adds `c1 c0`. Native scaled the box term only.
`retailPixelShade` now scales `retailDiffuseLight` (box and points). The
vertex-lit fallback already did. No shipped skin or hair permutation has a
projected light (385 projected permutations, 0 with a custom variation), so
`dc3SkinLight`/`dc3HairLight` were left alone. Test:
`RetailCharShadingTest.ProjectedShadowScalesTheWholeDiffuseLight`.

### 14.4 Against Xenia, after the three fixes

Mask px native/Xenia, and ratio (Xenia / native). Rows marked *boombox* have
the dancers hidden; that is the clean comparison, because the dancers' poses
differ between the two runs (native's performer is not Xenia's input).

| venue / shot | before (step 2) | after | ratio after |
|---|---|---|---|
| rollerrink far | 1.25 | 1.01 | 0.729 / 0.713 |
| rollerrink near | 1.46 | 1.04 | 0.748 / 0.739 |
| rollerrink cu02 | 1.36 | 1.02 | 0.772 / 0.741 |
| rollerrink cu02, boombox | 1.32 | 1.32 | 0.838 / 0.821 |
| streetside far | 1.11 | 1.07 | 0.614 / 0.579 |
| streetside near | | 0.88 | 0.613 / 0.565 |
| streetside, boombox | 0.88 | 0.95 | 0.591 / 0.587 |
| throneroom far | 0.00 | 0.88 | 0.749 / 0.757 |
| throneroom near | 0.00 | 0.87 | 0.773 / 0.815 |
| throneroom, boombox | none | 0.84 | 0.825 / 0.835 |
| houseparty far / near | | 1.01 / 0.78 | 0.626 / 0.618, 0.624 / 0.617 |
| glitterati far / near | | 0.96 / 0.88 | 0.664 / 0.678, 0.644 / 0.675 |
| dclive far / near | | 1.69 / 1.33 | 0.884 / 0.867, 0.873 / 0.855 |
| dci far / near | | no shadow on either side | |

**Open.**

- Streetside's *dancer* shadows now read darker than Xenia's (0.579 vs
  0.614). The boombox alone matches (0.587 vs 0.591), and the poses differ
  (the native dancers have their arms out). So this is not a darkness
  error that can be measured with this protocol.
- Rollerrink's boombox is still 1.32× Xenia's area. That venue's light is
  the additive gobo form (blend 0), which 14.3 does not touch. (Item 12:
  1.12 after the half-texel raster offset, §15.2.)
- dclive's shadows sit under the same feet in the same shape. They are
  larger and darker, and in the Xenia frame the dancers' arm shadows are
  cut where the gobo pool ends (`step3/dclive_far_crop.png`). This is a
  lead, not a finding: §10.1 matched dclive's shadow depth within 2.4 luma
  on a matched pose. (Item 12: the cause was the scaled light's caster
  transform, §15.1; dclive now reads 0.95–0.98 / 0.97.)

### 14.5 A larger shadow map does not draw a cleaner edge (not shipped)

Item 11 (c) asked whether a map sized to the output looks cleaner in the
enhanced look. It was measured on rollerrink cu02 (the dancers' shadows and
the boombox) at 1920x1080 and 3840x2160. Atlas sizes 512, 1024 (the
shipped size), 2048 and 4096 give tiles of 256 (retail's size), 512, 1024
and 2048. The binary was an experiment build (`SHX_MAPSIZE`; the tap
clamp taken from the texture's size). It is not committed. Mask px and
edge width are per 720p-equivalent pixel. Edge = pixels darkened by
10–90% of the full depth, divided by the mask's border length.

| output | tile | mask px | ratio | edge px |
|---|---|---|---|---|
| 1080 | 256 | 30375 | 0.762 | 9.39 |
| 1080 | 512 | 29285 | 0.743 | 9.35 |
| 1080 | 1024 | 29626 | 0.741 | 8.95 |
| 1080 | 2048 | 28826 | 0.738 | 8.67 |
| 2160 | 256 | 28615 | 0.740 | 8.71 |
| 2160 | 512 | 29153 | 0.741 | 8.28 |
| 2160 | 1024 | 28359 | 0.737 | 8.13 |
| 2160 | 2048 | 28589 | 0.738 | 7.73 |

The dancers' poses differ by a few frames from run to run (the pause lands
within ±50 ms), so mask px varies by up to 5% between runs (28359–30375 over all eight, with no trend in size).
Shape and darkness stay within that. The edge narrows by at most 0.8 px at
720p scale. In the 3× zoomed crops of a shoe's shadow and of the boombox
(lane archive `step3/mapsize/`), the four sizes cannot be told apart.
Taps and blur are fixed in light space (1.5/256 of the tile, as retail's
two 5-tap passes over a 256 map). So a finer map samples the same penumbra
more finely; it does not narrow it. Narrowing it would go past the
original's edge, which item 11 rules out. The map stays at 1024 (512
tiles) in both looks.

## 15. Item 12: the gobo-form shadow, and the caster cull (np-shadow2)

Lane archive: dc3-decomp `archive/np-gfx-2026-11-14/lanes/sharp/item12/`.
Protocol as §14 (omg, paused near song second 20, forced shot, post off,
`DC3_DISPLAY_GAMMA=off`). Shadow = the frame after `clear (shadow_objects)`
minus the frame with casters, in luma; mask = darkened by more than 8.7.
n/x = native mask px over Xenia's. Rows marked *boombox* have the dancers
hidden and are exactly reproducible run to run (the same binary gave the
same count twice). Dancer rows move by up to ±5% between runs because the
pause lands on one of three frames (20.009 to 20.042) while Xenia's is at
20.025; read them as a range.

### 15.1 dclive: a scaled light, drawn through its inverse

dclive's `a2_spotlight.lit` (projected_blend 0, the gobo form) has world
scale (1, 0.86, 0.86) (MEASURED: `get (world_xfm y_scale)` on both sides;
texture_xfm is identity). Retail draws the casters with
`NgLight::SetShadowTransforms` (100%): `Invert(WorldXfm)`, y and z swapped,
the cone projection, so clip = (xL, zL, yL/range, top + slope·yL). The
receivers read the map with `RndLight::Projection()`, which uses the raw
rows of `WorldXfm` times `mTextureXfm`. For an orthonormal light the two
are the same map. For a scaled one they are not, and the Xbox draws the
shadow where that mismatch puts it. Native drew the casters with the
receivers' `Projection()` clip, so its map agreed with its receivers and
its shadow sat under the geometry: about 25 px (720p) lower on the floor
than Xenia's, and larger.

Native now builds the caster clip as `SetShadowTransforms` does
(`ProjShadowCasterClip`, DC3SceneLighting.cpp, behind the rndshape seam),
and keeps `Projection()` for the receivers. Test:
`RetailLightingTest.ScaledLightDrawsItsCastersThroughTheInverse` (a light
scaled 0.86 in y and z; the expected local coordinates are worked by hand;
it also checks that the receivers' uv differs, so the two transforms cannot
silently collapse into one). Sabotage: drawing the casters with the
receivers' clip turns it red.

The three questions asked of the gobo form, each a measurement that could
have come out the other way (`tools/gobocmp.py`, dclive far and near):

| question | Xenia | native | answer |
|---|---|---|---|
| shadow where the gobo is ~0 (G = noShadow − noLight < 8.7)? | 0 px | 0 px | no: the factor scales the gobo term only |
| S/G inside the mask (must be ≤ 1 for gobo·(1 − s)) | p50 0.76, p90 1.00, >1.2: 0 | p50 0.78, p90 1.00, >1.2: 0 | same form |
| the gobo pool itself (G mask px, bbox) | 176712, x0-1268 y505-719 | 176592, x0-1260 y505-719 | same pool, same clamp and falloff |

So the factor, the gobo's border and the cone falloff are right. The
caster set was not the cause either: the shadows have the same shapes under
the same feet on both sides. What differed was where the casters land in
the map. The arm shadows that ended at the pool edge in Xenia's frame
(§14.4) were that: Xenia's shadow sits further up the floor, where the pool
ends sooner. dclive mask n/x: far 1.69 → **0.95–0.98**, near 1.33 →
**0.97** (item 11's figures were at a different pause; at the item-12 base
the same shots read 1.37 / 1.05). dclive's boombox casts no shadow on
either side.

### 15.2 Half a texel: D3D9 pixel centres in the caster pass

`DxRnd` leaves `D3DRS_HALFPIXELOFFSET` at 0 for every draw, including the
caster pass. Only `DxRnd::DrawRect` (rnddx9/Rnd.cpp ~525/537),
`DrawRectDepth` (589/592) and a point test (Rnd_Xbox.cpp 1207/1306) set it
to 1 and back. So texel *i* of the 256x256 map holds the caster at x = *i*,
while the receivers' bilinear read (FilterMode 1) puts texel *i* at *i* +
0.5: retail's map is half a texel (+u, +v) from the geometry.
`BlurShadowRT`'s two passes are DrawRects and add nothing (MEASURED from
source). WebGPU rasterises at pixel centres, so native moves the caster
geometry by half a 256-texel: +1/256·w in clip x, −1/256·w in clip y (clip
y is up, v is down). Native's tile is 512, but the offset is retail's
half-texel of 256, in light space.

Measured before shipping it, in one paused session with a test-only hook
switching the offset (`REG_*`, 0 → +0.5 texel): every changed pixel lies
inside the shadow region (0 px outside, for every shot; the same session's
frame-to-frame noise is 800–1370 px, the HUD). Boombox-only, against
Xenia:

| boombox | n/x before → after | IoU before → after | other |
|---|---|---|---|
| streetside | 0.951 → **0.981** | 0.812 → 0.851 | weighted centroid 285.1 → 274.1, Xenia 272.9 (12 px → 1 px); bbox equal to Xenia's |
| rollerrink cu02 | 1.318 → **1.120** | 0.674 → 0.731 | centroid 226 → 198, Xenia 205 |
| throneroom | 0.842 → **0.658** | | moves the other way; see 15.4 |

Test: `ProjectedShadowLightDrawsItsCastersWhereTheReceiversLook` and the
scaled-light test expect the half texel (sabotage: without it both fail).

### 15.3 The receivers' kernel is not the residual

Native filters with 9 compare taps (1 2 1)²/16, 1.5/256 apart; retail
blurs the map with two 5-tap passes (0.1/0.25/0.3/0.25/0.1, one texel
apart) and reads it bilinearly. Two experiment builds (not committed):
e1 = taps 1.69/256 apart, e2 = retail's exact 5x5 at 1/256.

| n/x | shipped | e1 | e2 |
|---|---|---|---|
| rollerrink cu02 boombox | 1.12 | 1.20 | 1.14 |
| streetside boombox | 0.98 | 1.01 | 1.00 |
| throneroom boombox | 0.66 | 0.64 | 0.64 |

No kernel closes rollerrink, and none moves throneroom. The map size does
not either: item 11's tile 256–2048 builds read 2776–2916 px on
rollerrink's boombox (Xenia 2127, before the half texel), with no trend.
The kernel stays as shipped.

### 15.4 After, and what is left

| venue / shot | item 11 | item 12 (two runs) |
|---|---|---|
| dclive far / near | 1.69 / 1.33 | **0.95–0.98 / 0.97** |
| rollerrink cu02, boombox | 1.32 | **1.12** |
| rollerrink far / near / cu02 | 1.01 / 1.04 / 1.02 | 0.98–1.04 / 0.99–1.00 / 1.02–1.03 |
| streetside far / near | 1.07 / 0.88 | 0.95–0.99 / 0.92–0.98 |
| streetside boombox | 0.95 | 0.98 |
| throneroom far / near | 0.88 / 0.87 | 0.87–0.88 / 0.85–0.87 |
| throneroom boombox | 0.84 | 0.66 |

**Open.**

- Rollerrink's boombox reads 1.12, just outside 0.9–1.1. The coverage
  S/G along the band matches Xenia's column by column, the gobo under it
  is within 4% (G mean 21.2 vs 20.4), and the peak S/G is the same
  (~0.75 = `1 − 0.75 s`). The extra is a fringe about 1 px wide on both
  edges of a band ~20 px tall (`rr_bb_ht_xor.png`): the low-coverage band
  0.15–0.3 holds more pixels on native. Not explained by kernel or map size.
- Throneroom's boombox is a grazing tip of 438 px (Xenia) on the gold
  carpet edge. Native was already short there (mass 0.73 of Xenia's), and
  the half texel shortens it further (0.57). The light is orthonormal with
  an identity texture_xfm (range 1000, radii 100/300), and the venue's
  dancer shadows do not move with the change. A tip at grazing angle turns
  a sub-texel difference into tens of pixels, so this is the least
  reliable of the three boombox measurements; the cause is not known.

### 15.5 The caster cull

Retail's `RndDrawable::Draw` skips a nested draw when
`MakeWorldSphere(s, false) && s > RndCam::Current()->WorldFrustum()`
(Draw.cpp). `RenderShadows` calls `DrawShowing` on the shadow objects
themselves, which are not culled; what they draw beneath them is, against
the *camera's* frustum (the shadow pass does not change `RndCam::Current`).
Groups have no authored sphere and are never culled; a mesh uses its
`mSphere` when its radius is non-zero.

Native now does the same (`AddOcclusionDraw`, depth > 0 only, against the
frustum `WriteDC3ProjShadow` records from the frame's camera; DC3 only,
behind the rndshape seam). Test:
`RetailLightingTest.NestedCastersOutOfTheCamerasViewAreCulled` (a group
holding an in-view mesh, one behind the camera and one with no sphere, plus
a top-level shadow object out of view: the cull keeps the three it should;
with no camera all four draw). Sabotages: no cull, and culling the shadow
objects themselves, each turn it red.

What it saves (a TMPDIAG count of caster meshes per frame, with and
without the cull, over a whole omg session with the director picking the
shots; not committed):

| venue | casters/frame | frames with a cull | where | most saved in a frame |
|---|---|---|---|---|
| rollerrink | 34.1 → 34.1 | 0.4% | the win close-up | 23 of 33 |
| streetside | 34.1 → 34.0 | 2.1% | the opening venue shot, the win close-up | 23 of 33 |
| throneroom | 34.1 → 33.8 | 1.3% | the opening venue shot, the win close-up | 23 of 37 |
| dclive | 34.1 → 34.1 | 0.4% | the win close-up | 23 of 33 |

In the gameplay shots (`area1_far/near/movement`) every dancer is in view
and nothing is culled. Where it does cull, a dancer's whole caster set
(7, or 11–12 meshes) goes at once.

**What it changes on screen: nothing that could be found.** In one paused
session per venue the cull was switched off and on in place (a TMPDIAG file
flag) at 27 shots: cu01–cu04, intro_quick01 and win_angel_P1_high_dlg in
all four venues, plus the shot rollerrink, streetside and throneroom open
on. Four of them culled:
streetside's opening venue06 (23 of 37), and dclive's cu01 (7),
intro_quick01 (23) and win dialogue (23). In none of the four does the
frame change beyond its own noise. The metric: pixels moving by more than 8
levels between cull on and off, minus those that also move between two
cull-on shots (HUD animation under the pause). The four read 165, 162, 70
and 7 px, and a shot where nothing was culled (dclive cu02) reads 351 px.
The dancers that are dropped stand off-screen, and so do their shadows
(`ci_ss_intro1.png`: the only changed pixels are the HUD corner). So this is
parity by construction and a small saving, not a visible fix. No frame was
found where an off-screen dancer's shadow lands in view, so there is no
Xenia comparison to make.

## 16. Item 13 (c): the normal detail map, and the houseparty floor sheen (np-look13)

Lane archive: dc3-decomp `archive/np-gfx-2026-11-14/lanes/sharp/item13/c/`
(README first). Protocol: omg paused at 20.16 s, `area1_far02.shot` forced,
`MILO_RENDER_SCALE=2`, `DC3_DISPLAY_GAMMA=off`, post as shipped unless noted.
PLAN H-114.

### 16.1 Bit 24: the retail path ignored the normal detail map (FIXED)

`RndShaderStandard::CalcShaderOpts` sets bit 24 (`mNormDetail`) for a
per-pixel material with a `norm_detail_map` and `norm_detail_strength > 0`.
`standard.ps` `0000030001030035` against `0000030000030035` differs by one
fetch:
- tf14 is fetched at `uv · c106.y` (wrap);
- before the de_normal factor k and the tangent frame, it adds
  `s · (dx, dy, sat(1 − dx² − dy²))` to the normal map's `(x, y, z)`, where
  `d = 2 · texel.rg − 1` and `s` = c106.x = `norm_detail_strength`.

All 191 shipped PS permutations with bit 24 also carry the normal map (bit
5), so the term lives inside the normal-map branch.

Native's retail path did not read the map. Its modern path,
`blendDetailNormal`, which rb3 and rb3-xenon use, already did, and it is
unchanged. The fix adds the term in `retailPixelShade` under
`retailHasNormDetail()`: `scene.retailLighting > 1.5` (the DC3 shape) and
the material fields. The texture is sampled with `mapSampler`, which repeats.

- Test: dc3 `RetailCharShadingTest`, kind 4 ("plain + detail"), 12 cases
  from the microcode model (2e-8).
- Sabotages, each red:
  - the term off;
  - the z term dropped;
  - gated on `retailLighting > 0.5`, which reaches rb3's form;
  - the detail scaled by `1/k`.

**Readback that could have come out the other way.** In one paused session,
every material with a detail map is set to strength 0. On the base binary
this moves only what animates under the pause (875–1,314 px per venue): the
HUD column (all of houseparty's 928) and glints on the left dancer's outfit
(181–299 px elsewhere). On the fixed binary it moves the materials listed
below. Had native already drawn the term some other way, the two binaries
would read alike.

### 16.2 Which venues carry it

Strength A/B in one session (shipped strength → 0, `seq/nd_ab.n.txt`):
pixels that move, those that move by more than 8 levels, and the mean luma
change on the moved pixels (on − off). Base is the control.

| venue | materials with bit 24 (strength, tiling) | moved px base / fix | > 8 lvl base / fix | mean Δluma (fix) |
|---|---|---|---|---|
| houseparty | `HSP_LivingRoomFloor` (0.15, 50) | 928 / 98,515 | 401 / 2,151 | +0.40 |
| rollerrink | `StageCarpet` (2.0, 20) | 980 / 325,443 | 269 / 91,587 | −1.26 |
| streetside | `Sidewalk` (10, 100), `Street` (5, 15), `building1/2_fireEscape` (1, 10), `Building2_brick` (0.5, 1) | 1,314 / 328,518 | 543 / 26,389 | +0.51 |
| throneroom | `THR_floor_main`, `_raised` (4, 10), `THR_columns_crack` (4, 1.9), `THR_rearWall` (2, 6), `THR_floor_main_lowNorm`, `_raised_lowNorm` (1, 10), `THR_floor_edge` (0.4, 10), `THR_columns`, `_darker`, `THR_crowStatue` (0.1) | 1,067 / 407,198 | 330 / 7,893 | +0.24 |
| dci | `Ceiling` (1.5, 15): ceiling, rear wall top, rear banister | 875 / 60,326 | 324 / 916 | −0.07 |
| dclive | none | 1,184 / 860 | 449 / 235 | — |
| glitterati | none (its screens animate under the pause, hence the counts) | 426,564 / 372,358 | 568 / 478 | — |

Houseparty has three more materials with a detail map but strength 0
(`HSP_Couch1`, `HSP_LivingRoomRug`, `HSP_XmasLights01`), so bit 24 is not
set on them. Characters: of the 1,956 skinned permutations in the shipped
cache, one carries bit 24 (`0000020011431037`). It has the houseparty floor's
feature set and none of the rim bits, so it is a skinned scene object, not a
dancer.

**Nothing moves outside the materials' own pixels.** Each venue was run once
more with the A/B, then a second off shot (noise), then the bit-24 meshes
hidden (footprint = off vs hidden, dilated 2 px; `seq/nd_ab2.n.txt` with
post as shipped, `nd_ab3.n.txt` with post off):

| venue | post off: footprint px | moved px | moved outside the footprint and the HUD column | post as shipped: moved outside (max level) |
|---|---|---|---|---|
| houseparty | 360,998 | 80,094 | **0** | 55 px (1) |
| rollerrink | 338,510 | 314,770 | **0** | 15 px (1) |
| streetside | 384,944 | 283,830 | **0** | 32 px (1) |
| throneroom | 461,863 | 339,956 | **0** | 0 |
| dci | 179,753 | 56,472 | **0** | 5 px (1) |
| dclive | none hidden (no bit-24 meshes) | 1,258 (animation) | **0** | 0 |
| glitterati | none hidden (no bit-24 meshes) | 951 (animation) | **0** | — (its screens animate under the pause) |

Every pixel outside the footprint that moves with post off is in the HUD
column (x ≥ 1090), which animates under the pause. With post on, the
footprint includes the bloom's spread, and the few pixels outside it move by
1 level (the post chain's rounding).

The footprint must be taken at "any change" (> 0). At "> 8 levels" it misses
a mesh's own pixels wherever what is behind it looks alike: dark brick in
front of a dark sky, or marble reflecting marble. That version reads tens of
thousands of "outside" pixels on streetside and throneroom.

### 16.3 Against Xenia

Xenia runs the shipped microcode, so it draws the term. The measure is
high-frequency energy in boxes on the changed surfaces: the std of luma
minus a box blur of radius r (`tools/hfenergy.py`). Both sides are far02
with post on, except rollerrink, where Xenia's bloom is off.

| venue, box | Xenia r1 / r2 / r4 | native before | native after |
|---|---|---|---|
| streetside, asphalt (700,640)+(350,80) | 4.34 / 5.42 / 5.67 | 0.85 / 1.16 / 1.62 | **4.19 / 5.34 / 5.62** |
| rollerrink, carpet (item 13a's floor boxes) | 6.09 / 8.62 / 11.42 | 4.54 / 7.55 / 10.77 | **6.66 / 8.99 / 11.43** |
| throneroom, marble (0,545)+(150,55), (0,600)+(60,40) | 1.72 / 2.04 / 2.31 | 0.91 / 1.21 / 1.59 | **1.92 / 2.23 / 2.51** |
| houseparty, wood (10,600)+(70,110), (350,695)+(500,25) | 5.09 / 6.10 / 6.77 | 2.70 / 4.16 / 5.46 | **3.87 / 5.16 / 6.20** |

Streetside's asphalt p95 goes from 58.3 to 66.6, against Xenia's 66.9
(`sheets/ss_asphalt_x2.png`: Xenia's grain was missing on native and is
back). Houseparty's remaining gap is the sheen (§16.4).

The detail term does **not** explain item 13a's rollerrink haze residual:
the carpet box's p5/p95 stays at 0.128, against retail's 0.20–0.28. That
row stays open.

DCI is not compared: the Xenia survey frame is far03.

### 16.4 The houseparty sheen is Xenia's (no change)

The survey row was a broad golden sheen on the foreground wood in Xenia,
missing on native. Inputs first:
- Retail PS `0000120011430037`: spec per point light =
  `lobe(p · 0.1592 + 0.3183) · sat(V · R)^p · colour · falloff`, with
  `p = max(specMap.a · power, 0.5)`.
- The constants match native's: camera position, light positions and
  colours, falloff slope.
- The spec map's alpha bytes, decoded from the milo, have 0 DXT1
  punch-through texels, so p is the material's 18.
- Native's light-01 spec equals analytic power-18 Phong on the flat floor.

Measurements:
- Xenia's sheen box p95 goes from 65.0 at power 18 to 33.3 at power 200.
  Native's is 32.2 at 18.
- Retail's stills (houseparty 12, 14, 02) show dark wood with no broad
  sheen. Wood p95 over rug p50, which cancels capture levels:
  - Xenia far01: **1.72**;
  - native far01: **1.07**;
  - retail 12: **1.26** (another song and a near camera).
- Nothing was tuned toward Xenia.

The rule is in dc3-backend-for-rb3-wii.md §37.10's list of Xenia artifacts.

Two small retail differences are left, both untried:
- Retail multiplies the spec sum by the projected-light multiplier
  `1 − 0.75 · c69 · tf5.a · sat(N · c65)`. Native does not, and the
  multiplier only darkens.
- Retail normalises V per vertex, across the floor's large triangles.
