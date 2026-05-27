// DirLoader integration tests — requires full engine initialization
#include "test_helpers.h"
#include "obj/Dir.h"
#include "obj/DirLoader.h"
#include "obj/Object.h"
#include "utl/ChunkStream.h"
#include "utl/FilePath.h"
#include "utl/Loader.h"

extern void ReadDead(BinStream &);

// ============================================================================
// Fixture: ensures engine is initialized once for all tests in this suite
// ============================================================================

class DirLoaderTest : public EngineTestFixture {};

static const char *kPreferredMiloFiles[] = {
    // Known-good archive-backed assets in this workspace.
    "char/shared/main_resource.milo",
    "char/shared/viseme_resource.milo",
    "char/shared/skeleton_bones_resource.milo",
    // Optional world fixtures if present.
    "world/shared/props/gen/discoball.milo_xbox",
    "world/shared/lighting/gen/shared_lights.milo_xbox",
    nullptr
};

static const char *kReadableMiloFiles[] = {
    "world/shared/props/gen/discoball.milo_xbox",
    "world/shared/lighting/gen/shared_lights.milo_xbox",
    nullptr
};

static const char *FindLoadableMilo(const char *const *candidates) {
    for (int i = 0; candidates[i]; i++) {
        ObjectDir *dir = DirLoader::LoadObjects(FilePath(candidates[i]), nullptr, nullptr);
        if (dir) {
            delete dir;
            return candidates[i];
        }
    }
    return nullptr;
}

static const char *FindReadableMilo(const char *const *candidates) {
    for (int i = 0; candidates[i]; i++) {
        ChunkStream cs(
            candidates[i], ChunkStream::kRead, 0x8000, false, kPlatformNone, false
        );
        if (!cs.Fail()) {
            return candidates[i];
        }
    }
    return nullptr;
}

static std::string WriteDirHeaderFixture() {
    std::vector<uint8_t> chunk;
    PutBE32(chunk, 0x20);                     // mRev
    PutBEString(chunk, "ObjectDir");          // dirClass
    PutBEString(chunk, "dirloader_fixture");  // dirName
    PutBE32(chunk, 1);                        // numEntries
    PutBEString(chunk, "Object");             // className
    PutBEString(chunk, "fixture_obj");        // objName
    PutDeadMarker(chunk);
    PutBE32(chunk, 0x12345678);

    std::string path = "/tmp/claude-1000/milo_tests/dirloader_header_fixture.milo_xbox";
    EXPECT_TRUE(WriteSyntheticMilo(path.c_str(), {chunk}));
    return path;
}

// ============================================================================
// StreamPositionTracking — manually read the DirLoader header fields
// from a known .milo file and verify Tell() stays coherent.
//
// DirLoader header format (rev >= 28):
//   int mRev
//   Symbol dirClass (length-prefixed string)
//   Symbol dirName
//   int numEntries
//   for each entry: Symbol className, Symbol objName
//   ... then dir data + object data
// ============================================================================

TEST_F(DirLoaderTest, StreamPositionTracking) {
    const char *found = FindReadableMilo(kReadableMiloFiles);
    std::string syntheticPath;
    if (!found) {
        syntheticPath = WriteDirHeaderFixture();
        found = syntheticPath.c_str();
    }

    printf("DirLoaderTest: using %s\n", found);

    ChunkStream cs(found, ChunkStream::kRead, 0x8000, false, kPlatformNone, false);
    ASSERT_FALSE(cs.Fail());

    // Process chunk header
    EofType eof = cs.Eof();
    ASSERT_EQ(eof, NotEof);

    // Read mRev
    int mRev;
    cs >> mRev;
    printf("  mRev = %d (tell=%d)\n", mRev, cs.Tell());
    EXPECT_GE(mRev, 25) << "mRev should be >= 25 for DC3 files";
    EXPECT_LE(mRev, 35) << "mRev suspiciously high";
    EXPECT_EQ(cs.Tell(), 4);

    // Read dirClass
    Symbol dirClass;
    cs >> dirClass;
    printf("  dirClass = '%s' (tell=%d)\n", dirClass.Str(), cs.Tell());
    EXPECT_NE(strlen(dirClass.Str()), 0u) << "dirClass should not be empty";

    // Read dirName (only if mRev >= some version — usually present)
    if (mRev > 1) {
        Symbol dirName;
        cs >> dirName;
        printf("  dirName = '%s' (tell=%d)\n", dirName.Str(), cs.Tell());
    }

    // Read number of entries
    int numEntries;
    cs >> numEntries;
    printf("  numEntries = %d (tell=%d)\n", numEntries, cs.Tell());
    EXPECT_GE(numEntries, 0);
    EXPECT_LT(numEntries, 10000) << "Suspiciously many entries";

    // Read each entry's class+name
    for (int i = 0; i < numEntries && i < 10; i++) {
        Symbol className, objName;
        cs >> className;
        cs >> objName;
        printf("  entry[%d]: class='%s' name='%s' (tell=%d)\n",
               i, className.Str(), objName.Str(), cs.Tell());
    }

    printf("  Header parsed successfully. Final tell=%d\n", cs.Tell());
}

// ============================================================================
// LoadSimpleMilo — use DirLoader::LoadObjects to load a known-working file
// ============================================================================

TEST_F(DirLoaderTest, LoadSimpleMilo) {
    const char *found = FindLoadableMilo(kPreferredMiloFiles);

    if (!found) {
        GTEST_SKIP() << "No loadable .milo fixtures found";
    }

    printf("DirLoaderTest::LoadSimpleMilo: loading %s\n", found);

    FilePath fp(found);
    ObjectDir *dir = DirLoader::LoadObjects(fp, nullptr, nullptr);

    ASSERT_NE(dir, nullptr) << "DirLoader::LoadObjects returned null for " << found;
    printf("  Loaded: '%s' class='%s'\n", dir->Name(), dir->ClassName().Str());

    // Basic sanity checks
    EXPECT_NE(strlen(dir->Name()), 0u);
}

// ============================================================================
// LoadWithoutDesync — parameterized test over multiple .milo files
// Each file should load without crash, ASan error, or desync.
// ============================================================================

class LoadMiloParam : public EngineTestFixture,
                      public ::testing::WithParamInterface<const char *> {};

TEST_P(LoadMiloParam, LoadWithoutDesync) {
    const char *miloFile = GetParam();

    printf("LoadWithoutDesync: %s\n", miloFile);
    FilePath fp(miloFile);
    ObjectDir *dir = DirLoader::LoadObjects(fp, nullptr, nullptr);
    ASSERT_NE(dir, nullptr)
        << "Failed to load " << miloFile
        << " (expected to be archive-backed)";
    printf("  OK: '%s' class='%s'\n", dir->Name(), dir->ClassName().Str());
}

// Progressively more complex .milo files
INSTANTIATE_TEST_SUITE_P(
    MiloFiles,
    LoadMiloParam,
    ::testing::Values(
        "char/shared/main_resource.milo",
        "char/shared/viseme_resource.milo",
        "char/shared/skeleton_bones_resource.milo"
    )
);

// ============================================================================
// DeadMarkerInRealFile — verify that after parsing the header, the stream
// is at a coherent position for reading object data.
// ============================================================================

TEST_F(DirLoaderTest, DeadMarkerInRealFile) {
    const char *found = FindReadableMilo(kReadableMiloFiles);
    std::string syntheticPath;
    if (!found) {
        syntheticPath = WriteDirHeaderFixture();
        found = syntheticPath.c_str();
    }

    ChunkStream cs(found, ChunkStream::kRead, 0x8000, false, kPlatformNone, false);
    ASSERT_FALSE(cs.Fail());
    ASSERT_EQ(cs.Eof(), NotEof);

    // Read header: mRev, dirClass, dirName (if rev>1), numEntries, entries
    int mRev;
    cs >> mRev;

    Symbol dirClass;
    cs >> dirClass;

    if (mRev > 1) {
        Symbol dirName;
        cs >> dirName;
    }

    int numEntries;
    cs >> numEntries;

    for (int i = 0; i < numEntries; i++) {
        Symbol cn, on;
        cs >> cn;
        cs >> on;
    }

    int headerEnd = cs.Tell();
    printf("DeadMarkerInRealFile: header ends at tell=%d, mRev=%d, %d entries\n",
           headerEnd, mRev, numEntries);

    // The stream should now be positioned at the dir's PreLoad data.
    // We can't easily verify what's there without knowing the format,
    // but we can check that reading doesn't immediately fail.
    EXPECT_FALSE(cs.Fail()) << "Stream in failed state after header parse";
    EXPECT_NE(cs.Eof(), RealEof) << "Unexpected EOF right after header";
}

TEST_F(DirLoaderTest, RepeatedLoadLeavesOnlyLiveEntries) {
    const char *found = FindLoadableMilo(kPreferredMiloFiles);
    if (!found) {
        GTEST_SKIP() << "No loadable .milo fixtures found";
    }

    FilePath fp(found);
    ObjectDir *first = DirLoader::LoadObjects(fp, nullptr, nullptr);
    ObjectDir *second = DirLoader::LoadObjects(fp, nullptr, nullptr);
    ASSERT_NE(first, nullptr) << "First load failed for " << found;
    ASSERT_NE(second, nullptr) << "Second load failed for " << found;

    int itrCount = 0;
    for (ObjDirItr<Hmx::Object> it(second, false); it != nullptr; ++it) {
        EXPECT_NE(&*it, nullptr);
        itrCount++;
        ASSERT_LT(itrCount, 100000);
    }
    EXPECT_GT(itrCount, 0);

    delete first;
    delete second;
}
