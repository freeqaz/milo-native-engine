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
(0.85) is **not** applied here. That belongs to the np-gfx-post lane, so
`DisplayGamma` stays 0 in the DC3 shape. Retail hardware captures are
post-ramp; compare them with `pow(x, 0.85)` applied to a native frame.

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
- The casters are drawn twice per frame. DC3's own `NgLight::RenderShadows`
  still runs into its own render target, which nothing samples, alongside
  the native coverage pass. That is wasted work, not a visible difference.

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
| getenv census, round 2 | rb3 main's `native_compat_census.py --engine-root <this branch> check` | OK, 446 flags, the same as on the base 5664e9d. The 4 above round 1's 442 came in with the np-gfx merge, and `git diff 5664e9d HEAD` has no `getenv`. |
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
