// Web Asset Fetcher Implementation
// Uses emscripten_fetch() to download files from the dev server's HTTP API
// into Emscripten's in-memory filesystem (MEMFS) under /data/.
// All fetches are async — poll WebAssetsAllDone() from the main loop.

#ifdef __EMSCRIPTEN__

#include "platform/WebAssets.h"
#include "platform/FrameTraceCounters.h"

#include <emscripten/fetch.h>
#include <emscripten/em_asm.h>
#include <emscripten/em_js.h>  // EM_ASYNC_JS (Q5 JSPI-suspending fetch)
#include <cstdio>
#include <cstdlib>             // getenv (RB3_SYNC_XHR_LEGACY)
#include <cstring>
#include <cerrno>
#include <sys/stat.h>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

struct FetchRequest {
    int id;
    std::string serverPath;  // e.g. "config/ham_keep.dta"
    std::string memfsPath;   // e.g. "/data/config/ham_keep.dta"
    bool done;
    bool success;
};

static int sNextFetchId = 1;
static int sPending = 0;
static int sCompleted = 0;
static int sFailed = 0;
static std::vector<FetchRequest *> sFetchRequests;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Recursively create directory path in MEMFS
static void mkdirRecursive(const char *path) {
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);  // Ignore EEXIST
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

// ---------------------------------------------------------------------------
// Fetch callbacks
// ---------------------------------------------------------------------------

static void onFetchSuccess(emscripten_fetch_t *fetch) {
    FetchRequest *req = static_cast<FetchRequest *>(fetch->userData);

    // Create parent directories
    std::string dir = req->memfsPath;
    size_t slash = dir.rfind('/');
    if (slash != std::string::npos) {
        dir.resize(slash);
        mkdirRecursive(dir.c_str());
    }

    // Write fetched data to MEMFS
    FILE *f = fopen(req->memfsPath.c_str(), "wb");
    if (f) {
        fwrite(fetch->data, 1, fetch->numBytes, f);
        fclose(f);
        printf("WebAssets: %s (%llu bytes)\n",
               req->serverPath.c_str(), (unsigned long long)fetch->numBytes);
        req->success = true;
        sCompleted++;
    } else {
        printf("WebAssets: MEMFS write failed %s (errno %d)\n",
               req->memfsPath.c_str(), errno);
        req->success = false;
        sFailed++;
    }

    req->done = true;
    sPending--;
    emscripten_fetch_close(fetch);
}

static void onFetchError(emscripten_fetch_t *fetch) {
    FetchRequest *req = static_cast<FetchRequest *>(fetch->userData);
    printf("WebAssets: FAILED %s (HTTP %d)\n", fetch->url, fetch->status);
    req->done = true;
    req->success = false;
    sPending--;
    sFailed++;
    emscripten_fetch_close(fetch);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void WebAssetsInit() {
    mkdir("/data", 0755);
    mkdir("/data/config", 0755);
    mkdir("/data/ui", 0755);
    mkdir("/data/world", 0755);
    mkdir("/data/char", 0755);
    mkdir("/data/songs", 0755);
    mkdir("/data/gen", 0755);
    mkdir("/data/videos", 0755);
    printf("WebAssets: MEMFS initialized\n");
}

int WebAssetsFetch(const char *serverPath) {
    FetchRequest *req = new FetchRequest();
    req->id = sNextFetchId++;
    req->serverPath = serverPath;
    req->memfsPath = std::string("/data/") + serverPath;
    req->done = false;
    req->success = false;
    sFetchRequests.push_back(req);

    // Build server URL
    char url[512];
    snprintf(url, sizeof(url), "/api/file/%s", serverPath);

    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    strcpy(attr.requestMethod, "GET");
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
    attr.onsuccess = onFetchSuccess;
    attr.onerror = onFetchError;
    attr.userData = req;

    emscripten_fetch(&attr, url);
    sPending++;

    return req->id;
}

bool WebAssetsFetchDone(int fetchId) {
    for (const auto *req : sFetchRequests) {
        if (req->id == fetchId) return req->done;
    }
    return true;  // Unknown ID treated as done
}

// ---------------------------------------------------------------------------
// Bundle download — single HTTP request for ALL assets
// ---------------------------------------------------------------------------

// W4b IDB write-back (R3). The synchronous on-demand fetch path
// (native_file.cpp cachePutAfterFetch → window.__rb3CachePut) writes each
// fetched asset through to the IndexedDB warm cache, so a 2nd boot serves it
// from IDB with zero network. The bundle path historically wrote straight to
// MEMFS and bypassed IDB — so once R3 routes the boot .milo_xbox set through
// the bundle, warm/repeat boots would re-download the whole bundle, regressing
// the W4b warm-cache win. Fix: as each bundle file is unpacked, ALSO persist it
// to IDB using the SAME cache key the sync path derives
// (cacheRelFromMemfsPath: strip "/data/", "/../", or a leading "/" off the
// resolved memfs path). The bytes are already in hand from the bundle buffer,
// so we hand them straight to __rb3CachePut (no FS.readFile).
//
// This is RB3-specific (DC3 may not define window.__rb3CachePut), so it is a
// no-op guarded on the hook's presence — safe in the shared engine for any
// caller of WebAssetsFetchBundle().
static void bundleCacheWriteThrough(const char *memfsPath, const void *bytes,
                                    size_t numBytes) {
    // Mirror native_file.cpp cacheRelFromMemfsPath() EXACTLY — the key must match
    // what cacheTryHit() looks up on a warm boot, or the bundle populates a cache
    // it never reads.
    const char *key = memfsPath;
    if (strncmp(key, "/data/", 6) == 0)
        key += 6;
    else if (strncmp(key, "/../", 4) == 0)
        key += 4;
    else if (key[0] == '/')
        key += 1;

    EM_ASM(
        {
            try {
                if (!window.__rb3CachePut) return;  // DC3 / no IDB shim → no-op
                var key = UTF8ToString($0);
                // Wrap the WASM heap slice WITHOUT copying; __rb3CachePut takes
                // its own copy before the heap can move (see rb3_pre.js).
                var bytes = HEAPU8.subarray($1, $1 + $2);
                window.__rb3CachePut(key, bytes);
            } catch (e) {
                console.log('[rb3-idb] bundle cache-put failed: ' + e);
            }
        },
        key, bytes, (int)numBytes);
}

static void onBundleSuccess(emscripten_fetch_t *fetch) {
    printf("WebAssets: bundle received (%llu bytes), unpacking...\n",
           (unsigned long long)fetch->numBytes);

    const uint8_t *ptr = (const uint8_t *)fetch->data;
    const uint8_t *end = ptr + fetch->numBytes;

    if (end - ptr < 4) {
        printf("WebAssets: bundle too small\n");
        sPending--;
        sFailed++;
        emscripten_fetch_close(fetch);
        return;
    }

    // Read file count (little-endian uint32)
    uint32_t count = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
    ptr += 4;

    int unpacked = 0;
    for (uint32_t i = 0; i < count && ptr + 4 <= end; i++) {
        // Read path
        uint32_t pathLen = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
        ptr += 4;
        if (ptr + pathLen > end) break;
        std::string relPath((const char *)ptr, pathLen);
        ptr += pathLen;

        // Read data
        if (ptr + 4 > end) break;
        uint32_t dataLen = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
        ptr += 4;
        if (ptr + dataLen > end) break;
        const uint8_t *data = ptr;
        ptr += dataLen;

        // Write to MEMFS — resolve ".." in path
        std::string memfsPath = std::string("/data/") + relPath;

        // Resolve ".." components to get a clean absolute path
        // e.g. "/data/../../system/run/config/macros.dta" → "/system/run/config/macros.dta"
        {
            std::vector<std::string> parts;
            size_t pos = 0;
            while (pos < memfsPath.size()) {
                size_t next = memfsPath.find('/', pos + 1);
                if (next == std::string::npos) next = memfsPath.size();
                std::string part = memfsPath.substr(pos, next - pos);
                if (part == "/..") {
                    if (!parts.empty()) parts.pop_back();
                } else if (part != "/.") {
                    parts.push_back(part);
                }
                pos = next;
            }
            memfsPath.clear();
            for (const auto &p : parts) memfsPath += p;
            if (memfsPath.empty()) memfsPath = "/";
        }

        // Create parent directories
        std::string dir = memfsPath;
        size_t slash = dir.rfind('/');
        if (slash != std::string::npos) {
            dir.resize(slash);
            mkdirRecursive(dir.c_str());
        }

        FILE *f = fopen(memfsPath.c_str(), "wb");
        if (f) {
            fwrite(data, 1, dataLen, f);
            fclose(f);
            unpacked++;
            // W4b (R3): write this file through to the IndexedDB warm cache so
            // repeat boots serve it from IDB instead of re-downloading the
            // bundle. No-op where window.__rb3CachePut is absent (DC3).
            bundleCacheWriteThrough(memfsPath.c_str(), data, dataLen);
        }
    }

    printf("WebAssets: unpacked %d/%u files into MEMFS\n", unpacked, count);
    sCompleted += unpacked;
    sPending--;
    emscripten_fetch_close(fetch);
}

static void onBundleError(emscripten_fetch_t *fetch) {
    printf("WebAssets: bundle download FAILED (HTTP %d)\n", fetch->status);
    sPending--;
    sFailed++;
    emscripten_fetch_close(fetch);
}

void WebAssetsFetchBundle(const char *url) {
    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    strcpy(attr.requestMethod, "GET");
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
    attr.onsuccess = onBundleSuccess;
    attr.onerror = onBundleError;

    emscripten_fetch(&attr, url ? url : "/api/bundle");
    sPending++;
}

// ---------------------------------------------------------------------------
// Blocking single-file fetch — preserves a SYNCHRONOUS C contract (bytes are
// resident at memfsPath on return) for the matched-fork's File ctor, which
// expects to fopen() the file immediately afterward.
//
// Two backends, both writing to the SAME MEMFS path with the SAME keys:
//   1. (default) webAssetsAsyncFetchToMemfs — a JSPI-suspending `await fetch()`
//      + arrayBuffer() copy. The wasm stack suspends across the network round
//      trip and the browser keeps compositing/handling input, so the canvas no
//      longer freezes for the whole fetch. Decoding of any Content-Encoding
//      (br/gz the server adds, server.py:221) is transparent — fetch() decodes
//      it before arrayBuffer(), exactly as the old XHR.responseText did, so the
//      bytes written to MEMFS are identical to the legacy path's.
//   2. (RB3_SYNC_XHR_LEGACY=1) webAssetsLegacyXhrToMemfs — the original
//      synchronous XHR + per-byte charCodeAt string→Uint8Array convert loop.
//      Kept compiled for one release as a fallback in case a caller turns out
//      to reach this from a non-JSPI-suspendable frame (none known — see the
//      caller audit below).
//
// Caller audit (all callers run under the JSPI-suspendable `_rb3MainLoopTick`
// export, which already JSPI-suspends today via Loader.cpp's emscripten_sleep):
//   - native_file.cpp NativeStdioFile ctor   → NewFile under RunOneFrame/loaders
//   - rb3_xma_sidecar.h TryLoad              → SampleInst start, main-thread
//                                              synth poll under RunOneFrame
//   - AsyncFile_Native.cpp _OpenAsync        → NOT compiled for rb3 (excluded in
//                                              MILO_ENGINE_DECOMP_PLATFORM_EXCLUDE);
//                                              DC3-only, also JSPI-driven.
// None run during static init / before runtime init (every caller's prelude
// runs JS via EM_ASM, which requires the runtime up) or inside the audio
// worklet (sample START is on the main thread; the worklet only pulls already-
// decoded PCM). A JSPI suspend is therefore legal at every call site, and even
// where it weren't, the suspend is strictly no worse than the blocking XHR it
// replaces.
// ---------------------------------------------------------------------------

// JSPI-suspending async fetch. Returns bytes written to MEMFS, or -1 on error.
// `Asyncify.handleAsync` (em_js.h) resolves to the JSPI suspend path under
// -sJSPI (libasync.js:462), so the wasm caller suspends across the await.
EM_ASYNC_JS(int, webAssetsAsyncFetchToMemfs, (const char *urlC, const char *memfsPathC), {
    try {
        var url = UTF8ToString(urlC);
        var memfsPath = UTF8ToString(memfsPathC);
        var res = await fetch(url);
        if (!res.ok) {
            console.log("WebAssets: fetch failed " + url + " status=" + res.status);
            return -1;
        }
        // fetch() transparently decodes any Content-Encoding (br/gz) before
        // arrayBuffer(), so `data` is the raw asset bytes — same as the old
        // XHR.responseText path produced.
        var buf = await res.arrayBuffer();
        var data = new Uint8Array(buf);

        // Create parent directories (identical to the legacy path).
        var parts = memfsPath.split("/");
        var dir = "";
        for (var i = 0; i < parts.length - 1; i++) {
            if (parts[i] === "") continue;
            dir += "/" + parts[i];
            try { FS.mkdir(dir); } catch (e) {}
        }

        FS.writeFile(memfsPath, data);
        return data.length;
    } catch (e) {
        console.log("WebAssets: fetch exception: " + e);
        return -1;
    }
});

// Legacy synchronous XHR + per-byte string convert. Kept behind
// RB3_SYNC_XHR_LEGACY for one release. Returns bytes written, or -1 on error.
static int webAssetsLegacyXhrToMemfs(const char *url, const char *memfsPath) {
    // Use synchronous XHR to fetch the file, then write to MEMFS via FS API.
    // Note: synchronous XHR cannot set responseType="arraybuffer" in browsers,
    // so we use overrideMimeType to force binary and manually convert the response.
    return EM_ASM_INT({
        try {
            var url = UTF8ToString($0);
            var memfsPath = UTF8ToString($1);
            var xhr = new XMLHttpRequest();
            xhr.open("GET", url, false);  // synchronous
            xhr.overrideMimeType("text/plain; charset=x-user-defined");
            xhr.send();
            if (xhr.status !== 200) {
                console.log("WebAssets: XHR failed " + url + " status=" + xhr.status);
                return -1;
            }

            // Convert binary string to Uint8Array
            var text = xhr.responseText;
            var data = new Uint8Array(text.length);
            for (var i = 0; i < text.length; i++) {
                data[i] = text.charCodeAt(i) & 0xFF;
            }

            // Create parent directories
            var parts = memfsPath.split("/");
            var dir = "";
            for (var i = 0; i < parts.length - 1; i++) {
                if (parts[i] === "") continue;
                dir += "/" + parts[i];
                try { FS.mkdir(dir); } catch(e) {}
            }

            // Write file to MEMFS
            FS.writeFile(memfsPath, data);
            return text.length;
        } catch(e) {
            console.log("WebAssets: XHR exception: " + e);
            return -1;
        }
    }, url, memfsPath);
}

// One-time read of the legacy opt-out (RB3_SYNC_XHR_LEGACY=1 forces the old
// blocking string-XHR). Pattern: static init + getenv (Loader.cpp:217-226).
static bool webAssetsUseLegacyXhr() {
    static int sLegacy = -1;
    if (sLegacy < 0) {
        const char *e = getenv("RB3_SYNC_XHR_LEGACY");
        sLegacy = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return sLegacy != 0;
}

bool WebAssetsFetchSync(const char *memfsPath) {
    // Normalize the MEMFS path to a server-relative path.
    // Paths come in several forms:
    //   /data/ui/gen/foo.milo_xbox          -> ui/gen/foo.milo_xbox
    //   /system/run/ham/gen/skeleton.milo   -> system/run/ham/gen/skeleton.milo
    //   /../system/run/config/gen/meta.milo -> system/run/config/gen/meta.milo
    const char *rel = memfsPath;
    if (strncmp(rel, "/data/", 6) == 0) {
        rel += 6;
    } else if (strncmp(rel, "/../", 4) == 0) {
        rel += 4;  // strip "/../" -> "system/run/..."
    } else if (rel[0] == '/') {
        rel += 1;  // strip leading "/" -> "system/run/..."
    }

    // Build server URL
    char url[512];
    snprintf(url, sizeof(url), "/api/file/%s", rel);

#ifdef DEBUG_LOGS
    printf("WebAssets: on-demand fetch %s -> %s\n", url, memfsPath);
#endif

    // Frame-trace: this blocking fetch is the I/O baseline every other counter
    // is judged against — the single biggest canvas-freeze suspect on web. Time
    // the whole fetch+write, count the call, and accumulate the byte count
    // (returned on success; -1 on failure). With the async path the wall-clock
    // here includes the JSPI-suspended network wait (during which the event loop
    // ran), but it remains the right thing to attribute to this open.
    double ftStart = gFrameTraceActive ? FrameTraceNowMs() : 0.0;

    int bytesWritten = webAssetsUseLegacyXhr()
                           ? webAssetsLegacyXhrToMemfs(url, memfsPath)
                           : webAssetsAsyncFetchToMemfs(url, memfsPath);

    bool result = (bytesWritten >= 0);
    if (gFrameTraceActive) {
        gFetchSyncMsThisFrame += (float)(FrameTraceNowMs() - ftStart);
        gFetchSyncCountThisFrame++;
        if (result) gFetchSyncBytesThisFrame += (double)bytesWritten;
    }

    if (!result) {
        fprintf(stderr, "WebAssets: FAILED on-demand fetch %s\n", rel);
    }
    return result;
}

bool WebAssetsAllDone() { return sPending == 0; }
int WebAssetsPendingCount() { return sPending; }
int WebAssetsCompletedCount() { return sCompleted; }
int WebAssetsFailedCount() { return sFailed; }

#endif // __EMSCRIPTEN__
