// DC3 Native Port — Bone setup utilities
// Extracted from Mesh_Wgpu.cpp to share bone matrix computation
// between DrawMeshImmediate and DrawMeshShadow.

#include "platform/BoneSetup.h"
#include "platform/TransformUtils.h"
#include "rndobj/Rnd.h"
#include "rndobj/Mesh.h"
#include "rndobj/Trans.h"
#include "math/Mtx.h"
#include "math/Vec.h"
#include <cstring>
#include <cstdlib>

// Dummy bone bind group for static meshes (pipeline layout requires group 3)
static wgpu::Buffer sDummyBoneBuffer;
static wgpu::BindGroup sDummyBoneBindGroup;

static int sBoneGarbageLogCount = 0;

static int DebugArmChainFrame() {
    static bool sInit = false;
    static int sFrame = -1;
    if (!sInit) {
        sInit = true;
        const char* env = getenv("MILO_DEBUG_ARM_CHAIN_FRAME");
        if (env && env[0]) sFrame = atoi(env);
    }
    return sFrame;
}

static const char* DebugArmChainDir() {
    static bool sInit = false;
    static const char* sDir = nullptr;
    if (!sInit) {
        sInit = true;
        sDir = getenv("MILO_DEBUG_ARM_CHAIN_DIR");
        if (!sDir || !sDir[0]) sDir = "player0";
    }
    return sDir;
}

static void MaybeDumpArmChain(int frameID, RndTransformable* boneTrans) {
    static bool sDumpedForFrame = false;
    static int sLastFrame = -1;
    if (frameID != sLastFrame) {
        sLastFrame = frameID;
        sDumpedForFrame = false;
    }
    if (sDumpedForFrame || frameID != DebugArmChainFrame() || !boneTrans) return;
    const char* boneName = boneTrans->Name();
    if (strcmp(boneName, "bone_L-hand.mesh") != 0 && strcmp(boneName, "bone_R-hand.mesh") != 0)
        return;
    ObjectDir* dir = boneTrans->Dir();
    if (!dir || strcmp(dir->Name(), DebugArmChainDir()) != 0)
        return;

    auto dumpSide = [&](const char* handName) {
        RndTransformable* hand = dir->Find<RndTransformable>(handName, false);
        if (!hand) return;
        RndTransformable* foreArm = hand->TransParent();
        RndTransformable* upperArm = foreArm ? foreArm->TransParent() : nullptr;
        const char* leftPrefix = strcmp(handName, "bone_L-hand.mesh") == 0 ? "bone_L-" : "bone_R-";
        char twist1Name[64];
        char twist2Name[64];
        snprintf(twist1Name, sizeof(twist1Name), "%sforeTwist1.mesh", leftPrefix);
        snprintf(twist2Name, sizeof(twist2Name), "%sforeTwist2.mesh", leftPrefix);
        RndTransformable* twist1 = dir->Find<RndTransformable>(twist1Name, false);
        RndTransformable* twist2 = dir->Find<RndTransformable>(twist2Name, false);
        if (!foreArm || !upperArm) {
            fprintf(stderr,
                    "ARM-CHAIN frame=%d dir='%s' hand='%s' missing parents foreArm=%p upperArm=%p\n",
                    frameID, dir->Name(), handName, (void*)foreArm, (void*)upperArm);
            return;
        }

        Vector3 upperToFore, foreToHand, bendCross;
        Subtract(foreArm->WorldXfm().v, upperArm->WorldXfm().v, upperToFore);
        Subtract(hand->WorldXfm().v, foreArm->WorldXfm().v, foreToHand);
        Cross(upperToFore, foreToHand, bendCross);
        float upperLen = Length(upperToFore);
        float foreLen = Length(foreToHand);
        float crossLen = Length(bendCross);
        float bendSin = 0.0f;
        if (upperLen > 1e-5f && foreLen > 1e-5f) {
            bendSin = crossLen / (upperLen * foreLen);
        }

        fprintf(stderr,
                "\n=== ARM CHAIN frame=%d dir='%s' hand='%s' ===\n",
                frameID, dir->Name(), handName);
        auto dumpBone = [](const char* label, RndTransformable* t) {
            const Transform& local = t->LocalXfm();
            const Transform& world = t->WorldXfm();
            fprintf(stderr,
                    "  %s name='%s' parent='%s'\n"
                    "    localPos=(%.3f, %.3f, %.3f)\n"
                    "    localRot=[%.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f]\n"
                    "    worldPos=(%.3f, %.3f, %.3f)\n"
                    "    worldRot=[%.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f]\n",
                    label,
                    t->Name(),
                    t->TransParent() ? t->TransParent()->Name() : "(none)",
                    local.v.x, local.v.y, local.v.z,
                    local.m.x.x, local.m.x.y, local.m.x.z,
                    local.m.y.x, local.m.y.y, local.m.y.z,
                    local.m.z.x, local.m.z.y, local.m.z.z,
                    world.v.x, world.v.y, world.v.z,
                    world.m.x.x, world.m.x.y, world.m.x.z,
                    world.m.y.x, world.m.y.y, world.m.y.z,
                    world.m.z.x, world.m.z.y, world.m.z.z);
        };
        dumpBone("upperArm", upperArm);
        dumpBone("foreArm", foreArm);
        dumpBone("hand", hand);
        if (twist1) dumpBone("foreTwist1", twist1);
        if (twist2) dumpBone("foreTwist2", twist2);
        fprintf(stderr,
                "  chain upper->fore=(%.3f, %.3f, %.3f) len=%.3f\n"
                "  chain fore->hand=(%.3f, %.3f, %.3f) len=%.3f\n"
                "  bendCross=(%.3f, %.3f, %.3f) |cross|=%.5f bendSin=%.5f\n",
                upperToFore.x, upperToFore.y, upperToFore.z, upperLen,
                foreToHand.x, foreToHand.y, foreToHand.z, foreLen,
                bendCross.x, bendCross.y, bendCross.z, crossLen, bendSin);
        if (twist1) {
            Vector3 upperToTwist1;
            Subtract(twist1->WorldXfm().v, upperArm->WorldXfm().v, upperToTwist1);
            fprintf(stderr,
                    "  chain upper->foreTwist1=(%.3f, %.3f, %.3f) len=%.3f\n",
                    upperToTwist1.x, upperToTwist1.y, upperToTwist1.z, Length(upperToTwist1));
        }
        if (twist2) {
            Vector3 upperToTwist2, twist2ToHand;
            Subtract(twist2->WorldXfm().v, upperArm->WorldXfm().v, upperToTwist2);
            Subtract(hand->WorldXfm().v, twist2->WorldXfm().v, twist2ToHand);
            fprintf(stderr,
                    "  chain upper->foreTwist2=(%.3f, %.3f, %.3f) len=%.3f\n"
                    "  chain foreTwist2->hand=(%.3f, %.3f, %.3f) len=%.3f\n",
                    upperToTwist2.x, upperToTwist2.y, upperToTwist2.z, Length(upperToTwist2),
                    twist2ToHand.x, twist2ToHand.y, twist2ToHand.z, Length(twist2ToHand));
        }
    };

    dumpSide("bone_L-hand.mesh");
    dumpSide("bone_R-hand.mesh");
    sDumpedForFrame = true;
}

void FillBoneUniforms(RndMesh* mesh, BoneUniforms& out) {
    memset(&out, 0, sizeof(out));

    int numBones = mesh->NumBones();
    if (numBones > kMaxBones) numBones = kMaxBones;

    // Periodic diagnostic: dump arm bone transforms every 200 frames after startup
    static int sBoneDiagFrames = 0;
    static int sBoneDiagCount = 0;
    sBoneDiagFrames++;
    // Fire at frames 1000, 1200, 1400 for the main body mesh (40 bones)
    bool doDiag = (sBoneDiagFrames >= 1000 && sBoneDiagCount < 3
                   && numBones >= 20 && mesh->Name()[0]
                   && (sBoneDiagFrames % 200 == 0));
    if (doDiag) {
        sBoneDiagCount++;
        fprintf(stderr, "\n=== BONE DIAG mesh='%s' numBones=%d frame=%d ===\n",
                mesh->Name(), numBones, sBoneDiagFrames);
        // Dump all bone names first
        fprintf(stderr, "  ALL BONES:");
        for (int b = 0; b < numBones; b++) {
            RndTransformable* bt = mesh->BoneTransAt(b);
            fprintf(stderr, " [%d]'%s'", b, bt ? bt->Name() : "NULL");
        }
        fprintf(stderr, "\n");
    }

    for (int i = 0; i < numBones; i++) {
        RndTransformable* boneTrans = mesh->BoneTransAt(i);
        if (boneTrans) {
            const Transform& wt = boneTrans->WorldXfm();
            MaybeDumpArmChain((int)TheRnd.GetFrameID(), boneTrans);

            // Log arm-related bones + first 3 for context
            bool isArm = (strstr(boneTrans->Name(), "Arm") || strstr(boneTrans->Name(), "arm")
                       || strstr(boneTrans->Name(), "shoulder") || strstr(boneTrans->Name(), "Shoulder")
                       || strstr(boneTrans->Name(), "clavicle") || strstr(boneTrans->Name(), "hand")
                       || strstr(boneTrans->Name(), "elbow") || strstr(boneTrans->Name(), "foreTwist")
                       || strstr(boneTrans->Name(), "Twist") || strstr(boneTrans->Name(), "upperArm")
                       || strstr(boneTrans->Name(), "foreArm"));
            if (doDiag && (i < 3 || isArm)) {
                const Transform& local = boneTrans->LocalXfm();
                const Transform& offset = mesh->BoneOffsetAt(i);
                RndTransformable* parent = boneTrans->TransParent();
                fprintf(stderr, "  bone[%d] '%s' ptr=%p parent='%s' dirty=%d\n",
                        i, boneTrans->Name(), (void*)boneTrans,
                        parent ? parent->Name() : "(none)",
                        boneTrans->Dirty());
                fprintf(stderr, "    localRot: [%.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f]\n",
                        local.m.x.x, local.m.x.y, local.m.x.z,
                        local.m.y.x, local.m.y.y, local.m.y.z,
                        local.m.z.x, local.m.z.y, local.m.z.z);
                fprintf(stderr, "    localPos: (%.3f, %.3f, %.3f)\n", local.v.x, local.v.y, local.v.z);
                fprintf(stderr, "    worldRot: [%.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f]\n",
                        wt.m.x.x, wt.m.x.y, wt.m.x.z,
                        wt.m.y.x, wt.m.y.y, wt.m.y.z,
                        wt.m.z.x, wt.m.z.y, wt.m.z.z);
                fprintf(stderr, "    worldPos: (%.3f, %.3f, %.3f)\n", wt.v.x, wt.v.y, wt.v.z);
                fprintf(stderr, "    offset:   [%.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f] + (%.3f, %.3f, %.3f)\n",
                        offset.m.x.x, offset.m.x.y, offset.m.x.z,
                        offset.m.y.x, offset.m.y.y, offset.m.y.z,
                        offset.m.z.x, offset.m.z.y, offset.m.z.z,
                        offset.v.x, offset.v.y, offset.v.z);
            }

            bool valid = (fabsf(wt.v.x) < 100000.0f &&
                          fabsf(wt.v.y) < 100000.0f &&
                          fabsf(wt.v.z) < 100000.0f);
            if (valid) {
                Transform skinMatrix;
                Multiply(mesh->BoneOffsetAt(i), wt, skinMatrix);

                // TODO HACK: Raise foot bones at render time to compensate
                // for IK ankle corrections lost to pelvis dirty cascade.
                // The IK correctly sets ankle Z during Poll, but pelvis IK
                // runs after and cascades SetDirty through the leg chain,
                // causing WorldXfm_Force to recompute from stale mLocalXfm.
                // This render-time offset is purely visual — no IK feedback.
                // Tunable via DC3_FOOT_OFFSET env var (default 3.5, 0 to disable).
                // Remove when dirty cascade root cause is fixed.
                {
                    static float sFootOffset = -1.0f;
                    if (sFootOffset < 0.0f) {
                        const char* env = getenv("DC3_FOOT_OFFSET");
                        sFootOffset = (env && env[0]) ? (float)atof(env) : 3.5f;
                    }
                    if (sFootOffset > 0.0f) {
                        const char* name = boneTrans->Name();
                        if (strstr(name, "ankle") || strstr(name, "toe")
                            || strstr(name, "ball")) {
                            skinMatrix.v.z += sFootOffset;
                        }
                    }
                }

                TransformToMat4(skinMatrix, out.bones[i]);
                if (doDiag && (i < 3 || isArm)) {
                    fprintf(stderr, "    skin:     [%.3f %.3f %.3f %.1f / %.3f %.3f %.3f %.1f / %.3f %.3f %.3f %.1f / %.3f %.3f %.3f %.1f]\n",
                            out.bones[i][0], out.bones[i][1], out.bones[i][2], out.bones[i][3],
                            out.bones[i][4], out.bones[i][5], out.bones[i][6], out.bones[i][7],
                            out.bones[i][8], out.bones[i][9], out.bones[i][10], out.bones[i][11],
                            out.bones[i][12], out.bones[i][13], out.bones[i][14], out.bones[i][15]);
                }
            } else {
                if (sBoneGarbageLogCount < 20) {
                    fprintf(stderr, "BoneSetup: garbage WorldXfm bone[%d] '%s' on mesh '%s' pos=(%.2e,%.2e,%.2e)\n",
                            i, boneTrans->Name(), mesh->Name(),
                            wt.v.x, wt.v.y, wt.v.z);
                    sBoneGarbageLogCount++;
                }
                out.bones[i][0]  = 1.0f;
                out.bones[i][5]  = 1.0f;
                out.bones[i][10] = 1.0f;
                out.bones[i][15] = 1.0f;
            }
        } else {
            out.bones[i][0]  = 1.0f;
            out.bones[i][5]  = 1.0f;
            out.bones[i][10] = 1.0f;
            out.bones[i][15] = 1.0f;
        }
    }

    // Fill remaining slots with identity
    for (int i = numBones; i < kMaxBones; i++) {
        out.bones[i][0]  = 1.0f;
        out.bones[i][5]  = 1.0f;
        out.bones[i][10] = 1.0f;
        out.bones[i][15] = 1.0f;
    }
}

void EnsureDummyBoneBindGroup() {
    if (sDummyBoneBindGroup) return;
    if (!gWgpuRnd) return;

    // Create a small buffer with identity matrices
    BoneUniforms identity{};
    memset(&identity, 0, sizeof(identity));
    for (int i = 0; i < kMaxBones; i++) {
        identity.bones[i][0]  = 1.0f; // m[0][0]
        identity.bones[i][5]  = 1.0f; // m[1][1]
        identity.bones[i][10] = 1.0f; // m[2][2]
        identity.bones[i][15] = 1.0f; // m[3][3]
    }

    wgpu::BufferDescriptor bd{};
    bd.label = "DummyBones";
    bd.size = sizeof(BoneUniforms);
    bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    sDummyBoneBuffer = gWgpuRnd->Gpu().Device().CreateBuffer(&bd);
    gWgpuRnd->Gpu().Queue().WriteBuffer(sDummyBoneBuffer, 0, &identity, sizeof(identity));

    wgpu::BindGroupEntry entry{};
    entry.binding = 0;
    entry.buffer = sDummyBoneBuffer;
    entry.offset = 0;
    entry.size = sizeof(BoneUniforms);

    wgpu::BindGroupDescriptor desc{};
    desc.layout = gWgpuRnd->Pipelines().BoneLayout();
    desc.entryCount = 1;
    desc.entries = &entry;
    sDummyBoneBindGroup = gWgpuRnd->Gpu().Device().CreateBindGroup(&desc);
}

wgpu::BindGroup GetDummyBoneBindGroup() {
    return sDummyBoneBindGroup;
}

void BoneSetupTerminate() {
    sDummyBoneBuffer = nullptr;
    sDummyBoneBindGroup = nullptr;
}
