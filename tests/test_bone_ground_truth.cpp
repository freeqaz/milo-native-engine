// Bone Ground Truth & Clip Validation tests (Gates 1-3)
// Validates bone topology, rest pose sanity, and clip pose application
// using real character .milo_xbox assets.
//
// Env vars:
//   MILO_TEST_CHAR  — bone dir override (default: skeleton_bones_resource)
//   MILO_TEST_CLIPS — clip dir override (default: auto-discover)
//
// Tests skip gracefully if assets are not found.

#include "test_helpers.h"
#include "obj/Dir.h"
#include "obj/DirLoader.h"
#include "rndobj/Trans.h"
#include "char/CharClip.h"
#include "char/CharUtl.h"
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
// Helper: try to load a .milo_xbox file, returns nullptr on failure
// ============================================================================

static ObjectDir *TryLoadMilo(const char *path) {
    FilePath fp(path);
    ChunkStream *probe = new ChunkStream(
        fp.c_str(), ChunkStream::kRead, 0x8000, false, kPlatformNone, false
    );
    if (probe->Fail()) {
        delete probe;
        return nullptr;
    }
    delete probe;

    printf("  TryLoadMilo: %s\n", path);
    ObjectDir *dir = DirLoader::LoadObjects(fp, nullptr, nullptr);
    if (dir) {
        printf("  TryLoadMilo: OK '%s' class='%s'\n",
               dir->Name(), dir->ClassName().Str());
    }
    return dir;
}

static float MaxMatrixDiff(const Hmx::Matrix3& a, const Hmx::Matrix3& b) {
    float d = 0.0f;
    d = std::max(d, std::fabs(a.x.x - b.x.x));
    d = std::max(d, std::fabs(a.x.y - b.x.y));
    d = std::max(d, std::fabs(a.x.z - b.x.z));
    d = std::max(d, std::fabs(a.y.x - b.y.x));
    d = std::max(d, std::fabs(a.y.y - b.y.y));
    d = std::max(d, std::fabs(a.y.z - b.y.z));
    d = std::max(d, std::fabs(a.z.x - b.z.x));
    d = std::max(d, std::fabs(a.z.y - b.z.y));
    d = std::max(d, std::fabs(a.z.z - b.z.z));
    return d;
}

// ============================================================================
// Shared fixture: loads skeleton bones once per test suite
// ============================================================================

class BoneGroundTruth : public EngineTestFixture {
protected:
    static ObjectDir *sDir;

    static void SetUpTestSuite() {
        EngineTestFixture::SetUpTestSuite();

        const char *envPath = std::getenv("MILO_TEST_CHAR");
        if (envPath) {
            sDir = TryLoadMilo(envPath);
            return;
        }

        // Try assets in order (simpler first to avoid crashes)
        const char *candidates[] = {
            "char/shared/gen/skeleton_bones_resource.milo_xbox",
            "char/main/gen/main.milo_xbox",
            nullptr
        };

        for (int i = 0; candidates[i]; i++) {
            sDir = TryLoadMilo(candidates[i]);
            if (sDir) return;
        }
        printf("BoneGroundTruth: no character asset found\n");
    }

    void SetUp() override {
        if (!sDir) {
            GTEST_SKIP() << "Character asset not loaded (set MILO_TEST_CHAR)";
        }
    }

    RndTransformable *FindBone(const char *name) {
        return sDir->Find<RndTransformable>(name, false);
    }
};

ObjectDir *BoneGroundTruth::sDir = nullptr;

// ============================================================================
// Gate 1: Bone Topology
// ============================================================================

TEST_F(BoneGroundTruth, BoneExists) {
    const char *boneNames[] = {
        "bone_pelvis.mesh", "bone_head.mesh",
        "bone_R-hand.mesh", "bone_L-hand.mesh",
        nullptr
    };

    for (int i = 0; boneNames[i]; i++) {
        RndTransformable *bone = FindBone(boneNames[i]);
        if (bone) {
            printf("  Found: %s\n", boneNames[i]);
        } else {
            printf("  NOT FOUND: %s\n", boneNames[i]);
        }
        ASSERT_NE(bone, nullptr) << "Missing bone: " << boneNames[i];
    }
}

TEST_F(BoneGroundTruth, BoneHierarchy) {
    RndTransformable *head = FindBone("bone_head.mesh");
    ASSERT_NE(head, nullptr);

    RndTransformable *headParent = head->TransParent();
    EXPECT_NE(headParent, nullptr) << "bone_head.mesh has no parent";
    if (headParent) {
        printf("  bone_head parent: %s\n", headParent->Name());
    }

    RndTransformable *rHand = FindBone("bone_R-hand.mesh");
    ASSERT_NE(rHand, nullptr);

    RndTransformable *rHandParent = rHand->TransParent();
    EXPECT_NE(rHandParent, nullptr) << "bone_R-hand.mesh has no parent";
    if (rHandParent) {
        printf("  bone_R-hand parent: %s\n", rHandParent->Name());
    }
}

TEST_F(BoneGroundTruth, BoneChildCount) {
    RndTransformable *pelvis = FindBone("bone_pelvis.mesh");
    ASSERT_NE(pelvis, nullptr);

    size_t childCount = pelvis->Children().size();
    printf("  bone_pelvis children: %zu\n", childCount);
    EXPECT_GT(childCount, 0u) << "pelvis should have children";
}

TEST_F(BoneGroundTruth, ManualTransformRoundTrip) {
    RndTransformable *pelvis = FindBone("bone_pelvis.mesh");
    ASSERT_NE(pelvis, nullptr);

    // Save original
    Vector3 origPos = pelvis->LocalXfm().v;

    // Set test position
    Vector3 testPos(1.0f, 2.0f, 3.0f);
    pelvis->SetLocalPos(testPos);

    const Vector3 &got = pelvis->LocalXfm().v;
    EXPECT_FLOAT_EQ(got.x, 1.0f);
    EXPECT_FLOAT_EQ(got.y, 2.0f);
    EXPECT_FLOAT_EQ(got.z, 3.0f);

    // Restore original
    pelvis->SetLocalPos(origPos);
}

// ============================================================================
// Gate 2: Rest Pose Sanity
// ============================================================================

TEST_F(BoneGroundTruth, RestPoseNonZero) {
    int nonIdentity = 0;
    int total = 0;

    for (ObjDirItr<RndTransformable> it(sDir, true); it; ++it) {
        RndTransformable *bone = it;
        total++;
        const Vector3 &pos = bone->WorldXfm().v;
        if (pos.x != 0.0f || pos.y != 0.0f || pos.z != 0.0f) {
            nonIdentity++;
        }
    }

    printf("  %d/%d transforms have non-zero world position\n", nonIdentity, total);
    EXPECT_GT(nonIdentity, 5)
        << "Expected at least some bones to have non-identity world transforms";
}

TEST_F(BoneGroundTruth, SymmetryCheck) {
    RndTransformable *lHand = FindBone("bone_L-hand.mesh");
    RndTransformable *rHand = FindBone("bone_R-hand.mesh");
    ASSERT_NE(lHand, nullptr);
    ASSERT_NE(rHand, nullptr);

    const Vector3 &lPos = lHand->WorldXfm().v;
    const Vector3 &rPos = rHand->WorldXfm().v;

    printf("  L-hand world: (%.3f, %.3f, %.3f)\n", lPos.x, lPos.y, lPos.z);
    printf("  R-hand world: (%.3f, %.3f, %.3f)\n", rPos.x, rPos.y, rPos.z);

    // Tolerance generous — model uses centimeters (hand at ~15 units from center)
    EXPECT_NEAR(lPos.x, -rPos.x, 1.0f) << "X axis symmetry";
    EXPECT_NEAR(lPos.y, rPos.y, 1.0f) << "Y axis similarity";
    EXPECT_NEAR(lPos.z, rPos.z, 1.0f) << "Z axis similarity";
}

TEST_F(BoneGroundTruth, LimbDistanceSanity) {
    RndTransformable *shoulder = FindBone("bone_R-upperArm.mesh");
    RndTransformable *elbow = FindBone("bone_R-foreArm.mesh");
    ASSERT_NE(shoulder, nullptr) << "bone_R-upperArm.mesh not found";
    ASSERT_NE(elbow, nullptr) << "bone_R-foreArm.mesh not found";

    const Vector3 &sPos = shoulder->WorldXfm().v;
    const Vector3 &ePos = elbow->WorldXfm().v;

    float dx = sPos.x - ePos.x;
    float dy = sPos.y - ePos.y;
    float dz = sPos.z - ePos.z;
    float len = std::sqrt(dx * dx + dy * dy + dz * dz);

    printf("  R-upperarm to R-forearm distance: %.3f\n", len);
    EXPECT_GT(len, 0.05f) << "Upper arm too short";
    EXPECT_LT(len, 300.0f) << "Upper arm too long";
}

TEST_F(BoneGroundTruth, HeadAbovePelvis) {
    RndTransformable *head = FindBone("bone_head.mesh");
    RndTransformable *pelvis = FindBone("bone_pelvis.mesh");
    ASSERT_NE(head, nullptr);
    ASSERT_NE(pelvis, nullptr);

    const Vector3 &headPos = head->WorldXfm().v;
    const Vector3 &pelvisPos = pelvis->WorldXfm().v;

    printf("  head pos=(%.3f, %.3f, %.3f), pelvis pos=(%.3f, %.3f, %.3f)\n",
           headPos.x, headPos.y, headPos.z, pelvisPos.x, pelvisPos.y, pelvisPos.z);
    // Milo uses Z-up coordinate system (head Z=64, pelvis Z=42.5)
    float headHeight = std::max(headPos.y, headPos.z);
    float pelvisHeight = std::max(pelvisPos.y, pelvisPos.z);
    EXPECT_GT(headHeight, pelvisHeight) << "Head should be above pelvis (max of Y,Z)";
}

// ============================================================================
// Gate 3: Clip Pose Validation
// ============================================================================

class ClipPoseFixture : public BoneGroundTruth {
protected:
    static CharClip *sClip;
    static ObjectDir *sClipDir;
    static bool sDanceClip; // true if we found a real dance clip (not skeleton retarget)

    static void SetUpTestSuite() {
        BoneGroundTruth::SetUpTestSuite();
        if (!sDir) return;

        // First try finding clips in the bone dir (and subdirs)
        for (ObjDirItr<CharClip> it(sDir, true); it; ++it) {
            sClip = it;
            sClipDir = sDir;
            sDanceClip = false; // likely skeleton retarget clip
            printf("ClipPoseFixture: found clip '%s' in bone dir\n", sClip->Name());
            break;
        }

        // Try loading dance animation clips (real movement data)
        const char *envClip = std::getenv("MILO_TEST_CLIPS");
        const char *danceCandidates[] = {
            "char/crowd/anim/gen/female_base.milo_xbox",
            "char/crowd/anim/gen/male_base.milo_xbox",
            nullptr
        };

        const char **candidates = danceCandidates;
        const char *singleEnv[2] = {nullptr, nullptr};
        if (envClip) {
            singleEnv[0] = envClip;
            candidates = singleEnv;
        }

        for (int i = 0; candidates[i]; i++) {
            ObjectDir *clipDir = TryLoadMilo(candidates[i]);
            if (clipDir) {
                for (ObjDirItr<CharClip> it(clipDir, true); it; ++it) {
                    CharClip *clip = it;
                    // Skip skeleton retarget clips, prefer dance clips
                    const char *name = clip->Name();
                    if (strstr(name, "skeleton") || strstr(name, "retarget"))
                        continue;
                    sClip = clip;
                    sClipDir = clipDir;
                    sDanceClip = true;
                    printf("ClipPoseFixture: found dance clip '%s' in %s\n",
                           name, candidates[i]);
                    break;
                }
                if (sDanceClip) break;
            }
        }

        // Fallback: try skeleton_clips if we still have no clip at all
        if (!sClip) {
            ObjectDir *clipDir = TryLoadMilo(
                "char/main/retarget_skeletons/gen/skeleton_clips.milo_xbox"
            );
            if (clipDir) {
                for (ObjDirItr<CharClip> it(clipDir, true); it; ++it) {
                    sClip = it;
                    sClipDir = clipDir;
                    sDanceClip = false;
                    printf("ClipPoseFixture: fallback clip '%s'\n", sClip->Name());
                    break;
                }
            }
        }

        if (!sClip) {
            printf("ClipPoseFixture: no CharClip found\n");
        }
    }

    void SetUp() override {
        BoneGroundTruth::SetUp();
        if (!sClip) {
            GTEST_SKIP() << "No CharClip found";
        }
    }
};

CharClip *ClipPoseFixture::sClip = nullptr;
ObjectDir *ClipPoseFixture::sClipDir = nullptr;
bool ClipPoseFixture::sDanceClip = false;

TEST_F(ClipPoseFixture, ClipExists) {
    int clipCount = 0;
    for (ObjDirItr<CharClip> it(sClipDir, true); it; ++it) {
        clipCount++;
        if (clipCount <= 5) {
            printf("  clip[%d]: '%s'\n", clipCount - 1, ((CharClip *)it)->Name());
        }
    }
    printf("  Total CharClips: %d\n", clipCount);
    printf("  Dance clip: %s\n", sDanceClip ? "yes" : "no (skeleton retarget)");
    EXPECT_GT(clipCount, 0);
}

TEST_F(ClipPoseFixture, CrouchingGreatClipContainsForeArmAndHandChannels) {
    CharClip* target = nullptr;
    for (ObjDirItr<CharClip> it(sClipDir, true); it; ++it) {
        if (strcmp(it->Name(), "crouching_great_01") == 0) {
            target = it;
            break;
        }
    }
    if (!target)
        GTEST_SKIP() << "Clip crouching_great_01 not found in loaded clip dir";

    std::list<CharBones::Bone> bones;
    target->ListBones(bones);
    printf("  crouching_great_01 bone channels: %zu\n", bones.size());
    int idx = 0;
    bool hasLForeArm = false;
    bool hasRForeArm = false;
    bool hasLHand = false;
    bool hasRHand = false;
    for (const auto& b : bones) {
        if (idx < 30) {
            printf("    [%02d] '%s' weight=%.2f type=%d\n",
                   idx, b.name.Str(), b.weight, (int)CharBones::TypeOf(b.name));
        }
        const char* name = b.name.Str();
        if (strstr(name, "bone_L-foreArm") != nullptr) hasLForeArm = true;
        if (strstr(name, "bone_R-foreArm") != nullptr) hasRForeArm = true;
        if (strstr(name, "bone_L-hand") != nullptr) hasLHand = true;
        if (strstr(name, "bone_R-hand") != nullptr) hasRHand = true;
        idx++;
    }
    if (idx > 30) {
        printf("    ... and %d more\n", idx - 30);
    }

    printf("  forearm/hand presence: L-foreArm=%d R-foreArm=%d L-hand=%d R-hand=%d\n",
           hasLForeArm, hasRForeArm, hasLHand, hasRHand);

    EXPECT_TRUE(hasLForeArm);
    EXPECT_TRUE(hasRForeArm);
    EXPECT_TRUE(hasLHand);
    EXPECT_TRUE(hasRHand);
}

TEST_F(ClipPoseFixture, PoseMeshesDoesNotCrash) {
    float beat = sClip->StartBeat();
    sClip->PoseMeshes(sDir, beat);
    printf("  PoseMeshes(dir, %.3f) completed without crash\n", beat);
}

TEST_F(ClipPoseFixture, PoseChangesTransforms) {
    // --- Diagnostic: inspect clip data before testing ---
    printf("  Clip: '%s'\n", sClip->Name());
    printf("  NumFrames: %d\n", sClip->NumFrames());
    printf("  StartBeat: %.3f  EndBeat: %.3f  LengthBeats: %.3f\n",
           sClip->StartBeat(), sClip->EndBeat(), sClip->LengthBeats());
    printf("  FramesPerSec: %.1f\n", sClip->FramesPerSec());

    // Check sample data
    const CharBonesSamples &full = sClip->GetFull();
    const CharBonesSamples &one = sClip->GetOne();
    printf("  Full: NumSamples=%d, NumFrames=%d, TotalSize=%d, Compression=%d\n",
           full.NumSamples(), full.NumFrames(), full.TotalSize(),
           (int)full.GetCompression());
    printf("  One:  NumSamples=%d, NumFrames=%d, TotalSize=%d\n",
           one.NumSamples(), one.NumFrames(), one.TotalSize());

    // List clip bones
    std::list<CharBones::Bone> boneList;
    sClip->ListBones(boneList);
    printf("  Clip has %zu bone channels\n", boneList.size());
    int bonesPrinted = 0;
    for (auto &b : boneList) {
        if (bonesPrinted < 10) {
            printf("    bone: '%s' weight=%.2f\n", b.name.Str(), b.weight);
        }
        bonesPrinted++;
    }
    if (bonesPrinted > 10) printf("    ... and %d more\n", bonesPrinted - 10);

    // Check how many clip bones can be found in the bone dir
    int foundCount = 0, missingCount = 0;
    for (auto &b : boneList) {
        // Simulate CharUtlFindBoneTrans logic
        char buf[256];
        strncpy(buf, b.name.Str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *dot = strrchr(buf, '.');
        if (!dot) dot = buf + strlen(buf);

        bool found = false;
        const char *suffixes[] = {".cb", ".trans", ".mesh", nullptr};
        for (int s = 0; suffixes[s]; s++) {
            strcpy(dot, suffixes[s]);
            if (sDir->Find<RndTransformable>(buf, false)) {
                found = true;
                break;
            }
        }
        if (found) foundCount++;
        else {
            missingCount++;
            if (missingCount <= 5)
                printf("    MISSING bone: '%s' (tried .cb/.trans/.mesh)\n", b.name.Str());
        }
    }
    printf("  Bone lookup: %d found, %d missing in dir '%s'\n",
           foundCount, missingCount, sDir->Name());

    // Check BeatToSample — use actual clip beat range!
    float beatA = sClip->StartBeat();
    float beatB = beatA + sClip->LengthBeats() * 0.5f; // midpoint
    float frac;
    int sampA = sClip->BeatToSample(beatA, &frac);
    printf("  BeatToSample(%.3f): sample=%d frac=%.4f\n", beatA, sampA, frac);
    int sampB = sClip->BeatToSample(beatB, &frac);
    printf("  BeatToSample(%.3f): sample=%d frac=%.4f\n", beatB, sampB, frac);

    // --- Actual test: check LocalXfm changes (not just WorldXfm) ---
    sClip->PoseMeshes(sDir, beatA);

    struct BoneSnapshot {
        RndTransformable *bone;
        Vector3 localPos;
        Vector3 worldPos;
    };
    std::vector<BoneSnapshot> frame0;

    for (ObjDirItr<RndTransformable> it(sDir, true); it; ++it) {
        RndTransformable *bone = it;
        frame0.push_back({bone, bone->LocalXfm().v, bone->WorldXfm().v});
    }

    // Apply clip at midpoint beat
    sClip->PoseMeshes(sDir, beatB);

    int localMoved = 0, worldMoved = 0;
    for (size_t i = 0; i < frame0.size(); i++) {
        const Vector3 &newLocal = frame0[i].bone->LocalXfm().v;
        const Vector3 &oldLocal = frame0[i].localPos;
        float dxL = newLocal.x - oldLocal.x;
        float dyL = newLocal.y - oldLocal.y;
        float dzL = newLocal.z - oldLocal.z;
        float distL = std::sqrt(dxL * dxL + dyL * dyL + dzL * dzL);
        if (distL > 0.001f) {
            localMoved++;
            if (localMoved <= 3) {
                printf("    LOCAL moved: '%s' (%.3f,%.3f,%.3f)->(%.3f,%.3f,%.3f) d=%.4f\n",
                       frame0[i].bone->Name(),
                       oldLocal.x, oldLocal.y, oldLocal.z,
                       newLocal.x, newLocal.y, newLocal.z, distL);
            }
        }

        const Vector3 &newWorld = frame0[i].bone->WorldXfm().v;
        const Vector3 &oldWorld = frame0[i].worldPos;
        float dxW = newWorld.x - oldWorld.x;
        float dyW = newWorld.y - oldWorld.y;
        float dzW = newWorld.z - oldWorld.z;
        float distW = std::sqrt(dxW * dxW + dyW * dyW + dzW * dzW);
        if (distW > 0.001f) worldMoved++;
    }

    printf("  %d/%zu bones LOCAL pos moved, %d/%zu bones WORLD pos moved (beat %.1f→%.1f)\n",
           localMoved, frame0.size(), worldMoved, frame0.size(), beatA, beatB);

    if (sDanceClip) {
        // Dance clips move bones via rotation (world positions change through parent chain)
        // At least some world-space positions should differ between two beats
        EXPECT_GT(worldMoved, 0) << "Dance clip should move bones in world space";
    } else if (worldMoved == 0) {
        printf("  NOTE: No bones moved — skeleton retarget clips are static poses\n");
    }
}

TEST_F(ClipPoseFixture, PoseDeterminism) {
    // Use a beat within the clip's actual range
    float beat = sClip->StartBeat() + sClip->LengthBeats() * 0.25f;
    sClip->PoseMeshes(sDir, beat);

    struct BoneSnapshot {
        RndTransformable *bone;
        Vector3 pos;
    };
    std::vector<BoneSnapshot> pass1;

    for (ObjDirItr<RndTransformable> it(sDir, true); it; ++it) {
        RndTransformable *bone = it;
        pass1.push_back({bone, bone->WorldXfm().v});
    }

    sClip->PoseMeshes(sDir, beat);

    int mismatches = 0;
    for (size_t i = 0; i < pass1.size(); i++) {
        const Vector3 &newPos = pass1[i].bone->WorldXfm().v;
        const Vector3 &oldPos = pass1[i].pos;
        if (newPos.x != oldPos.x || newPos.y != oldPos.y || newPos.z != oldPos.z) {
            if (mismatches < 3) {
                printf("  Mismatch: %s (%.6f,%.6f,%.6f) vs (%.6f,%.6f,%.6f)\n",
                       pass1[i].bone->Name(),
                       oldPos.x, oldPos.y, oldPos.z,
                       newPos.x, newPos.y, newPos.z);
            }
            mismatches++;
        }
    }

    printf("  %d/%zu bones had non-deterministic results\n",
           mismatches, pass1.size());
    EXPECT_EQ(mismatches, 0) << "PoseMeshes should be deterministic for same beat";
}

TEST_F(ClipPoseFixture, ChannelEvaluationIsFiniteAtKeyBeats) {
    // Triage note:
    // If this suddenly reports huge 1e20-1e38 magnitudes after source edits,
    // verify `native/build/milo-tests` was rebuilt first. Stale binaries have
    // previously produced false regression signals for this check.
    std::list<CharBones::Bone> bones;
    sClip->ListBones(bones);
    ASSERT_FALSE(bones.empty());

    float beats[2] = {
        sClip->StartBeat(),
        sClip->StartBeat() + sClip->LengthBeats() * 0.5f
    };

    for (float beat : beats) {
        int checked = 0;
        for (const auto &b : bones) {
            void *channel = sClip->GetChannel(b.name);
            ASSERT_NE(channel, nullptr) << "Missing channel for " << b.name.Str();

            CharBones::Type ty = CharBones::TypeOf(b.name);
            if (ty == CharBones::TYPE_POS || ty == CharBones::TYPE_SCALE) {
                Vector3 v;
                sClip->EvaluateChannel(&v, channel, beat);
                EXPECT_TRUE(std::isfinite(v.x));
                EXPECT_TRUE(std::isfinite(v.y));
                EXPECT_TRUE(std::isfinite(v.z));
                EXPECT_LT(std::fabs(v.x), 10000.0f);
                EXPECT_LT(std::fabs(v.y), 10000.0f);
                EXPECT_LT(std::fabs(v.z), 10000.0f);
            } else if (ty == CharBones::TYPE_QUAT) {
                Hmx::Quat q;
                sClip->EvaluateChannel(&q, channel, beat);
                EXPECT_TRUE(std::isfinite(q.x));
                EXPECT_TRUE(std::isfinite(q.y));
                EXPECT_TRUE(std::isfinite(q.z));
                EXPECT_TRUE(std::isfinite(q.w));
                float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
                EXPECT_GT(n, 0.001f);
                EXPECT_LT(n, 2.0f);
            } else {
                float r = 0.0f;
                sClip->EvaluateChannel(&r, channel, beat);
                EXPECT_TRUE(std::isfinite(r));
                EXPECT_LT(std::fabs(r), 100.0f);
            }
            checked++;
        }
        EXPECT_GT(checked, 0);
    }
}

TEST_F(ClipPoseFixture, PelvisPosChannelMatchesPoseMeshesLocalPos) {
    // End-to-end: clip channel evaluation -> ScaleAdd -> PoseMeshes -> RndTransformable local pose.
    Symbol pelvisPosSym("bone_pelvis.pos");
    void *channel = sClip->GetChannel(pelvisPosSym);
    ASSERT_NE(channel, nullptr) << "Missing channel bone_pelvis.pos";

    RndTransformable *pelvis = FindBone("bone_pelvis.mesh");
    ASSERT_NE(pelvis, nullptr);

    float beats[2] = {
        sClip->StartBeat(),
        sClip->StartBeat() + sClip->LengthBeats() * 0.5f
    };

    for (float beat : beats) {
        Vector3 expected;
        sClip->EvaluateChannel(&expected, channel, beat);

        sClip->PoseMeshes(sDir, beat);
        const Vector3 &actual = pelvis->LocalXfm().v;

        EXPECT_NEAR(actual.x, expected.x, 0.01f);
        EXPECT_NEAR(actual.y, expected.y, 0.01f);
        EXPECT_NEAR(actual.z, expected.z, 0.01f);
    }
}

TEST_F(ClipPoseFixture, WeightedPosAndQuatChannelsMatchLocalPose) {
    std::list<CharBones::Bone> bones;
    sClip->ListBones(bones);
    ASSERT_FALSE(bones.empty());

    float beats[2] = {
        sClip->StartBeat(),
        sClip->StartBeat() + sClip->LengthBeats() * 0.5f
    };

    int checkedPos = 0;
    int checkedQuat = 0;
    int skippedUnresolved = 0;
    int skippedWeighted = 0;

    for (float beat : beats) {
        sClip->PoseMeshes(sDir, beat);
        for (const auto& b : bones) {
            CharBones::Type ty = CharBones::TypeOf(b.name);
            if (ty != CharBones::TYPE_POS && ty != CharBones::TYPE_QUAT) continue;

            // Only assert direct channel->local equivalence for full-weight channels.
            // Partial-weight channels blend with base pose and won't match raw channel.
            if (std::fabs(b.weight - 1.0f) > 1e-4f) {
                skippedWeighted++;
                continue;
            }

            RndTransformable* trans = CharUtlFindBoneTrans(b.name.Str(), sDir);
            if (!trans) {
                skippedUnresolved++;
                continue;
            }

            void* channel = sClip->GetChannel(b.name);
            ASSERT_NE(channel, nullptr) << "Missing channel for " << b.name.Str();

            if (ty == CharBones::TYPE_POS) {
                Vector3 expected;
                sClip->EvaluateChannel(&expected, channel, beat);
                const Vector3& actual = trans->LocalXfm().v;
                EXPECT_NEAR(actual.x, expected.x, 0.02f) << b.name.Str();
                EXPECT_NEAR(actual.y, expected.y, 0.02f) << b.name.Str();
                EXPECT_NEAR(actual.z, expected.z, 0.02f) << b.name.Str();
                checkedPos++;
            } else {
                Hmx::Quat q;
                sClip->EvaluateChannel(&q, channel, beat);
                Hmx::Matrix3 expectedM;
                MakeRotMatrix(q, expectedM);
                float md = MaxMatrixDiff(trans->LocalXfm().m, expectedM);
                EXPECT_LT(md, 0.08f) << "matrix mismatch for " << b.name.Str();
                checkedQuat++;
            }
        }
    }

    printf("  weighted channel/local checks: pos=%d quat=%d unresolved=%d partialWeight=%d\n",
           checkedPos, checkedQuat, skippedUnresolved, skippedWeighted);
    EXPECT_GT(checkedPos, 0);
    EXPECT_GT(checkedQuat, 0);
}

TEST_F(ClipPoseFixture, BoneWorldMatchesLocalComposedWithParent) {
    float beat = sClip->StartBeat() + sClip->LengthBeats() * 0.5f;
    sClip->PoseMeshes(sDir, beat);

    int checked = 0;
    for (ObjDirItr<RndTransformable> it(sDir, true); it; ++it) {
        RndTransformable* t = it;
        if (strncmp(t->Name(), "bone_", 5) != 0) continue;

        RndTransformable* parent = t->TransParent();
        if (!parent) continue;

        Transform expected;
        Multiply(t->LocalXfm(), parent->WorldXfm(), expected);
        const Transform& actual = t->WorldXfm();

        EXPECT_NEAR(actual.v.x, expected.v.x, 1e-3f) << t->Name();
        EXPECT_NEAR(actual.v.y, expected.v.y, 1e-3f) << t->Name();
        EXPECT_NEAR(actual.v.z, expected.v.z, 1e-3f) << t->Name();
        EXPECT_LT(MaxMatrixDiff(actual.m, expected.m), 1e-3f) << t->Name();
        checked++;
    }

    printf("  checked parent/local/world composition on %d bones\n", checked);
    EXPECT_GT(checked, 20);
}

TEST_F(ClipPoseFixture, KeyBoneWorldDeltasAreFiniteAcrossBeats) {
    const char* keyBoneGroups[][3] = {
        {"bone_pelvis.mesh", "bone_pelvis.trans", nullptr},
        {"bone_head.mesh", "bone_head.trans", nullptr},
        {"bone_L-hand.mesh", "bone_L-hand.trans", nullptr},
        {"bone_R-hand.mesh", "bone_R-hand.trans", nullptr},
        {"bone_L-foot.mesh", "bone_L-ankle.mesh", nullptr},
        {"bone_R-foot.mesh", "bone_R-ankle.mesh", nullptr},
    };

    float b0 = sClip->StartBeat();
    float b1 = sClip->StartBeat() + sClip->LengthBeats() * 0.5f;
    float b2 = sClip->EndBeat();
    float beats[3] = {b0, b1, b2};

    int resolvedGroups = 0;
    for (const auto& group : keyBoneGroups) {
        RndTransformable* bone = nullptr;
        const char* usedName = nullptr;
        for (int gi = 0; group[gi]; gi++) {
            bone = FindBone(group[gi]);
            if (bone) {
                usedName = group[gi];
                break;
            }
        }
        if (!bone) continue;
        resolvedGroups++;

        Vector3 prev(0, 0, 0);
        bool hasPrev = false;
        for (float beat : beats) {
            sClip->PoseMeshes(sDir, beat);
            const Vector3& w = bone->WorldXfm().v;
            EXPECT_TRUE(std::isfinite(w.x));
            EXPECT_TRUE(std::isfinite(w.y));
            EXPECT_TRUE(std::isfinite(w.z));

            if (hasPrev) {
                float dx = w.x - prev.x;
                float dy = w.y - prev.y;
                float dz = w.z - prev.z;
                float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                EXPECT_LT(d, 300.0f) << usedName << " large jump at beat " << beat;
            }
            prev = w;
            hasPrev = true;
        }
    }
    EXPECT_GT(resolvedGroups, 3) << "Too few key bones resolved in current asset";
}

// ============================================================================
// Gate 4: Foot Orientation Invariants
//
// Validates that the ankle-to-toe bone relationship is geometrically correct
// in rest pose and after clip application. These are the invariants that
// break when feet become inverted due to IK mLocalXfm dirty cascade bugs.
//
// Invariants for CORRECT feet:
//   1. Toe Z < Ankle Z (toe is below ankle, closer to ground)
//   2. Ankle-to-toe vector points generally downward (negative Z component)
//   3. Ankle Z-axis does not point strongly upward (m.z.z < 0.7)
//   4. Shin (knee-to-ankle) and foot (ankle-to-toe) vectors are not colinear
//      pointing in the same direction (foot should bend forward/down from shin)
//
// See: docs/sessions/2026-03-25-feet-in-ground-fix.md
// ============================================================================

TEST_F(BoneGroundTruth, FootBonesExist) {
    const char *footBones[] = {
        "bone_L-ankle.mesh", "bone_R-ankle.mesh",
        "bone_L-toe.mesh", "bone_R-toe.mesh",
        nullptr
    };
    for (int i = 0; footBones[i]; i++) {
        RndTransformable *bone = FindBone(footBones[i]);
        if (bone) {
            printf("  Found: %s at (%.2f, %.2f, %.2f)\n",
                   footBones[i],
                   bone->WorldXfm().v.x,
                   bone->WorldXfm().v.y,
                   bone->WorldXfm().v.z);
        } else {
            printf("  NOT FOUND: %s\n", footBones[i]);
        }
        // Soft check — some skeleton resources may lack toe bones
        EXPECT_NE(bone, nullptr) << "Missing foot bone: " << footBones[i];
    }
}

TEST_F(BoneGroundTruth, RestPoseToesBelowAnkles) {
    // In rest pose, toe bones should be below ankle bones (closer to ground).
    // Milo uses Z-up coordinate system.
    struct Side { const char *ankle; const char *toe; const char *label; };
    Side sides[] = {
        {"bone_L-ankle.mesh", "bone_L-toe.mesh", "Left"},
        {"bone_R-ankle.mesh", "bone_R-toe.mesh", "Right"},
    };

    for (auto &s : sides) {
        RndTransformable *ankle = FindBone(s.ankle);
        RndTransformable *toe = FindBone(s.toe);
        if (!ankle || !toe) {
            printf("  SKIP %s: ankle=%p toe=%p\n", s.label, (void*)ankle, (void*)toe);
            continue;
        }

        float ankleZ = ankle->WorldXfm().v.z;
        float toeZ = toe->WorldXfm().v.z;
        printf("  %s foot: ankle Z=%.3f, toe Z=%.3f (delta=%.3f)\n",
               s.label, ankleZ, toeZ, toeZ - ankleZ);

        EXPECT_LT(toeZ, ankleZ + 2.0f)
            << s.label << " toe is above ankle in rest pose — foot is inverted. "
            << "toe Z=" << toeZ << ", ankle Z=" << ankleZ;
    }
}

TEST_F(BoneGroundTruth, RestPoseAnkleZAxisNotFlipped) {
    // The ankle bone's WorldXfm rotation matrix Z column (m.z) should not
    // point strongly upward. In rest pose, it should point roughly downward
    // or laterally. A Z-axis z-component > 0.7 means the foot is rotated
    // nearly 180 degrees from its intended orientation.
    const char *ankles[] = {"bone_L-ankle.mesh", "bone_R-ankle.mesh", nullptr};
    for (int i = 0; ankles[i]; i++) {
        RndTransformable *ankle = FindBone(ankles[i]);
        if (!ankle) continue;

        float zAxisZ = ankle->WorldXfm().m.z.z;
        printf("  %s Z-axis z-component: %.3f\n", ankles[i], zAxisZ);

        EXPECT_LT(zAxisZ, 0.7f)
            << ankles[i] << " Z-axis points upward (z.z=" << zAxisZ
            << "), indicating the ankle rotation is flipped. "
            << "Expected < 0.7 (roughly downward or lateral).";
    }
}

TEST_F(BoneGroundTruth, RestPoseAnkleToToeVectorPointsDown) {
    // The vector from ankle to toe should have a negative or small Z component
    // (pointing toward the ground). If positive and large, the foot is inverted.
    struct Side { const char *ankle; const char *toe; const char *label; };
    Side sides[] = {
        {"bone_L-ankle.mesh", "bone_L-toe.mesh", "Left"},
        {"bone_R-ankle.mesh", "bone_R-toe.mesh", "Right"},
    };

    for (auto &s : sides) {
        RndTransformable *ankle = FindBone(s.ankle);
        RndTransformable *toe = FindBone(s.toe);
        if (!ankle || !toe) continue;

        float dx = toe->WorldXfm().v.x - ankle->WorldXfm().v.x;
        float dy = toe->WorldXfm().v.y - ankle->WorldXfm().v.y;
        float dz = toe->WorldXfm().v.z - ankle->WorldXfm().v.z;
        float len = std::sqrt(dx * dx + dy * dy + dz * dz);

        printf("  %s ankle-to-toe: (%.2f, %.2f, %.2f) len=%.2f\n",
               s.label, dx, dy, dz, len);

        if (len > 0.01f) {
            // Normalized Z component of ankle-to-toe direction
            float normZ = dz / len;
            printf("  %s ankle-to-toe normalized Z: %.3f\n", s.label, normZ);

            // Should not be strongly positive (pointing up through shin)
            EXPECT_LT(normZ, 0.5f)
                << s.label << " ankle-to-toe vector points upward (normZ="
                << normZ << "). The foot is inverted — toe is above ankle.";
        }
    }
}

TEST_F(ClipPoseFixture, FootOrientationCorrectAfterClip) {
    // After applying a dance clip, verify the foot invariants still hold.
    // This catches bugs where clip pose data corrupts the ankle rotation.
    float beats[] = {
        sClip->StartBeat(),
        sClip->StartBeat() + sClip->LengthBeats() * 0.25f,
        sClip->StartBeat() + sClip->LengthBeats() * 0.5f,
        sClip->StartBeat() + sClip->LengthBeats() * 0.75f,
    };

    struct Side { const char *ankle; const char *toe; const char *label; };
    Side sides[] = {
        {"bone_L-ankle.mesh", "bone_L-toe.mesh", "Left"},
        {"bone_R-ankle.mesh", "bone_R-toe.mesh", "Right"},
    };

    int checked = 0;
    int invertedCount = 0;

    for (float beat : beats) {
        sClip->PoseMeshes(sDir, beat);

        for (auto &s : sides) {
            RndTransformable *ankle = FindBone(s.ankle);
            RndTransformable *toe = FindBone(s.toe);
            if (!ankle || !toe) continue;
            checked++;

            float ankleZ = ankle->WorldXfm().v.z;
            float toeZ = toe->WorldXfm().v.z;

            if (toeZ > ankleZ + 2.0f) {
                invertedCount++;
                printf("  INVERTED at beat %.2f: %s toe Z=%.2f > ankle Z=%.2f\n",
                       beat, s.label, toeZ, ankleZ);
            }
        }
    }

    if (checked == 0) {
        printf("  SKIP: no ankle/toe bones found in asset\n");
    } else {
        printf("  Checked %d ankle/toe pairs across %d beats, %d inverted\n",
               checked, (int)(sizeof(beats) / sizeof(beats[0])), invertedCount);

        EXPECT_EQ(invertedCount, 0)
            << "Foot was inverted after applying clip '" << sClip->Name()
            << "'. The toe bone is above the ankle bone in world Z.";
    }
}

// ============================================================================
// W0.4: Live-pose effector WORLD-position golden (placement-regression net)
//
// Every other live-pose test in this file is a RELATIVE / INVARIANT check
// (worldMoved > 0, isfinite, deterministic, world == local·parentWorld). None
// of them pins a specific posed effector WORLD position to a committed number,
// so a placement regression that still produces finite, deterministic, self-
// consistent output — exactly the count-in-shard / hub-bar / crowd-colocation
// bug class — passes green. This test closes that gap: it applies a PINNED clip
// at PINNED beats to the real crowd skeleton and asserts the WORLD-space (x,y,z)
// of the hand + foot effectors against committed golden constants within a
// tight epsilon. It grades the Phase-2 skinned-placement rewrite against numbers
// instead of eyeballs.
//
// Faithful reference: CharClip::PoseMeshes (StuffBones -> ScaleDown ->
// ScaleAdd(1,beat,0) -> CharBonesMeshes::PoseMeshes), world composition via
// Trans.cpp Multiply(local, parentWorld, world).
//
// The clip is pinned BY NAME and beats BY FRACTION of the clip range, so the
// golden never silently re-baselines on ObjDirItr order or absolute-beat drift.
//
// Env hooks:
//   MILO_TEST_DUMP_POSE_GOLDEN — print copy-paste-ready kEffectorGoldens[]
//                                initializers, then SKIP (no assert). This is
//                                how W0.4.S2 captures the goldens.
//   MILO_TEST_POSE_PERTURB=<f> — add <f> world units to each measured X before
//                                comparing (synthetic placement error), so S2/CI
//                                can prove the gate fails RED on a real shift.
// ============================================================================

namespace {

struct EffectorGolden {
    const char *bone;   // effector bone name (resolved in sDir, the posed dir)
    float beatFrac;     // pose beat as a fraction of the clip's [Start,Length]
    float x, y, z;      // committed WORLD-space position (see kEffectorEps)
};

// W0.4.S2: run MILO_TEST_DUMP_POSE_GOLDEN and paste the emitted initializer
// lines here (verbatim, keeping the printed float precision) to activate the
// gate. While this array is empty the test SKIPs (green-or-skip, never a
// spurious red) — S1 ships the harness, S2 fills the numbers.
static const EffectorGolden kEffectorGoldens[] = {
    // { "bone_R-hand.mesh", 0.00f, 0.000000f, 0.000000f, 0.000000f },
};

static CharClip *FindClipByName(ObjectDir *dir, const char *name) {
    if (!dir)
        return nullptr;
    for (ObjDirItr<CharClip> it(dir, true); it; ++it) {
        if (std::strcmp(it->Name(), name) == 0)
            return it;
    }
    return nullptr;
}

} // namespace

TEST_F(ClipPoseFixture, EffectorWorldPositionsMatchGolden) {
    // Epsilon: the test runs in exactly one build config, so PoseMeshes is
    // bit-deterministic (see PoseDeterminism). 0.05 world units absorbs any
    // Phase-1 "byte-identical" FP reordering while leaving enormous margin over
    // a real placement bug (count-in shard shifted effectors 50-65u). The model
    // is centimeter-scale (hands ~15u from center).
    static const float kEffectorEps = 0.05f;
    static const char *kPinnedClip = "crouching_great_01"; // known L/R foreArm+hand carrier

    CharClip *clip = FindClipByName(sClipDir, kPinnedClip);
    if (!clip)
        clip = FindClipByName(sDir, kPinnedClip);
    if (!clip)
        GTEST_SKIP() << "pinned clip '" << kPinnedClip << "' not found in loaded dirs";

    // Effector set: hands + toes, with ankle/foot fallback if a toe is absent.
    // Resolved against sDir ONLY — PoseMeshes(sDir, beat) poses sDir's bones, so
    // only sDir bones have a valid posed WorldXfm to pin.
    struct Effector { const char *primary; const char *fallback; };
    const Effector wanted[] = {
        {"bone_R-hand.mesh", nullptr},
        {"bone_L-hand.mesh", nullptr},
        {"bone_R-toe.mesh", "bone_R-ankle.mesh"},
        {"bone_L-toe.mesh", "bone_L-ankle.mesh"},
    };

    struct Resolved { const char *name; RndTransformable *bone; };
    std::vector<Resolved> effectors;
    for (const auto &w : wanted) {
        RndTransformable *b = FindBone(w.primary);
        const char *used = w.primary;
        if (!b && w.fallback) {
            b = FindBone(w.fallback);
            used = w.fallback;
        }
        if (b) {
            effectors.push_back({used, b});
            printf("  effector: '%s'\n", used);
        }
    }

    // Prop / drum-stick bone probe (brief: "include if reachable"). Search the
    // posed dir (sDir) only. skeleton_bones_resource is a bare crowd humanoid
    // with no instrument props, so none is expected — probe so the omission is
    // data-driven, not assumed. Do NOT load extra prop assets to chase one.
    RndTransformable *propBone = nullptr;
    const char *propName = nullptr;
    for (ObjDirItr<RndTransformable> it(sDir, true); it; ++it) {
        const char *n = it->Name();
        if (n && (std::strstr(n, "stick") || std::strstr(n, "prop"))) {
            propBone = it;
            propName = n;
            break;
        }
    }
    if (propBone) {
        effectors.push_back({propName, propBone});
        printf("  prop/stick effector included: '%s'\n", propName);
    } else {
        printf("  no prop/stick bone reachable in sDir (expected for "
               "skeleton_bones_resource) — prop effector omitted\n");
    }

    if (effectors.empty())
        GTEST_SKIP() << "no effector bones resolved in current asset";

    // Three pinned beats as fractions of the clip range. Avoid EndBeat() exactly
    // (some clips wrap). Expressed as fractions so the golden survives an asset
    // whose absolute beats shift.
    static const float kFracs[] = {0.0f, 0.5f, 0.9f};
    const float start = clip->StartBeat();
    const float len = clip->LengthBeats();

    // --- Golden-dump mode: emit initializers, then skip (no assertions) ---
    if (std::getenv("MILO_TEST_DUMP_POSE_GOLDEN")) {
        printf("\n// ==== W0.4 effector golden dump — clip '%s' ====\n", kPinnedClip);
        printf("// W0.4.S2: paste the following into kEffectorGoldens[]:\n");
        for (float frac : kFracs) {
            clip->PoseMeshes(sDir, start + len * frac);
            for (const auto &e : effectors) {
                const Vector3 &w = e.bone->WorldXfm().v;
                printf("    { \"%s\", %.2ff, %.6ff, %.6ff, %.6ff },\n",
                       e.name, frac, w.x, w.y, w.z);
            }
        }
        printf("// ==== end dump ====\n\n");
        GTEST_SKIP() << "golden dump complete (MILO_TEST_DUMP_POSE_GOLDEN set)";
    }

    // --- Placeholder guard: green-or-skip until S2 fills the table ---
    const size_t goldenCount = sizeof(kEffectorGoldens) / sizeof(kEffectorGoldens[0]);
    if (goldenCount == 0) {
        GTEST_SKIP() << "kEffectorGoldens[] is empty — run with "
                        "MILO_TEST_DUMP_POSE_GOLDEN=1 and paste the output "
                        "(W0.4.S2 fills the goldens and turns this test green)";
    }

    // --- Fail-red hook: synthetic X displacement proves the gate can go red ---
    const char *perturbEnv = std::getenv("MILO_TEST_POSE_PERTURB");
    const float perturb = perturbEnv ? (float)std::atof(perturbEnv) : 0.0f;
    if (perturb != 0.0f)
        printf("  MILO_TEST_POSE_PERTURB=%.4f — injecting synthetic X error\n", perturb);

    int checked = 0;
    for (const auto &g : kEffectorGoldens) {
        RndTransformable *bone = FindBone(g.bone);
        if (!bone) {
            ADD_FAILURE() << "golden references bone missing from asset: " << g.bone;
            continue;
        }
        clip->PoseMeshes(sDir, start + len * g.beatFrac);
        const Vector3 &w = bone->WorldXfm().v;
        EXPECT_NEAR(w.x + perturb, g.x, kEffectorEps)
            << g.bone << " @frac " << g.beatFrac << " world X";
        EXPECT_NEAR(w.y, g.y, kEffectorEps)
            << g.bone << " @frac " << g.beatFrac << " world Y";
        EXPECT_NEAR(w.z, g.z, kEffectorEps)
            << g.bone << " @frac " << g.beatFrac << " world Z";
        checked++;
    }
    printf("  checked %d effector golden(s) against eps=%.3f\n", checked, kEffectorEps);
    EXPECT_GT(checked, 0);
}

// ============================================================================
// main.milo_xbox loading (was DISABLED_ — crash fixed in RndMesh::OnSync)
// ============================================================================

class MainMiloLoadTest : public EngineTestFixture {};

TEST_F(MainMiloLoadTest, LoadMainCharacterMilo) {
    // main.milo_xbox has ~21 subdirs (wind, skeleton, flows, etc.)
    // Crash was fixed by adding bestFaceIt tracking in RndMesh::OnSync face patching.
    const char *path = "char/main/gen/main.milo_xbox";

    FilePath fp(path);
    ChunkStream *probe = new ChunkStream(
        fp.c_str(), ChunkStream::kRead, 0x8000, false, kPlatformNone, false
    );
    if (probe->Fail()) {
        delete probe;
        GTEST_SKIP() << "main.milo_xbox not found";
    }
    delete probe;

    printf("MainMiloLoadTest: attempting to load %s\n", path);
    ObjectDir *dir = DirLoader::LoadObjects(fp, nullptr, nullptr);

    // When this stops crashing, enable the test (remove DISABLED_ prefix)
    // and this assertion will verify it actually loaded.
    ASSERT_NE(dir, nullptr)
        << "main.milo_xbox should load without crashing. "
        << "If this fails after removing DISABLED_, the crash is fixed but "
        << "LoadObjects returned null.";

    printf("  Loaded: '%s' class='%s'\n", dir->Name(), dir->ClassName().Str());

    // Verify it has bones (main character should have full skeleton)
    RndTransformable *pelvis = dir->Find<RndTransformable>("bone_pelvis.mesh", false);
    EXPECT_NE(pelvis, nullptr) << "main.milo should contain bone_pelvis.mesh";
}
