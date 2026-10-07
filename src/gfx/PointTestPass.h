// PointTestPass — retail DxRnd::DoPointTests' occlusion queries on WebGPU.
//
// Retail (rb3-xenon rnddx9/Rnd_Xbox.cpp) answers each queued flare test with
// two occlusion queries drawn after the world, against the world's depth:
// a one-pixel point and the flare's area rect, both at the tested point's
// projected depth, depth test LESS, depth and colour writes off, the viewport
// disabled (raw screen pixels). It reads the answers back at the next
// DoPointTests, one frame later, after blocking on the previous frame's fence.
//
// This pass does the same with a WebGPU occlusion query set: Record() draws a
// batch of tests as one depth-only render pass over the frame's depth target,
// resolves the query set and copies it to a mappable buffer; Submitted() maps
// that buffer once the frame's commands are submitted; Collect() hands back
// every finished batch's answers. A query's count is in samples, so the area
// answer is divided by the depth target's sample count.
//
// Rndobj-free: the caller (WgpuRnd) turns a consumer's NativePointTest into
// target pixels and a window-space depth.
#pragma once
#include <webgpu/webgpu_cpp.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class GpuDevice;

class PointTestPass {
public:
    struct Query {
        const void* key = nullptr;
        float px = 0, py = 0;                   // the point, in target pixels
        float rx = 0, ry = 0, rw = 0, rh = 0;   // the area rect, in target pixels
        float z = 0;                            // window-space depth, 0..1
        bool point = false;
        bool area = false;
        float areaScale = 1.0f;                 // visible target pixels -> reported area
    };
    struct Answer {
        const void* key = nullptr;
        bool pointDone = false;
        bool visible = false;
        bool areaDone = false;
        float area = 0.0f;
        uint64_t seq = 0;  // the batch's LastSeq() when it was recorded
    };
    typedef void (*AnswerFn)(const Answer& answer, void* user);

    PointTestPass();
    ~PointTestPass();

    // Draws `count` queries as one depth-tested pass over `depthView` (a
    // `depthFormat` attachment of `sampleCount` samples, w x h). Its contents
    // are loaded and kept. Returns false, recording nothing, when there is
    // nothing to test or every batch is still in flight.
    bool Record(wgpu::CommandEncoder& encoder, const wgpu::TextureView& depthView,
                wgpu::TextureFormat depthFormat, uint32_t sampleCount, uint32_t w, uint32_t h,
                const Query* queries, size_t count, GpuDevice& gpu);
    // The encoder that carried the last Record() was submitted: start reading
    // the answers back.
    void Submitted();
    // The encoder that carried the last Record() will never be submitted.
    void DiscardUnsubmitted();
    // Hands every batch whose readback has finished to `fn`, oldest first, then
    // frees it. With `wait`, first blocks (up to a second per batch) until each
    // submitted batch has mapped; without it takes what is ready. Returns the
    // number of answers delivered. CollectThrough(~0, wait).
    int Collect(bool wait, AnswerFn fn, void* user, GpuDevice& gpu);
    // As Collect, for the batches whose sequence number is at most `through`
    // (see LastSeq()) only: later batches are left in flight even when their
    // readback has finished. Under __EMSCRIPTEN__ it never blocks.
    int CollectThrough(uint64_t through, bool wait, AnswerFn fn, void* user, GpuDevice& gpu);
    // No answer for `key` is delivered after this returns.
    void Cancel(const void* key);
    // Batches recorded or being read back.
    int InFlight() const;
    // The sequence number of the last batch Record() accepted (0: none yet).
    uint64_t LastSeq() const { return mNextSeq - 1; }
    void Terminate(GpuDevice* gpu);

    static constexpr int kMaxBatches = 3;

private:
    struct Batch;
    bool EnsurePipelines(wgpu::TextureFormat depthFormat, uint32_t sampleCount, GpuDevice& gpu);

    std::vector<std::unique_ptr<Batch>> mBatches;
    uint64_t mNextSeq = 1;
    wgpu::ShaderModule mShader;
    wgpu::RenderPipeline mPointPipe;
    wgpu::RenderPipeline mAreaPipe;
    wgpu::TextureFormat mDepthFormat = wgpu::TextureFormat::Undefined;
    uint32_t mSampleCount = 0;
};
