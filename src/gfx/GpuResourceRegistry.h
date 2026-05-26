#pragma once
#include <webgpu/webgpu_cpp.h>
#include <unordered_map>

class RndMesh;
class RndTex;
class RndCubeTex;

struct GpuMeshData {
    wgpu::Buffer vertexBuffer;
    wgpu::Buffer indexBuffer;
    int numIndices = 0;
    int numVertices = 0;
    bool skinned = false;
    bool uploaded = false;
    int32_t depthBias = 0;
};

struct GpuTexData {
    wgpu::Texture texture;
    wgpu::TextureView view;
    bool uploaded = false;
};

struct GpuCubeTexData {
    wgpu::Texture texture;
    wgpu::TextureView view;
    bool uploaded = false;
};

class GpuResourceRegistry {
public:
    static GpuResourceRegistry& Get();

    // Mesh
    GpuMeshData* FindMesh(RndMesh* mesh);
    GpuMeshData& GetOrCreateMesh(RndMesh* mesh);
    void InvalidateMesh(RndMesh* mesh);
    void RemoveMesh(RndMesh* mesh);

    // Texture
    GpuTexData* FindTex(RndTex* tex);
    GpuTexData& GetOrCreateTex(RndTex* tex);
    void RemoveTexture(RndTex* tex);

    // Cube texture
    GpuCubeTexData* FindCubeTex(RndCubeTex* tex);
    GpuCubeTexData& GetOrCreateCubeTex(RndCubeTex* tex);
    void RemoveCubeTexture(RndCubeTex* tex);

    // Debug
    int MeshCount() const { return (int)mMeshData.size(); }
    int TextureCount() const { return (int)mTexData.size(); }
    int CubeTextureCount() const { return (int)mCubeTexData.size(); }
    void DumpStats();

private:
    std::unordered_map<RndMesh*, GpuMeshData> mMeshData;
    std::unordered_map<RndTex*, GpuTexData> mTexData;
    std::unordered_map<RndCubeTex*, GpuCubeTexData> mCubeTexData;
};
