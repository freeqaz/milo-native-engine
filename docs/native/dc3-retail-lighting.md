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
   factor to the box term.
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
because the factor scales only the box term (`1 − 0.75 s`), never the
ambient, as in retail.

**Not done.**

- The receiver's vertex-lit form scales the whole interpolated light. That is
  INFERRED, and it does not matter in practice: the census above has no
  vertex-lit receiver, so it is a native-only fallback.
- `SphereConeTest` culling of casters is skipped. It only removes casters that
  cannot land in the map.
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
