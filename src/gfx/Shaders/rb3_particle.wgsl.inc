R"WGSL(
struct SceneUB {
    viewProj: mat4x4f,
};
@group(0) @binding(0) var<uniform> scene: SceneUB;

@group(1) @binding(0) var particleTex: texture_2d<f32>;
@group(1) @binding(1) var particleSampler: sampler;

struct VIn {
    @location(0) pos: vec3f,
    @location(1) uv: vec2f,
    @location(2) color: vec4f,
};
struct VOut {
    @builtin(position) clip: vec4f,
    @location(0) uv: vec2f,
    @location(1) color: vec4f,
};

@vertex fn vs_particle(in: VIn) -> VOut {
    var out: VOut;
    out.clip = scene.viewProj * vec4f(in.pos, 1.0);
    out.uv = in.uv;
    out.color = in.color;
    return out;
}

@fragment fn fs_particle(in: VOut) -> @location(0) vec4f {
    let tex = textureSample(particleTex, particleSampler, in.uv);
    let c = tex * in.color;
    if (c.a < 0.004) { discard; }
    return c;
}
)WGSL"
