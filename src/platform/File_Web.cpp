// DC3 Web Port — File I/O Implementation
// Replaces File_Native.cpp — MEMFS-backed file operations.
// Files are fetched from the server HTTP API into Emscripten's MEMFS
// under /data/, then opened via standard POSIX I/O.

#ifdef __EMSCRIPTEN__

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "os/Archive.h"
#include "os/Debug.h"
#include "os/File.h"
#include "os/System.h"

// Data directory in MEMFS — set by WebAssetsInit / main_web.cpp
static char gNativeDataDir[512] = "/data";

void NativeSetDataDir(const char *dir) {
    strncpy(gNativeDataDir, dir, sizeof(gNativeDataDir) - 1);
    gNativeDataDir[sizeof(gNativeDataDir) - 1] = '\0';
}

const char *NativeGetDataDir() { return gNativeDataDir; }

// On web, all files are "local" (in MEMFS) — no archive routing
bool FileIsLocal(const char *file) {
    return true;
}

int FileGetStat(const char *iFilename, FileStat *iBuffer) {
    String fullName;
    FileQualifiedFilename(fullName, iFilename);
    struct stat st;
    if (stat(fullName.c_str(), &st) != 0) return -1;
    iBuffer->st_mode = st.st_mode;
    iBuffer->st_size = st.st_size;
    // MEMFS timestamps are not meaningful — zero them out
    iBuffer->st_ctime = 0;
    iBuffer->st_atime = 0;
    iBuffer->st_mtime = 0;
    return 0;
}

int FileDelete(const char *iFilename) {
    String str;
    FileQualifiedFilename(str, iFilename);
    return unlink(str.c_str()) == 0 ? 0 : -1;
}

int FileMkDir(const char *iDirname) {
    String str;
    FileQualifiedFilename(str, iDirname);
    return mkdir(str.c_str(), 0755) == 0 ? 1 : 0;
}

void FileQualifiedFilename(char *out, int, const char *in) {
    MILO_ASSERT(in && out, 0x121);
    String str(in);
    const char *inStr = str.c_str();
    char buf[256];
    const char *path = FileMakePathBuf(gNativeDataDir, inStr, buf);
    strcpy(out, path);
}

void FileEnumerate(
    const char *dir,
    void (*cb)(const char *, const char *),
    bool recurse,
    const char *pattern,
    bool b2
) {
    // MEMFS supports opendir/readdir
    if (UsingCD() && TheArchive) {
        TheArchive->Enumerate(dir, cb, recurse, pattern);
        return;
    }

    char qualified[256];
    FileQualifiedFilename(qualified, 0x100, dir);

    DIR *d = opendir(qualified);
    if (!d) return;

    struct dirent *entry;
    while ((entry = readdir(d)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char buf[512];
        snprintf(buf, sizeof(buf), "%s/%s", qualified, entry->d_name);

        struct stat st;
        if (stat(buf, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (b2 && (!pattern || FileMatch(buf, pattern))) {
                cb(qualified, entry->d_name);
            }
            if (recurse) {
                FileEnumerate(buf, cb, recurse, pattern, b2);
            }
        } else {
            if (!b2 && (!pattern || FileMatch(buf, pattern))) {
                cb(qualified, entry->d_name);
            }
        }
    }
    closedir(d);
}

#endif // __EMSCRIPTEN__
