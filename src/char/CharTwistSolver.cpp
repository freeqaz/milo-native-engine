// CharTwistSolver — replicates CharUpperTwist::Poll(), CharForeTwist::Poll(),
// and CharNeckTwist::Poll() for outfit-only .milo files that don't include
// the shared twist pollables from main character setup dirs.

#include "char/CharTwistSolver.h"

#include "obj/Dir.h"
#include "obj/ObjPtr_p.h"
#include "rndobj/Trans.h"
#include "char/CharPollable.h"
#include "math/Rot.h"
#include "math/Trig.h"
#include "math/Vec.h"
#include "math/Mtx.h"

#include <cmath>
#include <cstdio>
#include <cstring>

// NormalizeAboutX is declared in math/Rot.h and defined in CharUpperTwist.cpp

// Exact CharUpperTwist::Poll() math.
static void SolveUpperTwistPoll(
    RndTransformable* twist2, RndTransformable* twist1, RndTransformable* upperArm
) {
    if (!twist2 || !twist1 || !upperArm) return;
    RndTransformable* parent = twist2->TransParent();
    if (!parent) return;
    const Transform& parentWorld = parent->WorldXfm();
    const Transform& twist2World = twist2->WorldXfm();
    Hmx::Quat q;
    MakeRotQuat(parentWorld.m.x, twist2World.m.x, q);
    Vector3 rotatedY;
    Multiply(parentWorld.m.y, q, rotatedY);

    Transform tf;
    tf.m.x = twist2World.m.x;
    tf.v = upperArm->WorldXfm().v;
    Interp(rotatedY, twist2World.m.y, 0.333f, tf.m.y);
    NormalizeAboutX(tf.m);
    upperArm->SetWorldXfm(tf);

    tf.v = twist1->WorldXfm().v;
    Interp(rotatedY, twist2World.m.y, 0.666f, tf.m.y);
    NormalizeAboutX(tf.m);
    twist1->SetWorldXfm(tf);
}

// LimitAng is declared in math/Trig.h and defined in CharForeTwist.cpp

// Replicates CharForeTwist::Poll() — distributes hand twist along forearm
static void SolveForeTwist(RndTransformable* hand, RndTransformable* twist2,
                           float offset, float bias) {
    if (!hand || !twist2 || !hand->TransParent() || !twist2->TransParent())
        return;
    const Transform& parentxfm = hand->TransParent()->WorldXfm();
    const Transform& handxfm = hand->WorldXfm();
    float clamped = Clamp(-1.0f, 1.0f, Dot(parentxfm.m.y, handxfm.m.z));
    Vector3 v98;
    Cross(parentxfm.m.y, handxfm.m.z, v98);
    float clamp2 = Clamp(-1.0f, 1.0f, Dot(parentxfm.m.x, v98));
    float newbias = bias * DEG2RAD;
    float tan2res = std::atan2(clamp2, clamped);
    float angle = LimitAng(offset * DEG2RAD + tan2res + newbias);
    float finalfloat = angle - newbias;
    if (finalfloat != finalfloat)
        return; // NaN guard — matches CharForeTwist::Poll()
    Hmx::Matrix3 m58;
    MakeRotMatrixX(finalfloat * 0.33333f, m58);
    RndTransformable* twistparent = twist2->TransParent();
    Transform tf;
    tf.v = parentxfm.v;
    Multiply(m58, parentxfm.m, tf.m);
    twistparent->SetWorldXfm(tf);
    Interp(tf.v, handxfm.v, twist2->LocalXfm().v.x / hand->LocalXfm().v.x, tf.v);
    Multiply(m58, tf.m, tf.m);
    twist2->SetWorldXfm(tf);
}

// Exact CharNeckTwist::Poll() math — applies half of head yaw to neck twist bone
static void SolveNeckTwist(RndTransformable* twist, RndTransformable* head) {
    if (!twist || !head) return;
    float headAngle = GetZAngle(head->LocalXfm().m);
    float half = headAngle * 0.5f;
    Hmx::Matrix3 rotMat;
    rotMat.x.Set(Cosine(half), Sine(half), 0);
    rotMat.y.Set(-Sine(half), Cosine(half), 0);
    rotMat.z.Set(0, 0, 1);
    Multiply(twist->LocalXfm().m, rotMat, twist->DirtyLocalXfm().m);
}

bool CharTwistSolver::IsDriverPollable(const CharPollable* p) {
    if (!p) return false;
    const char* cn = p->ClassName().Str();
    return strcmp(cn, "CharDriver") == 0
        || strcmp(cn, "CharDriverMidi") == 0
        || strcmp(cn, "CharServoBone") == 0;
}

bool CharTwistSolver::IsTwistPollable(const CharPollable* p) {
    if (!p) return false;
    const char* cn = p->ClassName().Str();
    return strcmp(cn, "CharForeTwist") == 0
        || strcmp(cn, "CharUpperTwist") == 0
        || strcmp(cn, "CharNeckTwist") == 0
        || strcmp(cn, "CharBoneTwist") == 0;
}

void CharTwistSolver::SolveAll(ObjectDir* dir) {
    if (!dir) return;

    // One-time dump of arm bone parent chain for debugging
    static int sHierarchyDump = 0;
    if (sHierarchyDump < 1) {
        sHierarchyDump++;
        const char* armBones[] = {
            "bone_L-clavicle.mesh", "bone_L-upperArm.mesh",
            "bone_L-upperTwist1.mesh", "bone_L-upperTwist2.mesh",
            "bone_L-foreArm.mesh",
            "bone_L-foreTwist1.mesh", "bone_L-foreTwist2.mesh",
            "bone_L-hand.mesh",
        };
        fprintf(stderr, "=== ARM BONE HIERARCHY DUMP ===\n");
        for (const char* name : armBones) {
            RndTransformable* b = dir->Find<RndTransformable>(name, false);
            if (b) {
                RndTransformable* p = b->TransParent();
                const Vector3& lp = b->LocalXfm().v;
                const Hmx::Matrix3& lr = b->LocalXfm().m;
                fprintf(stderr, "  '%s' parent='%s' constraint=%d\n"
                        "    localPos=(%.3f, %.3f, %.3f)\n"
                        "    localRot=[%.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f]\n",
                        name, p ? p->Name() : "(null)", (int)b->TransConstraint(),
                        lp.x, lp.y, lp.z,
                        lr.x.x, lr.x.y, lr.x.z,
                        lr.y.x, lr.y.y, lr.y.z,
                        lr.z.x, lr.z.y, lr.z.z);
            } else {
                fprintf(stderr, "  '%s' NOT FOUND\n", name);
            }
        }
        fprintf(stderr, "=== END ARM BONE HIERARCHY ===\n");
    }

    // Prefer authoritative in-scene pollables when present, but only on a
    // per-twist-type basis. Standalone character assets often contain the two
    // authored CharForeTwist objects while relying on fallback math for upper
    // arm and neck twist handling.
    //
    // IMPORTANT: CharUpperTwist must run BEFORE CharForeTwist because
    // CharUpperTwist modifies upperArm->SetWorldXfm() which dirties all
    // descendants (including foreTwist bones). If CharForeTwist runs first,
    // its SetWorldXfm results get dirtied away and revert to clip transforms.
    bool sawForeTwist = false;
    bool sawUpperTwist = false;
    bool sawNeckTwist = false;

    // First pass: poll CharUpperTwist and CharBoneTwist (upstream of fore twist)
    for (ObjDirItr<CharPollable> it(dir, true); it != nullptr; ++it) {
        const char* cn = it->ClassName().Str();
        if (strcmp(cn, "CharUpperTwist") == 0) {
            it->Poll();
            sawUpperTwist = true;
        } else if (strcmp(cn, "CharBoneTwist") == 0) {
            it->Poll();
        }
    }
    // Second pass: poll CharForeTwist and CharNeckTwist (downstream)
    for (ObjDirItr<CharPollable> it(dir, true); it != nullptr; ++it) {
        const char* cn = it->ClassName().Str();
        if (strcmp(cn, "CharForeTwist") == 0) {
            it->Poll();
            sawForeTwist = true;
        } else if (strcmp(cn, "CharNeckTwist") == 0) {
            it->Poll();
            sawNeckTwist = true;
        }
    }

    if (!sawUpperTwist) {
        const char* sides[] = {"L", "R"};
        for (auto side : sides) {
            char upperArmName[64], upperTwist1Name[64], upperTwist2Name[64];
            snprintf(upperArmName, sizeof(upperArmName), "bone_%s-upperArm.mesh", side);
            snprintf(upperTwist1Name, sizeof(upperTwist1Name), "bone_%s-upperTwist1.mesh", side);
            snprintf(upperTwist2Name, sizeof(upperTwist2Name), "bone_%s-upperTwist2.mesh", side);

            RndTransformable* upperArm = dir->Find<RndTransformable>(upperArmName, false);
            RndTransformable* upperTwist1 = dir->Find<RndTransformable>(upperTwist1Name, false);
            RndTransformable* upperTwist2 = dir->Find<RndTransformable>(upperTwist2Name, false);

            if (upperTwist2 && upperTwist1 && upperArm) {
                SolveUpperTwistPoll(upperTwist2, upperTwist1, upperArm);
            }
        }
    }

    if (!sawForeTwist) {
        // Use the same authored forearm-twist defaults observed in live gameplay:
        // left offset=0, right offset=180, bias=0 on the loaded CharForeTwist objects.
        // CharacterTest::AddDefaults() still uses older +/-90 fallback values, but those
        // do not match the runtime setup that gameplay is actually using.
        struct ForeTwistSetup {
            const char* hand; const char* twist2; float offset; float bias;
        };
        ForeTwistSetup foreSetups[] = {
            {"bone_L-hand.mesh", "bone_L-foreTwist2.mesh", 0.0f, 0.0f},
            {"bone_R-hand.mesh", "bone_R-foreTwist2.mesh", 180.0f, 0.0f},
        };
        for (auto& s : foreSetups) {
            SolveForeTwist(
                dir->Find<RndTransformable>(s.hand, false),
                dir->Find<RndTransformable>(s.twist2, false),
                s.offset, s.bias);
        }
    }

    if (!sawNeckTwist) {
        // Neck twist — CharNeckTwist::Poll() applies half of head yaw
        RndTransformable* neckTwist = dir->Find<RndTransformable>("bone_neckTwist.mesh", false);
        RndTransformable* headBone = dir->Find<RndTransformable>("bone_head.mesh", false);
        SolveNeckTwist(neckTwist, headBone);
    }

    // Periodic post-solve arm geometry check: is the arm actually collinear?
    static int sArmCheck = 0;
    if (sArmCheck < 10 || (sArmCheck % 300 == 0 && sArmCheck < 3000)) {
        sArmCheck++;
        RndTransformable* upperArm = dir->Find<RndTransformable>("bone_L-upperArm.mesh", false);
        RndTransformable* foreArm = dir->Find<RndTransformable>("bone_L-foreArm.mesh", false);
        RndTransformable* hand = dir->Find<RndTransformable>("bone_L-hand.mesh", false);
        if (upperArm && foreArm && hand) {
            Vector3 upper2fore, fore2hand, bendCross;
            Subtract(foreArm->WorldXfm().v, upperArm->WorldXfm().v, upper2fore);
            Subtract(hand->WorldXfm().v, foreArm->WorldXfm().v, fore2hand);
            Cross(upper2fore, fore2hand, bendCross);
            float upperLen = Length(upper2fore);
            float foreLen = Length(fore2hand);
            float crossLen = Length(bendCross);
            float bendSin = (upperLen > 1e-5f && foreLen > 1e-5f)
                ? crossLen / (upperLen * foreLen) : 0.0f;
            const Hmx::Matrix3& foreRot = foreArm->LocalXfm().m;
            bool rotIsIdentity = (std::fabs(foreRot.x.x - 1.0f) < 0.01f
                               && std::fabs(foreRot.y.y - 1.0f) < 0.01f
                               && std::fabs(foreRot.z.z - 1.0f) < 0.01f
                               && std::fabs(foreRot.x.y) < 0.01f
                               && std::fabs(foreRot.x.z) < 0.01f);
            fprintf(stderr, "ARM-CHECK[%d] bendSin=%.4f %s foreArmRotIdentity=%s "
                    "constraint=%d saw(upper=%d fore=%d)\n",
                    sArmCheck, bendSin,
                    bendSin < 0.05f ? "*** COLLINEAR ***" : "bent",
                    rotIsIdentity ? "YES" : "no",
                    (int)foreArm->TransConstraint(),
                    sawUpperTwist, sawForeTwist);
        }
    } else {
        sArmCheck++;
    }
}
