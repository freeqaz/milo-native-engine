// DC3 Native Port - Async File Implementation
// Replaces AsyncFile_Win.cpp - uses synchronous POSIX I/O under the hood

#include "os/AsyncFile.h"
#include "os/Debug.h"
#include "os/File.h"
#include "os/System.h"
#include "utl/MemMgr.h"
#include "gfx/FrameCost.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <strings.h>  // strncasecmp
#include <sys/stat.h>

#ifdef __EMSCRIPTEN__
#include "platform/WebAssets.h"
#endif

// Native async file - actually synchronous (good enough for initial port)
class AsyncFileNative : public AsyncFile {
public:
    AsyncFileNative(const char *filename, int mode)
        : AsyncFile(filename, mode), mFp(nullptr) {}

    virtual ~AsyncFileNative() { Terminate(); }

    MEM_OVERLOAD(AsyncFile, 0x17);

protected:
    virtual void _OpenAsync() {
        const char *fmode = "rb";
        if (mMode & 1) fmode = "r+b";
        if (mMode & 0x200) fmode = "wb";
        if (mMode & 0x100) fmode = "w+b";

        // mFilename is already qualified by AsyncFile::Init()
        framecost::Scope cost(framecost::kFileRead);  // the open and size probe (web: the fetch)
        mFp = fopen(mFilename.c_str(), fmode);
        const int openErr = mFp ? 0 : errno;  // before anything else can touch errno
#ifdef __EMSCRIPTEN__
        // On-demand fetch: if file isn't in MEMFS, try fetching from server.
        // Not for the Xbox dev kit's HDD ("devkit:\\..."): games probe it for
        // optional overrides (dc3: devkit:\\dancers.dta, the locale_keep.dta
        // alternate) and a browser never has one, so the request could only
        // 404 -- a blocking round trip and two console errors each.
        const bool devkitDrive = strncasecmp(mFilename.c_str(), "devkit:", 7) == 0;
        if (!mFp && !(mMode & 0x300) && !devkitDrive) {  // read-only modes only
            if (WebAssetsFetchSync(mFilename.c_str())) {
                mFp = fopen(mFilename.c_str(), fmode);
            }
        }
#endif
        if (!mFp) {
            // As the image's AsyncFileWin::_OpenAsync: a file or directory
            // that is not there fails the open silently (it skips ReadError
            // for ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND and
            // ERROR_NOT_READY), because callers probe for optional files
            // through it -- FileExists, Locale::Init's devkit:
            // locale_keep.dta, HamPlayerData's devkit:dancers.dta,
            // DataReadFile(file, false) -- and report a missing one
            // themselves when it matters.  Any other failure (a permission
            // error, a path that is not a file) is still logged.  On web a
            // file the server does not have either is a not-found too: the
            // fetch replaced nothing, so openErr from the MEMFS open stands.
            if (openErr != ENOENT && openErr != ENOTDIR) {
                MILO_LOG("AsyncFile: failed to open '%s' (%s)\n", mFilename.c_str(),
                         strerror(openErr));
            }
            mFail = true;
            return;
        }
        // Get file size
        fseek(mFp, 0, SEEK_END);
        mSize = ftell(mFp);
        if (!(mMode & 8)) {
            fseek(mFp, 0, SEEK_SET);
        }
    }

    virtual bool _OpenDone() { return true; }

    virtual void _WriteAsync(const void *data, int size) {
        if (mFp) fwrite(data, 1, size, mFp);
    }

    virtual bool _WriteDone() { return true; }

    virtual void _SeekToTell() {
        if (mFp) fseek(mFp, mTell, SEEK_SET);
    }

    virtual void _ReadAsync(void *buf, int size) {
        if (mFp) {
            framecost::Scope cost(framecost::kFileRead);
            size_t got = fread(buf, 1, size, mFp);
            (void)got; // Base class tracks mBytesRead; we must NOT overwrite it
        }
    }

    virtual bool _ReadDone() { return true; }

    virtual void _Close() {
        if (mFp) {
            fclose(mFp);
            mFp = nullptr;
        }
    }

private:
    FILE *mFp;
};

void ReadError(const char *cc) {
    MILO_LOG("ReadError: %s\n", cc);
}

AsyncFile *AsyncFile::New(const char *filename, int mode) {
    AsyncFile *result = new AsyncFileNative(filename, mode);
    result->Init();
    return result;
}
