R"WGSL(
struct VOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

struct BlendUB {
    blend: f32,
    pad0: f32,
    pad1: f32,
    pad2: f32,
};

@group(0) @binding(0) var srcTex: texture_2d<f32>;
@group(0) @binding(1) var srcSampler: sampler;
@group(0) @binding(2) var rawTex: texture_2d<f32>;
@group(0) @binding(3) var<uniform> blendUB: BlendUB;

@vertex fn vs_fullscreen(@builtin(vertex_index) idx: u32) -> VOut {
    var out: VOut;
    let x = f32(i32(idx & 1u)) * 4.0 - 1.0;
    let y = f32(i32(idx >> 1u)) * 4.0 - 1.0;
    out.pos = vec4f(x, y, 0.0, 1.0);
    out.uv = vec2f((x + 1.0) * 0.5, (1.0 - y) * 0.5);
    return out;
}

@fragment fn fs_blit(in: VOut) -> @location(0) vec4f {
    // srcTex = bloomed (blurred) halo; rawTex = un-blurred source footprint.
    // Subtract the source's own footprint so only the OUTER halo is added; the
    // gem body cancels to ~0 and keeps its saturated base-pass color.
    let bloomCol = textureSample(srcTex, srcSampler, in.uv).rgb;
    let rawCol = textureSample(rawTex, srcSampler, in.uv).rgb;
    let halo = max(bloomCol - rawCol, vec3f(0.0, 0.0, 0.0)) * blendUB.blend;
    return vec4f(halo, 1.0);
}
)WGSL"
