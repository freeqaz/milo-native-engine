// RB3 material -> MaterialUniforms translation — extracted VERBATIM from
// BandRnd::DrawMesh in platform/Rnd_Wgpu_RB3.cpp (W1.3.S2). RB3-only TU
// (MILO_ENGINE_GPU_PLATFORM_SOURCES_RB3); the RB3-only twin of DC3's
// gfx/MaterialSetup (MaterialSetup.cpp) — see RB3MaterialBinder.h / W1.3 PLAN
// for why the two are NOT convergeable.
//
// Pure MOVE: every asset-name special-case (crowd-dim, tail_* fret colours,
// track-light surface/rails/gem_smasher_glow/peakstate, gem_force, highlight_*
// UI bar, *_skin_diffuse debug probes) and every function-local `static` cache
// is copied byte-for-byte. The only interior edit is the sole BandRnd member
// reference `mGpu` -> the `gpu` parameter (the lazy diffuse upload stays inline,
// no reorder). isTextMeshHeur / gemForce are hoisted into the result struct.

#include "platform/RB3MaterialBinder.h"
#include "platform/GameRenderHook.h"   // W1.7 B7–B13: relocated material-name classifications

#include "rndobj/Cam.h"
#include "rndobj/Mesh.h"
#include "rndobj/Mat.h"
#include "rndobj/Tex.h"
#include "rndobj/Trans.h"
#include "rndobj/Dir.h"
#include "rndobj/Env.h"
#include "math/Mtx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

// Verbatim relocation of the DrawMesh material->uniform block (W1.3.S2). See the
// header for the interface contract. `mu` aliases the result's uniforms so the
// moved body — which fills a local `mu` — is unchanged.
RB3MaterialBindResult RB3BuildMaterialUniforms(
    RndMesh* mesh, RndMat* mat, bool skinned, RndMesh* owner, GpuDevice& gpu) {
    RB3MaterialBindResult r{};
    MaterialUniforms& mu = r.mu;
    // W5 text-mesh heuristic — mirror the DC3 draw path (Mesh_Wgpu.cpp:188).
    // RndText::UpdateMesh / RndText::CreateLines build per-font sub-meshes via
    // Hmx::Object::New<RndMesh>() (src/system/rndobj/Text.cpp:1766) and never
    // assign a Name, so an empty first byte is a reliable text-mesh
    // discriminator. Every gameplay/scene mesh in RB3 has a non-empty Name(),
    // so the predicate only fires on RndText sub-meshes. This is the same
    // condition MaterialSetup::BuildMaterialParams uses to set
    // useAlphaAsRGB + prelit for text on the DC3 draw path; without it, RB3
    // font atlases (DXT5 with glyphs in alpha, RGB == 0) collapse to black in
    // the shader's non-text diffuse-sampling branch (standard_wgsl.inc:631),
    // which is why menu / song-list / HUD text was invisible in the W4
    // baseline screenshots. See docs/plans/web-port/W5_TEXT_RENDERING.md.
    bool isTextMeshHeur = mesh->Name() && mesh->Name()[0] == '\0';
    // W7 Phase 3 Tier 1 — broaden the "looks like UI text" predicate to also
    // catch dim NAMED meshes that the W5/Phase-1 empty-name discriminator
    // misses. Static analysis (W5 doc + Text.cpp:1766) confirms RndText
    // sub-meshes created via Hmx::Object::New<RndMesh>() ARE caught, but
    // the W5p3 Tier-2 attempt (engine 08b3932) lifted those colours to
    // max(0.6, c) and pixel output was BYTE-IDENTICAL to baseline, proving
    // the residual song-row title + HUD-digit dimness is NOT on the
    // empty-name path. The static trace identifies the missed widgets:
    //
    //   * BandScoreboard digit slots — src/system/bandobj/BandScoreboard.cpp
    //     loads NAMED `num%d.mesh`, `thousands_comma.mesh`,
    //     `millions_comma.mesh`, `%d_source.mesh` from the .milo
    //     (BandScoreboard::SetupScore, lines 56-93). These are textured
    //     digit-sprite quads using a normal RGB diffuse — NOT an alpha-only
    //     font atlas — so useAlphaAsRGB would WRONGLY zero their RGB. We
    //     therefore only apply the colour FLOOR for the broader set;
    //     useAlphaAsRGB / prelit / zMode stay gated on the original
    //     empty-name discriminator (correct for the per-glyph quads of
    //     RndText::UpdateMesh).
    //   * UILabel `*.lbl`-named widgets — drawn through UILabel::DrawShowing
    //     which rewrites the font material's colour per draw from
    //     data-driven UIColors (W5 Phase 3 root cause). The glyph quads
    //     themselves are unnamed (already caught) but in some draw paths
    //     the widget mesh carries the dim mat colour without a sub-mesh.
    //   * Font/label materials by name — RB3 ships fonts as `*font*` /
    //     `*label*` materials; catching the mat-name pattern adds a third
    //     safety net for any text path that escapes both above heuristics.
    //
    // Predicate is conservative: pure name pattern matches, no nullptr
    // deref (Name() pointers and mat pointer all guarded), and the lift
    // uses std::max() so already-bright text (the W5-Phase-1 wins:
    // news-ticker / FRIEND-RANKINGS / CHOOSE-INSTRUMENT) is unchanged.
    const char* meshName = mesh->Name();
    const char* matName = (mat && mat->Name()) ? mat->Name() : "";
    // W1.7 (B7–B13): the RB3 asset-name material classifications below are game
    // content policy, relocated behind the game hook (QueryDrawMaterialPolicy). The
    // engine fetches the policy ONCE here and applies each class at its original
    // site with the SAME uniform math, so uniforms stay bit-identical. `camName` is
    // the active scene camera name, passed IN so the hook never reaches into
    // RndCam::sCurrent (Bucket-C: the cam gates for crowd/highway stay inline below).
    GameRenderHook* matHook = GetGameRenderHook();
    DrawMaterialPolicy matPolicy;
    if (matHook) {
        const char* camName = RndCam::sCurrent ? RndCam::sCurrent->Name() : nullptr;
        matPolicy = matHook->QueryDrawMaterialPolicy(mesh, mat, skinned, owner, camName);
    }
    // B7: engine keeps the empty-name RndText discriminator (isTextMeshHeur, a
    // direct '\0' compare — not an asset-name string; see BandScoreboard.cpp:79-91
    // + Text.cpp:1766 for the named-mesh cases the hook now owns) and ORs it with
    // the hook's NAMED-mesh (num*/_source.mesh/_comma.mesh/.lbl) + font/label
    // material-name half.
    bool isLikelyUiText = isTextMeshHeur || matPolicy.isUiText;
    // Menu-highlight-bar fix — the focused-menu-item yellow highlight bar.
    // UILabel::UpdateAndDrawHighlightMesh (src/system/ui/UILabel.cpp:316) and
    // LabelShrinkWrapper::UpdateAndDrawWrapper (LabelShrinkWrapper.cpp:49) draw
    // the highlight bar behind the focused BandButton — the meshes
    // `highlight_main.mesh` / `highlight_pattern.mesh` (in the button's
    // font/label resource milo, e.g. pentatonic_display.milo / the
    // label_shrink_wrapper_*.milo), with materials `highlight_main.mat` /
    // `highlight_main_spec.mat`. `highlight_main.mat` has mPreLit=0,
    // mUseEnviron=1 and a YELLOW register colour (0.82,0.82,0.17),
    // blend=kBlendSrcAlpha. With pre_lit=0 the `unlit` predicate below is
    // (!use_environ && !pre_lit) = false, so this material takes the LIT shader
    // branch and gets multiplied by the menu pass's near-zero lighting term ->
    // the yellow bar collapses to BLACK (the misplaced black box over the
    // focused hub item; the focused-item text, correctly drawn DARK on the
    // intended bright bar, then reads as faint). On Wii the bar reads its
    // authored colour directly; here it must skip lighting attenuation.
    //
    // Match the SPECIFIC highlight-bar mesh names (not a broad `highlight_*`
    // prefix) so gameplay/HUD meshes that merely contain "highlight" are never
    // affected. These are screen-space UI overlay meshes drawn only via the
    // UILabel / LabelShrinkWrapper highlight path; route them through the
    // register-colour (prelit) path exactly like text.
    // B8: the name match + the RB3_NO_HUB_HIGHLIGHT_FIX opt-out flag are relocated
    // to the game hook (QueryDrawMaterialPolicy.isHubHighlight); the engine keeps
    // the prelit-colour application below.
    bool isUiHighlightOverlay = matPolicy.isHubHighlight;
    if (mat) {
        const Hmx::Color& c = mat->GetColor();
        mu.color[0] = c.red; mu.color[1] = c.green; mu.color[2] = c.blue; mu.color[3] = c.alpha;
        // W7 Phase 3 Tier 1 — colour floor for UI-text-class meshes.
        // The Phase 3 static trace established that for some screens the
        // shipping .milo data drives the font material's GetColor() to
        // (0.20..0.50, ..., 1.0) — fine on a 480i CRT (retail target)
        // but invisible on a 1280x720 canvas alpha-blended over near-
        // black UI panels. Lift each channel to >= 0.6, preserving alpha
        // (used downstream as the fringe-fade input). No-op for already-
        // bright labels — max() guards regressions on the news-ticker /
        // FRIEND-RANKINGS / CHOOSE-INSTRUMENT text the W5 Phase 1 fix
        // recovered.
        if (isLikelyUiText) {
            mu.color[0] = std::max(0.6f, mu.color[0]);
            mu.color[1] = std::max(0.6f, mu.color[1]);
            mu.color[2] = std::max(0.6f, mu.color[2]);
        }
        mu.alphaThreshold = mat->mAlphaCut ? (mat->mAlphaThresh / 255.0f) : 0.0f;
        // V1: enable texture sampling if the material has a valid diffuse map.
        // We attempt a lazy upload here so meshes whose textures load mid-frame
        // still pick them up on subsequent draws (the MakeMaterialBindGroup
        // call below uploads + binds the matching view).
        RndTex* dt = mat->GetDiffuseTex();
        bool hasTex = false;
        if (dt) {
            wgpu::TextureView v = GetRB3TexView(dt);
            if (!v) v = UploadRndTexIfNeeded(gpu, dt);
            if (v) hasTex = true;
        }
        mu.useTexture = hasTex ? 1.0f : 0.0f;
        // RB3_HEADMAT_DBG: one-shot per mesh — dump the full material state of
        // any mesh sampling a composited `*_skin_diffuse_output` RT (C8 head-
        // invisible triage). Temporary probe.
        if (getenv("RB3_HEADMAT_DBG")) {
            const char* mnm = mesh->Name() ? mesh->Name() : "?";
            bool skinRt = dt && dt->Name() && std::strstr(dt->Name(), "skin_diffuse_output");
            Hmx::Object* meshDir = mesh->Dir();
            if (true) {
                static std::unordered_map<std::string, int> sSkinSeen;
                char ptr[32]; snprintf(ptr, sizeof(ptr), "%p", (void*)mat);
                if (sSkinSeen[std::string(mnm) + (mat->Name() ? mat->Name() : "?") + ptr]++ == 0) {
                    const Hmx::Color& mc = mat->GetColor();
                    fprintf(stderr,
                        "[HEADMAT] mesh='%s' dir='%s' owner=%p mat='%s'@%p diffuse='%s' hasTex=%d isRT=%d "
                        "blend=%d alphaCut=%d alphaThresh=%d zmode=%d color=(%.2f,%.2f,%.2f,a=%.2f) "
                        "prelit=%d useEnviron=%d texWrap=%d\n",
                        mnm, (meshDir && meshDir->Name()) ? meshDir->Name() : "-",
                        (void*)mesh, mat->Name() ? mat->Name() : "?", (void*)mat,
                        dt ? (dt->Name() ? dt->Name() : "?") : "<null>",
                        (int)hasTex, dt ? (int)dt->IsRenderTarget() : 0,
                        (int)mat->GetBlend(), (int)mat->mAlphaCut, (int)mat->mAlphaThresh,
                        (int)mat->GetZMode(), mc.red, mc.green, mc.blue, mc.alpha,
                        (int)mat->mPreLit, (int)mat->mUseEnviron, (int)mat->GetTexWrap());
                }
            }
        }
        mu.intensify = mat->mIntensify ? 2.0f : 1.0f;
        // W5 Phase 1: force-prelit text so font glyph quads pick up the
        // material colour directly without lighting attenuation.
        // Menu-highlight-bar fix: the focused-item highlight overlay
        // (`highlight_*` meshes/mats) is also drawn at its authored register
        // colour, never lit — without this it collapses to black (see above).
        mu.prelit = (mat->mPreLit || isTextMeshHeur || isUiHighlightOverlay) ? 1.0f : 0.0f;
        // Menu-lighting fix 1: honor RndMat::mUseEnviron. On Wii (WiiMat::Select),
        // a material with use_environ=0 && pre_lit=0 outputs its register colour
        // directly (GX_SRC_REG): NO ambient, NO lights, NO vertex colour — i.e.
        // full-bright authored colour × texture. This is how the menu venue's
        // neon / signs / posters / fog are authored (mostly ue=0,pl=0). The
        // shader treats unlit like prelit but keeps the white vertexTint already
        // forced for non-prelit static meshes (so no per-vertex AO tint leaks in).
        // Text/UI keep going through the prelit path above (isTextMeshHeur), so
        // they are unaffected by this flag.
        mu.unlit = (!mat->mUseEnviron && !mat->mPreLit) ? 1.0f : 0.0f;
        // Menu-lighting fix 2: material EMISSIVE on ALL cameras (was previously
        // only enabled inside the game.cam track-light block, so the menu venue's
        // self-lit windows / marquees / neon signage rendered with emissive=0).
        // The emissive map view is already resolved + bound (binding 5,
        // MakeMaterialBindGroup) for every draw; only the multiplier was zeroed.
        // Gated on map presence (many mats have mult>0 but a null map). The
        // game.cam-only boosts (gem_smasher_glow ×2, peakstate) still apply in
        // the sTrackLight block below — this just sets the baseline everywhere.
        {
            RndTex* emTexGeneral = (RndTex*)mat->mEmissiveMap;
            mu.emissiveMultiplier = emTexGeneral ? mat->mEmissiveMultiplier : 0.0f;
        }
        // W5 Phase 1: route font atlas glyph alpha into RGB. Mirrors the
        // shader's text branch (standard_wgsl.inc, useAlphaAsRGB == 1.0):
        //   baseColor.rgb *= diffuseSample.a;
        // instead of the default
        //   baseColor.rgb *= diffuseSample.rgb;
        // which is the actual root cause of W5's black-on-dark text.
        //
        // EXCEPTION — colour-icon glyph fonts (the overshell instrument-icon
        // font instrument_icons_small*, whose material name contains "icon"):
        // these are unnamed RndText glyph submeshes (so isTextMeshHeur fires)
        // but their atlas holds real RGB artwork (the guitar/bass/drums/vocals/
        // keys rings) with alpha as a solid circular cell MASK. useAlphaAsRGB
        // would discard the RGB artwork and multiply the (white) base colour by
        // the solid-circle alpha -> a SOLID WHITE CIRCLE (the overshell
        // white-blob bug). Letter fonts (Pentatonic_*) carry no "icon" in their
        // material name, so they keep the alpha->RGB glyph path.
        // B10: the "icon" material-name test is relocated to the game hook
        // (QueryDrawMaterialPolicy.isColorIcon); the engine keeps the useAlphaAsRGB
        // math (text glyph alpha->RGB, EXCEPT colour-icon fonts).
        bool isColorIconFont = matPolicy.isColorIcon;
        mu.useAlphaAsRGB = (isTextMeshHeur && !isColorIconFont) ? 1.0f : 0.0f;
        // W9 tail-color fix — apply the material's texture-coordinate transform.
        // RB3 sustain "tail" materials (tail_green.mat, tail_red.mat, ...) all
        // share one diffuse atlas (gem_tails.tex, an 8bpp DXT5 128x128 image of
        // vertically-striped fret colours) and a WHITE base mColor. Each tail
        // mesh has a CONSTANT u (~0.008) and full-height v; the per-fret colour
        // is selected entirely by the material's TexXfm U-translation
        // (TexXfm().v.x: 0.0 green, -0.115 red, -0.335 blue, ...), which shifts
        // the sample column onto a different coloured strip. Without writing
        // texGenMode + texXfmRow0/1 the shader takes its kTexGenNone branch (raw
        // u==0.008 for every material) and all tails sample the same column ->
        // uniformly white. This mirrors the DC3 draw path
        // (MaterialSetup::BuildMaterialParams, MaterialSetup.cpp:182-190).
        TexGen texGen = mat->mTexGen;
        mu.texGenMode = (float)texGen;
        if (texGen == kTexGenXfm || texGen == kTexGenXfmOrigin ||
            texGen == kTexGenProjected) {
            const Transform& txf = mat->TexXfm();
            mu.texXfmRow0[0] = txf.m.x.x; mu.texXfmRow0[1] = txf.m.x.y;
            mu.texXfmRow0[2] = txf.v.x;   mu.texXfmRow0[3] = txf.v.z;
            mu.texXfmRow1[0] = txf.m.y.x; mu.texXfmRow1[1] = txf.m.y.y;
            mu.texXfmRow1[2] = txf.v.y;   mu.texXfmRow1[3] = 0.0f;
        }
        // W9 tail-color fix (part 2): drive the per-fret colour from the tail
        // MATERIAL NAME and let the shared gem_tails.tex atlas supply only the
        // luminance/shape. The atlas is one image of vertically-striped fret
        // colours selected per material by a TexXfm U-offset; replicating the
        // Wii GX texture-matrix + sampler-wrap convention exactly so each fret
        // lands on its own strip is brittle (some frets wrap onto the atlas's
        // blank/white strip and render white). Because every tail mesh samples
        // a single near-constant texel column, the atlas effectively contributes
        // a vertical luminance ramp; multiplying it by the fret colour derived
        // from the material name (tail_green/red/yellow/blue/orange/...) yields
        // a correctly-tinted, shaped tail without depending on the fragile
        // strip-UV mapping. tail_bonus/star and tail_white stay white by name.
        // B11: the tail_* fret-name match + fret colour TABLE are relocated to the
        // game hook (QueryDrawMaterialPolicy.tailForceColor / .tailColor). The
        // engine applies the name-derived colour directly and drops the atlas tint
        // (useTexture=0): the atlas only contributes the fragile strip selection we
        // can't reproduce, so a solid fret colour is both correct and clean; the
        // tail's vertex alpha + SrcAlphaAdd blend still give it the lit look.
        // tail_white/bonus(star)/chord/miss have no fret match → keep the material's
        // own (white/grey) colour — correct for star power etc.
        if (matPolicy.tailForceColor) {
            mu.color[0] = matPolicy.tailColor[0];
            mu.color[1] = matPolicy.tailColor[1];
            mu.color[2] = matPolicy.tailColor[2];
            mu.useTexture = 0.0f;
        }
        // CHAR_DBG: one-shot per skinned mesh — report whether the character
        // outfit material resolved a diffuse texture (and what kind), to tell
        // "untextured because no tex bound" from "untextured because the
        // render-to-texture outfit composite never painted the target".
        if (skinned && getenv("CHAR_DBG")) {
            const char* mn = mesh->Name() ? mesh->Name() : "(null)";
            static std::unordered_map<std::string,int> sCharDbgSeen;
            if (sCharDbgSeen[mn]++ == 0) {
                int texType = dt ? (int)dt->GetType() : -1;
                fprintf(stderr,
                    "CHAR_DBG: skinned mesh '%s' mat='%s' diffuse=%s type=0x%x hasTexView=%d "
                    "color=(%.2f,%.2f,%.2f) bones=%d\n",
                    mn,
                    mat->Name() ? mat->Name() : "(null)",
                    dt ? (dt->Name() ? dt->Name() : "(unnamed)") : "(null)",
                    texType, hasTex ? 1 : 0,
                    c.red, c.green, c.blue, owner->NumBones());
            }
        }
        // GAP B(a): big_club (+ every venue) audience renders as stark FLAT-WHITE.
        // Two crowd render families, both dimmed here (RB3-only TU → DC3-safe).
        //
        // (1) DOMINANT — the 2D bowl-IMPOSTOR crowd BILLBOARDS. The visible white
        //     audience lining the highway edges is NOT the skinned crowd characters
        //     — measured (EMPTYNAME_PROBE): it is ~9000 4-vertex quads per frame,
        //     drawn under world.cam, with an EMPTY mesh name (so the text heuristic
        //     mis-tags them), an EMPTY-named shared material, color=(1,1,1), a baked
        //     impostor diffuse, blend=1, non-skinned. Forcing every skinned crowd
        //     char (and even every NAMED world.cam mesh) to magenta left these white
        //     → they are a distinct, un-named, un-materialed billboard family. They
        //     are the only empty-name world.cam draws that are NOT a Pentatonic/UI
        //     font quad (real text is under ui.cam/overshell.cam with blend=3 and a
        //     named font material), so the discriminator is exact:
        //       world.cam  &&  empty mesh name  &&  empty material name  &&  !skinned.
        //
        // (2) the skinned crowd/extras CHARACTERS (char/crowd|extras/*, non-band).
        //     Lower-magnitude but same retail-dim intent; band performers
        //     (skeleton_unshared.milo) carry real baked AO and are hard-excluded.
        //
        // Both are downstream of lighting (white% is exposure-invariant), so the only
        // lever is the BASE color. Measured A/B (big_club_01, foreground crowd strips):
        // OFF crowdR luma 60 white% 19; default 0.10 → luma ~33 white% ~0 (band
        // unchanged ~27). The impostor diffuse is near-white, so the multiplier must be
        // small (~0.10) to land the dim-but-present retail audience; 0.0 removes them.
        // Opt out RB3_CROWD_DIM_OFF=1; tune RB3_CROWD_DIM (default 0.10).
        static int sCrowdDimOff = -1;
        if (sCrowdDimOff < 0) {
            const char* e = getenv("RB3_CROWD_DIM_OFF");
            sCrowdDimOff = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (!sCrowdDimOff) {
            RndCam* pc = RndCam::sCurrent;
            bool worldCam = pc && pc->Name() &&
                            std::strcmp(pc->Name(), "world.cam") == 0;
            // (1) impostor crowd billboards: empty mesh name + empty material name
            //     + non-skinned, drawn under world.cam.
            bool impostorBillboard = worldCam && !skinned &&
                                     isTextMeshHeur && matName[0] == '\0';
            // (2) skinned crowd/extras characters (NOT band). Mirror the SHARD_GUARD
            //     skeleton walk (:5154-5162): band binds skeleton_unshared.milo;
            //     crowd/extras bind char/crowd/* | char/extras/* (or have a
            //     crowd/extra mesh name).
            bool skinnedCrowd = false;
            if (skinned && !isTextMeshHeur && !isLikelyUiText) {
                bool bandMember = false;
                bool isCrowdOrExtras = (meshName &&
                    (strstr(meshName, "crowd") || strstr(meshName, "extra"))) != 0;
                int nbones = owner ? owner->NumBones() : 0;
                for (int bi = 0; bi < nbones && !bandMember; bi++) {
                    RndTransformable* bbt = owner->BoneTransAt(bi);
                    ObjectDir* bbd = bbt ? bbt->Dir() : 0;
                    if (bbd && !bbd->mStoredFile.empty()) {
                        const char* sf = bbd->mStoredFile.c_str();
                        if (strstr(sf, "skeleton_unshared.milo")) bandMember = true;
                        else if (strstr(sf, "char/crowd/") || strstr(sf, "char/extras/"))
                            isCrowdOrExtras = true;
                    }
                }
                skinnedCrowd = isCrowdOrExtras && !bandMember;
            }
            if (impostorBillboard || skinnedCrowd) {
                static float sCrowdDim = -1.0f;
                if (sCrowdDim < 0.0f) {
                    const char* e = getenv("RB3_CROWD_DIM");
                    sCrowdDim = e ? (float)atof(e) : 0.10f;
                }
                mu.color[0] *= sCrowdDim;
                mu.color[1] *= sCrowdDim;
                mu.color[2] *= sCrowdDim;
            }
        }
    } else {
        mu.color[0] = mu.color[1] = mu.color[2] = mu.color[3] = 1.0f;
        mu.useTexture = 0.0f; mu.intensify = 1.0f; mu.prelit = 0.0f;
    }
    if (getenv("RB3_LIGHT_PROBE")) {
        const char* mn = mesh->Name() ? mesh->Name() : "<noname>";
        static std::unordered_map<std::string,int> sLightProbeSeen;
        if (sLightProbeSeen[mn]++ == 0) {
            RndCam* pc = RndCam::sCurrent;
            RndEnviron* pe = RndEnviron::sCurrent;
            RndTex* em = mat ? (RndTex*)mat->mEmissiveMap : nullptr;
            RndTex* dt = mat ? mat->GetDiffuseTex() : nullptr;
            Hmx::Color mc = mat ? mat->GetColor() : Hmx::Color(1,1,1,1);
            fprintf(stderr,
                "[LIGHT_PROBE] mesh='%s' cam='%s' env='%s' prelit=%d blend=%d color=(%.2f,%.2f,%.2f,%.2f) mat='%s' diff=%s emisMul=%.2f emisMap=%s\n",
                mn, (pc && pc->Name()) ? pc->Name() : "<none>",
                (pe && pe->Name()) ? pe->Name() : "<none>",
                mat ? (mat->mPreLit ? 1 : 0) : -1,
                mat ? (int)mat->GetBlend() : -1,
                mc.red, mc.green, mc.blue, mc.alpha,
                (mat && mat->Name()) ? mat->Name() : "<nomat>",
                dt ? (dt->Name() ? dt->Name() : "<unnamed>") : "null",
                mat ? mat->mEmissiveMultiplier : 0.f,
                em ? (em->Name() ? em->Name() : "<unnamed>") : "null");
        }
    }
    // Track-A glow: the gameplay-track look, applied ONLY under game.cam
    // (near30/far224) so the venue/band/crowd (drawn under world.cam) are untouched.
    // Parts:
    //  (1) Darken the prelit highway SURFACE so the prelit gems/lanes pop against a
    //      near-black track (retail's highway is dark, not the native mid-gray).
    //      surface.mat is prelit white; scale its base down (its bass-clef watermark
    //      survives via the emissive term below).
    //  (2) Re-enable material EMISSIVE (this backend's DrawMesh dropped it). Set the
    //      multiplier here (guarded by emissive-map presence — many mats have mult>0
    //      but a null map); the map texture itself is bound in MakeMaterialBindGroup.
    // Default-ON (the dark-track look is the correct native gameplay appearance);
    // RB3_TRACK_LIGHT_OFF=1 is the opt-out escape hatch for A/B (mirrors RB3_BLOOM_OFF).
    static int sTrackLight = -1;
    if (sTrackLight < 0) { const char* e = getenv("RB3_TRACK_LIGHT_OFF"); sTrackLight = (e && e[0] && e[0] != '0') ? 0 : 1; }
    if (sTrackLight && mat) {
        RndCam* pc = RndCam::sCurrent;
        if (pc && pc->Name() && std::strcmp(pc->Name(), "game.cam") == 0) {
            const char* mname = mat->Name() ? mat->Name() : "";
            if (std::strcmp(mname, "surface.mat") == 0) {
                mu.color[0] *= 0.12f; mu.color[1] *= 0.12f; mu.color[2] *= 0.12f;
                // GAP A: the highway watermark (the authored clef-scroll filigree in
                // surface.mat's emissive map, watermark_{bass,guitar,drum,keys}.tex,
                // emisMul 0.40) is the SAME pattern retail draws — but native renders
                // it ~3.4x too bright AND teal-saturated vs retail's faint near-neutral
                // ghost (retail stroke-bg delta ~24, native ~82; teal g+b-2r retail ~-16,
                // native ~+92). The shipped ×0.12 darkens only the base, never the
                // emissive add (standard_wgsl.inc:868: finalColor += tint × emisMul ×
                // tealSample), so the teal filigree dominates the dark base. Dim the
                // emissive multiplier (NOT remove — retail has the pattern) toward
                // retail's faint ghost: K ≈ 24/82 ≈ 0.30. RB3-only TU (DC3 uses
                // Rnd_Wgpu.cpp). Opt out RB3_HIGHWAY_WATERMARK_OFF=1; tune
                // RB3_HIGHWAY_WATERMARK_DIM. Note the emissiveMultiplier baseline was
                // set in the general material block (menu-lighting fix 2).
                static int sWmOff = -1;
                if (sWmOff < 0) {
                    const char* e = getenv("RB3_HIGHWAY_WATERMARK_OFF");
                    sWmOff = (e && e[0] && e[0] != '0') ? 1 : 0;
                }
                static float sWmDim = -1.0f;
                if (sWmDim < 0.0f) {
                    const char* e = getenv("RB3_HIGHWAY_WATERMARK_DIM");
                    sWmDim = e ? (float)atof(e) : 0.30f;
                }
                mu.emissiveMultiplier = sWmOff ? 0.0f : mu.emissiveMultiplier * sWmDim;
            }
            // Lit lanes: the track rails (rails.tex) are non-prelit, so they're
            // dimmed by the flat ambient and read as near-black separators on the
            // now-dark surface — the opposite of retail's bright glowing lane
            // dividers. Force them prelit so rails.tex shows at full authored
            // brightness (self-lit lanes), matching the dark-highway/bright-lane look.
            else if (std::strcmp(mname, "rails.mat") == 0) {
                mu.prelit = 1.0f;
                // rails.tex is authored bright-white; retail's lane dividers are a
                // cooler blue-white (measured normalized ~0.58/0.70/1.00 — blue
                // dominant, red suppressed). Apply a per-channel cool tint whose mean
                // is ~0.7, so overall lane brightness matches the prior flat ×0.7 but
                // the hue shifts toward retail and the gems stay the focal point.
                mu.color[0] *= 0.53f; mu.color[1] *= 0.64f; mu.color[2] *= 0.92f;
            }
            // mu.emissiveMultiplier is now set unconditionally in the general
            // material setup above (menu-lighting fix 2); the game.cam-only
            // boosts below multiply that baseline.
            // Brighter "now bar": the additive strike-line glow (gem_smasher_glow,
            // square_smasher_bright_*.tex, ships emissive mul 0.90) is dimmer/narrower
            // than retail's luminous now bar — boost its emissive contribution.
            // The glow mat has color=(0,0,0) (its per-slot colour lives in the
            // emissive MAP, bound by particle_slot_colors.anim); the standard
            // shader's emissive term now falls back to a white tint for near-black
            // bases (see standard_wgsl.inc) so a held fret actually lights up.
            //
            // wave-4: the original ×2.0 boost OVER-brightened the colored emissive
            // sample so its bright core clamped to white, washing the per-slot hue
            // (green/red/yellow/blue/orange) to near-white. (This combined with the
            // halo bloom — now excluded in IsHaloSourceMat — to make the giant white
            // sphere.) Reduced to ×1.25 so the now-bar stays clearly brighter than
            // authored while keeping the per-slot colour from saturating to white.
            // Opt-out RB3_FRET_GLOW_OFF=1 zeroes the multiplier → invisible (old
            // behaviour) for clean A/B of the held-fret glow.
            if (std::strcmp(mname, "gem_smasher_glow.mat") == 0) {
                static int sFretGlowOff = -1;
                if (sFretGlowOff < 0) {
                    const char* e = getenv("RB3_FRET_GLOW_OFF");
                    sFretGlowOff = (e && e[0] && e[0] != '0') ? 1 : 0;
                }
                mu.emissiveMultiplier = sFretGlowOff ? 0.0f : mu.emissiveMultiplier * 1.25f;
            }
            // SP "peak state" blue track overlay (peakstate_plane, spotlight_*_track.tex
            // filigree) fades in via PropAnim once the streak hits 4x, but draws faint
            // (gray base color × blue diffuse, alpha-blended). Brighten its color so the
            // blue reads as a glow over the dark track (retail's SP track is vividly
            // blue); the anim's alpha still drives the fade-in.
            if (std::strstr(mname, "peakstate") != nullptr) {
                mu.color[0] *= 2.0f; mu.color[1] *= 2.0f; mu.color[2] *= 2.0f;
            }
        }
    }
    // GEM_FORCE: debug — render prism gems as opaque bright magenta with depth
    // disabled, to disambiguate "geometry missing/off-screen/clipped" from
    // "material/alpha/occlusion makes it invisible". (Used in V13 to prove the
    // gems were frustum-clipped by a stale stationary-camera viewProj rather
    // than a material problem; left gated for future gem-render triage.)
    bool gemForce = false;
    if (getenv("GEM_FORCE")) {
        const char* mn = mesh->Name() ? mesh->Name() : "?";
        if (std::strstr(mn, "prism_gem")) {
            gemForce = true;
            mu.color[0] = 1.0f; mu.color[1] = 0.0f; mu.color[2] = 1.0f; mu.color[3] = 1.0f;
            mu.useTexture = 0.0f; mu.intensify = 1.0f; mu.prelit = 1.0f;
            mu.alphaThreshold = 0.0f;
        }
    }
    r.isTextMeshHeur = isTextMeshHeur;
    r.gemForce = gemForce;
    return r;
}
