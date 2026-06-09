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

#endif // __EMSCRIPTEN__
