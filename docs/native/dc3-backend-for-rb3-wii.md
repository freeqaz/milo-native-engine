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
  coordinator's call.
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
  or run under emscripten.
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
  suite; BandRnd's skinning workarounds. None of them was needed to match or
  beat BandRnd on the frames above; bloom is the most visible remaining gap
  against retail.
- **Crowd recolouring.** `Crowd.cpp` sets `kColorModModulate` with three random
  colours per crowd character; no backend implements `mColorMod`, and the Wii
  `WiiMat` decomp has no consumer of it to copy, so its semantics are unknown.
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
