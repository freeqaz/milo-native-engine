#include "gfx/GpuResourceRegistry.h"
#include <cstdio>

GpuResourceRegistry& GpuResourceRegistry::Get() {
    static GpuResourceRegistry sInstance;
    return sInstance;
}

GpuResourceRegistry::MeshEntry* GpuResourceRegistry::FindMesh(RndMesh* mesh) {
    auto it = mMeshData.find(mesh);
    return it != mMeshData.end() ? &it->second : nullptr;
}

GpuResourceRegistry::MeshEntry& GpuResourceRegistry::GetOrCreateMesh(RndMesh* mesh) {
    return mMeshData[mesh];
}

void GpuResourceRegistry::InvalidateMesh(RndMesh* mesh) {
    auto it = mMeshData.find(mesh);
    if (it != mMeshData.end()) {
        it->second.uploaded = false;
    }
}

void GpuResourceRegistry::RemoveMesh(RndMesh* mesh) {
    mMeshData.erase(mesh);
}

GpuResourceRegistry::TexEntry* GpuResourceRegistry::FindTex(RndTex* tex) {
    auto it = mTexData.find(tex);
    return it != mTexData.end() ? &it->second : nullptr;
}

GpuResourceRegistry::TexEntry& GpuResourceRegistry::GetOrCreateTex(RndTex* tex) {
    return mTexData[tex];
}

void GpuResourceRegistry::RemoveTexture(RndTex* tex) {
    mTexData.erase(tex);
}

GpuResourceRegistry::CubeTexEntry* GpuResourceRegistry::FindCubeTex(RndCubeTex* tex) {
    auto it = mCubeTexData.find(tex);
    return it != mCubeTexData.end() ? &it->second : nullptr;
}

GpuResourceRegistry::CubeTexEntry& GpuResourceRegistry::GetOrCreateCubeTex(RndCubeTex* tex) {
    return mCubeTexData[tex];
}

void GpuResourceRegistry::RemoveCubeTexture(RndCubeTex* tex) {
    mCubeTexData.erase(tex);
}

void GpuResourceRegistry::DumpStats() {
    fprintf(stderr, "GpuResourceRegistry: %d meshes, %d textures, %d cube textures\n",
            MeshCount(), TextureCount(), CubeTextureCount());
}
