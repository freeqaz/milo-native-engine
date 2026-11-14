// Bounded cache of bind groups, keyed by everything their descriptor binds.
//
// Why: the mesh path built a fresh wgpu::BindGroup for its material, its
// object uniforms and its bones on every draw (WgpuRnd::CreateMaterialBindGroup
// and friends). Creating one is a validation pass, a descriptor-set allocation
// and a write of every binding -- measured 14-22% of dc3-native's main thread
// (PLAN H-30). The resources behind a material's group (its texture views,
// samplers, the uniform ring buffer) are the same frame after frame; only the
// uniform data moves, and that is now bound with a dynamic offset.
//
// The key is the descriptor itself: the layout, and every field of every
// entry (binding, buffer, offset, size, sampler, texture view). Two equal
// keys therefore describe the same bind group, so a hit returns exactly what a
// CreateBindGroup with that descriptor would have made -- whatever per-draw
// value (phase, environ, material) chose those resources. Values that live in
// uniform DATA (gammaShading, lights, the object block) are not in the key and
// need not be: they are written to the ring per draw and reached through the
// dynamic offset.
//
// Handles as key words are safe from address reuse: every entry keeps its own
// references to the layout and to every resource it binds, so no handle in a
// live key can be freed and its address handed to a new object.
//
// Bounds: an entry not used for kMaxAgeFrames frames is dropped (swept every
// kSweepInterval frames), Forget() drops every entry that binds a resource
// that is being released (a texture's view when the RndTex dies, a render
// target's view when it is recreated), and the cache never holds more than
// kMaxEntries (on overflow, everything unused this frame goes, then
// everything). Clear() on Terminate.
#pragma once

#include <webgpu/webgpu_cpp.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class BindGroupCache {
public:
    static constexpr uint32_t kMaxBindings = 16;
    // layout + per entry {binding, buffer, offset, size, sampler, textureView}
    static constexpr uint32_t kWordsPerEntry = 6;
    static constexpr uint32_t kMaxWords = 1 + kMaxBindings * kWordsPerEntry;

    // Raw view of one descriptor entry: exactly the fields wgpu::BindGroupEntry
    // carries (no chained structs -- MakeKey refuses those).
    struct EntryRef {
        uint32_t binding = 0;
        const void* buffer = nullptr;
        uint64_t offset = 0;
        uint64_t size = 0;
        const void* sampler = nullptr;
        const void* textureView = nullptr;
    };

    struct Key {
        std::array<uint64_t, kMaxWords> words{};
        uint32_t count = 0;
        bool operator==(const Key& o) const {
            return count == o.count &&
                   std::memcmp(words.data(), o.words.data(), count * sizeof(uint64_t)) == 0;
        }
        bool operator!=(const Key& o) const { return !(*this == o); }
    };

    struct KeyHash {
        size_t operator()(const Key& k) const {
            uint64_t h = 0xcbf29ce484222325ULL ^ k.count;
            for (uint32_t i = 0; i < k.count; i++) {
                h ^= k.words[i] + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
                h *= 0x100000001b3ULL;
            }
            return (size_t)(h ^ (h >> 29));
        }
    };

    // The key of a descriptor given as raw pointers. False when it cannot be
    // keyed (too many entries): the caller then creates the group uncached.
    static bool MakeKey(const void* layout, const EntryRef* entries, uint32_t count, Key& out) {
        if (count > kMaxBindings) return false;
        out.count = 1 + count * kWordsPerEntry;
        out.words[0] = (uint64_t)(uintptr_t)layout;
        for (uint32_t i = 0; i < count; i++) {
            // Every field of the entry, so equal keys are equal descriptors.
            const EntryRef& e = entries[i];
            uint64_t* w = &out.words[1 + i * kWordsPerEntry];
            w[0] = e.binding;
            w[1] = (uint64_t)(uintptr_t)e.buffer;
            w[2] = e.offset;
            w[3] = e.size;
            w[4] = (uint64_t)(uintptr_t)e.sampler;
            w[5] = (uint64_t)(uintptr_t)e.textureView;
        }
        for (uint32_t i = out.count; i < kMaxWords; i++) out.words[i] = 0;
        return true;
    }

    static bool MakeKey(const wgpu::BindGroupDescriptor& desc, Key& out) {
        if (desc.nextInChain || desc.entryCount > kMaxBindings) return false;
        EntryRef refs[kMaxBindings];
        for (size_t i = 0; i < desc.entryCount; i++) {
            const wgpu::BindGroupEntry& e = desc.entries[i];
            if (e.nextInChain) return false;  // e.g. an external texture
            refs[i].binding = e.binding;
            refs[i].buffer = e.buffer.Get();
            refs[i].offset = e.offset;
            refs[i].size = e.size;
            refs[i].sampler = e.sampler.Get();
            refs[i].textureView = e.textureView.Get();
        }
        return MakeKey(desc.layout.Get(), refs, (uint32_t)desc.entryCount, out);
    }

    // The cached group for `desc`, or a new one from `create(desc)` (normally
    // device.CreateBindGroup). The creator is a parameter so the key and
    // eviction rules can be tested without a GPU.
    template <class CreateFn>
    wgpu::BindGroup GetOrCreate(const wgpu::BindGroupDescriptor& desc, CreateFn&& create) {
        Key key;
        if (!MakeKey(desc, key)) {
            ++mUncached;
            return create(desc);
        }
        auto it = mEntries.find(key);
        if (it != mEntries.end()) {
            it->second.lastUsed = mFrame;
            ++mHits;
            return it->second.group;
        }
        if (mEntries.size() >= kMaxEntries) {
            SweepOlderThan(mFrame);  // everything not used this frame
            if (mEntries.size() >= kMaxEntries) {
                mEvictions += mEntries.size();
                mEntries.clear();
            }
        }
        Entry entry;
        entry.group = create(desc);
        entry.layout = desc.layout;
        entry.bindings.assign(desc.entries, desc.entries + desc.entryCount);
        entry.lastUsed = mFrame;
        wgpu::BindGroup group = entry.group;
        mEntries.emplace(key, std::move(entry));
        ++mCreates;
        return group;
    }

    wgpu::BindGroup GetOrCreate(wgpu::Device& device, const wgpu::BindGroupDescriptor& desc) {
        return GetOrCreate(desc, [&device](const wgpu::BindGroupDescriptor& d) {
            return device.CreateBindGroup(&d);
        });
    }

    // Once per frame, before the frame's draws: ages entries and drops the
    // ones whose resources were Forget()-ed since the last call.
    void BeginFrame(uint64_t frame) {
        mFrame = frame;
        if (!mForgotten.empty()) {
            for (auto it = mEntries.begin(); it != mEntries.end();) {
                if (Binds(it->second, mForgotten)) {
                    it = mEntries.erase(it);
                    ++mEvictions;
                } else {
                    ++it;
                }
            }
            mForgotten.clear();
        }
        if (frame >= mLastSweep + kSweepInterval) {
            mLastSweep = frame;
            if (frame > kMaxAgeFrames) SweepOlderThan(frame - kMaxAgeFrames);
        }
    }

    // A resource (texture view, sampler or buffer handle) is being released:
    // every entry binding it goes at the next BeginFrame. Until then the entry
    // keeps the resource alive and a lookup can only find it through the same
    // live handle, so a draw in between still gets a correct group.
    void Forget(const void* handle) {
        if (handle) mForgotten.insert(handle);
    }

    void Clear() {
        mEntries.clear();
        mForgotten.clear();
    }

    size_t Size() const { return mEntries.size(); }
    uint64_t Creates() const { return mCreates; }
    uint64_t Hits() const { return mHits; }
    uint64_t Evictions() const { return mEvictions; }
    uint64_t Uncached() const { return mUncached; }

    static constexpr size_t kMaxEntries = 8192;
    static constexpr uint64_t kMaxAgeFrames = 120;
    static constexpr uint64_t kSweepInterval = 60;

private:
    struct Entry {
        wgpu::BindGroup group;
        // References that pin every handle in the key (see the header note).
        wgpu::BindGroupLayout layout;
        std::vector<wgpu::BindGroupEntry> bindings;
        uint64_t lastUsed = 0;
    };

    static bool Binds(const Entry& e, const std::unordered_set<const void*>& handles) {
        for (const wgpu::BindGroupEntry& b : e.bindings) {
            if ((b.buffer && handles.count(b.buffer.Get())) ||
                (b.sampler && handles.count(b.sampler.Get())) ||
                (b.textureView && handles.count(b.textureView.Get())))
                return true;
        }
        return false;
    }

    void SweepOlderThan(uint64_t frame) {
        for (auto it = mEntries.begin(); it != mEntries.end();) {
            if (it->second.lastUsed < frame) {
                it = mEntries.erase(it);
                ++mEvictions;
            } else {
                ++it;
            }
        }
    }

    std::unordered_map<Key, Entry, KeyHash> mEntries;
    std::unordered_set<const void*> mForgotten;
    uint64_t mFrame = 0;
    uint64_t mLastSweep = 0;
    uint64_t mCreates = 0;
    uint64_t mHits = 0;
    uint64_t mEvictions = 0;
    uint64_t mUncached = 0;
};
