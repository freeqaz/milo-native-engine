# The dc3 GPU backend for Wii RB3 (lane W16-PV, 2026-10-06)

One renderer for all three consumers. Before this lane the engine carried two GPU
backends, chosen by `MILO_ENGINE_GPU_BACKEND`:

| flavor | renderer | written against | consumers before |
|---|---|---|---|
| `dc3` | `WgpuRnd : NgRnd` (`src/platform/Rnd_Wgpu.cpp` + Mesh/Tex/Part/MaterialSetup/MeshGpuCache/TransparentQueue + six rndobj-coupled gfx passes) | DC3's 2012-era rndobj | dc3-decomp, rb3-xenon |
| `rb3` | `BandRnd : Rnd` (`src/platform/Rnd_Wgpu_RB3.cpp` + RB3PostProc/RB3Quad/...) | RB3-Wii's 2010-era rndobj | rb3 (Wii) |

Now rb3 (Wii) can build every native target against `dc3` as well
(`-DRB3_GPU_BACKEND=dc3` in rb3's `native/CMakeLists.txt`). The `rb3` flavor is
untouched and stays rb3's default for now (see "What is left").

Branches (not pushed, pin not bumped):

| repo | branch | commit |
|---|---|---|
| milo-native-engine | `w16-pv-dc3-backend-wii` | `3772f40` (on `e1b0c29`) |
| rb3 | `w16-pv-dc3-backend-wii` | `758358c6c` (on `c769de592`, which loads `.milo_wii`) |
| rb3-xenon | none — no xenon change was needed | |

## 1. The rndobj shape seam

The dc3 backend compiles inside the consumer's context, against that consumer's
rndobj headers, and it read DC3-only surfaces directly: `NgRnd`, `RndShaderMgr`,
`BaseMaterial` getters, `RndCam::GetViewProjectXfms`, `RndEnviron::Current()`,
`Rnd& TheRnd`, the fat DC3 `RndMesh::Vert`. RB3-Wii has none of them: one
`RndMat` holds the whole material, `TheRnd` is a `Rnd*`, verts pack colour as
`Color32`, and env/light/camera data are members without accessors.

`MILO_ENGINE_RNDOBJ_SHAPE` (`dc3` default | `rb3wii`) is orthogonal to the GPU
flavor. It selects `src/platform/rndshape/RndShape_{DC3,RB3Wii}.h` through
`RndShape.h`, and the `rb3wii` value adds a PUBLIC `MILO_RNDOBJ_SHAPE_RB3WII=1`, so
consumer TUs that include `Rnd_Wgpu.h` see the same shape the engine compiled.
Every rndobj read that differs between the two generations goes through
`rndshape::`:

| section | what it maps |
|---|---|
| renderer | `TheRndRef()`; `WgpuRndBase` = `NgRnd` (DC3) or a `Rnd` subclass carrying the four NgRnd virtuals WgpuRnd overrides (RB3-Wii) |
| material | `MatView`: the DC3 `BaseMaterial` getter surface over a Wii `RndMat`; DC3-only maps (normal/spec/rim/env/detail) report a fresh DC3 material's values |
| environment / light | current env, fog, light lists, light texture, `LightProjection` (DC3's `RndLight::Projection` over Wii members) |
| camera | `CamViewProjectXfms` (DC3's `GetViewProjectXfms` over Wii members; `UpdateLocal` is line-for-line the same in both), `CamZRange`, `CamScreenRect`, `CamViewProjMatrix` |
| mesh | geometry owner, compressed vert stream, vertex field access for the static/skinned unpackers |
| particles, cube textures | tile counts, cube face bitmaps |
| draw modes | `kDrawModeReflection` |
| camera select | `kCamSelectSetsViewport` |
| post-processing | `PostProcGrain` |

`RndShape_DC3.h` is identity forwarders, so a DC3-shaped consumer compiles the
same expressions as before. Measured: rb3-xenon's `rb3-render` default cells
(`tracksystem_meshes`, `crowd_female01`) are **byte-identical** (`cmp`) between
the base engine `e1b0c29` and the final branch commit, and the run prints
`RESULT: ALL GATES PASSED (0 gate failure(s))`.

### The rb3 side

`native/src/rb3_rnd_backend.h` declares `RB3RndBackend::`, the one seam the rb3
harnesses call: the `--viewer`, `RB3_RENDER_MESH`, the `RB3_GAME` boot, the web
boot, the HTTP debug server, uidump, gamewarm and joypad. None of them names
`gBandRnd` any more. Exactly one of `rb3_rnd_backend_rb3.cpp` and
`rb3_rnd_backend_dc3.cpp` links. The dc3 file also defines the hooks the RB3 fork
calls by name:
- draw-provenance scopes;
- the menu post-grade flush;
- the outfit-compose latch;
- the legacy `Tex`/`Text`/`Dir` factory aliases;
- the exit-time GPU teardown;
- the engine's `MeshFilter`/`DebugPanel` seams, with rb3-xenon's answers.

BandRnd-only instrumentation (draw log, progressive texture sharpen) reports
"off", and BandRnd's own test suites build only with the rb3 flavor.
(Superseded by section 14: both are implemented under dc3 and the suites build
for both flavors.)

One matched-fork edit was needed: rb3 `src/system/rndobj/Cam.cpp`
`RndCam::Select` (`#ifdef HX_NATIVE`) opens the camera's target texture as the
draw target, or returns to the frame. On the Wii that lived in the platform
camera; DC3's shared `Select` carries the same hook. Under BandRnd both calls are
the default empty bodies.

## 2. Per-game behaviour the RB3-Wii shape needed

Every item below was found on RB3's title screen (`RB3_GAME=1`, frames
60/200/400, 1280x720 headless) and measured before and after the change.

1. **Camera viewport and depth range: the big one.** RB3-Wii's shared
   `RndCam::Select` sets no viewport. On the Wii, `WiiCam::Select`
   (`rb3/src/system/rndwii/Cam.cpp`) called the base and then
   `GXSetViewport(..., mZRange.x, mZRange.y)`; DC3's shared `Select` ends with
   `TheNgRnd.SetViewport(...)`. The native Wii build has no platform camera, so
   WgpuRnd's viewport stayed at the zero-initialised `MinZ = MaxZ = 0`. Every
   fragment then wrote depth 0, and under the `Less` test **the first opaque draw
   at each pixel won**.
   - The sky gradient (draw 0, a sphere 8,532–13,090 units out) hid the
     skyline (4,847–8,296 units), the moon and the RB3 logo.
   - Early black geometry covered later geometry.
   - Skipping any late range of draws "fixed" it, which earlier read as a
     draw-count problem.

   Fix: `kCamSelectSetsViewport` (`true` for DC3, `false` for RB3-Wii). When it
   is false, `WgpuRnd::ApplyCameraViewport` applies DC3's/Wii's viewport on a
   camera change (`EnsureSceneUniformsCurrent`) and at `BeginDrawing`.

   How it was isolated: the same isolate on both flavors
   (`RB3_ISOLATE_MESH=sky` under BandRnd, a temporary name filter under dc3)
   showed the skyscrapers in front of the gradient under BandRnd and hidden
   under dc3. Projection terms (`world.cam` yy 2.0719 / xx 1.1654), world
   matrices and decoded vertex positions were then shown identical across the
   two, which left only depth state.

   Result: black pixels **15.4% → 3.6%** (BandRnd: 8.0%); the logo, skyline and
   moon are back.
2. **Reflection draw mode.** RB3's `WorldReflection::DrawShowing` sets mode 7,
   and DC3 renumbered it to 8. `Mesh_Wgpu` flips culling for the mirrored pass
   on `rndshape::kDrawModeReflection`.
3. **Post-process grain.** RB3 authors `mNoiseIntensity` around 3.0 as a gain on
   a noise *texture*; as a raw per-pixel add it buried the frame in white noise.
   `PostProcGrain` scales it the way the rb3 flavor's shader does (`kNoiseGain`
   0.04, magnitude clamp 3).
4. **Pre-initialised device.** RB3's `RB3_GAME` boot brings the `GpuDevice` up
   before `TheRnd->Init()` (so Dawn enumerates adapters from the original cwd).
   `WgpuRnd::Init` used to create a second device, and every resource bound to
   the first: 358k "cannot be used with [Device]" validation errors and a white
   screen. `Init` now reuses a ready device; DC3 and xenon never pre-init.
5. **Lazy texture upload for particles and rects.** DC3 uploads bitmaps from
   `MaterialSetup` (`PresyncBitmap`), but RB3's `RndTex::PostLoad` runs before
   the bitmap arrives. So `Part_Wgpu` and `DrawRect2D` now presync their diffuse
   too; cloud particles were untextured quads without it.
6. **Exit teardown.** RB3 leaves through `Debug::Exit` → `exit()`, not `_exit()`
   as DC3 does. The file-scope texture/mesh GPU caches then dropped the last
   device reference from a static destructor after the Vulkan loader began
   tearing down (gdb: `~GpuTexData` → `~VulkanInstance` → SIGSEGV).
   `WgpuRnd::Terminate` now empties them (`ClearGpuTexCaches`,
   `ClearMeshGpuCache`) before shutting the device down. RB3 dc3 exit:
   **rc 139 → 0**.

## 3. Measurements: dc3 flavor vs the current backend

Instrument: `pngstat.py` (pure-Python PNG reader).
- `diff` samples every 2nd pixel in both axes and reports the mean |Δ| per
  channel and the share of samples with any channel |Δ| > 32.
- `stats` reports the mean RGB and the share of near-black samples (all
  channels < 8).

**Control first.** The rb3 flavor on the branch vs an rb3 `master` build
(`69599d86b` + engine `e1b0c29`) differs on **0.63–0.72%** of samples (frames 60/200/400). Two runs
of the master build differ on **0.67–0.80%** (animation timing), so the facade,
the Cam hook and the shape seam leave BandRnd output inside its own noise.

Title screen, BandRnd vs the dc3 flavor (final commits):

| frame | BandRnd black | dc3 black | mean \|Δ\| | samples Δ>32 |
|---|---|---|---|---|
| 60 | 8.02% | 3.37% | (41.3, 40.8, 45.5) | 69.0% |
| 400 | 8.16% | 3.61% | (42.6, 44.1, 47.8) | 71.4% |
| 400, dc3 before the viewport fix | | 15.36% | (82.4, 74.4, 72.6) | 83.4% |

The structure now matches: same camera, logo, skyline, moon, Capitol marquee,
rooftops and cars. The remaining 71% is mostly global tone. The dc3 frame is
brighter (mean RGB 94/80/83 vs 80/67/66): WgpuRnd's DC3 lighting and post chain
are not BandRnd's RB3-specific grade, venue lighting and fog. Specific visible
differences:
- the textured cloud layer (`clouds` RT on the sky dome) is missing;
- the cyan street reflection is missing;
- the billboard RT is black;
- the logo is grey rather than lavender;
- the dc3 frame shows the rooftop band characters, which BandRnd does not draw.

`--viewer` cells (single milo, synthesized camera, 640x480; share of all pixels
further than 4 per channel from the modal colour, which is the clear colour
(31,36,46) in all eight images):

| cell | BandRnd | dc3 | why they differ |
|---|---|---|---|
| `tracksystem_meshes.milo_xbox` | 7.5% | 0.0% | all 129 meshes have no material (the track system assigns them at runtime); DC3's `DrawMeshImmediate` skips material-less meshes, BandRnd draws them with a default |
| `tracksystem_meshes.milo_wii` | 3.8% | 0.0% | same |
| `male_hair_crazyhawk_resource.milo_xbox` | 20.9% | 5.9% | skinned hair; its `bone_hair` parent does not resolve in a lone resource milo, so the palette collapses (rb3-xenon resolves dependencies first) |
| `male_hair_crazyhawk_resource.milo_wii` | 19.2% | 14.6% | as above; `.milo_wii` bitmaps arrive empty in both flavors (no GX texture decode exists in either) |

The viewer frames are byte-identical before and after the viewport and teardown
fixes, so those fixes did not move them.

## 4. What is left, and what was deliberately not done

> **Superseded in part by section 7 (lane W16-QE).** The cloud layer, the
> black billboard, the outfit render targets, the reflection pass's lighting
> and the GX texture decode are done there, and the ".milo_wii bitmaps arrive
> empty" note below was wrong (7.4). The rest of this list stands as written.

- **rb3's default is still `RB3_GPU_BACKEND=rb3`.** Flipping it is one line, but
  it needs this engine branch pinned first (the pinned `2ea8e34` has no shape
  seam), and it switches off BandRnd-only harness features: the draw log and its
  goldens, texture sharpen, and the WGSL validation suite. That is the
  coordinator's call. (Section 14: those features now work under dc3, so the
  flip no longer switches them off.)
- **Tone and post chain**: RB3's grade, venue lights and fog vs DC3's (section 3).
- **Render-target content**: the cloud layer and the billboard are RTs that
  RB3's systems fill (outfit/`TexRenderer`-style paths). They draw black or
  empty under dc3.
- **Planar reflection** draws (the mirrored city runs at 110–447 of ~890
  draws) but does not yet read like BandRnd's cyan street.
- **No GX texture decode**, so `.milo_wii` textures are empty in both flavors.
- **Loader crashes** on `51squier_sunburstblack.milo_wii` ("String chars
  1065353217 > 256") and on the extras/crowd milos happen in **both** flavors,
  so they are not renderer issues.
- **Web build** (`main_web.cpp`) compiles through the facade but was not built
  or run under emscripten. (Section 15: built and run; it needed two engine
  fixes and is now rb3-web's default.)
- **Venue cells in rb3-xenon's `rb3-render`** (`small_club_01`, `arena_01`)
  segfault identically on the base and branch engines (pre-existing, not
  touched).
- No `MILO_ENGINE_PIN` was bumped anywhere.

## 5. Reproducing

```sh
# rb3 (Wii), dc3 flavor
cd rb3/native && cmake -S . -B build-dc3 -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DRB3_NATIVE_GFX=ON \
  -DRB3_GPU_BACKEND=dc3 -DMILO_ENGINE_PATH=<engine checkout> \
  -DDawn_DIR=$HOME/code/milohax/dc3-decomp-deps/dawn/lib/cmake/Dawn
cmake --build build-dc3
# title screen, screenshots at frames 60/200/400
RB3_GAME=1 MILO_HEADLESS=1 MILO_MAX_FRAMES=420 MILO_SCREENSHOT_DIR=<out> \
  MILO_SCREENSHOT_FRAMES=60,200,400 RB3_DATA=<rb3>/orig-assets/extracted \
  build-dc3/rb3-native
# per-draw report for one frame (dc3 flavor)
MILO_CAPTURE_FRAME=60 ...
```

Under the rb3 flavor, screenshots are named `NN_fNNNN.png`; under dc3 they are
`frame_NNNNN.png`.

## 6. Consumer verification on this branch (2026-10-06)

Each consumer was built against this engine checkout (`MILO_ENGINE_PATH` pointed
at the branch worktree, confirmed in each `CMakeCache.txt`):

| consumer | instrument | result |
|---|---|---|
| rb3-xenon (on `c1f6bafef`) | `tools/native_health.sh` | `NATIVE_HEALTH_RESULT verdict=PASS link_verified=18 link_expected=18 link_skipped=0 runtime_ran=18 gates_pass=73 gates_fail=0 runtime_crashed=0 rc=0` |
| rb3-xenon | `tools/native_build_gate.sh` | PASS 18/18 (the line is in the lane report; it is the lane's last action) |
| rb3-xenon | `rb3-render` default cells vs base `e1b0c29` | byte-identical PNGs, `RESULT: ALL GATES PASSED` |
| dc3-decomp | `scripts/native_configure.sh` + `scripts/native_test.sh` | 626 registered, 557 executed, 557 passed, 0 failed, 69 skipped (budget 69), rc=0 |
| rb3 (Wii), rb3 flavor | title screen vs rb3 `master` | 0.63–0.72% of samples differ, inside the 0.67–0.80% run-to-run null |
| rb3 (Wii), dc3 flavor | title screen | renders the full scene, exits rc=0 (section 3) |

## 7. Parity pass (lane W16-QE, 2026-10-06)

Goal: bring the dc3 flavor up to the rb3 flavor (BandRnd) on RB3 content so dc3
can become rb3's default. **The default was not flipped, no branch was merged
and no pin was bumped**; those are the coordinator's.

Branches (not pushed):

| repo | branch | commits |
|---|---|---|
| milo-native-engine | `w16-qe-dc3-parity` | 10 code commits (including the merge of `w16-qe-wii-tex`) plus 3 doc commits on `5d5b02e`, engine `main` at the time |
| rb3 | `w16-qe-dc3-parity` | `5440427c7` on `04b189fcc` (`master`) |
| rb3-xenon, dc3-decomp | none — no consumer change was needed | |

`w16-qe-wii-tex` (`e0dee73`) is the pre-rebase copy of the Wii decode commit;
the one to land is its rebased twin `39e697c`, inside `w16-qe-dc3-parity`.

### 7.1 What changed

Every RB3-only behaviour sits behind the rndshape seam with an identity DC3
entry, so DC3 and rb3-xenon content does not take the new path. Five changes
are shared code and do reach DC3/xenon; they are bug fixes, marked **shared**.

| # | change | where | DC3 entry |
|---|---|---|---|
| 1 | **Cloud layer.** RB3's `RndTexRenderer::DrawToTexture` forces destination-alpha writes while a target is bound (`WiiMat::SetOverrideAlphaWrite`); NextPass materials never got their texgen transform, so `difference_clouds_b.mat` sampled one texel. `clouds_rnd.tex` alpha mean **1.2 → 96** | `kRenderTargetForcesAlphaWrite`; `FillTexGen` shared by both pass builders (**shared**) | false |
| 2 | **Per-draw uniform slots.** `Queue::WriteBuffer` runs at Submit, ahead of the whole frame, so particles, 2D rects and bloom sub-passes all read the *last* data written in the frame (the rooftop smoke appeared inside the cloud target) | `Part_Wgpu`, `DrawRect2D` vertex arenas; `BloomPass` 64 round-robin slots (**shared**) | — |
| 3 | **Black billboard.** A NextPass with its own diffuse (`billboard_texture.mat` under `adboard_01/02.mat`) was never presynced, so its upload missed and the pass drew black. Now shows the Ramones posters retail shows | `MaterialSetup` (**shared**) | — |
| 4 | **Scene lighting.** WgpuRnd's DC3 venue rig over-lit RB3 (title f400 city luma 68.7 vs retail 52.4). `rndshape::WriteSceneLighting` carries BandRnd's converged model (GX point falloff, exposure 0.70/0.80, ambient ×0.09 floored 0.008, char environs from the real list, flat key for non-world cameras), with BandRnd's env-var names | `rndshape/RB3WiiSceneLighting.cpp` (built only for dc3 + rb3wii) | `return false` |
| 5 | **Unlit materials.** Wii `use_environ=0 && pre_lit=0` takes the GX register colour only | `rndshape::MatUnlit` | false |
| 6 | **Particles.** RB3 positions are relative to `mRelativeXfm`; colour × material colour; translucent haze alpha ×0.35 with a near-camera fade (BandRnd's model) | `PartWorldPos`, `kPartMaterialTint` | `p->pos`, false |
| 7 | **Linear render targets.** `standard_wgsl` sRGB-encodes every fragment for the non-sRGB surface and a target is sampled back raw, so anything drawn through a target was encoded twice (the cloud target read 0.38 for a 0.12 material). Targets now store linear (`SceneUniforms.outputLinear`, replacing a pad float) | `kRenderTargetStoresLinear` | false |
| 8 | **Reflection camera.** `WorldReflection` draws through an unnamed copy of `world.cam` in draw mode 7; it is lit as the venue camera | `RB3WiiSceneLighting.cpp` | — |
| 9 | **Outfit composites were black.** `OutfitConfig::MatSwap::Compose` paints every `*_output` target in the pre-clear phase; restoring its camera made `MakeDrawTarget` open a `MainPassResume` pass before `BeginDrawing`'s own main pass, and Dawn invalidated the frame's whole command buffer (10 invalid frames in the first 60). `MakeDrawTarget` now resumes only once the frame pass exists → **0** | `Rnd_Wgpu.cpp` (**shared**) | — |
| 10 | **Outfit tint.** RB3's `DrawRect` colour is material colour × argument; Compose passes white and carries the palette tint in `sMat->SetColor()`. Garments, hair and eyes now take their palette colour (e.g. `denimjacket_clean` output mean RGB 133/134/135 → 94/101/116) | `kRectModulatesMatColor` in `DrawRect2D` | false |
| 11 | **Wii GX texture decode** (CMPR `0x48`; two-plane CMPR colour+alpha `0x148`; RGBA8 `0x40` and I8 `0xC0` implemented but unused by shipped data) with a rebuilt box mip chain | `gfx/GxTextureDecode`, `kGxTextureLayout` | false (`if constexpr`) |
| rb3 | **`TheNgStats` was null.** `band3_link_stubs.s` gave it a weak 256-byte zero blob, so `SpotlightDrawer` segfaulted every frame of a song (2,410 caught crashes); the facade now defines a real `NgStats`. Highway, HUD and spotlights render, and `game_screen` arrives at frame ~1,120 instead of ~1,900 | rb3 `native/src/rb3_rnd_backend_dc3.cpp` | — |

### 7.2 Title screen against retail

Instrument: `tools/rb3-dc3-parity/title_fidelity.py` against
`rb3/images/retail-screenshots/title_screen_360_tcrf.png` (the TCRF Xbox 360
capture). `sky_dE` / `city_dE` = mean |ΔRGB| (0–255) between 16×16-box-blurred
320×180 downsamples, sky = top 30%; lower is better. Both flavors were built
from the same rb3 + engine worktrees and run back to back
(`tools/rb3-dc3-parity/title_capture.sh`, Xbox data, 1280×720 headless).
Retail: luma 50.1, sky 44.8, city 52.4.

| frame | BandRnd sky_dE / city_dE | dc3 before this lane | dc3 now | dc3 now: luma / sky / city |
|---|---|---|---|---|
| 60 | 55.1 / 21.3 | 53.2 / 45.2 | **49.1 / 20.7** | 61.1 / 91.4 / 48.1 |
| 200 | 43.5 / 24.1 | 53.0 / 45.7 | 43.9 / **23.4** | 61.5 / 86.7 / 50.8 |
| 400 | 66.6 / 21.9 | 52.9 / 45.6 | **46.8 / 20.8** | 60.1 / 89.2 / 47.6 |

Run-to-run noise on the same build is about ±0.4 sky_dE (two dc3 runs at
frame 200: 43.8 and 44.2), so frame 200's sky is a tie and every other cell
is better under dc3.

The order in which the changes landed, frame 200 / 400 sky_dE (city_dE),
shows why they belong together: unlit materials alone made the sky worse until
the render-target encoding was fixed.

| step | f200 | f400 |
|---|---|---|
| lane start (engine `cfef5a2`; city luma 81) | 53.0 (45.7) | 52.9 (45.6) |
| cloud layer, per-draw slots, billboard presync | — | 70.3 (31.8) |
| + lighting seam | 50.5 (24.5) | 60.3 (21.4) |
| + unlit | 72.3 (26.9) | 91.1 (22.5) |
| + particle tint | 71.5 (26.0) | 90.0 (22.5) |
| + linear render targets | 44.1 (23.5) | 47.0 (20.9) |
| + reflection camera | 44.3 (23.5) | 46.8 (20.8) |
| + pass resume, rect tint, Wii decode, rebase | 43.9 (23.4) | 46.8 (20.8) |

What the frames show (compare `frame_00400.png` with the retail capture):
cloud layer, billboard posters, the rooftop band, skyline, Capitol marquee and
cars are present. **The rooftop characters are correct**: retail shows the
guitarist and singer on the left roof and the drummer/keys on the right; BandRnd
does not draw them, so "extra characters" was a BandRnd defect, not a dc3 one.
Remaining differences, all present under BandRnd too (the first three are
fixed in section 8): the sky is about twice retail's luma (the clouds read grey-lavender rather than dark purple), there is
no bloom/glow around lights, and the logo outline is grey-white rather than
lavender.

The street: retail shows no visible mirror reflection, only a green haze and
light bloom. Under dc3 the reflection pass draws (128 mirrored meshes) beneath
`city_road.mat` (SrcAlpha, alpha 0.9), so about 10% of it shows. BandRnd's cyan
street is not in retail and was not reproduced.

### 7.3 Venues

Instrument: `tools/rb3-dc3-parity/venue_capture.py` drives Quickplay over the
HTTP API (guitar, expert, no-fail, autohit), waits for `game_screen`, and
captures at game-relative frames 60/300/600/900; `frame_stats.py` gives global
luma. Camera cuts are random, so the two flavors show different shots and
only the distributions compare.

| | mean luma of the 4 shots | range |
|---|---|---|
| retail gameplay captures (5 images in `retail-screenshots/`) | 54.0 | 19.2–74.8 |
| BandRnd | 40.4 | 29.6–53.8 |
| dc3, before 7.1 #9/#10 (outfits black) | 72.2 | 20.5–102.1 |
| dc3 now | 39.9 | 36.0–42.1 |

Under dc3 the highway, gems, HUD, spotlights, crowd and the B&W/tinted
post-processing shots render, and band clothing carries its palette colours.
BandRnd's crowd dim (material colour ×0.10 on crowd extras and impostor
billboards) was **deliberately not ported**: it compensates for BandRnd's own
venue lighting, and dc3 venue luma is already inside retail's range without it.

### 7.4 Wii data

The game does not boot on Wii data under either flavor (`band_preinit_keep.dta`
fails with "Couldn't find 'mem'"), so Wii textures were measured in `--viewer`
cells (640×480, synthesized camera) against the same milo's `.milo_xbox`
render, which uses the trusted Xbox DXT path. Mean RGB distance over drawn
pixels, 78 Wii cells that render:

| | before | after |
|---|---|---|
| pixel-weighted mean | 87.2 | 63.0 |
| median cell | 81.2 | 35.9 |
| `list_store_storefront` | 132.6 | 2.8 |
| `campaign_topmeter` | 214.2 | 8.9 |

46 cells moved closer, 6 further (framing or the asset's own material colour,
checked by eye), 20 unchanged. All 72 Xbox-data cells are byte-identical
before and after. The section-3 note that ".milo_wii bitmaps arrive empty" was
wrong: the pixels arrived intact and were decoded as Xbox BC1. The crazyhawk
cell is empty because its `hair_straight.bmp` is absent from `wii-extracted`.

### 7.5 Not done

- **Default switch, merges, pin bumps** — coordinator.
- **BandRnd-only features not ported:** halo bloom and the highway track-light
  block; the menu-UI post-grade boundary, chroma preservation and soft-clip
  ceiling of `RB3PostProc`; sustain-tail colour; render-to-texture without
  depth; texture sharpen; the draw log and its goldens; the WGSL validation
  suite (these three closed in section 14); BandRnd's skinning workarounds. None of them was needed to match or
  beat BandRnd on the frames above; bloom is the most visible remaining gap
  against retail.
- **Crowd recolouring.** `Crowd.cpp` sets `kColorModModulate` with three random
  colours per crowd character; no backend implements `mColorMod`, and the Wii
  `WiiMat` decomp has no consumer of it to copy, so its semantics are unknown.
  Implemented from retail's shaders in section 14.4; inert on shipped content.
- **Sky brightness and logo colour** (7.2) — shared with BandRnd. Fixed in
  section 8 (gamma-space shading).
- **`PresyncBitmap`** fingerprints every texture on every draw (~170k calls in
  220 title frames); a performance issue, untouched.

### 7.6 Consumer verification (engine `w16-qe-dc3-parity`)

Each consumer was built in its own `~/tmp` worktree against the engine
worktree (`MILO_ENGINE_PATH`, confirmed in each `CMakeCache.txt`).

| consumer | instrument | result |
|---|---|---|
| rb3-xenon (on `9fd8de7b1`) | `tools/native_health.sh` | `NATIVE_HEALTH_RESULT verdict=PASS link=PASS link_verified=18 link_expected=18 link_skipped=0 runtime=PASS runtime_ran=18 runtime_total=18 gates_pass=77 gates_fail=0 unrunnable=none selftest=SKIPPED scatter_unlinked=16 scatter_dirb=0 scatter_multihost=17 rc=0 handpose_controls=- handpose_baseline_fail=- runtime_crashed=0 runtime_failed=none` |
| rb3-xenon | `tools/native_build_gate.sh` | run as the lane's last action; its `NATIVE_GATE_RESULT` line is in the lane report (the health run's embedded link gate read `verdict=PASS expected=18 verified=18 skipped=0 partial=0 failed=0 rc=0`) |
| rb3-xenon | `rb3-render` default cells, branch vs base engine `5d5b02e` | both `RESULT: ALL GATES PASSED`; both PNGs (`tracksystem_meshes`, `crowd_female01`) byte-identical |
| dc3-decomp (on `e992ee9b5`) | `scripts/native_configure.sh` + `scripts/native_test.sh` | 626 registered, 557 executed, 557 passed, 0 failed, 69 skipped (budget 69), rc=0 |
| rb3 (Wii), both flavors | title (7.2) and venues (7.3) | dc3 renders title and Quickplay to `game_screen`, exits cleanly |

Two environment traps hit on the way, neither an engine problem: a fresh
dc3-decomp worktree needs `archive/` and `orig-assets/` symlinked from the main
checkout. Without `archive/`, one more test skips (70 > budget 69 fails the
ratchet); with `archive/` but without `orig-assets/`, the two `XeniaGolden`
tests and `HttpInputTest.PressReachesTheUIAsAPad0Button` launch `dc3-native`
with no game data and fail ("could not find game data", SIGFPE before ready).
With both links all three pass.

## 8. Bloom and colour grading (lane W16-QN, 2026-10-06)

Section 7.5 left bloom/halo as the most visible gap against retail, followed by
RB3's colour grading. Both are now ported for RB3 content, behind the rndshape
seam, together with the shading fix that made a faithful port possible.

| repo | branch | base |
|---|---|---|
| milo-native-engine | `w16-qn` | `14b3c2d` (engine `main`, the W16-QE merge) |
| rb3, rb3-xenon, dc3-decomp | none — no consumer change was needed | |

### 8.1 Method: the retail shaders, not BandRnd

`RB3PostProc` (BandRnd's halo bloom and grade) is a hand-tuned approximation,
so it was not the source. RB3 ships its Xbox 360 shaders in one container,
`xbox_shaders` (XOBX v1). `tools/rb3-dc3-parity/xobx.py` parses it the way
`DxShaderMgr::LoadShaderFile` reads it and dumps each permutation's microcode;
xenia's `xenia-gpu-shader-compiler --shader_output_type=ucode` disassembles
them. The constants come from rb3-xenon's `rndobj/PostProc_NG.cpp`
(`DoBloom`, `SetBloomColor`, `ModulateColorXfm`). The chain, now in
`gfx/RB3RetailPost`:

1. **Mask.** Materials that `NgMat::AllowHDR()` admits (not alpha-blended,
   alpha-cut or alpha-writing) are drawn with `standard.ps`'s pseudo-HDR bit,
   which writes `a = dot(rgb, c7)` with `c7 = (0.3, 0.59, 0.11) × s`, where
   `s = 1/threshold` when the threshold is above 1, else 1. Other draws leave
   alpha alone, and the clear writes alpha 0.
2. **bloom.ps.** Averages `rgb × a` over the scene into a quarter-size target.
3. **Blur.** 15 taps, offsets −6.5 to 7.5 texels, horizontal then vertical,
   with `SetBloomBlurWeights`' Gaussian.
4. **downsample_4x.** A 4×4 box into the next set. There are three sets in all.
5. **postprocess.ps.** A screen blend `1 − (1 − b·c6)(1 − scene)`, where `b` is
   the sum of the three sets and `c6 = bloomColor × BloomIntensity()`. Then the
   `RndColorXfm` matrix (its 3×3 scaled by the flicker modulation) is applied
   as `sat(dp4)`.

Vignette, posterize, chromatic aberration and grain are kept from the generic
composite and applied after the transform.

### 8.2 Why the first faithful port failed: RB3 shades in gamma space

The first port (`c942ad9`) made the title far worse. Sky_dE went
**46.8 → 102.0** and city_dE **20.8 → 46.5** at frame 400. Venues washed out:
mean luma rose from 52.9 to 130.6 over 4 random shots, against retail's 54.

The cause was upstream of the post chain. Retail renders into a
`D3DFMT_A8R8G8B8` target with no linearization (`DxRnd::CreateEDRAMSurfaces`).
`standard.ps` multiplies the gamma-space texel by the material colour and the
lighting sum exactly as authored. The dc3 standard shader instead decodes
textures through sRGB views and encodes its output, so every factor it does
not decode is brightened. (Correction, section 9.5: the texture views are not
sRGB. Bitmaps upload as `*Unorm`, so the texel is not decoded either.) A material colour of 0.3 acts like about 0.58. The
title sky's ~2× brightness (7.2), which BandRnd shares, is this effect, and the
bloom mask is driven by the same over-bright luma.

For RB3 (`rndshape::kGammaSpaceShading`), `standard_wgsl` now decodes the
material colour and the lit term. Then `enc(dec(t) · dec(c)) = t · c`, which
is what retail computes. Particles decode their colour the same way
(`Part_Wgpu`).

(Superseded, section 10: since the texel is not decoded, that decode gave
`enc(t) · c · L`, not `t · c · L`. Under `kGammaSpaceShading` the shader now
does no decode and no encode at all.)

### 8.3 The other pieces

| change | where | DC3 entry |
|---|---|---|
| retail chain (8.1), used instead of the generic composite when an RB3 postproc is current | `gfx/RB3RetailPost`, `PostProcPass::Run`, `rndshape/RB3WiiPostChain.cpp` | `kRetailPostChain = false` |
| gamma-space shading (8.2) | `standard_wgsl.inc` (`MaterialUniforms.gammaShading`, replacing a pad float), `MaterialSetup`, `Part_Wgpu` | `kGammaSpaceShading = false` |
| bloom mask into scene alpha: `MaterialUniforms.bloomMaskScale` (the other pad float), `alphaWrite` keyed on for those draws, particles write the mask or keep RGB-only writes, frame clear alpha 0 | `Mesh_Wgpu`, `Part_Wgpu`, `Rnd_Wgpu::BeginFramePass` | `BloomMaskScale() = 0` |
| **world-end flush.** RB3 draws the note highway, gems and HUD *after* `Rnd::EndWorld`, which runs the postproc, so on retail they are neither bloomed nor graded. `WgpuRndBase::DoPostProcess` now ends the main pass, grades the world into a frame-sized texture, resumes the pass and blits the result back. Later draws land on top, and `EndDrawing` skips the post chain for that frame | `Rnd_Wgpu.cpp` `FlushWorldPost`, `RB3RetailPost::Blit` | not compiled (`#ifndef MILO_RNDOBJ_SHAPE_HAS_NGRND`) |
| ambient fallbacks re-expressed (8.5) | `RB3WiiSceneLighting.cpp` | not built for DC3 |

Inspection: `MILO_RB3_RETAIL_POST=0` uses the generic dc3 composite, `raw`
applies no post at all, `mask` shows the bloom mask, `bloom` shows the bloom
term, and `grade` applies the colour transform without bloom. Unset means the
full retail chain.

### 8.4 Title screen against retail

Same instrument as 7.2 (`title_capture.sh` + `title_fidelity.py`, sky_dE /
city_dE, lower is better). Run-to-run noise is about ±0.4.

| frame | base `14b3c2d` | `w16-qn` |
|---|---|---|
| 60 | 49.1 / 20.8 | **21.6 / 18.5** |
| 200 | 44.2 / 23.4 | **20.6 / 22.1** |
| 400 | 46.8 / 20.8 | **18.6 / 18.3** |

Sky_dE is better by more than half at every frame, and city_dE is better at
every frame. Frame 400's sky luma is now 54.4 (retail 44.8; base 89.2), and its
city luma is 52.1 (retail 52.4). By eye: the sky is dark purple rather than
grey-lavender, the logo outline is lavender, and lights glow. The 7.2 / 7.5
items "sky about twice retail's luma" and "logo outline grey-white" are fixed.
`frame_stats.py` at frame 400:

| | luma | p10 | dark % |
|---|---|---|---|
| retail | 50.1 | 11.8 | 25.7 |
| base | 60.0 | 9.6 | 18.0 |
| `w16-qn` | 52.8 | 21.1 | 8.6 |

Mean luma moved closer to retail. The darkest tenth moved away: shadows are
lifted. Two candidates were not separated: the bloom term adds light to dark
pixels near lights, and the lighting fallbacks (8.5) keep a floor.

Frame 400, steps and ablations (sky_dE / city_dE):

| build | f400 |
|---|---|
| base | 46.8 / 20.8 |
| faithful chain, linear shading (`c942ad9`) | 102.0 / 46.5 |
| + gamma-space shading, alpha mask, world-end flush | 18.4 / 18.5 |
| + ambient re-expression (shipped) | 18.6 / 18.3 |
| shipped, `MILO_RB3_RETAIL_POST=0` (generic composite instead of the chain) | 16.7 / 28.6 |
| shipped, `raw` (no post) | 17.1 / 28.4 |
| decoding the material colour but not the lit term | 43.4 / 37.6 |
| decoding only the directional-light part of the lit term | 43.0 / — |

Gamma shading fixes the sky. The retail chain is what fixes the city
(28.6 → 18.3): its grade and glow.

### 8.5 Venue lighting under gamma shading

The venue lighting model (7.1 #4) is a heuristic, not retail. Its ambient
fallbacks were fitted under linear shading, where the output encode lifted a
lit term v to about linearToSrgb(v). With gamma shading they darkened every
venue: mean luma fell to 26.6.

There are four fallbacks:
- the scale for an unauthored near-white ambient (0.09);
- the ambient floor (0.008);
- the character approx-ambient share (0.11) and its cap (0.14).

They are now passed through linearToSrgb (0.332, 0.086, 0.366, 0.410), so they
keep the brightness they were fitted to. They are still read in BandRnd's
units and under its env-var names. The light exposures (0.70/0.80) and the grey
key were tried both ways and left unchanged. Authored light colours are used as
authored, and re-expressing or raising them pushed the title further from
retail.

Instrument: `venue_capture.py` with `RB3_FIXED_CLOCK=1`, frames
60/180/300/420/600/780/900/1020, 8 shots. Camera cuts still differ run to run:
the same build has read 32.5, 34.8 and 35.0. The retail row is the five
gameplay captures in `retail-screenshots/`, which are different songs and
venues, so only the distributions compare. (7.3 quoted their mean as 54.0;
`frame_stats.py` gives 57.7.)

| config | title f60 / f200 / f400 | venue luma | p10 | dark % |
|---|---|---|---|---|
| retail | — | 57.7 | 9.4 | 30.5 |
| base `14b3c2d` | 49.1/20.8 · 44.2/23.4 · 46.8/20.8 | 68.1 | 25.4 | 11.3 |
| BandRnd ambients | 21.3/18.3 · 20.3/20.4 · 18.6/18.4 | 26.6, 27.7 | 7.7 | 64.9 |
| **shipped**: ambients re-expressed | 21.6/18.5 · 20.6/22.1 · 18.6/18.3 | 32.5–35.0 | 11.2–12.9 | 46–52 |
| + all constants re-expressed (exposures 0.854/0.906, grey key 0.506) | 22.4/21.4 · 21.8/26.1 · 18.8/20.8 | 34.8 | — | 49.7 |
| + exposures 1.0 (env `RB3_VENUE_POINT_EXPOSURE=1 RB3_VENUE_DIR_EXPOSURE=1`) | 22.5/21.8 · 22.1/27.4 · 19.1/21.4 | 42.2, 51.1 | 19.2 | 23.0 |

**Venues are a mixed result.**
- Mean luma is now further from retail than the base was (|Δ| about 24 vs 10).
- The shadow statistics are closer: p10 is 12 vs base 25.4 (retail 9.4), and
  dark % is about 48 vs base 11.3 (retail 30.5).
- By eye, the base showed a pale grey crowd washed over the whole frame. Now
  the crowd sits in shadow and the band and highway carry the light, as in the
  retail captures, but the whole frame is darker than retail.
- The exposure-1.0 row is the closest venue configuration on every venue
  statistic, but it costs the title 1–4 city_dE.

That trade is a question about the lighting model, not the post chain. The
model ships projected lights off (`RB3_ENV_PROJLIGHT`), and its exposures are
fitted rather than taken from retail. It is left to a lighting lane. The env
vars let a consumer choose the other trade-off without a rebuild.

> **Superseded by section 9 (W16-QS).** The default is now the retail light
> model. This heuristic remains behind `RB3_VENUE_LIGHT_LEGACY=1`, and the env
> vars above apply only there.

### 8.6 Not done

- **BandRnd's highway halo / track-light block** was not ported. Retail draws
  the track after `EndWorld`, unbloomed (8.3), so a halo there would not be
  retail.
- **Frames that never call `EndWorld`** (menus) still run the chain at
  `EndDrawing` over the UI too. Whether retail grades those was not verified.
- **Venue lighting** (8.5): projected lights, and a retail-derived exposure. Done in section 9.
- `PresyncBitmap`'s per-draw fingerprinting (7.5) — untouched.
- No merge, pin bump or push.

### 8.7 Consumer verification (engine `w16-qn`)

Each consumer was built in its own `~/tmp` worktree against the engine
worktree (`MILO_ENGINE_PATH`, confirmed in each `CMakeCache.txt`).

| consumer | instrument | result |
|---|---|---|
| rb3-xenon (on `ca61b767f`) | `tools/native_health.sh` | `NATIVE_HEALTH_RESULT verdict=PASS link=PASS link_verified=18 link_expected=18 link_skipped=0 runtime=PASS runtime_ran=18 runtime_total=18 gates_pass=77 gates_fail=0 unrunnable=none selftest=SKIPPED scatter_unlinked=16 scatter_dirb=0 scatter_multihost=17 rc=0 handpose_controls=- handpose_baseline_fail=- runtime_crashed=0 runtime_failed=none` (embedded link gate: `verdict=PASS expected=18 verified=18 skipped=0 partial=0 failed=0 rc=0`; layout-ODR `PASS`, x360 1,266 TUs, native 1,713) |
| rb3-xenon | `tools/native_build_gate.sh` | run as the lane's last action; its `NATIVE_GATE_RESULT` line is in the lane report |
| rb3-xenon | `rb3-render` default cells, branch vs base engine `14b3c2d` | both `RESULT: ALL GATES PASSED (0 gate failure(s))`; both PNGs (`tracksystem_meshes`, `crowd_female01`) byte-identical |
| dc3-decomp (on `e992ee9b5`) | `scripts/native_configure.sh` + `scripts/native_test.sh` | 626 registered, 557 executed, 557 passed, 0 failed, 69 skipped (budget 69), rc=0 |
| rb3 (Wii), dc3 flavor (default) | title (8.4) and venues (8.5) | renders title and Quickplay to `game_screen`, exits cleanly |

One environment trap, not an engine problem: the gate's layout-ODR check
compiles the X360 TUs from `build.ninja`. A plain `git worktree add` of
rb3-xenon has none, so the check reports UNRUNNABLE and the gate goes
INCOMPLETE (rc=3). In a worktree made with `scripts/setup_worktree.sh` it
passes.

## 9. Venue lighting from the retail light model (lane W16-QS, 2026-10-06)

Section 8.5 left venues at a mean luma of 32.5–35 against retail's 57.7. Its
model was a heuristic with fitted constants: exposures 0.70/0.80, re-expressed
ambient fallbacks, a grey key, and projected lights off. This lane replaces it
with the model the Xbox 360 retail build runs. Nothing in it is fitted.

| repo | branch | base |
|---|---|---|
| milo-native-engine | `w16-qs` | `w16-qn` (`7952f03`) |
| rb3, rb3-xenon, dc3-decomp | none — no consumer change was needed | |

### 9.1 Sources

- rb3-xenon `rndobj/Env_NG.cpp`: `NgEnviron::Select`, `SetPointLightRegisters`
  and `UpdateApproxLighting`.
- `rndobj/BoxMap.cpp`: `BoxMapLighting`.
- `rndobj/Mat_NG.cpp`: `NgMat::SetupAmbient`.
- `rndobj/Shader.cpp`: the option bits.
- `rndobj/Mesh.cpp`: `RndMesh::sUpdateApproxLight = true`. Its retail byte is
  `0x01`.
- `char/Character.cpp`: `DrawLodOrShadow`.
- The shipped `xbox_shaders` `standard` permutations, dumped with `xobx.py`
  and disassembled with xenia (8.1).

The model:

1. **Ambient.** The environ's ambient colour, unscaled (`c1`).
2. **Real lights.** At most two point lights that are `Showing()` with a
   non-zero packed colour, taken in list order. Lambert. The light is full
   strength inside `falloffStart` and fades linearly to zero at `range`:
   `sat(d·pos.w + col.w)`, where `pos.w = 1/(falloffStart − range)` and
   `col.w = −range·pos.w`. At most one projected light.
3. **Approx lights.** These are `mLightsApprox` plus the spotlight set in
   `RndEnviron::sGlobalLighting`. They are folded on the CPU into a six-face
   box map (`c80..c85`), and `standard.vs` weights each face by the positive
   part of the normal along that axis.
   - **The box map is per mesh.** While `sUpdateApproxLight` is set (its
     default), each mesh recomputes it at its world sphere centre, falling
     back to its origin.
   - `Character::DrawLodOrShadow` clears the flag and evaluates once, at the
     character's sphere centre.
4. **Shader.** `standard.vs` computes `material · (ambient + box(N) + points)`
   per vertex. `standard.ps` multiplies that by the texel, and nothing clamps
   it before the 8-bit target.
5. **Prelit materials.** A prelit material that uses the environ takes
   `vertexColour · ambient` as its ambient term. Only `use_environ = 0` keeps
   the bare vertex colour, because `SetupAmbient` loads an ambient of 1.

Some features are not modelled:

- **Projected lights.** Every one the shipped venues carry is blend 1
  (`shadow_projected.lit`). Retail uses those only to darken the light terms
  inside the projected shadow map, and only in the `per_pixel_lit`
  permutations, so they add no light.
- **The venue `SpotlightDrawer`.** It has influence 0, so a global set of 0 is
  genuine.
- **Pseudo-HDR (bit 22).** It only writes the luma mask into alpha (8.1).

### 9.2 What changed

| change | where | DC3 entry |
|---|---|---|
| `SceneUniforms.retailLighting` (a former pad float) gates the model; point falloff mode 2 = retail's linear falloff | `UniformStructs.h`, `standard_wgsl.inc` | never set, so the existing path is unchanged |
| `ObjectUniforms.boxLight[6]` (128 → 224 bytes): the per-draw box map | `UniformStructs.h`, `standard_wgsl.inc`, `Mesh_Wgpu.cpp` | zero |
| `rndshape::FillMeshApproxLighting`: runs `BoxMapLighting` at the mesh's (or its character's) sphere centre | `RB3WiiSceneLighting.cpp` | inline no-op |
| `WriteRetailLighting`: ambient + two point lights, written once per environ | `RB3WiiSceneLighting.cpp` | not built |
| per-vertex lit term (`VertexOutput.vertexLit`), no soft clip; `prelit = 2` for prelit + use_environ | `standard_wgsl.inc`, `MaterialSetup.cpp`, `rndshape::MatPrelitAmbient` | `MatPrelitAmbient` returns false |

The model applies to `world.cam` (and the reflection pass) with an environ.
Other cameras keep the flat key (8.5).
**`RB3_VENUE_LIGHT_LEGACY=1` restores the 8.5 heuristic.** Measured: title
f60/f200/f400 = 21.6/18.3 · 20.7/21.9 · 18.5/18.3, which is the `w16-qn` row
within noise. The 8.5 environment variables still tune that path.

### 9.3 Venues

Instrument as in 8.5: `venue_capture.py` with `RB3_FIXED_CLOCK=1`, 8 shots.
Camera cuts differ run to run; `w16-qn` has read 30.0–35.0.

| config | luma | p10 | dark % |
|---|---|---|---|
| retail (5 gameplay stills) | 57.7 | 9.4 | 30.5 |
| base `14b3c2d` | 68.1 | 25.4 | 11.3 |
| `w16-qn` (this lane's run) | 30.0 | 8.8 | 54.2 |
| `w16-qn` (8.5's runs) | 32.5–35.0 | 11.2–12.9 | 46–52 |
| box map once per environ, at the `Select` position (probe) | 38.8 | 11.8 | 36.6 |
| texel also decoded (probe, see 9.5) | 32.1 | 10.1 | 51.1 |
| **`w16-qs`**, run 1 | **43.6** | **13.8** | **34.0** |
| **`w16-qs`**, run 2 | **39.7** | **14.1** | **42.9** |

Per-shot luma for run 1: 21.0, 27.5, 12.2, 33.9, 41.4, 67.0, 70.0, 75.9.

**What moved:**
- Mean luma is closer to retail: |Δ| is about 16, against 23–28 for `w16-qn`.
- Dark % is closer: |Δ| is 4–12, against 16–24.
- The darkest tenth is slightly lifted: p10 is 14 against retail's 9.4.

By eye, lit performers and set pieces now carry the light, and dark shots
stay dark. Evaluating the box map per mesh rather than once per environ is
worth about 3–5 luma. That is the difference between
`sUpdateApproxLight` and the probe row.

The remaining deficit is about 14–18 luma. It is not a constant to fit. The
Wii material loader (rb3 `rndobj/Mat.cpp`) drops the Xbox material features
that add light on retail: `per_pixel_lit`, specular, rim, normal maps and
environment maps. They cannot be reproduced from the Wii data.

### 9.4 Title screen

Same instrument as 8.4: sky_dE / city_dE, lower is better.

| frame | `w16-qn` | `w16-qn` rerun | `w16-qs` | `w16-qs` rerun |
|---|---|---|---|---|
| 60 | 21.6 / 18.5 | 21.6 / 18.5 | 30.2 / 19.6 | 30.1 / 18.8 |
| 200 | 20.7 / 22.2 | 20.4 / 21.9 | 33.2 / 21.6 | 33.4 / 21.1 |
| 400 | 18.6 / 18.3 | 18.6 / 18.3 | 25.5 / 19.4 | 25.0 / 18.6 |

| f400 | luma | sky luma | city luma | p10 | dark % | city_edge |
|---|---|---|---|---|---|---|
| retail | 50.1 | 44.8 | 52.4 | 11.8 | 25.7 | 1 |
| `w16-qn` | 52.7 | 54.2 | 52.1 | 21.1 | 8.6 | 0.729 |
| `w16-qs` | 62.1 | 65.1 | 60.9 | 24.8 | 3.6 | 0.785 |

**The title is worse than `w16-qn`. This lane's done condition is not met
there.**
- Sky_dE is up 6.5–13 at every frame. City_dE is within about 1, and
  city_edge (structure) improved.
- What improved: the logo outline is lavender-blue, as in retail, instead of
  grey, and the sky glow around the lights is now present, as in retail.
- What got worse: the whole frame is brighter. Sky luma is 65 against
  retail's 45, and the shadows lift further.

The pieces were separated with a probe (`RB3_RETAIL_LIGHT_EXCEPT`, not
shipped). It kept the 8.5 heuristic for one title environ at a time and the
retail model for the rest. f400 sky_dE / city_dE, on the probe builds before
the per-mesh box map:

| environ kept on the heuristic | f400 |
|---|---|
| none | 23.5 / 19.1 |
| `logo` | 24.0 / 20.1 |
| `back_left`, `buildings_dim`, `cityscape`, `street`, `theater`, `train` | 23.0–23.5 / 18.4–19.2 |
| `char_rooftop` | 23.3 / 20.1 |
| `rooftop_foreground` | 23.2 / **16.8** |
| **`sky`** | **17.9–20.2** / 19.4–20.2 |

**Only `sky.env` moves sky_dE.** Its lit meshes are `skynight.mesh`
(`sky_gradient.mat`, use_environ 1) and the moon. The cloud layers
(`sky_dome.mat`, `sky_dome02.mat`) have use_environ 0 and are unlit under
both models.

`sky.env` has an ambient of (0.34, 0.46, 0.51) and one real point light,
`sky_light01`:
- colour up to 2.0;
- range 4000;
- falloff start 0.

Under retail's linear falloff, that light is at full colour near its source.
The heuristic scaled it by 0.70 and used GX's inverse-linear falloff.
Assuming the light values are right, the brighter sky is retail's own model
on these data. Why it still reads brighter than retail's screenshot is 9.5.

The `rooftop_foreground` row also suggests the city foreground runs a little
bright under the retail model.

### 9.5 Finding: textures are not decoded, and 8.2 is wrong about that

8.2 says the dc3 shader "decodes textures through sRGB views". **It does
not.**
- `gfx/TextureConvert.cpp` uploads every bitmap as `BC1RGBAUnorm`,
  `BC3RGBAUnorm` or `RGBA8Unorm`. No sRGB format or view is used anywhere.
- The standard shader decodes the material colour and the lit term, then
  encodes its output. A draw therefore computes about `enc(t · c · L)`, where
  retail computes `t · c · L`. A texel of 0.2 acts like about 0.48.
- The 8.5 heuristic absorbed this. Its fitted ambients were small. With
  retail's unfitted lit term the error shows: it is the 2× sky floor (7.2,
  8.4), now more visible.

Decoding the texel as well should then give retail's formula. **Measured, it
makes everything far too dark** (section 10.3: that probe was not retail's
formula either. It still stored linear values in render targets, so a texel
read back from one was decoded twice, and it kept the tanh highlight
shoulder):

| f400 | luma | sky luma | city luma | p10 | dark % | sky_dE / city_dE |
|---|---|---|---|---|---|---|
| texel decoded | 25.7 | 13.3 | 31.0 | 5.6 | 60.4 | 34.4 / 22.9 |

The venue luma for that build is 32.1. So the textures sit between "raw"
(too bright) and "decoded" (too dark), and neither reproduces retail.

The candidates were not separated, and that is the next step for this gap:
- Wii texture data that differs from the Xbox data;
- the gamma ramp retail applies at present time (`DxRnd::SetupGamma`, config
  `rnd/gamma`);
- something upstream of the lit term.

No texture decode ships.

### 9.6 Not done

- **The title sky** (9.4, 9.5). It was not tuned away. Keeping `sky.env` on
  the heuristic would score better, but nothing in retail lights it that
  way.
- **Xbox-only material features** dropped by the Wii loader: `per_pixel_lit`,
  specular, rim, normal and environment maps. As a consequence, neither the
  projected blend-1 shadow nor the per-pixel light path has anything to
  modulate.
- **The vignette.** Retail's (bit 36) is
  `w = sat((|uv − 0.5|² + I) · 3.724 − 4.655)²`, which differs from the
  generic one kept in 8.1. The title has none.
- **Ambient alpha.** It was not modelled.
- No merge, pin bump or push.

### 9.7 Consumer verification (engine `w16-qs`)

The consumers are the 8.7 worktrees, repointed with `-DMILO_ENGINE_PATH` at
the `w16-qs` engine worktree. The setting was confirmed in each
`CMakeCache.txt`, and the engine library was rebuilt in each.

| consumer | instrument | result |
|---|---|---|
| dc3-decomp (on `e992ee9b5`) | `scripts/native_configure.sh` + `scripts/native_test.sh` | 626 registered, 557 executed, 557 passed, 0 failed, 69 skipped (budget 69), rc=0 |
| rb3-xenon (on `ca61b767f`, a `scripts/setup_worktree.sh` worktree) | `tools/native_health.sh` | `NATIVE_HEALTH_RESULT verdict=PASS link=PASS link_verified=18 link_expected=18 link_skipped=0 runtime=PASS runtime_ran=18 runtime_total=18 gates_pass=77 gates_fail=0 unrunnable=none selftest=SKIPPED scatter_unlinked=16 scatter_dirb=0 scatter_multihost=17 rc=0 handpose_controls=- handpose_baseline_fail=- runtime_crashed=0 runtime_failed=none` (embedded link gate: `verdict=PASS expected=18 verified=18 skipped=0 partial=0 failed=0 rc=0`) |
| rb3-xenon | `tools/native_build_gate.sh` | run as the lane's last action; its `NATIVE_GATE_RESULT` line is in the lane report |
| rb3 (Wii), dc3 flavor | title (9.4) and venues (9.3) | renders the title and Quickplay to `game_screen`, exits cleanly |

DC3 and rb3-xenon use the DC3 shape, so for them the change compiles to the
no-op `FillMeshApproxLighting`, a never-set `retailLighting`, and a larger
`ObjectUniforms`.

## 10. Texture and display gamma, settled from retail (lane W16-QT, 2026-10-06)

9.5 left the textures between "raw" (too bright) and "decoded" (too dark).
This lane read what the Xbox 360 build does with a texel and with display
gamma, from rb3-xenon's matched code, the retail shaders, and a xenia capture
of the retail XEX. Engine branch `w16-qt`: `72f38c6` (shading and ramp),
`246264c` (ramp only on a presented frame), on `b18b088`.

### 10.1 What retail does

**Texels are not linearized, and nothing is decoded or encoded in a shader.**
- `DxRnd::D3DFormatForBitmap` (rb3-xenon `rnddx9/Rnd.cpp`, matched 100%
  against retail) returns `D3DFMT_DXT1` (`0x1a200152`), `DXT3`, `DXT5`, `DXN`,
  `A8R8G8B8` (`0x18280186`) or `A1R5G5B5`. The Xenos format word's four sign
  fields (bits 9–16), which select a gamma fetch, are 0 in every one of them.
  The fetch returns the stored value.
- The frame and render targets are `D3DFMT_A8R8G8B8`
  (`DxRnd::CreateEDRAMSurfaces`), sampled back as stored.
- Of 4,046 `standard` pixel shaders in the Xbox ark's `xbox_shaders`, 400 were
  run through `xenia-gpu-shader-compiler` and 61 disassembled. None contains a
  `log` or `exp` (the only way to compute a power). Each fetches the texel and
  multiplies it. The 27 `particles` pixel shaders (11 disassembled) are the
  same; the textured one is `tfetch2D r0, r0.xy, tf0` then `mul oC0, r0, r2`,
  i.e. texel × vertex colour.

So retail computes `t · c · L` on the stored values and writes the product,
saturated, to an 8-bit target.

**The display has a gamma ramp.** `DxRnd::InitRenderState` ends in
`DxRnd::SetupGamma` (`rnddx9/Rnd_Xbox.cpp`, matched 100%). When the system
config has `(rnd (gamma g))` it builds, for i = 0..255,
`entry = (u16)(pow(i / 256, g) * 1024) * 64` for red, green and blue, and calls
`D3DDevice_SetGammaRamp`. The Xbox config (`system/run/config/default.dta`)
has `(gamma 0.85)`. The scanned-out image is therefore about `x^0.85` of the
frame buffer, i.e. brighter than the frame.

**The Wii build has no ramp.** Its `default.dtb` carries the same
`(gamma 0.85)` (decrypted with the Rand2 LCG, seed = first int32), but no game
code reads it: in rb3 `src/`, `GXSetDispCopyGamma` and the VI gamma calls exist
only inside the RVL SDK, and `rndwii/Rnd.cpp`'s `CopyBuffer` only calls
`GXCopyDisp`. GX's TEV also multiplies stored values, so Wii shading is the
same `t · c · L`.

**Wii and Xbox texture data have the same tone.** 54 textures present in both
`.milo_wii` and `.milo_xbox` were decoded by the engine's own upload path (a
probe, not committed) and compared at the Wii size: Wii minus Xbox luma, mean
−0.56 and median +1.03 (8-bit); the median per-channel exponent fitted from
Xbox to Wii is 0.975. The Wii textures are downscaled, not regraded. (The
title and venue instruments here load the Xbox ark extract, so they render
Xbox textures either way.)

### 10.2 Which image the references are

A retail screenshot is the front buffer, before the ramp. This was measured,
not assumed: xenia (`xenia-headless`, Checked, Vulkan) ran clean TU5
(`fork-regress-content/rb3/tu5-clean-nodd/default.xex`) with

```
--protect_zero=false --rb3_tu5_app_run_direct=true --rb3_no_char_preview=true
--rb3dx_offline_join=true --rb3dx_skip_calibration=true --rb3dx_ui_probe=true
--local_user_count=2 --dump_frames_path=<dir> --headless_capture_interval=300
```

Its `frame_N_raw.ppm` is the guest front buffer with red and blue swapped
(swap them back). Ignore the non-raw `.ppm`, which xenia sRGB-encodes. Vanilla
TU5 shows a movie behind the title, so the rooftop city appears at
`main_hub_screen`, frame 1800: same city, same camera as the TCRF title shot.

| image | luma | sky_dE / city_dE vs TCRF |
|---|---|---|
| TCRF title (360) | 50.1 | — |
| xenia, front buffer | 53.3 | **16.2 / 17.7** |
| xenia, front buffer through the 0.85 ramp | 64.4 | 21.4 / 21.3 |

Luma by region (640×360, 8-bit):

| region | TCRF | xenia | xenia + ramp | this lane | forced ramp | base `b18b088` |
|---|---|---|---|---|---|---|
| sky, right | 32.8 | 32.5 | 42.3 | 35.9 | 47.0 | 50.2 |
| city, right | 37.5 | 38.3 | 48.9 | 27.0 | 35.7 | 51.5 |
| city, middle | 84.9 | 62.6 | 74.7 | 44.0 | 53.5 | 80.4 |
| left roof | 27.3 | 35.5 | 46.2 | 20.6 | 28.6 | 36.2 |

TCRF agrees with xenia's front buffer, not with the ramped image, so TCRF is
pre-ramp. The other retail stills are of unknown provenance (YouTube video and
fandom wiki).

### 10.3 What changed

- **Shading** (`72f38c6`). Under `kGammaSpaceShading`, `standard_wgsl` no
  longer decodes the prelit vertex colour, the material colour or the lit
  term, and does not encode its output. It computes `t · c · L` and clamps to
  [0, 1] (retail's saturating 8-bit write) instead of the tanh highlight
  shoulder. `Part_Wgpu` drops its colour decode (`tex * in.color`, as
  retail's particle shader). `srgbToLinear` is gone from the standard shader.
  Render targets are unaffected in practice: no RB3 draw encodes, so frame and
  target hold the same values, as on retail.
- **Why 9.5's decode was too dark.** It decoded the texel, the colour and the
  lit term, and encoded the result, which is `t · c · L` for an ordinary
  texture. But draws into a render target skip the encode
  (`kRenderTargetStoresLinear`), so they stored the linear product, and a
  texel read back from such a target (the title's `clouds_rnd.tex`, outfit
  composites) was decoded a second time. It also kept the tanh shoulder.
- **Display ramp** (`72f38c6`, `246264c`). `gfx/DisplayRamp` evaluates
  `SetupGamma`'s table per pixel (round to 8 bits, table entry, /65535) over
  the finished frame, UI included, after the post chain and before the ImGui
  overlay. `rndshape::DisplayGamma(presenting)` (`RB3WiiPostChain.cpp`) reads
  `(rnd (gamma))` from the system config, so it is 0.85 for RB3 data. It
  applies only to a frame presented to a window surface or the web canvas. A
  headless frame is the front buffer, which is what the references above are.
  `MILO_RB3_DISPLAY_GAMMA=<g>` forces the ramp for every output, headless
  included; `off` disables it. `GpuDevice::ConfigureSurface` adds `CopySrc` to
  the surface usage when the surface allows it (the pass copies the frame to a
  scratch texture); without it the pass warns once and is skipped.
- **DC3 shape**: `DisplayGamma` is an inline 0, so DC3 and rb3-xenon are
  unchanged.
- **Tool**: `tools/rb3-dc3-parity/screen_capture.py` takes screenshots on any
  UI screen (default `main_hub_screen`, navigation from `RB3_SCREEN_NAV`), for
  comparison with a xenia capture.

### 10.4 Title screen

sky_dE / city_dE against TCRF, lower is better. "This lane" is the default
headless output (no ramp). The ramp row is `MILO_RB3_DISPLAY_GAMMA=0.85`.
Both are the final `246264c` binary; the earlier always-on-ramp build read
the same within 0.2.

| frame | base `b18b088` | **this lane** | forced ramp |
|---|---|---|---|
| 60 | 29.8 / 19.1 | **14.9 / 18.8** | 17.3 / 15.4 |
| 200 | 33.0 / 21.2 | **15.6 / 19.2** | 16.7 / 17.1 |
| 400 | 24.8 / 18.9 | **13.6 / 19.2** | 15.0 / 15.6 |

| f400 | luma | sky luma | city luma | p10 | dark % |
|---|---|---|---|---|---|
| retail (TCRF) | 50.1 | 44.8 | 52.4 | 11.8 | 25.7 |
| base `b18b088` | 62.1 | 64.9 | 60.9 | 24.7 | 3.6 |
| **this lane** | 35.4 | 39.8 | 33.6 | 10.9 | 35.0 |
| forced ramp | 44.9 | 50.9 | 42.4 | 16.9 | 17.7 |

Prediction before the first run: f400 luma 45–55 and sky luma 40–50 for the
ramped build. Measured 45.1 and 51.2; sky luma was 1 over.

- **Sky_dE drops by about half at every frame** (24.8–33.0 → 13.6–15.6), and
  sky luma is now within 5 of retail instead of 20 over. p10 moves from 24.7 to
  10.9 against retail's 11.8.
- **City_dE is unchanged** (mean over the three frames 19.7 → 19.1). City luma,
  though, falls from 8.5 over retail to 18.8 under it, and dark % overshoots
  (35.0 against 25.7).
- The city deficit is lighting, not gamma. The region table shows the sky
  matching xenia's front buffer while the city is about 30% dark in the middle
  and right. The city buildings are lit materials (`prelit` 0 in the per-draw
  report), so their brightness is the lit term, which on retail includes the
  Xbox material features the Wii loader drops (9.3). The old encode hid this
  by brightening every factor.

### 10.5 Venues

Same instrument as 9.3 (`venue_capture.py`, `RB3_FIXED_CLOCK=1`, 8 shots per
run). Camera cuts differ between runs even with `RB3_LOAD_DETERMINISM=1`, so
runs are pooled: 8 runs per build, mean ± standard error over runs.

| config | runs | luma | p10 | dark % |
|---|---|---|---|---|
| retail, 5 gameplay stills (3 Wii YouTube, 2 360 fandom) | — | 54.0 | 9.0 | 34.4 |
| retail, Wii only | — | 47.2 | 2.7 | 50.2 |
| retail, 360 only | — | 64.2 | 18.6 | 10.7 |
| base `b18b088` | 8 | 46.0 ± 1.2 | 13.1 ± 1.3 | 34.2 ± 2.1 |
| **this lane** | 8 | **52.8 ± 3.8** | 16.3 ± 1.2 | 32.8 ± 2.9 |
| forced ramp | 3 | 76.1 ± 7.1 | 22.7 ± 1.7 | 15.6 ± 1.7 |

(The retail row is these five stills through `frame_stats.py`; 9.3's 57.7 /
9.4 / 30.5 was a different five.)

- **Mean luma moves toward retail**: |Δ| 8.0 → 1.2. A bootstrap over runs gives
  the change a 95% interval of [−0.6, +14.1], so it is likely but not proven.
- Dark % is within noise (interval [−8.1, +5.1]); |Δ| 0.2 → 1.6.
- p10 rises by 3.2 (interval [−0.1, +6.2]), away from the five-still mean.
  The retail stills disagree with each other by platform there (2.7 against
  18.6), so p10 does not separate the builds.
- The ramp overshoots the venues by about 22 luma against the five stills, as
  it overshoots the title against TCRF. That is consistent with the references
  being front-buffer images.

### 10.6 Consumer verification (engine `w16-qt`)

Fresh `~/tmp` worktrees, configured with `-DMILO_ENGINE_PATH` at the `w16-qt`
engine worktree (confirmed in each `CMakeCache.txt`), built after `246264c`.

| consumer | instrument | result |
|---|---|---|
| dc3-decomp (on `e992ee9b5`) | `scripts/native_configure.sh` + `scripts/native_test.sh` | 626 registered, 557 passed, 0 failed, 69 skipped (budget 69), rc=0 |
| rb3-xenon (on `442f01984`, a `scripts/setup_worktree.sh` worktree) | `tools/native_health.sh` | `NATIVE_HEALTH_RESULT verdict=PASS link=PASS link_verified=18 link_expected=18 link_skipped=0 runtime=PASS runtime_ran=18 runtime_total=18 gates_pass=77 gates_fail=0 unrunnable=none selftest=SKIPPED scatter_unlinked=16 scatter_dirb=0 scatter_multihost=17 rc=0 handpose_controls=- handpose_baseline_fail=- runtime_crashed=0 runtime_failed=none` (embedded link gate: `verdict=PASS expected=18 verified=18 skipped=0 partial=0 failed=0 rc=0`) |
| rb3-xenon | `tools/native_build_gate.sh` | run as the lane's last action; its `NATIVE_GATE_RESULT` line is in the lane report |
| rb3 (Wii), dc3 flavor | title (10.4) and venues (10.5) | title exits rc=0; Quickplay reaches `game_screen` in every run |

Two environment traps hit on the way, neither an engine problem:
- The first `native_health.sh` run read `FAIL 16/18`, with `rb3-frame` and
  `rb3-render` STALE. `246264c` was committed while that build ran, so ninja
  saw newer engine sources afterwards. The rerun above passes.
- A plain `git worktree add` of dc3-decomp skips 72 tests against a budget
  of 69. Its `scripts/setup_worktree.sh` symlinks `orig-assets/`, `archive/`,
  `native/third_party/` and `native/models/`; with those links the count is 69.

### 10.7 Not done

- **City and venue lighting.** The remaining title gap is the lit term
  (10.4), i.e. the Xbox-only material features listed in 9.6.
- **A retail venue reference.** Xenia was not driven into gameplay, so the
  venue comparison rests on five stills of unknown capture path. A xenia
  gameplay capture of the same song would allow a paired comparison.
- **The ramp on a real display** was not checked by eye; it is exercised
  headless only through `MILO_RB3_DISPLAY_GAMMA`, and on the web build not at
  all.
- No merge, pin bump or push.

## 11. Lit city: the emissive map, untinted (lane W16-QY, 2026-10-06)

Section 10.4 left the title city about 30% darker than xenia's front buffer
while the sky matched, and attributed the gap to the Xbox-only material
features the Wii loader drops (9.6). The city's gap came from somewhere else:
the dc3 shader tinted the emissive ("illum") map by the diffuse base, and
retail does not.

### 11.1 Evidence

**What the city materials carry.** An env-gated probe in the rb3 `Mat` loader
(investigation only, not committed) recorded the Xbox-only fields of every
material loaded during a title run: 849 distinct materials. 360 have
`per_pixel_lit`, 153 a non-zero specular colour, 104 a specular map, 91 a
normal map, 19 a rim colour, 0 an environment map, and 64 an emissive map.
The city's lit buildings are in that last group: `building_02`/`_03` (emissive
multiplier 1.5), `building_04`, `_05`, `building_misc_01`/`_02` (1.0),
`theatre_01` (1.25), `skyline_buildings02` (0.4), all `prelit` 0 with white
material colour. The Wii `Mat` already loads the emissive map and
`mEmissiveMultiplier`, and the engine already passes both to the shader, so
the lights in the windows were drawn, just at the wrong strength.

**What retail does with it.** Retail `standard.ps` permutations were taken
from the shipped shader archive (QT's `xobx.py` dumps, disassembled with
xenia's shader compiler). Nine of the dumped permutations that have the glow
bit (option bit 7) disassemble. All nine scale the emissive texel by `c5.x`
alone and add it as a separate term, either directly
(`standard_0000000000030080`: `mad oC0.xyz, r0.xyz, c5.xxxx, r3.xyz`) or
through a register (`standard_0000000000020081`: `mul r0.xyz, r0, c5.xxxx`
... `mad oC0.xyz, c1.xyz, c0.xyz, r0.xyz`). `c5` is written by
`NgMat::SetRegularShaderConst` (rb3-xenon, 100% matched) as
`(mEmissiveMultiplier, intensify + 1, 0, 0)`. Nothing multiplies the
emissive term by the texel or the material colour.

**What the engine did.** `standard_wgsl.inc` multiplied the emissive sample by
`mix(1, baseColor.rgb, smoothstep(0, 0.04, max(baseColor)))`, i.e. by the
diffuse texel times the material colour wherever the base is not black. On a
building facade (dark texel, bright window in the illum map) that cut the
window light to the facade's brightness.

**What else was checked and is not the cause.**
- The vertex light model. Retail `standard.vs` for a non-prelit, non-ppl
  draw computes `c0 · (c1 + box(N) + Σ points)`, with the box as six linear
  `max(0, ±N)` faces (`c80`–`c85`) and each point light as
  `c67.rgb · sat(d · c64.w + c67.w) · sat(N·L)`. The pixel shader then
  multiplies by the texel. That is what the engine's retail light model
  (section 9) already computes.
- Vertex colour. That permutation fetches no vertex colour, and the engine
  already ignores it for non-prelit draws under the retail light model.
- The env lights. The title's envs (`sky`, `buildings_dim`, `cityscape`,
  `street`, …) have near-black ambient, so the facades are almost entirely
  emissive plus point lights.

### 11.2 Change

`a8bde0b`, `src/gfx/standard_wgsl.inc`: when `material.gammaShading` is set
(RB3 content only, see 10.3) the emissive tint is 1, so the shader adds
`emissiveMultiplier · emissive.rgb` as retail does. The DC3 shape never sets
`gammaShading`, so DC3 and rb3-xenon are unchanged.

Before the change I expected the city luma ratio against xenia to rise from
about 0.70 to between 0.9 and 1.1, with the sky unchanged.

### 11.3 Title, before and after

`title_capture.sh` + `title_fidelity.py` against TCRF; "base" is engine
`1e47d3e`, "this lane" adds `a8bde0b`. Same rb3 worktree for both.

| frame | base sky_dE / city_dE | **this lane** |
|---|---|---|
| 60 | 14.8 / 18.9 | **14.7 / 12.0** |
| 200 | 15.5 / 19.2 | **15.3 / 13.7** |
| 400 | 13.6 / 19.2 | **13.4 / 12.2** |

(Xenia's own front buffer reads 16.2 / 17.7 against TCRF, 10.2: the hub and
the title differ in which rooftop lights are on.)

| f400 | luma | sky luma | city luma | p10 | dark % | city_edge |
|---|---|---|---|---|---|---|
| retail (TCRF) | 50.1 | 44.8 | 52.4 | 11.8 | 25.7 | — |
| base | 35.4 | 39.6 | 33.6 | 10.8 | 35.4 | 0.715 |
| **this lane** | 42.6 | 40.5 | 43.5 | 14.2 | 25.5 | 0.780 |

Luma by region against xenia's front buffer (`main_hub_screen` frame 1800,
same camera; regions as in 10.2), f400:

| region | xenia | base | **this lane** |
|---|---|---|---|
| sky, left | 33.6 | 22.5 | 22.7 |
| sky, right | 29.9 | 41.7 | 42.9 |
| city, right | 41.0 | 31.7 | **43.4** |
| city, middle | 51.9 | 22.7 | **42.8** |
| city, right-middle | 39.0 | 34.7 | **43.7** |
| left roof | 29.1 | 14.3 | 15.2 |
| city mean \|ΔRGB\| vs xenia | 0 | 20.9 | **14.9** |
| city luma / xenia | 1.000 | 0.701 | **1.002** |

- **The city moves to xenia's brightness**: luma ratio 0.701 → 1.002, inside
  the predicted band, and city error against xenia falls 20.9 → 14.9.
  Against TCRF, city_dE falls by about a third at every frame (mean over the
  three frames 19.1 → 12.6), below xenia's own 17.7.
- **The sky does not move**: sky_dE changes by at most 0.2 and the sky regions
  by at most 1.2 luma (the right sky region overlaps lit skyline windows).
- Dark % goes from 35.4 to 25.5 against TCRF's 25.7.
- Remaining differences seen by eye against xenia: the left roof is still
  about half xenia's luma (its envs have near-black ambient, see 11.5), the
  moon is dimmer and partly hidden, the right facade is slightly bluer, and
  the billboards show different frames.

### 11.4 Venues

Same instrument as 10.5 (`venue_capture.py`, `RB3_FIXED_CLOCK=1`, 8 shots at
game frames 60–1020, 8 runs per build), both builds run in this lane.

| config | runs | luma | p10 | dark % |
|---|---|---|---|---|
| retail, 5 gameplay stills (10.5) | — | 54.0 | 9.0 | 34.4 |
| base `1e47d3e` | 8 | 60.7 ± 5.0 | 17.3 ± 1.7 | 28.5 ± 3.5 |
| **this lane** | 8 | 52.4 ± 2.7 | 17.3 ± 1.7 | 29.4 ± 3.1 |

Bootstrap 95% intervals on this lane minus base: luma −8.4 [−19.1, +2.0],
p10 +0.0 [−4.2, +4.6], dark % +0.9 [−7.9, +9.3]. None separates from zero.
The base row reads 7.9 luma above 10.5's row for the same engine, which shows
the size of the camera-cut variance.

The luma drop cannot be the change. The tint was `mix(1, base, s)` with the
base at most 1, so removing it can only add light, and only on emissive
materials. A probe run of the venue path found 17 materials with an emissive
map beyond those the title loads (`city_sky`, `glass_02`, LED and flare
cards, the taxi dash and meter, `amp_fnr_bassman_head`). Pairing each venue
shot with the closest-looking base shot (320×180) gives the shot-level
effect: the 9 pairs within mean |ΔRGB| 10 are all as
bright or brighter in this lane, by 0.0 to 2.8 luma. Widening to 11 adds
three looser matches, one of them 1.3 darker.

### 11.5 Identified, not implemented

> Lane W16-RA implemented these terms in section 12. The AO input
> statement below is corrected in 12.2: the input is the vertex colour, which
> the engine had been reading with R and B swapped.

Retail terms the RB3 path still lacks, from the same sources. None of them is
the city's gap, and each needs an input this lane could not establish.

- **Ambient occlusion** (option bit 38, `standard.vs`). Retail scales
  ambient plus box light by
  `ao = sat((a · 1.128379 − 1) · c24.x + 1)`, `a` being one extra scalar
  vertex fetch and `c24.x` the env's `AOStrength`. It is selected when the
  draw is not prelit, the mesh has `HasAOCalc`, the env has AO enabled and
  `AOStrength > 0.003`. 760 of 819 title draws have `HasAOCalc`. Which vertex
  element `a` comes from is not known: the vertex declaration fetch is by
  stream offset, and the title meshes' vertex colours are not AO-like (mean
  RGBA about (0.5, 0.5, 0.8, 0.5) on `building_*`, `prison_03`,
  `theater01B`), so using the colour would be a guess. The effect can only
  darken ambient and box light, which are near black in the city envs.
- **Env colour adjustment** (option bit 21, `UseColorAdjust`, pixel
  constants `c109`–`c111`). Not measured on these scenes.
- **Specular, normal, specular-map and rim** from 9.6. The title loads 153
  materials with specular colour and 91 with normal maps; their absence is
  not visible at title distance against xenia, and they remain out of reach of
  the Wii loader.
- **Retail spot lights in the box map** have no 0.28 colour-sum skip (Wii's
  `BoxMap` has one). Not changed.

### 11.6 Consumer verification (engine `w16-qy`)

Fresh `~/tmp` worktrees made with each repo's `scripts/setup_worktree.sh`,
configured with `-DMILO_ENGINE_PATH` at the `w16-qy` engine worktree
(confirmed in each `CMakeCache.txt`), built after `b7f8f67`.

| consumer | instrument | result |
|---|---|---|
| dc3-decomp (on `e992ee9b5`) | `scripts/native_configure.sh` + `scripts/native_test.sh` | 626 registered, 557 passed, 0 failed, 69 skipped (budget 69), rc=0 |
| rb3-xenon (on `d5d873c95`) | `tools/native_health.sh` | `NATIVE_HEALTH_RESULT verdict=PASS link=PASS link_verified=18 link_expected=18 link_skipped=0 runtime=PASS runtime_ran=18 runtime_total=18 gates_pass=77 gates_fail=0 unrunnable=none selftest=SKIPPED scatter_unlinked=16 scatter_dirb=0 scatter_multihost=17 rc=0 handpose_controls=- handpose_baseline_fail=- runtime_crashed=0 runtime_failed=none` (embedded link gate: `verdict=PASS expected=18 verified=18 skipped=0 partial=0 failed=0 rc=0`) |
| rb3-xenon | `tools/native_build_gate.sh` | run as the lane's last action; its `NATIVE_GATE_RESULT` line is in the lane report |
| rb3 (Wii), dc3 flavor | title (11.3) and venues (11.4), plus a clean rebuild at `b7f8f67` | title exits rc=0 (f400 city_dE 12.2, as 11.3); Quickplay reaches `game_screen` in all 10 runs with this engine (and in the 8 base runs) |

### 11.7 Not done

- The AO and colour-adjust terms (11.5).
- The left roof and the moon (11.3).
- A paired venue comparison: camera cuts still differ between runs, so 11.4
  rests on pooled runs plus matched shots.
- No merge, pin bump or push.

## 12. Retail material terms: specular, normal map, rim, AO, colour adjust (lane W16-RA, 2026-10-06)

Engine `w16-ra` `2b578f6` (off `3cb54e5`) and rb3 `w16-ra` `0373e6aaa` (off
`4039f5528`). This lane implements the terms 11.5 lists as identified but not
implemented. The two that change nothing in the measured content are in 12.7,
with the reasons.

### 12.1 Sources

| what | where (rb3-xenon, matched) |
|---|---|
| which terms a draw gets | `rndobj/Shader.cpp` `CalcShaderOpts` |
| per-frame constants (AO strength `c24`, colour adjust `c109`–`c111`) | `rndobj/Env_NG.cpp` `NgEnviron::Select` |
| material inputs | `rndobj/Mat.cpp` `RndMat::Load` |
| AO vertex data | `rndobj/AmbientOcclusion.cpp` `CalculateAOAtPoint`, `BuildSHCoeff` |
| vertex colour packing | `rnddx9/Mesh.cpp` `FillCompressedVertex` (dc3-decomp's is the same) |
| box map | `rndobj/BoxMap.cpp` |
| shader math | the retail `xbox_shaders` cache (`orig-assets/extracted-xbox-full/(.)/xbox_shaders`), standard permutations disassembled with `xenia-gpu-shader-compiler` |

Selection (`CalcShaderOpts`; option bits as in the shader key):

| term | bit | selected when |
|---|---|---|
| per-pixel (ppl) | 0 | `AllowPerPixel` (default on) and the material's per-pixel flag |
| specular | 2 | `SpecularRGB.Pack() != 0` (packed rgb bytes; alpha ignored) |
| specular map | 1 | ppl and specular and a specular map |
| normal map | 5 | ppl and a normal map |
| rim | 37 | ppl and `RimRGB.Pack() != 0` |
| rim map / rim under | 15 / 14 | rim and the map / the flag |
| colour adjust | 21 | the environ's `UseColorAdjust()` |
| ambient occlusion | 38 | not prelit, mesh `HasAOCalc`, env AO enabled, `AOStrength > 0.003` (does not test use-environ) |

All lighting terms also need real or approx lights. The approx light count is
the queued local count plus the global count.

The permutations disassembled for the math: `0x14000030010` (AO),
`0x2000028037` (ppl, spec, spec map, normal map), `0x4000030035` (rim),
`0x2000024005` (rim under), `0x220010` (colour adjust).

The Wii loader reads every material input into locals and drops it. Under
`HX_NATIVE` only, rb3's `RndMat` now keeps them (`RB3_NATIVE_XBOX_MAT_FIELDS`),
with retail's revision fix-ups: before rev 0x25 a specular map forces white
specular, rim power is ×2.857143 (min 1) up to rev 0x39, and rim is cleared
before rev 0x3B.

### 12.2 Finding: the AO input is the vertex colour, and the engine read it with R and B swapped

11.5 rejected the vertex colour as the AO input because its mean was about
(0.5, 0.5, 0.8, 0.5). That colour is the AO data. `BuildSHCoeff` stores four
SH coefficients:

- R = band 0, at most about 0.886 (open sky). 1.128379 × 0.886 = 1, so the
  shader's ambient factor is 1 there.
- G, B, A = the y, z and x band-1 coefficients, remapped from [−1, 1] to
  [0, 1].

The mean only looked wrong because of the swap below.

Retail `FillCompressedVertex` packs a D3DCOLOR (A, R, G, B from the high byte
down). The engine's `UnpackColor_BE` read R from the low byte, so it swapped R
and B. A probe over the title's 162 compressed AO meshes showed both effects:

| | before the fix | after the fix |
|---|---|---|
| where the band-0 bound (0.898) showed up | blue channel's max | red channel's max (0.902) |
| what red tracked | the normal's z (sidewalk nz −0.5 → r 0.28; silos nz 0.38 → r 0.70) | — |

A z band coefficient behaves like the normal's z, as the "before" red did.
The fix is switched by `rndshape::kCompressedColorIsArgb`, which is on for the
RB3 shape. With the old read, the title gets much worse (`noSWAP`, 12.4).
DC3 packs the same way, so the DC3 shape probably has the same swap; that is
left alone here (12.7).

> **Superseded by section 13.1 (lane W16-RE).** DC3's retail packer is ARGB
> too; the switch is gone and both shapes read ARGB.

### 12.3 What changed

- **Seam** (`RndShape*.h`): `MatRetailTerms(mat, RetailMatTerms&)` and
  `FillMeshApproxLighting(mesh, box, retail)`. Both are no-ops for the DC3
  shape, and DC3 materials keep the dc3 model's fields.
- **Uniforms**:
  - scene: `aoStrength`, `colorAdjust`, `colorXfm[3]`
  - material: `retailSpec` (rgb, power), `retailRim` (rgb, power),
    `retailFlags` (ppl, normal map, spec map, rim map)
  - object: `retail` (`HasAOCalc`, approx light count)
  - The object group is now visible to the fragment stage.
- **Material** (`MaterialSetup.cpp` `FillRetailTerms`): fills the uniforms
  and binds the normal, specular and rim maps in the slots the dc3 model uses.
  The dc3 model's own fields stay zero, so its terms never run on top.
- **Environ** (`RB3WiiSceneLighting.cpp`): AO strength (when enabled and above
  0.003), colour adjust and its matrix (`colorXfm[c] = (m.x[c], m.y[c],
  m.z[c], v[c])`). `SetPConstant4x3` writes the matrix's columns, so
  `out.c = Σ rgb_i · m.row_i.c + v.c`.
- **Box map**: `ApplyRetailBoxMap` follows retail `BoxMap.cpp`, with no 0.28
  spot skip.
- **Shader** (`standard_wgsl.inc` `retailShade`), as retail computes it:
  - AO, from the SH vertex colour `(R, G, B, A)` and strength `S`:
    - `ambAO = sat((1.128379 R − 1) S + 1)` scales ambient plus box light.
    - Per point light, `ptAO = sat((v − 1) S + 1)`, where
      `v = (0.282 R + 0.4886 · dot((2A−1, 2G−1, 2B−1), Lloc)) / (3/16 + 9/16 N·Lloc)`.
      `Lloc` is the object-space light direction, and `v = 1` when the
      denominator is negative.
    - Non-ppl alpha is `ambAO` times the material alpha.
  - Specular:
    - `norm = p/2π + 1/π`
    - `F = 0.25 + (1 − sat(N·V))(0.5 Nz + 0.5)`
    - `R = 2(N·V)N − V`
    - box: `Σ pow(sat(±R), p) · face · norm · F · ambAO`
    - point: `norm · ptAO · colour · atten · pow(sat(R·L), p)`, with no
      Lambert term
    - A specular map multiplies the colour by its rgb and sets the power to
      `max(p · a, 0.5)`.
  - Normal map:
    - `z = sat(1 − x² − y²)` (no square root)
    - `N = normalize(z N + k x B + k y T)`, with `k = 1 − deNormal` and
      `B = cross(N, T) · w`. Red runs along B.
  - Rim:
    - `rim = pow((1 − sat(N·V))(0.5 Nz + 0.5), rp) · rimColour · box(−V)`
    - Diffuse is scaled by `1 + 0.3 rim`, then `0.7 rim` is added.
    - A rim map multiplies the colour and sets the power to `max(a · rp, 0.5)`.
  - Colour adjust: applied after fog, before the clamp.

### 12.4 Title, before and after

`title_capture.sh` + `title_fidelity.py` against TCRF. "base" is engine
`3cb54e5`, "this lane" is `2b578f6`. Both builds use the rb3 `w16-ra`
worktree; its `RndMat` additions only keep values the loader already read, and
the base engine never reads them. The fields that probe in the title:

- **Materials:** 113 drawn materials have per-pixel or specular, and all of
  them are per-pixel: 78 specular, 68 normal map, 63 specular map, 26 rim,
  0 rim map, deNormal 0 throughout.
- **Envs:** all have AO enabled at strength 1.0 (1.5 for `subwayhangout_geom`
  and one `geom.env`). No title env uses colour adjust.

| frame | base sky_dE / city_dE / city_edge | **this lane** |
|---|---|---|
| 60 | 14.8 / 12.0 / 0.780 | **13.4 / 11.7 / 0.910** |
| 200 | 15.4 / 13.7 / 0.719 | **14.7 / 13.1 / 0.870** |
| 400 | 13.5 / 12.3 / 0.779 | **11.6 / 11.7 / 0.912** |

| f400 | luma | p10 | dark % |
|---|---|---|---|
| retail (TCRF) | 50.1 | 11.8 | 25.7 |
| base | 42.4 | 14.1 | 25.7 |
| **this lane** | 43.7 | 13.0 | 27.7 |

Against xenia's front buffer (regions as in 10.2), f400:

| region | xenia | base | **this lane** |
|---|---|---|---|
| sky, left | 33.6 | 22.8 | 22.6 |
| sky, right | 29.9 | 43.2 | 42.4 |
| city, right | 41.0 | 43.4 | 42.3 |
| city, middle | 51.9 | 42.8 | 40.4 |
| city, right-middle | 39.0 | 43.2 | 41.2 |
| left roof | 29.1 | 15.1 | 13.4 |
| city mean \|ΔRGB\| vs xenia | 0 | 14.7 | **14.0** |
| city luma / xenia | 1.000 | 0.998 | 0.956 |

Ablations were measured on a build with the same terms plus one switch per
term, each switch run on its own. The full-terms leg of that build (0.4 or
less from this lane on every TCRF figure) is the "none" row.

| f400, switched off | sky_dE | city_dE | city_edge | luma | dark % | city luma / xenia |
|---|---|---|---|---|---|---|
| none (all terms) | 11.6 | 11.7 | 0.912 | 43.9 | — | 0.942 |
| vertex colour fix | 17.1 | 15.7 | 0.864 | 36.4 | 38.8 | 0.848 |
| AO | 11.6 | 11.9 | 0.907 | 46.3 | 24.1 | 1.048 |
| material terms | 13.7 | 13.1 | 0.821 | 40.5 | 31.3 | 0.947 |
| colour adjust | 11.5 | 11.7 | 0.913 | 43.9 | 27.6 | 0.973 |
| spot skip removal | 11.5 | 11.7 | 0.912 | 43.7 | 27.9 | 0.936 |

- **Against TCRF the title improves at every frame on every figure**: sky_dE
  by 0.7 to 1.9, city_dE by 0.3 to 0.6, city_edge by 0.13 to 0.15. Most of the
  edge gain comes from the material terms (normal maps and specular give the
  facades structure). Their removal also costs 2.1 sky_dE, because the
  skyline's specular facades sit inside the sky band.
- **The vertex colour fix is worth more than any term, but only with the
  terms in.** With the terms and the old read, the title is far worse than
  base (city_dE 15.7). The read only feeds AO here, so the swap was harmless
  until AO used it.
- **AO darkens the city**: the luma ratio against xenia goes from 1.048
  without AO to 0.94–0.96 with it. City error against xenia still improves
  (14.7 → 14.0), but by luma alone the city moves from xenia's level to about
  5% below it. The left roof and the middle city get darker, away from xenia.
  Their envs have near-black ambient (11.5), so what is left there is AO
  scaling the box light.
  *Corrected in 13.2:* most draws there have no box light at all; the AO
  darkening there is split between the point lights and the box, as retail
  does it, and the left roof matches the hardware capture.
- Colour adjust and the spot change are inert on the title, as predicted (no
  title env uses colour adjust, and no spots reached a box map in the venue probe, 12.7). The right-middle region
  moves between 39.0 and 43.2 across those runs because a billboard animates
  in it, which gives the run-to-run noise.

### 12.5 Venues

Same instrument as 10.5 and 11.4 (`venue_capture.py`, `RB3_FIXED_CLOCK=1`,
8 shots at game frames 60–1020, 8 runs per build). Both builds were run in
this lane. One venue env, `streaks_red.env`, uses colour adjust.

| config | runs | luma | p10 | dark % |
|---|---|---|---|---|
| retail, 5 gameplay stills (10.5) | — | 54.0 | 9.0 | 34.4 |
| base `3cb54e5` | 8 | 58.7 ± 2.1 | 19.3 ± 1.0 | 28.8 ± 1.9 |
| **this lane** | 8 | 51.7 ± 4.4 | 16.2 ± 2.4 | 33.7 ± 4.5 |

Bootstrap 95% intervals on this lane minus base:

- luma −7.0 [−16.2, +1.3]
- p10 −3.1 [−8.1, +1.5]
- dark % +5.0 [−3.5, +14.2]

All three move toward retail, and none separates from zero. Matched shots
(the closest-looking base shot at 320×180):

- Within mean |ΔRGB| 10 there are 3 pairs, at −0.9, −0.2 and −3.1 luma
  (mean −1.4).
- Widening to 14 gives 11 pairs, with mean +1.6.

The shot-level effect is therefore small in both directions. The pooled drop
is mostly in shots where AO darkens the crowd and stage set. By eye,
characters gain specular highlights (arms, instruments) and the crowd reads
darker under AO. Quickplay reached `game_screen` in all 16 runs.

### 12.6 Consumer verification (engine `w16-ra`)

Fresh `~/tmp` worktrees made with each repo's `scripts/setup_worktree.sh`,
configured with `-DMILO_ENGINE_PATH` at the `w16-ra` engine worktree
(confirmed in each `CMakeCache.txt`), built at `2b578f6`.

| consumer | instrument | result |
|---|---|---|
| dc3-decomp (on `e992ee9b5`) | `scripts/native_configure.sh` + `scripts/native_test.sh` | 626 registered, 557 executed, 557 passed, 0 failed, 69 skipped (budget 69), rc=0 |
| rb3-xenon (on `b5a56416b`) | `tools/native_health.sh` | `NATIVE_HEALTH_RESULT verdict=PASS link=PASS link_verified=18 link_expected=18 link_skipped=0 runtime=PASS runtime_ran=18 runtime_total=18 gates_pass=77 gates_fail=0 unrunnable=none selftest=SKIPPED scatter_unlinked=16 scatter_dirb=0 scatter_multihost=17 rc=0 handpose_controls=- handpose_baseline_fail=- runtime_crashed=0 runtime_failed=none` (embedded link gate: `verdict=PASS expected=18 verified=18 skipped=0 partial=0 failed=0 rc=0`) |
| rb3-xenon | `tools/native_build_gate.sh` | run as the lane's last action; its `NATIVE_GATE_RESULT` line is in the lane report |
| rb3 (Wii), dc3 flavor | title (12.4) and venues (12.5) | title exits rc=0 on the final build (f400 11.6 / 11.7 / 0.912); Quickplay reaches `game_screen` in all 16 venue runs |

DC3 and rb3-xenon use the DC3 shape. For them, `MatRetailTerms` returns false,
`FillMeshApproxLighting` does nothing, and the colour read is unchanged, so
the retail uniforms stay zero and the shader takes its existing branches.
(13.1 later changes the colour read for the DC3 shape.)

### 12.7 Not done

- **Rim under** (bit 14): no change. `0x2000024005` computes the same rim as
  the plain rim permutation, so there is nothing separate to implement.
- **Spot skip**: the 0.28 colour-sum skip is removed, but this changes nothing
  in the measured content. `BoxMapLighting::QueueLight` queues fake spots as
  directionals and has no case for floor spots or shadow refs. Spot entries
  reach a box map only through `SpotlightDrawer::ApplyLightingApprox`, which
  `SpotlightDrawer::UpdateBoxMap` calls from `EndWorld` into `sGlobalLighting`
  (retail and rb3 alike). A probe on the venue path counted the spots in
  `sGlobalLighting` at each approx-lit draw whose env uses global approx
  lighting: 0 over 340k draws. Why they never arrive (no spotlights in
  `sLights`, zero `mLightingInfluence`, `kProcessChar` not set, or no env
  using global approx) was not investigated. Until it is, the skip cannot be
  measured either way.
- **Tangent frame**: the normal map uses the vertex tangent as it comes and
  does not rotate it by the material's texgen transform. This only matters
  for normal-mapped materials with a rotated texgen.
- **Normal detail**: dc3's `normDetail` map is not combined with the retail
  normal map.
- **The DC3 shape's colour read** (*done in 13.1*): `kCompressedColorIsArgb` is off for DC3,
  although DC3's `FillCompressedVertex` packs ARGB too. Changing it would
  change DC3 renders, so it belongs to a DC3 lane with DC3 references.
- **Uncompressed Wii `Color32` on little-endian hosts**: the fields come out
  reversed (`fr()` reads the alpha byte). Correcting the word decode in
  `VertColor` would break the code that writes the fields directly (NoteTube
  `SetAlpha`, `OutfitConfig`, `ChordShapeGenerator`), so it is unchanged. The
  title's AO meshes are all compressed, so AO does not depend on it.
- **Inverse square root**: retail's box map uses the raw `frsqrte` estimate;
  this uses the exact value, as rb3-xenon's `HX_NATIVE` `BoxMap.cpp` does.
- **Open finding** (*answered in 13.2*): the city sits about 5% below xenia's luma with AO on
  (12.4). The left roof and middle city are darker than xenia and were not
  investigated.
- No merge, pin bump or push.

## 13. DC3's vertex colour read, and AO on the title city (lane W16-RE, 2026-10-06)

Engine `w16-re`, off `f0ecb01`. This lane takes the two open findings of 12.7:

- Does DC3 have the vertex colour swap that 12.2 fixed for RB3?
- Should AO scale the box light in the parts of the title city that read darker than xenia?

### 13.1 DC3 packs vertex colour as ARGB; both shapes now read it that way

**Retail code.** DC3's `FillCompressedVertex` is at `0x826204D8` (dc3-decomp
`build/373307D9/asm/system/rnddx9/Mesh.s`). It converts the four colour floats
of `RndMesh::Vert` (red at `0x30`, green `0x34`, blue `0x38`, alpha `0x3C`)
to bytes and joins them with three `rlwimi`:

```
rlwimi r28, r29, 8, 0, 23   ; r28 = alpha<<8 | red
rlwimi r11, r28, 8, 0, 23   ; r11 = ..   <<8 | green
rlwimi r10, r11, 8, 0, 23   ; r10 = ..   <<8 | blue
stw    r10, 0xc(r30)        ; mColor
```

So `mColor = alpha<<24 | red<<16 | green<<8 | blue`, a D3DCOLOR, the same as
RB3's. `SaveCompressedVertex` writes it as one big-endian word, so the disc
bytes are A, R, G, B. DC3's vertex declaration fetches offset 12 as
`D3DDECLTYPE_D3DCOLOR` (`rnddx9/Mesh.cpp`, the same as rb3-xenon's). The DC3
shape read red from the low byte, so it swapped red and blue, as RB3's did
before 12.2.

**Assets.** DC3 has no AO data, so 12.2's band-0 test cannot be repeated. A
temporary probe in milo-viewer logged the packed colour bytes of every
compressed mesh it uploaded. It covered 1,611 DC3 `.milo_xbox` files under
`milo-rnd-library/dc3`:

- world 141, char 322, ui 1,139, modular_song_data 6, flow 3.
- 1,552 runs were logged, and 1,082 of them uploaded at least one compressed mesh.
- 26 UI files stop in a texture load before any mesh (`PopRev ABORT`).
- That gave 2,798 records and 2,698 distinct mesh records.

| kind | distinct records | red ≠ blue |
|---|---|---|
| AO (`HasAOCalc`) | 0 | — |
| prelit, static | 135 (97 white, all grey or white) | 0 |
| lit, skinned | 753 (all white) | 0 |
| lit, static | 1,822 | 12 |

The 12, by disc bytes A, R, G, B:

| meshes | bytes | read as ARGB | read the old way |
|---|---|---|---|
| `tanarmy_row_01.1`–`.8` | 0, 252, 255, 0 | yellow | cyan |
| `tanarmy_row_01.9`, `tongue` | 255, ~4, ~4, ~250 | blue | red |
| `charged_xl_icon` and its `_alpha` copy | 255, 178, 25, 25 | red | blue |

All 12 are lit static meshes. The DC3 light model gives those no vertex tint
(`standard_wgsl.inc`: the baked colour counts only for prelit draws and, at
`kSkinnedVtxChroma`, for skinned ones). So no DC3 draw shows the swap.

**Change.** `kCompressedColorIsArgb` is removed from both shape headers.
`UnpackColor_BE` now always reads `alpha<<24 | red<<16 | green<<8 | blue`. The
RB3 shape's read is unchanged and the DC3 shape's read is corrected (engine
commit `9ec481d`).

**DC3 before and after.** `f0ecb01` vs `9ec481d`, dc3-decomp on `e992ee9b5`:

| render | pixels that differ |
|---|---|
| milo-viewer at 960×540: `glitterati`, `dci`, `tan_tanarmyrow`, `angel03`, `aubrey03`, `move_flashcard` | 0 in all six (and 0 between two runs of the old binary) |
| dc3-native boot, frames 300, 600 and 900 | 0 in all three |
| control: `tan_tanarmyrow` with `MILO_SIMPLE_RENDER=1` | 32,459 |

`MILO_SIMPLE_RENDER=1` forces every draw prelit, so the vertex colour shows.
In the control the army turns from cyan (mean RGB 3, 82, 83) to yellow
(81, 82, 4), so the instrument does see the change when a draw uses the colour.
The boot frame's mean |ΔRGB| to the xenia DC3 references is unchanged at
37.49 against `01_dc3_neon_logo` and 49.29 against `04_main_menu`.

**rb3-xenon** draws RB3 content through the DC3 shape. Its skinned meshes
carry RB3's AO SH data in the colour, and the DC3 model tints skinned draws by
a desaturated vertex colour, so this read does change rb3-xenon. `rb3-render`
was rendered against both engines, built from the same xenon tree (`5aa1d53b0`):

| cell | pixels that differ | mean RGB of those pixels, before → after |
|---|---|---|
| `crowd_female01` | 100,523 of 921,600 (largest channel step 26, 99th percentile 9) | 83.4, 87.9, 96.7 → 87.4, 89.3, 95.4 |
| `tracksystem_meshes` | 32 | — |

The crowd shifts slightly warmer. Two runs of the new engine are
pixel-identical, so this is not run-to-run noise. All 40 `rb3-render` gates
pass on both engines.

**RB3 title.** This change does nothing to the RB3 shape, which already read
ARGB. Same frame instrument as 12.4:

| f400 | sky_dE | city_dE | city_edge | city luma / xenia |
|---|---|---|---|---|
| `f0ecb01` | 11.6 | 11.6 | 0.913 | 0.946 |
| `9ec481d` | 11.5 | 11.6 | 0.910 | 0.955 |

The luma ratio moves only because the billboard in `city_rmid` animates
(40.4 vs 41.1 there).

### 13.2 AO and the box light on the title city

Retail scales the box light by AO. That is not why the middle city is dark.

**Retail shaders.** These are the vertex-lit and per-pixel permutations, read
in the xenia disassemblies. `c80`–`c85` are the box faces, `c1` the ambient,
and `c0` the diffuse colour.

The ambient AO factor `ambAO = sat((1.128379 R − 1) S + 1)` (S is `c24.x`)
multiplies the box term and the ambient term. Each point light gets its own
factor, `ptAO`. The constants are the shaders' `.lit` literals:

- `c253` = 3/4π, 0.282095
- `c254` = 1/4π, 2, −1, 1.128379
- `c255` = 0, 1, 0.488603, 3π/4

| permutation | box × ambAO | ambient × ambAO |
|---|---|---|
| `0x14000030010` VS | instr 58 `mad r1, r0.z, box, point` | instr 59 `mad r1, r0.z, c1, r1` |
| `0x14000030011` PS | instr 25 `mad r4, box, r4.w, point` | instr 5 `mul r2, c1·c0, r4.w` |
| `0x14000030037` PS | instr 56 `mul r1, box, r6.w` | instr 15 `mul r10, c1·c0, r6.w` |

The engine's retail light path does the same. So the answer to 12.7's
question is yes: AO should scale the box light, and it already does.

**What lights those regions.** A temporary per-draw probe ran on the title's
frame 398, just before the f400 capture. Of the 66 AO draws whose
world-sphere screen bounds overlap the middle city region, 44 have an all-zero
box:

- All draws from `back_left.env` and `buildings_dim.env`. Those envs have no approx lights.
- 36 of the 44 `cityscape.env` draws. That env's only approx light,
  `streetapprox` (point, range 1200), does not reach most building centres.

The 11 `theater.env` draws (the facade under the logo) do have box light
(brightest face 0.15–0.41). The ambient of `cityscape`, `back_left`,
`theater` and `street` is exactly 0. So most of that region is lit only by
point lights, each scaled by its own `ptAO`. 12.4's "what is left there is AO
scaling the box light" was wrong.

**Ablations.** Each AO term was turned off on its own, one build each, at
f400. The switch builds are `f0ecb01` plus one switch, so `f0ecb01`'s all-AO
row is their reference. All-AO is retail behaviour; the switches only
attribute the darkening.

| f400 luma | sky left | city right | city mid | city rmid | left roof | city luma / xenia |
|---|---|---|---|---|---|---|
| xenia front buffer (10.2) | 33.6 | 41.0 | 51.9 | 39.0 | 29.1 | 1.000 |
| TCRF hardware capture | 22.1 | 40.4 | 55.0 | 66.4 | 12.1 | — |
| `f0ecb01`, all AO (retail) | 22.6 | 42.0 | 40.3 | 40.4 | 13.5 | 0.946 |
| AO not applied to the box | 22.8 | 42.1 | 41.5 | 40.6 | 13.6 | 0.957 |
| AO not applied to point lights | 22.6 | 44.6 | 42.0 | 43.2 | 14.8 | 1.002 |
| no AO | 22.9 | 48.0 | 43.1 | 43.2 | 15.1 | 1.037 |
| `9ec481d`, all AO | 22.7 | 42.2 | 40.5 | 41.1 | 13.5 | 0.955 |

The two all-AO rows run identical RB3 code. Their gap gives the noise: up to
0.2, and 0.7 in `city_rmid`, where a billboard animates. The TCRF `city_rmid`
figure shows a different billboard frame.

- **The left roof is not a defect.** The hardware capture reads 12.1 there and
  the native 13.5. Xenia's 29.1 is the outlier, as is its left sky (33.6 vs
  22.1 on hardware). The xenia capture is the hub, whose camera is not quite
  the title's: the characters sit at different screen positions. AO takes
  1.6 off the native roof, nearly all through the point light.
- **The middle city is dark, but AO is not the main reason.** Both references
  read 52–55. The native reads 43.1 with no AO at all, in line with 12.4's
  pre-W16-RA base (42.8). The gap to xenia is 11.6, and AO accounts for 2.8 of
  it: 1.2 through the box and 1.7 through the point lights. The rest was
  there before AO; 13.4 has the lead.
- So the city-wide 0.946–0.955 is retail AO on top of a middle city that was
  already about 17% below xenia without it.

No code change for this finding.

### 13.3 Consumer verification (engine `w16-re`)

Fresh `~/tmp` worktrees made with each repo's `scripts/setup_worktree.sh`,
configured with `-DMILO_ENGINE_PATH` at the `w16-re` engine worktree
(confirmed in each `CMakeCache.txt`), built at `9ec481d`.

| consumer | instrument | result |
|---|---|---|
| dc3-decomp (on `e992ee9b5`) | `scripts/native_test.sh` | 626 registered, 557 executed, 557 passed, 0 failed, 69 skipped (budget 69), rc=0 |
| rb3-xenon (on `5aa1d53b0`) | `tools/native_health.sh` | `NATIVE_HEALTH_RESULT verdict=PASS link=PASS link_verified=18 link_expected=18 link_skipped=0 runtime=PASS runtime_ran=18 runtime_total=18 gates_pass=77 gates_fail=0 unrunnable=none selftest=SKIPPED scatter_unlinked=16 scatter_dirb=0 scatter_multihost=17 rc=0 handpose_controls=- handpose_baseline_fail=- runtime_crashed=0 runtime_failed=none` (embedded link gate: `verdict=PASS expected=18 verified=18 skipped=0 partial=0 failed=0 rc=0`) |
| rb3-xenon | `tools/native_build_gate.sh` | run as the lane's last action; its `NATIVE_GATE_RESULT` line is in the lane report |
| rb3 (Wii), dc3 flavor (on `cbce497e4`) | title (13.1) and Quickplay | title rc=0 (f400 11.5 / 11.6 / 0.910); Quickplay reaches `game_screen` in 2 of 2 runs (first logged at frames 818 and 801) |

A rebuild of the rb3 binary from the committed engine is byte-identical to the
one measured (`cmp`).

The engine's shader source was briefly edited for the AO switches while the
consumers were building. Both consumers' `PipelineManager.cpp.o` were compiled
after the file was restored (rb3-xenon 19:45:08, dc3 19:45:14, against a
restore at 19:44:29). rb3-xenon's health run was restarted on the clean tree.

### 13.4 Not done

- **The middle city's remaining gap** (13.2): the region reads about 43 with AO off, against 52–55 in both
  references. The theater facade's env, `theater.env`, has a real point light
  (`theater.lit`, 2.0 / 1.435 / 0.557, range 500) and four approx lights.
  Whether retail lights that region brighter was not examined.
- **The DC3 light model's vertex tint** (prelit: as authored; skinned:
  `kSkinnedVtxChroma` of the chroma; lit static: none) is a native heuristic.
  It was not compared against DC3's retail standard shader. The 12 coloured
  lit static meshes in 13.1 would show if it changed.
- The colour probe, the per-draw probe and the AO switches were temporary and
  are not committed.
- **Inverse square root**: unchanged from 12.7.
- No merge, pin bump or push.

## 14. The last BandRnd-only features: texture sharpen, draw log, WGSL coverage, crowd colour mod (lane W16-RH, 2026-10-06)

Before this lane, four rb3 test suites (`test_texsharpen`,
`test_texsharpen_manager`, `test_wgsl_validation`, `test_draw_log_golden`)
built only when `RB3_GPU_BACKEND=rb3`, and section 7.5 listed crowd recolouring
as unimplemented in both backends. Under the dc3 flavor, the rb3 facade
(`rb3_rnd_backend_dc3.cpp`) stubbed the draw log as empty and texture sharpen as
off. All four suites now build and pass under both flavors. The colour
modulation is implemented with retail's math in the shared standard shader.

| gap | before | after |
|---|---|---|
| Progressive texture sharpen | dc3 facade stub: `RB3ProgressiveSharpenEnabled()` returns false; suites rb3-only | the engine's `RB3TexSharpen.cpp` manager builds for dc3 + rb3wii over new texture-cache hooks; 7/7 sharpen tests pass under dc3 |
| Draw log + provenance | dc3 facade stub: empty log, `RB3DebugDrawLogEnabled()` false | every `Mesh_Wgpu` draw is recorded in BandRnd's format; the title dump has 881 draws, `/api/drawlog?prov=1` has 890 with provenance; 10/10 golden-comparator tests pass, the real-draw test skips by design in both flavors |
| WGSL validation | compiled BandRnd's 5 `.inc` modules + standard; rb3-only | compiles the linked flavor's own table (10 modules under dc3, 6 under rb3); found and fixed a real DoF shader bug |
| Crowd colour mod | no backend reads `mColorModFlags` / `mColorMod`; semantics unknown | retail's three modes implemented in `standard_wgsl.inc`; mode 0 is an exact identity; inert on shipped RB3 content (14.4) |

### 14.1 Texture sharpen

The manager (`platform/RB3TexSharpen.cpp`) was already flavor-neutral apart
from four texture-cache hooks. `platform/rndshape/RB3WiiTexSharpen.cpp`
implements them over `Tex_Wgpu`:

- `RB3SharpenTexFingerprint`: a new `GpuTexPixelFingerprint`, the same hash
  `PresyncBitmap` uses to detect changed pixels.
- `RB3SharpenReuploadTex`: calls `PresyncBitmap`. It reports success when the
  texture's create counter moved (`GetGpuTexDebugInfo(tex).createCount`, a
  new global counter of `PresyncBitmap` re-creates). `PresyncBitmap` re-creates
  at the bitmap's current size whenever the pixel pointer or fingerprint
  changed. The dc3 mesh path builds the material bind group per draw from
  `GetGpuTexView`, so the next draw binds the new view without further
  invalidation.
- `RB3DebugUploadTex` and `RB3DebugGetTexGpuInfo`: a test-facing view of
  `GpuTexDebugInfo` (present, uploaded, size, view/texture handles, create
  count). `GpuTexData` records the last uploaded size for this.

The manager test's "GPU not ready" case used to set `gBandRnd.mGpuReady=false`.
It now calls a new `RB3DebugSetSharpenGpuUnavailable(bool)`, which both flavors
define.

### 14.2 Draw log and provenance

`platform/rndshape/RB3WiiDrawLog.cpp` (dc3 + rb3wii only) carries BandRnd's
record, provenance, dump and accessor code. The engine side is a neutral seam
in `RndShape.h`: `DrawLogActive`, `DrawLogFrameBegin`, `DrawLogPassOpen`,
`DrawLogRecord` and `DrawLogFrameEnd`. They are inline no-ops in
`RndShape_DC3.h`, so DC3 and rb3-xenon compile them away. `WgpuRnd` opens the
frame in `BeginDrawing`, numbers the passes (main, overlay, HUD, texture) and
writes the `RB3_DRAWLOG_DUMP` JSON in `EndDrawing`. `Mesh_Wgpu` fills a
`DrawLogDraw` after each `DrawIndexed`. The log is active under `RB3_DRAWLOG`,
under `RB3DebugSetDrawLogEnabled(true)`, or when provenance is on.

Differences from BandRnd's records, each a consequence of how the dc3 path
binds, not an omission:

- **Tokens.** BandRnd's scene/material/object/bone tokens are bind-group
  handles. The dc3 path builds bind groups per draw, so a handle would name
  every draw differently. The tokens here hash (uniform ring buffer, offset),
  which names the same uniform data the same way. The bone token of an
  unskinned draw is the dummy bone bind group.
- **World matrix** is `ObjectUniforms.world`, which is identity for skinned
  meshes (their bones carry the transform).
- **Vertex count** is the uploaded buffer's count.
- **`boneFallback`** is always 0: the dc3 skinning path has no fallback state.
- The provenance sphere and exact-vertex screen rects use the mesh's own
  `WorldXfm`.

Measured on the title under dc3. The `RB3_DRAWLOG_DUMP` at frame 401 has
881 draws, with 21 distinct scene tokens, 881 material, 881 object, 568 bone
(567 skinned draws plus the dummy), 21 pipelines and 269 mesh names. HTTP
`/api/drawlog?prov=1` at frame 312 returned 890 draws, all with provenance
(`rectKind` 1: 312, 3: 576, 2: 1, 0: 1; camera `world.cam` 553, none 335).
`/api/uidump` returns ok. The golden test's own comment records BandRnd's boot
capture of a menu frame as 877–891 draws.

`DrawLogGolden.PopulatesFromRealDrawMesh` skips in both flavors by design (its
comment: the unit fixture stands up no render pass). The live captures above
are the real-draw evidence.

### 14.3 WGSL coverage, and a DoF shader bug

The old test embedded BandRnd's five `.inc` modules and the standard shader,
so under dc3 it would have validated shaders the renderer never compiles.
`gfx/ShippedWgsl.h` now declares `ShippedWgslModules()`, and exactly one
definition links per flavor:

- dc3 (`gfx/ShippedWgsl_DC3.cpp`): standard, bloom, DoF, DoF depth resolve,
  `DrawRect2D`, post-proc, `RB3RetailPost`, display ramp, shadow, particles.
  Each source comes from an accessor in the file that compiles it, so the table
  holds the exact bytes.
- rb3 (`platform/RB3ShippedWgsl.cpp`): standard plus the five BandRnd modules.

`WgslValidation.AllShippedShadersCompile` compiles every entry on the device
and fails on any error. `HarnessCatchesBadShader` still proves the harness can
fail.

The first dc3 run failed:
`gfx/DofPass.cpp (dof): 'textureSample' must only be called from uniform
control flow`. DoF's blur loop sampled after an early return. Dawn rejects
that module, so a DoF pipeline built from it is invalid. The pass runs only
when `TheDOFProc` is enabled, which is why no frame showed it. The taps now use
`textureSampleLevel(..., 0.0)`; the scene texture has a single mip, so the
result is the same. All 10 modules compile.

Not examined: the DoF depth texture is bound as `UnfilterableFloat` and sampled
through a filtering sampler. That is a bind-group-layout question that shader
compilation cannot see. Only creating the DoF pipeline with DoF enabled would
show it.

(Examined in section 18: it was a real validation error, and the pass was
rewritten from retail.)

### 14.4 Crowd colour modulation, from retail

RB3's `RndMat` carries `mColorModFlags` and three `mColorMod` colours.
`Crowd::Init` (`world/Crowd.cpp`) gives each 3D crowd character's materials
`kColorModModulate` with three `ColorPalette` picks. Retail
`NgMat::SetupShader` loads the colours into `c131..c133` whenever the flags
are set. Section 7.5 noted that no backend consumed this and that the Wii
material had nothing to copy. The retail Xbox shader microcode does consume it,
and its math is what is now implemented:

| mode | bands of texel alpha `a` | colour | output alpha |
|---|---|---|---|
| 3 Modulate | [0, 1/3], [1/3, 2/3], [2/3, 1]; t = 3a − band | × `mix(c_band, 1, t)` | unchanged |
| 2 AlphaUnpackModulate | [0.1, 0.4], [0.4, 0.7], [0.7, 1]; t = (a − lo)/0.3 | × `mix(c_band, 1, t)` | 10a |
| 1 AlphaPack | — | unchanged | 0.1 + 0.9a |

Band membership is `clamp(a, lo, hi) == a`, so a value on a band edge falls
in both bands. The retail shader applies three chained conditional multiplies,
and so does `colorModScale`. The scale applies to the fully shaded colour,
additive terms included.

Implementation:

- `MaterialUniforms.colorMod` (`vec4u`) holds the three colours as RGBA8 in
  `pack4x8unorm` order, with the mode in `.w`. A first version used 3×`vec4f`,
  which made the struct 288 bytes and doubled its ring slot. Packing keeps it at
  256 (`static_assert`).
- `rndshape::MatColorMod` reads the flags and colours. It is inline in
  `RndShape_RB3Wii.h` and returns 0 from `RndShape_DC3.h`.
- `MaterialSetup::FillColorMod` packs them for the primary material and the
  NextPass.

**It is inert on shipped RB3 content, and that was measured, not assumed.**
I predicted crowd draws in Quickplay would use Modulate. A temporary
per-bind probe counted 320,000 material binds through `game_screen`: all
mode 0, none AlphaPack, AlphaUnpackModulate or Modulate. The cause:
`WorldCrowd::AssignRandomColors` sets `mUseRandomColor` only when the venue's
`random1.pal`–`random3.pal` palettes load. None of the 4,455 RB3 `.milo_xbox`
files contains them (84 files contain the `ColorPalette` class string). So
`Crowd::Init` never sets Modulate. Consistent with that, the retail RB3 shader
cache has no Modulate permutation; it does have the AlphaUnpackModulate one
(`0x4202000010`). The code is there for content that sets the flags.

Mode 0 is `× vec3f(1.0)` with the alpha unchanged, which is bit-exact. Every
DC3 material, and every rb3-xenon material, takes mode 0.

### 14.5 Exit-time teardown in test binaries

Under dc3, all four suites passed and then the binary died with rc=139 at
exit. gdb put the fault in `~ShadowPass` inside `~WgpuRnd`, during libc's
static-destructor phase, after Dawn's Vulkan backend was gone. The game
avoids this through the `Debug::Exit` callback (`RB3RegisterBandRndShutdown`).
A gtest binary returns from `main` and never reaches it. The rb3 flavor
had the same fault in the same suites (`~PipelineManager` in `~BandRnd`):
10 SegFaults in its ctest run. Both facades' `InitGpu` now registers an
`atexit` teardown once. That handler runs before the renderer's static
destructor and is a no-op if the teardown already ran.

### 14.6 Title and Quickplay

rb3 dc3-flavor `rb3-native` built from the committed rb3 + engine branches
(`bin-final`, byte-identical by `cmp` to the build the colour-mod probe and
shots were measured on). `title_capture.sh` (`RB3_FIXED_CLOCK=1`) and
`title_fidelity.py` against TCRF, frame 400:

| run | sky_dE | city_dE | city_edge |
|---|---|---|---|
| engine base `b44d40d`, three runs | 11.5–11.6 | 11.7–11.9 | 0.911–0.912 |
| this lane, two runs (288-byte colour-mod build) | 11.5 | 11.9 | 0.910 |
| this lane, final | 11.5 | 11.7 | 0.912 |

Within run-to-run noise, which is large: two base runs differ in about 95% of
pixels even under `RB3_FIXED_CLOCK`. That is the expected result, since every
title material takes colour-mod mode 0. Title exit rc=0.

Quickplay (`venue_capture.py`, `RB3_FIXED_CLOCK=1`) reached `game_screen` at
frame 759 and captured gameplay at frames 822, 1060, 1360 and 1663. Exit
rc=0, with no crash or WGSL validation lines in the log. Earlier runs in this lane:
base 1,769, the 288-byte build 795, the probe build 734. The arrival frame
varies run to run, so venue frames are not compared pixel for pixel across
builds.

### 14.7 Consumer verification (engine `w16-rh`)

Fresh `~/tmp` worktrees made with each repo's `scripts/setup_worktree.sh`,
configured with `-DMILO_ENGINE_PATH` at the `w16-rh` engine worktree
(confirmed in each `CMakeCache.txt`), built at `000d802`.

| consumer | instrument | result |
|---|---|---|
| dc3-decomp (on `e992ee9b5`) | `scripts/native_test.sh` | 626 registered, 557 executed, 557 passed, 0 failed, 69 skipped (budget 69), rc=0 |
| rb3-xenon (on `dd88a0ee7`) | `tools/native_health.sh` | `NATIVE_HEALTH_RESULT verdict=PASS link=PASS link_verified=18 link_expected=18 link_skipped=0 runtime=PASS runtime_ran=18 runtime_total=18 gates_pass=77 gates_fail=0 unrunnable=none selftest=SKIPPED scatter_unlinked=16 scatter_dirb=0 scatter_multihost=17 rc=0 handpose_controls=- handpose_baseline_fail=- runtime_crashed=0 runtime_failed=none` (embedded link gate: `verdict=PASS expected=18 verified=18 skipped=0 partial=0 failed=0 rc=0`) |
| rb3-xenon | `tools/native_build_gate.sh` | run as the lane's last action; its `NATIVE_GATE_RESULT` line is in the lane report |
| rb3 (Wii) `w16-rh` (`763ef1efc`), dc3 flavor | `ctest` | 123 tests: 116 passed, 7 skipped (six fixture-gated tests plus `PopulatesFromRealDrawMesh`), 0 failed |
| rb3 (Wii) `w16-rh`, rb3 flavor | `ctest` | 123 tests: 116 passed, the same 7 skipped, 0 failed (10 exit SegFaults before 14.5) |
| rb3 (Wii), dc3 flavor | title and Quickplay | title rc=0 (f400 11.5 / 11.7 / 0.912); Quickplay reaches `game_screen` at frame 759, rc=0 (14.6) |

Before this lane, the dc3-flavor ctest did not build the four suites: on the
base engine it ran 104 tests. It now runs the same 123 as the rb3 flavor; the
19 added are the four suites.

### 14.8 Not done

- **BandRnd's material binder does not fill `colorMod`.** Under the rb3
  flavor every draw takes mode 0, as before; BandRnd is not the shipping path.
- **DC3's own colour modulation.** DC3 materials use `kColorMod*` bits in the
  permutation key (bits 32–33). `RndShape_DC3.h::MatColorMod` returns 0, so DC3
  is unchanged. Wiring it is a follow-up for the DC3 consumer.
- **The pseudo-HDR alpha** (the bloom mask in alpha) is computed from the
  post-modulation colour; retail computes it before. This matters only for a
  material with a non-zero mode, and shipped content has none.
- **DoF depth binding** (14.3): unfilterable depth through a filtering sampler,
  not examined. Done in section 18.
- The colour-mod probe was temporary and is not committed.
- No merge, pin bump or push.

## 15. rb3-web on the dc3 backend (lane W16-RJ, 2026-10-06)

rb3's Emscripten target `rb3-web` now defaults to `RB3_GPU_BACKEND=dc3`, like
the desktop build. Before this lane, `native/CMakeLists.txt` kept web on `rb3`
with the reason "rb3-web still drives gBandRnd directly". That reason was
stale: `main_web.cpp` already reached the renderer only through
`RB3RndBackend` (section 1's facade), and no web-compiled rb3 source names
`gBandRnd` outside comments. Section 4 recorded that the web build "compiles
through the facade but was not built or run under emscripten". Building and
running it found two engine bugs in `WgpuRnd`'s `__EMSCRIPTEN__` arms. Both
were latent because DC3, until now the arm's only consumer, boots in a
different order and names its canvas `#dc3-canvas`.

Branches (not merged, not pushed, no pin bumped):

| repo | branch | commits |
|---|---|---|
| milo-native-engine | `w16-rj` (on `cc8acc2`) | `81b96a0` the two fixes; this section |
| rb3 | `w16-rj` (on `05f959153`) | `24a21baf5` default + `build.sh --backend`; `7079f0b22` smoke checks |

Landing order matters. rb3 `24a21baf5` makes web default to dc3, and before
engine `81b96a0` the dc3 web renderer draws nothing (15.2). So the engine
branch, and an rb3 pin bump to include it, must land first or together.

### 15.1 What changed

Engine (`81b96a0`):

- **`WgpuRnd::Init` re-created the GPU device on web.** rb3-web runs
  `RB3RndBackend::StartGpuInit`, waits for `IsReady`, calls
  `InitGpuResources`, and only then constructs the `App`, whose ctor calls
  `TheRnd->Init()`. The desktop arm already handled a consumer that brought
  the device up first (section 1, RB3's `RB3_GAME` boot). The web arm called
  `mGpu.Init` unconditionally. That replaced the instance, and once its
  adapter callback ran, the device, so every pipeline, ring and texture made
  before it belonged to a dead device. The web arm now takes the same "already
  up, so `InitGpuResources`" branch. `InitGpuResources` is idempotent. DC3's
  web boot calls `Init` before any device exists, so it still takes the old
  path.
- **`BeginDrawing` polled `#dc3-canvas` and ignored the result.** On
  `#rb3-canvas`, `emscripten_get_canvas_element_size` failed and left both ints
  unwritten. Every frame then resized the surface to stack garbage: depth and
  MSAA textures of 5684687×5685950, and "Could not create the swapchain
  texture". The selector is `MILO_WEB_CANVAS_SELECTOR`, which only TUs
  compiled into the consumer's web target see; `Rnd_Wgpu.cpp` is in
  `libmilo-engine.a`. So `GpuDevice` gained `CanvasSize(int&, int&)`, defined
  in `GpuDevice_Web.cpp` and declared only under `__EMSCRIPTEN__`, which
  returns false and zero sizes when the page has no such canvas. The poll uses
  it and checks the result. dc3-decomp's `native/CMakeLists.txt` sets
  `milo_engine_set_web_canvas_selector(dc3-web "#dc3-canvas")`, so DC3 reads
  the same canvas as before.

rb3:

- `native/CMakeLists.txt`: `RB3_GPU_BACKEND` defaults to `dc3` on every
  platform; `rb3` stays selectable. It also rewrites two stale comments: the
  one above the variable still said WgpuRnd "stay[s] OFF for RB3", and the
  rb3-web boot comment still named `gBandRnd`.
- `scripts/web/build.sh --backend dc3|rb3` (default `dc3`, or
  `$RB3_GPU_BACKEND`) passes the flavor explicitly, and the script's
  reconfigure-on-drift check now covers it. `RB3_GPU_BACKEND` is a non-FORCE
  cache variable, so a web build dir configured while the default was `rb3`
  would otherwise stay on BandRnd. The main checkout's three web build dirs
  predate the variable and have no entry, so their next configure takes `dc3`
  either way.
- `scripts/web/smoke-test.mjs`: a fifth check (15.3).

### 15.2 Measurements

Every leg: `RB3_WEB_RELEASE=ON` (`-O0 -g0 -fno-inline`), emcc 5.0.2, engine
`w16-rj`, assets from `~/code/milohax/rb3/orig-assets/extracted` via
`RB3_ASSETS`, served by `native/web/server.py`, driven by headless Playwright
Chromium. The flavor of each run is read from its own console line
`StartGpuInit (async, <flavor> backend)`, not inferred from what was deployed.

| leg | wasm sha1 | boot smoke (checks 1–4) | uncaptured WebGPU errors | main_hub canvas |
|---|---|---|---|---|
| rb3 flavor, base engine | `0d981db5` | PASS, 83 songs | 0 | renders (84.21% painted) |
| dc3, base engine | `267c2e99` | **PASS**, 83 songs | **369,829** (1,298,924 console lines) | screenshot fails |
| dc3, `Init` fix only | `e7cfa7f5` | **PASS**, 83 songs | 33,650 | black (0.00%) |
| dc3, both fixes | `6f308bf8` | PASS, 83 songs | **0** | renders (75.48%) |

The two middle rows are the reason for 15.3: the existing smoke test passed a
renderer that drew nothing.

Further checks on the final state:

- **Link.** The undefined-symbol sets the link warns about
  (`-sERROR_ON_UNDEFINED_SYMBOLS=0` turns each into a no-op JS stub) are 257
  on both flavors and identical. The dc3 link introduces no new silent stub.
- **Gameplay.** `scripts/web/keyboard-to-gameplay.mjs` on dc3: PASS, reaching
  `game_screen` by pure keyboard (guitar, hard), with 0 uncaptured WebGPU
  errors. The venue, its lights and the crowd render.
- **The documented entry point.** `scripts/web/build.sh --release` with no
  flavor flag, in a fresh dir: rc=0, `RB3_GPU_BACKEND:STRING=dc3`, wasm
  `b4928136` (8.5 MB; 1.9 MB brotli). On that deploy the strengthened smoke
  passes (0 WebGPU errors, 76.06% painted) and so does keyboard-to-gameplay
  (0 errors).
- **A flagless `emcmake cmake` configure** gives `RB3_GPU_BACKEND=dc3` and
  `MILO_ENGINE_GPU_BACKEND=dc3`. Its build graph has `rb3_rnd_backend_dc3.cpp`
  and `Rnd_Wgpu.cpp`, and no `Rnd_Wgpu_RB3` / `rb3_rnd_backend_rb3`.
- **The rb3 flavor is untouched.** Rebuilt on the final engine (13 TUs
  recompiled), its wasm is byte-identical to the base-engine build
  (`0d981db5`). `CanvasSize` is unreferenced there, and the strengthened smoke
  passes (85.35% painted).
- **Desktop is untouched.** Every edited line is inside `__EMSCRIPTEN__`.
  Measured on rb3's desktop dc3 compile command for `Rnd_Wgpu.cpp` (which
  includes `GpuDevice.h`), with `-E -P` against `cc8acc2` and `w16-rj`: both
  outputs are 90,534 lines and byte-identical. Control: the base leg's line
  markers show all 26 engine headers it read came from the base checkout and
  none from `w16-rj`. `GpuDevice_Web.cpp` is not compiled on desktop.
- **Remaining web console lines** that rb3 does not print are the dc3 path's
  own stderr diagnostics, which emscripten routes to `console.error`:
  `BoneSetup`'s `BONE DIAG` (three dumps per process at frames 1000–1600) and
  `Mesh_Wgpu: skipping … no vertices`. Desktop prints them too. The NOTIFY
  lines differ between runs because the hub cast is randomized per boot.
- **Character look.** On the web hub, part of the dc3 cast renders bald with
  glossy skin, where the rb3 flavor's cast had hair. The desktop dc3 build at
  the same screen (Start, overshell options, band shot, frame 500 after
  arrival) shows the same: bald heads on part of the cast, specular skin. So
  this is how the dc3 backend renders this content, not a web regression.
  The tint differs (gold on web, magenta on desktop) because the hub's lights
  animate and the two frames are at different times. Not investigated
  further.

### 15.3 The smoke test now sees the renderer

`smoke-test.mjs` check 5 fails on any `GpuDevice: uncaptured error` console
line, or on a main_hub canvas below 10% painted (`captureCanvasStats`,
threshold 12). `result.json` records `gpu_error_count` and `painted_pct`. The
floor comes from measurement: working frames read 58–97% (rb3 web 84.76,
dc3 web 76.06, desktop dc3 hub 58.24, gameplay 96.81), and the black canvas
reads 0.00.

Control: the old `#dc3-canvas` poll restored and rebuilt. The smoke fails
(rc=1) on both new lines: 33,593 WebGPU errors and a 0% painted canvas. With
the poll restored to the fix and rebuilt, the wasm is byte-identical to the
fixed build (`6f308bf8`).

### 15.4 Consumer verification (engine `w16-rj`)

| consumer | instrument | result |
|---|---|---|
| rb3 `w16-rj`, desktop, flavor from the default (`dc3`) | `ctest` | 123 tests: 116 passed, 7 skipped (the six fixture-gated tests plus `PopulatesFromRealDrawMesh`), 0 failed, rc=0; same counts as 14.7 |
| rb3 `w16-rj`, web, dc3 | `smoke-test.mjs` (with check 5), `keyboard-to-gameplay.mjs` | PASS, PASS (15.2) |
| rb3 `w16-rj`, web, rb3 flavor | `smoke-test.mjs` (with check 5) | PASS |
| rb3-xenon, dc3-decomp (desktop) | preprocessed-source identity (15.2) | the changed TU preprocesses byte-identically; their desktop suites were not rerun |

### 15.5 Not done

- **dc3-decomp's own web target (`dc3-web`) was not rebuilt or run.** Its
  selector is `#dc3-canvas`, so `CanvasSize` reads the same canvas the
  hardcoded poll did. Its boot calls `WgpuRnd::Init` before any device
  exists, which takes the unchanged branch. Both are by reading, not by
  measurement.
- **rb3-xenon and dc3-decomp desktop suites** were not rerun; 15.2 shows the
  changed TU preprocesses identically on desktop.
- **Web vs desktop pixel parity** under dc3 was not measured. The cast and
  hub lighting differ run to run, and the web `.milo_xbox` downscale and
  on-demand fetch paths differ from desktop's disk reads.
- **The debug web build** (`build.sh --debug`, `-g2`) and the `-O>0` release
  were not built. The shipped release default is `-O0`.
- **SFX sidecar 404s** (`sfx/gen/xma_pcm/*`) appear in every leg because the
  server, run from a worktree, does not auto-detect the main checkout's
  `orig-assets/derived`. They are environmental and identical across flavors.
- **The display ramp on the web canvas** (section 10.7). Web frames call
  `DisplayGamma(presenting=true)`, and the ramp's frame texture there is
  `FrameResolved`, which carries `CopySrc`. No run printed the ramp's
  "does not allow CopySrc" warning. Whether the ramp applied (a non-zero
  gamma) was not probed, and its output was not checked against a
  reference.
- No merge, pin bump or push.

## 16. The bald, glossy band on main_hub (lane W16-RL, 2026-10-06)

Section 15.2 recorded part of the dc3 cast on main_hub rendering bald with
glossy skin, and left it uninvestigated. There are two separate causes. Both
are in rb3's native game code (`src/system/bandobj`), not in this engine, and
both fixes are there. The engine branch carries only this section.

### 16.1 Bald: multi-bone hair drawn at the world origin

A hair mesh with one bone (`bone_hair`) rendered. A hair mesh with several
bones (fauxhawk, ziggymullet, messyshort, visor, 50sbandana) did not. The
extra bones are the CharHair strand chains `bone_hair-*`, which live in the
hair resource's own dir. A temporary probe in `Mesh_Wgpu.cpp` printed, per
skinned draw, the skinned vertex average next to the bind average and each
bone's parent and world:

| mesh | skinned avg | bind avg | member's head |
|---|---|---|---|
| fauxhawk_resource.mesh | (-0.0, 0.6, 70.7) | (-0.0, 0.6, 70.9) | head.mesh skinned to (48.2, 24.2, 65.8) |

The strand roots' parent is `bone_hair.mesh` of the **shared static magnet
skeleton**: dir `''`, parent `bone_head` at (0, 0.3, 65.4), the world origin.
The world transforms are not stale: a recomposed world equals the cached one,
and dirty=0. `BandCharacter::RebindHeadHandsAtRest` moves the other head
meshes onto the member's own skeleton, but it resolves every bone by name in
the member. The strand bones never resolve, so the whole mesh stayed pending
(`HEAD_REBIND_PENDING … fauxhawk_resource.mesh miss=6 resolvable=1/7
why=unresolvable`, and `visor_resource.mesh miss=8 resolvable=1/9`). It was
then drawn bound to the magnet chain, so the hair sat at the origin and the
member looked bald.

The rb3 flavor had the same defect. The same pending lines appear there, and
its hub cast also lost those hairstyles; it only looked less bald. Section
15's "rb3 flavor's cast had hair" compared different casts.

The fix is rb3's `BandCharacter::NativeAnchorOutfitChain`, called from pass A
of `RebindHeadHandsAtRest` when a bone does not resolve:

1. Walk up to the first ancestor that resolves to a bone of this member
   (`bone_hair.mesh`).
2. Take the bone's rest as its current transform relative to that ancestor,
   times the ancestor's captured char-space rest. This uses the same capture
   rules as a distinct bone (no capture while a clip plays, finite only).
3. In pass B, reparent the chain's root onto the member's bone, keeping its
   local transform. The mesh is then baked and flagged like every other head
   mesh.

CharHair reads `Root()->TransParent()->WorldXfm()`, so the hair simulation
follows the live head too. Opt-out: `RB3_NO_OUTFIT_CHAIN_ANCHOR=1`.

After the fix:
- fauxhawk skins to (48.9, 33.7, 70.2), on the member's head.
- No `HEAD_REBIND_PENDING` lines appear in either flavor's hub run.

### 16.2 Glossy: skin specular with no specular map

Every band `head.mesh` draws one shared material: `head_naked.mat` from
`char/main/shared/char_shared.milo`, the same pointer for all four members.
Its retail terms (`MatRetailTerms`) are:
- per-pixel lit;
- specular rgb (1, 1, 1), power 30;
- rim (0.93, 0.16, 0.03), power 3;
- **no specular map and no normal map.**

The dc3 backend applies retail's specular terms (section 12). An unmasked
white specular at power 30 put hot spots on every face and torso.

Retail Xbox `OutfitConfig::SetSkinTextures` (rb3-xenon, same function) binds,
for each of the five skin materials:
- the specular map `<gender>_<part>_spec.tex`;
- a normal map: `<gender>_<part>_norm[_<variant>].tex` for torso, legs and
  feet, and the wrinkle blender's `head_wrinkle_output.tex` for the head.

The Wii build has no specular or normal maps, so the Wii `SetSkinTextures` in
rb3 dropped those lines. The textures exist in the Xbox data
(`char/main/shared/colorpalettes.milo`: `male_head_spec.tex`,
`male_head00_norm.tex`, the torso and legs variants).

The fix is rb3's `NativeBindSkinMaps`, called from `SetSkinTextures` under
`HX_NATIVE`. It restores that binding into `mXbSpecularMap` / `mXbNormalMap`
on dir1's material and on every drawn instance of the same name. It searches
recursively, because the textures sit in a nested subdir. It also ports
retail's `BandCharDesc::HeadNormVariant` for the body-type suffix.

One substitution: the head gets `<gender>_head00_norm.tex`, not the wrinkle
RT, because `RndTexBlender::DrawShowing` is a no-op in rb3's tree and that RT
is never painted. Opt-out: `RB3_NO_SKIN_MAPS=1`.

The BandRnd path never reads the `mXb*` maps (no `mXb` in `Rnd_Wgpu_RB3.cpp`
or `RB3MaterialBinder.cpp`), so the rb3 flavor's shading is unaffected.

### 16.3 Before / after

There is one binary for both legs; the "before" leg uses the two opt-outs.
Both legs use `RB3_FIXED_CLOCK=1`, Start ×2, and frame 580 on main_hub. The
hub camera's timing is not frame-exact between runs, so the framing differs
slightly.

- `~/tmp/w16rl_before_after_f580.png`: before
  (`RB3_NO_OUTFIT_CHAIN_ANCHOR=1 RB3_NO_SKIN_MAPS=1`) next to after. Before,
  the right member is bald and the visor member has no hair under the visor.
  After, both have hair, and the specular hot spots on faces and chests are
  gone.
- `~/tmp/w16rl_ab_montage.png`: before, hair fix only, hair + skin, at frames
  300 and 580.
- `~/tmp/w16rl_rb3_after/01_f0580.png`: rb3 flavor after the fix. The
  fauxhawk and spiky hair now render on the two right members, which were
  slicked or bald in `~/tmp/w16rl_rb3probe/01_f0580.png`.

These are scratch paths, not committed.

### 16.4 Verification

| consumer | instrument | result |
|---|---|---|
| rb3 `w16-rl`, desktop, dc3 (engine `w16-rl`) | `ctest` | 123 tests: 116 passed, 7 skipped, 0 failed, rc=0; same counts as 15.4 |

### 16.5 Not done

- **Shared skin materials.** One `head_naked.mat` instance serves every band
  member natively (the native milo merge; see the black-head comment in rb3's
  `OutfitConfig.cpp`). The last member to run `SetSkinTextures` sets its
  gender's spec and normal maps for all. A mixed-gender cast shares one set,
  just as it already shares one head diffuse. **Fixed in section 17.**
- **The head wrinkle normal RT** (`RndTexBlender`) is not composed natively.
  The head uses the neutral `head00` normal.
- **The web build** was not rebuilt. The fixes are in rb3 game code that both
  targets compile.
- rb3's rb3-flavor `ctest` and the native gate were not run.
- No merge, pin bump or push.

## 17. One skin material per band member (lane W16-RP, 2026-10-06)

Section 16.5 recorded that every band member draws the same `head_naked.mat`.
That is true of all five skin materials, and the cause is in rb3's native
merge shim, not in this engine. The fix is in rb3 game code
(`src/system/bandobj/BandCharacter.cpp`). The engine branch carries this section
and the registry rows for the two new flags.

Branches (not merged, not pushed, no pin bumped):

| repo | branch | commits |
|---|---|---|
| rb3 | `w16-rp` (on `f71e9e036`) | `46c37d736` the fix, probe, comments, regenerated ledger |
| milo-native-engine | `w16-rp` (on `f99c1f2`) | registry rows for `RB3_NO_SKIN_MAT_ADOPT` and `RB3_SKIN_MAT_ADOPT_PROBE`; this section |

### 17.1 Cause

A probe at the end of `OutfitConfig::SetSkinTextures` printed, for every mesh in
the member's tree whose material is one of the five skin names, the material
pointer and the dir that owns it. On main_hub (4 members, 2 male and 2 female),
every drawn skin mesh used `char/main/shared/char_shared.milo`'s instance. That
covers `head.mesh`, `hands_naked.mesh`, and every outfit `*_skin.N.mesh` (torso,
legs, feet). Each member's own same-named material, the one `dir1->Find`
returns and `SetSkinTextures` fills, was not drawn. The meshes are per member.
Only the materials were shared.

Consequences, before the fix:
- `head.mesh` on all four members drew one material, with
  `male_head_diff.tex`. The black-head fix binds a diffuse only when it is null,
  so the first member processed (male) set it for everyone. The two women drew
  the male head diffuse.
- Spec and normal maps (`NativeBindSkinMaps`, section 16.2) and the skin tone
  colour came from the last member to run `SetSkinTextures`.
- Torso, legs and feet drew char_shared's `dummy_torso.tex`, `dummy_legs.tex`
  and `dummy_feet.tex`, not the member's `<gender>_<part>_diff.tex`.

How retail gives each member its own materials: `BandCharacter::Filter` has a
branch for objects whose `Dir()` is `sCharSharedDir`, which is
char_shared.milo, found through `feet_skin.mat` in the outfit. The branch finds
the member's own object of the same name (retail asserts it exists and lives in
the member), calls `ReplaceRefs(theirs, mine)`, and returns `kIgnore`.
`ReplaceRefs` only rewrites refs owned by objects in `sOutfitDir`,
`sResourceDir` or `sToDir`, which is the outfit being installed. So each
install moves that outfit's skin meshes onto the member's own materials, and
the shared copy is never moved or written.

That branch runs only if the merge visits char_shared's objects, which needs
`kMerge` on the subdir. Natively, `BandCharacter::FilterSubdir` turns every
on-disk subdir's `kMerge` into `kReplace` (the shim against draining
`colorpalettes.milo`, documented at the function). `kReplace` appends the
subdir and returns without visiting its objects or its nested subdirs, so
`Filter` never saw char_shared. char_shared is reached in two ways, and the
shim blocks both:
- directly, as a subdir of the head, hands, hair and accessory outfits;
- nested under the torso, legs and feet outfits' `*_resource.milo`. There the
  shim stops one level higher, at the resource dir.

### 17.2 Fix

rb3 `NativeAdoptCharSharedRefs`, called from `FilterSubdir`. It runs whenever
the shim overrode `kMerge`, if the overridden subdir's tree contains
`sCharSharedDir`. It does the retail branch's step for each object in
char_shared's own table:
1. Find the member's object of that name.
2. Skip it unless it exists, is not char_shared's own object (natively,
   char_shared is also a subdir of the member, so a missing name resolves back
   into it), lives in the member (`Dir() == this`), and has the same class.
3. Call `ReplaceRefs(theirs, mine)`.

The shim's `kReplace` topology is unchanged: char_shared and the resource dirs
stay appended as subdirs, and nothing is moved. Each adoption reports 5 objects
adopted, the five skin materials. Opt-out: `RB3_NO_SKIN_MAT_ADOPT=1`. Probe:
`RB3_SKIN_MAT_ADOPT_PROBE=1` prints `[SKIN_MAT_ADOPT]` per adoption and
`[SKIN_MAT]` per drawn skin mesh, with `own=1` when the material is the
member's.

The native code downstream of the split still runs and still works:
- The black-head fix still binds the drawn `head.mesh`'s material. That is now
  `dir1`'s, which already has the gender diffuse.
- `NativeBindSkinMaps` still binds every drawn instance. That set is now
  `dir1`'s material plus `colorpalettes.milo`'s texblender copy of
  `head_naked.mat`, which the wrinkle meshes use. That copy is still shared,
  and its maps are still last-writer-wins. It is not drawn in the band shot.

### 17.3 Measurements

One binary for both legs. The "before" leg sets `RB3_NO_SKIN_MAT_ADOPT=1`.
Both legs use `RB3_FIXED_CLOCK=1`, `RB3_GAME_INPUT="@10:start,@30:start"`, and
the dc3 backend on desktop. The hub cast is the same in both: player0 male,
player1 female, player2 male, player3 female.

| instrument | before | after |
|---|---|---|
| `[SKIN_MAT]` rows for drawn skin meshes, colorpalettes excluded, with `own=1` | 0 / 63 | 63 / 63 |
| distinct `head.mesh` materials across the 4 members | 1 | 4 |
| head diffuse, player1 / player3 (female) | `male_head_diff.tex` | `female_head_diff.tex` |
| head spec, player0 / player2 (male), at the end of loading | `female_head_spec.tex` (shared, last writer) | `male_head_spec.tex` |
| torso / legs / feet skin diffuse | `dummy_*.tex` | `<gender>_torso_diff.tex`, `<gender>_legs_diff.tex` |

The head-spec row reads the shared material's state after the last member
loaded. Printed per call, the before leg shows each member's own gender
momentarily, which is overwritten by the next member.

Screenshots (scratch paths, not committed):
- `~/tmp/w16rp_cast_wide_full_f580.png`, and its 2x crop
  `~/tmp/w16rp_cast_wide_f580.png`: frame 580, wide shot, before next to
  after. Before, the woman on the left and the man in the visor have the same
  skin tone, and her face uses the male head texture. After, she has her own
  darker tone and the female head, and the man's face and bare arm have his
  lighter tone.
- `~/tmp/w16rp_before_after_f500.png` and `~/tmp/w16rp_heads_f580.png`: the
  close-up shot from the final binary. The male faces change little. Neck and
  chest skin show the torso detail texture instead of the flat dummy.
- `~/tmp/w16rp_before_after_f580.png`: the full frames for that pair.

The wide-shot pair comes from the first build of the fix, which differs from
the committed one only in the probe's name and in comments. Hub camera timing
is not frame-exact between runs (section 16.3). The final binary's frame 580
landed on the close-up in both legs.

### 17.4 Consumer verification (engine `w16-rp`)

| consumer | instrument | result |
|---|---|---|
| rb3 `w16-rp`, desktop, dc3 (`MILO_ENGINE_PATH` = engine `w16-rp`) | `ctest` | 125 tests: 118 passed, 7 skipped (the same fixture-gated set as 16.4), 0 failed, rc=0 |
| same | `native_compat_census.py check` (also run by ctest) | rc=0, 432 flags, regen clean |

16.4's 123 tests became 125 when W16-RN added the two census tests.

### 17.5 Not done

- **The rb3 flavor (BandRnd)** was not built or run. The fix is in game code
  both flavors compile, so its cast also changes, from shared materials to
  per-member ones.
- **The web build** was not rebuilt.
- **The native gate** (rb3-xenon's `tools/native_build_gate.sh`) was not run.
  rb3-xenon does not compile rb3's `bandobj`.
- **`RB3_SKIN_RTT=1`**, the composite path, was not exercised. Its MatSwap
  materials are in `sToDir`, so `ReplaceRefs` repoints them too, as in
  retail. The RT rebind comment in `SetSkinTextures` describes the split
  before this fix.
- **`RB3_HANDS_BINDFIX=1`** exempts char_shared from the shim. Retail's
  `Filter` branch then runs and the adoption does not, because `overrode` is
  false. This was not run.
- **colorpalettes.milo's `head_naked.mat`** (the texblender copy) is still one
  instance for all members.

### 17.6 Other flavors and web (lane W16-RR, 2026-10-06)

rb3 master `e90f93e6d` with the engine at `bbbf9c1` (the 16 and 17 fixes),
measured with the probe `RB3_SKIN_MAT_ADOPT_PROBE`; "before" sets all three
opt-outs (`RB3_NO_OUTFIT_CHAIN_ANCHOR`, `RB3_NO_SKIN_MAT_ADOPT`,
`RB3_NO_SKIN_MAPS`). No code change was needed.

| combination | test | own-material skin meshes, before / after |
|---|---|---|
| desktop, rb3 flavor | `ctest` 125: 118 passed, 7 skipped, 0 failed | 0/66 / 66/66 |
| web, dc3 (`build.sh --release`) | smoke PASS, 0 WebGPU errors, 73.31% painted | 0/63 / 65/65 |
| web, rb3 flavor | smoke PASS, 0 WebGPU errors, 86.45% painted | 0/65 / 66/66 |

All four members have hair in every "after" frame. On web dc3 the "before"
frame also shows the white specular hot spots of 16; they are gone after.
A web smoke run from a `~/tmp` worktree needs the four asset directories
passed explicitly (`--assets-dir`, `--assets-fallback`, `--sidecar-dir`,
`--downscale-dir`): otherwise the intro video 404s and boot stalls before the
splash screen. 15.5 noted the same path problem for sound files only.

## 18. Depth of field, from retail (lane W16-RO, 2026-10-06)

Section 14.3 left one DoF question open: the depth texture was bound as
`UnfilterableFloat` and sampled through a filtering sampler. It was a real
validation error, and the pass around it was not retail's, so `gfx/DofPass` was
rewritten from the retail shaders.

| repo | branch | commit |
|---|---|---|
| milo-native-engine | `w16-ro` | this commit (on `f99c1f2`) |
| rb3 | `w16-ro` | `89b6e0804` (on `f71e9e036`) |

### 18.1 Nothing could reach the pass

RB3's base `DOFProc` ignores `Set` and returns `Enabled()` false, and the Wii
`WiiDOFProc::Set` is empty. `CameraManager::Init` creates `TheDOFProc` from the
base factory, so under the dc3 flavor `DofPass::Run` returned on its first line
every frame. In retail Xbox gameplay DoF is common: `CameraShot` sets it every
frame for any shot with a focal target (`mUseDepthOfField` defaults true,
`max_blur` 1).

rb3 now gives the dc3 flavor a `DOFProc` that keeps what retail
`NgDOFProc::Set` (`0x82B8BF78`) keeps: focal plane, blur depth and blur range
after `RndPostProc::DOFOverrides`, and `Enabled = maxBlur > 0`. `DOFProc::Init`
reaches it through a weak `RB3RegisterNativeDOFProc()`, called after
`Rnd::PreInit` registers the base class. Targets without the hook keep the base
class.

### 18.2 The old pass, with DoF reachable

The rb3 change built against the unmodified engine, Quickplay
(`venue_capture.py`, `RB3_FIXED_CLOCK=1`):

| error | count |
|---|---|
| `Texture binding (group:0, binding:1) is TextureSampleType::UnfilterableFloat but used statically with a sampler (group:0, binding:2) that's SamplerBindingType::Filtering`, at `CreateRenderPipeline` (`fs_dof`) | 1 |
| `Multiple aspects (Depth\|Stencil) selected in [TextureView "defaulted from [Texture "DepthStencil"]"]`, the depth-resolve bind group | 2,395 |
| `[Invalid BindGroup]` | 2,395 |
| `[Invalid CommandBuffer from CommandEncoder "FrameEncoder"]` | 2,395 |

So it was not wrong sampling but three defects. The DoF pipeline was invalid.
The scene depth had no `TextureBinding` usage, and its default view selects
both aspects. A bad bind group invalidates the frame's single command buffer,
so every DoF frame submitted nothing: the four gameplay shots (game frames
60, 300, 600, 900) are byte-identical, the screen frozen on the last frame
before the first DoF shot. The title raised 0 errors; no title shot sets DoF.

### 18.3 What retail does

`NgDOFProc::DoPost` and the shaders it drives, read from `xbox_shaders` with
`tools/rb3-dc3-parity/xobx.py` and xenia's ucode disassembler:

1. **`downsample_4x`** (one permutation): four bilinear taps averaged (literal
   c255.x = 0.25) into the blur target, a quarter of the pre-process texture
   each way. Color only.
2. **`blur`**, permutation `0x1c000` (`SetNumTaps(8)`): `Σ c(47+i) ·
   tex(uv + c(31+i).xy)` over eight taps, no centre tap. `SetVHBlurWeights`
   loads every weight as 0.125 and the offsets as `tap · w·0.666·s/204800 · 5`
   (x) and `tap · h·0.666·s/64800 · 5` (y), where w × h is the blur target
   and s is `DOFOverrides().mBlurWidthScale`. That is 1.67 texels per unit tap
   at 320 × 180. The "horizontal" pass (blur A to B) and the "vertical" pass
   (B to A) use two different 2D tap tables.
3. **`postprocess`, bit `0x8`** (`ShaderMgr.unk26`, set by `DoPost`):

   ```
   tfetch2D r2, r0.xy, tf8           blur, linear, clamp
   tfetch2D r1, r0.xy, tf6           scene
   tfetch2D r0._x__, r0.xy, tf9      depth
   subsc  r0.x, c255.x, r0.y         c255.x = 1.0
   mad    r0.x, r0.x, c24.x, c24.y
   max    r0.x, |r0.x|, c24.z
   min    r0.x, r0.x, c24.w
   add    r2, r2, -r1  + maxs_sat r0.x, |r0.x|
   mad_sat oC0, r2, r0.x, r1
   ```

   c24 = (range, −scale·range, min(minBlur, maxBlur), maxBlur or 1 if
   negative), range = 1/(scale − bias). `scale` and `bias` are the projected
   depths, through the camera's ZRange, of the focal plane and of
   `focal · (1 − blurDepth)`, as `NgDOFProc::Set` computes them. Retail runs
   reverse-Z (`DxRnd::mReverseZ(1)`, `D3DCMP_GREATER`, `DxCam::ProjectZ`), so
   `1 − z` is the standard [0 near, 1 far] depth. The depth is read once per
   pixel at the texel centre of a same-size texture, so no filter applies to it.

The old pass had none of this. It linearised depth, used its own
`|depth − focal| / blurDepth` circle of confusion, and ran one full-resolution
Poisson pass.

### 18.4 The port

`gfx/DofPass.cpp`:

- Quarter-size A, B and a full-size output in the scene's format. Downsample
  is the exact 4×4 box (as `RB3RetailPost`'s `downsample_4x`). The two blurs use
  the retail tables and scales, with a linear, clamp sampler. The composite is
  the bit-`0x8` mix with `z` read as stored, since this renderer is standard-Z.
- The depth is a `texture_depth_multisampled_2d` read with `textureLoad`
  (sample 0, as a Xenos depth resolve keeps). No sampler touches it, so no
  filterable sample type is needed. The separate depth-resolve pass and its
  module are gone; the dc3 module table is 9 entries, not 10.
- `WgpuRnd::CreateDepthTexture` adds `TextureBinding` and a `DepthOnly` view,
  `mDepthSampleView`. The attachment view keeps both aspects.
- The pass no longer swaps the renderer's intermediate texture with its own.
  `PostProcPass` hands its output to the composite as the scene. Bloom still
  reads the sharp scene (`RB3RetailPost::Run` takes the bloom source
  separately), as retail's bloom reads the pre-process texture. Frames without
  DoF pass the same view for both.
- Each blur pass has its own uniform buffer: `Queue::WriteBuffer` lands at
  Submit, ahead of every pass in the frame.
- `scale`/`bias` are derived in the pass from `TheDOFProc`'s values and
  `RndCam::Current()`. A temporary probe showed the camera there is the camera
  `DOFProc::Set` received in 22 of 22 samples across a Quickplay run (ZRange
  0.1 to 1, near 10, far 10,000).

The tap tables (16 + 16 values) and the four scale constants were compared
mechanically with rb3-xenon's `rndobj/DOFProc_NG.cpp`: identical.

### 18.5 Measurements

**Validation.** Quickplay through `game_screen` and four gameplay offsets, twice
(the probe build, then the final build): 0 WebGPU errors each. Title: 0 on all
three legs.

**GPU test** (`rb3 native/tests/test_dof_pass.cpp`, dc3 flavor). This drives
`DofPass::Run` on the device at 1280 × 720 inside a validation error scope. It
compares the output with a CPU model of the chain (box, two 8-tap bilinear
blurs, the bit-`0x8` mix, 8-bit targets between passes):

| depth | f | max \|gpu − model\| (of 255) |
|---|---|---|
| focal plane (z = scale) | 0 | 0 |
| near plane (z = 0) | 1 (maxBlur) | 2 |
| half way (z = (scale + bias)/2) | 0.5 | 1 |

The tolerance is 3. Replacing `abs(t)` with `t` in the composite reads 185 and
92 and fails. A third case feeds the old depth+stencil view and requires the
scope to report the aspect error. The model shares `RetailBlurOffsets` and
`RetailConstants` with the pass, so the test checks the shaders against those
functions; the functions' constants are the ones compared with rb3-xenon above.

**Before / after.** Base is rb3 without the DOFProc (DoF never on); before is
the DOFProc on the old engine; after is the final build:

| leg | gameplay errors | the four gameplay shots |
|---|---|---|
| base | 0 | four distinct frames, no DoF |
| before | 7,186 | byte-identical: frozen |
| after | 0 | four distinct frames with DoF |

A same-run toggle (`/api/dta/eval {rnd set_dof_max_scale 0}`, which makes
`maxBlur` 0 and `Enabled()` false, a shot 4 to 6 frames later, then restore)
gives a DoF-on/DoF-off pair at each offset. Mean horizontal gradient, as a
sharpness figure, is lower with DoF on in all four pairs: 4.27 / 5.35,
10.58 / 11.93, 1.49 / 1.67, 2.84 / 3.40. By eye, near crowd silhouettes and
the venue behind the subject soften while the track stays sharp, consistent
with a retail 360 gameplay capture
(`rb3/images/retail-screenshots/yt_qRagnZCIMzk_gameplay_guitar.png`), whose
venue is heavily blurred behind the highway. That capture is a different song
and shot, so it supports the effect, not its exact strength. At offset 60 the
camera cut between the two shots, so that pair is not the same content.

Title, `title_fidelity.py` against TCRF, frame 400 (sky_dE / city_dE /
city_edge): base 11.4 / 11.8 / 0.910, before 11.5 / 11.8 / 0.912, after
11.4 / 11.9 / 0.910. These are within run-to-run noise, as expected for a
screen with no DoF.

Scratch paths (not committed): `~/tmp/w16ro/` (`venue-{base,before,probe,after}`,
`title-*`, `sheet-*.png`, the disassembly under `sh/`).

### 18.6 Consumer verification (engine `w16-ro`)

| consumer | instrument | result |
|---|---|---|
| rb3 `w16-ro`, desktop, dc3 (`-DMILO_ENGINE_PATH` at this worktree, confirmed in `CMakeCache.txt`) | `ctest` | 128 tests: 121 passed, 7 skipped (the seven of 16.4), 0 failed, rc=0. The three added are `DofPassTest.*`; base was 125 (123 + the two census tests) |
| rb3-xenon (DC3 shape) | its `native/build` compile commands, engine paths pointed at this worktree | `DofPass`, `PostProcPass`, `RB3RetailPost`, `BloomPass`, `Rnd_Wgpu`, `ShippedWgsl_DC3` compile with 0 errors; the object defines `DofPass::RetailConstants`, so the worktree source was compiled |

### 18.7 Not done

- **Retail's downsample taps.** Retail averages four bilinear taps at
  ±2·c15 (c15 is set outside the shader and was not traced). The port takes
  the exact 4×4 box, as `RB3RetailPost` does.
- **DoF with other post bits.** Only permutation `0x8` alone was read. How
  retail orders the DoF mix against chromatic aberration in one `postprocess`
  permutation was not examined. Here the aberration taps read the DoF image.
- **No pixel comparison with retail** at the same shot (no xenia gameplay
  capture of the same frame).
- **DC3 content and rb3-xenon at runtime.** Their `NgDOFProc` drives the same
  pass. rb3-xenon's native `Set` only enables on the game screen. Neither
  consumer was run; DoF frames there would have hit the same invalid command
  buffer before this change.
- rb3's rb3 flavor (no `DofPass`; the weak hook stays unset), the web build,
  rb3-xenon's native gate and dc3-decomp were not built or run.
- No merge, pin bump or push.
