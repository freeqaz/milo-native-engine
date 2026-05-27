// CharBonesSamples serialization tests — verify stream byte consumption
// is correct for various compression modes and bone counts.
//
// Key invariant: sizeof(Vector3) = 16 (has u32 PAD for SIMD alignment),
// but operator>>(BinStream&, Vector3&) reads only 12 bytes (3 floats).
// Cached .milo_xbox files write 16 bytes per Vector3 (12 data + 4 zero pad).
// The bulk read path (HX_NATIVE) must read mTotalSize * mNumSamples bytes.
// The element-by-element path reads 12 bytes per Vector3.
//
// These tests verify that:
// 1. sizeof(Vector3) is indeed 16
// 2. BinStream >> Vector3 reads exactly 12 bytes
// 3. BinStream >> Hmx::Quat reads exactly 16 bytes
// 4. CharBonesSamples::Save writes correct cached format
// 5. After Load, stream position is exactly where expected

#include "test_helpers.h"
#include "math/Vec.h"
#include "math/Mtx.h"
#include "char/CharBones.h"
#include "char/CharBonesSamples.h"
#include "os/Platform.h"

class CachedMemBinStream : public MemBinStream {
public:
    CachedMemBinStream(const void *data, int size, bool littleEndian = false,
                       Platform platform = kPlatformXBox)
        : MemBinStream(data, size, littleEndian), mPlatform(platform) {}

    explicit CachedMemBinStream(bool littleEndian = false,
                                Platform platform = kPlatformXBox)
        : MemBinStream(littleEndian), mPlatform(platform) {}

    bool Cached() const override { return true; }
    Platform GetPlatform() const override { return mPlatform; }

private:
    Platform mPlatform;
};

// ============================================================================
// Math type size invariants
// ============================================================================

TEST(MathTypeSizes, Vector3_Is16Bytes) {
    // Vector3 has u32 PAD for SIMD alignment
    EXPECT_EQ(sizeof(Vector3), 16u);
}

TEST(MathTypeSizes, Quat_Is16Bytes) {
    // Hmx::Quat has 4 floats, no PAD needed
    EXPECT_EQ(sizeof(Hmx::Quat), 16u);
}

TEST(MathTypeSizes, Matrix3_Is48Bytes) {
    // 3 Vector3s at 16 bytes each
    EXPECT_EQ(sizeof(Hmx::Matrix3), 48u);
}

TEST(MathTypeSizes, Transform_Is64Bytes) {
    // Matrix3 (48) + Vector3 (16)
    EXPECT_EQ(sizeof(Transform), 64u);
}

// ============================================================================
// BinStream serialization byte counts for math types
// ============================================================================

TEST(MathTypeSerialization, Vector3_Reads12Bytes) {
    // operator>>(BinStream&, Vector3&) reads 3 floats = 12 bytes
    std::vector<uint8_t> buf;
    PutBEFloat(buf, 1.0f);
    PutBEFloat(buf, 2.0f);
    PutBEFloat(buf, 3.0f);
    PutBEFloat(buf, 99.0f); // extra — should NOT be consumed

    MemBinStream ms(buf.data(), buf.size(), false);
    Vector3 v;
    ms >> v;
    EXPECT_EQ(ms.Tell(), 12); // NOT 16
    EXPECT_FLOAT_EQ(v.x, 1.0f);
    EXPECT_FLOAT_EQ(v.y, 2.0f);
    EXPECT_FLOAT_EQ(v.z, 3.0f);
}

TEST(MathTypeSerialization, Vector3_Writes12Bytes) {
    MemBinStream ms(false);
    Vector3 v(1.0f, 2.0f, 3.0f);
    ms << v;
    EXPECT_EQ(ms.Tell(), 12); // NOT 16
}

TEST(MathTypeSerialization, Quat_Reads16Bytes) {
    std::vector<uint8_t> buf;
    PutBEFloat(buf, 0.0f);
    PutBEFloat(buf, 0.0f);
    PutBEFloat(buf, 0.0f);
    PutBEFloat(buf, 1.0f);

    MemBinStream ms(buf.data(), buf.size(), false);
    Hmx::Quat q;
    ms >> q;
    EXPECT_EQ(ms.Tell(), 16);
    EXPECT_FLOAT_EQ(q.w, 1.0f);
}

TEST(MathTypeSerialization, Matrix3_Reads36Bytes) {
    // 3 Vector3s × 12 bytes each = 36 bytes (NOT 48)
    std::vector<uint8_t> buf;
    for (int i = 0; i < 9; i++) // 9 floats
        PutBEFloat(buf, (float)i);
    PutBEFloat(buf, 99.0f); // sentinel

    MemBinStream ms(buf.data(), buf.size(), false);
    Hmx::Matrix3 m;
    ms >> m;
    EXPECT_EQ(ms.Tell(), 36); // NOT 48
}

TEST(MathTypeSerialization, Transform_Reads48Bytes) {
    // Matrix3 (36) + Vector3 (12) = 48 bytes (NOT 64)
    std::vector<uint8_t> buf;
    for (int i = 0; i < 12; i++) // 12 floats
        PutBEFloat(buf, (float)i);
    PutBEFloat(buf, 99.0f); // sentinel

    MemBinStream ms(buf.data(), buf.size(), false);
    Transform tf;
    ms >> tf;
    EXPECT_EQ(ms.Tell(), 48); // NOT 64
}

// ============================================================================
// CharBonesSamples cached format — Save writes 16 bytes per Vector3
// ============================================================================

class CharBonesSamplesTest : public SymbolTestFixture {};

// Helper: build a CharBonesSamples with given bone config
static void SetupSimpleSamples(
    CharBonesSamples &samples,
    int numPosBones,
    int numQuatBones,
    int numSamples,
    CharBones::CompressionType comp
) {
    std::vector<CharBones::Bone> bones;
    for (int i = 0; i < numPosBones; i++) {
        char name[32];
        snprintf(name, sizeof(name), "bone%d.pos", i);
        bones.push_back({Symbol(name), 1.0f});
    }
    for (int i = 0; i < numQuatBones; i++) {
        char name[32];
        snprintf(name, sizeof(name), "bone%d.quat", i);
        bones.push_back({Symbol(name), 1.0f});
    }
    samples.Set(bones, numSamples, comp);
    // Zero the raw data so Save output is deterministic
    // Raw data is allocated by Set() — zero it via the public interface isn't
    // available, but Set() initializes it. Values don't matter for stream size tests.
}

TEST_F(CharBonesSamplesTest, SaveCachedFormat_1Pos_Uncompressed) {
    // 1 position bone, kCompressRots, 2 samples
    // Cached format: each Vector3 = 12 data + 4 pad = 16 bytes
    // Per sample: 1 pos × 16 = 16 bytes (already 16-aligned, delta=0)
    // Total data: 16 × 2 = 32 bytes
    CharBonesSamples samples;
    SetupSimpleSamples(samples, 1, 0, 2, CharBones::kCompressRots);

    MemBinStream writer(false);
    // Simulate cached Xbox platform
    // Note: we need to test Save behavior but can't easily set platform.
    // Instead, verify the element-by-element write size.
    samples.Save(writer);
    int saveSize = writer.Tell();

    // Non-cached Save: rev(4) + bones vector + counts(28) + comp(4) + numSamples(4)
    // + frames vector(4 for empty) + data
    // Data per sample (non-cached): 1 pos × 12 bytes = 12
    // Data total (non-cached): 12 × 2 = 24
    // The exact header size depends on bone name serialization

    // Read it back
    MemBinStream reader(writer.Buffer(), writer.Size(), false);
    CharBonesSamples loaded;
    loaded.Load(reader);

    // Verify stream was fully consumed (no bytes left over)
    EXPECT_EQ(reader.Tell(), saveSize);
    EXPECT_FALSE(reader.Fail());

    // Verify loaded state matches
    EXPECT_EQ(loaded.NumSamples(), 2);
    EXPECT_EQ(loaded.GetBones().size(), 1);
}

TEST_F(CharBonesSamplesTest, SaveLoadRoundTrip_MultipleBones) {
    // 2 pos bones + 1 quat bone, kCompressRots, 3 samples
    CharBonesSamples samples;
    SetupSimpleSamples(samples, 2, 1, 3, CharBones::kCompressRots);

    MemBinStream writer(false);
    samples.Save(writer);

    MemBinStream reader(writer.Buffer(), writer.Size(), false);
    CharBonesSamples loaded;
    loaded.Load(reader);

    EXPECT_EQ(reader.Tell(), writer.Tell());
    EXPECT_FALSE(reader.Fail());
    EXPECT_EQ(loaded.NumSamples(), 3);
}

TEST_F(CharBonesSamplesTest, SaveLoadRoundTrip_CompressVects) {
    // kCompressVects: Vector3 stored as 3 shorts (6 bytes), no PAD issue
    CharBonesSamples samples;
    SetupSimpleSamples(samples, 2, 0, 2, CharBones::kCompressVects);

    MemBinStream writer(false);
    samples.Save(writer);

    MemBinStream reader(writer.Buffer(), writer.Size(), false);
    CharBonesSamples loaded;
    loaded.Load(reader);

    EXPECT_EQ(reader.Tell(), writer.Tell());
    EXPECT_FALSE(reader.Fail());
}

TEST_F(CharBonesSamplesTest, SaveLoadRoundTrip_CompressAll) {
    // kCompressAll: shorts for vects, bytes for quats
    CharBonesSamples samples;
    SetupSimpleSamples(samples, 1, 1, 4, CharBones::kCompressAll);

    MemBinStream writer(false);
    samples.Save(writer);

    MemBinStream reader(writer.Buffer(), writer.Size(), false);
    CharBonesSamples loaded;
    loaded.Load(reader);

    EXPECT_EQ(reader.Tell(), writer.Tell());
    EXPECT_FALSE(reader.Fail());
}

TEST_F(CharBonesSamplesTest, SaveLoadRoundTrip_NoCompression) {
    // kCompressNone: full Vector3 (12 bytes stream) + full Quat (16 bytes) + full float rotations
    CharBonesSamples samples;
    SetupSimpleSamples(samples, 1, 1, 2, CharBones::kCompressNone);

    MemBinStream writer(false);
    samples.Save(writer);

    MemBinStream reader(writer.Buffer(), writer.Size(), false);
    CharBonesSamples loaded;
    loaded.Load(reader);

    EXPECT_EQ(reader.Tell(), writer.Tell());
    EXPECT_FALSE(reader.Fail());
}

// ============================================================================
// Two consecutive Loads from one stream (simulates CharClip mFull + mOne)
// ============================================================================

TEST_F(CharBonesSamplesTest, TwoConsecutiveLoads_NoDesync) {
    // This is the exact scenario that caused the stream desync bug:
    // CharClip::Load calls mFull.Load(bs) then mOne.Load(bs) sequentially.
    // If mFull.Load reads wrong byte count, mOne.Load reads garbage.

    CharBonesSamples full, one;
    SetupSimpleSamples(full, 1, 0, 2, CharBones::kCompressRots);
    SetupSimpleSamples(one, 3, 2, 5, CharBones::kCompressRots);

    // Write both to a single stream
    MemBinStream writer(false);
    full.Save(writer);
    int afterFull = writer.Tell();
    one.Save(writer);
    int afterOne = writer.Tell();

    // Read both back sequentially
    MemBinStream reader(writer.Buffer(), writer.Size(), false);
    CharBonesSamples loadedFull, loadedOne;

    loadedFull.Load(reader);
    EXPECT_EQ(reader.Tell(), afterFull) << "mFull.Load consumed wrong number of bytes!";
    EXPECT_FALSE(reader.Fail());

    loadedOne.Load(reader);
    EXPECT_EQ(reader.Tell(), afterOne) << "mOne.Load consumed wrong number of bytes!";
    EXPECT_FALSE(reader.Fail());

    EXPECT_EQ(loadedFull.NumSamples(), 2);
    EXPECT_EQ(loadedOne.NumSamples(), 5);
}

TEST_F(CharBonesSamplesTest, TwoConsecutiveLoads_AllCompressions) {
    // Test all compression modes to catch any stream desync
    CharBones::CompressionType modes[] = {
        CharBones::kCompressNone,
        CharBones::kCompressRots,
        CharBones::kCompressVects,
        CharBones::kCompressQuats,
        CharBones::kCompressAll,
    };

    for (auto comp : modes) {
        CharBonesSamples a, b;
        SetupSimpleSamples(a, 2, 1, 3, comp);
        SetupSimpleSamples(b, 1, 2, 4, comp);

        MemBinStream writer(false);
        a.Save(writer);
        int splitPos = writer.Tell();
        b.Save(writer);

        MemBinStream reader(writer.Buffer(), writer.Size(), false);
        CharBonesSamples la, lb;
        la.Load(reader);
        EXPECT_EQ(reader.Tell(), splitPos)
            << "Stream desync after first Load with compression=" << (int)comp;
        lb.Load(reader);
        EXPECT_EQ(reader.Tell(), writer.Tell())
            << "Stream desync after second Load with compression=" << (int)comp;
        EXPECT_FALSE(reader.Fail());
    }
}

TEST_F(CharBonesSamplesTest, SaveCachedFormat_SizeDeltaMatchesExpectedPadding) {
    // Uncompressed positions in cached streams write 12-byte Vector3 plus 4-byte padding.
    // Cached streams also add per-sample alignment padding to 16-byte boundaries.
    CharBonesSamples samples;
    SetupSimpleSamples(samples, 1, 2, 3, CharBones::kCompressRots);

    MemBinStream nonCachedWriter(false);
    samples.Save(nonCachedWriter);

    CachedMemBinStream cachedWriter(false, kPlatformXBox);
    samples.Save(cachedWriter);

    int vecCount = samples.GetCount(CharBones::TYPE_QUAT) - samples.GetCount(CharBones::TYPE_POS);
    int dataSize = samples.GetOffset(CharBones::TYPE_END) - samples.GetOffset(CharBones::TYPE_POS);
    int deltaPerSample = ((dataSize + 0xF) & ~0xF) - dataSize;
    int expectedExtra = (vecCount * 4 + deltaPerSample) * samples.NumSamples();

    EXPECT_EQ(cachedWriter.Size() - nonCachedWriter.Size(), expectedExtra);

    CachedMemBinStream reader(cachedWriter.Buffer(), cachedWriter.Size(), false, kPlatformXBox);
    CharBonesSamples loaded;
    loaded.Load(reader);
    EXPECT_EQ(reader.Tell(), cachedWriter.Size());
    EXPECT_FALSE(reader.Fail());
    EXPECT_EQ(loaded.NumSamples(), samples.NumSamples());
}

TEST_F(CharBonesSamplesTest, TwoConsecutiveLoads_CachedUncompressed_NoDesync) {
    // Reproduce CharClip mFull+mOne sequential load in cached mode with
    // uncompressed positions (kCompressRots), which triggers cachedPaddingMismatch path.
    CharBonesSamples full, one;
    SetupSimpleSamples(full, 1, 2, 4, CharBones::kCompressRots);
    SetupSimpleSamples(one, 3, 1, 2, CharBones::kCompressRots);

    CachedMemBinStream writer(false, kPlatformXBox);
    full.Save(writer);
    int afterFull = writer.Tell();
    one.Save(writer);
    int afterOne = writer.Tell();

    CachedMemBinStream reader(writer.Buffer(), writer.Size(), false, kPlatformXBox);
    CharBonesSamples loadedFull, loadedOne;

    loadedFull.Load(reader);
    EXPECT_EQ(reader.Tell(), afterFull) << "cached mFull.Load consumed wrong bytes";
    EXPECT_FALSE(reader.Fail());

    loadedOne.Load(reader);
    EXPECT_EQ(reader.Tell(), afterOne) << "cached mOne.Load consumed wrong bytes";
    EXPECT_FALSE(reader.Fail());
}

TEST_F(CharBonesSamplesTest, TwoConsecutiveLoads_CachedCompressedVects_NoDesync) {
    // Cached mode with compressed vectors should take the bulk-read path.
    CharBonesSamples full, one;
    SetupSimpleSamples(full, 2, 2, 3, CharBones::kCompressVects);
    SetupSimpleSamples(one, 1, 1, 5, CharBones::kCompressVects);

    CachedMemBinStream writer(false, kPlatformXBox);
    full.Save(writer);
    int afterFull = writer.Tell();
    one.Save(writer);
    int afterOne = writer.Tell();

    CachedMemBinStream reader(writer.Buffer(), writer.Size(), false, kPlatformXBox);
    CharBonesSamples loadedFull, loadedOne;

    loadedFull.Load(reader);
    EXPECT_EQ(reader.Tell(), afterFull) << "cached compressed mFull.Load consumed wrong bytes";
    EXPECT_FALSE(reader.Fail());

    loadedOne.Load(reader);
    EXPECT_EQ(reader.Tell(), afterOne) << "cached compressed mOne.Load consumed wrong bytes";
    EXPECT_FALSE(reader.Fail());
}
