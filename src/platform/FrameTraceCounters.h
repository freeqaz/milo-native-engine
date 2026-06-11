// FrameTraceCounters.h — per-frame attribution counters for the load-perf
// frame tracer (rb3 native/src/rb3_frame_trace.cpp, RB3_FRAME_TRACE=<path>).
//
// These are the engine-side choke-point counters that complement the existing
// rb3-side `lp`/`lpu`/`ld`/`st` fields. Each `g*MsThisFrame` accumulates the ms
// spent THIS frame inside one cost class (fetch, dta-parse, object-load, audio
// prime, texture/mesh upload, pipeline create, stream inflate). The recorder
// reads + zeroes them AFTER writing each JSONL line (the same convention as the
// existing `gFrameTraceLoaderAdds`/`gFrameTraceStreamOpens` event counters).
//
// Linkage: the strong definitions live in rb3 (src/system/utl/Loader.cpp), so
// every native target that links Loader.cpp (rb3-native, rb3-web) resolves them
// there. The engine is ALSO built standalone (milo-engine-tests) without
// Loader.cpp, so this header's matching .cpp provides WEAK fallback storage —
// the strong rb3 defs override the weak engine defs at final link, and the
// standalone engine library links cleanly. All increments are guarded by
// `gFrameTraceActive` (false until a trace file is opened), so they cost one
// predicted-not-taken branch when tracing is off.

#pragma once

#include <chrono>

// Monotonic ms clock shared by all engine-side trace timers. Header-inline so
// every instrumented TU (WebAssets / ThreadCall / Rnd_Wgpu_RB3 / PipelineManager)
// uses the same source; works on native (steady_clock) and Emscripten.
static inline double FrameTraceNowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The gate (defined strong in rb3 Loader.cpp, weak in PipelineManager.cpp).
extern bool gFrameTraceActive;

// ms-this-frame accumulators (read + zeroed by RB3FrameTraceRecord).
extern float gFetchSyncMsThisFrame;      // WebAssetsFetchSync blocking XHR (web)
extern int   gFetchSyncCountThisFrame;   // # of blocking fetches this frame
extern double gFetchSyncBytesThisFrame;  // bytes pulled by those fetches
extern float gDtaParseMsThisFrame;       // ThreadCallPoll run-inline job (web)
extern float gObjLoadMsThisFrame;        // DirLoader PreLoad+PostLoad per object
extern float gObjLoadWorstMs;            // slowest single object this frame
extern char  gObjLoadWorstName[64];      // class:name of the slowest object
extern float gAudioPrimeMsThisFrame;     // StandardStream::Play prime pumps
extern float gTexUploadMsThisFrame;      // UploadRndTexIfNeeded decode+upload
extern int   gTexUploadCountThisFrame;   // # of texture uploads this frame
extern float gMeshUploadMsThisFrame;     // DrawMesh needUpload VB/IB write
extern int   gMeshUploadCountThisFrame;  // # of mesh uploads this frame
extern float gVertUnpackMsThisFrame;     // DrawMesh CPU vertex unpack (Be*/Half2Float)
extern int   gVertUnpackCountThisFrame;  // # of meshes whose verts were unpacked this frame
extern float gPipelineCreateMsThisFrame; // PipelineManager::GetPipeline miss
extern int   gPipelineCreateCountThisFrame;
extern float gStreamReadMsThisFrame;     // ChunkStream read/inflate byte-shovel
