#include "test_helpers.h"

#include "gfx/VertexFormats.h"
#include "math/Rot.h"
#include "platform/TransformUtils.h"
#include "rndobj/Mesh.h"
#include "rndobj/MeshVertCompress.h"

BinStreamRev &operator>>(BinStreamRev &, RndMesh::Vert &);

namespace {

class MeshVertexLoading : public SymbolTestFixture {};

class TestMesh : public RndMesh {
public:
    TestMesh() : RndMesh() {}
    using RndMesh::LoadVertices;
    using RndMesh::SetNumVerts;
};

class TestTrans : public RndTransformable {
public:
    TestTrans() : RndTransformable() {}
};

static std::vector<uint8_t> MakeCompressedVertexRecord(
    uint32_t packedWeights,
    uint32_t packedIndices
) {
    std::vector<uint8_t> buf;
    buf.reserve(sizeof(CompressedVertex_Xbox));
    PutBEFloat(buf, 1.0f);
    PutBEFloat(buf, 2.0f);
    PutBEFloat(buf, 3.0f);
    PutBE32(buf, 0xFF7F3F1F);
    PutBE32(buf, 0);
    PutBE32(buf, 0);
    PutBE32(buf, 0);
    PutBE32(buf, packedWeights);
    PutBE32(buf, packedIndices);
    return buf;
}

static std::vector<uint8_t> SerializeCompressedVertexBE(const CompressedVertex_Xbox &cv) {
    std::vector<uint8_t> buf;
    buf.reserve(sizeof(CompressedVertex_Xbox));
    PutBEFloat(buf, cv.mPosX);
    PutBEFloat(buf, cv.mPosY);
    PutBEFloat(buf, cv.mPosZ);
    PutBE32(buf, (uint32_t)cv.mColor);
    PutBE32(buf, (uint32_t)cv.mNormal);
    PutBE32(buf, (uint32_t)cv.mTangent);
    PutBE32(buf, (uint32_t)cv.mBinormal);
    PutBE32(buf, (uint32_t)cv.mBoneIndices);
    PutBE32(buf, (uint32_t)cv.mBoneWeights);
    return buf;
}

static Vector3 ApplyGpuMatrix(const float *m, const Vector3 &p) {
    return Vector3(
        m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
        m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
        m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]
    );
}

} // namespace

TEST_F(MeshVertexLoading, NativeCompressedLoadPreservesRawBlob) {
    TestMesh mesh;

    const uint32_t packedWeights = 1023u;
    const uint32_t packedIndices = 2u | (3u << 8) | (4u << 16) | (5u << 24);
    std::vector<uint8_t> record = MakeCompressedVertexRecord(packedWeights, packedIndices);

    std::vector<uint8_t> streamBytes;
    PutBE32(streamBytes, 1);
    streamBytes.push_back(1);
    PutBE32(streamBytes, sizeof(CompressedVertex_Xbox));
    PutBE32(streamBytes, 1);
    streamBytes.insert(streamBytes.end(), record.begin(), record.end());

    MemBinStream ms(streamBytes.data(), (int)streamBytes.size(), false);
    BinStreamRev rev(ms, 0x26);
    mesh.LoadVertices(rev);

    EXPECT_EQ(mesh.NumVerts(), 0);
    EXPECT_EQ(mesh.NumCompressedVerts(), 1u);
    ASSERT_NE(mesh.CompressedVerts(), nullptr);
    EXPECT_EQ(memcmp(mesh.CompressedVerts(), record.data(), sizeof(CompressedVertex_Xbox)), 0);
}

TEST_F(MeshVertexLoading, CompressedSkinnedDecodePreservesBoneWeightsAndIndices) {
    const uint32_t packedWeights = 1023u;
    const uint32_t packedIndices = 2u | (3u << 8) | (4u << 16) | (5u << 24);
    std::vector<uint8_t> record = MakeCompressedVertexRecord(packedWeights, packedIndices);

    GpuVertexSkinned out{};
    ASSERT_EQ(
        VertexFormats::UnpackCompressedSkinnedVertices(
            record.data(), 1, &out, 1
        ),
        1
    );

    EXPECT_FLOAT_EQ(out.pos[0], 1.0f);
    EXPECT_FLOAT_EQ(out.pos[1], 2.0f);
    EXPECT_FLOAT_EQ(out.pos[2], 3.0f);
    EXPECT_FLOAT_EQ(out.boneWeights[0], 1.0f);
    EXPECT_FLOAT_EQ(out.boneWeights[1], 0.0f);
    EXPECT_FLOAT_EQ(out.boneWeights[2], 0.0f);
    EXPECT_FLOAT_EQ(out.boneWeights[3], 0.0f);
    EXPECT_EQ(out.boneIndices[0], 2);
    EXPECT_EQ(out.boneIndices[1], 3);
    EXPECT_EQ(out.boneIndices[2], 4);
    EXPECT_EQ(out.boneIndices[3], 5);
}

TEST_F(MeshVertexLoading, UncompressedVertRev26ReadsWeightsAndIndices) {
    std::vector<uint8_t> buf;
    PutBEFloat(buf, 1.0f);
    PutBEFloat(buf, 2.0f);
    PutBEFloat(buf, 3.0f);
    PutBEFloat(buf, 4.0f);
    PutBEFloat(buf, 5.0f);
    PutBEFloat(buf, 6.0f);
    PutBEFloat(buf, 0.1f);
    PutBEFloat(buf, 0.2f);
    PutBEFloat(buf, 0.3f);
    PutBEFloat(buf, 0.4f);
    PutBEFloat(buf, 0.5f);
    PutBEFloat(buf, 0.6f);
    PutBEFloat(buf, 0.7f);
    PutBEFloat(buf, 0.2f);
    PutBEFloat(buf, 0.1f);
    PutBEFloat(buf, 0.0f);
    PutBE16(buf, 1);
    PutBE16(buf, 2);
    PutBE16(buf, 3);
    PutBE16(buf, 4);
    PutBEFloat(buf, 1.0f);
    PutBEFloat(buf, 0.0f);
    PutBEFloat(buf, 0.0f);
    PutBEFloat(buf, 1.0f);

    MemBinStream ms(buf.data(), (int)buf.size(), false);
    BinStreamRev rev(ms, 0x26);
    RndMesh::Vert vert;
    rev >> vert;

    EXPECT_FLOAT_EQ(vert.boneWeights.x, 0.7f);
    EXPECT_FLOAT_EQ(vert.boneWeights.y, 0.2f);
    EXPECT_FLOAT_EQ(vert.boneWeights.z, 0.1f);
    EXPECT_FLOAT_EQ(vert.boneWeights.w, 0.0f);
    EXPECT_EQ(vert.boneIndices[0], 1);
    EXPECT_EQ(vert.boneIndices[1], 2);
    EXPECT_EQ(vert.boneIndices[2], 3);
    EXPECT_EQ(vert.boneIndices[3], 4);
}

TEST_F(MeshVertexLoading, CompressedSkinningMatchesCpuSkinningForSyntheticBones) {
    TestMesh mesh;
    mesh.SetNumBones(2);

    TestTrans bone0;
    TestTrans bone1;

    Transform tf0;
    tf0.Reset();
    tf0.v.Set(10.0f, 0.0f, 0.0f);
    bone0.SetWorldXfm(tf0);

    Transform tf1;
    tf1.Reset();
    MakeRotMatrixZ(0.7f, tf1.m);
    tf1.v.Set(0.0f, 20.0f, 5.0f);
    bone1.SetWorldXfm(tf1);

    mesh.SetBone(0, &bone0, false);
    mesh.SetBone(1, &bone1, false);

    RndMesh::Vert vert{};
    vert.pos.Set(1.0f, 2.0f, 3.0f);
    vert.norm.Set(0.0f, 1.0f, 0.0f);
    vert.boneWeights.Set(0.25f, 0.75f, 0.0f, 0.0f);
    vert.boneIndices[0] = 0;
    vert.boneIndices[1] = 1;
    vert.boneIndices[2] = 0;
    vert.boneIndices[3] = 0;

    CompressedVertex_Xbox cv{};
    FillCompressedVertex(cv, vert, false);
    std::vector<uint8_t> bytes = SerializeCompressedVertexBE(cv);

    GpuVertexSkinned gpuVert{};
    ASSERT_EQ(VertexFormats::UnpackCompressedSkinnedVertices(
                  bytes.data(), 1, &gpuVert, 1),
              1);

    float m0[16];
    float m1[16];
    Transform boneXfm;
    Multiply(mesh.BoneOffsetAt(0), bone0.WorldXfm(), boneXfm);
    TransformToMat4(boneXfm, m0);
    Multiply(mesh.BoneOffsetAt(1), bone1.WorldXfm(), boneXfm);
    TransformToMat4(boneXfm, m1);

    Vector3 gpuSkinned(0.0f, 0.0f, 0.0f);
    Vector3 p(gpuVert.pos[0], gpuVert.pos[1], gpuVert.pos[2]);
    Vector3 t0 = ApplyGpuMatrix(m0, p);
    Vector3 t1 = ApplyGpuMatrix(m1, p);
    ScaleAddEq(gpuSkinned, t0, gpuVert.boneWeights[0]);
    ScaleAddEq(gpuSkinned, t1, gpuVert.boneWeights[1]);

    Vector3 cpuSkinned = mesh.SkinVertex(vert, nullptr);

    // Compressed skinning uses 10/10/10/2 normalized weights, so a small
    // quantization delta versus the raw CPU vertex is expected.
    EXPECT_NEAR(cpuSkinned.x, gpuSkinned.x, 0.02f);
    EXPECT_NEAR(cpuSkinned.y, gpuSkinned.y, 0.02f);
    EXPECT_NEAR(cpuSkinned.z, gpuSkinned.z, 0.02f);
}

TEST_F(MeshVertexLoading, UncompressedSkinningMatchesCpuSkinningForSyntheticBones) {
    TestMesh mesh;
    mesh.SetNumBones(2);
    mesh.SetNumVerts(1);

    TestTrans bone0;
    TestTrans bone1;

    Transform tf0;
    tf0.Reset();
    tf0.v.Set(10.0f, 0.0f, 0.0f);
    bone0.SetWorldXfm(tf0);

    Transform tf1;
    tf1.Reset();
    MakeRotMatrixZ(0.7f, tf1.m);
    tf1.v.Set(0.0f, 20.0f, 5.0f);
    bone1.SetWorldXfm(tf1);

    mesh.SetBone(0, &bone0, false);
    mesh.SetBone(1, &bone1, false);

    RndMesh::Vert &vert = mesh.Verts(0);
    vert.pos.Set(1.0f, 2.0f, 3.0f);
    vert.norm.Set(0.0f, 1.0f, 0.0f);
    vert.boneWeights.Set(0.25f, 0.75f, 0.0f, 0.0f);
    vert.boneIndices[0] = 0;
    vert.boneIndices[1] = 1;
    vert.boneIndices[2] = 0;
    vert.boneIndices[3] = 0;

    GpuVertexSkinned gpuVert{};
    ASSERT_EQ(VertexFormats::UnpackSkinnedVertices(mesh, &gpuVert, 1), 1);

    float m0[16];
    float m1[16];
    Transform boneXfm;
    Multiply(mesh.BoneOffsetAt(0), bone0.WorldXfm(), boneXfm);
    TransformToMat4(boneXfm, m0);
    Multiply(mesh.BoneOffsetAt(1), bone1.WorldXfm(), boneXfm);
    TransformToMat4(boneXfm, m1);

    Vector3 gpuSkinned(0.0f, 0.0f, 0.0f);
    Vector3 p(gpuVert.pos[0], gpuVert.pos[1], gpuVert.pos[2]);
    Vector3 t0 = ApplyGpuMatrix(m0, p);
    Vector3 t1 = ApplyGpuMatrix(m1, p);
    ScaleAddEq(gpuSkinned, t0, gpuVert.boneWeights[0]);
    ScaleAddEq(gpuSkinned, t1, gpuVert.boneWeights[1]);

    Vector3 cpuSkinned = mesh.SkinVertex(vert, nullptr);

    EXPECT_NEAR(cpuSkinned.x, gpuSkinned.x, 1e-4f);
    EXPECT_NEAR(cpuSkinned.y, gpuSkinned.y, 1e-4f);
    EXPECT_NEAR(cpuSkinned.z, gpuSkinned.z, 1e-4f);
}
