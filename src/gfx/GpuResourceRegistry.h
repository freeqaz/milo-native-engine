#pragma once
#include <webgpu/webgpu_cpp.h>
#include <unordered_map>

class RndMesh;
class RndTex;
class RndCubeTex;

// The registry's record types are NESTED on purpose.  They used to be the
// namespace-scope GpuMeshData / GpuTexData / GpuCubeTexData, which
// platform/MeshGpuCache.h and platform/Tex_Wgpu.cpp also define, with
// different members.  Two definitions of one class in one program is an ODR
// violation, and here a live one: both sides instantiate
// std::unordered_map<RndMesh*, GpuMeshData> (and the RndTex one), the linker
// keeps ONE copy of each of its member functions, and the copy kept may size
// the node for the other definition.  Found by rb3-xenon's
// tools/layout_odr.py (rb3-render and rb3-frame).
class GpuResourceRegistry {
public:
    struct MeshEntry {
        wgpu::Buffer vertexBuffer;
        wgpu::Buffer indexBuffer;
        int numIndices = 0;
        int numVertices = 0;
        bool skinned = false;
        bool uploaded = false;
        int32_t depthBias = 0;
    };

    struct TexEntry {
        wgpu::Texture texture;
        wgpu::TextureView view;
        bool uploaded = false;
    };

    struct CubeTexEntry {
        wgpu::Texture texture;
        wgpu::TextureView view;
        bool uploaded = false;
    };

    static GpuResourceRegistry& Get();

    // Mesh
    MeshEntry* FindMesh(RndMesh* mesh);
    MeshEntry& GetOrCreateMesh(RndMesh* mesh);
    void InvalidateMesh(RndMesh* mesh);
    void RemoveMesh(RndMesh* mesh);

    // Texture
    TexEntry* FindTex(RndTex* tex);
    TexEntry& GetOrCreateTex(RndTex* tex);
    void RemoveTexture(RndTex* tex);

    // Cube texture
    CubeTexEntry* FindCubeTex(RndCubeTex* tex);
    CubeTexEntry& GetOrCreateCubeTex(RndCubeTex* tex);
    void RemoveCubeTexture(RndCubeTex* tex);

    // Debug
    int MeshCount() const { return (int)mMeshData.size(); }
    int TextureCount() const { return (int)mTexData.size(); }
    int CubeTextureCount() const { return (int)mCubeTexData.size(); }
    void DumpStats();

private:
    std::unordered_map<RndMesh*, MeshEntry> mMeshData;
    std::unordered_map<RndTex*, TexEntry> mTexData;
    std::unordered_map<RndCubeTex*, CubeTexEntry> mCubeTexData;
};
