// Progressive-texture-sharpen manager (research/13 T1) — native-only.
//
// See RB3TexSharpen.h for the design. In short: A4 stripped the top mip off web
// venue textures (half-res base, fast load to gameplay); this manager restores
// them to full resolution live, in-session, by swapping each RndBitmap back up to
// the full-res top-mip carried in the venue's `.sharpen` sidecar and re-invoking
// the GPU upload (UploadRndTexIfNeeded recreates the texture at the new size +
// publishes a new view; the cached material bind group rebuilds automatically).
//
// Owns: the parsed sidecar entry table, the fingerprint→entry index, the matched
// RndTex list, the incremental scheduler. Does NOT do I/O — the rb3 glue fetches
// the sidecar (low-priority async on web; local file on native) and hands the
// bytes to RB3SharpenLoadSidecar.

#include "platform/RB3TexSharpen.h"

#include "rndobj/Tex.h"
#include "rndobj/Bitmap.h"
#include "obj/Dir.h"      // ObjectDir, ObjDirItr<RndTex>
#include "obj/Object.h"
#include "utl/MemMgr.h"   // _MemAlloc / _MemFree (the allocator RndBitmap owns its buffer with)

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Flags (getenv-once).
// ---------------------------------------------------------------------------
bool RB3ProgressiveSharpenEnabled() {
    static int s = -1;
    if (s < 0) {
        const char* e = getenv("RB3_PROGRESSIVE_SHARPEN");
        // Default ON; opt-out only on an explicit falsey value.
        s = (e && (e[0] == '0' || e[0] == 'f' || e[0] == 'n' || e[0] == 'F' ||
                   e[0] == 'N')) ? 0 : 1;
    }
    return s != 0;
}

int RB3SharpenPerFrame() {
    static int n = -1;
    if (n < 0) {
        const char* e = getenv("RB3_SHARPEN_PER_FRAME");
        n = e ? atoi(e) : 4;
        if (n < 1) n = 1;
        if (n > 256) n = 256;
    }
    return n;
}

static bool RB3SharpenDbg() {
    static int s = -1;
    if (s < 0) s = (getenv("RB3_SHARPEN_DBG") != nullptr) ? 1 : 0;
    return s != 0;
}

// MILO_LOG is heavy to pull in here; a tiny stderr shim keeps this TU lean and
// only fires under RB3_SHARPEN_DBG.
#include <cstdio>
#define SHARPEN_LOG(...) do { if (RB3SharpenDbg()) fprintf(stderr, __VA_ARGS__); } while (0)

// ---------------------------------------------------------------------------
// SHRP sidecar format (mirrors scripts/milo/mip_strip.py, all LE; the embedded
// top-mip pixel bytes are verbatim on-disk BE DXT words the upload path swaps).
//
//   magic 'SHRP', u32 version, u32 levels, u32 entry_count
//   per entry: '<I HHH HHH BB I I I I' =
//     index, full_w, full_h, full_rb, stripped_w, stripped_h, stripped_rb,
//     bpp, _pad, order, stripped_fp, topmip_len, name_len
//   then name (name_len bytes, latin-1), then topmip (topmip_len bytes).
// ---------------------------------------------------------------------------
namespace {

const uint32_t kSharpenMagic = 0x50524853u; // 'SHRP' little-endian as read below
const uint32_t kSharpenVersion = 1;

static inline uint32_t RdU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static inline uint16_t RdU16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

struct SharpenEntry {
    uint32_t index = 0;
    uint16_t fullW = 0, fullH = 0, fullRb = 0;
    uint16_t strippedW = 0, strippedH = 0, strippedRb = 0;
    uint8_t  bpp = 0;
    uint32_t order = 0;
    uint32_t strippedFp = 0;
    // Owned full-res base-level (top-mip) bytes — a copy of the sidecar slice, so
    // it outlives the transient sidecar blob and can be handed to the RndBitmap.
    std::vector<uint8_t> topmip;
    // Best-effort RndTex object name (latin-1, null-terminated). Empty when the
    // sidecar declined the name correlation (fingerprint is then authoritative).
    // std::string is shadowed by the matched-fork stlport headers in this TU, so
    // a char vector is used directly.
    std::vector<char> name;

    // Match state:
    RndTex* tex = nullptr;   // the loaded RndTex this entry matched
    bool    sharpened = false;
};

struct SharpenSession {
    ObjectDir* venueDir = nullptr;
    uint32_t   levels = 1;
    std::vector<SharpenEntry> entries;     // sidecar entries (all, in file order)
    std::vector<int> matchedIdx;           // indices into `entries` that matched a tex
    int        nextToSharpen = 0;          // cursor into matchedIdx
    int        sharpenedCount = 0;
    uint64_t   bytesUpgraded = 0;
    bool       active = false;
};

SharpenSession gSession;

// Parse the SHRP blob into `out` (copying each top-mip slice). Returns false on
// any structural surprise (caller then leaves the session inactive).
bool ParseSidecar(const uint8_t* d, uint32_t n, std::vector<SharpenEntry>& out,
                  uint32_t& levelsOut) {
    out.clear();
    if (!d || n < 16) return false;
    if (RdU32(d + 0) != kSharpenMagic) { SHARPEN_LOG("[sharpen] bad magic\n"); return false; }
    uint32_t ver   = RdU32(d + 4);
    uint32_t levels = RdU32(d + 8);
    uint32_t count = RdU32(d + 12);
    if (ver != kSharpenVersion) { SHARPEN_LOG("[sharpen] bad version %u\n", ver); return false; }
    if (count > 100000) return false;
    levelsOut = levels;
    // Per-entry fixed header: '<I HHH HHH BB I I I I' = 4 + 6*2 + 2 + 4*4 = 34 bytes.
    const uint32_t kRec = 34;
    uint32_t o = 16;
    out.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        if (o + kRec > n) return false;
        SharpenEntry e;
        e.index     = RdU32(d + o + 0);
        e.fullW     = RdU16(d + o + 4);
        e.fullH     = RdU16(d + o + 6);
        e.fullRb    = RdU16(d + o + 8);
        e.strippedW = RdU16(d + o + 10);
        e.strippedH = RdU16(d + o + 12);
        e.strippedRb= RdU16(d + o + 14);
        e.bpp       = d[o + 16];
        // d[o + 17] == pad
        e.order     = RdU32(d + o + 18);
        e.strippedFp= RdU32(d + o + 22);
        uint32_t topmipLen = RdU32(d + o + 26);
        uint32_t nameLen   = RdU32(d + o + 30);
        o += kRec;
        if (o + nameLen > n) return false;
        if (nameLen > 0) {
            e.name.assign((const char*)(d + o), (const char*)(d + o) + nameLen);
            e.name.push_back('\0');
        }
        o += nameLen;
        if ((uint64_t)o + topmipLen > n) return false;
        // The carried top-mip must be exactly the full base level (full_rb*full_h);
        // reject any entry that doesn't reconstruct a coherent full-res base so a
        // corrupt sidecar can never drive an over-/under-sized GPU upload.
        uint32_t expect = (uint32_t)e.fullRb * (uint32_t)e.fullH;
        if (topmipLen != expect) {
            SHARPEN_LOG("[sharpen] entry %u topmip %u != full %u — skip\n",
                        i, topmipLen, expect);
            o += topmipLen;
            continue;
        }
        e.topmip.assign(d + o, d + o + topmipLen);
        o += topmipLen;
        out.push_back(std::move(e));
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Load + match.
// ---------------------------------------------------------------------------
int RB3SharpenLoadSidecar(ObjectDir* venueDir, const uint8_t* bytes, uint32_t len) {
    if (!RB3ProgressiveSharpenEnabled() || !venueDir || !bytes || len == 0)
        return 0;

    RB3SharpenReset();

    std::vector<SharpenEntry> entries;
    uint32_t levels = 1;
    if (!ParseSidecar(bytes, len, entries, levels)) {
        SHARPEN_LOG("[sharpen] sidecar parse failed (%u bytes)\n", len);
        return 0;
    }
    if (entries.empty()) return 0;

    gSession.venueDir = venueDir;
    gSession.levels = levels;
    gSession.entries = std::move(entries);
    gSession.matchedIdx.clear();
    gSession.nextToSharpen = 0;
    gSession.sharpenedCount = 0;
    gSession.bytesUpgraded = 0;
    gSession.active = true;

    // Build a fingerprint → entry-index multimap. The fingerprint is the robust
    // primary match key (the sidecar carries no reliable name on these venues).
    // Several distinct textures CAN share a fingerprint (it samples 8 bytes), so
    // use a multimap and disambiguate by full size — a tex only matches an entry
    // whose stripped dimensions equal the tex's CURRENT (loaded) dimensions.
    std::unordered_multimap<uint32_t, int> byFp;
    for (size_t i = 0; i < gSession.entries.size(); i++)
        byFp.emplace(gSession.entries[i].strippedFp, (int)i);

    // Walk the venue's loaded RndTex objects (incl. resident subdirs). For each,
    // recompute the fingerprint over its live (stripped) pixels and find the
    // sidecar entry whose strippedFp + stripped W/H/bpp/order all agree. First
    // unclaimed entry wins (each entry → at most one tex; each tex → at most one
    // entry) so two identical-fingerprint textures don't both grab the same entry.
    int matched = 0;
    for (ObjDirItr<RndTex> it(venueDir, true); it != nullptr; ++it) {
        RndTex* tex = it;
        if (!tex) continue;
        const RndBitmap& bmp = tex->mBitmap;
        if (!bmp.Pixels() || bmp.Width() <= 0 || bmp.Height() <= 0) continue;
        uint32_t fp = RB3SharpenTexFingerprint(tex);
        if (fp == 0) continue;

        auto range = byFp.equal_range(fp);
        for (auto mit = range.first; mit != range.second; ++mit) {
            SharpenEntry& e = gSession.entries[mit->second];
            if (e.tex) continue;                          // already claimed
            // Dimensional agreement: the loaded bitmap must be the STRIPPED level.
            if ((int)e.strippedW != bmp.Width() ||
                (int)e.strippedH != bmp.Height() ||
                (int)e.bpp       != bmp.Bpp())
                continue;
            // Guard against re-sharpening an already-full-res texture (e.g. a
            // second song re-uses a cached tex): only sharpen when the loaded
            // size is genuinely SMALLER than the carried full-res.
            if ((int)e.fullW <= bmp.Width() && (int)e.fullH <= bmp.Height())
                continue;
            e.tex = tex;
            gSession.matchedIdx.push_back(mit->second);
            matched++;
            break;
        }
    }

    SHARPEN_LOG("[sharpen] loaded %zu entries, matched %d to loaded RndTex (venue=%p)\n",
                gSession.entries.size(), matched, (void*)venueDir);

    if (matched == 0) {
        // Nothing to do — drop the session (frees the top-mip buffers).
        RB3SharpenReset();
        return 0;
    }
    return matched;
}

// ---------------------------------------------------------------------------
// Incremental swap + reupload.
// ---------------------------------------------------------------------------
int RB3SharpenStep(int maxThisFrame) {
    if (!gSession.active) return 0;
    if (maxThisFrame <= 0) maxThisFrame = 1;

    int doneThisCall = 0;
    while (gSession.nextToSharpen < (int)gSession.matchedIdx.size() &&
           doneThisCall < maxThisFrame) {
        int ei = gSession.matchedIdx[gSession.nextToSharpen++];
        SharpenEntry& e = gSession.entries[ei];
        if (!e.tex || e.sharpened) continue;

        RndBitmap& bmp = e.tex->mBitmap;

        // Reconstruct the full-res base level into an engine-owned buffer. The
        // sidecar's top-mip IS the original mip[0] (the full base level). We
        // allocate a fresh _MemAlloc'd buffer (32-byte aligned — the alignment
        // RndBitmap::AllocateBuffer/Create require, see Bitmap.cpp asserts 441/465),
        // copy the carried bytes in, then hand it to the bitmap as BOTH mBuffer
        // (so RndBitmap::Reset/dtor frees it via _MemFree — no leak) and mPixels.
        // The OLD stripped allocation is freed first. This makes the RndBitmap the
        // sole owner of the full-res buffer for the rest of the venue's lifetime;
        // the session keeps no reference, so RB3SharpenReset can never dangle it.
        //
        // DXT bitmaps have no palette (PaletteBytes()==0 for mOrder & 0x38), so the
        // pre-strip layout is buffer==pixels — we mirror that exactly.
        const size_t fullBytes = (size_t)e.fullRb * (size_t)e.fullH;
        u8* newBuf = (u8*)_MemAlloc((int)fullBytes, 32);
        if (!newBuf) {
            // OOM — leave this texture stripped, count it consumed, move on.
            e.sharpened = true;
            std::vector<uint8_t>().swap(e.topmip);
            doneThisCall++;
            continue;
        }
        std::memcpy(newBuf, e.topmip.data(), fullBytes);
        // Free the carried copy now that it's been duplicated into the owned buffer.
        std::vector<uint8_t>().swap(e.topmip);

        // Free the old stripped allocation (the milo loader's _MemAlloc'd mBuffer,
        // which == the old mPixels for these palette-free DXT bitmaps) and install
        // the full-res buffer. If mBuffer was somehow null/shared we still install
        // ours; the bitmap then owns exactly one buffer (no double free on teardown).
        if (bmp.mBuffer) {
            _MemFree(bmp.mBuffer);
            bmp.mBuffer = nullptr;
        }
        bmp.mBuffer   = newBuf;
        bmp.mPixels   = newBuf;
        bmp.mPalette  = nullptr;     // DXT: no palette
        bmp.mWidth    = (u16)e.fullW;
        bmp.mHeight   = (u16)e.fullH;
        bmp.mRowBytes = (u16)e.fullRb;
        // Keep RndTex's own mirror fields consistent (some paths read these).
        e.tex->mWidth  = e.fullW;
        e.tex->mHeight = e.fullH;

        // Re-invoke the upload: pixel pointer + fingerprint both changed → cache
        // miss → recreate at full size → new view. The cached material bind group
        // rebuilds on the next draw via its existing view-handle compare.
        bool recreated = RB3SharpenReuploadTex(e.tex);
        e.sharpened = true;
        gSession.sharpenedCount++;
        gSession.bytesUpgraded += fullBytes;
        doneThisCall++;

        SHARPEN_LOG("[sharpen] %s %dx%d -> %dx%d (recreate=%d) [%d/%zu]\n",
                    e.tex->Name() ? e.tex->Name() : "?",
                    e.strippedW, e.strippedH, e.fullW, e.fullH, (int)recreated,
                    gSession.sharpenedCount, gSession.matchedIdx.size());
    }

    if (gSession.nextToSharpen >= (int)gSession.matchedIdx.size() &&
        gSession.sharpenedCount > 0) {
        SHARPEN_LOG("[sharpen] session COMPLETE: %d textures, %llu bytes upgraded\n",
                    gSession.sharpenedCount,
                    (unsigned long long)gSession.bytesUpgraded);
    }
    return doneThisCall;
}

bool RB3SharpenComplete() {
    if (!gSession.active) return false;
    return gSession.nextToSharpen >= (int)gSession.matchedIdx.size();
}

void RB3SharpenReset() {
    // Clean teardown. A swapped-in entry already _MemAlloc'd its full-res buffer
    // INTO the RndBitmap (which owns + frees it in its own dtor) and freed the
    // carried `e.topmip` copy at swap time — so the session holds no buffer a live
    // bitmap depends on, and dropping it can never dangle a tex's mPixels. A
    // not-yet-swapped entry's `e.topmip` is freed here by the vector dtor. We do
    // NOT touch e.tex's bitmap (the venue owns it; it may already be torn down).
    gSession = SharpenSession();
}

RB3SharpenStatus RB3SharpenGetStatus() {
    RB3SharpenStatus s;
    s.active = gSession.active;
    s.matched = (int)gSession.matchedIdx.size();
    s.sharpened = gSession.sharpenedCount;
    s.total = (int)gSession.entries.size();
    s.bytesUpgraded = gSession.bytesUpgraded;
    return s;
}
