R"WGSL(
struct PostProcUB {
    contrast: f32,
    brightness: f32,
    saturation: f32,
    vignetteIntensity: f32,
    vignetteColor: vec4f,
    chromaticOffset: f32,
    chromaticSharpen: f32,
    posterLevels: f32,
    posterMin: f32,
    levelInLo: vec4f,
    levelInHi: vec4f,
    levelOutLo: vec4f,
    levelOutHi: vec4f,
    bloomIntensity: f32,
    noiseIntensity: f32,
    noiseMidtone: f32,
    flickerMul: f32,
    bloomColor: vec4f,
    time: f32,
    noiseScaleX: f32,
    noiseScaleY: f32,
    noiseHasMap: f32,
};

@group(0) @binding(0) var sceneTex: texture_2d<f32>;
@group(0) @binding(1) var sceneSampler: sampler;
@group(0) @binding(2) var<uniform> pp: PostProcUB;
@group(0) @binding(3) var bloomTex: texture_2d<f32>;
@group(0) @binding(4) var noiseTex: texture_2d<f32>;

// RB3's grain is a *map-domain* gain (mNoiseIntensity ~3.0 on the menu/world
// postprocs is a texel-space multiplier, NOT a screen-space add). Applying the
// raw (n-0.5)*intensity term washed the whole frame gray (the v1 failure mode).
// kNoiseGain attenuates the per-pixel luminance deviation down to film-grain
// magnitude. With gain 0.04 and clamped intensity 3.0 the per-pixel swing is
// ~±0.06 (~±15/255) — a fine, visible, ZERO-MEAN grain (mean luminance
// unchanged → no gray wash, the v1 failure mode). Verified on the static
// song_select menu: grain ON vs OFF shifts mean luminance by <0.5/255 and
// saturation by <0.002 while raising midtone high-frequency variance, matching
// the Wii's midtone-concentrated grain. (0.03 was nearly imperceptible; 0.05
// reads slightly heavy — 0.04 is the balance, with margin on the no-wash gate.)
const kNoiseGain: f32 = 0.04;

struct VOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

@vertex fn vs_fullscreen(@builtin(vertex_index) idx: u32) -> VOut {
    var out: VOut;
    let x = f32(i32(idx & 1u)) * 4.0 - 1.0;
    let y = f32(i32(idx >> 1u)) * 4.0 - 1.0;
    out.pos = vec4f(x, y, 0.0, 1.0);
    out.uv = vec2f((x + 1.0) * 0.5, (1.0 - y) * 0.5);
    return out;
}

@fragment fn fs_postproc(in: VOut) -> @location(0) vec4f {
    let texSize = vec2f(textureDimensions(sceneTex));

    var color: vec3f;
    if (pp.chromaticOffset > 0.0) {
        let offset = pp.chromaticOffset / texSize;
        let r = textureSample(sceneTex, sceneSampler, in.uv + vec2f(offset.x, 0.0)).r;
        let g = textureSample(sceneTex, sceneSampler, in.uv).g;
        let b = textureSample(sceneTex, sceneSampler, in.uv - vec2f(offset.x, 0.0)).b;
        if (pp.chromaticSharpen > 0.5) {
            let center = textureSample(sceneTex, sceneSampler, in.uv).rgb;
            let blur = vec3f(r, g, b);
            color = center + (center - blur) * 1.5;
        } else {
            color = vec3f(r, g, b);
        }
    } else {
        color = textureSample(sceneTex, sceneSampler, in.uv).rgb;
    }

    let inRange = max(pp.levelInHi.rgb - pp.levelInLo.rgb, vec3f(0.001));
    let normalized = clamp((color - pp.levelInLo.rgb) / inRange, vec3f(0.0), vec3f(1.0));
    color = mix(pp.levelOutLo.rgb, pp.levelOutHi.rgb, normalized);

    // Match Xbox's non-linear contrast formula (from RndColorXfm::AdjustContrast)
    var contrastMul: f32;
    let contrastNorm = pp.contrast / 100.0;
    if (contrastNorm > 0.0) {
        contrastMul = 1.0 / (contrastNorm * -0.9921875 + 1.0);
    } else {
        contrastMul = -(contrastNorm * -0.992126 - 1.0);
    }
    let contrastOff = (1.0 - contrastMul) * 0.5;
    color = color * contrastMul + contrastOff;
    // Brightness: match Xbox formula
    let brightnessAdj = (pp.brightness + 100.0) / 200.0 - 0.5;
    color = color + brightnessAdj;

    let luma = dot(color, vec3f(0.2126, 0.7152, 0.0722));
    color = mix(vec3f(luma), color, 1.0 + pp.saturation / 100.0);

    if (pp.posterLevels > 1.0) {
        let levels = pp.posterLevels;
        let intensity = max(max(color.r, color.g), color.b);
        if (intensity >= pp.posterMin) {
            color = floor(color * levels + 0.5) / levels;
        }
    }

    if (pp.vignetteIntensity > 0.0) {
        let center = in.uv - 0.5;
        let dist = length(center) * 1.414;
        let vig = 1.0 - smoothstep(0.4, 1.0, dist) * pp.vignetteIntensity;
        color = mix(pp.vignetteColor.rgb, color, vig);
    }

    // Flicker: time-based brightness modulation
    if (pp.flickerMul != 1.0) {
        color = color * pp.flickerMul;
    }

    // Noise/grain: tiled noise TEXTURE (the real RB3 path) with a procedural
    // hash fallback for postprocs that set an intensity but ship no bitmap.
    if (pp.noiseIntensity != 0.0) {
        var n: f32;
        if (pp.noiseHasMap > 0.5) {
            // Tiled noise bitmap. mNoiseBaseScale is the X/Y tiling count; sample
            // by screen-UV * scale (Repeat sampler) so it reads as fine,
            // stationary film grain — NOT a moving wash (Wii mNoiseStationary).
            let nuv = in.uv * vec2f(pp.noiseScaleX, pp.noiseScaleY);
            n = textureSample(noiseTex, sceneSampler, nuv).r;
        } else {
            // Procedural fallback (no bitmap on this postproc).
            let px = in.uv * vec2f(textureDimensions(sceneTex));
            n = fract(sin(dot(px + pp.time * 43.17, vec2f(12.9898, 78.233))) * 43758.5453);
        }
        // KEY no-gray-wash fix: scale the per-pixel deviation DOWN (kNoiseGain)
        // and clamp the map-domain intensity. Zero-mean about 0.5 → mean
        // luminance unchanged. The fallback shares this attenuation so it can
        // never reproduce the v1 intensity-3.0 blizzard either.
        let grain = (n - 0.5) * min(abs(pp.noiseIntensity), 3.0) * kNoiseGain;
        if (pp.noiseMidtone > 0.5) {
            // Overlay-style midtone weight: pins grain OFF at black (luma 0) and
            // white (luma 1), peaks at luma 0.5 — keeps shadows/highlights clean.
            let nl = dot(color, vec3f(0.2126, 0.7152, 0.0722));
            let midtoneMask = 4.0 * nl * (1.0 - nl);
            color = color + grain * midtoneMask;
        } else {
            color = color + grain;
        }
    }

    if (pp.bloomIntensity > 0.0) {
        let bloom = textureSample(bloomTex, sceneSampler, in.uv).rgb;
        // Clamp intensity to prevent overpowering bloom from aggressive game data
        let clampedIntensity = min(pp.bloomIntensity, 1.0);
        let bloomContrib = bloom * clampedIntensity * pp.bloomColor.rgb;
        // ADDITIVE blend (not screen): screen-blend `1-(1-c)*(1-b*k)` lifts DARK
        // pixels hardest (the (1-c) term is large there) → the whole dark
        // background washes milky-bright (the v1-bloom failure mode on the dark-blue
        // menu). Additive `c + b*k` instead lifts every pixel by the SAME absolute
        // amount, so a tight highlight halo reads as a glow while the dark
        // background stays dark. bloomColor.a carries the blend factor (CPU-set;
        // see RunPostProcComposite) so it's tunable without a uniform-layout change.
        color = color + bloomContrib * pp.bloomColor.a;
    }

    // Gameplay-entry "first-frame flash" guard. The postproc composite (render the
    // venue into the offscreen intermediate, then grade-blit it to the framebuffer)
    // structurally over-brightens the venue during its song-START lighting REVEAL
    // (the stage lights flash up as gameplay begins; the native lit-path runs that
    // peak hotter than the Wii's GX backdrop), washing the WHOLE venue to a flat
    // pink/white field for the first ~1-2s. Measured: with the composite the venue
    // first-frame clipW (pixels pinned white in all channels) hits 9/16 boots
    // >5% (max ~83%); rendering DIRECT (no composite) it is 0/16. The composite
    // grade params are neutral here — the wash survives identity levels + bloom-off
    // + noise-off — so it is the intermediate→composite STRUCTURE, not a grade term.
    //
    // Guard: soft-clip the composite OUTPUT with a Reinhard rolloff to a ceiling
    // BELOW pure white, so a hot venue moment compresses to a bright-but-readable
    // result instead of clipping to a flat white wash. Identity below the knee
    // (correctly-exposed venue/menu/song-select frames are untouched — they never
    // reach the knee), C1-continuous above. The composite grades ONLY the venue
    // backdrop (the gameplay highway / gems / HUD draw UNGRADED on the framebuffer
    // after the mid-frame flush — FlushPostProcMidFrame), so this never dims the
    // HUD, gem cores, or bright UI.
    let ppCeil = 0.97;
    let ppKnee = 0.82;
    let ppSpan = ppCeil - ppKnee;
    let ppOver = max(color - vec3f(ppKnee), vec3f(0.0));
    let ppRolled = vec3f(ppKnee) + vec3f(ppSpan) * (ppOver / (ppOver + vec3f(ppSpan)));
    color = select(color, ppRolled, color > vec3f(ppKnee));

    return vec4f(clamp(color, vec3f(0.0), vec3f(1.0)), 1.0);
}
)WGSL"
