R"WGSL(
struct VertexRect {
    @location(0) pos: vec2f,
    @location(1) uv: vec2f,
    @location(2) color: vec4f,
};

struct VSOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
    @location(1) color: vec4f,
};

struct RectUB {
    modColor: vec4f,
    flags: vec4u,   // flags.x = colorMod
};

@group(0) @binding(0) var rectTex: texture_2d<f32>;
@group(0) @binding(1) var rectSampler: sampler;
@group(0) @binding(2) var<uniform> rectUB: RectUB;

@vertex fn vs_rect(in: VertexRect) -> VSOut {
    var out: VSOut;
    out.pos = vec4f(in.pos, 0.0, 1.0);
    out.uv = in.uv;
    out.color = in.color;
    return out;
}

@fragment fn fs_rect(in: VSOut) -> @location(0) vec4f {
    let tex = textureSample(rectTex, rectSampler, in.uv);
    // colorMod == kColorModAlphaUnpackModulate (2): treat the diffuse alpha as a
    // grayscale mask (v1 approximation of the Wii alpha-unpack-modulate path).
    var src = tex;
    if (rectUB.flags.x == 2u) {
        src = vec4f(tex.a, tex.a, tex.a, tex.a);
    }
    return src * rectUB.modColor * in.color;
}

@fragment fn fs_rect_notex(in: VSOut) -> @location(0) vec4f {
    return rectUB.modColor * in.color;
}
)WGSL"
