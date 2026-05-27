// Extract .bik files from game ark for use as test fixtures.
//
// This test boots the engine (via EngineTestFixture), enumerates .bik files
// in the archive, and extracts the smallest one to a temp directory.
//
// Usage:
//   cd native/build
//   cmake --build . --target milo-tests -j$(nproc)
//   ./milo-tests --gtest_filter=ExtractBik.*
//
// Extracted files go to /tmp/claude-1000/bik_fixtures/

#include "test_helpers.h"
#include "os/Archive.h"
#include "os/Debug.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include <sys/stat.h>

extern Archive *TheArchive;
extern bool NativeArkRead(int arkFile, long long byteOffset, void *buffer, int bytes);

struct BikFileInfo {
    std::string path;
    int arkFile;
    unsigned long long offset;
    int size;
    int ucSize;
};

static std::vector<BikFileInfo> gFoundBiks;

static int gEnumCount = 0;

static void EnumCallback(const char *dir, const char *file) {
    gEnumCount++;

    // Callback args: dir=filename, file=path (Archive::Enumerate convention)
    const char *name = dir;
    const char *path = file;

    // Check if it's a .bik file
    const char *ext = strrchr(name, '.');
    if (!ext) return;
    if (strcasecmp(ext, ".bik") != 0) return;

    std::string fullPath;
    if (path && path[0]) {
        fullPath = std::string(path) + "/" + name;
    } else {
        fullPath = name;
    }

    int arkFile = 0;
    unsigned long long byteOffset = 0;
    int fileSize = 0, ucSize = 0;

    if (TheArchive->GetFileInfo(fullPath.c_str(), arkFile, byteOffset, fileSize, ucSize)) {
        BikFileInfo info;
        info.path = fullPath;
        info.arkFile = arkFile;
        info.offset = byteOffset;
        info.size = fileSize;
        info.ucSize = ucSize;
        gFoundBiks.push_back(info);
    }
}

// Enumerate .bik files from all known archive directories.
// Archive::Enumerate("", ..., true, ...) only matches entries whose path is
// the empty string (root-level files).  Real .bik files live under "videos/"
// and "songs/<name>/", so we enumerate those trees explicitly.
static void EnumerateAllBiks() {
    // Top-level directories known to contain .bik files in DC3
    static const char *kBikDirs[] = {
        "videos",
        "songs",
        nullptr
    };
    for (const char **d = kBikDirs; *d; d++) {
        TheArchive->Enumerate(*d, EnumCallback, true, nullptr);
    }
}

class ExtractBik : public EngineTestFixture {};

TEST_F(ExtractBik, ListBikFiles) {
    ASSERT_NE(TheArchive, nullptr) << "Archive not initialized";

    gFoundBiks.clear();
    gEnumCount = 0;
    EnumerateAllBiks();

    printf("\n=== .bik files in archive ===\n");
    printf("Total files enumerated: %d\n", gEnumCount);
    printf("Found %zu .bik files\n\n", gFoundBiks.size());

    // Sort by size
    std::sort(gFoundBiks.begin(), gFoundBiks.end(),
              [](const BikFileInfo &a, const BikFileInfo &b) { return a.size < b.size; });

    for (size_t i = 0; i < gFoundBiks.size(); i++) {
        auto &f = gFoundBiks[i];
        printf("  [%3zu] %7d bytes  ark=%d  offset=0x%llx  %s\n",
               i, f.size, f.arkFile, f.offset, f.path.c_str());
    }
}

TEST_F(ExtractBik, ExtractSmallest) {
    ASSERT_NE(TheArchive, nullptr) << "Archive not initialized";

    gFoundBiks.clear();
    gEnumCount = 0;
    EnumerateAllBiks();
    if (gFoundBiks.empty()) {
        GTEST_SKIP() << "No .bik files found in archive (DC3 may not have bik files under videos/ or songs/)";
    }

    // Sort by size, extract smallest
    std::sort(gFoundBiks.begin(), gFoundBiks.end(),
              [](const BikFileInfo &a, const BikFileInfo &b) { return a.size < b.size; });

    // Create output directory
    const char *outDir = "/tmp/claude-1000/bik_fixtures";
    mkdir(outDir, 0755);

    // Extract up to 3 smallest .bik files + 1 song preview (has audio)
    int extracted = 0;
    bool gotPreview = false;
    for (size_t i = 0; i < gFoundBiks.size() && (extracted < 3 || !gotPreview); i++) {
        auto &f = gFoundBiks[i];

        // Skip very tiny files (< 1KB, likely corrupt/empty)
        if (f.size < 1024) continue;

        // After first 3, only extract song previews (have audio)
        bool isPreview = f.path.find("_prev.bik") != std::string::npos;
        if (extracted >= 3 && !isPreview) continue;
        if (extracted >= 3 && gotPreview) continue;

        // Build output filename
        const char *basename = strrchr(f.path.c_str(), '/');
        basename = basename ? basename + 1 : f.path.c_str();

        char outPath[512];
        snprintf(outPath, sizeof(outPath), "%s/%s", outDir, basename);

        printf("\nExtracting: %s (%d bytes) → %s\n", f.path.c_str(), f.size, outPath);

        // Read from ark
        std::vector<uint8_t> buf(f.size);
        bool ok = NativeArkRead(f.arkFile, f.offset, buf.data(), f.size);
        ASSERT_TRUE(ok) << "Failed to read from ark";

        // Verify Bink signature (first 3 bytes should be "BIK" or "KB2")
        if (buf.size() >= 4) {
            printf("  Header: %02x %02x %02x %02x ('%c%c%c%c')\n",
                   buf[0], buf[1], buf[2], buf[3],
                   (buf[0] >= 32 ? buf[0] : '.'), (buf[1] >= 32 ? buf[1] : '.'),
                   (buf[2] >= 32 ? buf[2] : '.'), (buf[3] >= 32 ? buf[3] : '.'));
        }

        // Write to disk
        FILE *fp = fopen(outPath, "wb");
        ASSERT_NE(fp, nullptr) << "Failed to create " << outPath;
        size_t written = fwrite(buf.data(), 1, buf.size(), fp);
        fclose(fp);
        ASSERT_EQ((int)written, f.size);

        printf("  Written %d bytes to %s\n", f.size, outPath);
        extracted++;
        if (isPreview) gotPreview = true;
    }

    EXPECT_GT(extracted, 0) << "No .bik files extracted";
    printf("\n=== Extracted %d .bik files to %s ===\n", extracted, outDir);
    printf("Run FFmpeg tests with:\n");
    printf("  MILO_TEST_BIK=%s/<file>.bik ./milo-tests '--gtest_filter=BinkFFmpeg.*'\n", outDir);
}
