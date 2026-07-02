// Web Asset Fetcher
// Downloads game assets from the dev server's HTTP API into Emscripten MEMFS.
// Assets are stored under /data/ in MEMFS, mirroring the server's directory structure.

#pragma once

#ifdef __EMSCRIPTEN__

// Create the MEMFS directory skeleton (/data/config/, /data/ui/, etc.)
void WebAssetsInit();

// Queue an async fetch of a file from the server.
// serverPath is relative (e.g. "config/ham_keep.dta").
// The file will be written to /data/<serverPath> in MEMFS when complete.
// Returns a fetch ID for tracking.
int WebAssetsFetch(const char *serverPath);

// Check if a specific fetch is complete (success or failure).
bool WebAssetsFetchDone(int fetchId);

// Check if ALL pending fetches have completed.
bool WebAssetsAllDone();

// Download a bundle of assets as a single HTTP request and unpack it into
// /data/ in MEMFS. Much faster than individual fetches for bulk loading.
//
// `url` selects the server bundle route; it defaults to "/api/bundle" (the
// .dta/.dtb config bundle) so every existing caller is unchanged. RB3's web
// boot also fires "/api/bundle/boot" (the boot-critical .milo_xbox set, R3) via
// the same async + unpack path; both bump the shared pending counter, so the
// boot gate (WebAssetsAllDone) waits for all in-flight bundles.
void WebAssetsFetchBundle(const char *url = "/api/bundle");

// Synchronously fetch a single file from the server into MEMFS.
// memfsPath is the full MEMFS path (e.g. "/data/ui/gen/helpbar.milo_xbox").
// Returns true if the file was fetched and written successfully.
bool WebAssetsFetchSync(const char *memfsPath);

// Counters
int WebAssetsPendingCount();
int WebAssetsCompletedCount();
int WebAssetsFailedCount();

// ---------------------------------------------------------------------------
// A1 (incremental-load-perf PLAN.md T6) — pending-File async-open support.
//
// The manifest map (/api/manifest, server.py:414-435) is both the synchronous
// Size() oracle and a synchronous *positive* existence oracle for a pending
// open: a WebPendingFile answers Size()/Fail() from it the instant it is
// constructed, with zero network. It is pre-warmed into a JS map by rb3_pre.js
// (racing the wasm download); WebAssetsManifestLoad() reads that map if present,
// else pulls /api/manifest itself (sync JSPI fetch) so the oracle is always
// available by the first on-demand open.
// ---------------------------------------------------------------------------

// Load the asset manifest (path -> size) into an internal C-side map. Idempotent
// (no-op after the first successful load). Called from WebAssetsInit(); safe to
// call again. Returns the number of entries loaded (0 if unavailable).
int WebAssetsManifestLoad();

// Look up an asset's size by its SERVER-RELATIVE path (the same key form passed
// to /api/file/<rel>, e.g. "ui/gen/foo.milo_xbox" or "system/run/.../bar.milo").
// Returns the byte size if the manifest knows the path, or -1 if it does not.
//
// IMPORTANT: a -1 result is NOT a definitive 404 — the manifest only covers the
// curated ASSETS_DIR, while the server ALSO serves files from fallback roots and
// sidecar dirs that the manifest never walks. So -1 means "not a manifest-backed
// async-open candidate; use the legacy sync fallback", not "this file is absent".
// A non-negative result is a reliable existence + size answer.
long WebAssetsManifestSize(const char *serverRelPath);

// Kick an async fetch of `serverRelPath` into MEMFS (/data/<rel>) if it is not
// already resident and not already in flight. Idempotent: a duplicate call for
// the same path while a fetch is pending is a no-op (in-flight dedupe). This is
// the async counterpart to WebAssetsFetchSync used by WebPendingFile's open.
void WebAssetsEnsureResidentAsync(const char *serverRelPath);

// True once `serverRelPath` is resident in MEMFS (/data/<rel>) — i.e. a prior
// WebAssetsEnsureResidentAsync / WebAssetsFetch / bundle write has landed.
bool WebAssetsIsResident(const char *serverRelPath);

// Status of a pending async open: 0 = pending (fetch in flight, keep polling),
// 1 = resident (bytes landed, open the real file), 2 = failed (fetch finished
// without residency — report Fail() so the loader cleans up). A WebPendingFile
// polls this each ReadDone() to advance or fail without spinning forever.
int WebAssetsEnsureStatus(const char *serverRelPath);

// ---------------------------------------------------------------------------
// Q3 (PLAN.md T7) — Range-backed streaming for .mogg. A WebRangeFile fetches
// fixed-size byte windows over HTTP 206 instead of whole-filing the 31-36 MB
// mogg into MEMFS. The engine owns the async fetch + completion bookkeeping; the
// File-side cache/seek logic lives in native_file.cpp.
// ---------------------------------------------------------------------------

// Start an async HTTP Range fetch of [offset, offset+length) of the asset at
// `serverRelPath`. Returns a positive request id to poll, or 0 on immediate
// failure. The bytes are NOT written to MEMFS (Range responses are transient).
int WebAssetsRangeFetch(const char *serverRelPath, long offset, int length);

// Poll a Range fetch. Returns true when complete (success or error). On success
// (*outBytes set to the byte count, which equals the requested length unless the
// server clamped at EOF), the caller must immediately WebAssetsRangeTake() the
// data. *outOk reports success vs error.
bool WebAssetsRangeDone(int reqId, int *outBytes, bool *outOk);

// Copy a completed Range fetch's bytes into `dst` (up to `dstCap`) and release
// the request. Returns the number of bytes copied. Only valid once
// WebAssetsRangeDone(reqId) returned true with *outOk==true. Frees the request.
int WebAssetsRangeTake(int reqId, void *dst, int dstCap);

// Drop a Range request without taking its bytes (e.g. on error). Frees it.
void WebAssetsRangeDrop(int reqId);

// Number of Range fetches currently in flight (issued, not yet completed,
// not abandoned). The sharpen-sidecar chunk pump polls this before kicking a
// chunk so a cosmetic sidecar transfer strictly yields to mogg streaming
// (research/14 Lane B). Cheap (registry scan; the registry holds only live +
// just-completed-unconsumed requests).
int WebAssetsRangeInFlightCount();

#endif // __EMSCRIPTEN__
