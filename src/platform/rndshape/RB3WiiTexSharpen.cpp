// RB3WiiTexSharpen.cpp — the texture-cache hooks the progressive-sharpen
// manager (platform/RB3TexSharpen.cpp) needs, over the dc3 backend's texture
// cache (Tex_Wgpu.cpp). Built only for the dc3 GPU backend with
// MILO_ENGINE_RNDOBJ_SHAPE=rb3wii: the manager reads RB3's RndBitmap members,
// and only RB3 venues ship a stripped top mip with a .sharpen sidecar.
//
// The manager swaps a texture's RndBitmap up to full resolution and then asks
// for a re-upload. RndTex::PresyncBitmap re-creates the GPU texture whenever
// the bitmap's pixel pointer or content fingerprint changed, at the bitmap's
// current size, and the dc3 mesh path builds the material bind group per draw
// from GetGpuTexView, so the next draw binds the new view without any further
// invalidation.
#include "platform/RB3TexSharpen.h"
#include "platform/RB3TexSharpenDebug.h"
#include "platform/Rnd_Wgpu.h"
#include "platform/TexGpu.h"

#include "rndobj/Tex.h"
#include "rndobj/Bitmap.h"

uint32_t RB3SharpenTexFingerprint(const RndTex *tex) {
    if (!tex)
        return 0;
    const RndBitmap &bmp = tex->mBitmap;
    return GpuTexPixelFingerprint(bmp.Pixels(), bmp.PixelBytes());
}

static bool GpuUp() { return gWgpuRnd && gWgpuRnd->Gpu().IsReady(); }

static bool sSharpenGpuUnavailable = false;
void RB3DebugSetSharpenGpuUnavailable(bool on) { sSharpenGpuUnavailable = on; }

bool RB3SharpenReuploadTex(RndTex *tex) {
    if (!tex || !GpuUp() || sSharpenGpuUnavailable)
        return false;
    unsigned long long before = GetGpuTexDebugInfo(tex).createCount;
    tex->PresyncBitmap();
    return GetGpuTexDebugInfo(tex).createCount != before;
}

bool RB3DebugUploadTex(RndTex *tex) {
    if (!tex || !GpuUp())
        return false;
    tex->PresyncBitmap();
    return GetGpuTexDebugInfo(tex).uploaded;
}

RB3TexGpuInfo RB3DebugGetTexGpuInfo(RndTex *tex) {
    GpuTexDebugInfo d = GetGpuTexDebugInfo(tex);
    RB3TexGpuInfo info;
    info.present = d.present;
    info.uploaded = d.uploaded;
    info.texW = d.width;
    info.texH = d.height;
    info.viewPtr = d.view;
    info.texPtr = d.texture;
    info.globalRecreateCount = d.createCount;
    return info;
}
