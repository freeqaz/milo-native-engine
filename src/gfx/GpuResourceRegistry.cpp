#include "gfx/GpuResourceRegistry.h"
#include <cstdio>

GpuResourceRegistry& GpuResourceRegistry::Get() {
    static GpuResourceRegistry sInstance;
    return sInstance;
}

GpuMeshData* GpuResourceRegistry::FindMesh(RndMesh* mesh) {
    auto it = mMeshData.find(mesh);
    return it != mMeshData.end() ? &it->second : nullptr;
}

GpuMeshData& GpuResourceRegistry::GetOrCreateMesh(RndMesh* mesh) {
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

GpuTexData* GpuResourceRegistry::FindTex(RndTex* tex) {
    auto it = mTexData.find(tex);
    return it != mTexData.end() ? &it->second : nullptr;
}

GpuTexData& GpuResourceRegistry::GetOrCreateTex(RndTex* tex) {
    return mTexData[tex];
}

void GpuResourceRegistry::RemoveTexture(RndTex* tex) {
    mTexData.erase(tex);
}

GpuCubeTexData* GpuResourceRegistry::FindCubeTex(RndCubeTex* tex) {
    auto it = mCubeTexData.find(tex);
    return it != mCubeTexData.end() ? &it->second : nullptr;
}

GpuCubeTexData& GpuResourceRegistry::GetOrCreateCubeTex(RndCubeTex* tex) {
    return mCubeTexData[tex];
}

void GpuResourceRegistry::RemoveCubeTexture(RndCubeTex* tex) {
    mCubeTexData.erase(tex);
}

void GpuResourceRegistry::DumpStats() {
    fprintf(stderr, "GpuResourceRegistry: %d meshes, %d textures, %d cube textures\n",
            MeshCount(), TextureCount(), CubeTextureCount());
}
