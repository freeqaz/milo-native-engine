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
  *Resolved in section 19:* the lights are lit as retail lights them. The
  gap was the title's eight flares, which never drew.
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
is never painted. Opt-out: `RB3_NO_SKIN_MAPS=1`. (Section 23 removes the
substitution: the RT is now composed and bound as in retail.)

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
  The head uses the neutral `head00` normal. **Fixed in section 23.**
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

## 19. The middle city: eight flares that never drew (lane W16-RS, 2026-10-06)

Section 13.4 left the title's middle city (the theater roof and upper facade
under the logo) at about 40 luma with AO, against 52–55 in both references,
and named `theater.env`'s lights as the lead. Those lights are lit the way
retail lights them. The missing light was the title's eight `RndFlare`s, which
the native build never drew. The fix is in rb3, not the engine.

| repo | branch | commit |
|---|---|---|
| rb3 | `w16-rs` | `e6b31ba95` (on `169e1bb3a`) |
| milo-native-engine | `w16-rs` | this section only (on `806a8db`) |

Region figures use 13.2's regions (640×360; `city_mid` is
x 190–330, y 205–265). All title runs are `title_capture.sh` with
`RB3_FIXED_CLOCK=1`.

### 19.1 Where the region's light comes from

The `MILO_RB3_RETAIL_POST` inspection views (8.3) at f400, engine `806a8db`:

| f400 view | frame luma | `city_mid` |
|---|---|---|
| full chain | 43.6 | 40.2 |
| `raw` (no post) | 27.0 | 16.4 |
| `grade` (no bloom) | 26.6 | 16.3 |
| `bloom` (the term the screen blend adds) | 21.9 | 26.9 |

The grade does not change luma here. Bloom takes the region from 16 to 40, so
the region's brightness depends on bright sources nearby, not only on the
surfaces in it. The 1/64-size bloom set covers most of the screen, so
"nearby" is wide.

By eye (contrast-boosted crops), TCRF and xenia show a smooth warm haze over
the roof and the dark building to its left, with about the same texture
contrast as ours. A luma-difference map of TCRF minus the native frame
(box-blurred, 640×360) has four bright blobs, at about (90, 210), (211, 326),
(330, 250) and (450, 210). Section 19.3 places the four lamp flares at
those points.

### 19.2 What was checked and is not the cause

- **`theater.env`'s lights.** A temporary probe dumped every env's lights on
  the title:

  | light | list | colour | position (x, y, z) | range |
  |---|---|---|---|---|
  | `theater.lit` | real, point | 2.0, 1.435, 0.557 | −1386, 662, −841 | 500 |
  | `theaterred` | approx, point | 1.189, 0, 0 | −1297, 946, −1019 | 700 |
  | `theater02` | approx, point | 0, 0.620, 0.878 | −1503, 1213, −1083 | 1500 |
  | `theaterpurp` | approx, point | 0.396, 0, 0.675 | −1350, 1900, −941 | 1000 |
  | `theaterwhite` | approx, point | 0.902, 0.569, 0 | −1297, 2036, −946 | 800 |

  All falloff starts are 0 and the env's ambient is 0. `theaterroof.mesh`'s
  world sphere is centred at (−1617, 968, −437), radius 410. `theater.lit` is
  557 from that centre (range 500) and 400 below it, so the roof's upward
  faces get almost none of it. All four approx lights sit below the roof.
  At the centre, `theaterwhite` (1,225 away) and `theaterpurp` (1,092) are
  out of range. `theaterred` contributes 5%. `theater02` lands mostly on the
  box's −Z face.
  The engine picks the real lights as retail `NgEnviron::Select` does (the
  first two lit point lights in list order; `ReclassifyLights` only migrates
  the legacy list). The approx lights go through `BoxMapLighting` at the
  sphere centre, as `UpdateApproxLighting` does. Under retail's own rules the
  roof is barely lit by these lights. No light ablation was run.
- **Bloom parameters.** `sv8_a.milo_xbox` (the title city) holds one
  postproc, `drop_fade.pp`. A probe in `FillRetailPost` read intensity 1.5,
  colour (0.973, 0.961, 0.961), threshold 0.1, no glare or streak, for the
  whole run. The engine's blur weights and offsets equal retail
  `SetBloomBlurWeights`'s, and the three sets are ¼, 1/16 and 1/64 of the
  pre-process texture, as in `BloomTextures::AllocateTextures`.
- **The bloom mask.** Retail permutation `standard_0000014000430090` (diffuse,
  glow, real and approx lights, pseudo-HDR, AO, one point light, vertex-lit)
  disassembles to

  ```
  mad r2.xyz_, r0.xyzz, r2.xyzz, r3.xyzz     ; texel * lit + additive
  mad r2.xyz_, r1.xyzz, c5.xxxx, r2.xyzz     ; + emissive * c5.x
  dp3 r2.___w, r2.zxyy, c7.zxyy              ; alpha = dot(rgb, c7)
  max oC0, r2, r2
  ```

  Retail takes the dot of the unclamped colour; the engine takes it of the
  clamped one. That differs only where a channel is above 1, and both
  saturate alpha at 1. Not changed. (Its vertex shader does not disassemble
  with xenia's tool, as 11.1 found for other glow permutations.)
- **Haze particles.** The particle path scales translucent "haze" systems'
  alpha by 0.35 and fades them near the camera (7.1 #6, BandRnd's tuning). An
  env switch on that factor, one binary: `city_mid` reads 40.2 with the
  factor at 0, 40.3 at 0.35 (shipped), 40.3 at 1.0 and 40.3 at 1.0 with no
  fade. Haze is not the cause, and the factor is unchanged.

### 19.3 Cause: point-tested flares never drew

`sv8_a` carries eight `RndFlare`s:

- `Flare_lamp01`–`04`, material `flare_lamp01.mat`, texture
  `flare_light_can_star.tex`;
- `Flare_red_blink`, `_blink01`, `_blink02` and `_blink_slow`.

`RndFlare::DrawShowing` (the Wii source and the 360 one agree here) scales
the flare by `step/steps`. When `mAreaTest` is set, it also multiplies by
`mOcclusionResult / (rect.w · rect.h)`, the field rb3's header calls `unkec`.
`mAreaTest` is not loaded: the constructor sets it, so it is always on. The
flare assigns that field itself only when it does not point-test. For a
point-tested flare it comes from retail `DxRnd::DoPointTests` (rb3-xenon
`rnddx9/Rnd_Xbox.cpp`), which draws the flare's rect as an occlusion query
and stores the visible pixel count (`SetOcclusionResult`).

rb3's native `Rnd::TestPoint` stood in for the query with `SetVisible(true)`
alone. Its comment says to treat in-frustum flares as fully visible, but it
never stored the area, so the ratio was 0. A temporary probe in
`DrawShowing` logged `ratio=0.000 unkec=0.0` for all eight flares on every
logged frame (frames 1–53, 50 lines each).

With the area stored, the same probe gives ratio 1.000 for all eight, at
these positions (1280×720 frame):

| flare | screen x, y | rect | 640×360 point |
|---|---|---|---|
| `Flare_lamp01` | 0.330, 0.905 | 128×128 | (211, 326), the marquee's left end |
| `Flare_lamp02` | 0.545, 0.736 | 128×128 | (349, 265), beside the theater |
| `Flare_lamp03` | 0.146, 0.608 | 64×64 | (93, 219) |
| `Flare_lamp04` | 0.707, 0.617 | 128×128 | (452, 222) |
| red blinks | 0.13–0.19, 0.23; 0.76, 0.20 | 32×32, 64×64 | tower tops in the sky band |

These are the four blobs of the TCRF difference map (19.1). TCRF also shows a
glow on the left tower top and the skyscraper top, where the red blinks are.

### 19.4 Change

rb3 `e6b31ba95`, `src/system/rndobj/Rnd.cpp`, inside `#ifdef HX_NATIVE`.
`TestPoint` now also stores the on-screen area of the flare's rect: `mArea`
from the `CalcRect` that `DrawShowing` ran just before, clipped to the
screen. That is what retail's area query returns for an unoccluded flare.
The Wii build is unchanged. The engine is unchanged.

Before the change I expected `city_mid` to rise by several luma, with the
largest gains at the four blob positions and little change in the sky.

### 19.5 Title, before and after

Base and fix binaries were built from one rb3 worktree, with the change
reverted for the base. Both use engine `806a8db`. Two runs per leg; the runs
agree to 0.2 or better on every figure. `title_fidelity.py` against TCRF
(sky_dE / city_dE / city_edge):

| frame | base | **fix** |
|---|---|---|
| 60 | 13.4 / 11.8 / 0.908 | 14.2 / **10.3 / 0.924** |
| 200 | 14.8 / 13.2 / 0.870 | 15.3 / **12.3 / 0.886** |
| 400 | 11.4 / 11.8 / 0.913 | 11.9 / **10.3 / 0.926** |

| f400 | luma | city luma | p10 | dark % |
|---|---|---|---|---|
| retail (TCRF) | 50.1 | 52.4 | 11.8 | 25.7 |
| base | 43.6 | 43.1 | 13.0 | 28.0 |
| **fix** | **50.1** | **52.1** | 15.3 | 20.6 |

Regions, f400:

| region | TCRF | xenia | base | **fix** |
|---|---|---|---|---|
| sky, left | 22.1 | 33.6 | 22.5 | 22.7 |
| sky, right | 50.5 | 29.9 | 42.4 | 43.5 |
| city, right | 40.4 | 41.0 | 42.0 | 43.1 |
| **city, middle** | **55.0** | **51.9** | **40.2** | **49.6** |
| city, right-middle | 66.4 | 39.0 | 39.4 | 61.2 |
| left roof | 12.1 | 29.1 | 13.7 | 15.0 |

`city_mid` split into 3×4 cells (20×35 px each, rows top to bottom):

| | row 1 | row 2 | row 3 |
|---|---|---|---|
| TCRF | 32.2 24.8 36.8 44.5 | 26.3 47.7 62.7 77.1 | 35.9 62.5 108.4 101.6 |
| base | 29.1 18.0 26.2 31.0 | 20.7 37.9 46.1 53.1 | 25.2 45.2 85.4 65.1 |
| **fix** | 32.8 22.2 31.4 40.1 | 25.5 42.7 52.8 72.8 | 33.0 52.6 92.5 97.2 |
| xenia | 48.7 29.8 37.7 41.3 | 36.9 49.7 54.4 59.4 | 44.5 60.3 93.4 66.9 |

Mean |Δluma| against TCRF, box-blurred, 640×360: whole frame 13.6 → 12.6,
city 13.5 → 11.9, `city_mid` 14.8 → 6.1.

- **The middle city moves 40.2 → 49.6.** The gap to TCRF goes from 14.8 to
  5.4, and to xenia from 11.7 to 2.3. Cell by cell, the region now has
  TCRF's shape, brightest at the lower right toward `Flare_lamp02`.
- **The city improves at every frame**: city_dE by 0.8 to 1.5, city_edge by
  0.013 to 0.016. Frame luma now equals TCRF's (50.1). City luma is 52.1
  against 52.4.
- **Costs.** sky_dE is 0.4 to 0.8 worse. The increase is in columns
  x ≈ 320–560 of the lower sky rows: lamp04's halo and the slow red blink add
  1.0–1.6 luma to a sky whose hue already differs from TCRF (grey-green
  against purple). TCRF's flares there are, if anything, brighter than ours.
  Shadows lift: p10 goes 13.0 → 15.3 (TCRF 11.8) and dark % goes 28.0 → 20.6
  (TCRF 25.7). Dark % moves from 2.3 above TCRF to 5.1 below it.
- City, right moves 1.1 away from both references, which agree with each
  other there.

**The 360 flare colour.** The 360 `DrawShowing` sets the material colour to
`alpha`; the Wii source, which rb3 builds, sets `alpha · 0.6`. With 1.0 (env
switch, one run, f400): `city_mid` 52.1, right-middle 69.6, frame luma
52.3, city luma 55.0, and sky_dE / city_dE / city_edge 12.0 / 12.0 / 0.909.
Against the 0.6 build, the middle region gets closer to TCRF and every other
city figure gets worse. By eye the flares are over-bright at 1.0. Not
adopted; see 19.8.

### 19.6 xenia is not a reference for flares

xenia answers every occlusion query with a fixed sample count
(`query_occlusion_fake_sample_count`, default 1000;
`xenia/gpu/command_processor.cc`). A 128×128 lamp flare reads
1000 / 16384 ≈ 0.06 of its strength there, a 64×64 one 0.24, and a 32×32 red
blink about 1. So xenia's front buffer (10.2) has the lamp flares at about 6%
of their strength. That is why the region ratio against xenia
(`city_lum/x`) moves away, 0.94 → 1.19, while TCRF, a hardware capture,
agrees. It is mostly `city_rmid`, where xenia reads 39.0 and TCRF 66.4. The
same applies to any xenia comparison near a flare.

It also means xenia's 51.9 in `city_mid` comes from something other than
flares. The cell table shows xenia brighter than TCRF on the region's left
(48.7 vs 32.2) and dimmer at the lamp corner (66.9 vs 101.6). That is
consistent with its missing flares and the hub's different camera (13.2).

### 19.7 Venues and the hub

The change applies wherever a flare point-tests, so it was also checked away
from the title. A temporary probe in `TestPoint` counted the flares it
answered on each frame. A second probe zeroed the stored area whenever a
marker file existed, so flares could be switched off and on in one running
process.

- **Flares answered by frame.** In a `venue_capture.py` run, `TestPoint`
  answered 8 flares per frame up to about frame 30 (the title), then 2 large
  ones (about 195,000 px each, the hub) up to frame 240, and none after that.
  Gameplay began at about frame 1,676. **No flare was answered in any
  gameplay frame of these runs**, so the gameplay venue frames they captured
  do not depend on this change. The venue files still carry flares (`.flare`
  name references: arena 1,443, big_club 68, small_club 10, festival 4).
  Whether those point-test in other shots or songs was not checked.
- **Pooled venue luma does not answer the question.** Two runs per leg gave
  59.2 / 68.9 (base) and 44.7 / 56.8 (fix). The two legs never captured the
  same shot: the closest pair of frames still differs by 17.7 mean |ΔRGB|. So
  the gap is shot selection, not the change, which (above) touched no
  gameplay frame.
- **Hub, flares off and on in one process** (`screen_toggle.py`, frames
  100–300; the probe counted 2 flares at frames 90–120 and 1 at 150 and 300).
  On minus off, frame luma: +1.8 (f100), −0.7 (f130), +11.5 (f160),
  +0.1 (f200), −3.4 (f240), +2.3 (f300). Each pair is about five frames apart
  and the hub camera is moving: 7–30% of pixels differ by more than 20 in both
  directions. The pairs are therefore dominated by animation and inconclusive
  as numbers. Side by side (f100 and f160), the f160 legs show different
  camera positions, so the +11.5 is not flare light. Neither pair shows a
  flare drawn over geometry or a blown-out blob, and the lamp glows look the
  same in both legs.

### 19.8 Not done

- **The 360 flare colour factor** (`alpha` against the Wii source's
  `alpha · 0.6`, 19.5). rb3 builds the Wii source and keeps 0.6. On the
  title, 1.0 trades the middle region against every other city figure.
- **Occlusion.** Native has no occlusion query. Every flare whose point test
  passes is treated as fully visible, including one behind geometry that
  retail's query would have dimmed or hidden. That matches what the native
  path already did for visibility; it only now applies to brightness as well.
  **Done for the rb3 consumer on the dc3 backend in section 20 (W16-RU).**
- **The bloom mask's dot product** is taken of the clamped colour, not the
  unclamped one as retail does (19.2).
- **The rest of the title gap.** TCRF's greenish street haze and the
  star-shaped flare highlights, and the sky hue (grey-green against purple),
  are untouched. `city_mid` is 49.6 against TCRF's 55.0.
- **Other consumers.** BandRnd (the rb3 flavor) also runs this `Rnd.cpp` and
  was not built or run. rb3-web, rb3-xenon and dc3-decomp were not run.
- **No merge, pin bump or push.** That is for the coordinator.

### 19.9 Consumer verification

- rb3 `native/build-native` was configured with
  `MILO_ENGINE_PATH=/home/free/tmp/wt-w16rs-eng` (read back from
  `CMakeCache.txt`), dc3 GPU backend, all targets built.
- `ctest`: **100% tests passed out of 128**, 7 skipped (the same seven
  fixture-gated tests as 16.4 and 18.6), rc=0.
- The `rb3-native` binary rebuilt after the probes were removed is
  byte-identical (`cmp`) to the `fix` binary measured in 19.5.
- The change is inside `#ifdef HX_NATIVE`, so the Wii match build compiles
  the same code as before.

## 20. Flare occlusion queries, from retail (lane W16-RU, 2026-10-06)

Section 19 made the title's flares draw, but with no occlusion test: native
`Rnd::TestPoint` treated every in-frustum flare as visible and unoccluded
(19.8). A flare behind geometry therefore drew at full strength. This section
adds retail's point test to the dc3 backend as WebGPU occlusion queries, and
rb3's `TestPoint` now uses it.

| repo | branch | commit |
|---|---|---|
| milo-native-engine | `w16-ru` | `f40a970` (on `a78f844`), and this section |
| rb3 | `w16-ru` | `104d2ab0a` (on `48f4f734f`) |

### 20.1 What retail does

`DxRnd::DoPointTests` (rb3-xenon `rnddx9/Rnd_Xbox.cpp`) runs from
`DoWorldEnd`, after the world and before post-processing.

1. It blocks on the previous frame's fence and reads back the queries it
   issued then. The point query gives `flare->SetVisible(samples != 0)`; the
   area query gives `flare->SetOcclusionResult(samples)`.
2. It issues this frame's queries for every test `Rnd::TestPoint` queued: a
   one-pixel point at the flare's screen position and the flare's `mArea`
   rect. Both are drawn at the point's projected depth (`ProjectZ`), with
   depth test LESS, depth and colour writes off and the viewport disabled.

`RndFlare::DrawShowing` multiplies the flare's strength by
`mOcclusionResult / (rect.w · rect.h)` (always on, 19.3), and a flare whose
point test fails fades out. The answer a flare draws with is therefore one
frame old.

### 20.2 Change

**The seam** (`src/platform/PointTestHook.h`, always built, no Milo or WebGPU
types):

- `NativePointTester` is implemented by a backend with occlusion queries.
  `QueuePointTest(NativePointTest)` returns false when it cannot test now,
  and the consumer then uses its own fallback. `CancelPointTests(key)`
  guarantees that no answer for that key is delivered afterwards.
- `NativePointTest` holds the flare (an opaque key), the tested world point,
  its 0..1 screen position, `mArea` in the consumer's screen pixels and those
  pixels' size, and the point/area flags.
- The consumer registers a `NativePointTestResultFn`. It receives
  `{key, pointDone, visible, areaDone, area}`, with `area` in the consumer's
  pixels.
- `Set/GetNativePointTester` and `Set/GetNativePointTestResultFn` hold the
  two pointers. With no tester registered (headless builds, the rb3 BandRnd
  flavor) the consumer keeps its fallback.

**The pass** (`src/gfx/PointTestPass.{h,cpp}`, rndobj-free, WGSL
`vs_point_test` in the dc3 shipped-WGSL table):

- `Record` draws a batch as one render pass with no colour attachments over
  the frame's depth target (`Depth24PlusStencil8`, 4x MSAA). It loads and
  keeps the depth, and attaches an occlusion `QuerySet` with two queries per
  test.
- The draws are a point list and a 4-vertex strip from a per-batch vertex
  buffer. The pipelines have a vertex stage only, depth compare `Less`, depth
  write off and stencil masks 0. The viewport is the whole target with depth
  range 0..1, and the vertices carry window depth directly.
- It then resolves the query set and copies it to a `MapRead` buffer.
  `Submitted()` maps that buffer after the frame's submit.
- `Collect(wait)` delivers finished batches oldest first. With `wait` it
  blocks in `Instance::WaitAny` for up to 1 s per batch, as retail blocks on
  the fence; without it (`__EMSCRIPTEN__`) it calls `ProcessEvents` and takes
  what is ready.
- Counts are in samples, so `area = samples / sampleCount · areaScale`.
  `areaScale` converts target pixels back to the consumer's pixels.
- At most 3 batches are in flight. `Cancel(key)` nulls the key in every
  in-flight batch, and `Terminate` unmaps before freeing.

**The backend** (`src/platform/Rnd_Wgpu.{h,cpp}`):

- `QueuePointTest` computes the window depth the way the scene draws:
  - `CamSceneMatrices` is factored out of `WriteSceneUniforms` (a pure
    refactor, still used there);
  - the result goes through the camera's z range:
    `z = zr.x + ndcZ · (zr.y − zr.x)`, as `ApplyCameraViewport` sets it.

  The point is placed at `floor(screenX · w) + 0.5`, matching retail's
  `(int)(x · width)`, and the rect is scaled from the consumer's pixels to
  the target's.
- `RunPointTests` runs once per frame:
  1. collect the previous frames' answers;
  2. if tests are queued and the frame (not a render target) is being drawn,
     end the active pass, record the batch against `mDepthView`, and resume
     the frame pass.
- It is called from `WgpuRndBase::DoWorldEnd` (new override, RB3-Wii shape;
  retail's order is `Rnd::DoWorldEnd`, then `DoPointTests`). `EndDrawing`
  also calls it as a fallback for frames that never end a world, and on the
  DC3 shape it is a no-op with an empty queue.
- `BeginDrawing` drops anything queued but never recorded.

**The consumer** (rb3 `src/system/rndobj/Rnd.cpp`, `#ifdef HX_NATIVE`):

- `TestPoint` fills a `NativePointTest` from the flare (`mArea`, `mPointTest`,
  `mAreaTest`, `Width()`/`Height()`) and queues it, keeping W16-RS's
  "visible, clipped area" fallback when there is no tester or it declines.
- The result handler mirrors retail: `SetVisible(visible)` for an answered
  point, and the area into the occlusion-result field (`unkec`) for an
  answered area.
- `RemovePointTest` also cancels the flare's tests, so a deleted flare is
  never answered.

Before the change I expected:
- flares wholly behind geometry to stop drawing;
- partly covered ones to dim in proportion to their hidden area;
- unoccluded ones (`Flare_lamp01`, `Flare_red_blink01`) to keep their W16-RS
  brightness;
- frame luma to drop slightly, since 19.5 found TCRF's flares about as bright
  as ours.

### 20.3 What the queries answer on the title

A temporary probe (removed before commit) logged each flare's area ratio
(`unkec / (rect.w · rect.h)`) and its point answer. The values were constant
from frame 30 to 400:

| flare | ratio | point | what covers it |
|---|---|---|---|
| `Flare_lamp01` | 1.000 (16383 / 16384) | visible | nothing |
| `Flare_red_blink01` | 1.000 | visible | nothing |
| `Flare_lamp02` | 0.880 | visible | the theater's edge |
| `Flare_lamp03` | 0.875 | visible | a building edge |
| `Flare_blink_slow` | 0.813 | visible | a tower edge |
| `Flare_lamp04` | 0.409 | visible | the roof in front of it |
| `Flare_red_blink02` | 0.150 | **hidden** | the 3D logo |
| `Flare_red_blink` | 0 | **hidden** | the 3D logo |

The two point-hidden blinks sit behind the logo's left glyphs. With the point
hidden, `DrawShowing` fades them out.

### 20.4 Title, before and after

Base: rb3 `48f4f734f` (W16-RS), built against this engine worktree before the
change. Fix: `104d2ab0a` + `f40a970`. Both use `title_capture.sh` with
`RB3_FIXED_CLOCK=1`, frames 60/200/400, two runs per leg. The final
`rb3-native`, rebuilt after the probe was removed, is byte-identical (`cmp`)
to the measured fix binary.

Mean luma in each flare's rect (1280×720, box blur 2, mean of both runs):

| flare (ratio) | f60 base → fix | f200 | f400 |
|---|---|---|---|
| `lamp01` (1.000) | 149.1 → 148.4 | 148.7 → 148.1 | 148.7 → 148.8 |
| `red_blink01` (1.000) | 41.3 → 40.6 | 40.4 → 40.7 | 42.9 → 43.8 |
| `lamp02` (0.880) | 163.8 → 160.0 | 162.3 → 159.4 | 163.0 → 159.1 |
| `lamp03` (0.875) | 90.1 → 86.2 | 93.6 → 89.7 | 91.0 → 87.2 |
| `blink_slow` (0.813) | 81.2 → 79.4 | 58.2 → 57.8 | 58.9 → 57.9 |
| **`lamp04` (0.409)** | **82.2 → 57.6** | **81.5 → 57.2** | **81.7 → 57.3** |
| **`red_blink02` (hidden)** | **66.7 → 60.2** | **66.4 → 60.1** | **62.0 → 60.2** |
| **`red_blink` (hidden)** | **40.8 → 36.4** | **42.0 → 38.0** | **36.4 → 35.3** |

- The unoccluded flares are unchanged. They move by 0.9 or less, within the
  run-to-run spread: the two base runs alone differ by up to 1.8 at
  `lamp01`.
- The hidden blinks lose their glow. The amplified difference image (below)
  puts all the removed light inside their two rects.
- `lamp04` loses 30% of its rect's luma, the largest change on the screen.
  The partly covered lamps lose about 4.

`title_fidelity.py` against TCRF (sky_dE / city_dE / city_edge, both runs):

| frame | base | **fix** |
|---|---|---|
| 60 | 14.2–14.3 / 10.3–10.4 / 0.924–0.927 | 14.0–14.1 / **9.5–9.7 / 0.933–0.934** |
| 200 | 15.3 / 12.2–12.3 / 0.884 | 15.2–15.3 / **11.5–11.6 / 0.887–0.889** |
| 400 | 11.8 / 10.1–10.2 / 0.925 | 11.7 / **9.4 / 0.932** |

| f400 (mean of runs) | frame luma | city luma | `city_mid` |
|---|---|---|---|
| retail (TCRF) | 50.1 | 52.4 | 55.0 |
| base (W16-RS) | 50.1 | 52.0 | 49.6 |
| **fix** | 49.15 | 50.75 | 49.0 |

- city_dE improves by 0.6–0.8 at every frame and city_edge by 0.003–0.009;
  sky_dE is unchanged or 0.1 better.
- Frame luma drops by 0.9 at every frame (51.92 → 51.01, 53.22 → 52.37,
  50.04 → 49.13; the runs agree to 0.24). It is now 1.0 below TCRF at f400,
  where W16-RS had matched it. `city_mid` keeps W16-RS's gain (49.6 → 49.0).
- **TCRF agrees on `lamp04`.** At 2x, TCRF shows a compact star with a weak
  glow at that point. The base showed a large bloom over the roof; the fix
  shows a small one, close to TCRF. TCRF also shows no glow at the two
  hidden blinks' rects.

Images (f200, mean of both runs; panels: before, after, TCRF, before − after
luma ×6):

- `~/tmp/w16ru/ab_blinks_f200.png`: the two hidden blinks (red rects) lose
  their glow, and the unoccluded `red_blink01` (green) does not change.
- `~/tmp/w16ru/ab_lamp04_f200.png`: `lamp04`'s bloom over the roof is gone.
- `~/tmp/w16ru/ab_lamp01_f200.png`: `lamp01`, unoccluded, does not change;
  its difference panel is flat.

**0 `WebGPU error` lines** in all six title runs, base and fix. The one
`device lost (reason 2): Device was destroyed` per run is shutdown, present
in every leg and in W16-RS's logs.

### 20.5 Tests

rb3 `native/tests/test_point_test.cpp` (dc3 backend only, in `rb3-tests`):

- `PointTestPassTest.CountsVisiblePixelsAgainstDepth`: a 128×64 4x
  `Depth24PlusStencil8` target, cleared to 1.0, with z = 0.5 written over the
  left half. Six 16×16 queries, checking that nothing is answered before
  submit:

  | case | expected |
  |---|---|
  | open, right half | area 256 |
  | behind | 0, hidden |
  | in front | 256 |
  | straddling the edge | 128 |
  | half off-screen, `areaScale` 0.25 | 32 |
  | coplanar (LESS fails) | 0 |

- `PointTestPassTest.CancelDropsInFlightAnswers`.
- `RndTestPoint.FlareBehindGeometryReadsZero`, the end-to-end test through
  rb3's real `Rnd::TestPoint`. Setup:
  - a camera, and a real `RndMesh` wall (named, so it is not drawn as a text
    mesh) covering x < 0;
  - three `RndFlare`s: behind the wall, beside it, and in front of it.

  Over two frames it checks:
  - nothing is answered in the first frame;
  - in the second, all three are answered, at `DoWorldEnd` (not the
    `EndDrawing` fallback), with 0 WebGPU errors;
  - the behind flare is hidden with area 0, and the other two are visible
    with area ≈ 64 (8×8).

  It runs both with and without a world end.
- `RndTestPoint.RemovedFlareIsNeverAnswered`: a flare deleted between frames
  gets no answer.

**Sabotage**, each applied to a copy and reverted (restored sources
byte-identical, all four tests pass again):

| sabotage | tests that went red (predicted = observed) |
|---|---|
| depth compare `Always` | CountsVisible, FlareBehind |
| `TestPoint` ignores the tester | FlareBehind, Removed |
| `DoWorldEnd` does not run the tests | FlareBehind |
| `RemovePointTest` does not cancel | Removed |
| area reported in samples | CountsVisible, Cancel, FlareBehind |

### 20.6 Not done

- **The rb3 BandRnd flavor** registers no tester, so it keeps W16-RS's
  "visible, unoccluded" fallback. It was not built or run.
- **The web.** The pass is in the dc3 flavor's shipped WGSL, and `Collect`
  does not block under `__EMSCRIPTEN__`, so answers arrive whenever the map
  finishes rather than exactly one frame late. rb3-web was not built or run.
- **The DC3 shape** (rb3-xenon, dc3-decomp) has no consumer calling the hook.
  rb3-xenon's `Rnd::TestPoint` keeps its own `HX_NATIVE` fallback, the same
  one W16-RS gave rb3, and could use this hook the same way. On that shape
  `RunPointTests` runs only from `EndDrawing`, with an empty queue. (Section 21
  moves the world-end step to `WgpuRnd`, so this shape gets it too.) rb3-xenon's native build was compiled against this worktree (20.7);
  dc3-decomp was not.
- **A flare with `mAreaTest` off.** Retail `DoPointTests`'s area-test else arm
  calls `SetVisible(true)` immediately, in the same frame; the native path
  does not mirror that arm. `mAreaTest` is always on in rb3 (19.3), so no
  rb3 flare reaches it.
- **Reverse Z.** Retail uses GREATER when `mReverseZ` is set; the native pass
  always uses LESS, which is the engine's depth convention.
- **Frame cost** was not measured. It is one depth-only pass and a 16-byte
  readback per test, plus a wait on the previous frame's map, which the
  frame's own submit has normally already passed. (Section 24 measures it,
  and moves that wait to after the submit.)
- **Venues and the hub** were not re-checked; 19.7 found no flare answered in
  any gameplay frame of those runs.
- **No merge, pin bump or push.** That is for the coordinator.

### 20.7 Consumer verification

- rb3 `native/build-native` was configured with
  `MILO_ENGINE_PATH=/home/free/tmp/wt-w16ru-eng` (read back from
  `CMakeCache.txt`) on the dc3 GPU backend, and all targets were built.
- `ctest`: **100% tests passed out of 132**, 7 skipped (the same seven
  fixture-gated tests as 19.9), rc=0. The four new tests account for 128 → 132.
- **The DC3 shape compiles.** rb3-xenon's native build (main checkout,
  read-only, out-of-tree build dir `~/tmp/w16ru/xenon-build`) was configured
  against this worktree with `MILO_ENGINE_RNDOBJ_SHAPE=dc3`. All 3,914 edges
  built, including `PointTestPass.cpp`, `PointTestHook.cpp` and
  `Rnd_Wgpu.cpp`: rc=0, 0 `error:` lines. It was not run.
- The rb3 change is inside `#ifdef HX_NATIVE`, so the Wii match build
  compiles the same code as before.

## 21. Flare point tests at world end on the NgRnd shape (lane W16-RX, 2026-10-07)

Section 20 ran the point tests from `WgpuRndBase::DoWorldEnd`, an override
that exists only on the RB3-Wii rndobj shape. On the NgRnd shape (rb3-xenon,
DC3) `WgpuRndBase` is `NgRnd` itself, so the override was compiled out and the
tests ran only from `EndDrawing`. By then the depth buffer also holds whatever
the frame drew after the world (UI, HUD), so a flare under a menu panel read
as hidden. rb3-xenon's W16-RW consumer was answered there (its gdb backtrace
showed `WgpuRnd::EndDrawing`).

| repo | branch | commit |
|---|---|---|
| milo-native-engine | `w16-rx` | `d51d304` (on `76a355a`), and this section |
| rb3-xenon | `w16-rx` | `eeef8d548` (on `2b280289f`) |

### 21.1 What retail does

Retail RB3 (rb3-xenon `rnddx9/Rnd_Xbox.cpp`) and DC3 order the step the same
way:

1. `Rnd::EndWorld` runs `DoWorldEnd`, then `DoPostProcess`. `WorldDir::
   DrawShowing` and `PanelDir` call it after the world's drawables;
   `Rnd::EndDrawing` calls it too, for a frame that never ended its world.
2. `DoWorldEnd` is virtual. `NgRnd` does not override it; `DxRnd`, the
   platform renderer, does:
   `if (mProcCmds & kProcessWorld) { Rnd::DoWorldEnd(); DoPointTests(); SavePreBuffer(); }`.

So the point tests belong to the platform renderer, after `Rnd::DoWorldEnd`
and before post-processing.

### 21.2 Change

- **Engine** (`src/platform/Rnd_Wgpu.{h,cpp}`,
  `src/platform/rndshape/RndShape_RB3Wii.h`): the override moves from the
  RB3-Wii `WgpuRndBase` to `WgpuRnd`, which stands in for `DxRnd` on both
  shapes. `WgpuRnd::DoWorldEnd` runs `WgpuRndBase::DoWorldEnd()` (that is,
  `Rnd::DoWorldEnd`), then `RunPointTests()`. There is one definition, and
  the RB3-Wii shape runs the same code as before.
  - `EndDrawing` still calls `RunPointTests` first, for frames that never
    end their world. `mPointTestsRan` keeps it to once per frame.
  - Retail's `kProcessWorld` check is left out. `WgpuRnd::BeginDrawing` does
    not run the proc counter, so `mProcCmds` stays `kProcessAll` and the
    check would always pass.
  - On DC3 (no consumer queues tests) the step collects nothing, and with an
    empty queue it returns before touching the render pass.
- **rb3-xenon** (`native/src/main_render.cpp`): `rb3-render` drew the world
  and went straight to `EndDrawing`, so no native rb3-xenon target ever ran
  `Rnd::EndWorld`. The cell now calls `TheRnd.EndWorld()` after the meshes,
  crowd and flares, as `WorldDir::DrawShowing` does.
  - It wraps the handler `Rnd::TestPoint` registers and counts the answers
    each step delivers, printing them per frame
    (`point tests: frame N answered A at the world end, B at EndDrawing`).
  - A new gate, `flare-tests-at-world-end`, requires every answer at the
    world end. It runs only when a cell has flares, ends its world, and
    draws at least two frames; the default cells have no flares, so
    `native_health`'s gate count does not change.
  - `RB3_NO_END_WORLD=1` restores the old flow.
  - `RB3_POST_WORLD_OCCLUDER=1` draws a full-frame quad 4× the near plane in
    front of the camera after the world end, standing in for UI. It covers
    the frame, so `image-not-empty` fails under it.

### 21.3 Results

`rb3-render <xbox-zip> <out> world/vignette/shell/gen/sv8_a.milo_xbox
--frames 8 --focus mesh`. Fix = engine `d51d304`; base = engine `76a355a`
(W16-RU, what rb3-xenon pins). Both legs use the same rb3-render source
(`eeef8d548`), built into separate dirs.

| leg | answers at world end / EndDrawing | `flare-tests-at-world-end` | flares visible | lamp04 ratio |
|---|---|---|---|---|
| base | 0 / 56 | **FAIL** | lamp04, lamp02 | 0.964 |
| **fix** | **56 / 0** | **PASS** | lamp04, lamp02 | 0.964 |
| base + occluder | 0 / 56 | FAIL | **none** | **0.000** |
| **fix + occluder** | **56 / 0** | PASS | lamp04, lamp02 | **0.964** |
| fix + occluder + `RB3_NO_END_WORLD` | 0 / 56 | (not run) | **none** | **0.000** |

- Frame 0 answers nothing (answers arrive one frame late); frames 1–7 answer
  all eight.
- **The occluder no longer hides anything.** With the fix, all eight answers
  under the occluder equal the no-occluder answers, value for value. With
  the base engine, or with the fix but no world end, the same occluder reads
  every flare hidden with area 0.
- The fix's eight answers equal W16-RW's `c_fix2` run value for value, and its
  `sv8_a.png` is byte-identical (`cmp`) to W16-RW's. On this cell nothing
  draws after the flares, so the depth at world end and at `EndDrawing` is
  the same.
- **Backtrace** (gdb, breakpoint on the counting wrapper, fix binary):
  `CountPointTestAnswer` ← `DeliverPointTestAnswer` ←
  `PointTestPass::Collect` ← `WgpuRnd::RunPointTests` ←
  **`WgpuRnd::DoWorldEnd`** ← **`Rnd::EndWorld`** ← `RenderCell`.

The world end itself changes no pixels:

- The default cells (`tracksystem_meshes`, `crowd_female01`) are
  byte-identical to W16-RW's `rb3-render`.
- With `--postproc post_process_fx_venue` (`intro_contrast_flame.pp`) the
  PNG is not reproducible on any binary: two W16-RW runs differ in 777,440
  pixels (max 13, mean 2.97). W16-RW vs fix differs by 782,294 pixels (max
  13, mean 3.02), and fix vs fix without world end by 780,295. Both are
  inside that run-to-run spread. The flare answers are identical in both
  legs.

### 21.4 Verification

| consumer | check | result |
|---|---|---|
| rb3-xenon `w16-rx` (`eeef8d548`), `native/build` with `MILO_ENGINE_PATH` = this worktree (read back from `CMakeCache.txt`) | `tools/native_build_gate.sh` | `NATIVE_GATE_RESULT verdict=PASS expected=18 verified=18 skipped=0 partial=0 failed=0 rc=0` |
| same | `tools/native_health.sh` | `NATIVE_HEALTH_RESULT verdict=PASS link=PASS link_verified=18 link_expected=18 link_skipped=0 runtime=PASS runtime_ran=18 runtime_total=18 gates_pass=77 gates_fail=0 unrunnable=none selftest=SKIPPED … rc=0`; 77 gates, as at W16-RU |
| rb3 `master` (`beb08ab14`), `native/build-native`, desktop, dc3 flavor, `MILO_ENGINE_PATH` = this worktree | `ctest` | **100% tests passed out of 132**, 7 skipped (the seven of 20.7), rc=0 |

- rb3's `RndTestPoint.FlareBehindGeometryReadsZero` passes. It checks that
  the answers arrive at `DoWorldEnd` on the RB3-Wii shape, so the moved
  override still does what 20.5 tested. It calls `TheRnd->DoWorldEnd()`, an
  `Rnd*`, which now dispatches to `WgpuRnd::DoWorldEnd`.
- The rb3 change is engine-only; rb3's source is untouched.

### 21.5 Not done

- **dc3-decomp** was not built or run. Its renderer shape is the one
  rb3-xenon compiles (`MILO_ENGINE_RNDOBJ_SHAPE=dc3`), and on DC3 the new step
  runs with an empty queue (see 21.2).
- **A real UI over flares** was not captured. The occluder stands in for it.
  rb3-xenon has no native target that draws a `PanelDir` over a venue.
- **SavePreBuffer** (the third step of retail's `DoWorldEnd`) still has no
  native counterpart. **Done in section 22 (W16-RZ).**
- **No merge, pin bump or push.** That is for the coordinator. rb3-xenon's pin
  (`76a355a`) predates `d51d304`; with that pin, `flare-tests-at-world-end`
  fails on any flare cell (base row in 21.3), so `eeef8d548` should land
  together with a pin bump.

## 22. SavePreBuffer and world refraction, from retail (lane W16-RZ, 2026-10-07)

Retail `DxRnd::DoWorldEnd` runs `Rnd::DoWorldEnd`, then the point tests
(section 21), then `SavePreBuffer`. Section 21.5 left `SavePreBuffer` without a
native counterpart. This section establishes what that buffer is and which
effects read it, then ports the one reader rb3 content reaches through the dc3
backend: world refraction.

| repo | branch | commit |
|---|---|---|
| milo-native-engine | `w16-rz` | `528f9a6` (on `1eadb7a`), and this section |
| rb3 | `w16-rz` | `802bdcdc4` (test only; on `e1dbd2a00`) |

### 22.1 What the buffer is

All references are rb3-xenon (retail-matched) `src/system/rnddx9/Rnd_Xbox.cpp`
unless noted.

- `SavePreBuffer` (l.616) makes two `D3DDevice_Resolve` calls out of EDRAM:
  - flags `0x14` (DEPTHSTENCIL | FRAGMENT0) resolve the world's **depth** into
    `mFrontBufferDepth`;
  - flags `0x300` (RT0 | CLEARRENDERTARGET | CLEARDEPTHSTENCIL) resolve the
    world's **colour** into `mPreProcessBuffer` (A8R8G8B8, screen size, l.932).
    They clear colour to the clear colour with alpha 0, and depth to 0.
- `CreatePostTextures` (l.962) wraps the two buffers as `PreProcessTexture()`
  and `PreDepthTexture()`. `SetFrameBuffersAsSource` (l.484) binds sampler 6
  to the pre colour, 9 to the pre depth and 14 to the post buffer.
- `GetCurrentFrameTex(bool resolve)` (l.669) returns `PreProcessTexture()`
  until `mPostProcDone`, then `PostProcessTexture()`. `mPostProcDone` is set
  at the end of `DoPostProcess` (l.295) and cleared in `EndDrawing` (l.764).
  The post buffer is filled by `SavePostBuffer` in `FinishPostProcess`. With
  `resolve` set, it would resolve the current EDRAM into the pre buffer
  first, but every retail caller passes `false`.

So the pre buffer is **the world, as it stands at world end, before
post-processing**: colour plus depth.

### 22.2 Who reads it

**From code** (all retail RB3; each is in `splits.txt` / `objects.json`):

| reader | reads | how |
|---|---|---|
| `NgPostProc` bloom / grade (`PostProc_NG.cpp`) | colour | `PreProcessTexture()` as the bloom source; already ported (sections 7 and 18) |
| `DOFProc_NG` | colour + depth | already ported (section 18), reading `mDepthSampleView` directly |
| `NgMat` refraction (`Mat_NG.cpp:331`) | colour | `GetCurrentFrameTex(false)` bound as PS texture 6 when `GetRefractEnabled(false)` |
| `RndSoftParticleBuffer::DoPost` | depth | `PreDepthTexture()` into sampler 9 |
| `RndVelocityBuffer` (motion blur, fed by `RndMotionBlur` → `RndPostProc::QueueMotionBlurObject`) | depth | `PreDepthTexture()` into sampler 9 |
| `SpotlightDrawer_NG` | size only | `PreProcessTexture()->Width()/Height()` |

**From shader microcode.** I disassembled all 5,670 xbox_shaders pixel-shader
permutations (`tools/rb3-dc3-parity/xobx.py` dump, then the xenia shader
compiler; 5,654 disassembled). These fetch from `tf6` (pre colour) or `tf9`
(pre depth):

| shader | `tf6` | `tf9` |
|---|---|---|
| postprocess | 1,343 | 672 |
| postproc_error | 2 | — |
| standard | **38, every one with option bit 46 `mRefractWorld`** | — |
| particles | 2 | 11 (soft depth) |
| velocity_camera, velocity_object | — | yes |
| depthvolume | — | 6 (not traced further) |

**Which of these rb3 content can reach on the dc3 backend:**

- RB3-Wii's `RndMotionBlur::DrawShowing` is empty, its `SoftParticles` drops
  its particle list, and its `GetCurrentFrameTex` returns 0. So on the RB3-Wii
  shape, soft particles and motion blur never draw, whatever the backend does.
- On rb3-xenon (NgRnd shape), soft particles are skipped because
  `NgRnd::PreDepthTexture()` is null natively.
- Bloom/grade and DoF are already ported and read their own copies.
- **That leaves refraction.** rb3's `RndMat` loads its fields
  (`rndobj/Mat.cpp:310`), and the engine can draw it.

### 22.3 Content census

`~/tmp/w16rz/scan_prebuffer.py` (scratch) inflates every milo and counts:
- directory entries of type `Mat`, `SoftParticles` and `MotionBlur`;
- for every chunk that starts with Mat rev 0x44, the refraction tail (u8
  enabled, f32 strength, string normal map; `BaseMaterial.cpp:436`).

It self-validates: the parsed rev-0x44 chunks match the `Mat` entries to
within one, with 0 unparsed.

| class | Xbox (`.milo_xbox`, 4,455 files) | Wii (`.milo_wii`, 4,506 files) |
|---|---|---|
| `Mat` entries / rev-0x44 tails parsed / unparsed | 19,285 / 19,286 / 0 | 18,422 / 18,423 / 0 |
| refraction enabled, strength > 0 | **184 in 122 files** | **178 in 116 files** |
| `MotionBlur` objects (crowd/extras `*.blur`) | 186 in 82 files | 176 in 78 files |
| `SoftParticles` objects (`SoftParticles.soft`) | 37 | 38 |

Where the refracting materials are:
- **Almost all are UI glass**: `ui/resource` 48, `ui/accomplishments` 20,
  `ui/tour` 13, `ui/overshell` 11, and others. Their normal maps are
  `header_song_bg_normal*.tex` and `song_bg_normal*.tex`, mostly at
  strength 20.
- **World ones**:
  - `world/vignette/shell/gen/sv8_a` (the title and hub city);
  - `sv2_a` and `world/vignette/transition/gen/tv1_a`
    (`recordplayer_lid_norm.tex`, strength 25);
  - on Xbox only, 15 `big_club` venues (`heat_distortion_norm.tex`,
    strength 100).
- 6 Xbox materials name no refract normal map. They rely on
  `GetRefractNormalMap`'s fallback to the material's normal map.

On the title, exactly **one** material takes the refraction path. A temporary
probe (not committed) printed
`city_road.mat strength=20 normal=city_road_norm.tex`. It has no diffuse map,
colour (1, 1, 1, 0.9), SrcAlpha blend, vertex lighting, and next pass
`city_road_diffuse.mat`, which alpha-blends `city_road.tex` over it at colour
0.6. About 6.6 road draws per frame, all before the frame's world end.

### 22.4 Retail refraction math

From the disassembly of `standard_0000400000000000` (option bit 46 alone) and
its variants (`…20000`, `…10`, `…400000`, `…4000000`):

```
n   = (tex1.g * 2 - 1, tex1.r * 2 - 1)               tf1 = refract normal map, material uv
uv  = (clip.xy + n * c119.w) * (0.5, -0.5) / clip.w + 0.5
                                                     c119 = kPS_RefractStrength (raw strength)
scr = tfetch(tf6, uv).rgb                            alpha forced to 1; linear, clamp
oC0.rgb = scr * [diffuse.rgb] * r3.rgb [+ r4]        r3 = the vertex shader's lit colour
```

- The literal constant is c255 = (0.5, −0.5, −1, 2).
- `Mat_NG.cpp:331` passes a literal `false` (`li r4, 0`), so the fetch reads
  the buffer the **last** world end saved. A refracting world surface
  therefore sees the previous frame's world, itself included.
- `Shader.cpp:718` sets bit 46 when `GetRefractEnabled(b) &&
  GetRefractNormalMap()`. `GetRefractEnabled` (`Mat.cpp:286`) is
  `mRefractEnabled && mRefractStrength > 0 && (mRefractNormalMap ?:
  mNormalMap)`.

### 22.5 Change

Engine `528f9a6`:

- **`WgpuRnd::SavePreBuffer`** runs last in `WgpuRnd::DoWorldEnd`, after
  `RunPointTests`. It ends the frame pass, copies the colour texture the frame
  pass writes (recorded by `BeginFramePass`: the post-processing intermediate,
  or the frame target) into a persistent `mPreTex`, and resumes the pass.
  - It saves once per frame. A frame that never ends its world saves at
    `EndDrawing`, as retail's `Rnd::EndDrawing` → `EndWorld` would.
  - The intermediate texture gains `CopySrc`. The headless and web frame
    textures already had it; a desktop surface without `CopySrc` simply
    skips the copy.
  - **Depth is not saved.** The engine draws no reader of it (22.2).
  - Retail's resolve also clears the EDRAM, because retail's post chain
    redraws the frame from the buffers. Here the frame pass keeps its
    contents, so nothing is cleared.
- **`RefractFrameView`** stands in for `GetCurrentFrameTex(false)`. It returns
  `mPostOutView` once this frame's `FlushWorldPost` has run (retail
  `mPostProcDone`, RB3-Wii shape only), otherwise the pre buffer, and black
  before the first save.
- **Material bind group, bindings 11–13**: the refract normal map, the frame,
  and a linear clamp sampler (`PipelineManager`, `CreateMaterialBindGroup`,
  and BandRnd's three bind-group builders, which bind placeholders).
- **`MaterialUniforms` stays 256 bytes.** The strength rides as an f16 in the
  high 16 bits of `colorMod.w`; the colour-mod mode keeps the low 16. Growing
  the struct to 272 bytes would have doubled every draw's ring slot to 512.
- **`rndshape::MatRefract`** (`RndShape.h` `RefractTerms`):
  - RB3-Wii: `mRefractEnabled && mRefractStrength > 0` and
    `mRefractNormalMap`, falling back to `mXbNormalMap` (the Xbox
    `GetRefractNormalMap`).
  - DC3 / rb3-xenon: the decomp's own `RndMat::GetRefractEnabled(true)`,
    `GetRefractStrength`, `GetRefractNormalMap`.
  - `MaterialSetup` fills it for the primary material and every next pass.
- **`standard_wgsl.inc` `refractScreen`**: the math of 22.4. The screen rgb
  multiplies the texel before intensify, lighting and fog. A linear-space
  (DC3) material decodes the 8-bit frame with `srgbToLinear` first; a
  gamma-space (RB3) one uses it as stored, as retail does.

### 22.6 Results

**Title** (`title_capture.sh`, frames 60/200/400, rb3 `w16-rz`, dc3 flavor).
Legs:
- before = engine `1eadb7a`;
- after = `528f9a6`.

Each binary ran twice, because the title is not frame-deterministic: two
before runs differ in 20–22% of pixels by more than 8. The rb3-native built
after removing the probes is byte-identical (`cmp`) to the measured "after"
binary.

By eye (`~/tmp/w16rz/street_tcrf_before_after.png`): the bright orange
asphalt the baseline drew between the Capitol theater and the parked cars is
gone. TCRF shows no orange asphalt there.

Street sub-regions, mean over 6 shots per leg, |dRGB| to TCRF (1920×1080
resized to 1280×720):

| region (1280×720 box) | TCRF luma | before luma / \|dRGB\| | after luma / \|dRGB\| |
|---|---|---|---|
| street_L (300,640)–(410,712) | 60.2 | 76.5 / 18.5 | **65.2 / 8.0** |
| crowd under the marquee (430,640)–(590,712) | 104.3 | 125.3 / 17.3 | **113.2 / 7.4** |
| street_R (600,650)–(740,712) | 92.8 | 88.0 / **23.0** | 37.5 / 49.3 |

`title_fidelity.py` (sky_dE / city_dE / city_edge, means over frames
60/200/400 and both runs):

| leg | sky_dE | city_dE | city_edge | chroma |
|---|---|---|---|---|
| before | 13.70 | **10.30** | 0.918 | 21.8 |
| after | 13.70 | 10.85 | **0.921** | 19.3 |

- Run-to-run spread is about ±0.1 on city_dE. **The whole-title figure gets
  slightly worse (+0.55)**, while two of the three street regions move
  sharply toward TCRF.
- The right-hand street turns much darker than TCRF's olive haze. My reading,
  **not tested**:
  - The road is a feedback loop. Each frame it multiplies last frame's road by
    its vertex lighting at alpha 0.9. Its steady state scales as
    1 / (1 − 0.9·L), so a lighting deficit is amplified.
  - The title's city is already dimmer than TCRF (`city_mid` 49.6 against
    55.0, section 19).
  - Retail's olive tint is what a near-unity loop gain looks like: the
    channel with the highest gain wins.
- I prefer this port to the baseline. It is retail's code and math (22.4),
  and it removes an orange street retail never shows. It is still a fidelity
  trade, not a win.

**Hub** (`screen_capture.py`, `main_hub_screen`, +120/+240): the QUICKPLAY
glass header now shows the scene through it, darker and bluer, instead of a
flat lavender bar (`~/tmp/w16rz/hub_menu_crop.png`).
- The bar's mean RGB goes from (69, 72, 93) to (18, 20, 40).
- Xenia's clean-TU5 front buffer (W16-QT `x4` frame 1800) has (46, 44, 79)
  with visible distortion. But that frame's background is the rooftop band,
  while ours is the Baboon Nest sign. Glass shows what is behind it, so the
  distance to Xenia is confounded and **not** a fidelity measure.

**WebGPU errors.** Counted as `WebGPU error` lines (`GpuDevice.cpp:156`):

| run | errors |
|---|---|
| title, before (2 runs) | 0 |
| title, after (2 runs) | 0 |
| hub, before | 0 |
| hub, after | 0 |
| BandRnd (rb3 flavor) title, after | 0 |

Each run logs one `device lost … destroyed` line at shutdown, as before.

### 22.7 Verification

| check | result |
|---|---|
| rb3 `w16-rz`, `native/build-native`, desktop, dc3 flavor, `MILO_ENGINE_PATH` = this worktree, `ctest` | **100% tests passed out of 134** (132 before, plus 2 new), 7 skipped (the seven of 20.7), rc=0 |
| new `RefractTest.RefractingSurfaceShowsTheSavedWorld` | passes: a red world saved at world end, then a green world; a white refracting quad over the right half reads red |
| same test with both save sites removed (`DoWorldEnd` and the `EndDrawing` fallback; sabotage, reverted) | **fails**, `right.r` = 0 (the black placeholder); the control `NonRefractingSurfaceKeepsItsColour` still passes |
| DC3 shape compile (`cmake -C cmake/dc3-reference.cmake`, pointed at `~/code/milohax/dc3-decomp`) | `MaterialSetup`, `Mesh_Wgpu`, `Rnd_Wgpu`, `PipelineManager` compile |
| rb3 flavor (`RB3_GPU_BACKEND=rb3`) build + title | builds; 0 WebGPU errors |
| `test_wgsl_validation` (in the ctest above) | passes with the new shader |

### 22.8 Not done

- **Pre-depth** (`mFrontBufferDepth` / `PreDepthTexture`) is not saved. Its
  readers are:
  - soft particles and motion blur: unreachable on the RB3-Wii shape (22.2);
    not drawn on rb3-xenon (`PreDepthTexture` null);
  - DoF: reads the live depth.
- **Soft particles** (37 Xbox venues) and **motion blur** (186 crowd/extras
  objects) stay unported for the same reason.
- **The depth clear after world end.** Retail clears depth twice: in the
  `0x300` resolve, and in `DoPostProcess`'s `BeginTiling` (`0x31`, l.288).
  So the draws after world end start with empty depth. `FlushWorldPost`
  keeps the world's depth ("depth and stencil carry on from the world").
  This is a separate deviation, not measured here.
- **UI glass semantics after world end.** The bind group chooses
  `mPostOutView` only when `FlushWorldPost` ran. A frame whose world end has
  no post-processing falls back to the pre buffer, which holds the same
  world. Retail's `CopyPostProcess` path for that case was not traced.
- **The right-hand street hypothesis** (22.6) is untested. It would need
  retail's vertex lighting for `city_road.mat`.
- **rb3-xenon and dc3-decomp were not run.** Their shape now refracts any
  material with `GetRefractEnabled(true)` (only the compile was checked), and
  rb3-xenon's main checkout was not built, per the brief.
- **BandRnd** (rb3 flavor) binds placeholders and does not refract.
- **No merge, pin bump or push.** That is for the coordinator.

## 23. The band head's wrinkle normal map, composed as retail does (lane W16-SA, 2026-10-07)

Section 16 bound retail's skin maps but left the head on the plain
`<gender>_head00_norm.tex`, because the Wii `RndTexBlender::DrawShowing` is
empty and `head_wrinkle_output.tex` was never painted (16.5). This section
ports retail's texture blender, has the dc3 backend draw it, and binds its
output as the head normal map, as retail does.

| repo | branch | commits |
|---|---|---|
| milo-native-engine | `w16-sa` | `965b9b6` (pass + seam), `0d8ec8d` (flag registry), and this section, on `f901441` |
| rb3 | `w16-sa` | `1482e35a9` (port), `fffd837ab` (GPU test, ledger), on `69c88e9a1` |

### 23.1 What retail does

rb3-xenon (retail Xbox, matched source) has three parts:

- `RndTexBlender::DrawShowing` (`rndobj/TexBlender.cpp`):
  - It returns unless the draw mode is normal and the world is processed,
    and unless its output is a no-z render target.
  - Each controller's `GetBlendState(alpha, influence)` sorts it into a near,
    far or custom list. The alpha is a smoothstep of the distance between two
    bones against reference/min/max distances, times the influence, clamped
    and quantised to 8 bits; anything below 1/255 drops out.
  - It returns early when its re-render flag is clear, all lists are empty
    and it last drew the base alone.
  - Otherwise it binds the output as target and draws:
    1. the base map as a full-target rect, blending off;
    2. the near list, then the far list, each sorted by ascending alpha,
       with the blender's near or far map;
    3. the custom list, each controller with its own override map (`mTex`).
  - Every list entry draws the controller mesh's faces with the `unwrapuv`
    shader, blending `SrcAlpha`/`InvSrcAlpha`, clamped sampling, and the
    work material's alpha set to the entry's alpha.
  - The retail ucode (disassembled from the shipped shader) is
    `tfetch2D r0.xyz1, r0.xy, tf0; mul oC0, r0, r2`: the texel's rgb, with
    alpha = material alpha. The vertex shader places each vertex at its UV.
- `OutfitConfig::SetSkinTextures`:
  - The head's normal map is `dir2`'s `head_wrinkle_output.tex`.
  - The eyes.cfg block: `SetHeadNormMap` points `norm_<part>.texblendctl`'s
    `mTex` at `<gender>_head_norm%02d.tex` (option + 1) for chin, eye, mouth,
    nose and shape.
  - If any of those changed, it sets the re-render flag on eyes.cfg's blender
    and on `wrinkle.texblend`.
- `OutfitConfig::DrawPreClear` draws the eyes.cfg blender when it is dirty,
  and the wrinkle blender every frame.

The data, measured natively (`RB3_TEXBLEND_PROBE`), per band member:

| blender | base | layers | output |
|---|---|---|---|
| `norm.texblend` | `<gender>_head00_norm.tex` | 5 custom controllers (the five head features) | `norm_output.tex`, 256×256 |
| `wrinkle.texblend` | `norm_output.tex` | 12 bone-distance controllers, maps `<gender>_head_wrinkles_near/far.tex` | `head_wrinkle_output.tex`, 256×256 |

All 17 controllers are revision 2.

### 23.2 Two loader gaps the port uncovered

- **Override maps were never read.** The Wii `RndTexBlendController::Load`
  reads `mTex` only when `gRev > 1`, but never stores `gRev`. A rev 2
  controller therefore loaded without its map. Retail stores the revision.
  Under `HX_NATIVE` the Load now does too.
- **Every member's controllers measured one shared skeleton.** The
  controllers' `mObject1/mObject2` resolved to bones in
  `char/main/skeleton_unshared.milo`, the same pointers for every member, so
  no member's wrinkles followed their own face.
  `NativeRepointBlendBones` (rb3 `OutfitConfig.cpp`) repoints each to the
  member's own same-named transform: 28 per member. Opt-out:
  `RB3_NO_TEXBLEND_BONE_REMAP=1`. This is a native-only fix: retail's
  per-member resolution yields the member's bones by itself.

### 23.3 Change

- **Engine.**
  - `platform/TexBlendHook.h`: a forward-declared seam, in the pattern of
    `PointTestHook.h`. `NativeTexBlendComposer::ComposeTexBlend(output,
    base, layers, count)`, where each layer is a mesh, a texture and an alpha.
  - `gfx/TexBlendPass`: rndobj-free. It records one render pass over the
    output, with load-op Load, in three pipelines per target format:
    - base: a full-target triangle sampling the base at the pixel's UV,
      blend off;
    - unwrap, static stride (64) and skinned stride (88): the UV at byte 40
      becomes clip `(2u − 1, 1 − 2v)`, the pixel is `vec4(tex.rgb, alpha)`,
      blend `SrcAlpha`/`OneMinusSrcAlpha`, no culling, clamp/linear sampling.

    Each draw gets its own parameter block and bind group.
  - `WgpuRnd::ComposeTexBlend` resolves the target (`GetGpuTexView`,
    `IsGpuTexRenderable`), the textures (`PresyncBitmap`) and the mesh
    buffers (`EnsureMeshUploaded`). It ends the open frame pass, records,
    and resumes the frame pass.
    - It declines (returns false) with no frame open, or while another
      target is bound.
    - The composer is registered in `Init` and cleared in `Terminate`.
- **rb3.**
  - `TexBlender.cpp` (`HX_NATIVE`) is retail `DrawShowing`: the gates, the
    lists, the early-out, the sort, and the near/far/custom layer order and
    alphas, handed to the composer. One deviation: the re-render flag
    (`unk9p6`, retail `unkc0`) is cleared only after the backend records,
    so a declined frame retries.
  - `TexBlendController`: retail `IsValid` and `GetBlendState`, plus the
    `gRev` store (23.2).
  - `OutfitConfig::SetSkinTextures`:
    - retail's eyes.cfg block;
    - the head binds `head_wrinkle_output.tex` whenever a composer is
      registered (else `head00`, as before);
    - the bone repoint.
  - Flags (registered and classified in `NativeCompatFlags`):

    | flag | class | effect |
    |---|---|---|
    | `RB3_NO_WRINKLE_BLEND` | feature, default on | opt-out: empty DrawShowing and the `head00` binding |
    | `RB3_NO_TEXBLEND_BONE_REMAP` | native-only workaround | keeps the shared skeleton's bones |
    | `RB3_TEXBLEND_PROBE` | probe | `[TEXBLEND]` lines |
    | `RB3_TEXBLEND_AB_FRAME` | probe | in-run A/B, see 23.4 |

### 23.4 Results

**Composition runs per member.** Gameplay, quickplay song, 3,780 frames
(`~/tmp/w16sa/P3/run.log`):
- 370 `[TEXBLEND] draw` lines, all `ok=1`, 0 declined;
- 4 `norm.texblend` draws per outfit build, with male and female bases
  (`states=0x9`: base + 5 custom);
- `wrinkle.texblend` re-renders whenever its near/far sets change
  (`states=0x7`, e.g. `near=2 far=7`);
- 0 `GpuDevice: WebGPU error` lines;
- the `[SKIN_MAPS]` probe shows every head instance bound to
  `head_wrinkle_output.tex`.

In a dumped pair (`MILO_DUMP_RT=1`, main_hub frame 300), the wrinkle layers
change 6,887 of 65,536 texels of `norm_output.tex` (max channel delta 147).

**An A/B across runs is impossible here, and the obvious comparator lied.**
- `magick compare -metric AE` reported **0 differing pixels** for every
  before/after pair, including a sabotage leg that bound the specular map as
  the head normal.
  - Cause: the PNGs carry an alpha channel of mean 0.003, and ImageMagick
    compares near-transparent pixels as equal. Every frame comparison in
    this section uses rgb with alpha dropped (`-alpha off`, numpy).
- Measured that way, two runs of the same binary with the same flags differ
  across the whole frame: main_hub frame 300, 797,956 px, max 254.
  Gameplay camera cuts also land on different shots. A before run against
  an after run therefore measures run-to-run drift, not the change.

**In-run A/B.** `RB3_TEXBLEND_AB_FRAME="a-b,…"` makes every blender draw its
chain's first base map alone inside each window. That is `head00` for the
wrinkle output, the pre-W16-SA head normal. `~/tmp/w16sa_pause.sh` sets up
each pause point P:
1. pause the song with `msg:beatmatch:set_paused:1:0:0` (the scene freezes);
2. capture 8 composed frames (P+14…28);
3. A/B window [P+30, P+50); capture 8 `head00` frames (P+34…48);
4. capture 8 composed frames again (P+54…68) as the control;
5. unpause at P+70.

The film grain still moves in a paused frame (±15/255, per frame), so each
set of 8 is averaged. Treatment is |composed − head00| and control is
|composed − composed again|. Pause points where heads were lit, P3 run:

| P | control max | treatment max | px above control max | px > 8, treatment / control |
|---|---|---|---|---|
| 2700 | 2.4 | 22.9 | 2,553 | 173 / 0 |
| 2850 | 6.1 | 47.6 | 1,658 | 1,180 / 0 |
| 3000 | 8.4 | 61.9 | 714 | 764 / 6 |
| 3150 | 9.8 | 133.6 | 1,005 | 1,619 / 108 |

The P4 run used the final rebased binary. Its shots differ (camera cuts) and
its grain is noisier (control max 15–20). At P = 3300 the treatment max is
197.3 against a control max of 14.6, with 519 px above it, centred on a face
(x 1023–1061, y 274–303, 10th–90th percentile).

- The treatment pixels sit on the faces: cheeks, nose, nasolabial folds,
  brow.
- Composed frames carry the feature normals and the bone-driven wrinkles;
  the `head00` frames carry the neutral head.
- Close-ups: `~/tmp/w16sa/W16SA_head_ab.png`. Each row is composed, then
  `head00`, then the 8× difference of the averages. Rows: P4 P=3300, P3
  P=3150, P3 P=2850, P3 P=2700.
- The per-pause heat maps and averages are under `~/tmp/w16sa/P3` and
  `~/tmp/w16sa/P4`.

Pause points with no lit head show nothing, and that is expected.

- On main_hub the drawn `head.mesh` has no approx lights (`object.retail.y`
  = 0) and no point lights. The shader's `retailLightsOn()` is false, so the
  per-pixel branch, the only reader of the normal map, does not run.
- In paused gameplay frames P3 2400 and 3300 (no lit head in shot), the
  treatment stays at or under the control.
- This matches retail's `RndShaderStandard::CalcShaderOpts`, which selects
  the normal map only with real or approx lights. The one difference is
  noted in 23.6.

### 23.5 Verification

| check | result |
|---|---|
| rb3 `w16-sa` (`fffd837ab`), `native/build-dc3`, desktop, dc3 flavor, `MILO_ENGINE_PATH` = this worktree (read back from `CMakeCache.txt`), `ctest` | **100% tests passed out of 137** (134 before, plus 3 new), 7 skipped (the seven of 20.7), rc=0 |
| new `TexBlendPassTest.BaseThenUnwrappedLayersMatchRetailModel` | Gradient base, a static-stride quad (alpha 0.5, texel alpha 0) and a skinned-stride triangle (alpha 0.25, opposite winding). Within **1/255** of a CPU model, off the triangle's diagonal. |
| same test, unwrap's v flipped (sabotage, reverted) | **fails** at 63/255 |
| `TexBlendPassTest.NullBaseLoadsTarget`, `ErrorScopeCatchesUnrenderableTarget` | pass; the error scope does catch a validation error |
| `native_compat_census.py --engine-root <this> check` | `OK — 436 scanned flags all present in registry, regen clean` |
| gameplay runs above | rc=0, 0 WebGPU errors |

### 23.6 Not done

- **Projected lights do not enable per-pixel lighting natively.** Retail
  `NumLights_Real` counts projected lights too (`NgEnviron::Select`), so a
  venue with only a `shadow_projected.lit` would light heads per pixel.
  The native gate (`retailLightsOn`) counts approx and point lights only.
  - With no approx lights the box term is zero, so the normal map would add
    almost nothing there.
  - Neither venue nor main_hub was checked for projected lights.
- **No retail capture** to compare the composed RT against.
- **`colorpalettes.milo`'s shared `head_naked.mat` copy** (17.2) also binds
  `head_wrinkle_output.tex`, last writer wins. It is not drawn in the band
  shots.
- **The rb3 flavor (BandRnd)** registers no composer, so rb3's
  `DrawShowing` stays a no-op there and the head keeps `head00`.
- **rb3-xenon and dc3-decomp** were not built. The engine change is
  additive: a seam nobody else registers against.
- **Web build** not rebuilt.
- **No merge, pin bump or push.**

## 24. What the flare point tests cost the frame (lane W16-SB, 2026-10-07)

Sections 20 and 21 left one cost unmeasured (20.6). On every desktop frame
that queued tests, `WgpuRnd::RunPointTests` called
`PointTestPass::Collect(wait=true)` at the world end, which blocks in
`Instance::WaitAny` (up to 1 s per batch) until the previous frame's readback
has mapped. The world end comes before the frame's own submit. So when the
GPU is behind, the CPU waits there and the GPU then sits idle until the frame
is submitted. This section measures that cost on the title and moves the wait
to after the submit.

| repo | branch | commits |
|---|---|---|
| milo-native-engine | `w16-sb` | `7e92531` (instrument), `9e72545` (change), and this section, on `4a66a9f` |
| rb3 | `w16-sb` | `6be95edcd` (NativeCompat ledger), `51585398a` (tests), on `013862302` |
| rb3-xenon | `w16-sb` | `9fd319ae8` (`flare-tests-at-world-end` checks answer age, 24.8), on `962c20ec5` |

Before the rebase onto `4a66a9f` the engine commits were `3885d18` and `ea9af8c`, on `f901441`.

### 24.1 Instrument

- **`MILO_FRAME_TIMES=<path>`** (engine, `3885d18`) writes one CSV row per frame
  from `WgpuRnd::EndDrawing`. Columns:
  - `period_ms` and `cpu_ms`: wall time and the main thread's CPU time
    (`CLOCK_THREAD_CPUTIME_ID`) from one `EndDrawing` to the next. That covers
    the whole frame: game poll, draw and submit.
  - `draw_ms`: `BeginDrawing` to the end of `EndDrawing`.
  - `pt_wait_ms`: time inside the world-end `Collect`.
  - `pt_recorded`, `pt_answers`: tests recorded and answers delivered this
    frame.
  - `pt_age`: how many frames after its tests were recorded the oldest answer
    delivered this frame arrived. `Answer` now carries its batch's sequence
    number for this.
  - `pt_end_wait_ms`, `pt_after_submit` (added with the change): the time and
    answers of the new end-of-frame step.
- **`MILO_NO_POINT_TESTS=1`** registers no `NativePointTester`, so rb3's
  `Rnd::TestPoint` keeps its no-tester fallback. This is the "without flare
  tests" leg.
- Both flags are classified in `NativeCompatFlags.classification.json` as
  probes; `native_compat_census.py check` passes (434 flags).

Every leg ran the title headless with `RB3_GAME=1 MILO_HEADLESS=1
MILO_MAX_FRAMES=420 RB3_FIXED_CLOCK=1`, with no screenshots, because a
screenshot readback blocks on the GPU itself. Statistics are over engine
frames 150–421 (272 frames); frames 100–110 still load. The headless loop has no
frame pacing, so a stall shows directly in `period_ms`.

Two builds of rb3 `rb3-native`, both against the engine worktree:

- **Debug** (`native/build-native`, `-O0`), the desktop build `ctest` uses;
- **`-O2 -fno-inline -g0`** (`CMAKE_BUILD_TYPE=Release` with those flags), the
  web release's optimisation. Plain `Release` does not link (undefined
  references to functions defined out of line elsewhere, the fault 1281ff of
  rb3's `native/CMakeLists.txt` describes for the web), so this is not a
  shipped desktop configuration.

Host: an RTX 3090, shared with other lanes (load average 29–35 on 32 cores,
four other `rb3-native` processes on the same GPU during the runs). Identical
legs differ by up to ±2 ms of CPU median, and single runs were sometimes
slowed by 10 ms or more (marked `*` below). The legs were run interleaved:
before-on, after-on, before-off, after-off, three rounds.

### 24.2 The cost on the title

8 flares are tested each frame, and every answer arrives at age 1.

Main-thread CPU time per frame, median of each run (ms):

| build | leg | before | after |
|---|---|---|---|
| Debug | tests on | 29.03 / 28.26 / 25.61 | 25.76 / 25.99 / 27.52 |
| Debug | tests off | 27.26 / 28.92 / 24.99 | 29.04 / 27.34 / 28.86 |
| `-O2` | tests on | 35.16\* / 20.43 / 22.10 | 33.09\* / 22.04 / 20.88 |
| `-O2` | tests off | 22.82 / 21.01 / 22.88 | 22.81 / 24.33 / 33.58\* |

Time in the point-test collect per frame, mean (p95, max) in ms:

| build | before: world-end wait | after: world-end collect | after: end-of-frame wait |
|---|---|---|---|
| Debug | 0.043–0.050 (≤ 0.071, 0.367) | 0.069–0.076 (≤ 0.105, 0.642) | 0.000 (max 0.002) |
| `-O2` | 0.042–0.047 (≤ 0.062, 0.930), \*run 0.292 | 0.070–0.072 (≤ 0.093, 0.299), \*run 0.434 | 0.000 (max 0.002) |

- **On this host the wait does not stall the title.** The blocking wait costs
  about 0.05 ms of a 20–30 ms frame, and it does not grow with resolution. An
  earlier two-run set at 3840×2160 and 7680×4320 (the log confirms
  `GpuDevice: initialized (7680x4320, headless)`) measured 0.046–0.055 ms, with
  CPU medians of 18.8–24.3 ms, as at 720p.
- **With and without tests, the CPU time differs by less than the run-to-run
  spread**, in both builds, before and after.
- The non-blocking collect (`ProcessEvents`) costs about 0.025 ms more than
  `WaitAny` on a future that has already completed. The end-of-frame step
  costs nothing when the world end has already taken the answers, which it
  does on every unloaded frame (`pt_after_submit` = 0).
- The two `*` maxima (6 ms, 32 ms) were slow in every column of the run.
  They look like the thread being descheduled, not the GPU.

GPU time per frame (temporary probe, removed: wait for
`OnSubmittedWorkDone` straight after the submit, which serialises the frame):
**0.83 ms median at 1280×720** (p95 2.49) and **7.47 ms at 7680×4320** (p95
9.48). The CPU frame is about 25× longer than the GPU's at 720p. A GPU would
have to be that much slower before the old wait stalled anything.

### 24.3 The stall, when the GPU is behind

To reach the case the old wait handles badly, a temporary probe (removed)
submitted a compute job, calibrated to N ms of GPU time, at each
`BeginDrawing` from frame 60 on, ahead of the frame's own commands. That
stands in for a slower GPU. `-O2` build, tests on, 720p.

| load | leg | period med | CPU med | world-end wait mean | end-of-frame wait mean |
|---|---|---|---|---|---|
| 60 ms | before | 59.24 / 58.38 | 21.33 / 23.88 | **37.29 / 33.75** | — |
| 60 ms | after | 58.92 / 58.12 / 57.04 | 21.19 / 22.68 / 22.10 | **0.012–0.014** | 37.69 / 38.38 / 34.35 |

- The first 60 ms "before" run is left out. Its calibration read 1.75e-4
  ms/iteration against 7.1–7.8e-5 for the other 60 ms runs, so its load was
  about 25 ms (period 26.9 ms). Each run calibrates once, on a GPU other lanes
  were using.
- **Before:** the CPU waits 34–37 ms per frame at the world end, before the
  frame is submitted.
- **After:** the world end does not wait, and the same 34–38 ms is spent after
  the submit, while the GPU works on the frame just submitted. The period and
  the CPU time do not change materially. The frame is GPU-bound, and with no
  frame pacing headless the CPU has to wait for the GPU somewhere. The
  throughput gain is bounded by the CPU work between the world end and the
  submit (UI, post-processing encode), which is small on the title: under
  1 ms of period here (medians 57.0–58.9 after, 58.4–59.2 before), inside the
  spread.
- At a 30 ms load the runs sit on the CPU/GPU boundary and disagree from run
  to run; their calibrations (7.2e-5 to 1.15e-4) put the real load between
  about 19 and 30 ms. One before run waited 9.2 ms per frame at the world end (max 106);
  the after runs moved 10.1 and 10.9 ms to the end of the frame and kept the
  world end at 0.013–0.016 ms.

So what the change buys is a world end that never blocks: the step retail
takes at `DoWorldEnd` no longer serialises the CPU against an idle GPU. It
does not make a GPU-bound frame faster in any amount measurable here, and on
this host the title is never GPU-bound.

### 24.4 Change

- **`PointTestPass::CollectThrough(through, wait, …)`** considers only batches
  whose sequence number is at most `through`. Without `wait` it takes those
  whose map has finished (`ProcessEvents`); with `wait` it blocks for them, up
  to 1 s each. Later batches stay in flight even when finished. `Collect(wait)`
  is `CollectThrough(~0, wait)`, unchanged for its callers. Under
  `__EMSCRIPTEN__` it never blocks.
- **`RunPointTests`** (world end) calls `CollectThrough(seqBeforeFrame,
  false)`, where `seqBeforeFrame` is `LastSeq()` at `BeginDrawing`. It
  delivers the previous frame's answers if ready, and no longer blocks.
- **`FinishPointTestReadback`**, the new last step of `EndDrawing`, runs after
  the submit and the present. It calls `CollectThrough(seqBeforeFrame, true)`
  and blocks only for earlier frames' batches still mapping. Every answer
  therefore still arrives during the frame after its test, as retail's does
  after `DxRnd::DoPointTests` blocks on the fence. It is desktop-only; the
  web keeps taking what is ready at the world end, as before.
- **The `through` filter is load-bearing.** The first version waited only
  through `seqBeforeFrame` but then took any finished batch. On the title,
  frame 3 is a long boot frame, and that version delivered frame 3's own
  answers at the end of frame 3: 16 answers there, none in frame 4. That is
  one frame earlier than retail, and a flare would have reacted a frame
  sooner. The answer comparison (24.5) caught it.
  `PointTestPassTest.CollectThroughTakesOnlyBatchesUpToSeq` now guards it.

### 24.5 The answers on the title are unchanged

A temporary probe in rb3's `ApplyNativePointTest` (removed) printed every
answer with the flare's name. The answers were grouped by the frame loop's
`frame N complete` marker and compared frame by frame against the
before-change engine (`-O2`, same fixed clock):

| leg | answers | frames with answers | frames that differ from before | answers delivered after the submit |
|---|---|---|---|---|
| before | 3,336 | 417 (3–419) | — | 0 |
| after | 3,336 | 417 | **0** | 0 |
| after, 40 ms GPU load | 3,336 | 417 | **0** | 2,000 |
| after, rebased on `f901441` | 3,336 | 417 | **0** | 0 |

Under load, 2,000 answers came through the end-of-frame step instead of the
world end. They arrived in the same frames with the same values, and every
delivered answer had age 1 in every leg. The values are W16-RU's (20.3), for
example `Flare_lamp01` 16,383 visible and `Flare_red_blink02` 154 hidden.

### 24.6 Tests

rb3 `native/tests/test_point_test.cpp`:

- A helper, `GpuBusy`, submits a compute job calibrated on first use to
  ~150 ms of GPU time. Anything submitted after it waits behind it, so
  whether a readback is ready at a given point is not a race.
- **`PointTestPassTest.CollectThroughTakesOnlyBatchesUpToSeq`** (new). Two
  one-test batches queue behind the busy job.
  - `CollectThrough(second, false)` delivers nothing and returns in under
    75 ms.
  - After the GPU is idle and `ProcessEvents`, `CollectThrough(first, true)`
    delivers exactly the first batch, and the finished second batch stays in
    flight.
- **`RndTestPoint.SlowGpuAnswersArriveByFrameEndNotAtWorldEnd`** (new). Frame
  A's commands queue behind the busy job. At frame B's world end:
  - nothing is delivered, and `DoWorldEnd` takes under 75 ms;
  - by the end of frame B exactly frame A's three answers have arrived, and
    they are correct: the flare behind the wall reads hidden, area 0;
  - frame B's own answers arrive at the next world end.
- **`RndTestPoint.FlareBehindGeometryReadsZero`** now waits for the GPU to go
  idle before the frame that must answer at the world end. Otherwise "answered
  at world end" would depend on the GPU's timing.

Sabotage, each applied to the engine and reverted (sources restored
byte-identical, `sha1sum -c`):

| sabotage | predicted red | observed red |
|---|---|---|
| world end blocks again (`wait = true`) | SlowGpu | SlowGpu |
| no `FinishPointTestReadback` call | SlowGpu | SlowGpu |
| end-of-frame wait through `LastSeq()` (the current frame) | SlowGpu | SlowGpu, FlareBehind, RemovedFlare (each also checks that nothing is answered in the test's own frame) |
| `CollectThrough` delivers past `through` | CollectThroughTakesOnly | none at first; CollectThroughTakesOnly once the test calls `ProcessEvents` before the wait |

The last row is a test fix. In wait mode `CollectThrough` never calls
`ProcessEvents`, so the newer batch's map callback had not fired and the
filter was never exercised. The test now completes both maps first.

### 24.7 Verification

| check | result |
|---|---|
| rb3 `w16-sb` (`bf637da92`), `native/build-native`, desktop, dc3 flavor, `MILO_ENGINE_PATH=/home/free/tmp/wt-w16sb-eng` (read back from `CMakeCache.txt`), engine rebased on `f901441` | `ctest`: **100% tests passed out of 136**, 7 skipped (the seven fixture-gated tests of 20.7), rc=0. 134 → 136 is W16-RZ's two `RefractTest`s; before the rebase it was 134/134 (132 + the two new tests) |
| rb3 `w16-sb` (`51585398a`) rebased on `013862302`, engine rebased on `4a66a9f`, same build dir | `ctest`: **100% tests passed out of 139**, the same 7 skipped, rc=0. 136 → 139 is W16-SA's tests |
| the six point tests in those runs | all pass |
| `native_compat_census.check` (in those runs) | passes. After the rebase, `gen` rewrote the engine's `NativeCompatFlags.gen.inc` and sidecar byte-identically (sha1 checked); the rb3 ledger was regenerated from master's copy, +2 rows (`MILO_FRAME_TIMES`, `MILO_NO_POINT_TESTS`) next to W16-SA's `RB3_TEXBLEND_*` |
| WebGPU errors | no `WebGPU error` line in any of the 76 title runs |
| rebased Debug, tests on / off | CPU median 29.97 / 29.95 ms, world-end collect 0.078 ms: as in 24.2 |
| DC3 shape compile (`cmake -C cmake/dc3-reference.cmake`, a copy pointed at `~/code/milohax/dc3-decomp`, build dir `~/tmp/w16sb/build-dc3ref`) | `Rnd_Wgpu.cpp` and `PointTestPass.cpp` compile, rc=0, 0 `error:` lines |
| rb3-xenon `962c20ec5` (main checkout, read-only), `rb3-render` built out of tree in `~/tmp/w16sb/xenon-build` with `MILO_ENGINE_PATH` = this worktree (read back from `CMakeCache.txt`); 21.3's `sv8_a` cell, `--frames 8 --focus mesh`, five runs | **`flare-tests-at-world-end` PASS in 5 of 5**: 56 answers at the world end, 0 at EndDrawing, as in 21.3's fix row. The five runs' flare answers are identical, with `lamp04` visible at ratio 0.964 |

The rb3 change is test-only, plus the regenerated NativeCompat ledger.

### 24.8 rb3-xenon's flare gate checks answer age

Before this change rb3-xenon's `flare-tests-at-world-end` gate (21.2) required
every answer at `Rnd::EndWorld` and none at `EndDrawing`. With the change, an
answer whose readback is not finished at the world end arrives at the end of
`EndDrawing` instead, still one frame old. So the old gate depended on GPU
timing. rb3-xenon `9fd319ae8` (`native/src/main_render.cpp`) now wraps the
backend's `NativePointTester` for `rb3-render`'s frame loop. It notes the frame
each accepted test was queued in, per flare, and matches each answer to that
flare's oldest noted test. The gate passes when:

- there are answers;
- none is older than one frame;
- no test is still unanswered at the end of the frame after its own;
- no answer is unmatched.

The world end / `EndDrawing` split is still printed.

Measured on `sv8_a`, `--frames 8 --focus mesh`, `rb3-render` built in
`~/tmp/wt-w16sb-xen/native/build` with `MILO_ENGINE_PATH` = this worktree
(read back from `CMakeCache.txt`) and a test-only `MILO_ENGINE_PIN` of
`c3eee3d`, not committed. The two sabotages were temporary edits to
`Rnd_Wgpu.cpp`, restored by sha1 afterwards:

| engine | new gate | answers, aged 0 / 1 / 2+ | overdue | world end / EndDrawing | old gate |
|---|---|---|---|---|---|
| this branch, 4 runs | **PASS** | 56: 0 / 56 / 0 | 0 | 56 / 0 | pass |
| delivers two frames late (limit from the previous frame's `BeginDrawing`) | **FAIL** | 48: 0 / 0 / 48 | 56 | 48 / 0 | **pass** |
| skips the world-end collect (every answer at `EndDrawing`) | **PASS** | 56: 0 / 56 / 0 | 0 | 0 / 56 | **fail** |
| this branch, `RB3_POST_WORLD_OCCLUDER=1` | PASS | 56: 0 / 56 / 0 | 0 | 56 / 0 | pass |

In every row `lamp04` reads visible at ratio 0.964. Under
`RB3_POST_WORLD_OCCLUDER` `image-not-empty` fails, as that knob's comment says
it will. Under `RB3_NO_END_WORLD` the gate is skipped, as before.

### 24.9 Not done

- **A windowed, presenting desktop** was not run (no display on this host).
  With FIFO present, `GetCurrentTexture` and present pace the frame. The old
  wait would then trade against that throttle, not against an unpaced loop.
- **A GPU slower than the CPU frame** was only simulated, with the compute
  load of 24.3. No real low-end GPU or software adapter was measured.
- **rb3-web** was not built. Its path is unchanged: the world end still takes
  what is ready, and the end-of-frame step is compiled out.
- **dc3-decomp** was not built or run; only the two changed engine objects were
  compiled on its shape (24.7). On DC3 the step runs with an empty queue.
- **`native_health.sh` / `native_build_gate.sh`** were not run for rb3-xenon;
  only `rb3-render` was built. rb3-xenon's real `MILO_ENGINE_PIN` is not set
  here.
- **The probes** (GPU time, GPU load, flare names) are not committed. The
  GPU-load probe crashed at process exit (its static wgpu objects outlived the
  device) after all 420 frames had been recorded. The patches are in
  `~/tmp/w16sb/probe-*.patch`, and the runs and the `leg.sh` / `stats.py` /
  `flares.py` scripts are in `~/tmp/w16sb/`.
- **No merge, pin bump or push.** That is for the coordinator. This section was
  renumbered 23 → 24 after W16-SA's section 23 landed.

## 25. The dark right-hand street after world refraction (lane W16-SE, 2026-10-07)

Section 22 left the title's right-hand street much darker than TCRF
(`street_R` luma 37.5 against 92.8). It also left a guess, never tested:
`city_road.mat` refracts its own previous frame and so amplifies a city
lighting gap. This section tests that guess and finds it does not explain
the street. It also fixes one rb3 defect found on the way, and names the part
of the gap that is still unported.

| repo | branch | commit |
|---|---|---|
| rb3 | `w16-se` | `c354d413e` (on `0d9b64067`) |
| milo-native-engine | `w16-se` | this section only (on `886b18b`) |

All native runs use `title_capture.sh` with `RB3_FIXED_CLOCK=1` at frames
60/200/400 and the dc3 flavor. The regions are section 22's (`street_R`,
`street_L`, `crowd`), plus a **road window**, which is the
`city_road.mat` pixels inside x 600–820, y 597–625. That window lies above the
overshell bar, so it can be compared with xenia's hub frame. The road mask
comes from a probe draw of the road material (`~/tmp/w16se/roadmask.png`,
3,651 px in the window). Scripts: `~/tmp/w16se/roadcmp.py` and
`~/tmp/w16rz/street_regions.py`.

### 25.1 The guess, measured

The guess was tested with an engine shader probe (not committed). It writes
the terms of the refracting draw into the world colour buffer, and that buffer
is then read back from the saved pre buffer. Probe modes:

- the lit material colour with the screen multiply skipped;
- the refract fetch replaced by 0 and by 1;
- the screen-space offset;
- the refract normal sample.

The road's next pass is the alpha-blended overlay layer. For each pixel, the
road's output is then

```
S = (1 − a_ov)·(0.9·L·S′ + 0.1·dst) + a_ov·T·L2
```

Here `S′` is the previous frame's pre buffer at the refracted position, `L`
is the road's lit colour (22.4's `r3`), and `a_ov·T·L2` is the overlay. The
loop gain is `0.9·L·(1 − a_ov)`. Frame 200, `street_R`:

| term | R | G | B |
|---|---|---|---|
| `L`, unclamped mean | 0.659 | 0.355 | 0.004 |
| `L`, p90 / max | 0.863 / 0.988 | 0.471 / 0.533 | 0 / 0.157 |
| `1 − a_ov` | 0.464 | 0.464 | 0.464 |
| `a_ov·T·L2` | 0.036 | 0.018 | 0 |
| loop gain, mean | **0.275** | **0.148** | 0.002 |
| loop gain, max | 0.634 | 0.339 | 0.086 |

- **The loop exists**: the road does sample last frame's world, and over the
  road that is mostly road. The decoded offset is about 0.06 of the screen in
  each axis (median |Δuv| 0.058 / 0.062; read through the road's own blend,
  so approximate).
- **It does not amplify much.** At gain 0.28 the steady state is 1/(1 − g) ≈
  **1.4×** in red, and less in the other channels. Even the maximum gain
  (0.63) stays far from unity, and `L` never exceeds 1 on the street. A loop
  this weak cannot turn a small lighting deficit into a 2.5× luma gap.
- **The darkness is the multiply itself.** With the screen multiply skipped
  (lit colour only), the road window reads luma 87.4. With retail's
  `scr * r3` it reads 49. The saved pre buffer under the road is dark (road
  window 9.8 as dumped), and the road is that darkness times its lighting.
  This is 22.4's math and the order is retail's.

### 25.2 What retail code renders there

xenia running clean TU5 (`~/tmp/w16qt/x4/rgb_1800.png`: `main_hub_screen`
frame 1800, same city, same camera; section 10's capture) draws the same
street **darker than native does**:

| road window | luma | RGB |
|---|---|---|
| TCRF title | 98.6 | 115.6 / 97.5 / 59.8 |
| xenia TU5, hub f1800 | **22.0** | 27.3 / 21.0 / 13.1 |
| xenia TU5, hub f2100 | 21.1 | 21.0 / 20.2 / 26.0 |
| native before W16-RZ | 73.9 | 103.2 / 66.8 / 33.3 |
| native after W16-RZ | 49.2 | 64.5 / 44.1 / 35.2 |
| native, this lane | 49.0 | 64.1 / 44.0 / 35.0 |

Retail's code, emulated, puts this street at 22. Our port of the same
refraction gives 49. **The street's deviation from TCRF is therefore not a
refraction defect**, and it is not something W16-RZ introduced against
retail. The bright street in TCRF comes from a different draw.

### 25.3 What TCRF shows instead: a spotlight beam

In TCRF, a cone of warm light with visible rays spreads from the street lamp
down across the road (`~/tmp/w16se/street4.png`: TCRF, xenia f1800, xenia
f2700, native). That is a volumetric spotlight beam. Native draws no beams on
the title, for two reasons.

**Defect 1, ours, fixed in rb3: no spotlight ever registered.** Both
`ObjPtr<T>::Load` and `ObjOwnerPtr<T>::Load` returned `false` whenever a dir
was available, including when the name was found. `Spotlight::Load` does
`if (!mTarget.Load(bs, false, 0)) mTargetLoaded = false;`. As a result every
spotlight had `mTargetLoaded == false`, and `SpotlightDrawer::DrawLight`, gated
on it, never added one to the drawer. That dropped beams, spotlight flares,
lenses and additional objects.

- Retail's answer comes from the Wii target asm (`Spotlight::Load`'s inlined
  copy, 0x8085C5B8–0x8085C6A8), which loads 0 into the result only on the
  `mPtr == 0 && buf[0] != 0` path and 1 otherwise. rb3-xenon's
  `Spotlight.cpp:424` consumes it the same way.
- **The fix**: return false only for a non-empty name that is not found.
  - Title: 0 → **12** spotlights registered (`Spotlight01`–`08`, `subway`,
    `subway01`, `subway03`, `subway_bridge`). For example, `Spotlight01` is
    warm (packed `0x4386ff`: R 255, G 134, B 67), cone length 600, radius
    185 → 45.
  - Wii matching build (whole binary, one worktree): **+2 functions /
    +3,784 B**. `Spotlight::Load` went 99.37 → 100 and
    `LightPreset::SpotlightEntry::Load` went mpn 97.20 → 100; no row
    regressed.
  - New test `NativeSubsystems.ObjPtrLoadFailsOnlyForMissingName`. With the
    fix reverted, 3 of its 4 assertions fail.

**Defect 2, not ported: the NG beam pass.** Retail 360 runs with
`kNewGfx`, so `SpotlightDrawer::DrawWorld` skips the old-gfx `DrawBeams`, and
`NgSpotlightDrawer` draws the beams instead. In rb3-xenon, `RenderScene` is at
99.6 and `DoPost`, `EndWorld`, `BlurRT` and `SetupForPostProcess` are at 100.
The pass works like this:

- `NgSpotlightDrawer::DoPost` → `RenderScene` renders each beam into a
  half-resolution target.
- It uses the beam definition's cross-section texture (`SetXSectionTexture`,
  PS sampler 0xB), fog density and the smoke/half-distance parameters.
- It blurs the target (`BlurRT`) and composites it in post.

Native rb3 runs `kOldGfx` (`System.cpp: SetGfxMode(kOldGfx)`), so it takes
`DrawBeams`. That draws each spotlight's beam mesh with the beam's own
material. The Xbox beams carry no material, so the engine skips those draws.
The engine has no counterpart to the NG pass.

**What is not proven.** xenia's hub frame shows the lamp but no cone either,
and vanilla TU5 shows a movie behind the title, so no retail TU5 capture of
this screen exists. The TCRF title predates TU5's movie title. So either the
hub's light state differs from the title's, or xenia does not render the pass.
The beam is the only draw found that puts light on this street in TCRF and
that native lacks. It is still an attribution, not a measured equality.

### 25.4 Results

Title, frames 60/200/400 (luma, mean of three; TCRF single frame):

| region | TCRF | after W16-RZ | this lane |
|---|---|---|---|
| street_R | 92.8 | 37.4 | 37.2 |
| street_L | 60.2 | 65.1 | 64.0 |
| crowd | 104.3 | 113.2 | 112.8 |
| road window | 98.6 | 49.2 | 49.0 |

As predicted, registering the spotlights does not move the street: their beams
still have nothing to draw them on native. The run had 0 WebGPU errors.

### 25.5 Verification

| check | result |
|---|---|
| rb3 native ctest (`build-native`, dc3 flavor) | 140 tests, **100% passed**, 7 skipped (the usual real-capture fixtures) |
| sabotage: fix reverted, new test | **fails** (found name, empty name, `ObjOwnerPtr` found name) |
| Wii whole-binary A/B (`tools/setup-worktree.sh`, warm cache, 661 objs rebuilt) | 31,942 → 31,944 fns, 7,219,476 → 7,223,260 B; 2 rows changed, both up |
| title capture | 0 WebGPU errors, street metrics within run spread of after-RZ |

### 25.6 Not done

- **The NG volumetric beam pass** is not ported. It would need retail's
  spotlight shaders, the half-resolution target, fog density, the blur and the
  post composite, plus a way to run it under rb3's `kOldGfx`. That is a lane of
  its own. It would also have to settle 25.3's open question first, for
  example with a xenia capture of a TU5 screen where the city spotlights draw
  beams.
- **No other `ObjPtr::Load` caller was audited for behaviour.** In rb3 only
  `Spotlight::Load`, `LightPreset` (`tPtr`, which has no owner and so takes the
  unchanged branch) and `RndPostProc::Load` (`mColorXfm`, a different `Load`)
  test the result. The Wii A/B shows no row moved except the two above.
- **rb3-xenon and dc3-decomp were not touched.** Their `ObjPtr` headers are
  their own.
- **The probes** (shader modes, pre buffer dump, spotlight logging) are not
  committed. The captures are in `~/tmp/w16se/cap_*`.
- **No merge, pin bump or push.** That is for the coordinator.

## 26. Volumetric spotlight beams, from retail (lane W16-SI, 2026-10-07)

Section 25 left the title's street lamp without its beam. Retail 360 draws
beams in `NgSpotlightDrawer` (half-size render, blur, post composite), and
nothing in the engine did that. This section ports the pass to the dc3
backend, fixes two rb3 defects that kept the beams faint and misaimed, and
compares the result with TCRF.

| repo | branch | commit |
|---|---|---|
| milo-native-engine | `w16-si` | `fd6441c` + this section (on `c97d5da`) |
| rb3 | `w16-si` | `775737cba` (on `ec475582e`) |

### 26.1 What retail does

Sources: rb3-xenon `world/SpotlightDrawer_NG.cpp` for the CPU side
(`RenderScene`, `RenderBeams`, `RenderCone`, `RenderConeDefs`,
`SetupXSection`, `RenderSheet`, `RenderSphere`, `BlurRT`,
`SetupForPostProcess`). The shipped `xbox_shaders` for the GPU side, dumped
with `tools/rb3-dc3-parity/xobx.py` and disassembled with
`xenia-gpu-shader-compiler` (dumps in `~/tmp/w16si/sh/`).

1. **`RenderBeams`**: each spotlight's NG shaft mesh (`Spotlight::BuildNGShaft`)
   is drawn with `depthvolume` into a half-size A8R8G8B8 target cleared to
   (0, 0, 0, 1), blend ONE/ONE add, with no depth test.
   - The shape picks the option: `mShape` 0/1 → cone, 2 → sheet, 3/4 → sphere.
   - The cone uses cull override 3 (D3D CCW, the reverse of a material's CW),
     so the far side of the shaft draws. The sheet and the sphere cull nothing.
2. **The cone PS** intersects the view ray with the analytic cone. The apex
   is `c25`, the axis `c26`, and `c28.w` is cos² of the half angle. The ray
   is clipped by the scene's view depth and the pixel's own distance. It
   writes `c90.rgb · 0.004 · |Δviewdepth| · mean((1 − s)²) · xsection`.
   - The literals are verified in the disassembly: `c254 = (0.004, 1/3, 0, 0)`
     and `c255 = (0.5, −0.5, 0, 1)`.
   - The mean has the closed form `(oE³ − oX³) / 3 / Δs`.
3. **`BlurRT`**: 5 taps, weights 0.1 / 0.25 / 0.3 / 0.25 / 0.1, along x and
   then along y. The retail statics are `sSeparateBlurPasses` = 1 and
   `sBlurAmount` = 1.0 (0x82C711C4 / 0x82C711C0).
4. **The composite**: `postprocess` option bit 51, after the bloom screen
   blend and before the colour transform. Verified from
   `postprocess_0008000000000000.ps`:

   ```
   c += beam.rgb · (tf5.r · c127.y + c127.x) · c91.x
   ```

   - `c91.x` = `mIntensity · 32`.
   - `c127` = (base · 0.01, smoke · 0.01 · (1 − base · 0.01)). The 0.01 is
     `lbl_82017E70`, read at 0x824D1FF0.
   - `tf5` is the proxy's fog density map, else `mTexture`, else
     `kDefaultTex_Black`.

### 26.2 The port

- **Engine, `gfx/SpotBeamPass`.** The constants are a line-by-line port of
  `RenderConeDefs` / `SetupXSection` / `RenderSheet` / `RenderSphere`, using
  the retail statics:
  - `sBeamIntensity` 8, `sBeamBrighten` 0.1, `sSheetIntensity` 8,
    `sSheetW` 0.5, `sSphereScale` 1, `sFogScale` 0.125.
  - The WGSL ports the three `depthvolume` PS options and the blur.
  - Scene depth comes from the 4× MSAA depth view through `textureLoad`, and
    is unprojected through the inverse view-projection.
  - The fog terms `c127.zw` are 0 while the beams draw
    (`SetupFogDensityState`), so the density map never reaches the beam
    shaders.
- **Engine, `gfx/RB3RetailPost`.** Bit 51's add, on two new bindings (beam
  target, fog density). The fog binding is black unless the drawer has
  `mTexture` and no proxy. `MILO_RB3_RETAIL_POST=beams` shows the blurred
  target.
- **Engine, `platform/SpotBeamHook`** (`NativeSpotBeamRenderer`). This is the
  seam. `WgpuRnd` registers a renderer when the retail post chain is on and
  `MILO_NO_SPOT_BEAMS` is unset. It resolves the meshes, cross-section
  textures and camera, runs the pass in `FlushWorldPost` before the post
  chain, and hands the target to the composite.
- **rb3, `SpotlightDrawer::DrawWorld`** (under `HX_NATIVE`). rb3 runs
  `kOldGfx`, so this path is gated on a renderer being registered. When one
  is, `DrawNGSpotlights()` is true, and each showing beam goes into a
  `NativeSpotBeam` instead of the old-gfx `DrawBeams`. The frame is submitted
  once, after the loop. `RB3_SPOT_BEAM_LOG` prints the drawer's parameters
  and each beam for the first three frames.

### 26.3 Two rb3 defects, both against target asm

With only the port in place, the beams showed as a faint haze (about +6
levels on the street) and their shapes were inconsistent. Both came from rb3
source.

- **`SpotDrawParams::Load`, rev > 3.** The Wii target (0x8086F300) reads
  offsets 0x14 / 0x18 / 0x1c = base, smoke, half distance, and reads
  lighting influence (0x20) only when rev > 4. rb3 read smoke, half,
  lighting, so base intensity kept its 0.1 default. The title drawer then
  composited at weight 0.032 instead of 0.32.
  - Fixed. The title drawer now reads intensity 1, base 1, smoke 1, half 250,
    with no texture and no proxy.
  - rb3-xenon's `Load` is already right.
- **`Spotlight::UpdateTransforms`.** Both targets give the beam mesh the
  identity rotation when `mIsCone` is set and the spotlight rotation
  otherwise: Wii 0x80860464, 360 0x824D93CC. In the Wii objdiff, `r7 = rot`
  and is overwritten with `ident` on `mIsCone != 0`. rb3 had the ternary
  reversed, so the NG cone mesh pointed along the spot's +z while the shader's
  analytic cone runs along `m.y`. The shaft and the analytic cone then
  overlapped only in part.
  - Fixed. The log shows mesh y == spot `m.y` for all 12 title beams.
  - ⚠ **rb3-xenon has the same reversed ternary** (`UpdateTransforms` 99.66%).
    It was not touched here, only reported.

Wii matching build, measured in a `tools/setup-worktree.sh` worktree at
`775737cba`, after touching both TUs so they recompiled. The script's 2020
source mtimes otherwise leave the warm objs in place, and the first read was
byte-identical to main for that reason.

| row | main | this lane |
|---|---|---|
| `SpotDrawParams::Load` (548 B) | 99.905 | **99.927** (the three field-offset `diff_arg`s gone; the rev < 4 locals' stack slots remain) |
| `Spotlight::UpdateTransforms` (2,392 B) | 79.881 | 79.881: Δ0, because both arms' addresses were already charged (target `lis/addi` per static, ours an `r31` base) |
| whole binary | 31,944 fns / 7,223,260 B | unchanged |

### 26.4 Retail data that differs from rb3-xenon's source

These were read from the retail image while porting. They are reported here
and not fixed in rb3-xenon:

| static | retail | rb3-xenon source |
|---|---|---|
| `sSheetW` (0x82C71198) | 0.5 | 0.0 |
| `sSeparateBlurPasses` / `sBlurAmount` (0x82C711C4 / C0) | 1 / 1.0 | false / 0.5 |
| `sFogScale` (0x82C711CC) | 0.125 | 1 |

### 26.5 Results

Title, frames 60/200/400, mean luma (TCRF is a single frame). Region script
`~/tmp/w16rz/street_regions.py`; road window as in 25.

| region | TCRF | no beams (`MILO_NO_SPOT_BEAMS=1`) | beams, final | \|dRGB\| to TCRF, before → after |
|---|---|---|---|---|
| street_R | 92.8 | 37.3 | **108.7** | 49.5 → **15.8** |
| street_L | 60.2 | 65.3 | 94.0 | 7.9 → 34.7 |
| crowd | 104.3 | 112.6 | 146.3 | 6.7 → 40.9 |
| road window | 98.6 | 49.1 | 124.4 | |

- **The beam draws.** In the beams view (`~/tmp/w16si/capB3`) the cones
  appear with notches where buildings occlude them. In the composite
  (`~/tmp/w16si/quad3.png`, `full3.png`), a hazy cone falls over the street
  with a building notch at its top, as in TCRF. There are 0 WebGPU errors in
  every capture.
- **street_R closes most of section 22's gap**, overshooting TCRF by about
  16 luma.
- **The haze reaches too far, and its colour is wrong.** Native's beam light
  is more orange-red and spreads over the Capitol and the crowd. TCRF's is
  yellower and sits lower right. As a result, street_L and crowd moved
  *away* from TCRF.
  - Not resolved. Candidates: TCRF predates TU5, so its title light state
    may differ; some of the 12 beams (for example the subway ones) may be
    off in TCRF's preset; and the packed colour decode is the same one
    section 25 used.
  - Section 25's open question still stands. xenia's hub frame runs retail
    code, which reads `Load` correctly, and shows no cone, so either the
    hub's light state differs or xenia does not render the pass.

### 26.6 Verification

| check | result |
|---|---|
| new `rb3-tests` `SpotBeamConstants.*` (3) | constants against hand-worked `RenderConeDefs`, sheet/sphere, composite and blur taps |
| new `SpotBeamPassTest.ConeMatchesShaderModel` | the real pass on the device against a CPU model of the cone PS and two-pass blur, both quantised to 8 bits: **max \|gpu − model\| 1/255** over 64×64. Predicted red at texels 24 / 40 on the centre row ≈ 29 / 11, measured 26 / 11 |
| `ConeCullsFrontFaces` / `SceneDepthOccludesBeam` | 0 lit texels, with the opposite winding / with depth at view depth 100 (the model test lights 170 texels with the same inputs, so both can fail) |
| WGSL validation | `gfx/SpotBeamPass.cpp OK` |
| `native_compat_census.check` | OK, 440 flags (`MILO_NO_SPOT_BEAMS` and `RB3_SPOT_BEAM_LOG` classified as probes) |
| rb3 native ctest (`build-native`, dc3) | **147 tests, 100% passed**, 7 skipped (the usual real-capture fixtures) |
| title capture, final binaries (`~/tmp/w16si/cap4`) | 0 `GpuDevice: WebGPU error` lines; regions within run spread of `cap3` |

### 26.7 Not done

- **The fog density map from a proxy** is not rendered. The seam carries
  `hasProxy` and the composite then reads black. The title drawer has no
  proxy.
- **The sheet and sphere shaders** are ported and validate, but the title
  has no such beams, so they are untested against retail output.
- **26.5's colour and extent gap** is open.
- **rb3-xenon** is untouched: its reversed `UpdateTransforms` ternary and
  the 26.4 statics are for a matching lane.
- **No merge, pin bump or push.** That is for the coordinator.

## 27. Where section 26's extra haze comes from (lane W16-SR, 2026-10-07)

Section 26.5 left the title haze too red and too wide: street_L 7.9 → 34.7,
crowd 6.7 → 40.9. This lane looked for the cause and did not find a defect
in the pass. The extra haze comes from **two of the twelve beams,
`Spotlight02` and `subway_bridge`**. With those two removed, every region
lands within a few levels of TCRF. The other ten beams reproduce TCRF where
TCRF shows beam light.

No code mechanism was found by which retail drops those two. Every input
the pass reads was checked against retail and matches (27.3). **So no engine
change is committed.** A name-based skip would only fit the score to one
still image. This section is the measured explanation.

| repo | branch | commit |
|---|---|---|
| milo-native-engine | `w16-sr` | this section only (on `e8aaf63`) |
| rb3 | `w16-sr` (local worktree) | diagnostics only, not committed |

### 27.1 Per-beam attribution

Before measuring, I expected the excess to be a pass-wide error: wrong
culling, depth or the cross section, which would scale every beam. That was
wrong. The beams were isolated with a temporary rb3 skip list
(`RB3_SPOT_BEAM_SKIP`, substring match in `AddNativeBeam`), all at frame 200,
`RB3_FIXED_CLOCK=1`, dc3 flavour. The table gives \|dRGB\| to TCRF, with
luma in brackets.

| leg (beams drawn) | street_R | street_L | crowd |
|---|---|---|---|
| none (`MILO_NO_SPOT_BEAMS=1`) | 50.6 (36.1) | 8.1 (64.2) | 6.8 (112.5) |
| all 12 (section 26 as landed) | 14.5 (107.3) | 35.2 (94.6) | 42.0 (147.3) |
| `Spotlight01` only | **10.3** (88.1) | 8.1 (66.4) | 13.3 (119.8) |
| `Spotlight02` + `subway_bridge` only | 17.7 (67.6) | **34.2** (93.5) | **37.9** (142.3) |
| `subway`, `subway01`, `subway03` only | 50.7 | 8.1 | 7.4 |
| `Spotlight05`–`08` only | 50.4 | 8.1 | 7.7 |
| `Spotlight03`, `Spotlight04` only | 50.4 | 7.8 | 7.0 |
| all but `Spotlight02` + `subway_bridge` | **10.3** (87.9) | **8.2** (64.1) | **13.4** (119.9) |

Frames 60/200/400 averaged for the three main legs: none 49.6 / 8.2 / 6.6,
all 15.6 / 35.1 / 41.8, and all-but-two **11.4 / 8.0 / 13.2**.

The contributions add up: the all-but-two leg equals `Spotlight01` alone to
within 0.2 luma in every region. Upper-image regions, by luma (frame 200):

| region | TCRF | none | all 12 | `02`+`bridge` only | all but those two |
|---|---|---|---|---|---|
| facade_lo (380,470,560,560) | 67.7 | 64.1 | 84.3 | 82.7 | 65.7 |
| facade_hi (330,380,520,460) | 32.1 | 47.8 | 54.5 | 54.3 | 48.1 |
| marquee_L (250,560,380,640) | 41.9 | 44.8 | 64.1 | 64.2 | 44.8 |
| sky_far_R (1000,300,1250,380) | 36.8 | 27.8 | 39.4 | 27.8 | 39.1 |

- **`Spotlight01` is section 25's street lamp.** Alone, it adds
  (+74, +47, +18) RGB over the no-beams frame at street_R. TCRF's excess over
  the no-beams frame there is (+65, +58, +29). The size matches, and the hue
  is close.
- **`Spotlight05`–`08`** are the far beams. They light sky_far_R from 27.8
  to 39.2 luma, against 36.8 in TCRF.
- **`Spotlight02` and `subway_bridge`** account for all of the street_L,
  crowd, facade and marquee excess. At marquee_L, TCRF sits *below* the
  no-beams frame (41.9 vs 44.8), so it holds none of their +19 luma. At
  facade_lo, TCRF's red is +11 over no-beams against their +44, so at most
  about a quarter.
- **"Too red" is their colour.** Both carry colour (0.537, 0.267, 0.267) at
  intensity 0.45, read from the file bytes. Every other beam is warm-white
  or yellow.

### 27.2 Why those two are different

Read from `sv8_a.milo_xbox`, decompressed, with blobs located by their
`0xADDEADDE` separators and checked against the `[DefDiag]` log:

- They are a **copy pair**. Same rotation (axis = `m.y` =
  (0.2526, −0.3746, 0.8921)), same local sphere ((−68.89, 371.98, 51.38),
  r 392.97), same beam (cone, length 550, top 40, bottom 250) and same
  colour. Only the position differs: (−498, 508, −342) and (−805, 314, −598).
  No other beam has top radius 40 or intensity 0.45.
- They are the only beams that **point at the camera from outside their
  cone**. The camera, `world.cam` at (110.8, 60.1, 300) with no parent
  (`[CamDiag]`), lies past the open end of both:

  | beam | axial distance / length | radial / cone radius there | axis·view |
  |---|---|---|---|
  | `Spotlight01` | 939 / 600 | 104 / 264 (inside the extended cone) | 0.994 |
  | `Spotlight02` | 895 / 550 | 429 / 382 (outside) | 0.902 |
  | `subway_bridge` | 1128 / 550 | 662 / 471 (outside) | 0.862 |

  Both lamps project just below the frame, at screen (0.49, 1.00) and
  (0.30, 1.13). Each cone opens toward the viewer and covers the lower-left
  of the image.
- `SetupXSection`'s fade gives them `c86.x` = 1. For `Spotlight02`,
  `fade` = 0.930 and axis·view = 0.902, so the slack is 0.028 (≥ 0.02). The
  cross section is therefore applied. With no `mXSection`, it is
  `SR().unk14` = `mDefaultTex[2]`, which retail reads at
  `lwz r11, 0xb8(r11)` in `CheckRTs` (0x824D1D70). That slot's
  `sDefColor` row is `0xFFFFFFFF` (`lbl_8205EA68`), white. So the factor is
  1, and the cross section does not dim them.

### 27.3 Inputs checked against retail (all equal)

| input | retail evidence | native |
|---|---|---|
| cone PS math | `depthvolume_0000000000000000.ps` re-read: maxT = `min(linear tf9, sqrt(dist²))` (`mins r1.w, r5.zw`), nappe selection, mean falloff, Δviewdepth; **no `c29` read** | same |
| cull | override 3. **`Spotlight01` proves the far shell:** its camera is past its wide cap, inside the extended cone. With front faces only, maxT would be the cap distance and the whole segment would have s > 1, so falloff would be 0 and the beam would be black. TCRF shows it lit | far shell (`CullMode::Front` under CCW-front) |
| cross section | white, as above | white 1×1 |
| target format and blend | A8R8G8B8, clear (0,0,0,1), ONE/ONE | RGBA8Unorm, same |
| composite weight | `c91.x` = 32, `c127` = (0.01, 0.0099); `tf5` black | same (drawer: intensity 1, base 1, smoke 1, half 250) |
| beam colour | `colorOwner->mIntensity · 8 · mColor · brighten`; no material | same, owner = self |
| queueing | `DrawLight`: packed key (61, 30, 30) passes; `mTargetLoaded` (no target name); `Showing()` 1 | queued |
| frustum cull (`RndDrawable::Draw`, mSphere · WorldXfm) | sphere centres at view depth 987 / 1171, r 393, inside the frustum | culling off |
| light state | no `LightPreset` in `sv8_a`; no `PropAnim`, `EventTrigger` or group names either spot (each name occurs exactly twice in the decompressed dir: entry table and own `mColorOwner`); no other `.milo_xbox` or `.dta` names `subway_bridge` | file state |
| `SpotlightEnder` / `UpdateBoxMap` | rebuilds `sGlobalLighting` only; never touches `sLights` | — |
| camera | `CheckCam` copies `TheWorld->Cam()` and drops the parent. `world.cam` has no parent, so its local and world transforms are equal | `world.cam` world |

### 27.4 What remains open

- **The only retail reference is one still image.** TCRF's frame differs in
  time from frame 200: the train is absent from the bridge. TCRF also
  possibly predates TU5 (26.5).
  - The xenia hub frame cannot help. Per section 25.3, it renders no cone
    at all.
  - Its facade_lo (64.8) and marquee_L (43.8) values match the no-beams
    frame, but that only shows that xenia lacks the pass. It is not
    evidence about these two beams.
- **Untested lead.** Retail may write scene depth for geometry that native
  skips, for example meshes without a material, which section 25 notes the
  engine does not draw. That depth would cut the two segments that run
  toward the viewer. A material-less or depth-only mesh between the camera
  and the street would show this. It was not enumerated.
- **Side finding, not fixed.** Native's `RndCam::WorldFrustum()` reports
  every spotlight sphere as culled from this camera. The test with the
  correct frustum culls none. Frustum culling is off natively
  (`RB3VenueFrustumCull`), so nothing uses it today. Turning culling on
  without fixing the frustum would drop all twelve beams.

### 27.5 Not done

- **No engine change.** The pass is unchanged from `e8aaf63`. Skipping the
  two beams by name would score street_L 8.0 and crowd 13.2 (from 35.1 /
  41.8), but it would fit one screenshot without a retail mechanism behind
  it.
- **The rb3 diagnostics** (`RB3_SPOT_BEAM_SKIP`, `[DefDiag]`, `[CullCam]`,
  `[CamDiag]`, `[PresetDiag]`) stay in the uncommitted `~/tmp/wt-w16sr-rb3`
  worktree.
- **No merge, pin bump or push.** That is for the coordinator.

## 28. Section 27's two open items: material-less meshes and the world frustum (lane W16-SU, 2026-10-07)

Section 27.4 left two items. The first was an untested lead: retail may write
scene depth for meshes that native skips, for example meshes without a
material, and that depth would cut the `Spotlight02` / `subway_bridge` beams.
The second was a side finding: native `RndCam::WorldFrustum()` reported every
title spotlight as culled. This section tests the first against retail and
fixes the second.

| repo | branch | commit |
|---|---|---|
| milo-native-engine | `w16-su` | `452b184` + this section (on `e093b66`) |
| rb3 | `w16-su` | `44b2d10c4`, `57de56778` (on `e3234ee2e`) |

The instrument is section 27's: `title_capture.sh`, `RB3_FIXED_CLOCK=1`,
frames 60/200/400, dc3 flavour, \|dRGB\| to TCRF per region
(`~/tmp/w16rz/street_regions.py`, `~/tmp/w16sr/facade.py`). Captures are in
`~/tmp/w16su/`.

⚠ **Per-pixel diffs are not usable here.** Two runs of the same binary differ
by a mean of 5.4 levels, with 22% of pixels above 8 levels, spread over the
whole frame. That is as large as any change measured below. So every
comparison uses the region means over three frames, and leg `fi2` (a second
run of the `fi` binary) is the noise control.

### 28.1 Material-less meshes: retail draws them, but they do not cut the beams

**What retail does.** Retail `DxMesh::DrawShowing` (rb3-xenon 0x82738E38)
reads `Mat()` from 0xf8. When it is null, the function sets `r31 = 0` and calls
`RndShader::SelectConfig(null, kStandardShader, false)` (0x824A5740). It then
calls `DrawFaces` through vtable slot 0x38. `RndShaderStandard::Select`
(0x824A8080, 100% matched) begins with `if (!mat) mat = TheRnd.DefaultMat();`.
The default material comes from `Rnd::Init`, with `use_environ` 0 and
`pre_lit` 1. So retail draws a material-less mesh, colour and depth, as
vertex colour times white. dc3-decomp's `DxMesh::DrawShowing` and `Select`
do the same. The engine refused such a mesh twice: `"no material"` in both
`RndMeshDrawShowingSkip` and `DrawMeshImmediate`.

**What the title has.** This is a census from the frame capture
(`MILO_CAPTURE_FRAME=200`). The frame records 888 mesh submissions: 881
draws and 7 skips.

- 6 skips are `Man_scale01/02/04.mesh` in two passes. These meshes have no
  vertices, so retail's `CanDraw` refuses them as well.
- 1 skip is the only material-less mesh, **`kick_drum_01.mesh`**. It sits at
  NDC (0.569, −0.942), which is screen ≈ (1004, 699), at the bottom right.
  `street_L` and `crowd` span x 300–590.
- Nothing is filtered by the consumer (rb3's `ShouldSkipMesh` returns false).

So, apart from those no-vertex meshes, native submits every mesh on this
frame. One mesh is not drawn, and it is not between the camera and the beams.

**Prediction:** drawing it as retail does leaves every region within noise.

**The fix** (`452b184`): `rndshape::DefaultMat()` is a new accessor on both
shapes (Wii `TheRnd->mDefaultMat`, DC3 `TheRnd.DefaultMat()`).
`DrawMeshImmediate` uses it for a null `Mat()`, and `RndMeshDrawShowingSkip`
no longer refuses. The capture now records
`DRAW mesh='kick_drum_01.mesh' mat='' … prelit=1`, with 882 draws and 6 skips.

| region | TCRF luma | `fi` | `fi2` (noise) | `dm` (+ default material) |
|---|---|---|---|---|
| street_R | 92.8 | 15.7 | 15.6 | 15.9 |
| street_L | 60.2 | 34.1 | 34.7 | 34.3 |
| crowd | 104.3 | 41.6 | 41.6 | 42.0 |
| drum box (940,660)–(1080,720), luma | | 40.5 | 40.5 | 40.6 |

The values are \|dRGB\| to TCRF, the mean of frames 60/200/400. The
prediction held. **The lead does not hold.** The beams cannot be cut by a
material-less mesh on this frame, because the frame has none in their path.
The retail behaviour is committed anyway, because it is retail's. The
`Spotlight02` / `subway_bridge` excess (27.1) remains unexplained.

### 28.2 The world frustum: `FastInvert(Matrix3)` was not an inverse

Before measuring, I expected a bad aspect ratio or a stale world transform in
`RndCam::UpdateLocal`. That was wrong. `UpdatedWorldXfm` builds
`mWorldFrustum` with `Multiply(Frustum, Transform)`. That function moves each
plane with `Multiply(Plane, Transform, Plane)` (`math/Geo.cpp`), and the plane
multiply inverts the rotation with `FastInvert(t.m, invM)`.

- **The defect.** rb3's `FastInvert(Matrix3)` scaled each row by its own
  1/\|row\|² and did not transpose. For a rotation, that returns the rotation
  itself rather than its inverse. The frustum planes were therefore turned by
  the inverse of the camera's rotation. A camera with a nontrivial rotation
  (`world.cam`) then had a frustum that faced away from what it sees.
- **The target.** The Wii target (`FastInvert__FRCQ23Hmx7Matrix3RQ23Hmx7Matrix3`,
  0x80401560) computes x, y and z = 1/\|row\|². It stores `xz·x` to 0x18,
  `xx·x` to 0x0, `yx·y` to 0x4, `zx·z` to 0x8, `xy·x` to 0xc, and so on. So
  `out.x` = (xx·x, yx·y, zx·z): the transpose. rb3-xenon (`math/mtx.cpp`) and
  dc3-decomp already have the transposed body.
- **The fix** (rb3 `44b2d10c4`, then `57de56778` in the target's load and
  store order). All nine loads come before any store, so the in-place callers
  (`FastInvert(evalMat, evalMat)` in `CharBonesSamples`) stay correct.

Results, from section 27's `[CullDiag]` probe (`RndCam::Current()->CompareSphereToWorld`
on each spotlight's world sphere), on the first queued frame:

| | culled of 12 |
|---|---|
| before | **12** |
| after | **0** (the result section 27's independent frustum gave) |

Frustum culling is off natively (`RB3VenueFrustumCull`, opt-in with
`RB3_VENUE_FRUSTUM_CULL=1`), so the fix should not move the default image:

| region | `base` | `fi` (fix) | `fi2` (same binary) | `ficull` (fix + `RB3_VENUE_FRUSTUM_CULL=1`) |
|---|---|---|---|---|
| street_R | 15.7 | 15.7 | 15.6 | 15.7 |
| street_L | 35.1 | 34.1 | 34.7 | 34.4 |
| crowd | 41.4 | 41.6 | 41.6 | 41.3 |

- With culling on and the fixed frustum, frame 200 draws **870 meshes instead
  of 881**, and every region holds.
- All 12 beams still queue on most frames. `subway`, `subway01` and
  `subway03` are culled on some frames: they queue 181, 181 and 233 times out
  of 418. These are the three beams that section 27.1 found contribute nothing
  to the measured regions.
- The comment on `RB3VenueFrustumCull` (rb3 `rndobj/Draw.cpp`) says native
  culling was disabled because "the baked Xbox mSphere was wrong, so culling
  dropped visible meshes". With this defect, any rotated camera culled with a
  mirrored frustum. That explains dropped meshes without wrong spheres. This
  was not tested beyond the title frame, and the default was not flipped.
- **Other callers also got wrong results before**, since `FastInvert(Transform)`
  wraps this function: `TransformNormal`, `RndMesh::CollideShowing` and
  `CollidePlane`, `RndMultiMesh::MakeWorldSphere`, `AttachMesh`, and the
  `UpdateSphere` of `Spotlight`, `Character`, `Mesh`, `Group`, `Generator`,
  `Dir`, `Line` and `ParticleSys`, plus `CharServoBone`, `CharIKMidi`, `CharCuff`, `BandCharacter` and
  `CameraShot`. The title regions do not move, and the full native ctest
  passes. Nothing else was audited for visible change.

Wii matching build (`tools/setup-worktree.sh` at `e3234ee2e`, with `Rot.cpp`
touched on every leg so it recompiles):

| `FastInvert(Matrix3)` (192 B) | fuzzy |
|---|---|
| main (untransposed) | 85.56 |
| `44b2d10c4` (transposed `Set`) | 77.10 |
| `57de56778` (target load and store order) | **82.81** |

The whole binary is 31,944 fns / 7,223,260 B on every leg, and no other row
moves. The score drops 2.75 points for a body whose values are now the
target's. The residual is register allocation and scheduling. Among the
variants tried, a `Dot()`-based body scored 54.3 and a fully inlined
`min.*` body 64.0.

### 28.3 Final state

`final` is the committed engine and rb3 state, without section 27's
diagnostics. Values are \|dRGB\| to TCRF over three frames; facade rows are
frame-200 luma.

| region | TCRF | base (as section 27 landed) | final |
|---|---|---|---|
| street_R | 92.8 | 15.7 | 15.8 |
| street_L | 60.2 | 35.1 | 34.5 |
| crowd | 104.3 | 41.4 | 41.4 |
| facade_lo (luma) | 67.7 | 84.2 | 84.3 |
| marquee_L (luma) | 41.9 | 64.1 | 64.3 |
| sky_far_R (luma) | 36.8 | 38.9 | 38.6 |

These match section 27's own reproduction (15.6 / 35.1 / 41.8), and the
section 27.1 excess is unchanged. Both items are closed: the first as a lead
that does not hold, the second as fixed. Neither touches the haze.

### 28.4 Verification

| check | result |
|---|---|
| new rb3 tests `NativeSubsystems.FastInvertIsTheInverseOfAScaledRotation`, `WorldFrustumFollowsAYawedCamera`, `MeshWithoutMaterialIsNotRefused` | pass |
| sabotage: old `FastInvert` body + the engine's `"no material"` refusal restored | **all three fail** (product off-identity; the sphere 900 ahead tests as outside; `refused a material-less mesh: no material`) |
| rb3 native ctest (`build-native`, dc3) | **150 tests, 100% passed**, 7 skipped (the usual real-capture fixtures) |
| title captures | 0 `GpuDevice: WebGPU error` lines in every leg |
| DC3 shape | `Mesh_Wgpu.cpp` passes `-fsyntax-only` with dc3-decomp's compile command pointed at this tree (its PCH dropped, since it was stale) |

### 28.5 For the coordinator

- **rb3 and the engine move together.** rb3's `MeshWithoutMaterialIsNotRefused`
  fails against the current pin `e8aaf63`, so bump the pin with the rb3 branch.
- **dc3-decomp's pin bump will fail one control.**
  `NativeSuspectsTest.MeshDrawShowingDrawsAHiddenNamedMesh`
  (`native/tests/test_native_suspects.cpp:271-274`) uses a material-less mesh
  as its "the predicate can refuse" control. That mesh no longer refuses. The
  same file's `grid_80by60_cube.mesh` assertion (line 287) still shows that the
  predicate can refuse.
- **rb3-xenon** is untouched. Its `FastInvert` is already transposed.

### 28.6 Not done

- The `Spotlight02` / `subway_bridge` excess (27.1) is still open. Its last
  named code lead is closed above.
- `RB3_VENUE_FRUSTUM_CULL` stays opt-in. Making it the default needs captures
  beyond the title.
- **No merge, pin bump or push.** That is for the coordinator.

## 29. The flares' bloom mask and colour factor, from retail (lane W16-SZ, 2026-10-07)

Section 19.8 left three title items: the 360 flare colour factor, the bloom
mask, and the star-shaped flare highlights TCRF shows. They turn out to be
linked. The engine's rect path wrote a **solid square of bloom mask** under
every flare, so the bloom turned each star into a wide wash. With the mask
written as retail writes it, the stars appear, and the retail colour factor
(1.0, not the Wii's 0.6) then matches TCRF's flare cores. Both are settled on
retail code and committed.

| repo | branch | commit |
|---|---|---|
| milo-native-engine | `w16-sz` | `ca6b466` + this section (on `570c305`) |
| rb3 | `w16-sz` | `bc92aea59` (on `1374ddb96`) |

The instrument is section 28's: `title_capture.sh`, `RB3_FIXED_CLOCK=1`,
frames 60/200/400, dc3 flavour, Debug build, means over the three frames.
Scripts and captures are in `~/tmp/w16sz/` (`measure.py` = `title_fidelity.py`'s
sky_dE / city_dE plus section 13.2's regions and section 20's flare rects;
`core.py` = flare cores). **Noise control:** two runs of the base binary
differ by at most 0.08 in city_dE and 0.6 in any flare rect, and two runs of
the 1.0 binary by at most 0.8. A probe build with the factor at 0.6
reproduces the base binary within that.

### 29.1 The colour factor, on retail bytes

rb3-xenon `RndFlare::DrawShowing` (0x82477270, 98.5% fuzzy) computes
`alpha = clamp(t · ratio)` with `fsel` against `f30` and `f31`. The PE holds
`f31 = lbl_820009FC = 1.0` and `f30 = lbl_82000D78 = 0.0`. It then moves the
result into all three channels (`fmr f13, f0; fmr f12, f0`), optionally
multiplies by `RndEnviron::sCurrent->mAmbientFogOwner->mAmbientColor`
(`lwz 0x7c` = the owner pointer at 0x74 + 8, colour at 0x64), and stores it at
mat + 0x2c/0x30/0x34. **There is no 0.6.** The Wii source rb3 builds
multiplies by 0.6. The ambient term is the same field rb3's `AmbientColor()`
returns, so that is not a difference.

### 29.2 First measurement: the factor alone moves away from TCRF

Before the change I expected 1.0 to brighten the flares toward TCRF's, as
section 19.5's `city_mid` did. Measured with the factor alone (probe binary,
env-switched; values are means of f60/200/400):

| leg | city_dE | frame luma | `lamp01` | `lamp02` | `lamp03` | `lamp04` |
|---|---|---|---|---|---|---|
| TCRF | 0 | 50.1 | 124.9 | 144.1 | 83.3 | 39.9 |
| base (0.6), 2 runs | 12.81 | 59.1 | 167.7 | 189.6 | 109.3 | 64.1 |
| factor 1.0, 2 runs | 14.77 | 60.6 | 183.5 | 199.6 | 126.1 | 76.0 |
| factor 0 (flares drawn black) | 9.86 | 56.5 | 134.7 | 168.0 | 80.0 | 44.8 |

(Flare columns are mean luma in section 20's flare rects.) The prediction
failed, and the last row says why rect luma cannot settle the factor: with
the flares drawn black, `lamp01`'s rect is still brighter than TCRF's with
its flare. The scene has grown brighter since section 20 (frame luma then
49.2, now 59.1), mostly from the beam haze of section 26.

Side by side (`~/tmp/w16sz/lamps_crop.png`), TCRF's flares are **sharp star
cores** with a modest glow. Native's are **round washes with no core** at
either factor. The flare's own contribution (1.0 minus 0,
`flare_contrib.png`) is a smooth disc.

### 29.3 Cause: each flare wrote a square of bloom mask

A temporary probe in `RndFlare::DrawShowing` logged the material for every
flare: all eight use `flare_light_can_star.tex` (256×256, DXT1, order 0x8, no
mips), `texgen` 1 (`kTexGenXfm`). The lamps' `flare_lamp01.mat` is blend 2
(`kBlendAdd`) and the blinks' `flare_red_blink.mat` is blend 4
(`kBlendSrcAlphaAdd`).

- **The texture is a star.** The probe drew `Flare_lamp02` as a 576×576
  rect. With `MILO_RB3_RETAIL_POST=raw` it shows a star with rays; with the
  retail chain the same frame is a white wash over the whole middle
  (`~/tmp/w16sz/big.png`).
- **The mask view shows squares.** `MILO_RB3_RETAIL_POST=mask`, f200: each
  lamp's rect is solid white, means 254.1 / 242.5 / 251.0 / 254.0 for
  `lamp01`–`04`, and each blink is a grey square (63.7, 63.0).
- **Why.** `DrawRect2D` set `writeMask = All` and wrote
  `texel.a · colour.a` to alpha. DXT1 alpha is 1, so every flare wrote mask
  1 over its whole rect, and the bloom spread a square of whatever lay
  under it. Meshes (`Mesh_Wgpu`) and particles (`Part_Wgpu`) already follow
  retail's rule; the rect path did not.

**Retail's rule for a rect** (rb3-xenon, every function 100% matched):

| function | what it does |
|---|---|
| `DxRnd::DrawRect` (6-arg) | draws with `kDrawRectShader` |
| `RndShaderDrawRect::Select` | selects the **standard** shader with `CalcShaderOpts`'s options, then `SetColorWriteMask(opts, mat)`; a null material is `DrawRectMat` |
| `RndShaderDrawRect::CalcShaderOpts` | pseudo-HDR (bit 22) = `!Offscreen() && mat->AllowHDR()` |
| `SetColorWriteMask` (0x824A5AF8) | RGBA if bit 22, `Offscreen()` or `mat->mAlphaWrite`; otherwise **RGB only** |
| `NgMat::AllowHDR` | not SrcAlpha, SrcAlphaAdd or PreMultAlpha; not alpha-cut; not alpha-write |

So in the main frame a lamp flare (Add) writes `a = dot(rgb, c7)`, star
shaped, and a blink (SrcAlphaAdd) or a material-less rect leaves alpha alone.

### 29.4 Change

- **Engine** (`ca6b466`, `gfx/DrawRect2D.cpp`): in the main frame
  (`CurrentPassHasDepth()`, no target) with `rndshape::RetailBloomMaskActive()`,
  a rect writes alpha = clamp(luma(rgb) · `BloomMaskScale(mat)`) for an
  `AllowHDR` material (a WGSL override constant, as `Part_Wgpu` uses), its own
  alpha for an alpha-write material, and RGB only otherwise. Render targets
  and the DC3 shape (`RetailBloomMaskActive()` is false) are unchanged.
- **rb3** (`bc92aea59`): under `HX_NATIVE`, `RndFlare::DrawShowing` scales by
  `gNativeFlareColorScale` (default 0.6, the Wii value); the dc3 backend sets
  1 in `InitGpuResources`. The Wii build compiles the same literal as
  before (`#else`), and the BandRnd flavour keeps 0.6.

Prediction for the mask fix: the squares become star-shaped masks, the blink
squares go, frame luma and city_dE fall toward TCRF, and the 0.6 / 1.0 gap
shrinks.

### 29.5 Results

Mask view, f200 (`~/tmp/w16sz/mask_before_after_f200.png`), mean in each
flare rect:

| | frame | `lamp01` | `lamp02` | `lamp03` | `lamp04` | `blink01` | `slow` |
|---|---|---|---|---|---|---|---|
| before | 36.3 | 254.1 | 242.5 | 251.0 | 254.0 | 63.7 | 63.0 |
| after | 24.9 | 64.1 | 89.2 | 43.2 | 21.3 | 19.1 | 18.3 |

Title against TCRF, means of f60/200/400 (`m06` / `m10` are the probe binary
with the mask fix at each factor; `final` is the committed code, no probe):

| leg | sky_dE | city_dE | frame luma | city luma | `city_mid` | `city_rmid` |
|---|---|---|---|---|---|---|
| TCRF | 0 | 0 | 50.1 | 52.4 | 55.2 | 66.5 |
| base (2 runs) | 13.63 | 12.81 | 59.1 | 64.0 | 69.8 | 84.4 |
| mask fix, 0.6 | 13.26 | **8.10** | 54.8 | 58.1 | 62.3 | 72.8 |
| mask fix, 1.0 (2 runs) | 13.35 | 8.85 | 55.9 | 59.6 | 63.7 | 75.2 |
| **final** | 13.36 | 8.84 | 55.9 | 59.6 | 63.7 | 75.2 |
| mask fix, flares black | 13.20 | 8.20 | 53.5 | 56.3 | 61.1 | 70.2 |

**Flare cores** decide the factor. `core.py` takes the brightest 9×9 mean
within 12 px of each lamp's centre, and its contrast over a ring at
r = 40–56 px (core / core − ring):

| lamp | TCRF | base | mask fix, 0.6 | mask fix, 1.0 | **final** |
|---|---|---|---|---|---|
| `lamp01` | 251.4 / 123.3 | 239.6 / 67.8 | 210.6 / 84.8 | 254.0 / 113.4 | **254.0 / 114.1** |
| `lamp02` | 251.5 / 112.3 | 251.7 / 62.0 | 226.9 / 67.5 | 254.6 / 86.6 | **254.6 / 86.8** |
| `lamp03` | 200.2 / 139.6 | 181.5 / 107.4 | 151.3 / 86.2 | 207.1 / 139.9 | **207.3 / 138.1** |
| `lamp04` | 113.7 / 76.7 | 122.6 / 56.3 | 82.4 / 42.0 | 121.1 / 79.0 | **121.3 / 79.6** |

- **The mask fix holds and is the larger change.** city_dE 12.81 → 8.10 at
  the old factor, frame luma 59.1 → 54.8, every region closer to TCRF, and
  sky_dE 0.4 better. With the mask fixed, the flare rects at 0.6 land within
  1.2 of TCRF on `lamp01`, `lamp03` and `lamp04` (124.0 / 82.5 / 38.8).
- **The retail factor holds on the flares themselves.** At 0.6 every core
  sits 25–49 below TCRF's and its contrast is 55–70% of TCRF's. At 1.0 the
  cores agree within 8 and the contrast within 10, except `lamp02`, whose
  ring is brighter than TCRF's (the haze around the theater). The crops show
  the same: at 1.0 the cores are crisp white stars like TCRF's
  (`lamps_crop_fix.png`).
- **What 1.0 costs.** City_dE 8.10 → 8.84 and frame luma +1.1 against 0.6,
  because the extra flare light lands on a background that is already
  brighter than TCRF's (29.2's last row). The flare rects then read 5–15 over
  TCRF. That is the background, not the flare, which the core contrast
  separates. Both legs are far closer to TCRF than the base.
- `final` reproduces the probe's 1.0 legs within the run-to-run spread on
  every figure.

**Section 27's haze.** `lamp01`'s rect overlaps `street_L` and `crowd`, so its
mask square bloomed whatever light lay there, including the
`Spotlight02` / `subway_bridge` haze. \|dRGB\| to TCRF (section 27's
`street_regions.py`, three frames) and luma (`facade.py`, f200):

| region | base | mask fix, 0.6 | final |
|---|---|---|---|
| street_R | 15.7 | 9.8 | 11.1 |
| street_L | 34.4 | 16.0 | 16.1 |
| crowd | 41.7 | 18.9 | 25.9 |
| facade_lo (luma; TCRF 67.7) | 84.2 | | 79.0 |
| marquee_L (luma; TCRF 41.9) | 64.0 | | 54.2 |
| right_bld (luma; TCRF 30.2) | 40.8 | | 32.8 |

About half of section 27's street_L / crowd excess was this square. Section
27's per-beam attribution remains correct as measured. Removing those two
beams also removed the light that the square bloomed.

### 29.6 Verification

| check | result |
|---|---|
| new rb3 tests `RectBloomMaskTest.AdditiveRectMasksByItsLuma`, `SrcAlphaAddRectLeavesTheMaskAlone` (grey non-HDR world, one rect, world end with a bloom postproc, luma 1/16 outside the rect) | pass |
| control inside the test: a white additive rect must bloom past its edge | passes (so the observable can fire) |
| sabotage: `DrawRect2D.cpp` restored from `570c305`, tests rebuilt | **both fail**: black additive rect off by 39 luma, white SrcAlphaAdd by 154; source restored and compared with `cmp` |
| rb3 native ctest (`build-native`, dc3, Debug) | **152 tests, 100% passed**, 7 skipped (the usual real-capture fixtures) |
| title captures | 0 `WebGPU error` lines in every leg |
| DC3 shape | `DrawRect2D.cpp` passes `-fsyntax-only` with dc3-decomp's compile command pointed at this tree (stale PCH dropped). Control: the same command on a copy calling a missing `GetAlphaWriteNoSuch()` in the new branch **fails**, so the branch is compiled under the DC3 shape. (A first control, `-Drndshape=…`, could not fail: a macro rename is consistent across declarations and uses.) |
| Wii build | `Flare.cpp`'s Wii path is the same text under `#else`; the header addition is `#ifdef HX_NATIVE`. Not rebuilt. |

### 29.7 Found, not done

- **Retail blends the mask with MAX.** `DxRnd::SetDefaultRenderStates`
  enables separate alpha blending with `SrcBlendAlpha` 1, `DestBlendAlpha` 1
  and `BlendOpAlpha` 3, and `NgMat::SetBasicState` passes alpha factors (1, 1)
  for every material. On the 360's numbering (`Mat_NG`'s own uses: Blend 0
  ZERO, 1 ONE; op 4 is the Subtract material's reverse subtract), op 3 is
  MAX. So a blended draw's mask is `max(src.a, dst.a)`. The engine's
  `PipelineManager::MapBlend` replaces alpha (One/Zero) for every blend, for
  meshes, particles and rects alike. Where a faint flare edge lies over a
  window, native erases the window's mask and retail keeps it. This is
  engine-wide and `MapBlend` is shared with DC3, so a fix needs the rndshape
  seam. It was not measured.
- **The clamped-colour dot** of 19.2 (retail takes the mask of the unclamped
  colour) is unchanged.
- **`MILO_NO_SPOT_BEAMS=1` is broken on the base engine.** All three legs run
  with it (factor 0, 0.6, 1.0, rb3 probe binary on `570c305`) read frame luma
  ≈171 against 59 without it, so section 27's beams-off control is not
  usable at this commit. Not investigated.
- **The sky hue** (19.8) is untouched.
- **BandRnd** keeps 0.6 and was not built or run.
- **No merge, pin bump or push.** That is for the coordinator. rb3's
  `RectBloomMaskTest` fails against the current pin `570c305`, so bump the pin
  with the rb3 branch.
