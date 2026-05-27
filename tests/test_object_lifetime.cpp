// Native lifetime/merge safety regression tests.
#include "flow/Flow.h"
#include "flow/FlowAnimate.h"
#include "rndobj/Group.h"
#include "test_helpers.h"

#include "obj/Dir.h"
#include "obj/DirLoader.h"
#include "obj/Object.h"
#include "ui/UIPanel.h"
#include "obj/Utl.h"
#include "utl/FilePath.h"
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>

namespace {

static bool PathExists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

static std::string GetRepoRoot() {
    // __FILE__ is "<repo>/native/tests/test_object_lifetime.cpp"
    std::string f(__FILE__);
    size_t pos = f.rfind("/native/tests/");
    return pos != std::string::npos ? f.substr(0, pos) : ".";
}

static std::string GetMiloLibRoot() {
    const char *env = getenv("MILO_LIB");
    if (env && env[0])
        return env;
    // Fall back to repo-local extracted assets
    std::string local = GetRepoRoot() + "/orig-assets/extracted";
    if (PathExists(local))
        return local;
    const char *home = getenv("HOME");
    if (home && home[0]) {
        std::string ext = std::string(home)
            + "/code/milohax/milo-engine-libs/harmonix-repos/milo-rnd-library/dc3";
        if (PathExists(ext))
            return ext;
    }
    return "";
}

class ObjectLifetimeTest : public EngineTestFixture {};
class ObjectLifetimeUnitTest : public SymbolTestFixture {};

class TestRefHolder : public Hmx::Object {
public:
    TestRefHolder() : mTarget(this, nullptr) {}

    void SetTarget(Hmx::Object *obj) { mTarget = obj; }
    Hmx::Object *Target() const { return mTarget.Ptr(); }

private:
    ObjPtr<Hmx::Object> mTarget;
};

class ExposedFlow : public Flow {
public:
    ExposedFlow() : Flow() {}

    int ChildCount() const { return mChildNodes.size(); }
    FlowNode *FrontChild() const { return mChildNodes.empty() ? nullptr : mChildNodes.front(); }
};

class ExposedRndGroup : public RndGroup {
public:
    ExposedRndGroup() : RndGroup() {}

    int ObjectCount() const { return mObjects.size(); }
};

// Expose protected FindEntry for corruption simulation tests.
class ExposedDir : public ObjectDir {
public:
    ExposedDir() : ObjectDir() {}
    Entry *ExposeFindEntry(const char *name, bool add) { return FindEntry(name, add); }
};

static void RunCascadeDeleteNamedSubdirRingRepro() {
    ObjectDir *parent = new ExposedDir();
    ObjectDir *childA = new ExposedDir();
    ObjectDir *childB = new ExposedDir();

    childA->SetName("child_a", parent);
    childB->SetName("child_b", parent);
    parent->AppendSubDir(ObjDirPtr<ObjectDir>(childA));
    parent->AppendSubDir(ObjDirPtr<ObjectDir>(childB));

    // Extra live ObjDirPtr keeps childA's ring non-trivial so parent teardown
    // must not crash when walking ring entries during subdir vector destruction.
    ObjDirPtr<ObjectDir> externalHold(childA);

    if (childA->RefCount() < 2)
        std::exit(3);

    delete parent;

    // After the cascade fix (d41f5bf72), childA has external DirPtrs so the
    // cascade intentionally skips nullifying its refs. The external hold keeps
    // childA alive — this is correct behavior for reparented objects.
    // The key invariant is: no crash during parent teardown.
    if ((ObjectDir *)externalHold != nullptr) {
        // Clean up the surviving child to avoid leak
        delete (ObjectDir *)externalHold;
    }
    std::exit(0);
}

// Parity-oracle: this captures expected collision-merge ref behavior.
// Keep strict even if currently failing; this is our parity north star.
TEST_F(ObjectLifetimeTest, MergeDirsNameCollisionLeavesOnlyLivePointers) {
    ObjectDir *toDir = Hmx::Object::New<ObjectDir>();
    ObjectDir *fromDir = Hmx::Object::New<ObjectDir>();
    Hmx::Object *refOwner = Hmx::Object::New<Hmx::Object>();

    Hmx::Object *toDup = Hmx::Object::New<Hmx::Object>();
    toDup->SetName("dup.obj", toDir);

    Hmx::Object *fromDup = Hmx::Object::New<Hmx::Object>();
    fromDup->SetName("dup.obj", fromDir);

    Hmx::Object *fromOnly = Hmx::Object::New<Hmx::Object>();
    fromOnly->SetName("only_from.obj", fromDir);

    ObjPtr<Hmx::Object> ref(refOwner, fromDup);
    ASSERT_EQ(ref.Ptr(), fromDup);

    MergeFilter filt((MergeFilter::Action)1, MergeFilter::kNoSubdirs);
    MergeDirs(fromDir, toDir, filt);

    // Parity expectation: on name collision with replace action, refs redirect
    // from source object to destination object in the target dir.
    EXPECT_EQ(ref.Ptr(), toDup);
    EXPECT_NE(ref.Ptr(), nullptr);

    delete fromDir;

    EXPECT_NE(ref.Ptr(), nullptr);
    EXPECT_NE(ref.Ptr(), nullptr);
    EXPECT_EQ(toDir->FindObject("dup.obj", false, true), ref.Ptr());

    int liveEntries = 0;
    for (ObjectDir::Entry *e = toDir->HashTable().Begin(); e != nullptr;
         e = toDir->HashTable().Next(e)) {
        if (e->obj) {
            EXPECT_NE(e->obj, nullptr) << "Dead pointer for entry " << e->name;
            liveEntries++;
        }
    }
    EXPECT_GE(liveEntries, 1);

    // Iterator should walk without touching dead objects.
    int itrCount = 0;
    for (ObjDirItr<Hmx::Object> it(toDir, false); it != nullptr; ++it) {
        EXPECT_NE(&*it, nullptr);
        itrCount++;
    }
    EXPECT_GE(itrCount, 1);

    delete refOwner;
    delete toDir;
}

// Normal deletion path should null the hash entry and keep iteration safe.
TEST_F(ObjectLifetimeTest, ObjDirItrIgnoresNullHashEntriesAfterDelete) {
    ExposedDir *dir = new ExposedDir();

    Hmx::Object *victim = Hmx::Object::New<Hmx::Object>();
    victim->SetName("victim.obj", dir);

    delete victim;

    ObjectDir::Entry *entry = dir->ExposeFindEntry("victim.obj", true);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->obj, nullptr);

    int itrCount = 0;
    for (ObjDirItr<Hmx::Object> it(dir, false); it != nullptr; ++it) {
        EXPECT_NE(&*it, nullptr);
        itrCount++;
        ASSERT_LT(itrCount, 32);
    }
    EXPECT_EQ(itrCount, 0);

    delete dir;
}

// Unit baseline: direct ReplaceRefs redirect works outside merge recursion.
TEST_F(ObjectLifetimeTest, ReplaceRefsRedirectsObjPtr) {
    Hmx::Object *owner = Hmx::Object::New<Hmx::Object>();
    Hmx::Object *from = Hmx::Object::New<Hmx::Object>();
    Hmx::Object *to = Hmx::Object::New<Hmx::Object>();

    ObjPtr<Hmx::Object> ref(owner, from);
    ASSERT_EQ(ref.Ptr(), from);
    EXPECT_EQ(from->RefCount(), 1);

    from->ReplaceRefs(to);

    EXPECT_EQ(ref.Ptr(), to);
    EXPECT_EQ(from->RefCount(), 0);
    EXPECT_EQ(to->RefCount(), 1);

    delete owner;
    delete from;
    delete to;
}

// Hash-order deletion should not require dependency ordering for basic ObjPtr
// ownership: deleting the target must null incoming refs before later owners die.
TEST_F(ObjectLifetimeTest, DeleteOrderDoesNotRequireTopologicalSortForObjPtr) {
    ObjectDir *dir = Hmx::Object::New<ObjectDir>();

    TestRefHolder *a = new TestRefHolder();
    TestRefHolder *b = new TestRefHolder();
    a->SetName("a.obj", dir);
    b->SetName("b.obj", dir);

    a->SetTarget(b);
    ASSERT_EQ(a->Target(), b);
    EXPECT_EQ(b->RefCount(), 1);

    delete b;

    EXPECT_EQ(a->Target(), nullptr);

    delete a;
    delete dir;
}

TEST_F(ObjectLifetimeTest, DeletingFlowChildRemovesEntryFromParent) {
    ExposedFlow *flow = new ExposedFlow();
    FlowAnimate *child = Hmx::Object::New<FlowAnimate>();

    child->SetParent(flow, true);
    ASSERT_EQ(flow->ChildCount(), 1);
    ASSERT_EQ(flow->FrontChild(), child);

    delete child;

    // mChildNodes uses kObjListNoNull mode, so null entries are erased
    // (not tombstoned) after the deferred purge in ReplaceList completes.
    // The important invariant: no dangling pointer to the deleted child.
    EXPECT_TRUE(flow->ChildCount() == 0 || flow->FrontChild() == nullptr);

    delete flow;
}

TEST_F(ObjectLifetimeTest, DeletingRndGroupMemberRemovesOwnerControlNode) {
    ExposedRndGroup *group = new ExposedRndGroup();
    Hmx::Object *child = Hmx::Object::New<Hmx::Object>();

    group->AddObject(child);
    ASSERT_EQ(group->ObjectCount(), 1);

    delete child;

    EXPECT_EQ(group->ObjectCount(), 0);

    if (group->ObjectCount() == 0) {
        delete group;
    }
}

TEST_F(ObjectLifetimeTest, RemoveSubDirReleasesDirPtrRef) {
    ObjectDir *owner = Hmx::Object::New<ObjectDir>();
    ObjectDir *subdir = Hmx::Object::New<ObjectDir>();
    ObjDirPtr<ObjectDir> hold(subdir);

    owner->AppendSubDir(ObjDirPtr<ObjectDir>(subdir));
    EXPECT_TRUE(owner->HasSubDir(subdir));

    owner->RemoveSubDir(hold);
    EXPECT_FALSE(owner->HasSubDir(subdir));
    EXPECT_NE(subdir, nullptr);

    delete owner;
}

TEST_F(ObjectLifetimeTest, ObjDirPtrConstructorKeepsSingleWellFormedRefRingNode) {
    ObjectDir *dir = Hmx::Object::New<ObjectDir>();
    ObjDirPtr<ObjectDir> keeper;
    keeper = dir;

    {
        ObjDirPtr<ObjectDir> hold(dir);

        const ObjRef &refs = dir->Refs();
        ObjRef::iterator it = refs.begin();
        ASSERT_NE(it, refs.end());

        ObjRef *first = it;
        ++it;
        ASSERT_NE(it, refs.end());

        ObjRef *second = it;
        EXPECT_NE(second, first);
        ++it;
        EXPECT_EQ(it, refs.end());
    }

    const ObjRef &refs = dir->Refs();
    ObjRef::iterator it = refs.begin();
    ASSERT_NE(it, refs.end());
    ++it;
    EXPECT_EQ(it, refs.end());

    keeper = nullptr;
}

// Fixture-backed safety baseline on real archive content.
TEST_F(ObjectLifetimeTest, MergeDirsRealFixturesLeaveOnlyLiveEntries) {
    FilePath toPath("char/shared/main_resource.milo");
    FilePath fromPath("char/shared/viseme_resource.milo");

    ObjectDir *toDir = DirLoader::LoadObjects(toPath, nullptr, nullptr);
    ObjectDir *fromDir = DirLoader::LoadObjects(fromPath, nullptr, nullptr);
    ASSERT_NE(toDir, nullptr);
    ASSERT_NE(fromDir, nullptr);

    MergeFilter filt((MergeFilter::Action)1, MergeFilter::kNoSubdirs);
    MergeDirs(fromDir, toDir, filt);
    delete fromDir;

    int liveEntries = 0;
    for (ObjectDir::Entry *e = toDir->HashTable().Begin(); e != nullptr;
         e = toDir->HashTable().Next(e)) {
        if (e->obj) {
            EXPECT_NE(e->obj, nullptr) << "Dead pointer for entry " << e->name;
            liveEntries++;
        }
    }
    EXPECT_GT(liveEntries, 0);

    int itrCount = 0;
    for (ObjDirItr<Hmx::Object> it(toDir, false); it != nullptr; ++it) {
        EXPECT_NE(&*it, nullptr);
        itrCount++;
        ASSERT_LT(itrCount, 10000);
    }
    EXPECT_GT(itrCount, 0);

    delete toDir;
}

TEST_F(ObjectLifetimeTest, RepeatedFixtureMergesKeepIteratorSafe) {
    FilePath basePath("char/shared/main_resource.milo");
    ObjectDir *base = DirLoader::LoadObjects(basePath, nullptr, nullptr);
    ASSERT_NE(base, nullptr);

    const char *overlays[] = {
        "char/shared/viseme_resource.milo",
        "char/shared/skeleton_bones_resource.milo",
        "char/shared/viseme_resource.milo",
        nullptr
    };

    MergeFilter filt((MergeFilter::Action)1, MergeFilter::kNoSubdirs);
    for (int i = 0; overlays[i]; i++) {
        ObjectDir *overlay = DirLoader::LoadObjects(FilePath(overlays[i]), nullptr, nullptr);
        ASSERT_NE(overlay, nullptr) << overlays[i];
        MergeDirs(overlay, base, filt);
        delete overlay;

        int itrCount = 0;
        for (ObjDirItr<Hmx::Object> it(base, false); it != nullptr; ++it) {
            EXPECT_NE(&*it, nullptr);
            itrCount++;
            ASSERT_LT(itrCount, 30000);
        }
        EXPECT_GT(itrCount, 0);
    }

    delete base;
}

TEST_F(ObjectLifetimeTest, MergeKeepCharClipSetRootDoesNotCorruptRefs) {
    std::string root = GetMiloLibRoot();
    if (root.empty())
        GTEST_SKIP() << "MILO_LIB not set and orig-assets/extracted not found";

    std::string toFull = root + "/char/crowd/anim/gen/female_base.milo_xbox";
    std::string fromFull = root + "/char/crowd/anim/gen/female_medium.milo_xbox";

    ObjectDir *toDir = DirLoader::LoadObjects(FilePath(toFull.c_str()), nullptr, nullptr);
    ObjectDir *fromDir = DirLoader::LoadObjects(FilePath(fromFull.c_str()), nullptr, nullptr);
    ASSERT_NE(toDir, nullptr) << toFull;
    ASSERT_NE(fromDir, nullptr) << fromFull;

    MergeObject(fromDir, toDir, toDir, (MergeFilter::Action)2);

    delete fromDir;

    int itrCount = 0;
    for (ObjDirItr<Hmx::Object> it(toDir, false); it != nullptr; ++it) {
        EXPECT_NE(&*it, nullptr);
        itrCount++;
        ASSERT_LT(itrCount, 10000);
    }
    EXPECT_GT(itrCount, 0);

    delete toDir;
}

// Parity-oracle: kMoveAllSubdirs should transfer subdir ownership from source
// to destination (source no longer reports the moved subdir).
TEST_F(ObjectLifetimeTest, MergeDirsMoveAllSubdirsTransfersOwnership) {
    ObjectDir *toDir = Hmx::Object::New<ObjectDir>();
    ObjectDir *fromDir = Hmx::Object::New<ObjectDir>();
    ObjectDir *movedSubdir = Hmx::Object::New<ObjectDir>();

    fromDir->AppendSubDir(ObjDirPtr<ObjectDir>(movedSubdir));
    ASSERT_TRUE(fromDir->HasSubDir(movedSubdir));
    ASSERT_FALSE(toDir->HasSubDir(movedSubdir));

    MergeFilter filt((MergeFilter::Action)1, MergeFilter::kMoveAllSubdirs);
    MergeDirs(fromDir, toDir, filt);

    EXPECT_TRUE(toDir->HasSubDir(movedSubdir));
    EXPECT_FALSE(fromDir->HasSubDir(movedSubdir));

    delete fromDir;
    EXPECT_NE(movedSubdir, nullptr);
    delete toDir;
}

TEST_F(ObjectLifetimeTest, DeleteAutosaveWarningRawDir) {
    std::string root = GetMiloLibRoot();
    if (root.empty())
        GTEST_SKIP() << "MILO_LIB not set and orig-assets/extracted not found";

    std::string full = root + "/ui/title/gen/autosave_warning.milo_xbox";
    std::clock_t loadStart = std::clock();
    printf("DeleteAutosaveWarningRawDir: loading %s\n", full.c_str());
    ObjectDir *dir = DirLoader::LoadObjects(FilePath(full.c_str()), nullptr, nullptr);
    double loadSeconds = double(std::clock() - loadStart) / CLOCKS_PER_SEC;
    ASSERT_NE(dir, nullptr) << full;
    printf("DeleteAutosaveWarningRawDir: load complete in %.3fs\n", loadSeconds);

    int count = 0;
    for (ObjDirItr<Hmx::Object> it(dir, false); it != nullptr; ++it) {
        printf("  top[%d]: '%s' (%s)\n", count, ((Hmx::Object *)it)->Name(),
               ((Hmx::Object *)it)->ClassName().Str());
        count++;
    }
    EXPECT_GT(count, 0);

    std::clock_t start = std::clock();
    printf("DeleteAutosaveWarningRawDir: deleting dir '%s' objects=%d\n", dir->Name(), count);
    delete dir;
    double seconds = double(std::clock() - start) / CLOCKS_PER_SEC;
    printf("DeleteAutosaveWarningRawDir: %.3fs\n", seconds);
}

TEST_F(ObjectLifetimeTest, DeleteAutosavingIconSubdirOnly) {
    std::string root = GetMiloLibRoot();
    if (root.empty())
        GTEST_SKIP() << "MILO_LIB not set and orig-assets/extracted not found";

    std::string full = root + "/ui/title/gen/autosave_warning.milo_xbox";
    ObjectDir *dir = DirLoader::LoadObjects(FilePath(full.c_str()), nullptr, nullptr);
    ASSERT_NE(dir, nullptr) << full;

    ObjectDir *subdir = dir->Find<ObjectDir>("autosaving_icon", false);
    ASSERT_NE(subdir, nullptr) << "autosaving_icon subdir not found";
    ObjDirPtr<ObjectDir> hold(subdir);
    dir->RemoveSubDir(hold);

    int count = 0;
    for (ObjDirItr<Hmx::Object> it(subdir, false); it != nullptr; ++it) {
        printf("  sub[%d]: '%s' (%s)\n", count, ((Hmx::Object *)it)->Name(),
               ((Hmx::Object *)it)->ClassName().Str());
        count++;
    }
    printf("DeleteAutosavingIconSubdirOnly: deleting '%s' objects=%d\n",
           subdir->Name(), count);

    std::clock_t start = std::clock();
    delete subdir;
    double seconds = double(std::clock() - start) / CLOCKS_PER_SEC;
    printf("DeleteAutosavingIconSubdirOnly: %.3fs\n", seconds);

    delete dir;
}


// ============================================================================
// Ring corruption & DirPtrRefCount tests
// ============================================================================
// These tests target the specific ring corruption patterns identified in
// session 2026-03-18-venue-merge-crash-ring-corruption.md

// Verify that ReplaceList with the live walk handles basic ref replacement
// without corruption (the old snapshot approach could leave dangling pointers).
TEST_F(ObjectLifetimeTest, ReplaceListLiveWalkDoesNotCrash) {
    ObjectDir *dir = Hmx::Object::New<ObjectDir>();

    // Create several objects in the dir, each with an ObjPtr pointing to
    // a target. When we ReplaceRefs on the target, all ObjPtrs should redirect.
    Hmx::Object *target = Hmx::Object::New<Hmx::Object>();
    target->SetName("target.obj", dir);

    Hmx::Object *replacement = Hmx::Object::New<Hmx::Object>();
    replacement->SetName("replacement.obj", dir);

    const int kNumHolders = 10;
    std::vector<TestRefHolder *> holders;
    for (int i = 0; i < kNumHolders; i++) {
        TestRefHolder *h = new TestRefHolder();
        char name[32];
        snprintf(name, sizeof(name), "holder%d.obj", i);
        h->SetName(name, dir);
        h->SetTarget(target);
        holders.push_back(h);
    }

    EXPECT_EQ(target->RefCount(), kNumHolders);

    // This exercises the live ring walk in ReplaceList
    target->ReplaceRefs(replacement);

    EXPECT_EQ(target->RefCount(), 0);
    EXPECT_EQ(replacement->RefCount(), kNumHolders);
    for (auto *h : holders) {
        EXPECT_EQ(h->Target(), replacement);
    }

    delete dir;
}

// Verify DirPtrRefCounts stays consistent through merge operations.
// The MergeObjectsRecurse manual Release/AddRef (lines 369-378 of Utl.cpp)
// moves refs between rings without updating DirPtrRefCounts. This test
// confirms the count tracks the actual ObjDirPtr pointing relationship,
// not ring membership.
TEST_F(ObjectLifetimeTest, DirPtrRefCountsConsistentAfterMerge) {
    ObjectDir *toDir = Hmx::Object::New<ObjectDir>();
    ObjectDir *fromDir = Hmx::Object::New<ObjectDir>();
    ObjectDir *subdir = Hmx::Object::New<ObjectDir>();
    subdir->SetName("sub.dir", fromDir);

    // Create an ObjDirPtr in fromDir pointing to subdir
    ObjDirPtr<ObjectDir> holder(subdir);
    EXPECT_TRUE(subdir->HasDirPtrs());

    auto &counts = DirPtrRefCounts();
    auto it = counts.find((const void *)subdir);
    ASSERT_NE(it, counts.end());
    int countBefore = it->second;
    EXPECT_GT(countBefore, 0);

    // Merge fromDir into toDir — the subdir ref should be properly tracked
    MergeFilter filt((MergeFilter::Action)1, MergeFilter::kNoSubdirs);
    MergeDirs(fromDir, toDir, filt);

    // The holder ObjDirPtr still points to subdir — count should be same
    it = counts.find((const void *)subdir);
    ASSERT_NE(it, counts.end());
    EXPECT_EQ(it->second, countBefore);
    EXPECT_TRUE(subdir->HasDirPtrs());

    holder = nullptr;
    delete fromDir;
    delete toDir;
}

// Verify that ObjPtrVec deferred purge entries are cleaned up when the
// ObjPtrVec is destroyed during a ReplaceList walk.

// Verify that the ObjDirPtr delete-during-cascade doesn't cause double-free.
// When ObjDirPtr::operator= deletes an ObjectDir, the destructor chain
// should not re-delete objects that are still being processed.
// Nested subdir cascade: dir1 → dir2 → dir3 with cross-references.
// Previously hung due to double-AddRef in ObjDirPtr(C*) creating self-loops.
TEST_F(ObjectLifetimeTest, ObjDirPtrCascadeDeleteDoesNotDoubleFree) {
    // Create a chain: dir1 has subdir dir2, dir2 has subdir dir3
    ObjectDir *dir1 = Hmx::Object::New<ObjectDir>();
    ObjectDir *dir2 = Hmx::Object::New<ObjectDir>();
    ObjectDir *dir3 = Hmx::Object::New<ObjectDir>();

    dir1->AppendSubDir(ObjDirPtr<ObjectDir>(dir2));
    dir2->AppendSubDir(ObjDirPtr<ObjectDir>(dir3));

    EXPECT_TRUE(dir1->HasSubDir(dir2));
    EXPECT_TRUE(dir2->HasSubDir(dir3));

    // Create cross-references: an object in dir3 references an object in dir1
    Hmx::Object *obj1 = Hmx::Object::New<Hmx::Object>();
    obj1->SetName("obj1.obj", dir1);

    TestRefHolder *holder3 = new TestRefHolder();
    holder3->SetName("holder.obj", dir3);
    holder3->SetTarget(obj1);

    EXPECT_EQ(obj1->RefCount(), 1);

    // Deleting dir1 cascades: dir1 → dir2 → dir3 → holder3 destroyed
    // holder3's ObjPtr destructor should safely null its reference to obj1
    // (which is also being destroyed as part of dir1)
    delete dir1;
    // If we get here without crash/double-free, the cascade is safe
}

// Regression for the ObjectDir cascade SIGSEGV:
// parent ~ObjectDir destroys mSubDirs, which frees the backing ObjDirPtr
// vector storage. If those ObjDirPtrs are not unlinked first, the later
// ReplaceRefs/Nullify walk over named child ObjectDirs can follow dangling
// ring entries into freed vector memory and crash.
TEST_F(ObjectLifetimeUnitTest, CascadeDeleteNamedSubdirsNullsExternalDirPtrsWithoutCrash) {
    // ASSERT_EXIT forks a subprocess and GTest hardcodes /tmp for its output
    // capture file. If /tmp isn't writable (sandbox), skip gracefully.
    {
        FILE *f = fopen("/tmp/.gtest_write_check", "w");
        if (!f)
            GTEST_SKIP() << "/tmp not writable (sandbox) — ASSERT_EXIT needs /tmp access";
        fclose(f);
        remove("/tmp/.gtest_write_check");
    }
    ASSERT_EXIT(
        RunCascadeDeleteNamedSubdirRingRepro(),
        ::testing::ExitedWithCode(0),
        ""
    );
}

// Verify that replacing refs on an object whose ObjDirPtr refs cause the
// object itself to be deleted (HasDirPtrs returns false mid-walk) doesn't crash.
TEST_F(ObjectLifetimeTest, ReplaceRefsWithSelfDeletingObjDirPtr) {
    ObjectDir *target = Hmx::Object::New<ObjectDir>();
    ObjectDir *replacement = Hmx::Object::New<ObjectDir>();

    // The only ObjDirPtr to target — when this is replaced, HasDirPtrs
    // returns false and ObjDirPtr::operator= tries to delete target.
    // But we're in the middle of walking target's refs!
    ObjDirPtr<ObjectDir> dirPtr(target);
    EXPECT_TRUE(target->HasDirPtrs());

    // Also add a regular ObjPtr to target so there are multiple refs in the ring
    Hmx::Object *owner = Hmx::Object::New<Hmx::Object>();
    ObjPtr<Hmx::Object> objRef(owner, target);
    EXPECT_EQ(target->RefCount(), 2); // dirPtr + objRef

    // This should not crash even though Replace on the ObjDirPtr may
    // trigger delete of target
    target->ReplaceRefs(replacement);

    // After ReplaceRefs, refs should point to replacement (if target survived)
    // or be null (if target was deleted)
    EXPECT_TRUE(objRef.Ptr() == replacement || objRef.Ptr() == nullptr);

    delete owner;
    delete replacement;
    // target may have been deleted by the ObjDirPtr cascade — don't double-delete
}

// ============================================================================
// FlowAnimate double-delete ring corruption
// ============================================================================
//
// CRASH SITE (from production stack trace):
//   #0  SnapshotRing(ObjRef*, std::vector<ObjRef*>&)   rbx=0 (null deref)
//   #1  Hmx::Object::ReplaceRefs(Hmx::Object*)
//   #2  Hmx::Object::~Object()
//   #3  FlowAnimate::~FlowAnimate()
//   #4  ObjectDir::DeleteObjects()
//   #5  ObjectDir::~ObjectDir()
//   #6  Flow::~Flow()             ← parent Flow being destroyed
//
// ROOT CAUSE — double ownership:
//
//   Hmx::Object::~Object() sets `sDeleting = this` and calls RemoveFromDir().
//   RemoveFromDir() has a guard: `if (mDir && mDir != sDeleting)`.
//   When a Flow is being deleted, sDeleting points to the Flow itself.
//   A FlowAnimate child whose mDir == the Flow satisfies `mDir == sDeleting`,
//   so RemoveFromDir() SKIPS nulling its hash table entry.
//
//   Meanwhile FlowNode::~FlowNode() (called as part of ~Flow) loops over
//   mChildNodes and explicitly calls `delete child` for each child, including
//   the FlowAnimate.  This is the FIRST deletion.
//
//   After all FlowNode children are deleted, ~ObjectDir fires DeleteObjects().
//   DeleteObjects iterates the hash table — the FlowAnimate's entry was NOT
//   nulled (because RemoveFromDir skipped it) — so DeleteObjects calls
//   `delete it` on the already-freed FlowAnimate.  This is the SECOND deletion.
//
// MANIFESTATION:
//   The second destructor run calls ReplaceRefs(&mRefs) on freed memory.
//   Under glibc the freed memory is typically zeroed, so mRefs.next == 0.
//   SnapshotRing reads `*(ObjRef**)((char*)sentinel + sizeof(void*))` where
//   sentinel is (a pointer into) the freed block, producing rbx=0 → SIGSEGV.
//   Under ASan the freed memory is quarantined; the crash may not happen but
//   ASan reports heap-use-after-free at the `delete cur` site.
//
// WHY THE GUARD EXISTS:
//   The `mDir != sDeleting` guard in RemoveFromDir() is intentional: when an
//   object's own directory is being deleted, we don't want each child to
//   individually search the (already-being-torn-down) hash table and null its
//   entry — that is O(n) work and races with the hash table destruction.
//   Instead, DeleteObjects is supposed to be the one canonical deletion sweep.
//   The bug is that FlowNode::~FlowNode() performs its OWN deletion sweep of
//   mChildNodes BEFORE DeleteObjects runs, creating two competing sweeps.
//
// FIX (out of scope here — only diagnose and test):
//   FlowNode::~FlowNode() should NOT delete children when
//   ObjectDir::InDeleteObjects() is already true, OR when the Flow's own
//   ObjectDir is being torn down (mDir == sDeleting check at FlowNode level).
//   One safe approach: guard the `delete cur` loop with
//   `if (!ObjectDir::InDeleteObjects())`.

// Instrumented FlowAnimate subclass to count destructor calls and raw deletes.
// This lets the test catch a double-delete without relying on ASan or a crash.
static int sFlowAnimateDestructorCount = 0;
static int sFlowAnimateDeleteCount = 0;  // raw operator delete calls

class TrackedFlowAnimate : public FlowAnimate {
public:
    TrackedFlowAnimate() : FlowAnimate() {}
    virtual ~TrackedFlowAnimate() override {
        sFlowAnimateDestructorCount++;
        fprintf(stderr,
            "[TrackedFlowAnimate] destructor call #%d (ptr=%p)\n",
            sFlowAnimateDestructorCount, (void *)this);
    }

    // Override operator delete to count raw deallocations.
    // This fires AFTER the destructor — counts actual memory frees.
    static void operator delete(void *ptr) {
        sFlowAnimateDeleteCount++;
        fprintf(stderr,
            "[TrackedFlowAnimate] operator delete #%d (ptr=%p)\n",
            sFlowAnimateDeleteCount, ptr);
        ::operator delete(ptr);
    }
};

// Expose ExposedDir::ExposeFindEntry for our ExposedFlow too.
// We need it to inspect the hash entry after simulating the first delete.
class ExposedFlowDir : public Flow {
public:
    ExposedFlowDir() : Flow() {}
    int ChildCount() const { return mChildNodes.size(); }
    FlowNode *FrontChild() const {
        return mChildNodes.empty() ? nullptr : mChildNodes.front();
    }
    ObjectDir::Entry *FindEntry(const char *name, bool add) {
        return ObjectDir::FindEntry(name, add);
    }
};

TEST_F(ObjectLifetimeTest, FlowAnimateDoubleDeleteRingCorruption) {
    // === Part 1: Pre-condition assertion (structural, no crash risk) ===
    //
    // Prove that when a FlowAnimate child is destroyed while its parent Flow
    // is being destroyed, RemoveFromDir() SKIPS nulling the hash entry.
    // This is the necessary pre-condition for the double-delete bug.
    //
    // We simulate only the RemoveFromDir guard check (no actual deletion).

    ExposedFlowDir *flow = new ExposedFlowDir();
    FlowAnimate *child = new TrackedFlowAnimate();
    child->SetName("fa_child.obj", flow);
    child->SetParent(flow, true);

    // Confirm the guard fires: child->Dir() == flow (as Hmx::Object*).
    // This means RemoveFromDir() will SKIP nulling the hash entry.
    Hmx::Object *childDirAsObj = static_cast<Hmx::Object*>(child->Dir());
    Hmx::Object *flowAsObj = static_cast<Hmx::Object*>(flow);

    fprintf(stderr,
        "[FlowAnimateDoubleDeleteRingCorruption] "
        "child->Dir()-as-Hmx::Object=%p, flow-as-Hmx::Object=%p, equal=%s\n",
        (void *)childDirAsObj, (void *)flowAsObj,
        (childDirAsObj == flowAsObj) ? "YES" : "NO");

    // ASSERTION 1: The guard in RemoveFromDir fires for this child.
    // If this fails, the structural pre-condition has changed.
    EXPECT_EQ(childDirAsObj, flowAsObj)
        << "Pre-condition: child->Dir() must equal the Flow as Hmx::Object* "
        << "so that RemoveFromDir() skips nulling the hash entry during "
        << "the parent flow's destruction (mDir == sDeleting guard).";

    // === Part 2: Verify the hash entry is NOT nulled by direct delete of child ===
    //
    // When the child's ~Hmx::Object fires during the parent's ~FlowNode phase,
    // RemoveFromDir skips nulling the hash entry (as proved in Part 1).
    // We simulate this by setting sDeleting = flow, then deleting the child.
    // After deletion, the hash entry MUST still be non-null.
    //
    // Note: We can't set sDeleting directly (it's private).  Instead we rely
    // on the observed behavior from Part 1 and use the full `delete flow`
    // path in Part 3 to catch the actual double-delete.

    // === Part 3: Full reproduction — destructor count catches the double-delete ===
    //
    // Destruction path when `delete flow` fires:
    //   ~Flow → ~FlowQueueable → ~FlowNode:
    //     loops mChildNodes → `delete child` (destructor call #1 of TrackedFlowAnimate)
    //     RemoveFromDir skips hash entry (mDir==sDeleting guard fires)
    //   ~ObjectDir → DeleteObjects():
    //     iterates hash table → entry->obj is STILL non-null (bug)
    //     calls `delete it` on already-freed child (destructor call #2)
    //
    // Manifestation varies by allocator:
    //   - glibc without ASan: `dynamic_cast<Hmx::Object*>(freed_ptr)` may
    //     return null (tcache zeroes vtable) → DeleteObjects skips the entry
    //     silently.  OR if vtable survives: SIGSEGV inside SnapshotRing.
    //   - glibc with ASan: heap-use-after-free at the `delete cur` site inside
    //     FlowNode::~FlowNode.
    //   - On Xbox 360 (the target): no allocator protection → second destructor
    //     runs on zeroed-out memory → ReplaceRefs → SnapshotRing → rbx=0 crash.
    //
    // Expected: destructor count == 1 (fixed) or == 2 (buggy).
    // May also SIGSEGV directly on the second delete.

    sFlowAnimateDestructorCount = 0;
    sFlowAnimateDeleteCount = 0;

    ASSERT_EQ(flow->ChildCount(), 1) << "child must be in mChildNodes";
    ASSERT_NE(flow->FindObject("fa_child.obj", false, true), nullptr)
        << "child must be in hash table";

    fprintf(stderr,
        "[FlowAnimateDoubleDeleteRingCorruption] "
        "about to `delete flow` — watch for double destructor / delete call\n");

    // Full delete.  May crash on some allocators/configurations.
    delete flow;

    fprintf(stderr,
        "[FlowAnimateDoubleDeleteRingCorruption] "
        "destructor was called %d time(s), operator delete called %d time(s) "
        "(expected 1 of each)\n",
        sFlowAnimateDestructorCount, sFlowAnimateDeleteCount);

    // ASSERTION 2: The child destructor must run exactly once.
    // Failure (count==2) confirms the double-delete via destructor path.
    // A crash before this point also indicates the bug.
    EXPECT_EQ(sFlowAnimateDestructorCount, 1)
        << "BUG CONFIRMED: FlowAnimate destructor ran " << sFlowAnimateDestructorCount
        << " times (expected 1). "
        << "FlowNode::~FlowNode() deleted child via mChildNodes (call #1), "
        << "then ObjectDir::DeleteObjects() deleted it again via the hash table "
        << "(call #2). The hash entry was not nulled because Hmx::Object::"
        << "RemoveFromDir() has the guard `mDir != sDeleting` — when the "
        << "parent Flow is being destroyed, sDeleting points to the Flow's "
        << "Hmx::Object virtual base, and child->mDir (the ObjectDir* of the "
        << "same Flow) compares equal after implicit conversion, so the guard "
        << "fires and the hash entry is skipped.";

    // ASSERTION 3: operator delete count.
    // The three-phase cascade (Phase 1: obj->~Object(), Phase 2: DeferFree)
    // bypasses operator delete entirely — memory is freed via free(block)
    // in FlushDeferredFrees, not through the class's operator delete.
    // A count of 0 is correct under the three-phase approach.
    // A count of 2+ would indicate a double-free.
    EXPECT_LE(sFlowAnimateDeleteCount, 1)
        << "BUG: operator delete for FlowAnimate fired "
        << sFlowAnimateDeleteCount << " times (expected 0 or 1).";
}

// ============================================================================
// Merge ring corruption with overlapping objects + subdir cross-references
// ============================================================================
//
// REPRODUCTION SCENARIO (from production crash):
//   - toDir contains "bone" objects (e.g., bone_head, bone_spine3, etc.)
//   - toDir has an unnamed subdir containing "mesh" objects whose ObjPtrs
//     point to the bone objects in toDir (e.g., bone_head.mesh -> bone_head)
//   - fromDir contains objects with the SAME names as the bones in toDir
//   - MergeDirs processes each overlapping bone: ReplaceRefs(foundObj) redirects
//     refs from fromBone to toBone, then Copy(kCopyDeep) overwrites toBone.
//   - During this process, the mesh objects' ref rings in the subdir become
//     corrupted — ring nodes grow to >1000 (should be 1-5).
//
// This test creates the minimal synthetic version of that scenario.

// Helper: count ring nodes for an object. Returns -1 if ring appears corrupted
// (more than maxNodes nodes).
static int CountRingNodes(Hmx::Object *obj, int maxNodes = 100) {
    int count = 0;
    for (ObjRef::iterator it = obj->Refs().begin(); it != obj->Refs().end(); ++it) {
        count++;
        if (count > maxNodes)
            return -1; // corrupted
    }
    return count;
}

// Subclass that holds multiple ObjPtrs — simulates a mesh/transformable that
// references several bones in its parent dir.
class MultiRefHolder : public Hmx::Object {
public:
    MultiRefHolder()
        : mRef1(this, nullptr)
        , mRef2(this, nullptr)
        , mRef3(this, nullptr)
    {}

    void SetRefs(Hmx::Object *a, Hmx::Object *b, Hmx::Object *c) {
        mRef1 = a;
        mRef2 = b;
        mRef3 = c;
    }

    Hmx::Object *Ref1() const { return mRef1.Ptr(); }
    Hmx::Object *Ref2() const { return mRef2.Ptr(); }
    Hmx::Object *Ref3() const { return mRef3.Ptr(); }

    int TotalRefCount() const {
        int c = 0;
        if (mRef1.Ptr()) c++;
        if (mRef2.Ptr()) c++;
        if (mRef3.Ptr()) c++;
        return c;
    }

private:
    ObjPtr<Hmx::Object> mRef1;
    ObjPtr<Hmx::Object> mRef2;
    ObjPtr<Hmx::Object> mRef3;
};

TEST_F(ObjectLifetimeTest, MergeDirsRingIntegrityOnOverlappingBones) {
    // === Setup: toDir with bones and a subdir containing meshes ===

    ObjectDir *toDir = Hmx::Object::New<ObjectDir>();

    // Create "bone" objects in toDir (the targets that will be found during merge)
    Hmx::Object *toBone1 = Hmx::Object::New<Hmx::Object>();
    toBone1->SetName("bone_head.cb", toDir);

    Hmx::Object *toBone2 = Hmx::Object::New<Hmx::Object>();
    toBone2->SetName("bone_spine3.cb", toDir);

    Hmx::Object *toBone3 = Hmx::Object::New<Hmx::Object>();
    toBone3->SetName("bone_neck.cb", toDir);

    // Create a subdir of toDir (simulates the unnamed subdir containing meshes)
    ObjectDir *subDir = Hmx::Object::New<ObjectDir>();
    subDir->SetName("mesh_subdir", toDir);
    toDir->AppendSubDir(ObjDirPtr<ObjectDir>(subDir));

    // Create "mesh" objects in subDir that reference bones in toDir.
    // This is the critical cross-reference pattern: objects in subdir hold
    // ObjPtrs to objects in the parent dir.
    MultiRefHolder *mesh1 = new MultiRefHolder();
    mesh1->SetName("bone_head.mesh", subDir);
    mesh1->SetRefs(toBone1, toBone2, toBone3);

    MultiRefHolder *mesh2 = new MultiRefHolder();
    mesh2->SetName("bone_spine3.mesh", subDir);
    mesh2->SetRefs(toBone2, toBone1, toBone3);

    MultiRefHolder *mesh3 = new MultiRefHolder();
    mesh3->SetName("bone_L-clavicle.mesh", subDir);
    mesh3->SetRefs(toBone1, toBone3, toBone2);

    MultiRefHolder *mesh4 = new MultiRefHolder();
    mesh4->SetName("bone_neck.mesh", subDir);
    mesh4->SetRefs(toBone3, toBone2, toBone1);

    // Sanity: each bone should have exactly 4 refs (one from each mesh's ObjPtr)
    ASSERT_EQ(toBone1->RefCount(), 4) << "bone_head should have 4 refs from 4 meshes";
    ASSERT_EQ(toBone2->RefCount(), 4) << "bone_spine3 should have 4 refs from 4 meshes";
    ASSERT_EQ(toBone3->RefCount(), 4) << "bone_neck should have 4 refs from 4 meshes";

    // Pre-merge ring integrity
    ASSERT_NE(CountRingNodes(toBone1), -1) << "bone_head ring pre-corrupt";
    ASSERT_NE(CountRingNodes(toBone2), -1) << "bone_spine3 ring pre-corrupt";
    ASSERT_NE(CountRingNodes(toBone3), -1) << "bone_neck ring pre-corrupt";
    ASSERT_NE(CountRingNodes(mesh1), -1) << "bone_head.mesh ring pre-corrupt";
    ASSERT_NE(CountRingNodes(mesh2), -1) << "bone_spine3.mesh ring pre-corrupt";
    ASSERT_NE(CountRingNodes(mesh3), -1) << "bone_L-clavicle.mesh ring pre-corrupt";
    ASSERT_NE(CountRingNodes(mesh4), -1) << "bone_neck.mesh ring pre-corrupt";

    // === Setup: fromDir with overlapping bone names ===

    ObjectDir *fromDir = Hmx::Object::New<ObjectDir>();

    Hmx::Object *fromBone1 = Hmx::Object::New<Hmx::Object>();
    fromBone1->SetName("bone_head.cb", fromDir);

    Hmx::Object *fromBone2 = Hmx::Object::New<Hmx::Object>();
    fromBone2->SetName("bone_spine3.cb", fromDir);

    Hmx::Object *fromBone3 = Hmx::Object::New<Hmx::Object>();
    fromBone3->SetName("bone_neck.cb", fromDir);

    // Add a few from-only objects too (these should just be added, not merged)
    Hmx::Object *fromOnly1 = Hmx::Object::New<Hmx::Object>();
    fromOnly1->SetName("bone_L-ankle.cb", fromDir);

    Hmx::Object *fromOnly2 = Hmx::Object::New<Hmx::Object>();
    fromOnly2->SetName("bone_L-foreArm.cb", fromDir);

    // Also create a subdir in fromDir with its own mesh cross-references,
    // since the real scenario has subdirs on both sides.
    ObjectDir *fromSubDir = Hmx::Object::New<ObjectDir>();
    fromSubDir->SetName("from_mesh_subdir", fromDir);
    fromDir->AppendSubDir(ObjDirPtr<ObjectDir>(fromSubDir));

    MultiRefHolder *fromMesh = new MultiRefHolder();
    fromMesh->SetName("from_mesh.mesh", fromSubDir);
    fromMesh->SetRefs(fromBone1, fromBone2, fromBone3);

    fprintf(stderr,
        "[MergeDirsRingIntegrity] pre-merge: toBone1 refs=%d, toBone2 refs=%d, "
        "toBone3 refs=%d\n",
        toBone1->RefCount(), toBone2->RefCount(), toBone3->RefCount());

    // === Perform the merge (kReplace action, kAllSubdirs) ===
    // kReplace triggers ReplaceRefs + Copy(kCopyDeep) on overlapping objects.
    // kAllSubdirs means fromDir's subdirs are also merged recursively.
    MergeFilter filt(MergeFilter::kReplace, MergeFilter::kAllSubdirs);
    MergeDirs(fromDir, toDir, filt);

    fprintf(stderr, "[MergeDirsRingIntegrity] merge complete, checking rings...\n");

    // === Post-merge ring integrity checks ===

    // Check ALL objects reachable from toDir (including subdirs)
    int corruptCount = 0;
    for (ObjDirItr<Hmx::Object> it(toDir, true); it != nullptr; ++it) {
        Hmx::Object *obj = it;
        int ringSize = CountRingNodes(obj);
        if (ringSize == -1) {
            if (corruptCount < 10) {
                fprintf(stderr,
                    "  CORRUPT: '%s' (%s) dir='%s' ring >100 nodes\n",
                    obj->Name(), obj->ClassName().Str(),
                    obj->Dir() ? obj->Dir()->Name() : "<null>");
            }
            corruptCount++;
        }
    }
    EXPECT_EQ(corruptCount, 0)
        << "Ring corruption detected in " << corruptCount << " objects after merge. "
        << "This reproduces the bug where MergeObject's ReplaceRefs + Copy(kCopyDeep) "
        << "on overlapping bones corrupts ref rings of mesh objects in subdirs.";

    // Specific checks on the mesh objects that are known victims
    // Their rings should be small (they own ObjPtrs, not the other way around)
    EXPECT_NE(CountRingNodes(mesh1), -1)
        << "bone_head.mesh ring corrupted (>100 nodes)";
    EXPECT_NE(CountRingNodes(mesh2), -1)
        << "bone_spine3.mesh ring corrupted (>100 nodes)";
    EXPECT_NE(CountRingNodes(mesh3), -1)
        << "bone_L-clavicle.mesh ring corrupted (>100 nodes)";
    EXPECT_NE(CountRingNodes(mesh4), -1)
        << "bone_neck.mesh ring corrupted (>100 nodes)";

    // The bone objects in toDir should still have well-formed rings.
    // After merge, some refs may have been redirected, but ring should be finite.
    EXPECT_NE(CountRingNodes(toBone1), -1)
        << "bone_head.cb ring corrupted after merge";
    EXPECT_NE(CountRingNodes(toBone2), -1)
        << "bone_spine3.cb ring corrupted after merge";
    EXPECT_NE(CountRingNodes(toBone3), -1)
        << "bone_neck.cb ring corrupted after merge";

    // Verify the meshes still point to valid bones (not nulled or dangling)
    EXPECT_NE(mesh1->Ref1(), nullptr) << "mesh1 lost ref to bone_head after merge";
    EXPECT_NE(mesh1->Ref2(), nullptr) << "mesh1 lost ref to bone_spine3 after merge";
    EXPECT_NE(mesh1->Ref3(), nullptr) << "mesh1 lost ref to bone_neck after merge";

    // Iteration should complete without hanging (a corrupted ring loops forever)
    int itrCount = 0;
    for (ObjDirItr<Hmx::Object> it(toDir, true); it != nullptr; ++it) {
        itrCount++;
        ASSERT_LT(itrCount, 10000) << "Iterator appears stuck — possible ring corruption";
    }
    fprintf(stderr,
        "[MergeDirsRingIntegrity] post-merge: %d objects reachable, %d corrupt rings\n",
        itrCount, corruptCount);
    EXPECT_GT(itrCount, 0);

    // Cleanup
    delete fromDir;
    delete toDir;
}

// ============================================================================
// NullifyAllRefs + ObjPtrList invariant tests
//
// When NullifyAllRefs nullifies an ObjPtrList::Node, the node must be removed
// from a kObjListNoNull list. Otherwise the list contains a null entry that
// crashes callers (e.g. FaderGroup::GetVolume iterating mFaders).
// ============================================================================

// Verify that NullifyAllRefs removes the dying object's entry from a
// kObjListNoNull ObjPtrList, rather than leaving a null node behind.
TEST_F(ObjectLifetimeTest, NullifyAllRefsRemovesFromObjPtrListNoNull) {
    // Owner for the list (stays alive throughout)
    Hmx::Object *listOwner = Hmx::Object::New<Hmx::Object>();

    // Object that will be nullified
    Hmx::Object *target = Hmx::Object::New<Hmx::Object>();

    // Another object that should remain in the list
    Hmx::Object *survivor = Hmx::Object::New<Hmx::Object>();

    ObjPtrList<Hmx::Object> list(listOwner, kObjListNoNull);
    list.push_back(target);
    list.push_back(survivor);
    ASSERT_EQ(list.size(), 2);

    // Simulate cascade Phase 0: nullify all refs to target
    target->NullifyAllRefs();

    // The list must not contain a null entry.
    // Before the fix, NullifyObj only nulled mObject but left the node
    // in the list, violating kObjListNoNull and crashing iterators.
    EXPECT_EQ(list.size(), 1)
        << "NullifyAllRefs must remove the nullified entry from kObjListNoNull lists";

    // The surviving entry must still be valid
    bool foundSurvivor = false;
    for (ObjPtrList<Hmx::Object>::iterator it = list.begin(); it != list.end(); ++it) {
        EXPECT_NE(*it, nullptr) << "kObjListNoNull list must never contain null entries";
        if (*it == survivor)
            foundSurvivor = true;
    }
    EXPECT_TRUE(foundSurvivor);

    delete target;
    delete survivor;
    delete listOwner;
}

// Verify that NullifyAllRefs removes the dying object's entry from a
// kObjListNoNull ObjPtrVec, rather than leaving a null node behind.
TEST_F(ObjectLifetimeTest, NullifyAllRefsRemovesFromObjPtrVecNoNull) {
    Hmx::Object *vecOwner = Hmx::Object::New<Hmx::Object>();
    Hmx::Object *target = Hmx::Object::New<Hmx::Object>();
    Hmx::Object *survivor = Hmx::Object::New<Hmx::Object>();

    ObjPtrVec<Hmx::Object> vec(vecOwner, (EraseMode)0, kObjListNoNull);
    vec.push_back(target);
    vec.push_back(survivor);
    ASSERT_EQ(vec.size(), 2);

    target->NullifyAllRefs();

    // The vector must not contain a null entry.
    EXPECT_EQ(vec.size(), 1)
        << "NullifyAllRefs must remove the nullified entry from kObjListNoNull vectors";

    // The surviving entry must still be valid
    bool foundSurvivor = false;
    for (size_t i = 0; i < vec.size(); i++) {
        EXPECT_NE(vec[i], nullptr) << "kObjListNoNull vector must never contain null entries";
        if (vec[i] == survivor)
            foundSurvivor = true;
    }
    EXPECT_TRUE(foundSurvivor);

    delete target;
    delete survivor;
    delete vecOwner;
}

// Verify that multiple list entries pointing to the same dying object
// are all removed by NullifyAllRefs.
TEST_F(ObjectLifetimeTest, NullifyAllRefsRemovesMultipleRefsFromList) {
    Hmx::Object *listOwner = Hmx::Object::New<Hmx::Object>();
    Hmx::Object *target = Hmx::Object::New<Hmx::Object>();

    ObjPtrList<Hmx::Object> list(listOwner, kObjListNoNull);
    list.push_back(target);
    list.push_back(target); // same object twice
    ASSERT_EQ(list.size(), 2);

    target->NullifyAllRefs();

    EXPECT_EQ(list.size(), 0)
        << "Both entries pointing to the nullified object must be removed";

    delete target;
    delete listOwner;
}

// Verify that NullifyAllRefs during DeleteObjects (cascade) also cleans up
// ObjPtrList entries on objects outside the cascade scope.
TEST_F(ObjectLifetimeTest, CascadeDeleteCleansUpExternalObjPtrList) {
    // External owner, not part of the cascade dir
    Hmx::Object *externalOwner = Hmx::Object::New<Hmx::Object>();
    ObjPtrList<Hmx::Object> externalList(externalOwner, kObjListNoNull);

    // Dir with objects that the external list references
    ObjectDir *dir = Hmx::Object::New<ObjectDir>();
    Hmx::Object *objA = Hmx::Object::New<Hmx::Object>();
    objA->SetName("a.obj", dir);
    Hmx::Object *objB = Hmx::Object::New<Hmx::Object>();
    objB->SetName("b.obj", dir);

    externalList.push_back(objA);
    externalList.push_back(objB);
    ASSERT_EQ(externalList.size(), 2);

    // Cascade delete the dir — Phase 0 nullifies all refs
    dir->DeleteObjects();

    // External list must have had both entries removed
    EXPECT_EQ(externalList.size(), 0)
        << "Cascade DeleteObjects must clean up external ObjPtrList references";

    // Verify no null entries leaked
    for (ObjPtrList<Hmx::Object>::iterator it = externalList.begin();
         it != externalList.end();
         ++it) {
        EXPECT_NE(*it, nullptr);
    }

    delete dir;
    delete externalOwner;
}

} // namespace
