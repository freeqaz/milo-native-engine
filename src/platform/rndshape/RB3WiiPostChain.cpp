// RB3WiiPostChain.cpp — fills gfx/RB3RetailPost's parameters from an RB3
// RndPostProc, the way NgPostProc sets the retail shader constants
// (rb3-xenon rndobj/PostProc_NG.cpp). Built only for the dc3 GPU backend with
// MILO_ENGINE_RNDOBJ_SHAPE=rb3wii; the DC3 shape's FillRetailPost is a no-op.
#include "platform/rndshape/RndShape.h"
#include "gfx/RB3RetailPost.h"

#include "rndobj/PostProc.h"
#include "rndobj/ColorXfm.h"

#include <cstdlib>
#include <cstring>

namespace rndshape {

int RetailPostMode() {
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("MILO_RB3_RETAIL_POST");
        s = !e ? 1 : (e[0] == '0') ? 0 : (strcmp(e, "raw") == 0) ? 2
            : (strcmp(e, "mask") == 0) ? 3 : (strcmp(e, "bloom") == 0) ? 4
            : (strcmp(e, "grade") == 0) ? 5 : 1;
    }
    return s;
}

// SetBloomColor: c7 = (0.3, 0.59, 0.11) / d, d = threshold when it is above 1,
// else 1. Thresholds at or below 1 therefore all mean "weight by luma".
static float BloomLumaScale(const RndPostProc *pp) {
    const float thr = pp->GetBloomThreshold();
    return (1.0f - thr >= 0.0f) ? 1.0f : 1.0f / thr;
}

float BloomMaskScale(const MatView &m) {
    if (!RetailBloomMaskActive()) return 0.0f;
    const RndPostProc *pp = RndPostProc::Current();
    if (!pp) return 0.0f;
    // NgMat::AllowHDR.
    const RndMat *mat = m.Raw();
    const RndMat::Blend b = mat->GetBlend();
    if (b == RndMat::kBlendSrcAlpha || b == RndMat::kBlendSrcAlphaAdd
        || b == RndMat::kPreMultAlpha || mat->mAlphaCut || mat->mAlphaWrite)
        return 0.0f;
    return BloomLumaScale(pp);
}

void FillRetailPost(const RndPostProc *pp, float flickerMul, RetailPostParams &out) {
    out = RetailPostParams();
    if (!pp) return;

    // DoBloom: doBloom = BloomIntensity() > 0 || bloomColor.alpha > 0; the
    // composite's c6 is the bloom colour scaled by BloomIntensity() (which
    // already thirds a glare bloom on the hi-res screen).
    const float intensity = pp->BloomIntensity();
    const Hmx::Color &bc = pp->GetBloomColor();
    out.bloom = intensity > 0.0f || bc.alpha > 0.0f;
    out.bloomColor[0] = bc.red * intensity;
    out.bloomColor[1] = bc.green * intensity;
    out.bloomColor[2] = bc.blue * intensity;
    out.bloomLumaScale = BloomLumaScale(pp);

    // ModulateColorXfm: the RndColorXfm matrix, its 3x3 part scaled by the
    // flicker modulation, applied in the composite as dp4 against (rgb, 1).
    // Milo transforms colours as row vectors (c' = c.r*m.x + c.g*m.y + c.b*m.z
    // + v), so output channel i takes column i of the 3x3.
    const Transform &t = pp->GetColorXfm().mColorXfm;
    const Vector3 *rows[3] = {&t.m.x, &t.m.y, &t.m.z};
    for (int i = 0; i < 3; i++) {
        const float vi = i == 0 ? t.v.x : i == 1 ? t.v.y : t.v.z;
        out.xfm[i][0] = (i == 0 ? rows[0]->x : i == 1 ? rows[0]->y : rows[0]->z) * flickerMul;
        out.xfm[i][1] = (i == 0 ? rows[1]->x : i == 1 ? rows[1]->y : rows[1]->z) * flickerMul;
        out.xfm[i][2] = (i == 0 ? rows[2]->x : i == 1 ? rows[2]->y : rows[2]->z) * flickerMul;
        out.xfm[i][3] = vi;
    }

    out.vignetteIntensity = pp->GetVignetteIntensity();
    const Hmx::Color &vc = pp->GetVignetteColor();
    out.vignetteColor[0] = vc.red;
    out.vignetteColor[1] = vc.green;
    out.vignetteColor[2] = vc.blue;
    out.vignetteColor[3] = vc.alpha;
    out.posterLevels = pp->GetPosterLevels();
    out.posterMin = pp->GetPosterMin();
    out.chromaticOffset = pp->GetChromaticAberrationOffset();
    out.chromaticSharpen = pp->GetChromaticSharpen() ? 1.0f : 0.0f;
    out.noiseIntensity = PostProcGrain(pp);
    out.noiseMidtone = pp->GetNoiseMidtone() ? 1.0f : 0.0f;
}

} // namespace rndshape
