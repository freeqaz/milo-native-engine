// Persistent pipeline cache (see PipelineDiskCache.h). Native only.

#include "gfx/PipelineDiskCache.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#if defined(__linux__)
#include <elf.h>
#include <link.h>
#endif

__attribute__((weak)) const char* MiloPipelineCacheDefaultDir() { return nullptr; }

namespace {

constexpr char kMagic[4] = {'M', 'P', 'C', '1'};

struct BlobHeader {
    char magic[4];
    uint32_t keySize;
    uint64_t valueSize;
    uint64_t valueHash;
};
static_assert(sizeof(BlobHeader) == 24, "blob header layout");

bool MakeDirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || (path[i] == '/' && i > 0)) {
            cur = path.substr(0, i);
            if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    }
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

struct FileEntry {
    std::string path;
    uint64_t size;
    time_t mtime;
    bool temp;
};

// Every file one or two levels under root (root/<isolation>/<blob>).
void ListFiles(const std::string& root, int depth, std::vector<FileEntry>* out) {
    DIR* d = opendir(root.c_str());
    if (!d) return;
    while (struct dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        std::string p = root + "/" + e->d_name;
        struct stat st;
        if (lstat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth > 0) ListFiles(p, depth - 1, out);
        } else if (S_ISREG(st.st_mode)) {
            out->push_back({p, (uint64_t)st.st_size, st.st_mtime,
                            strstr(e->d_name, ".tmp.") != nullptr});
        }
    }
    closedir(d);
}

bool ReadWhole(const std::string& path, std::vector<uint8_t>* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    bool ok = fseek(f, 0, SEEK_END) == 0;
    long n = ok ? ftell(f) : -1;
    ok = ok && n >= 0 && fseek(f, 0, SEEK_SET) == 0;
    if (ok) {
        out->resize((size_t)n);
        ok = n == 0 || fread(out->data(), 1, (size_t)n, f) == (size_t)n;
    }
    fclose(f);
    return ok;
}

// The last blob a thread validated: Dawn asks for the size, then for the
// bytes, so the second call copies instead of reading the file again.
thread_local std::string tLastPath;
thread_local std::vector<uint8_t> tLastFile;

} // namespace

uint64_t PipelineDiskCache::Hash64(const void* data, size_t size, uint64_t seed) {
    // FNV-1a, 64-bit.
    uint64_t h = seed;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

std::string PipelineDiskCache::ExecutableBuildId() {
#if defined(__linux__)
    std::string id;
    dl_iterate_phdr(
        [](struct dl_phdr_info* info, size_t, void* data) -> int {
            // The first object is the executable itself.
            std::string* out = (std::string*)data;
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const ElfW(Phdr)& ph = info->dlpi_phdr[i];
                if (ph.p_type != PT_NOTE) continue;
                const uint8_t* p = (const uint8_t*)(info->dlpi_addr + ph.p_vaddr);
                const uint8_t* end = p + ph.p_memsz;
                while (p + sizeof(ElfW(Nhdr)) <= end) {
                    const ElfW(Nhdr)* n = (const ElfW(Nhdr)*)p;
                    const uint8_t* name = p + sizeof(ElfW(Nhdr));
                    const uint8_t* desc = name + ((n->n_namesz + 3) & ~3u);
                    if (desc + n->n_descsz > end) break;
                    if (n->n_type == NT_GNU_BUILD_ID && n->n_namesz == 4 &&
                        memcmp(name, "GNU", 4) == 0) {
                        static const char kHex[] = "0123456789abcdef";
                        for (uint32_t b = 0; b < n->n_descsz; ++b) {
                            out->push_back(kHex[desc[b] >> 4]);
                            out->push_back(kHex[desc[b] & 15]);
                        }
                        return 1;
                    }
                    p = desc + ((n->n_descsz + 3) & ~3u);
                }
            }
            return 1;  // only the executable
        },
        &id);
    return id;
#else
    return "";
#endif
}

std::string PipelineDiskCache::ResolveDir() {
    if (const char* v = getenv("MILO_PIPELINE_CACHE"); v && *v) {
        if (!strcmp(v, "0") || !strcmp(v, "off") || !strcmp(v, "OFF")) return "";
        return v;
    }
    const char* d = MiloPipelineCacheDefaultDir();
    return d && *d ? d : "";
}

bool PipelineDiskCache::Open(const std::string& dir, const std::string& isolation,
                             uint64_t maxBytes) {
    mDir.clear();
    mMaxBytes = maxBytes;
    if (dir.empty()) return false;
    char sub[17];
    snprintf(sub, sizeof(sub), "%016llx",
             (unsigned long long)Hash64(isolation.data(), isolation.size()));
    std::string path = dir + "/" + sub;
    if (!MakeDirs(path)) {
        fprintf(stderr, "PipelineDiskCache: cannot create %s (%s); off\n", path.c_str(),
                strerror(errno));
        return false;
    }
    // Bound the whole directory (every isolation key's blobs): drop abandoned
    // temporaries, then the least recently used files down to 3/4 of the cap.
    std::vector<FileEntry> files;
    ListFiles(dir, 1, &files);
    const time_t now = time(nullptr);
    uint64_t total = 0;
    std::vector<FileEntry> kept;
    for (FileEntry& f : files) {
        if (f.temp && now - f.mtime > 600) {
            if (unlink(f.path.c_str()) == 0) mEvicted++;
            continue;
        }
        total += f.size;
        kept.push_back(std::move(f));
    }
    if (total > mMaxBytes) {
        std::sort(kept.begin(), kept.end(),
                  [](const FileEntry& a, const FileEntry& b) { return a.mtime < b.mtime; });
        for (const FileEntry& f : kept) {
            if (total <= mMaxBytes / 4 * 3) break;
            if (f.temp) continue;
            if (unlink(f.path.c_str()) == 0) {
                total -= f.size;
                mEvicted++;
            }
        }
    }
    mTotalBytes = total;
    mDir = path;
    return true;
}

std::string PipelineDiskCache::PathFor(const void* key, size_t keySize) const {
    char name[24];
    snprintf(name, sizeof(name), "%016llx", (unsigned long long)Hash64(key, keySize));
    return mDir + "/" + name + ".blob";
}

size_t PipelineDiskCache::Load(const void* key, size_t keySize, void* value, size_t valueSize) {
    const std::string path = PathFor(key, keySize);
    if (value && tLastPath == path) {
        // Second call: the bytes validated by the first.
        const BlobHeader* h = (const BlobHeader*)tLastFile.data();
        const uint8_t* v = tLastFile.data() + sizeof(BlobHeader) + h->keySize;
        const size_t n = (size_t)h->valueSize;
        tLastPath.clear();
        if (valueSize < n) return 0;
        memcpy(value, v, n);
        mBytesRead += n;
        return n;
    }
    tLastPath.clear();
    std::vector<uint8_t> file;
    if (!ReadWhole(path, &file)) {
        mMisses++;
        return 0;
    }
    const BlobHeader* h = (const BlobHeader*)file.data();
    const bool ok = file.size() >= sizeof(BlobHeader) && memcmp(h->magic, kMagic, 4) == 0 &&
                    h->keySize == keySize &&
                    file.size() == sizeof(BlobHeader) + (uint64_t)h->keySize + h->valueSize &&
                    memcmp(file.data() + sizeof(BlobHeader), key, keySize) == 0 &&
                    Hash64(file.data() + sizeof(BlobHeader) + keySize, (size_t)h->valueSize) ==
                        h->valueHash;
    if (!ok) {
        // Short, foreign or damaged: a miss, and gone so a fresh one replaces it.
        mRejected++;
        mMisses++;
        if (unlink(path.c_str()) == 0) mTotalBytes -= std::min<uint64_t>(mTotalBytes, file.size());
        fprintf(stderr, "PipelineDiskCache: rejected %s (%zu bytes); compiling instead\n",
                path.c_str(), file.size());
        return 0;
    }
    mHits++;
    utimensat(AT_FDCWD, path.c_str(), nullptr, 0);  // recently used: evicted last
    const size_t n = (size_t)h->valueSize;
    if (value) {
        if (valueSize < n) return 0;
        memcpy(value, file.data() + sizeof(BlobHeader) + keySize, n);
        mBytesRead += n;
        return n;
    }
    tLastPath = path;
    tLastFile.swap(file);
    return n;
}

void PipelineDiskCache::Store(const void* key, size_t keySize, const void* value,
                              size_t valueSize) {
    const uint64_t bytes = sizeof(BlobHeader) + keySize + valueSize;
    if (keySize > 0xffffffffu || mTotalBytes + bytes > mMaxBytes) {
        mStoreSkipped++;
        return;
    }
    const std::string path = PathFor(key, keySize);
    static std::atomic<uint32_t> sSeq{0};
    char tmp[64];
    snprintf(tmp, sizeof(tmp), ".tmp.%d.%u", (int)getpid(), sSeq++);
    const std::string tmpPath = path + tmp;
    BlobHeader h{};
    memcpy(h.magic, kMagic, 4);
    h.keySize = (uint32_t)keySize;
    h.valueSize = valueSize;
    h.valueHash = Hash64(value, valueSize);
    FILE* f = fopen(tmpPath.c_str(), "wb");
    if (!f) {
        mStoreSkipped++;
        return;
    }
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(key, 1, keySize, f) == keySize &&
              fwrite(value, 1, valueSize, f) == valueSize;
    ok = (fclose(f) == 0) && ok;
    // Renamed into place whole, so a reader never sees half a blob.
    if (!ok || rename(tmpPath.c_str(), path.c_str()) != 0) {
        unlink(tmpPath.c_str());
        mStoreSkipped++;
        return;
    }
    mStores++;
    mTotalBytes += bytes;
    mBytesWritten += bytes;
}

size_t PipelineDiskCache::LoadThunk(const void* key, size_t keySize, void* value,
                                    size_t valueSize, void* userdata) {
    return ((PipelineDiskCache*)userdata)->Load(key, keySize, value, valueSize);
}

void PipelineDiskCache::StoreThunk(const void* key, size_t keySize, const void* value,
                                   size_t valueSize, void* userdata) {
    ((PipelineDiskCache*)userdata)->Store(key, keySize, value, valueSize);
}

PipelineDiskCache::Stats PipelineDiskCache::GetStats() const {
    Stats s;
    s.hits = mHits;
    s.misses = mMisses;
    s.rejected = mRejected;
    s.stores = mStores;
    s.storeSkipped = mStoreSkipped;
    s.bytesRead = mBytesRead;
    s.bytesWritten = mBytesWritten;
    s.evicted = mEvicted;
    return s;
}
