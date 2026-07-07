// ============================================================================
// test_skin_golden.cpp — CPU reference skinner + per-vertex golden gate
//
// Work item W0.1 (Phase-0 safety net). This is the oracle the twice-reverted
// BandPatchMesh skinning rewrites never had: a standalone CPU reference skinner
// that reproduces the decomp `RndMesh::SkinVertex` semantics EXACTLY, run
// against a real posed character, whose final 4-bone-blended WORLD positions —
// including hand/finger-region verts — are pinned to a committed golden and can
// be made to FAIL RED on a deliberately-corrupted skin basis.
//
// ---------------------------------------------------------------------------
// Faithful reference (decomp ground truth)
// ---------------------------------------------------------------------------
// rb3/src/system/rndobj/Mesh.cpp:1367-1410 `RndMesh::SkinVertex`
//   ("// matches in retail").  This engine test suite compiles + links the
//   *DC3* matched fork (dc3_runtime_sources.cmake links
//   dc3-decomp/src/system/rndobj/Mesh.cpp), so the compiled oracle we
//   cross-check against is DC3's SkinVertex — semantically identical to RB3's:
//
//     Vector3 ret(0,0,0);
//     if (NumBones() > 0) {
//       Transform tf60; tf60.Zero();
//       for (i=0..3) {
//         if (vert.boneIndices[i] < NumBones()) {          // (a) SKIP, not clamp
//           int idx = vert.boneIndices[i];
//           RndTransformable* bt = BoneTransAt(idx);
//           float w = (&vert.boneWeights.x)[i];            // per-component weight
//           if (w != 0 && bt) {
//             Transform tf90;
//             Multiply(BoneOffsetAt(idx), bt->WorldXfm(), tf90);  // (c) invBind·boneWorld
//             ScaleAddEq(tf60, tf90, w);                    // (b) NO normalization
//           }
//         }
//       }
//       Multiply(vert.pos, tf60, ret);                      // row-vector pos·tf60
//     }
//     return ret;
//
// Faithful invariants preserved by RefSkinVertex():
//   (a) `boneIndices[i] < NumBones()` skip (out-of-range bones ignored, NOT clamped)
//   (b) NO weight normalization — tf60 is the raw weighted sum of skin matrices
//   (c) skinMat = BoneOffsetAt(i) · boneWorld_i  (invBind on the LEFT, Milo
//       row-vector convention)
//
// DEVIATION FROM W0.1/PLAN.md (recorded in STATUS.md): PLAN.md cites RB3's
// `Vector4_16_01 boneWeights` "hate format" u16 weights read via `.FloatAt(i)`.
// The engine-test compile context is the DC3 decomp headers, where
// `RndMesh::Vert::boneWeights` is a plain float `Vector4` read as
// `(&boneWeights.x)[i]`.  RefSkinVertex mirrors the DC3 (== compiled) form so
// the internal-oracle cross-check against `mesh->SkinVertex` holds to 1e-4.
// The three faithful invariants (a)/(b)/(c) are IDENTICAL across both forks.
//
// ---------------------------------------------------------------------------
// Golden provenance (see kGolden[] below)
// ---------------------------------------------------------------------------
//   asset dir : char/main/gen/main.milo_xbox   (DC3 orig-assets/extracted)
//   pose      : ApplyDeterministicPose() — a fixed rotation applied to the
//               arm/hand bones' LocalXfm (see that function). This is idempotent
//               (composed once from the loaded rest pose) and fully self-
//               contained: it needs no animation clip.
//   engine HEAD: a8089c3d9db9b467c31e10a118e584415b2a50ac
//   captured  : 2026-07-05  (MILO_SKIN_GOLDEN_CAPTURE=1, run twice, byte-identical)
//   epsilon   : kEpsAbs = 5e-2f.  Posed world positions are order 10-100 units;
//               5e-2 absorbs cross-machine FP reassociation noise yet still
//               catches a multi-unit "fling" (a basis error displaces a vert at
//               bone-local radius R by ~sqrt(2)*R, i.e. tens of units).
//
// DEVIATION #2 (recorded in STATUS.md): PLAN.md poses the character with
// `CharClip::PoseMeshes(sDir, StartBeat)`. In the dc3 checkout this test builds
// against, PoseMeshes(main|crowd, female_base) trips a hardened
// `std::vector<CharBones::Bone>` bounds assert inside `CharBones::ScaleAdd`
// (the crowd/main skeletons carry fewer bones than that clip channels retarget
// onto — an OOB the shipped build reads through but _GLIBCXX_ASSERTIONS aborts
// on). A test that aborts the whole suite is unacceptable, and fixing dc3's
// CharBones retarget is out of W0.1 scope. We therefore pose the arm/hand bones
// DIRECTLY (SetLocalRot -> lazy WorldXfm_Force recompute), which is more
// deterministic (no clip/beat/asset-anim dependency) and still moves the hand
// verts through a large arc so the fail-red flings are physically meaningful.
//
// Env hooks:
//   MILO_SKIN_GOLDEN_CAPTURE=1  print the selected (mesh,vert,x,y,z) rows and
//                               skip the golden asserts (regeneration mode).
//   MILO_SKIN_GOLDEN_BREAK=1    the golden test itself skins with the corrupted
//                               invBind basis -> the EXPECT_NEARs go RED. This is
//                               the human/CI-runnable fail-red demonstration for
//                               the Phase-0 exit gate (self-contained; does NOT
//                               use the non-existent RB3_SKEL_REBIND_FULL flag).
//
// Skips cleanly (GTEST_SKIP) when the char/clip assets are absent, matching the
// other 16 gtests; the coordinator's gate environment has the assets present.
// ============================================================================

#include "test_helpers.h"
#include "obj/Dir.h"
#include "obj/DirLoader.h"
#include "rndobj/Trans.h"
#include "rndobj/Mesh.h"
#include "gfx/VertexFormats.h"
#include "utl/ChunkStream.h"
#include "utl/FilePath.h"
#include "math/Vec.h"
#include "math/Mtx.h"
#include "math/Rot.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ============================================================================
// Link-anchor shim (dc3 native-build drift workaround — NOT part of the skin
// oracle). The DC3 runtime this suite links (Memcard_Stub.cpp) instantiates a
// global `MemcardXbox TheMC;` whose vtable's key function is `MemcardXbox::Init`.
// In the current dc3 checkout `Init()` is declared out-of-line and defined only
// in the non-native Memcard_Xbox.cpp, so a fresh engine-test binary has an
// UNDEFINED `_ZTV11MemcardXbox` and fails to LOAD (before any test runs). We
// emit the vtable here by anchoring the key function; --allow-multiple-definition
// (already set for this target) makes this a harmless no-op in any environment
// whose dc3 already provides the symbol. Remove if dc3 restores an Init stub.
// ============================================================================
#include "os/Memcard_Xbox.h"
void MemcardXbox::Init() { Memcard::Init(); }

// Second drift anchor: dc3's in-progress DC3_AUDIO_TRACE refactor left one caller
// referencing `Hmx::SoundAudioTraceOn()` while the definition (Sound.cpp:226) is
// at global scope, so `_ZN3Hmx17SoundAudioTraceOnEv` is undefined and the binary
// fails to load. Provide the namespaced symbol (trace off). Harmless everywhere
// (--allow-multiple-definition); delete when dc3's namespace is consistent.
namespace Hmx { bool SoundAudioTraceOn() { return false; } }

// ============================================================================
// Local asset loader (copied from test_bone_ground_truth.cpp — static/internal
// linkage, so no ODR clash with that TU's identically-named helper).
// ============================================================================

static ObjectDir *SkinTryLoadMilo(const char *path) {
    FilePath fp(path);
    ChunkStream *probe = new ChunkStream(
        fp.c_str(), ChunkStream::kRead, 0x8000, false, kPlatformNone, false
    );
    if (probe->Fail()) {
        delete probe;
        return nullptr;
    }
    delete probe;

    printf("  SkinTryLoadMilo: %s\n", path);
    ObjectDir *dir = DirLoader::LoadObjects(fp, nullptr, nullptr);
    if (dir) {
        printf("  SkinTryLoadMilo: OK '%s' class='%s'\n",
               dir->Name(), dir->ClassName().Str());
    }
    return dir;
}

// ============================================================================
// Reference skinner — the ORACLE. Mirrors Mesh.cpp:1367-1410 exactly (DC3 form).
// Must NOT call mesh->SkinVertex (that is the compiled second oracle we
// cross-check against). `breakBasis` corrupts the invBind rotation basis with a
// fixed 90-degrees-about-Z rotation to drive the self-contained fail-red path.
// ============================================================================

static Vector3 RefSkinVertex(RndMesh *m, const RndMesh::Vert &v, bool breakBasis) {
    Vector3 ret(0, 0, 0);
    if (m->NumBones() > 0) {
        Transform tf60;
        tf60.Zero();
        for (int i = 0; i < 4; i++) {
            int boneIdx = v.boneIndices[i];
            if (boneIdx < m->NumBones()) {                 // (a) skip out-of-range
                RndTransformable *bt = m->BoneTransAt(boneIdx);
                float w = (&v.boneWeights.x)[i];
                if (w != 0.0f && bt) {
                    const Transform &invBind = m->BoneOffsetAt(boneIdx);  // (c) invBind
                    Transform tf90;
                    if (breakBasis) {
                        // Fixed 90-degrees-about-Z rotation of the invBind 3x3
                        // (row-vector convention), translation preserved. A 90
                        // degree angular error guarantees a fling ~= sqrt(2)*R
                        // for a vert at bone-local radius R, regardless of asset.
                        Hmx::Matrix3 rz(0.0f, 1.0f, 0.0f,
                                        -1.0f, 0.0f, 0.0f,
                                        0.0f, 0.0f, 1.0f);
                        Transform broken;
                        Multiply(rz, invBind.m, broken.m);
                        broken.v = invBind.v;
                        Multiply(broken, bt->WorldXfm(), tf90);
                    } else {
                        Multiply(invBind, bt->WorldXfm(), tf90);
                    }
                    ScaleAddEq(tf60, tf90, w);             // (b) no normalization
                }
            }
        }
        Multiply(v.pos, tf60, ret);                        // row-vector pos·tf60
    } else {
        Multiply(v.pos, m->WorldXfm(), ret);
    }
    return ret;
}

// ============================================================================
// Real asset meshes store Xbox-compressed verts (mCompressedVerts), so
// RndMesh::Verts() (the uncompressed CPU array) is empty after load. Decode the
// compressed stream into RndMesh::Vert structs — pos + per-component float
// boneWeights + bone indices — the same inputs SkinVertex consumes. Only
// self-geometry-owning meshes are decoded so a vert's boneIndices align with
// THIS mesh's own bone list. Returns the decoded verts (empty if none).
// ============================================================================
static std::vector<RndMesh::Vert> UnpackMeshVerts(RndMesh *m) {
    std::vector<RndMesh::Vert> out;
    if (!m || m->GetGeomOwner() != m) return out; // shared-geometry LOD: skip

    int nUncompressed = m->Verts().size();
    if (nUncompressed > 0) {
        out.reserve(nUncompressed);
        for (int i = 0; i < nUncompressed; i++) out.push_back(m->Verts()[i]);
        return out;
    }

    int nc = (int)m->NumCompressedVerts();
    if (nc <= 0 || !m->CompressedVerts()) return out;
    std::vector<GpuVertexSkinned> gpu(nc);
    int got = VertexFormats::UnpackCompressedSkinnedVertices(
        m->CompressedVerts(), nc, gpu.data(), nc);
    out.reserve(got);
    for (int i = 0; i < got; i++) {
        RndMesh::Vert v;
        v.pos.Set(gpu[i].pos[0], gpu[i].pos[1], gpu[i].pos[2]);
        v.norm.Set(gpu[i].norm[0], gpu[i].norm[1], gpu[i].norm[2]);
        v.boneWeights.Set(gpu[i].boneWeights[0], gpu[i].boneWeights[1],
                          gpu[i].boneWeights[2], gpu[i].boneWeights[3]);
        for (int b = 0; b < 4; b++) v.boneIndices[b] = (short)gpu[i].boneIndices[b];
        out.push_back(v);
    }
    return out;
}

// ============================================================================
// Committed golden. Each row is the RefSkinVertex(false) result for one posed
// vertex, captured from the real asset (see provenance in the file header).
// The selection ((mesh,vert) pairs) is PART OF the golden and is never
// re-derived at assert time.  isHand marks a hand/finger-region vertex (its
// dominant bone name contains hand/finger/digit/thumb) — the fail-red +
// positive-control tests require at least one.
// ============================================================================

struct GoldenVert {
    const char *mesh;  // RndMesh::Name() of the posed skinned mesh
    int vert;          // vertex index within that mesh
    bool isHand;       // dominant bone is a hand/finger-region bone
    float x, y, z;     // RefSkinVertex(false) world position
};

// Captured MILO_SKIN_GOLDEN_CAPTURE=1 on 2026-07-05 (run twice, byte-identical).
// asset=char/main/gen/main.milo_xbox, pose=ApplyDeterministicPose. Verts 81-86
// of left_arm.mesh are hand-region (dominant bone is a hand bone); 335/336/340/363
// are contrast verts from the same mesh (shoulder/back region).
static const GoldenVert kGolden[] = {
    { "left_arm.mesh", 81, true, -8.126060f, -19.200035f, 35.165977f },
    { "left_arm.mesh", 82, true, -8.117785f, -18.919395f, 35.203842f },
    { "left_arm.mesh", 83, true, -7.674300f, -18.970154f, 35.397396f },
    { "left_arm.mesh", 84, true, -7.622164f, -19.241859f, 35.310776f },
    { "left_arm.mesh", 85, true, -8.109385f, -18.830833f, 35.249828f },
    { "left_arm.mesh", 86, true, -7.665000f, -18.853039f, 35.401161f },
    { "left_arm.mesh", 335, false, -3.749948f, -6.547750f, 47.034073f },
    { "left_arm.mesh", 336, false, -3.451988f, -5.905475f, 44.288643f },
    { "left_arm.mesh", 340, false, -3.443486f, -6.255157f, 47.667652f },
    { "left_arm.mesh", 363, false, -3.741484f, -7.586143f, 49.948334f },
};
static const int kGoldenCount = (int)(sizeof(kGolden) / sizeof(kGolden[0]));
static bool GoldenIsPlaceholder() {
    return kGoldenCount == 1 && strcmp(kGolden[0].mesh, "PLACEHOLDER") == 0;
}

static const float kEpsAbs = 5e-2f;    // golden tolerance (see header rationale)
static const float kEpsOracle = 1e-4f; // RefSkinVertex vs compiled SkinVertex
static const float kBreakMinFling = 5.0f; // positive-control min displacement

// Candidate character assets (skinned body meshes + full skeleton). main.milo
// is the committed default; crowd chars are probed as fallbacks if main lacks
// hand-region skinned verts.
static const char *kCharCandidates[] = {
    "char/main/gen/main.milo_xbox",
    "char/crowd/gen/crowd_f_01.milo_xbox",
    "char/crowd/gen/crowd_m_01.milo_xbox",
    nullptr,
};

// ============================================================================
// Fixture: load a char dir once and pose the arm/hand bones by hand (see
// DEVIATION #2 in the header — CharClip::PoseMeshes aborts on the retarget
// mismatch in this dc3 build). The loader helper mirrors
// test_bone_ground_truth.cpp; the shared file itself is NOT edited (W0.4 owns it).
// ============================================================================

class SkinGolden : public EngineTestFixture {
protected:
    static ObjectDir *sDir;

    // Fixed deterministic pose: swing the arm/hand chain so hand-region verts
    // move through a large arc away from their bind position. Composed ONCE from
    // the loaded rest pose (idempotent); no animation clip involved.
    static void ApplyDeterministicPose(ObjectDir *dir) {
        const float kArmAngle = 0.6f;  // ~34 degrees, radians
        for (ObjDirItr<RndTransformable> it(dir, true); it; ++it) {
            RndTransformable *t = it;
            std::string n(t->Name() ? t->Name() : "");
            for (char &c : n) c = (char)tolower((unsigned char)c);
            bool arm = n.find("arm") != std::string::npos ||
                       n.find("clav") != std::string::npos ||
                       n.find("shoulder") != std::string::npos ||
                       n.find("hand") != std::string::npos;
            if (!arm) continue;
            Hmx::Matrix3 rotated;
            RotateAboutX(t->LocalXfm().m, kArmAngle, rotated);
            t->SetLocalRot(rotated);  // marks subtree dirty -> WorldXfm recomputes
        }
    }

    static void SetUpTestSuite() {
        EngineTestFixture::SetUpTestSuite();

        const char *envChar = std::getenv("MILO_TEST_CHAR");
        if (envChar) {
            sDir = SkinTryLoadMilo(envChar);
        } else {
            for (int i = 0; kCharCandidates[i] && !sDir; i++) {
                sDir = SkinTryLoadMilo(kCharCandidates[i]);
            }
        }
        if (!sDir) {
            printf("SkinGolden: no character asset found\n");
            return;
        }
        ApplyDeterministicPose(sDir);
        // Force the whole transform tree to resolve now so the posed WorldXfms
        // are stable for every test (SkinVertex reads BoneTransAt->WorldXfm()).
        for (ObjDirItr<RndTransformable> it(sDir, true); it; ++it)
            (void)it->WorldXfm();
    }

    void SetUp() override {
        if (!sDir) GTEST_SKIP() << "Character asset not loaded (set MILO_TEST_CHAR)";
    }

    // Name of the max-weight in-range bone for a vertex (the "dominant" bone).
    static const char *DominantBoneName(RndMesh *m, const RndMesh::Vert &v) {
        float best = 0.0f;
        RndTransformable *bestBone = nullptr;
        for (int i = 0; i < 4; i++) {
            int idx = v.boneIndices[i];
            if (idx < m->NumBones()) {
                float w = (&v.boneWeights.x)[i];
                RndTransformable *bt = m->BoneTransAt(idx);
                if (w > best && bt) { best = w; bestBone = bt; }
            }
        }
        return bestBone ? bestBone->Name() : "";
    }

    static bool NameIsHand(const char *n) {
        if (!n) return false;
        // case-insensitive substring search over hand/finger tokens
        std::string s(n);
        for (char &c : s) c = (char)tolower((unsigned char)c);
        return s.find("hand") != std::string::npos ||
               s.find("finger") != std::string::npos ||
               s.find("digit") != std::string::npos ||
               s.find("thumb") != std::string::npos;
    }

    static bool NameIsTorso(const char *n) {
        if (!n) return false;
        std::string s(n);
        for (char &c : s) c = (char)tolower((unsigned char)c);
        return s.find("pelvis") != std::string::npos ||
               s.find("spine") != std::string::npos ||
               s.find("chest") != std::string::npos ||
               s.find("torso") != std::string::npos ||
               s.find("back") != std::string::npos ||
               s.find("rib") != std::string::npos;
    }

    // Vertex is genuinely skinned (>=1 nonzero weight to a valid, non-null bone)
    // AND has enough bone-local radius that a basis error visibly displaces it.
    static bool VertUsable(RndMesh *m, const RndMesh::Vert &v) {
        bool skinned = false;
        for (int i = 0; i < 4; i++) {
            int idx = v.boneIndices[i];
            if (idx < m->NumBones() && (&v.boneWeights.x)[i] != 0.0f &&
                m->BoneTransAt(idx)) {
                skinned = true;
                break;
            }
        }
        if (!skinned) return false;
        return Length(v.pos) > 3.0f; // skip verts sitting at the bone origin
    }

    // Resolve a golden row's mesh: first recurse-order self-owned skinned mesh
    // with a matching Name(). Deterministic (same order as capture).
    RndMesh *FindGoldenMesh(const char *name) {
        for (ObjDirItr<RndMesh> it(sDir, true); it; ++it) {
            RndMesh *m = it;
            if (!m->IsSkinned()) continue;
            if (m->GetGeomOwner() != m) continue;
            if (strcmp(m->Name(), name) != 0) continue;
            return m;
        }
        return nullptr;
    }
};

ObjectDir *SkinGolden::sDir = nullptr;

// ============================================================================
// CaptureGolden — regeneration mode. Selects a deterministic vertex set that
// MUST include hand/finger-region verts, prints paste-ready kGolden rows, and
// does NOT assert. Enabled only with MILO_SKIN_GOLDEN_CAPTURE set.
// ============================================================================

TEST_F(SkinGolden, CaptureGolden) {
    if (!std::getenv("MILO_SKIN_GOLDEN_CAPTURE"))
        GTEST_SKIP() << "capture mode off (set MILO_SKIN_GOLDEN_CAPTURE=1)";

    const int kHandWanted = 6;
    const int kTorsoWanted = 4;

    struct Sel { RndMesh *mesh; int vert; bool isHand; };
    std::vector<Sel> hands, torsos;

    // Diagnostic: list every self-owned skinned mesh + how many hand/torso verts.
    printf("=== skinned meshes in char dir ===\n");
    for (ObjDirItr<RndMesh> it(sDir, true); it; ++it) {
        RndMesh *m = it;
        if (!m->IsSkinned() || m->GetGeomOwner() != m) continue;
        std::vector<RndMesh::Vert> verts = UnpackMeshVerts(m);
        int nHand = 0, nTorso = 0;
        for (int vi = 0; vi < (int)verts.size(); vi++) {
            const RndMesh::Vert &v = verts[vi];
            if (!VertUsable(m, v)) continue;
            const char *bn = DominantBoneName(m, v);
            if (NameIsHand(bn)) nHand++;
            else if (NameIsTorso(bn)) nTorso++;
        }
        printf("  mesh '%s' verts=%d bones=%d handVerts=%d torsoVerts=%d\n",
               m->Name(), (int)verts.size(), m->NumBones(), nHand, nTorso);

        for (int vi = 0; vi < (int)verts.size(); vi++) {
            const RndMesh::Vert &v = verts[vi];
            if (!VertUsable(m, v)) continue;
            const char *bn = DominantBoneName(m, v);
            if ((int)hands.size() < kHandWanted && NameIsHand(bn))
                hands.push_back({ m, vi, true });
            else if ((int)torsos.size() < kTorsoWanted && NameIsTorso(bn))
                torsos.push_back({ m, vi, false });
        }
    }

    printf("=== selected %zu hand + %zu torso verts ===\n",
           hands.size(), torsos.size());
    if (hands.empty()) {
        printf("WARN: no hand/finger-region verts found in this asset — probe "
               "another char asset before committing (hand coverage is required)\n");
    }

    printf("// paste into kGolden[] (asset=%s pose=ApplyDeterministicPose)\n",
           sDir->Name());
    printf("static const GoldenVert kGolden[] = {\n");
    std::vector<Sel> all;
    all.insert(all.end(), hands.begin(), hands.end());
    all.insert(all.end(), torsos.begin(), torsos.end());
    for (auto &s : all) {
        std::vector<RndMesh::Vert> verts = UnpackMeshVerts(s.mesh);
        const RndMesh::Vert &v = verts[s.vert];
        Vector3 p = RefSkinVertex(s.mesh, v, false);
        printf("    { \"%s\", %d, %s, %.6ff, %.6ff, %.6ff },\n",
               s.mesh->Name(), s.vert, s.isHand ? "true" : "false",
               p.x, p.y, p.z);
    }
    printf("};\n");

    EXPECT_FALSE(hands.empty())
        << "capture must find hand/finger-region verts (see WARN above)";
}

// ============================================================================
// GoldenMatchesReference — the primary gate. Recompute RefSkinVertex for each
// committed row and pin to the golden within kEpsAbs. With MILO_SKIN_GOLDEN_BREAK
// set, skin with the corrupted basis so this test goes RED (fail-red demo).
// ============================================================================

TEST_F(SkinGolden, GoldenMatchesReference) {
    if (GoldenIsPlaceholder())
        GTEST_SKIP() << "golden not yet captured (placeholder table)";

    const bool breakIt = std::getenv("MILO_SKIN_GOLDEN_BREAK") != nullptr;
    if (breakIt)
        printf("  MILO_SKIN_GOLDEN_BREAK set — skinning with CORRUPTED basis; "
               "the golden asserts below are EXPECTED to fail red.\n");

    int handChecked = 0;
    for (int i = 0; i < kGoldenCount; i++) {
        const GoldenVert &g = kGolden[i];
        RndMesh *m = FindGoldenMesh(g.mesh);
        ASSERT_NE(m, nullptr) << "golden mesh not resolved: " << g.mesh;
        std::vector<RndMesh::Vert> verts = UnpackMeshVerts(m);
        ASSERT_LT(g.vert, (int)verts.size()) << g.mesh << " vert idx OOB";
        const RndMesh::Vert &v = verts[g.vert];
        Vector3 p = RefSkinVertex(m, v, breakIt);

        EXPECT_NEAR(p.x, g.x, kEpsAbs) << g.mesh << " vert " << g.vert << " .x";
        EXPECT_NEAR(p.y, g.y, kEpsAbs) << g.mesh << " vert " << g.vert << " .y";
        EXPECT_NEAR(p.z, g.z, kEpsAbs) << g.mesh << " vert " << g.vert << " .z";
        if (g.isHand) handChecked++;
    }
    EXPECT_GT(handChecked, 0)
        << "golden must include at least one hand/finger-region vertex";
    printf("  checked %d golden verts (%d hand/finger-region)\n",
           kGoldenCount, handChecked);
}

// ============================================================================
// ReferenceMatchesCompiledSkinVertex — internal-oracle cross-check. Proves the
// standalone RefSkinVertex is faithful to the compiled decomp RndMesh::SkinVertex
// (guards silent drift of either side).
// ============================================================================

TEST_F(SkinGolden, ReferenceMatchesCompiledSkinVertex) {
    if (GoldenIsPlaceholder())
        GTEST_SKIP() << "golden not yet captured (placeholder table)";

    for (int i = 0; i < kGoldenCount; i++) {
        const GoldenVert &g = kGolden[i];
        RndMesh *m = FindGoldenMesh(g.mesh);
        ASSERT_NE(m, nullptr) << "golden mesh not resolved: " << g.mesh;
        std::vector<RndMesh::Vert> verts = UnpackMeshVerts(m);
        ASSERT_LT(g.vert, (int)verts.size()) << g.mesh << " vert idx OOB";
        const RndMesh::Vert &v = verts[g.vert];

        Vector3 ref = RefSkinVertex(m, v, false);
        Vector3 comp = m->SkinVertex(v, nullptr);
        EXPECT_NEAR(ref.x, comp.x, kEpsOracle) << g.mesh << " vert " << g.vert;
        EXPECT_NEAR(ref.y, comp.y, kEpsOracle) << g.mesh << " vert " << g.vert;
        EXPECT_NEAR(ref.z, comp.z, kEpsOracle) << g.mesh << " vert " << g.vert;
    }
}

// ============================================================================
// BrokenSkinDivergesFromGolden — permanent positive control. Always runs the
// broken skinner (no env needed) and requires at least one hand/finger-region
// vertex to displace by > kBreakMinFling units from its golden. Proves the
// oracle stays sensitive; stays GREEN in normal CI.
// ============================================================================

TEST_F(SkinGolden, BrokenSkinDivergesFromGolden) {
    if (GoldenIsPlaceholder())
        GTEST_SKIP() << "golden not yet captured (placeholder table)";

    float maxHandFling = 0.0f;
    int handSeen = 0;
    for (int i = 0; i < kGoldenCount; i++) {
        const GoldenVert &g = kGolden[i];
        if (!g.isHand) continue;
        handSeen++;
        RndMesh *m = FindGoldenMesh(g.mesh);
        ASSERT_NE(m, nullptr) << "golden mesh not resolved: " << g.mesh;
        std::vector<RndMesh::Vert> verts = UnpackMeshVerts(m);
        ASSERT_LT(g.vert, (int)verts.size()) << g.mesh << " vert idx OOB";
        const RndMesh::Vert &v = verts[g.vert];
        Vector3 p = RefSkinVertex(m, v, true); // corrupted basis
        float d = std::max(std::max(std::fabs(p.x - g.x), std::fabs(p.y - g.y)),
                           std::fabs(p.z - g.z));
        maxHandFling = std::max(maxHandFling, d);
    }
    printf("  hand verts=%d  maxHandFling=%.3f (threshold %.1f)\n",
           handSeen, maxHandFling, kBreakMinFling);
    ASSERT_GT(handSeen, 0) << "positive control needs a hand/finger golden vert";
    EXPECT_GT(maxHandFling, kBreakMinFling)
        << "corrupted invBind basis should fling a hand vert by many units — "
           "the oracle has lost sensitivity";
}

// ============================================================================
// ShellInvariantAxisOracle (W2.8g B-S1) — composition oracle for Instrument B's
// rest-free axis discriminator. Validates the SPACE-vs-DECODE truth table that
// the in-engine [INSTR_B] instrument (Rnd_Wgpu_RB3.cpp) uses to classify the
// hands smear, on REAL hand-region bind verts (pulled from the loaded char; a
// finger-scale synthetic fallback keeps it self-contained). Three skin modes on
// one dominant bone:
//   COHERENT : skin = liveW                         (off == inv(rest), rest = I)
//   SPACE    : skin = R87 * liveW                   (offset basis 87deg off own rest)
//   DECODE   : each vert skinned by a DIFFERENT rigid xf (vert bound to wrong bone)
// Predicted (and asserted): a SPACE conjugation is a single RIGID rotation ->
// isoDistort ~0 (isometry preserved) yet displaces the shell by R*2sin(theta/2)
// (large shellErr); a DECODE corruption tears the sub-shell -> isoDistort >> 0.
// Therefore the in-engine hands reading (isoDistort ~0 AND large shell error) is
// the SPACE signature, NOT decode. This is the oracle that makes the axis call
// trustworthy (composition; DC3 source = corroboration only).
// ============================================================================
TEST_F(SkinGolden, ShellInvariantAxisOracle) {
    // Real hand-region bind verts (radius from bone origin drives R*sin(theta)).
    std::vector<Vector3> bind;
    if (sDir) {
        for (ObjDirItr<RndMesh> it(sDir, true); it && bind.size() < 24; ++it) {
            RndMesh *m = it;
            if (!m->IsSkinned() || m->GetGeomOwner() != m) continue;
            std::vector<RndMesh::Vert> verts = UnpackMeshVerts(m);
            for (const auto &v : verts) {
                if (!VertUsable(m, v)) continue;
                if (!NameIsHand(DominantBoneName(m, v))) continue;
                bind.push_back(v.pos);
                if (bind.size() >= 24) break;
            }
        }
    }
    bool real = bind.size() >= 4;
    if (!real) { // finger-scale synthetic fallback (radii ~10-30u, like the fingers)
        bind.clear();
        for (int i = 0; i < 12; i++) {
            float a = i * 0.5f;
            bind.push_back(Vector3(8.f + i * 1.7f, 5.f * std::sin(a), 5.f * std::cos(a)));
        }
    }
    const int N = (int)bind.size();

    Hmx::Matrix3 I3(Vector3(1,0,0), Vector3(0,1,0), Vector3(0,0,1));
    // liveW: a nontrivial live bone rotation (the pose). rest == I (off == inv(rest) == I).
    Transform live; live.v.Set(0,0,0); RotateAboutY(I3, 0.7f, live.m);
    // R87: the 87deg own-rest conjugation baked into the offset (== A.S1/Tier-1 signal).
    const float theta = 1.518f; // ~87 deg
    Transform r87; r87.v.Set(0,0,0); RotateAboutZ(I3, theta, r87.m);

    auto skinBy = [](const Vector3 &p, const Transform &t) {
        Vector3 o; Multiply(p, t, o); return o; };
    auto isoDistort = [&](const std::vector<Vector3> &s) {
        double sum = 0; int np = 0;
        for (int a = 0; a < N; a++) for (int c = a + 1; c < N; c++) {
            float db = Length(bind[a] - bind[c]); if (db < 1e-3f) continue;
            float ds = Length(s[a] - s[c]);
            sum += std::fabs(ds - db) / db; np++;
        }
        return np ? (float)(sum / np) : -1.f; };

    // COHERENT: single rigid map (liveW).
    Transform spaceT; Multiply(r87, live, spaceT); // v * r87 * liveW
    std::vector<Vector3> sCoh(N), sSpace(N), sDec(N);
    double shellSum = 0; float meanR = 0;
    for (int i = 0; i < N; i++) {
        sCoh[i]   = skinBy(bind[i], live);
        sSpace[i] = skinBy(bind[i], spaceT);
        // DECODE: vert i skinned by a DIFFERENT rigid xf (models a per-vert wrong-bone bind).
        Transform di; di.v.Set(0,0,0);
        RotateAboutZ(I3, 0.20f + 0.13f * i, di.m);
        Transform decT; Multiply(di, live, decT);
        sDec[i] = skinBy(bind[i], decT);
        shellSum += Length(sSpace[i] - sCoh[i]);
        meanR += Length(bind[i]);
    }
    meanR /= N;
    float shellErr = (float)(shellSum / N);
    float isoCoh = isoDistort(sCoh), isoSpace = isoDistort(sSpace), isoDec = isoDistort(sDec);

    printf("  ShellInvariantAxisOracle: source=%s N=%d meanR=%.1fu\n",
           real ? "REAL-hand-verts" : "synthetic-finger", N, meanR);
    printf("    isoDistort  coherent=%.5f  SPACE=%.5f  DECODE=%.5f\n", isoCoh, isoSpace, isoDec);
    printf("    shellErr(SPACE vs coherent) mean=%.1fu  R*2sin(th/2)=%.1fu\n",
           shellErr, meanR * 2.f * std::sin(theta * 0.5f));

    // Truth table:
    // (1) coherent is isometric.
    EXPECT_LT(isoCoh, 1e-3f) << "coherent single-bone skin must be isometric";
    // (2) a SPACE conjugation stays RIGID (isoDistort ~0) -> iso cannot see it...
    EXPECT_LT(isoSpace, 1e-3f) << "SPACE conjugation is a rigid rotation -> isometry preserved";
    // (3) ...but DISPLACES the shell by ~R*2sin(theta/2) (this is what wext/shellMax read).
    EXPECT_GT(shellErr, 0.5f * meanR) << "SPACE conjugation must fling the shell (R*sin theta)";
    // (4) a DECODE (per-vert wrong bone) TEARS the sub-shell -> isoDistort >> 0.
    EXPECT_GT(isoDec, 0.05f) << "DECODE corruption must break isometry (torn sub-shell)";
    // => in-engine hands (isoDistort ~0 + large shellErr) == SPACE, not DECODE.
}
