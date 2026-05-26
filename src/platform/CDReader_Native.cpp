// DC3 Native Port - CDReader Implementation
// Replaces CDReader.cpp - uses POSIX fopen/fread for .ark file access

#include "os/CDReader.h"
#include "os/Archive.h"
#include "os/Debug.h"
#include "os/File.h"
#include "os/System.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
    std::vector<FILE *> gArkFiles;
    bool gReadDone = true;

    bool ArkFilesInit() {
        if (!gArkFiles.empty()) return true;

        int numArks = TheArchive->NumArkFiles();
        gArkFiles.resize(numArks, nullptr);

        for (int i = 0; i < numArks; i++) {
            const char *arkFileName = TheArchive->GetArkfileName(i);
            String fullPath;
            FileQualifiedFilename(fullPath, arkFileName);
            gArkFiles[i] = fopen(fullPath.c_str(), "rb");
            if (!gArkFiles[i]) {
                MILO_LOG("CDReader_Native: failed to open %s\n", fullPath.c_str());
                return false;
            }
        }
        return true;
    }
}

bool CDReadDone() {
    return gReadDone;
}

int CDRead(int arkFile, int offset, int size, void *buffer) {
    printf("DC3 Native: CDRead(ark=%d, offset=%d, size=%d) [bytes: offset=%lld size=%d]\n",
           arkFile, offset, size, (long long)offset << 11, size << 11);
    if (!UsingCD()) return 1;
    if (!ArkFilesInit()) return 1;

    if (arkFile < 0 || arkFile >= (int)gArkFiles.size() || !gArkFiles[arkFile]) {
        MILO_LOG("CDRead: invalid ark file %d\n", arkFile);
        return 1;
    }

    // offset and size are in 2048-byte blocks
    long long byteOffset = (long long)offset << 11;
    int byteSize = size << 11;

    FILE *fp = gArkFiles[arkFile];
    if (fseeko(fp, byteOffset, SEEK_SET) != 0) {
        MILO_LOG("CDRead: seek failed at offset %lld\n", byteOffset);
        return 1;
    }

    size_t bytesRead = fread(buffer, 1, byteSize, fp);
    if ((int)bytesRead != byteSize) {
        MILO_LOG("CDRead: short read %zu/%d at offset %lld\n",
                 bytesRead, byteSize, byteOffset);
        // Not necessarily an error - could be end of file
    }

    gReadDone = true;
    return 0;
}

// Direct byte-level read for native ArkFile (bypasses BlockMgr)
bool NativeArkRead(int arkFile, long long byteOffset, void *buffer, int bytes) {
    if (!ArkFilesInit()) return false;
    if (arkFile < 0 || arkFile >= (int)gArkFiles.size() || !gArkFiles[arkFile])
        return false;
    FILE *fp = gArkFiles[arkFile];
    if (fseeko(fp, byteOffset, SEEK_SET) != 0)
        return false;
    size_t got = fread(buffer, 1, bytes, fp);
    return (int)got == bytes;
}

bool CDReadExternal(void *&v, int arkFile, u64 byteOffset) {
    if (!ArkFilesInit()) return false;
    if (arkFile < 0 || arkFile >= (int)gArkFiles.size() || !gArkFiles[arkFile])
        return false;

    // Return the FILE* as the "handle" and seek to the requested offset
    FILE *fp = gArkFiles[arkFile];
    fseeko(fp, byteOffset, SEEK_SET);
    v = fp;
    return true;
}
