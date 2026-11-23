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
#include "gfx/ColorModW.h"
#include "gfx/ShippedWgsl.h"
#include "gfx/UniformStructs.h"
#include "platform/rndshape/RndShape.h"
#include "rndobj/Mat.h"

#include <cstring>
#include <string>

// Every colorMod.w field except the ambient-source flag (gfx/ColorModW.h).
static uint32_t OtherColorModWFields() {
    uint32_t others = 0;
    for (const ColorModWField &f : kColorModWFields)
        if (f.mask != kMatAmbientIsEnvironBit) others |= f.mask;
    return others;
}

// The bit position of a one-bit mask.
static int BitIndex(uint32_t mask) {
    for (int i = 0; i < 32; i++)
        if (mask == (1u << i)) return i;
    return -1;
}

class EnvironAmbientAlphaTest : public EngineTestFixture {};

// retailLighting: 1 the RB3 retail model (rb3-xenon, and RB3-Wii with w = 1),
// 2 DC3's retail model, 0 the heuristic arm.
TEST(EnvironAmbientAlpha, UseEnvironMaterialTakesTheEnvironAlpha) {
    EXPECT_EQ(RetailC1Alpha(kMatAmbientIsEnvironBit, 1.0f, 0.5f), 0.5f);
    EXPECT_EQ(RetailC1Alpha(kMatAmbientIsEnvironBit, 1.0f, 0.0f), 0.0f);
    EXPECT_EQ(RetailC1Alpha(kMatAmbientIsEnvironBit, 1.0f, 0.922f), 0.922f);
    // The other colorMod.w fields do not disturb it.
    EXPECT_EQ(RetailC1Alpha(kMatAmbientIsEnvironBit | 0x3u | kColorModWScreenAlignedBit |
                                kColorModWFadesOutBit | (0x3C00u << kColorModWRefractStrengthShift),
                            1.0f, 0.5f),
              0.5f);
}

TEST(EnvironAmbientAlpha, MaterialThatIgnoresTheEnvironKeepsOne) {
    // use_environ off: c1 = (1, 1, 1, 1), whatever the environ's alpha.
    EXPECT_EQ(RetailC1Alpha(0u, 1.0f, 0.5f), 1.0f);
    // Every other colorMod.w field set, fade_out included, is not the flag.
    EXPECT_EQ(RetailC1Alpha(OtherColorModWFields(), 1.0f, 0.0f), 1.0f);
    EXPECT_EQ(RetailC1Alpha(kColorModWFadesOutBit, 1.0f, 0.0f), 1.0f);
}

TEST(EnvironAmbientAlpha, OtherLightModelsKeepOne) {
    // DC3 (2) and the heuristic arm (0) read c1.w = 1 even with the bit set.
    EXPECT_EQ(RetailC1Alpha(kMatAmbientIsEnvironBit, 2.0f, 0.5f), 1.0f);
    EXPECT_EQ(RetailC1Alpha(kMatAmbientIsEnvironBit, 0.0f, 0.5f), 1.0f);
}

TEST(EnvironAmbientAlpha, BitDoesNotCollideWithTheOtherColorModFields) {
    // The flag is one allocated field of gfx/ColorModW.h, read by retailC1Alpha.
    int entries = 0;
    for (const ColorModWField &f : kColorModWFields) {
        if (f.mask != kMatAmbientIsEnvironBit) continue;
        entries++;
        ASSERT_NE(f.wgslAccessor, nullptr);
        EXPECT_STREQ(f.wgslAccessor, "retailC1Alpha");
    }
    EXPECT_EQ(entries, 1);
    EXPECT_EQ(BitIndex(kMatAmbientIsEnvironBit), (int)kColorModWAmbientIsEnvironShift);
    // Against every allocated field by name (mode, band selector, screen
    // aligned, flip_normal, fade_out, refraction strength), and all of them
    // pairwise, so a field that lands on another's bit fails here.
    EXPECT_EQ(kMatAmbientIsEnvironBit & kColorModWFadesOutBit, 0u) << "collides with fade_out";
    EXPECT_EQ(kMatAmbientIsEnvironBit & OtherColorModWFields(), 0u);
    const size_t n = sizeof(kColorModWFields) / sizeof(kColorModWFields[0]);
    for (size_t a = 0; a < n; a++)
        for (size_t b = a + 1; b < n; b++)
            EXPECT_EQ(kColorModWFields[a].mask & kColorModWFields[b].mask, 0u)
                << kColorModWFields[a].name << " and " << kColorModWFields[b].name
                << " share a colorMod.w bit";
    EXPECT_TRUE(ColorModWFieldsDisjoint());
    // Bit 12 (bits 0-11 and 16-31 were taken when it was allocated).
    EXPECT_EQ(kMatAmbientIsEnvironBit, 1u << 12);
}

// The WGSL reads each one-bit flag in its own accessor with a literal shift
// (standard_wgsl.inc cannot include the header): each accessor's shift is the
// allocated one, and the shader reads no colorMod.w bit that is not allocated.
TEST(EnvironAmbientAlpha, ShaderReadsEveryFlagAtItsAllocatedBit) {
    const std::string wgsl = StandardWgslSource();
    const std::string read = "material.colorMod.w >> ";
    int flags = 0;
    for (const ColorModWField &f : kColorModWFields) {
        if (!f.wgslAccessor) continue;
        flags++;
        const size_t fn = wgsl.find(std::string("fn ") + f.wgslAccessor + "()");
        ASSERT_NE(fn, std::string::npos) << f.wgslAccessor;
        const size_t end = wgsl.find("\n}\n", fn);
        ASSERT_NE(end, std::string::npos) << f.wgslAccessor;
        const std::string body = wgsl.substr(fn, end - fn);
        const std::string expect = read + std::to_string(BitIndex(f.mask)) + "u)";
        EXPECT_NE(body.find(expect), std::string::npos)
            << f.wgslAccessor << " does not read " << f.name << " at " << expect;
        EXPECT_EQ(body.find(read), body.rfind(read)) << f.wgslAccessor << " reads two shifts";
    }
    EXPECT_EQ(flags, 5);
    int reads = 0;
    for (size_t at = wgsl.find(read); at != std::string::npos; at = wgsl.find(read, at + 1)) {
        reads++;
        const int shift = std::stoi(wgsl.substr(at + read.size()));
        bool allocated = false;
        for (const ColorModWField &f : kColorModWFields)
            if (f.wgslAccessor && BitIndex(f.mask) == shift) allocated = true;
        EXPECT_TRUE(allocated) << "the shader reads colorMod.w bit " << shift
                               << ", which gfx/ColorModW.h does not allocate to a flag";
    }
    EXPECT_EQ(reads, flags);
}

TEST(EnvironAmbientAlpha, ShaderMirrorsTheContract) {
    const char *wgsl = StandardWgslSource();
    ASSERT_NE(wgsl, nullptr);
    // The bit, the light-model gate and the scene ambient's w ...
    const std::string body = std::string("fn retailC1Alpha() -> f32 {\n"
                                         "    let rb3RetailModel = scene.retailLighting > 0.5 && "
                                         "scene.retailLighting < 1.5;\n"
                                         "    if (rb3RetailModel && ((material.colorMod.w >> ") +
                             std::to_string(kColorModWAmbientIsEnvironShift) +
                             "u) & 1u) != 0u) {\n"
                             "        return scene.ambientColor.w;\n"
                             "    }\n"
                             "    return 1.0;\n"
                             "}";
    EXPECT_NE(strstr(wgsl, body.c_str()), nullptr);
    // ... as a factor of the material alpha every output alpha is built from.
    EXPECT_NE(strstr(wgsl, "let materialAlpha = material.color.a * retailC1Alpha();"), nullptr);
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
