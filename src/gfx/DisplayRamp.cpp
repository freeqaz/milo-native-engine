// DisplayRamp — see DisplayRamp.h.
#include "gfx/DisplayRamp.h"
#include "gfx/BindGroupCount.h"
#include "gfx/GpuTimers.h"
#include "gfx/GpuDevice.h"

#include <cmath>
#include <cstdio>
#include "gfx/PipelineCount.h"

namespace {

// The console's float order (gamma.s): every single-precision op rounds to
// float, fmsubs is one fused rounding (std::fma), pow is the float overload
// ?pow@@YAMMM@Z, modelled as (float)pow(double, double). One op per
// statement so nothing is contracted into an FMA the console does not use.
float PowF(float x, float y) { return (float)std::pow((double)x, (double)y); }

// D3D::Gamma_sRGB(x, decode)
float GammaSRGB(float x, bool decode) {
    if (decode) {
        if ((double)x > 0.04045) {
            const float a = x + 0.055f;
            const float b = a * 0.94786733f;   // 1 / 1.055
            return PowF(b, 2.4f);
        }
        return x * 0.07739938f;                // 1 / 12.92
    }
    if ((double)x > 0.0031308)
        return std::fma(PowF(x, 0.41666666f), 1.055f, -0.055f);
    return x * 12.92f;
}

// D3D::Gamma_TV(x, decode): BT.709
float GammaTV(float x, bool decode) {
    if (decode) {
        if ((double)x >= 0.0812) {
            const float a = x + 0.099f;
            const float b = a * 0.9099181f;    // 1 / 1.099
            return PowF(b, 2.2222223f);        // 1 / 0.45
        }
        return x * 0.22222222f;                // 1 / 4.5
    }
    if ((double)x >= 0.018)
        return std::fma(PowF(x, 0.45f), 1.099f, -0.099f);
    return x * 4.5f;
}

// D3D::Gamma_Standard(x, power, decode)
float GammaStandard(float x, float power, bool decode) {
    if (decode) return PowF(x, power);
    const float inv = 1.0f / power;
    return PowF(x, inv);
}

const char* kShader = R"WGSL(
override kGamma: f32 = 1.0;

@group(0) @binding(0) var srcTex: texture_2d<f32>;
// fs_table only: entry i of the corrected table x 64 (a 16-bit ramp entry),
// four to a vec4.
@group(0) @binding(1) var<uniform> lut: array<vec4f, 64>;

@vertex fn vs_full(@builtin(vertex_index) idx: u32) -> @builtin(position) vec4f {
    let x = f32(i32(idx & 1u)) * 4.0 - 1.0;
    let y = f32(i32(idx >> 1u)) * 4.0 - 1.0;
    return vec4f(x, y, 0.0, 1.0);
}

// DxRnd::SetupGamma's table, entry i = (u16)(pow(i / 256, g) * 1024) * 64.
@fragment fn fs_ramp(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let c = textureLoad(srcTex, vec2i(pos.xy), 0);
    let i = round(clamp(c.rgb, vec3f(0.0), vec3f(1.0)) * 255.0);
    let p = select(vec3f(0.0), pow(max(i, vec3f(1.0)) / 256.0, vec3f(kGamma)), i > vec3f(0.0));
    let entry = floor(p * 1024.0) * 64.0;
    return vec4f(min(entry, vec3f(65535.0)) / 65535.0, c.a);
}

// The display-corrected table (DisplayRamp::HardwareTable), looked up per byte.
fn tableEntry(i: u32) -> f32 { return lut[i >> 2u][i & 3u]; }
@fragment fn fs_table(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let c = textureLoad(srcTex, vec2i(pos.xy), 0);
    let i = vec3u(round(clamp(c.rgb, vec3f(0.0), vec3f(1.0)) * 255.0));
    let e = vec3f(tableEntry(i.r), tableEntry(i.g), tableEntry(i.b));
    return vec4f(e / 65535.0, c.a);
}
)WGSL";

}  // namespace

uint16_t DisplayRamp::RampCode(float gamma, int i) {
    // DxRnd::SetupGamma: (unsigned short)(pow(i * 0.00390625f, g) * 1024.0f) << 6.
    const float x = (float)i * 0.00390625f;
    const float p = PowF(x, gamma);
    const float e = p * 1024.0f;
    const uint16_t entry = (uint16_t)(((unsigned)(uint16_t)e << 6) & 0xFFFFu);
    return (uint16_t)(entry >> 6);
}

void DisplayRamp::DisplayTable(int type, float power, uint16_t out[1024]) {
    // D3D::GetCurrentGammaTable(0): sRGB decode, then the display's encode.
    for (int k = 0; k < 1024; k++) {
        float x = (float)k * 0.0009775171f;   // 1 / 1023
        x = GammaSRGB(x, true);
        if (type == kDisplaySRGB) x = GammaSRGB(x, false);
        else if (type == kDisplayPower) x = GammaStandard(x, power, false);
        else x = GammaTV(x, false);           // 2, and any other type
        const double d = std::fma((double)x, 1023.0, 0.5);
        int v = (int)d;                       // fctiwz
        v = v < 0 ? 0 : (v > 1023 ? 1023 : v);
        out[k] = (uint16_t)v;
    }
}

void DisplayRamp::HardwareTable(float gamma, int type, float power, uint16_t out[256]) {
    // D3D::GammaRampCorrected: out = (table[in >> 6] & 0x3FF) << 6.
    uint16_t table[1024];
    DisplayTable(type, power, table);
    for (int i = 0; i < 256; i++) out[i] = (uint16_t)(table[RampCode(gamma, i)] & 0x3FF);
}

bool DisplayRamp::Apply(wgpu::CommandEncoder& encoder, const wgpu::Texture& frameTex,
                        const wgpu::TextureView& frameView, float gamma, GpuDevice& gpu,
                        int type, float power) {
    if (!frameTex || !frameView || gamma <= 0.0f) return false;
    if (!(frameTex.GetUsage() & wgpu::TextureUsage::CopySrc)) {
        static bool warned = false;
        if (!warned) {
            fprintf(stderr, "DisplayRamp: the frame texture does not allow CopySrc; "
                            "presenting without the display gamma ramp\n");
            warned = true;
        }
        return false;
    }
    auto& dev = gpu.Device();
    const wgpu::TextureFormat fmt = frameTex.GetFormat();
    const uint32_t w = frameTex.GetWidth(), h = frameTex.GetHeight();

    const bool table = type != kDisplaySRGB;
    if (!mPipe || mFormat != fmt || (!table && mGamma != gamma) || mTablePipe != table) {
        if (!mShader) {
            wgpu::ShaderSourceWGSL src;
            src.code = kShader;
            wgpu::ShaderModuleDescriptor smDesc{};
            smDesc.nextInChain = &src;
            smDesc.label = "DisplayRamp";
            mShader = dev.CreateShaderModule(&smDesc);

            wgpu::BindGroupLayoutEntry e[2]{};
            e[0].binding = 0;
            e[0].visibility = wgpu::ShaderStage::Fragment;
            e[0].texture.sampleType = wgpu::TextureSampleType::Float;
            e[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
            e[1].binding = 1;
            e[1].visibility = wgpu::ShaderStage::Fragment;
            e[1].buffer.type = wgpu::BufferBindingType::Uniform;
            e[1].buffer.minBindingSize = 64 * 16;
            wgpu::BindGroupLayoutDescriptor bd{};
            bd.entryCount = 2;
            bd.entries = e;
            mBGL = dev.CreateBindGroupLayout(&bd);

            wgpu::BufferDescriptor lb{};
            lb.label = "DisplayRampTable";
            lb.size = 64 * 16;
            lb.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
            mLut = dev.CreateBuffer(&lb);
            mTableType = 0;
        }
        wgpu::PipelineLayoutDescriptor pld{};
        pld.bindGroupLayoutCount = 1;
        pld.bindGroupLayouts = &mBGL;
        wgpu::PipelineLayout pl = dev.CreatePipelineLayout(&pld);

        wgpu::ConstantEntry k{};
        k.key = "kGamma";
        k.value = gamma;
        wgpu::ColorTargetState ct{};
        ct.format = fmt;
        ct.writeMask = wgpu::ColorWriteMask::All;
        wgpu::FragmentState frag{};
        frag.module = mShader;
        frag.entryPoint = table ? "fs_table" : "fs_ramp";
        frag.constantCount = table ? 0 : 1;
        frag.constants = table ? nullptr : &k;
        frag.targetCount = 1;
        frag.targets = &ct;
        wgpu::RenderPipelineDescriptor pd{};
        pd.label = "DisplayRamp";
        pd.layout = pl;
        pd.vertex.module = mShader;
        pd.vertex.entryPoint = "vs_full";
        pd.fragment = &frag;
        pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        mPipe = CreateCountedRenderPipeline(dev, &pd);
        mFormat = fmt;
        mGamma = gamma;
        mTablePipe = table;
        mScratch = nullptr;
    }
    if (table && (mTableType != type || mTableGamma != gamma || mTablePower != power)) {
        uint16_t hw[256];
        HardwareTable(gamma, type, power, hw);
        float data[256];
        for (int i = 0; i < 256; i++) data[i] = (float)hw[i] * 64.0f;
        gpu.Queue().WriteBuffer(mLut, 0, data, sizeof(data));
        mTableType = type;
        mTableGamma = gamma;
        mTablePower = power;
    }
    if (!mScratch || mW != w || mH != h) {
        wgpu::TextureDescriptor td{};
        if (mBindGroups && mScratchView) mBindGroups->Forget(mScratchView.Get());
        td.label = "DisplayRampSource";
        td.size = {w, h, 1};
        td.format = fmt;
        td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
        mScratch = gpu.CreateTexture(td);
        mScratchView = mScratch.CreateView();
        mW = w;
        mH = h;
    }

    wgpu::TexelCopyTextureInfo src{};
    src.texture = frameTex;
    wgpu::TexelCopyTextureInfo dst{};
    dst.texture = mScratch;
    wgpu::Extent3D size = {w, h, 1};
    encoder.CopyTextureToTexture(&src, &dst, &size);

    wgpu::BindGroupEntry be[2]{};
    be[0].binding = 0;
    be[0].textureView = mScratchView;
    be[1].binding = 1;
    be[1].buffer = mLut;
    be[1].size = 64 * 16;
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = mBGL;
    bgd.entryCount = 2;
    bgd.entries = be;
    wgpu::BindGroup bg = mBindGroups ? mBindGroups->GetOrCreate(dev, bgd) : CreateCountedBindGroup(dev, &bgd);

    wgpu::RenderPassColorAttachment att{};
    att.view = frameView;
    att.loadOp = wgpu::LoadOp::Load;
    att.storeOp = wgpu::StoreOp::Store;
    wgpu::RenderPassDescriptor rp{};
    rp.label = "DisplayRampPass";
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &att;
    MILO_GPU_TIME_PASS(rp);
    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&rp);
    pass.SetPipeline(mPipe);
    pass.SetBindGroup(0, bg);
    pass.Draw(3);
    pass.End();
    return true;
}

// Shipped WGSL accessor (gfx/ShippedWgsl.h): lets the validation test compile
// the exact source this file hands CreateShaderModule.
#include "gfx/ShippedWgsl.h"
const char* DisplayRampWgslSource() { return kShader; }
