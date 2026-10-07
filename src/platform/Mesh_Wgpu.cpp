// DC3 Native Port — WebGPU Mesh Drawing
// Provides RndMesh::DrawShowing() implementation for native build.
// Uses side tables for GPU vertex/index buffers.
// Supports both static and bone-skinned meshes.

#include "platform/MeshGpuCache.h"
#include "platform/Rnd_Wgpu.h"
#include "platform/BoneSetup.h"
#include "platform/MaterialSetup.h"
#include "platform/MeshDrawShowing.h"
#include "platform/MeshFilter.h"
#include "platform/TransformUtils.h"
#include "platform/TexGpu.h"
#include "gfx/FrameCapture.h"
#include "gfx/VertexFormats.h"
#include "rndobj/Mesh.h"
#include "rndobj/Mat.h"
#include "platform/rndshape/RndShape.h"
#include "rndobj/Cam.h"
#include "rndobj/Rnd.h"
#include "math/Mtx.h"
#include <cstdio>
#include <cstring>

// Forward declaration — draws a mesh immediately (called for both opaque and deferred).
// Non-static: TransparentQueue.cpp calls this via extern linkage.
void DrawMeshImmediate(RndMesh* mesh);

// Record a draw call for frame capture diagnostics.
// Encapsulates NDC projection, texture binding info, and material uniform snapshot.
static void RecordDrawCall(
    RndMesh* mesh, RndMat* rawMat,
    const MaterialParams& matParams,
    bool skinned, int blend,
    uint32_t heuristics)
{
    auto mat = rndshape::Mat(rawMat);
    auto& rec = FrameCapture::Get().AddDraw();
    rec.meshName = MeshLabel(mesh);
    rec.materialName = mat->Name();
    RndCam* cam = RndCam::Current();
    rec.cameraName = cam ? cam->Name() : nullptr;
    rec.blend = blend;
    rec.zMode = mat->GetZMode();
    rec.cull = mat->GetCull();
    rec.stencil = mat->GetStencil();
    rec.skinned = skinned;
    rec.alphaCut = mat->GetAlphaCut();
    rec.alphaWrite = mat->GetAlphaWrite();
    memcpy(rec.color, matParams.uniforms.color, sizeof(rec.color));
    rec.specularPower = matParams.uniforms.specularPower;
    rec.emissiveMultiplier = matParams.uniforms.emissiveMultiplier;
    rec.prelit = matParams.uniforms.prelit;
    rec.useTexture = matParams.uniforms.useTexture;
    rec.alpha = matParams.uniforms.color[3];
    const Vector3& worldPos = mesh->WorldXfm().v;
    rec.worldPos[0] = worldPos.x;
    rec.worldPos[1] = worldPos.y;
    rec.worldPos[2] = worldPos.z;
    rec.hasNdcPos = false;
    if (cam) {
        cam->UpdatedWorldXfm();
        Transform viewXfm;
        Hmx::Matrix4 projMtx;
        rndshape::CamViewProjectXfms(cam, viewXfm, projMtx);

        float view[16] = {
            viewXfm.m.x.x, viewXfm.m.x.y, viewXfm.m.x.z, 0.0f,
            viewXfm.m.y.x, viewXfm.m.y.y, viewXfm.m.y.z, 0.0f,
            viewXfm.m.z.x, viewXfm.m.z.y, viewXfm.m.z.z, 0.0f,
            viewXfm.v.x,   viewXfm.v.y,   viewXfm.v.z,   1.0f
        };
        float proj[16] = {
            projMtx.x.x, projMtx.x.y, projMtx.x.z, projMtx.x.w,
            projMtx.y.x, projMtx.y.y, projMtx.y.z, projMtx.y.w,
            projMtx.z.x, projMtx.z.y, projMtx.z.z, projMtx.z.w,
            projMtx.w.x, projMtx.w.y, projMtx.w.z, projMtx.w.w,
        };
        float viewProj[16];
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                float sum = 0.0f;
                for (int k = 0; k < 4; k++) {
                    sum += view[i * 4 + k] * proj[k * 4 + j];
                }
                viewProj[i * 4 + j] = sum;
            }
        }

        float clip[4];
        float pos[4] = {worldPos.x, worldPos.y, worldPos.z, 1.0f};
        for (int j = 0; j < 4; j++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += pos[k] * viewProj[k * 4 + j];
            }
            clip[j] = sum;
        }
        if (clip[3] != 0.0f) {
            rec.ndcPos[0] = clip[0] / clip[3];
            rec.ndcPos[1] = clip[1] / clip[3];
            rec.ndcPos[2] = clip[2] / clip[3];
            rec.hasNdcPos = true;
        }
    }
    rec.heuristicsApplied = heuristics;
    // Texture binding info
    const char* slotNames[] = {"diffuse","normal","specular","emissive","rim","environCube","normDetail"};
    RndTex* slotSources[] = {
        mat->GetDiffuseTex(), mat->NormalMap(), mat->GetSpecularMap(),
        mat->GetEmissiveMap(), mat->GetRimMap(), nullptr, mat->GetNormDetailMap()
    };
    const wgpu::TextureView* slotViews[] = {
        &matParams.texViews.diffuse, &matParams.texViews.normal, &matParams.texViews.specular,
        &matParams.texViews.emissive, &matParams.texViews.rim, &matParams.texViews.environCube, &matParams.texViews.normDetail
    };
    for (int t = 0; t < 7; t++) {
        rec.texBindings[t].slotName = slotNames[t];
        rec.texBindings[t].source = slotSources[t];
        rec.texBindings[t].uploaded = (*slotViews[t]) != nullptr;
        rec.texBindings[t].usingFallback = slotSources[t] && !GetGpuTexView(slotSources[t]);
    }
}

const char* RndMeshDrawShowingSkip(RndMesh* mesh) {
    // No Showing() test, deliberately. The image's override is
    // DxMesh::DrawShowing (826229B0; 100% matched in dc3-decomp's
    // src/system/rnddx9/Mesh.cpp), and its only refusal is `!geom->CanDraw()`.
    // Showing() is gated one level up, in RndDrawable::Draw(). Everything that
    // calls DrawShowing() DIRECTLY draws the mesh whatever its flag says --
    // UIListMeshElement::Draw on a list's hidden template mesh (the case that
    // made dc3 carry a SetShowing(true)/restore workaround), RndText, RndLine,
    // RndRibbon, RndMultiMeshProxy, CharFeedback. A native-only test here
    // dropped every hidden *named* mesh on those paths.
    //
    // Tools that walk an ObjectDir and call DrawShowing() on every mesh (the
    // viewers, render-test, rb3-xenon's main_render) are bypassing Draw(), so
    // they must apply the showing gate themselves -- and do.

    // Content filters are CONSUMER policy, not engine semantics, and the engine
    // already owns a seam for them: ShouldSkipMesh (platform/MeshFilter.h), which
    // every consumer defines for itself. Two game-specific name tests used to be
    // hardcoded here instead -- a `strstr(Name(), "_lod")` LOD skip and a
    // `grid_80by60` Kinect skip. Both are DC3 assumptions, and the LOD one is
    // wrong for RB3: its crowd characters are authored *as* their LOD-2 asset
    // (char/crowd/gen/crowd_female01 ships one body mesh,
    // female_crowd_body01_lod02.mesh), so the blanket test deleted the whole
    // character. They now live in DC3's MeshFilter.cpp, where the consumer that
    // wants them can keep them and the consumers that do not are not taxed.
    //
    // DrawMeshImmediate calls ShouldSkipMesh again; it is a pure name/material
    // predicate, so the second call is free. Testing here as well keeps the skip
    // ahead of IncrementMeshDrawCalls, so the draw-call counter is unchanged.
    if (ShouldSkipMesh(mesh->Name(), mesh->Mat())) return "filtered by consumer";

    // No material test either. DxMesh::DrawShowing hands a null Mat() to
    // RndShader::SelectConfig, and RndShaderStandard::Select replaces it with
    // TheRnd.DefaultMat() (rb3-xenon 0x82738E38 and 0x824A8080, the latter 100%
    // matched; dc3-decomp's DxMesh::DrawShowing and Select are the same), so the
    // image draws a material-less mesh, colour and depth, with the default
    // material. DrawMeshImmediate does that substitution.
    return nullptr;
}

void RndMesh::DrawShowing() {
    if (!gWgpuRnd || !gWgpuRnd->IsInPass()) return;

    if (const char* skip = RndMeshDrawShowingSkip(this)) {
        if (FrameCapture::Get().IsCapturing()) FrameCapture::Get().AddSkip(Name(), skip);
        return;
    }

    IncrementMeshDrawCalls();
    DrawMeshImmediate(this);
}

// Draw-log identity of a uniform binding: which buffer, at which offset. Never
// dereferenced; equal tokens mean the draws bound the same uniform data.
static uint64_t DrawLogToken(const void* buffer, uint32_t offset) {
    uint64_t h = (uint64_t)(uintptr_t)buffer;
    h ^= (uint64_t)offset + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

void DrawMeshImmediate(RndMesh* mesh) {
    if (!gWgpuRnd || !gWgpuRnd->IsInPass()) return;

    bool capturing = FrameCapture::Get().IsCapturing();
    uint32_t heuristics = 0;

    // Re-upload scene uniforms if camera changed (e.g., UI camera vs world camera)
    gWgpuRnd->EnsureSceneUniformsCurrent();

    // DC3 BaseMaterial getters, whatever the rndobj shape (platform/rndshape/).
    // A material-less mesh draws with the renderer's default material, as the
    // image's RndShaderStandard::Select does (see RndMeshDrawShowingSkip).
    RndMat* rawMat = mesh->Mat();
    if (!rawMat) rawMat = rndshape::DefaultMat();
    auto mat = rndshape::Mat(rawMat);
    if (!mat) {
        if (capturing) FrameCapture::Get().AddSkip(MeshLabel(mesh), "no material");
        return;
    }

    if (ShouldSkipMesh(mesh->Name(), mat)) return;

    // Ensure mesh data is on GPU
    if (!EnsureMeshUploaded(mesh)) {
        if (capturing) FrameCapture::Get().AddSkip(MeshLabel(mesh), "upload failed");
        return;
    }

    auto& meshData = *GetMeshGpuData(mesh);
    auto& pass = gWgpuRnd->CurrentPass();
    bool skinned = meshData.skinned;

    // Text meshes (created by RndText::FontMap) have empty names (not registered in ObjectDir).
    // Debug labels are stored in GpuMeshData::debugLabel for GPU debugging.
    bool isTextMesh = !mesh->Name()[0];

    // --- Pipeline selection ---
    PipelineKey key{};
    key.shaderType = 18; // kStandardShader

    BaseMaterial::Blend matBlend = mat->GetBlend();

    // Overlay pass (no depth buffer) = HUD/2D elements. Disable face culling
    // because Xbox D3D9 uses CW front face while WebGPU uses CCW. HUD quads
    // authored for CW winding are back-facing in CCW and would be culled.
    bool isOverlayPass = !gWgpuRnd->CurrentPassHasDepth();

    key.blend = (WgpuBlend)matBlend;
    key.zMode = isTextMesh ? (WgpuZMode)0 : (WgpuZMode)mat->GetZMode(); // No depth for text
    WgpuCull matCull = (isTextMesh || isOverlayPass) ? WgpuCull::None : (WgpuCull)mat->GetCull();
    // Reflection mode (rndshape::kDrawModeReflection) flips the camera, reversing winding order.
    // Flip cull mode so front faces aren't discarded.
    if (rndshape::TheRndRef().DrawMode() == rndshape::kDrawModeReflection && matCull != WgpuCull::None) {
        matCull = (matCull == WgpuCull::Regular) ? WgpuCull::Backwards : WgpuCull::Regular;
    }
    key.cull = matCull;
    key.stencil = (WgpuStencil)mat->GetStencil();
    key.layout = skinned ? VertexLayoutType::Skinned : VertexLayoutType::Static;
    key.targetFormat = gWgpuRnd->CurrentTargetFormat();
    key.sampleCount = gWgpuRnd->CurrentSampleCount();
    key.hasDepth = gWgpuRnd->CurrentPassHasDepth();
    key.alphaCut = mat->GetAlphaCut();
    key.alphaWrite = mat->GetAlphaWrite();
    if (rndshape::kRenderTargetForcesAlphaWrite && gWgpuRnd->ActiveTargetTex())
        key.alphaWrite = true;
    // RB3's pseudo-HDR bloom mask (rndshape::BloomMaskScale; 0 for DC3): an
    // AllowHDR material drawn into the main frame writes its luma to alpha.
    const float bloomMaskScale = (!isOverlayPass && !gWgpuRnd->ActiveTargetTex())
        ? rndshape::BloomMaskScale(mat) : 0.0f;
    if (bloomMaskScale > 0.0f)
        key.alphaWrite = true;
    key.alphaToCoverage = mat->GetAlphaCut();
    key.depthBias = meshData.depthBias;

    wgpu::RenderPipeline pipeline = gWgpuRnd->Pipelines().GetPipeline(key);
    if (!pipeline) return;

    pass.SetPipeline(pipeline);

    // --- Material (group 1) ---
    MaterialParams matParams = BuildMaterialParams(mat, isTextMesh);
    matParams.uniforms.bloomMaskScale = bloomMaskScale;
    heuristics |= matParams.heuristics;

    uint32_t matOffset = gWgpuRnd->MaterialRing().Write(
        gWgpuRnd->Gpu().Queue(), &matParams.uniforms, sizeof(matParams.uniforms));

    wgpu::Sampler sampler = gWgpuRnd->Gpu().GetSampler(matParams.samplerDesc);
    wgpu::Sampler mapSampler = gWgpuRnd->Gpu().GetSampler(matParams.mapSamplerDesc);

    wgpu::BindGroup matBG = gWgpuRnd->CreateMaterialBindGroup(
        matOffset, sizeof(MaterialUniforms), matParams.texViews, sampler, mapSampler);
    pass.SetBindGroup(1, matBG);

    // --- Object uniforms (group 2) ---
    ObjectUniforms objUni{};
    if (skinned) {
        // Skinned: bone matrices already produce world-space positions,
        // so object transform must be identity to avoid double-transform
        Transform identity;
        identity.Reset();
        FillObjectUniforms(identity, objUni);
    } else {
        FillObjectUniforms(mesh->WorldXfm(), objUni);
    }
    // RB3's retail approx-light box map for this mesh (no-op for DC3).
    rndshape::FillMeshApproxLighting(mesh, objUni.boxLight, objUni.retail);

    uint32_t objOffset = gWgpuRnd->ObjectRing().Write(
        gWgpuRnd->Gpu().Queue(), &objUni, sizeof(objUni));

    wgpu::BindGroup objBG = gWgpuRnd->CreateObjectBindGroup(
        objOffset, sizeof(ObjectUniforms));
    pass.SetBindGroup(2, objBG);

    // --- Bone uniforms (group 3) ---
    uint64_t boneToken = 0;   // draw-log identity of the bone data bound
    if (skinned) {
        BoneUniforms boneUni{};
        FillBoneUniforms(mesh, boneUni);

        uint32_t boneOffset = gWgpuRnd->BoneRing().Write(
            gWgpuRnd->Gpu().Queue(), &boneUni, sizeof(boneUni));

        wgpu::BindGroup boneBG = gWgpuRnd->CreateBoneBindGroup(
            boneOffset, sizeof(BoneUniforms));
        pass.SetBindGroup(3, boneBG);
        if (rndshape::DrawLogActive())
            boneToken = DrawLogToken(gWgpuRnd->BoneRing().Buffer().Get(), boneOffset);
    } else {
        // Static mesh: bind dummy bone bind group (pipeline layout requires group 3)
        EnsureDummyBoneBindGroup();
        pass.SetBindGroup(3, GetDummyBoneBindGroup());
        if (rndshape::DrawLogActive())
            boneToken = DrawLogToken(GetDummyBoneBindGroup().Get(), 0);
    }

    // --- Capture record ---
    if (capturing) {
        RecordDrawCall(mesh, mat, matParams, skinned, (int)matBlend, heuristics);
    }

    // --- Draw ---
    size_t vertexSize = skinned ? sizeof(GpuVertexSkinned) : sizeof(GpuVertex);
    pass.SetVertexBuffer(0, meshData.vertexBuffer, 0, meshData.numVertices * vertexSize);
    pass.SetIndexBuffer(meshData.indexBuffer, wgpu::IndexFormat::Uint16, 0,
                        meshData.numIndices * sizeof(uint16_t));

    pass.DrawIndexed(meshData.numIndices);

    // --- Per-draw state log (RB3's RB3_DRAWLOG; off for DC3) ---
    if (rndshape::DrawLogActive()) {
        rndshape::DrawLogDraw d;
        d.pipelineHash = (uint64_t)PipelineKeyHash{}(key);
        d.blend = (uint8_t)(int)key.blend;
        d.zMode = (uint8_t)(int)key.zMode;
        d.layout = (uint8_t)(int)key.layout;
        d.hasDepth = key.hasDepth;
        d.alphaCut = key.alphaCut;
        d.alphaWrite = key.alphaWrite;
        d.skinned = skinned;
        d.targetFormat = (uint32_t)key.targetFormat;
        d.indexCount = meshData.numIndices;
        d.triCount = meshData.numIndices / 3;
        d.vertCount = meshData.numVertices;
        d.mesh = mesh;
        d.mat = rawMat;
        d.world = objUni.world;
        d.viewProj = gWgpuRnd->LastSceneViewProj();
        d.boundColor = matParams.uniforms.color;
        d.viewportW = (float)gWgpuRnd->Gpu().WindowWidth();
        d.viewportH = (float)gWgpuRnd->Gpu().WindowHeight();
        d.sceneToken = DrawLogToken(gWgpuRnd->SceneBuffer().Get(), gWgpuRnd->SceneOffset());
        d.matToken = DrawLogToken(gWgpuRnd->MaterialRing().Buffer().Get(), matOffset);
        d.objToken = DrawLogToken(gWgpuRnd->ObjectRing().Buffer().Get(), objOffset);
        d.boneToken = boneToken;
        rndshape::DrawLogRecord(d);
    }

    // --- Multi-pass materials ---
    // Walk the NextPass chain and draw additional passes with the same geometry
    BaseMaterial* nextPass = mat->NextPass();
    while (nextPass) {
        // Pipeline may differ (blend mode, z mode, etc.)
        PipelineKey npKey = key;
        npKey.blend = (WgpuBlend)rndshape::Mat(nextPass)->GetBlend();
        npKey.zMode = (WgpuZMode)rndshape::Mat(nextPass)->GetZMode();
        WgpuCull npCull = (WgpuCull)rndshape::Mat(nextPass)->GetCull();
        if (rndshape::TheRndRef().DrawMode() == rndshape::kDrawModeReflection && npCull != WgpuCull::None) {
            npCull = (npCull == WgpuCull::Regular) ? WgpuCull::Backwards : WgpuCull::Regular;
        }
        npKey.cull = npCull;
        npKey.stencil = (WgpuStencil)rndshape::Mat(nextPass)->GetStencil();
        npKey.alphaCut = rndshape::Mat(nextPass)->GetAlphaCut();
        npKey.alphaWrite = rndshape::Mat(nextPass)->GetAlphaWrite();
        if (rndshape::kRenderTargetForcesAlphaWrite && gWgpuRnd->ActiveTargetTex())
            npKey.alphaWrite = true;
        npKey.alphaToCoverage = rndshape::Mat(nextPass)->GetAlphaCut();

        wgpu::RenderPipeline npPipeline = gWgpuRnd->Pipelines().GetPipeline(npKey);
        if (npPipeline) {
            pass.SetPipeline(npPipeline);

            MaterialParams npParams = BuildPassMaterialParams(nextPass);

            uint32_t npMatOffset = gWgpuRnd->MaterialRing().Write(
                gWgpuRnd->Gpu().Queue(), &npParams.uniforms, sizeof(npParams.uniforms));

            wgpu::BindGroup npMatBG = gWgpuRnd->CreateMaterialBindGroup(
                npMatOffset, sizeof(MaterialUniforms), npParams.texViews, sampler, mapSampler);
            pass.SetBindGroup(1, npMatBG);

            // Object + bone bind groups unchanged, just re-draw
            pass.DrawIndexed(meshData.numIndices);
        }
        nextPass = rndshape::Mat(nextPass)->NextPass();
    }
}

// ============================================================================
// Shadow depth drawing — simplified path for shadow map generation
// ============================================================================

void DrawMeshShadow(RndMesh* mesh) {
    if (!gWgpuRnd || !gWgpuRnd->InShadowPass()) return;

    // Ensure mesh data is on GPU
    if (!EnsureMeshUploaded(mesh)) return;

    auto& meshData = *GetMeshGpuData(mesh);
    auto& pass = gWgpuRnd->ShadowRenderPass();
    bool skinned = meshData.skinned;

    // Select shadow pipeline
    if (skinned) {
        pass.SetPipeline(gWgpuRnd->ShadowSkinnedPipeline());
    } else {
        pass.SetPipeline(gWgpuRnd->ShadowStaticPipeline());
    }

    // Object uniforms (group 1) — world matrix
    ObjectUniforms objUni{};
    if (skinned) {
        Transform identity;
        identity.Reset();
        FillObjectUniforms(identity, objUni);
    } else {
        FillObjectUniforms(mesh->WorldXfm(), objUni);
    }

    uint32_t objOffset = gWgpuRnd->ObjectRing().Write(
        gWgpuRnd->Gpu().Queue(), &objUni, sizeof(objUni));

    // Create object bind group using shadow-specific layout
    {
        wgpu::BindGroupEntry entry{};
        entry.binding = 0;
        entry.buffer = gWgpuRnd->ObjectRing().Buffer();
        entry.offset = objOffset;
        entry.size = sizeof(ObjectUniforms);

        wgpu::BindGroupDescriptor bgDesc{};
        bgDesc.layout = gWgpuRnd->ShadowObjectBGL();
        bgDesc.entryCount = 1;
        bgDesc.entries = &entry;
        wgpu::BindGroup objBG = gWgpuRnd->Gpu().Device().CreateBindGroup(&bgDesc);
        pass.SetBindGroup(1, objBG);
    }

    // Bone uniforms (group 2) — only for skinned
    if (skinned) {
        BoneUniforms boneUni{};
        FillBoneUniforms(mesh, boneUni);

        uint32_t boneOffset = gWgpuRnd->BoneRing().Write(
            gWgpuRnd->Gpu().Queue(), &boneUni, sizeof(boneUni));

        wgpu::BindGroupEntry entry{};
        entry.binding = 0;
        entry.buffer = gWgpuRnd->BoneRing().Buffer();
        entry.offset = boneOffset;
        entry.size = sizeof(BoneUniforms);

        wgpu::BindGroupDescriptor bgDesc{};
        bgDesc.layout = gWgpuRnd->ShadowBoneBGL();
        bgDesc.entryCount = 1;
        bgDesc.entries = &entry;
        wgpu::BindGroup boneBG = gWgpuRnd->Gpu().Device().CreateBindGroup(&bgDesc);
        pass.SetBindGroup(2, boneBG);
    }

    // Draw
    size_t vertexSize = skinned ? sizeof(GpuVertexSkinned) : sizeof(GpuVertex);
    pass.SetVertexBuffer(0, meshData.vertexBuffer, 0, meshData.numVertices * vertexSize);
    pass.SetIndexBuffer(meshData.indexBuffer, wgpu::IndexFormat::Uint16, 0,
                        meshData.numIndices * sizeof(uint16_t));
    pass.DrawIndexed(meshData.numIndices);
}
