// The ambient constant's alpha, c1.w (gfx/AmbientAlpha.h).
//
// Retail RB3 X360 NgMat::SetupAmbient (band.exe TU5 fn_824A9090) loads the
// current environ's ambient colour, alpha included, into c1 for a use_environ
// material, and (1, 1, 1, 1) otherwise; every non-pseudo-HDR standard program
// multiplies the output alpha by c1.w (decoded rb3 xbox_shaders standard_10,
// standard_110, standard_20010). RB3's UI fades whole groups through an
// environ's ambient_alpha (movie_pause.milo fade.env, fade_text.env).
//
// This suite builds in DC3's context, so it checks the decomp-agnostic
// contract (RetailC1Alpha and its WGSL mirror) and that DC3 cannot change by
// construction; the RB3 shape's half (MatAmbientIsEnviron, the scene writer's
// w) is gated in rb3-xenon (native/src/rb3_app_gates.cpp).

#include "test_helpers.h"

#include "gfx/AmbientAlpha.h"
#include "gfx/ShippedWgsl.h"
#include "gfx/UniformStructs.h"
#include "platform/rndshape/RndShape.h"
#include "rndobj/Mat.h"

#include <cstring>

class EnvironAmbientAlphaTest : public EngineTestFixture {};

// retailLighting: 1 the RB3 retail model (rb3-xenon, and RB3-Wii with w = 1),
// 2 DC3's retail model, 0 the heuristic arm.
TEST(EnvironAmbientAlpha, UseEnvironMaterialTakesTheEnvironAlpha) {
    EXPECT_EQ(RetailC1Alpha(kMatAmbientIsEnvironBit, 1.0f, 0.5f), 0.5f);
    EXPECT_EQ(RetailC1Alpha(kMatAmbientIsEnvironBit, 1.0f, 0.0f), 0.0f);
    EXPECT_EQ(RetailC1Alpha(kMatAmbientIsEnvironBit, 1.0f, 0.922f), 0.922f);
    // The other colorMod.w fields do not disturb it.
    EXPECT_EQ(RetailC1Alpha(kMatAmbientIsEnvironBit | 0x3u | (1u << 9) | (0x3C00u << 16), 1.0f,
                            0.5f),
              0.5f);
}

TEST(EnvironAmbientAlpha, MaterialThatIgnoresTheEnvironKeepsOne) {
    // use_environ off: c1 = (1, 1, 1, 1), whatever the environ's alpha.
    EXPECT_EQ(RetailC1Alpha(0u, 1.0f, 0.5f), 1.0f);
    EXPECT_EQ(RetailC1Alpha(0x3u | (1u << 8) | (1u << 10), 1.0f, 0.0f), 1.0f);
}

TEST(EnvironAmbientAlpha, OtherLightModelsKeepOne) {
    // DC3 (2) and the heuristic arm (0) read c1.w = 1 even with the bit set.
    EXPECT_EQ(RetailC1Alpha(kMatAmbientIsEnvironBit, 2.0f, 0.5f), 1.0f);
    EXPECT_EQ(RetailC1Alpha(kMatAmbientIsEnvironBit, 0.0f, 0.5f), 1.0f);
}

TEST(EnvironAmbientAlpha, BitDoesNotCollideWithTheOtherColorModFields) {
    // Low byte mode, bit 8 band selector, bit 9 screen-aligned, bit 10
    // flip_normal, high half the refraction strength (UniformStructs.h).
    const uint32_t others = 0xFFu | (1u << 8) | (1u << 9) | (1u << 10) | 0xFFFF0000u;
    EXPECT_EQ(kMatAmbientIsEnvironBit & others, 0u);
}

TEST(EnvironAmbientAlpha, ShaderMirrorsTheContract) {
    const char *wgsl = StandardWgslSource();
    ASSERT_NE(wgsl, nullptr);
    // The bit, the light-model gate and the scene ambient's w ...
    EXPECT_NE(strstr(wgsl, "fn retailC1Alpha() -> f32 {\n"
                           "    let rb3RetailModel = scene.retailLighting > 0.5 && "
                           "scene.retailLighting < 1.5;\n"
                           "    if (rb3RetailModel && ((material.colorMod.w >> 11u) & 1u) != 0u) {\n"
                           "        return scene.ambientColor.w;\n"
                           "    }\n"
                           "    return 1.0;\n"
                           "}"),
              nullptr);
    // ... as a factor of the material alpha every output alpha is built from.
    EXPECT_NE(strstr(wgsl, "let materialAlpha = material.color.a * retailC1Alpha();"), nullptr);
    EXPECT_EQ(kMatAmbientIsEnvironBit, 1u << 11);
}

// DC3 by construction: its shape never marks a material's c1 as the environ's
// (so the shader's factor is exactly 1), with use_environ on or off, prelit
// or not.
TEST_F(EnvironAmbientAlphaTest, DC3ShapeNeverMarksTheMaterial) {
    EXPECT_FALSE(rndshape::kRB3RetailLight);
    for (int useEnv = 0; useEnv < 2; useEnv++) {
        for (int prelit = 0; prelit < 2; prelit++) {
            RndMat *mat = Hmx::Object::New<RndMat>();
            mat->SetUseEnv(useEnv != 0);
            mat->SetPreLit(prelit != 0);
            EXPECT_FALSE(rndshape::MatAmbientIsEnviron(rndshape::Mat(mat)))
                << "use_environ " << useEnv << " prelit " << prelit;
            delete mat;
        }
    }
}
