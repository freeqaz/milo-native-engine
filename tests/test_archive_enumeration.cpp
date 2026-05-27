#include "test_helpers.h"

#include "os/Archive.h"

#include <string>
#include <vector>

extern Archive *TheArchive;

namespace {
std::vector<std::string> gEnumeratedDtbs;
std::vector<std::string> gEnumeratedDtas;

void CollectDtbPath(const char *name, const char *path) {
    std::string fullPath;
    if (path && path[0]) {
        fullPath = std::string(path) + "/" + name;
    } else {
        fullPath = name;
    }
    gEnumeratedDtbs.push_back(fullPath);
}

void CollectDtaPath(const char *path, const char *name) {
    std::string fullPath;
    if (path && path[0]) {
        fullPath = std::string(path) + "/" + name;
    } else {
        fullPath = name;
    }
    gEnumeratedDtas.push_back(fullPath);
}
}

class ArchiveEnumerationTest : public EngineTestFixture {};

TEST_F(ArchiveEnumerationTest, EnumerateSongDtbFilesFromArchive) {
    ASSERT_NE(TheArchive, nullptr) << "Archive not initialized";

    gEnumeratedDtbs.clear();
    TheArchive->Enumerate("songs/gen", CollectDtbPath, false, "songs/gen/songs*.dtb");

    EXPECT_FALSE(gEnumeratedDtbs.empty())
        << "Archive::Enumerate failed to find songs/gen/songs*.dtb";
}

TEST_F(ArchiveEnumerationTest, EnumerateSongDtaFilesViaDtaRewrite) {
    ASSERT_NE(TheArchive, nullptr) << "Archive not initialized";

    gEnumeratedDtas.clear();
    TheArchive->Enumerate("songs", CollectDtaPath, false, "songs/songs*.dta");

    EXPECT_FALSE(gEnumeratedDtas.empty())
        << "Archive::Enumerate failed to translate songs/songs*.dta into ark-backed DTB entries";
}
