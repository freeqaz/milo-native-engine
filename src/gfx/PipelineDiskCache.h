#pragma once

// Persistent pipeline cache: the store behind Dawn's blob cache
// (wgpu::DawnCacheDeviceDescriptor). Dawn hands it the compiled shaders
// (Tint's SPIR-V for each pipeline stage) and the Vulkan pipeline cache data
// for each render pipeline, under keys Dawn builds; the next run reads them
// back instead of compiling. Native only -- on the web the browser keeps its
// own shader and pipeline caches and Dawn's blob cache does not exist there,
// so this file is not in the web build at all.
//
// Where: the directory the consumer names (MiloPipelineCacheDefaultDir; DC3:
// <its cache root>/dawn-pipelines) or MILO_PIPELINE_CACHE=<path>;
// MILO_PIPELINE_CACHE=0 (or "off") turns it off, and so does a consumer that
// names no directory -- rb3 and rb3-xenon, which run without it unless asked.
//
// What makes a blob readable: Dawn's own key (its device cache key -- adapter,
// backend, toggles, the isolation key below -- plus the shader source and the
// pipeline description; Vulkan's pipeline-cache blobs are also checked by the
// driver against its own header), and this file's isolation key, which is the
// running executable's build id (so a rebuilt engine, Dawn or Tint never reads
// an old build's blobs), the adapter's vendor/device ids and its description
// (on Vulkan that carries the driver version), and a format version. Blobs of
// one isolation key live in their own subdirectory.
//
// Safe to delete: every file is a cache entry and nothing else; a missing
// directory is created. A blob that fails to read back exactly (short file,
// wrong magic, a key that is not the one asked for, a checksum mismatch) is a
// miss: Dawn compiles as if it were absent and stores a fresh one.
//
// Bounded: kMaxBytes over the whole directory. Open() evicts the oldest files
// (by modification time, which a read refreshes) down to 3/4 of it, and a run
// stops storing once it would pass it.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

class PipelineDiskCache {
public:
    static constexpr uint64_t kMaxBytes = 64ull << 20;

    struct Stats {
        uint64_t hits = 0, misses = 0, rejected = 0, stores = 0, storeSkipped = 0;
        uint64_t bytesRead = 0, bytesWritten = 0, evicted = 0;
    };

    // The cache directory this run uses: MILO_PIPELINE_CACHE if set (0/off =
    // none), else the consumer's MiloPipelineCacheDefaultDir(). Empty = off.
    static std::string ResolveDir();

    // Prepare <dir>/<hex of isolation> and evict over the bound. False (and
    // the cache stays off) when the directory cannot be made.
    // maxBytes: the bound (kMaxBytes; a test passes a small one).
    bool Open(const std::string& dir, const std::string& isolation, uint64_t maxBytes = kMaxBytes);
    bool IsOpen() const { return !mDir.empty(); }
    const std::string& Dir() const { return mDir; }

    // Dawn's callbacks (WGPUDawnLoadCacheDataFunction / StoreCacheData),
    // userdata = this. Thread-safe: Dawn may call them from its workers.
    static size_t LoadThunk(const void* key, size_t keySize, void* value, size_t valueSize,
                            void* userdata);
    static void StoreThunk(const void* key, size_t keySize, const void* value, size_t valueSize,
                           void* userdata);

    Stats GetStats() const;

    static uint64_t Hash64(const void* data, size_t size, uint64_t seed = 0xcbf29ce484222325ull);
    // The running executable's build id (GNU build-id note) as hex, "" if it
    // has none (then the cache stays off: nothing would tell builds apart).
    static std::string ExecutableBuildId();

private:
    size_t Load(const void* key, size_t keySize, void* value, size_t valueSize);
    void Store(const void* key, size_t keySize, const void* value, size_t valueSize);
    std::string PathFor(const void* key, size_t keySize) const;

    std::string mDir;
    uint64_t mMaxBytes = kMaxBytes;
    std::atomic<uint64_t> mTotalBytes{0};
    std::atomic<uint64_t> mHits{0}, mMisses{0}, mRejected{0}, mStores{0}, mStoreSkipped{0};
    std::atomic<uint64_t> mBytesRead{0}, mBytesWritten{0}, mEvicted{0};
};

// The consumer's default cache directory, or nullptr for none (the engine's
// weak default). DC3 defines it (native/src/platform/Cache_Stub.cpp).
const char* MiloPipelineCacheDefaultDir();
