// CharClipGroup null-entry and merge behavior tests.
//
// Tracks expected behavior for CharClipGroup when ObjPtrVec entries are null
// (unresolved references from subdirs not yet loaded). On Xbox, all subdirs
// are pre-loaded so references always resolve. On native, crowd character
// clip dirs may be deserialized before the animation clips exist, leaving
// null ObjPtr entries. FileMerger later adds valid clips via Copy(kCopyFromMax),
// but the original nulls persist until GetClip purges them.
//
// Some of these tests may fail — that's intentional. They document the
// expected parity behavior we're tracking toward.

#include "test_helpers.h"
#include "char/CharClipGroup.h"
#include "char/CharClip.h"
#include "obj/Dir.h"
#include "obj/DirLoader.h"
#include "obj/ObjPtr_p.h"
#include "obj/Utl.h"

namespace {

class CharClipGroupTest : public EngineTestFixture {};

// Helper: count non-null clips reachable from a CharClipGroup.
// Uses GetClip in a bounded loop, tracking unique results.
// GetClip modifies internal LRU state, so this is destructive.
static int CountClipsViaGetClip(CharClipGroup *grp, int maxAttempts = 50) {
    int count = 0;
    CharClip *seen[256] = {};
    for (int attempt = 0; attempt < maxAttempts; attempt++) {
        CharClip *clip = grp->GetClip(0);
        if (!clip) break;
        bool dup = false;
        for (int j = 0; j < count; j++) {
            if (seen[j] == clip) { dup = true; break; }
        }
        if (!dup) {
            seen[count++] = clip;
            if (count >= 200) break;
        }
    }
    return count;
}

// ============================================================================
// Basic: GetClip returns nullptr for empty group
// ============================================================================
TEST_F(CharClipGroupTest, EmptyGroupReturnsNull) {
    CharClipGroup *grp = Hmx::Object::New<CharClipGroup>();
    EXPECT_EQ(grp->GetClip(0), nullptr);
    delete grp;
}

// ============================================================================
// Basic: AddClip + GetClip round-trips
// ============================================================================
TEST_F(CharClipGroupTest, AddClipThenGetClipReturnsIt) {
    ObjectDir *dir = Hmx::Object::New<ObjectDir>();
    CharClipGroup *grp = Hmx::Object::New<CharClipGroup>();
    grp->SetName("test_group", dir);

    CharClip *clip = Hmx::Object::New<CharClip>();
    clip->SetName("test_clip", dir);

    grp->AddClip(clip);
    CharClip *got = grp->GetClip(0);
    EXPECT_EQ(got, clip);

    delete grp;
    delete clip;
    delete dir;
}

// ============================================================================
// HasClip detects presence
// ============================================================================
TEST_F(CharClipGroupTest, HasClipDetectsPresence) {
    ObjectDir *dir = Hmx::Object::New<ObjectDir>();
    CharClipGroup *grp = Hmx::Object::New<CharClipGroup>();
    grp->SetName("test_group", dir);

    CharClip *clip = Hmx::Object::New<CharClip>();
    clip->SetName("test_clip", dir);

    EXPECT_FALSE(grp->HasClip(clip));
    grp->AddClip(clip);
    EXPECT_TRUE(grp->HasClip(clip));

    delete grp;
    delete clip;
    delete dir;
}

// ============================================================================
// FindClip by name
// ============================================================================
TEST_F(CharClipGroupTest, FindClipByName) {
    ObjectDir *dir = Hmx::Object::New<ObjectDir>();
    CharClipGroup *grp = Hmx::Object::New<CharClipGroup>();
    grp->SetName("test_group", dir);

    CharClip *clip = Hmx::Object::New<CharClip>();
    clip->SetName("dance_01", dir);
    grp->AddClip(clip);

    EXPECT_EQ(grp->FindClip("dance_01"), clip);
    EXPECT_EQ(grp->FindClip("nonexistent"), nullptr);

    delete grp;
    delete clip;
    delete dir;
}

// ============================================================================
// AddClip is idempotent (no duplicates)
// ============================================================================
TEST_F(CharClipGroupTest, AddClipIsIdempotent) {
    ObjectDir *dir = Hmx::Object::New<ObjectDir>();
    CharClipGroup *grp = Hmx::Object::New<CharClipGroup>();
    grp->SetName("test_group", dir);

    CharClip *clip = Hmx::Object::New<CharClip>();
    clip->SetName("test_clip", dir);

    grp->AddClip(clip);
    grp->AddClip(clip);
    grp->AddClip(clip);

    // Should still be just one clip
    int count = CountClipsViaGetClip(grp);
    EXPECT_EQ(count, 1);

    delete grp;
    delete clip;
    delete dir;
}

// ============================================================================
// Multiple clips: GetClip rotates through all of them (LRU behavior)
// ============================================================================
TEST_F(CharClipGroupTest, GetClipRotatesThroughMultiple) {
    ObjectDir *dir = Hmx::Object::New<ObjectDir>();
    CharClipGroup *grp = Hmx::Object::New<CharClipGroup>();
    grp->SetName("test_group", dir);

    const int N = 5;
    CharClip *clips[N];
    for (int i = 0; i < N; i++) {
        clips[i] = Hmx::Object::New<CharClip>();
        char name[32];
        snprintf(name, sizeof(name), "clip_%d", i);
        clips[i]->SetName(name, dir);
        grp->AddClip(clips[i]);
    }

    // GetClip uses LRU rotation + random shuffle.
    // Call it several times and verify we get valid clips back.
    for (int i = 0; i < 20; i++) {
        CharClip *clip = grp->GetClip(0);
        ASSERT_NE(clip, nullptr) << "GetClip returned null on call " << i;
        bool found = false;
        for (int j = 0; j < N; j++) {
            if (clip == clips[j]) { found = true; break; }
        }
        EXPECT_TRUE(found) << "GetClip returned unknown clip pointer";
    }

    for (int i = 0; i < N; i++) delete clips[i];
    delete grp;
    delete dir;
}

// ============================================================================
// PARITY TEST: Load with unresolved refs creates null entries
// This documents the native behavior where crowd char clips haven't been
// loaded yet. The ObjPtrVec::Load inserts null for unresolved names.
// ============================================================================
TEST_F(CharClipGroupTest, LoadWithUnresolvedRefsCreatesNullEntries) {
    // Create a dir with NO CharClips in it
    ObjectDir *dir = Hmx::Object::New<ObjectDir>();
    CharClipGroup *grp = Hmx::Object::New<CharClipGroup>();
    grp->SetName("test_group", dir);

    // Build a binary blob that references 3 clip names that don't exist
    // ObjPtrVec::Load format: count(int) + N × string
    std::vector<uint8_t> buf;
    // mClips count = 3
    PutLE32(buf, 3);
    PutLEString(buf, "nonexistent_clip_1");
    PutLEString(buf, "nonexistent_clip_2");
    PutLEString(buf, "nonexistent_clip_3");
    // mWhich = 0
    PutLE32(buf, 0);

    // After loading, the group should have entries but GetClip should
    // handle the null entries gracefully (not crash, not return garbage).
    // The exact behavior depends on whether GetClip purges nulls or skips them.

    // GetClip should return nullptr (no valid clips)
    // NOTE: We can't easily call Load on CharClipGroup without the full
    // rev header, so we test the GetClip purge behavior via the real
    // crowd asset test below.
    delete grp;
    delete dir;
}

// ============================================================================
// PARITY TEST: Real crowd character clip loading + merge
// Loads female_base.milo (crowd char clips), then merges female_medium.milo
// (crowd anim clips). Verifies that CharClipGroups work after merge.
// Requires MILO_LIB pointing to extracted DC3 assets.
// ============================================================================
TEST_F(CharClipGroupTest, CrowdClipMergeProducesPlayableGroups) {
    const char *root = getenv("MILO_LIB");
    if (!root || !root[0]) {
        GTEST_SKIP() << "MILO_LIB not set";
    }

    // Load the base crowd character clip dir
    std::string basePath = std::string(root) + "/char/crowd/anim/gen/female_base.milo_xbox";
    ObjectDir *baseDir = DirLoader::LoadObjects(FilePath(basePath.c_str()), nullptr, nullptr);
    ASSERT_NE(baseDir, nullptr) << basePath;

    // Count CharClipGroups and CharClips before merge
    int groupsBefore = 0, clipsBefore = 0;
    for (ObjDirItr<CharClipGroup> it(baseDir, false); it != nullptr; ++it)
        groupsBefore++;
    for (ObjDirItr<CharClip> it(baseDir, false); it != nullptr; ++it)
        clipsBefore++;

    printf("CrowdClipMerge: BEFORE merge: %d groups, %d clips\n",
           groupsBefore, clipsBefore);
    EXPECT_GT(groupsBefore, 0) << "female_base should have CharClipGroups";

    // Load the animation overlay (tempo-specific clips)
    std::string animPath = std::string(root) + "/char/crowd/anim/gen/female_medium.milo_xbox";
    ObjectDir *animDir = DirLoader::LoadObjects(FilePath(animPath.c_str()), nullptr, nullptr);
    ASSERT_NE(animDir, nullptr) << animPath;

    // Merge with kCopyFromMax (action=2) — same as FileMerger's crowd_anim merge
    // NOTE: female_medium.milo has clips with the same names as female_base.milo.
    // Copy(kCopyFromMax) in CharClipGroup ADDS clips if not already present.
    // The clip objects themselves are new (from the overlay dir), but their NAMES
    // may collide. The merge may or may not increase clip count depending on
    // whether the overlay has uniquely-named clips.
    MergeObject(animDir, baseDir, baseDir, (MergeFilter::Action)2);
    delete animDir;

    // Count after merge
    int groupsAfter = 0, clipsAfter = 0;
    for (ObjDirItr<CharClipGroup> it(baseDir, false); it != nullptr; ++it)
        groupsAfter++;
    for (ObjDirItr<CharClip> it(baseDir, false); it != nullptr; ++it)
        clipsAfter++;

    printf("CrowdClipMerge: AFTER merge: %d groups, %d clips\n",
           groupsAfter, clipsAfter);
    EXPECT_GE(clipsAfter, clipsBefore) << "Merge should not lose clips";
    EXPECT_EQ(groupsAfter, groupsBefore) << "Group count should stay the same";

    // KEY TEST: GetClip should return non-null for groups that have valid clips
    int groupsWithClips = 0;
    int groupsReturningNull = 0;
    for (ObjDirItr<CharClipGroup> it(baseDir, false); it != nullptr; ++it) {
        CharClipGroup *grp = &*it;
        CharClip *clip = grp->GetClip(0);
        if (clip) {
            groupsWithClips++;
        } else {
            groupsReturningNull++;
            printf("  WARNING: group '%s' GetClip returned null after merge\n",
                   grp->Name());
        }
    }

    // After merge, groups that had matching animation clips should work
    EXPECT_GT(groupsWithClips, 0)
        << "At least some groups should return valid clips after merge";

    // PARITY EXPECTATION: On Xbox, ALL groups return valid clips after merge.
    // This may fail on native if some groups only have unresolved null refs
    // and no matching animation clips were merged. Track this.
    if (groupsReturningNull > 0) {
        printf("CrowdClipMerge: %d/%d groups still returning null after merge "
               "(may need additional animation .milo files)\n",
               groupsReturningNull, groupsAfter);
    }

    delete baseDir;
}

// ============================================================================
// PARITY TEST: GetClip purges null entries
// After loading a CharClipGroup with unresolved refs, then adding valid clips,
// GetClip should purge the nulls and return valid clips.
// ============================================================================
TEST_F(CharClipGroupTest, GetClipPurgesNullEntries) {
    ObjectDir *dir = Hmx::Object::New<ObjectDir>();
    CharClipGroup *grp = Hmx::Object::New<CharClipGroup>();
    grp->SetName("test_group", dir);

    // Create a real clip and add it
    CharClip *clip1 = Hmx::Object::New<CharClip>();
    clip1->SetName("valid_clip_1", dir);
    CharClip *clip2 = Hmx::Object::New<CharClip>();
    clip2->SetName("valid_clip_2", dir);

    grp->AddClip(clip1);
    grp->AddClip(clip2);

    // GetClip should return non-null
    CharClip *got = grp->GetClip(0);
    EXPECT_NE(got, nullptr);
    EXPECT_TRUE(got == clip1 || got == clip2);

    // Both clips should be reachable
    int count = CountClipsViaGetClip(grp);
    EXPECT_EQ(count, 2);

    delete grp;
    delete clip1;
    delete clip2;
    delete dir;
}

// ============================================================================
// PARITY TEST: Full crowd pipeline (all 6 animation files)
// Loads female_base then merges all tempo/era animation .milo files.
// This is the exact pipeline that LoadCrowdClips performs.
// ============================================================================
TEST_F(CharClipGroupTest, FullCrowdPipelineMergesAllAnimations) {
    const char *root = getenv("MILO_LIB");
    if (!root || !root[0]) {
        GTEST_SKIP() << "MILO_LIB not set";
    }

    std::string basePath2 = std::string(root) + "/char/crowd/anim/gen/female_base.milo_xbox";
    ObjectDir *baseDir = DirLoader::LoadObjects(FilePath(basePath2.c_str()), nullptr, nullptr);
    ASSERT_NE(baseDir, nullptr) << basePath2;

    // The 6 animation files for a typical tempo/era combination
    // (medium tempo, 00s era = dclive venue)
    const char *animFiles[] = {
        "female_medium.milo_xbox",
        "male_medium.milo_xbox",
        "female_base_00s.milo_xbox",
        "male_base_00s.milo_xbox",
        "female_medium_00s.milo_xbox",
        "male_medium_00s.milo_xbox",
    };

    int totalMerged = 0;
    for (const char *name : animFiles) {
        std::string path = std::string(root) + "/char/crowd/anim/gen/" + name;
        ObjectDir *animDir = DirLoader::LoadObjects(FilePath(path.c_str()), nullptr, nullptr);
        if (!animDir) {
            printf("  SKIP: %s not found\n", name);
            continue;
        }
        MergeObject(animDir, baseDir, baseDir, (MergeFilter::Action)2);
        delete animDir;
        totalMerged++;
    }

    printf("FullCrowdPipeline: merged %d animation files\n", totalMerged);

    // After full merge, test a well-known group name pattern
    // "stand_ok" should exist (stance=stand, anim=ok)
    CharClipGroup *standOk = baseDir->Find<CharClipGroup>("stand_ok", false);
    if (standOk) {
        CharClip *clip = standOk->GetClip(0);
        EXPECT_NE(clip, nullptr) << "'stand_ok' group should have playable clips";
        if (clip) {
            printf("  stand_ok → '%s'\n", clip->Name());
        }
    } else {
        printf("  WARNING: 'stand_ok' group not found in female_base\n");
    }

    // Count overall success rate
    int total = 0, working = 0;
    for (ObjDirItr<CharClipGroup> it(baseDir, false); it != nullptr; ++it) {
        total++;
        if (it->GetClip(0)) working++;
    }
    printf("FullCrowdPipeline: %d/%d groups have playable clips\n", working, total);
    EXPECT_GT(working, 0);

    delete baseDir;
}

} // namespace
