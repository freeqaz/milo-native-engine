// GPU time per render pass. See GpuTimers.h.

#include "gfx/GpuTimers.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

bool GpuTimers::sActive = false;

GpuTimers& GpuTimers::Get() {
    // Leaked on purpose: a map callback must never find it destroyed at exit.
    static GpuTimers* sInstance = new GpuTimers();
    return *sInstance;
}

bool GpuTimers::Requested() {
    static const bool sRequested = [] {
        const char* v = getenv("MILO_GPU_TIMERS");
        if (!v || !*v) return false;
        if (v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F') return false;
        if ((v[0] == 'o' || v[0] == 'O') && (v[1] == 'f' || v[1] == 'F')) return false;
        return true;
    }();
    return sRequested;
}

static double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void GpuTimers::NoteUnavailable(const char* why) {
    if (!Requested()) return;
    std::lock_guard<std::mutex> lock(mSnapMutex);
    if (mSnap.unavailableReason.empty()) {
        fprintf(stderr, "GPU timers unavailable: %s\n", why && *why ? why : "unknown reason");
    }
    mSnap.active = false;
    mSnap.unavailableReason = why && *why ? why : "unknown reason";
}

void GpuTimers::SetWindowSeconds(double s) { mWindowSeconds = s > 0.0 ? s : 1.0; }

#ifdef __EMSCRIPTEN__

// The browser build does not wire timestamp queries (GpuDevice_Web requests
// no timestamp-query feature, and the emscripten WebGPU headers are a
// different revision of the API). Everything is a no-op; Active() stays false.
struct GpuTimers::Impl {};
void GpuTimers::Init(const wgpu::Device&, const wgpu::Instance&, bool, const char* whyNot, int) {
    NoteUnavailable(whyNot && *whyNot ? whyNot : "not implemented in the web build");
}
void GpuTimers::Shutdown() { sActive = false; }
void GpuTimers::InstrumentPass(wgpu::RenderPassDescriptor&, const char*, int) {}
void GpuTimers::EndFrame(const wgpu::Queue&) {}
void GpuTimers::Collect() {}
void GpuTimers::AccumulateFrame(int) {}
void GpuTimers::PublishWindow(double) {}

#else

namespace {
constexpr int kSlots = 4;          // readback buffers in flight
constexpr int kMaxPasses = 256;    // timed passes per frame (2 queries each)
constexpr uint64_t kBytesPerPass = 2 * sizeof(uint64_t);

struct Acc {
    double sum = 0.0;   // over timed frames, of the frame's sum for this name
    double max = 0.0;
    long long count = 0;
};
}  // namespace

struct GpuTimers::Impl {
    enum class State { Idle, Recording, Mapping, Ready, Failed };
    struct Slot {
        wgpu::QuerySet querySet;
        wgpu::Buffer readback;
        State state = State::Idle;
        int count = 0;
        std::vector<std::string> names;
    };

    wgpu::Device device;
    wgpu::Instance instance;
    wgpu::Buffer resolve;
    Slot slots[kSlots];
    int cur = 0;
    int generation = 0;
    wgpu::PassTimestampWrites writes[kMaxPasses];

    // This frame.
    bool frameDecided = false;
    bool frameTimed = false;
    bool frameOverflow = false;

    // This window.
    std::map<std::string, Acc> acc;
    int frames = 0;
    int skipped = 0;
    long long invalid = 0;  // pass pairs that resolved to 0 or ran backwards
    double sumAcc = 0.0, sumMax = 0.0, spanAcc = 0.0, spanMax = 0.0;
    uint64_t lowBitsOr = 0;  // OR of every timestamp: trailing zeros = quantization
    long long samples = 0;
    double windowStart = 0.0;
    long long windowIndex = 0;
};

static int sGeneration = 0;

void GpuTimers::Init(const wgpu::Device& device, const wgpu::Instance& instance,
                     bool featureEnabled, const char* whyNot, int quantizationOff) {
    Shutdown();
    if (!Requested()) return;
    if (!featureEnabled || !device) {
        NoteUnavailable(whyNot && *whyNot ? whyNot : "no device with timestamp-query");
        return;
    }
    Impl* im = new Impl();
    im->device = device;
    im->instance = instance;
    im->generation = ++sGeneration;

    wgpu::BufferDescriptor rd{};
    rd.label = "GpuTimersResolve";
    rd.size = kBytesPerPass * kMaxPasses;
    rd.usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc;
    im->resolve = device.CreateBuffer(&rd);
    for (int i = 0; i < kSlots; ++i) {
        wgpu::QuerySetDescriptor qd{};
        qd.label = "GpuTimersQueries";
        qd.type = wgpu::QueryType::Timestamp;
        qd.count = 2 * kMaxPasses;
        im->slots[i].querySet = device.CreateQuerySet(&qd);
        wgpu::BufferDescriptor bd{};
        bd.label = "GpuTimersReadback";
        bd.size = kBytesPerPass * kMaxPasses;
        bd.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
        im->slots[i].readback = device.CreateBuffer(&bd);
        im->slots[i].names.reserve(64);
    }
    if (!im->resolve || !im->slots[0].querySet) {
        delete im;
        NoteUnavailable("query set / buffer creation failed");
        return;
    }
    im->windowStart = NowSeconds();
    mImpl = im;
    {
        std::lock_guard<std::mutex> lock(mSnapMutex);
        mSnap = Snapshot();
        mSnap.active = true;
        mSnap.quantizationOff = quantizationOff;
    }
    sActive = true;
    fprintf(stderr,
            "GPU timers ON (MILO_GPU_TIMERS): %d passes/frame max, %d readbacks in flight, "
            "timestamp_quantization %s; [GPUTIME] lines every %.1f s\n",
            kMaxPasses, kSlots,
            quantizationOff > 0    ? "off (device confirms)"
            : quantizationOff == 0 ? "ON (the device still reports it enabled)"
                                   : "off (requested; the device's toggles cannot be read)",
            mWindowSeconds);
}

void GpuTimers::Shutdown() {
    sActive = false;
    if (!mImpl) return;
    Impl* im = mImpl;
    mImpl = nullptr;  // a late map callback checks this (and the generation)
    for (Impl::Slot& s : im->slots) {
        if (s.state == Impl::State::Ready) s.readback.Unmap();
    }
    delete im;
    std::lock_guard<std::mutex> lock(mSnapMutex);
    mSnap.active = false;
}

void GpuTimers::InstrumentPass(wgpu::RenderPassDescriptor& desc, const char* file, int line) {
    Impl* im = mImpl;
    if (!im) {
        desc.timestampWrites = nullptr;
        return;
    }
    Impl::Slot& s = im->slots[im->cur];
    if (!im->frameDecided) {
        im->frameDecided = true;
        im->frameOverflow = false;
        im->frameTimed = (s.state == Impl::State::Idle);
        if (im->frameTimed) {
            s.state = Impl::State::Recording;
            s.count = 0;
            s.names.clear();
        }
    }
    if (!im->frameTimed || s.count >= kMaxPasses) {
        if (im->frameTimed) im->frameOverflow = true;
        desc.timestampWrites = nullptr;
        return;
    }
    const int i = s.count++;
    wgpu::PassTimestampWrites& w = im->writes[i];
    w.querySet = s.querySet;
    w.beginningOfPassWriteIndex = 2 * i;
    w.endOfPassWriteIndex = 2 * i + 1;
    desc.timestampWrites = &w;

    const char* data = desc.label.data;
    size_t len = desc.label.length;
    if (data && len == WGPU_STRLEN) len = strlen(data);
    if (data && len > 0) {
        s.names.emplace_back(data, len);
    } else {
        const char* base = file ? strrchr(file, '/') : nullptr;
        base = base ? base + 1 : (file ? file : "?");
        char buf[160];
        snprintf(buf, sizeof(buf), "unnamed@%s:%d", base, line);
        s.names.emplace_back(buf);
    }
}

void GpuTimers::EndFrame(const wgpu::Queue& queue) {
    Impl* im = mImpl;
    if (!sActive || !im) return;
    Impl::Slot& s = im->slots[im->cur];
    if (im->frameDecided) {
        if (!im->frameTimed || im->frameOverflow) im->skipped++;
        if (im->frameTimed && s.count > 0 && !im->frameOverflow) {
            const uint64_t bytes = kBytesPerPass * (uint64_t)s.count;
            wgpu::CommandEncoderDescriptor ed{};
            ed.label = "GpuTimersResolve";
            wgpu::CommandEncoder enc = im->device.CreateCommandEncoder(&ed);
            enc.ResolveQuerySet(s.querySet, 0, 2 * (uint32_t)s.count, im->resolve, 0);
            enc.CopyBufferToBuffer(im->resolve, 0, s.readback, 0, bytes);
            wgpu::CommandBuffer cmd = enc.Finish();
            queue.Submit(1, &cmd);
            s.state = Impl::State::Mapping;
            const int slot = im->cur, gen = im->generation;
            s.readback.MapAsync(
                wgpu::MapMode::Read, 0, bytes, wgpu::CallbackMode::AllowProcessEvents,
                [this, slot, gen](wgpu::MapAsyncStatus status, wgpu::StringView) {
                    Impl* cur = mImpl;
                    if (!cur || cur->generation != gen) return;
                    cur->slots[slot].state = status == wgpu::MapAsyncStatus::Success
                                                 ? Impl::State::Ready
                                                 : Impl::State::Failed;
                });
            im->cur = (im->cur + 1) % kSlots;
        } else if (im->frameTimed) {
            s.state = Impl::State::Idle;  // overflowed or no pass recorded: drop it
        }
        im->frameDecided = false;
    }
    if (im->instance) im->instance.ProcessEvents();
    Collect();
    const double now = NowSeconds();
    if (now - im->windowStart >= mWindowSeconds) PublishWindow(now);
}

void GpuTimers::Collect() {
    Impl* im = mImpl;
    if (!im) return;
    // Oldest first: slots are filled round-robin starting after `cur`.
    for (int k = 0; k < kSlots; ++k) {
        int i = (im->cur + k) % kSlots;
        Impl::Slot& s = im->slots[i];
        if (s.state == Impl::State::Ready) {
            AccumulateFrame(i);
            s.readback.Unmap();
            s.state = Impl::State::Idle;
        } else if (s.state == Impl::State::Failed) {
            s.state = Impl::State::Idle;
        }
    }
}

void GpuTimers::AccumulateFrame(int slot) {
    Impl* im = mImpl;
    Impl::Slot& s = im->slots[slot];
    const uint64_t* ts = static_cast<const uint64_t*>(
        s.readback.GetConstMappedRange(0, kBytesPerPass * (uint64_t)s.count));
    if (!ts) return;
    std::map<std::string, std::pair<double, int>> frame;
    double sum = 0.0;
    uint64_t first = UINT64_MAX, last = 0;
    int valid = 0;
    for (int i = 0; i < s.count; ++i) {
        const uint64_t b = ts[2 * i], e = ts[2 * i + 1];
        if (b == 0 || e == 0 || e < b) {
            im->invalid++;
            continue;
        }
        im->lowBitsOr |= b | e;
        im->samples += 2;
        const double ms = (double)(e - b) / 1.0e6;  // WebGPU timestamps are ns
        auto& f = frame[s.names[i]];
        f.first += ms;
        f.second += 1;
        sum += ms;
        first = std::min(first, b);
        last = std::max(last, e);
        valid++;
    }
    if (valid == 0) return;
    const double span = (double)(last - first) / 1.0e6;
    im->frames++;
    im->sumAcc += sum;
    im->sumMax = std::max(im->sumMax, sum);
    im->spanAcc += span;
    im->spanMax = std::max(im->spanMax, span);
    for (auto& [name, v] : frame) {
        Acc& a = im->acc[name];
        a.sum += v.first;
        a.max = std::max(a.max, v.first);
        a.count += v.second;
    }
}

void GpuTimers::PublishWindow(double now) {
    Impl* im = mImpl;
    Snapshot snap;
    snap.active = true;
    {
        std::lock_guard<std::mutex> lock(mSnapMutex);
        snap.quantizationOff = mSnap.quantizationOff;
    }
    snap.frames = im->frames;
    snap.skipped = im->skipped;
    snap.windowSeconds = now - im->windowStart;
    snap.windowIndex = ++im->windowIndex;
    if (im->frames > 0) {
        snap.sumMeanMs = im->sumAcc / im->frames;
        snap.sumMaxMs = im->sumMax;
        snap.spanMeanMs = im->spanAcc / im->frames;
        snap.spanMaxMs = im->spanMax;
        for (auto& [name, a] : im->acc) {
            PassStat p;
            p.name = name;
            p.meanMs = a.sum / im->frames;
            p.maxMs = a.max;
            p.meanCount = (double)a.count / im->frames;
            snap.passes.push_back(std::move(p));
        }
        std::sort(snap.passes.begin(), snap.passes.end(),
                  [](const PassStat& a, const PassStat& b) { return a.meanMs > b.meanMs; });
    }
    // Low bits that were zero in every timestamp of the window: ~16 or more
    // means the timestamps are quantized (Dawn's timestamp_quantization).
    int zeroLow = 0;
    if (im->samples > 0) {
        uint64_t v = im->lowBitsOr;
        while (zeroLow < 64 && !(v & 1)) {
            v >>= 1;
            zeroLow++;
        }
    }
    fprintf(stderr,
            "[GPUTIME] window=%.2fs frames=%d skipped=%d invalid=%lld gpu_sum_ms=%.3f "
            "gpu_sum_max=%.3f gpu_span_ms=%.3f gpu_span_max=%.3f passes=%zu ts_zero_low_bits=%d\n",
            snap.windowSeconds, snap.frames, snap.skipped, im->invalid, snap.sumMeanMs,
            snap.sumMaxMs, snap.spanMeanMs, snap.spanMaxMs, snap.passes.size(), zeroLow);
    for (const PassStat& p : snap.passes) {
        fprintf(stderr, "[GPUTIME] pass=%s ms=%.4f max=%.4f n=%.2f\n", p.name.c_str(), p.meanMs,
                p.maxMs, p.meanCount);
    }
    {
        std::lock_guard<std::mutex> lock(mSnapMutex);
        mSnap = std::move(snap);
    }
    im->acc.clear();
    im->frames = 0;
    im->skipped = 0;
    im->invalid = 0;
    im->sumAcc = im->sumMax = im->spanAcc = im->spanMax = 0.0;
    im->lowBitsOr = 0;
    im->samples = 0;
    im->windowStart = now;
}

#endif  // __EMSCRIPTEN__

GpuTimers::Snapshot GpuTimers::Latest() const {
    std::lock_guard<std::mutex> lock(mSnapMutex);
    return mSnap;
}

static void JsonEscape(std::string& out, const std::string& s) {
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if ((unsigned char)c < 0x20) {
            char b[8];
            snprintf(b, sizeof(b), "\\u%04x", (unsigned char)c);
            out += b;
        } else {
            out += c;
        }
    }
}

std::string GpuTimers::LatestJson() const {
    Snapshot s = Latest();
    std::string out;
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"requested\":%s,\"active\":%s,\"quantizationOff\":%d,\"window\":%lld,"
             "\"windowSeconds\":%.3f,\"frames\":%d,\"skipped\":%d,\"gpuSumMs\":%.4f,"
             "\"gpuSumMaxMs\":%.4f,\"gpuSpanMs\":%.4f,\"gpuSpanMaxMs\":%.4f,\"unavailable\":\"",
             Requested() ? "true" : "false", s.active ? "true" : "false",
             s.quantizationOff, s.windowIndex, s.windowSeconds, s.frames,
             s.skipped, s.sumMeanMs, s.sumMaxMs, s.spanMeanMs, s.spanMaxMs);
    out += buf;
    JsonEscape(out, s.unavailableReason);
    out += "\",\"passes\":[";
    for (size_t i = 0; i < s.passes.size(); ++i) {
        const PassStat& p = s.passes[i];
        out += i ? ",{\"name\":\"" : "{\"name\":\"";
        JsonEscape(out, p.name);
        snprintf(buf, sizeof(buf), "\",\"ms\":%.4f,\"maxMs\":%.4f,\"n\":%.2f}", p.meanMs, p.maxMs,
                 p.meanCount);
        out += buf;
    }
    out += "]}";
    return out;
}
