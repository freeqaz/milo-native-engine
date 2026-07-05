R"WGSL(
struct VertexRect { @location(0) pos: vec2f, @location(1) uv: vec2f, @location(2) color: vec4f, };
struct VSOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f, };
struct ComposeUB { color2: vec4f, };
@group(0) @binding(0) var diffTex: texture_2d<f32>;
@group(0) @binding(1) var interpTex: texture_2d<f32>;
@group(0) @binding(2) var composeSampler: sampler;
@group(0) @binding(3) var<uniform> cub: ComposeUB;
@vertex fn vs_compose(in: VertexRect) -> VSOut {
    var out: VSOut;
    out.pos = vec4f(in.pos, 0.0, 1.0);
    out.uv = in.uv;
    return out;
}
@fragment fn fs_compose_interp(in: VSOut) -> @location(0) vec4f {
    let d = textureSample(diffTex, composeSampler, in.uv);
    let w = textureSample(interpTex, composeSampler, in.uv).a;
    // src.rgb = diff.rgb * color2 ; src.a = interp weight w. Blended SrcAlpha
    // OVER the destination (already = diff*color1) yields, per pixel:
    //   diff.rgb * color2 * w + diff.rgb * color1 * (1-w)
    //   = diff.rgb * lerp(color1, color2, w).
    return vec4f(d.rgb * cub.color2.rgb, w);
}
)WGSL"
