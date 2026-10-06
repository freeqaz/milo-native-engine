// PointTestPass — see PointTestPass.h.
#include "gfx/PointTestPass.h"
#include "gfx/GpuDevice.h"

#include <cstdio>
#include <cstring>

namespace {

// Positions arrive in clip space with w = 1 (the caller maps target pixels to
// NDC and passes window depth as z, the viewport covering the whole target
// with depth range 0..1), as retail draws them with the viewport disabled.
// There is no fragment stage: the queries count samples that pass the depth
// test, and nothing is written.
const char* kShader = R"WGSL(
@vertex fn vs_point_test(@location(0) p: vec4f) -> @builtin(position) vec4f {
    return p;
}
)WGSL";

constexpr uint32_t kVertsPerQuery = 5;          // one point, then the area's strip
constexpr uint32_t kVertexBytes = 4 * sizeof(float);
constexpr uint32_t kQueriesPerTest = 2;         // point at 2i, area at 2i + 1
constexpr uint32_t kResultBytes = sizeof(uint64_t);

}  // namespace

struct PointTestPass::Batch {
    enum State { kFree, kRecorded, kMapping, kMapped, kFailed };
    State state = kFree;
    uint64_t seq = 0;
    uint32_t capacity = 0;
    uint32_t count = 0;
    uint32_t sampleCount = 1;
    wgpu::QuerySet querySet;
    wgpu::Buffer vertices;
    wgpu::Buffer resolve;
    wgpu::Buffer readback;
    wgpu::Future future{};
    std::vector<Query> queries;
};

PointTestPass::PointTestPass() {}
PointTestPass::~PointTestPass() {}

bool PointTestPass::EnsurePipelines(wgpu::TextureFormat depthFormat, uint32_t sampleCount,
                                    GpuDevice& gpu) {
    if (mPointPipe && mDepthFormat == depthFormat && mSampleCount == sampleCount) return true;
    wgpu::Device& dev = gpu.Device();
    if (!mShader) {
        wgpu::ShaderSourceWGSL src;
        src.code = kShader;
        wgpu::ShaderModuleDescriptor sm{};
        sm.nextInChain = &src;
        sm.label = "PointTest";
        mShader = dev.CreateShaderModule(&sm);
    }
    wgpu::VertexAttribute attr{};
    attr.format = wgpu::VertexFormat::Float32x4;
    attr.offset = 0;
    attr.shaderLocation = 0;
    wgpu::VertexBufferLayout vbl{};
    vbl.arrayStride = kVertexBytes;
    vbl.stepMode = wgpu::VertexStepMode::Vertex;
    vbl.attributeCount = 1;
    vbl.attributes = &attr;

    // Retail: ZEnable 1, ZWriteEnable 0, ZFunc LESS (mReverseZ is off),
    // ColorWriteEnable 0, alpha blend and alpha test off.
    wgpu::DepthStencilState ds{};
    ds.format = depthFormat;
    ds.depthWriteEnabled = wgpu::OptionalBool::False;
    ds.depthCompare = wgpu::CompareFunction::Less;
    ds.stencilReadMask = 0;
    ds.stencilWriteMask = 0;

    wgpu::RenderPipelineDescriptor pd{};
    pd.layout = nullptr;  // auto: the shader binds nothing
    pd.vertex.module = mShader;
    pd.vertex.entryPoint = "vs_point_test";
    pd.vertex.bufferCount = 1;
    pd.vertex.buffers = &vbl;
    pd.depthStencil = &ds;
    pd.multisample.count = sampleCount;
    pd.fragment = nullptr;

    pd.label = "PointTestPoint";
    pd.primitive.topology = wgpu::PrimitiveTopology::PointList;
    mPointPipe = dev.CreateRenderPipeline(&pd);
    pd.label = "PointTestArea";
    pd.primitive.topology = wgpu::PrimitiveTopology::TriangleStrip;
    mAreaPipe = dev.CreateRenderPipeline(&pd);
    mDepthFormat = depthFormat;
    mSampleCount = sampleCount;
    return mPointPipe && mAreaPipe;
}

bool PointTestPass::Record(wgpu::CommandEncoder& encoder, const wgpu::TextureView& depthView,
                           wgpu::TextureFormat depthFormat, uint32_t sampleCount, uint32_t w,
                           uint32_t h, const Query* queries, size_t count, GpuDevice& gpu) {
    if (!encoder || !depthView || !queries || count == 0 || w == 0 || h == 0) return false;
    if (!EnsurePipelines(depthFormat, sampleCount, gpu)) return false;

    Batch* b = nullptr;
    for (auto& it : mBatches) {
        if (it->state == Batch::kFree) {
            b = it.get();
            break;
        }
    }
    if (!b) {
        if ((int)mBatches.size() >= kMaxBatches) return false;
        mBatches.emplace_back(new Batch);
        b = mBatches.back().get();
    }

    wgpu::Device& dev = gpu.Device();
    if (b->capacity < count) {
        uint32_t cap = 32;
        while (cap < count) cap *= 2;
        wgpu::QuerySetDescriptor qd{};
        qd.label = "PointTestQueries";
        qd.type = wgpu::QueryType::Occlusion;
        qd.count = cap * kQueriesPerTest;
        b->querySet = dev.CreateQuerySet(&qd);

        wgpu::BufferDescriptor bd{};
        bd.label = "PointTestVertices";
        bd.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        bd.size = (uint64_t)cap * kVertsPerQuery * kVertexBytes;
        b->vertices = dev.CreateBuffer(&bd);
        bd.label = "PointTestResolve";
        bd.usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc;
        bd.size = (uint64_t)cap * kQueriesPerTest * kResultBytes;
        b->resolve = dev.CreateBuffer(&bd);
        bd.label = "PointTestReadback";
        bd.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
        b->readback = dev.CreateBuffer(&bd);
        b->capacity = cap;
    }

    // Target pixels -> clip space over the whole target (y down on screen).
    const float sx = 2.0f / (float)w, sy = 2.0f / (float)h;
    std::vector<float> verts((size_t)count * kVertsPerQuery * 4, 0.0f);
    for (size_t i = 0; i < count; i++) {
        const Query& q = queries[i];
        float* v = &verts[i * kVertsPerQuery * 4];
        const float corners[5][2] = {
            {q.px, q.py},
            {q.rx, q.ry},
            {q.rx, q.ry + q.rh},
            {q.rx + q.rw, q.ry},
            {q.rx + q.rw, q.ry + q.rh},
        };
        for (int c = 0; c < 5; c++) {
            v[c * 4 + 0] = corners[c][0] * sx - 1.0f;
            v[c * 4 + 1] = 1.0f - corners[c][1] * sy;
            v[c * 4 + 2] = q.z;
            v[c * 4 + 3] = 1.0f;
        }
    }
    gpu.Queue().WriteBuffer(b->vertices, 0, verts.data(), verts.size() * sizeof(float));

    wgpu::RenderPassDepthStencilAttachment da{};
    da.view = depthView;
    da.depthLoadOp = wgpu::LoadOp::Load;
    da.depthStoreOp = wgpu::StoreOp::Store;
    da.stencilLoadOp = wgpu::LoadOp::Load;
    da.stencilStoreOp = wgpu::StoreOp::Store;
    wgpu::RenderPassDescriptor rp{};
    rp.label = "PointTestPass";
    rp.depthStencilAttachment = &da;
    rp.occlusionQuerySet = b->querySet;
    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&rp);
    pass.SetViewport(0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f);
    pass.SetVertexBuffer(0, b->vertices, 0, (uint64_t)count * kVertsPerQuery * kVertexBytes);
    pass.SetPipeline(mPointPipe);
    for (uint32_t i = 0; i < count; i++) {
        if (!queries[i].point) continue;
        pass.BeginOcclusionQuery(i * kQueriesPerTest);
        pass.Draw(1, 1, i * kVertsPerQuery, 0);
        pass.EndOcclusionQuery();
    }
    pass.SetPipeline(mAreaPipe);
    for (uint32_t i = 0; i < count; i++) {
        if (!queries[i].area) continue;
        pass.BeginOcclusionQuery(i * kQueriesPerTest + 1);
        pass.Draw(4, 1, i * kVertsPerQuery + 1, 0);
        pass.EndOcclusionQuery();
    }
    pass.End();

    const uint32_t nq = (uint32_t)count * kQueriesPerTest;
    encoder.ResolveQuerySet(b->querySet, 0, nq, b->resolve, 0);
    encoder.CopyBufferToBuffer(b->resolve, 0, b->readback, 0, (uint64_t)nq * kResultBytes);

    b->queries.assign(queries, queries + count);
    b->count = (uint32_t)count;
    b->sampleCount = sampleCount ? sampleCount : 1;
    b->seq = mNextSeq++;
    b->state = Batch::kRecorded;
    return true;
}

void PointTestPass::Submitted() {
    for (auto& it : mBatches) {
        Batch* b = it.get();
        if (b->state != Batch::kRecorded) continue;
        b->state = Batch::kMapping;
        b->future = b->readback.MapAsync(
            wgpu::MapMode::Read, 0, (uint64_t)b->count * kQueriesPerTest * kResultBytes,
            wgpu::CallbackMode::AllowProcessEvents,
            [b](wgpu::MapAsyncStatus status, wgpu::StringView) {
                b->state = status == wgpu::MapAsyncStatus::Success ? Batch::kMapped
                                                                    : Batch::kFailed;
            });
    }
}

void PointTestPass::DiscardUnsubmitted() {
    for (auto& it : mBatches) {
        if (it->state == Batch::kRecorded) {
            it->state = Batch::kFree;
            it->queries.clear();
        }
    }
}

int PointTestPass::Collect(bool wait, AnswerFn fn, void* user, GpuDevice& gpu) {
    if (mBatches.empty()) return 0;
    wgpu::Instance& inst = gpu.Instance();
    if (wait) {
        for (auto& it : mBatches) {
            if (it->state != Batch::kMapping) continue;
            wgpu::WaitStatus ws = inst.WaitAny(it->future, 1000000000ull);
            if (ws != wgpu::WaitStatus::Success && it->state == Batch::kMapping) {
                static int warned = 0;
                if (warned++ < 3)
                    fprintf(stderr, "PointTestPass: query readback not ready (wait status %d)\n",
                            (int)ws);
            }
        }
    } else if (inst) {
        inst.ProcessEvents();
    }

    int delivered = 0;
    for (;;) {
        // Oldest finished batch first.
        Batch* b = nullptr;
        for (auto& it : mBatches) {
            if ((it->state == Batch::kMapped || it->state == Batch::kFailed) &&
                (!b || it->seq < b->seq))
                b = it.get();
        }
        if (!b) break;
        if (b->state == Batch::kMapped) {
            const uint64_t bytes = (uint64_t)b->count * kQueriesPerTest * kResultBytes;
            const uint64_t* r = static_cast<const uint64_t*>(b->readback.GetConstMappedRange(0, bytes));
            for (uint32_t i = 0; r && i < b->count; i++) {
                const Query& q = b->queries[i];
                if (!q.key || (!q.point && !q.area)) continue;
                Answer a;
                a.key = q.key;
                a.pointDone = q.point;
                a.visible = q.point && r[i * kQueriesPerTest] != 0;
                a.areaDone = q.area;
                a.area = q.area ? (float)((double)r[i * kQueriesPerTest + 1] / b->sampleCount) *
                                      q.areaScale
                                : 0.0f;
                if (fn) fn(a, user);
                delivered++;
            }
            b->readback.Unmap();
        }
        b->state = Batch::kFree;
        b->queries.clear();
    }
    return delivered;
}

void PointTestPass::Cancel(const void* key) {
    if (!key) return;
    for (auto& it : mBatches)
        for (Query& q : it->queries)
            if (q.key == key) q.key = nullptr;
}

int PointTestPass::InFlight() const {
    int n = 0;
    for (const auto& it : mBatches)
        if (it->state != Batch::kFree) n++;
    return n;
}

void PointTestPass::Terminate(GpuDevice* gpu) {
    // A pending map's callback captures its batch: abort every pending map and
    // let the callbacks run while the batches still exist.
    bool pending = false;
    for (auto& it : mBatches) {
        if (it->state == Batch::kMapping || it->state == Batch::kMapped) {
            it->readback.Unmap();
            pending = true;
        }
    }
    if (pending && gpu && gpu->Instance()) gpu->Instance().ProcessEvents();
    mBatches.clear();
    mPointPipe = nullptr;
    mAreaPipe = nullptr;
    mShader = nullptr;
    mDepthFormat = wgpu::TextureFormat::Undefined;
    mSampleCount = 0;
}

// Shipped WGSL accessor (gfx/ShippedWgsl.h).
#include "gfx/ShippedWgsl.h"
const char* PointTestPassWgslSource() { return kShader; }
