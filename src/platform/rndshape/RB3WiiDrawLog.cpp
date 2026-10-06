// RB3WiiDrawLog.cpp — RB3's per-draw state log and provenance sidecar
// (platform/RB3DrawLogDebug.h) over the dc3 backend. Built only for the dc3 GPU
// backend with MILO_ENGINE_RNDOBJ_SHAPE=rb3wii; the DC3 shape's hooks are
// inline no-ops.
//
// The record format, the gates and the JSON dump are the ones BandRnd writes
// (Rnd_Wgpu_RB3.cpp), so RB3's golden comparator, /api/drawlog and the UI dump
// read either flavor:
//   RB3_DRAWLOG=1        record every mesh draw into the frame's log
//   RB3_DRAWLOG_DUMP=f   write the log as JSON to f at the end of each frame
//   RB3_DRAWLOG_PROV=1   also record names, colours, pass and a screen rect
//
// What differs from BandRnd, and why:
//   - Identity tokens. BandRnd caches a bind group per mesh slot, so it logs
//     bind-group handles. WgpuRnd builds bind groups per draw, so a handle says
//     nothing; the tokens here name the uniform data the draw bound instead
//     (ring buffer + offset; the shared bind group for a static mesh's bones).
//     Draws that share uniforms share a token, which is what the comparator's
//     sharing pattern tests.
//   - world is ObjectUniforms.world, the matrix the GPU received: identity for
//     a skinned mesh, whose bones already place it in the world.
//   - vertCount is the uploaded vertex buffer's count.
//   - boneFallback is 0: the dc3 bone palette has no bind-pose fallback count.
#include "platform/rndshape/RndShape.h"
#include "platform/RB3DrawLogDebug.h"
#include "gfx/UniformStructs.h"   // kMaxBones

#include "rndobj/Cam.h"
#include "rndobj/Mat.h"
#include "rndobj/Mesh.h"
#include "rndobj/Trans.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

std::vector<RB3DrawRecord> sDrawLog;
std::vector<RB3DrawProv> sDrawProv;
bool sDrawLogForced = false;   // RB3DebugSetDrawLogEnabled

// Provenance pass state: a frame-monotonic pass index and the depth LoadOp of
// the pass currently open.
uint16_t sProvPassCounter = 0;
uint16_t sProvCurPassIdx = 0;
uint8_t sProvCurPassDepthOp = 0;

// Provenance scope stacks (kind 0 panel, kind 1 owner); single render thread.
std::vector<std::string> sProvScopePanel;
std::vector<std::string> sProvScopeOwner;

bool EnvFlag(const char *name) {
    const char *e = getenv(name);
    return e && e[0] && e[0] != '0';
}

bool DrawLogEnvOn() {
    static int s = -1;
    if (s < 0)
        s = EnvFlag("RB3_DRAWLOG") ? 1 : 0;
    return s != 0;
}

// FNV-1a of a NUL-terminated string (empty/NULL -> 0). Stable across runs/hosts.
uint64_t Fnv1a(const char *s) {
    if (!s || !s[0])
        return 0;
    uint64_t h = 1469598103934665603ULL;
    for (; *s; ++s) {
        h ^= (uint64_t)(unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

// Project a mesh-local position through world -> viewProj -> screen pixels. Both
// matrices column-major (clip = viewProj * (world * pos)). False if the point is
// at or behind the camera.
bool ProjectToScreen(const float world[16], const float vp[16], float lx, float ly, float lz,
                     float vpW, float vpH, float &px, float &py) {
    float wx = world[0] * lx + world[4] * ly + world[8] * lz + world[12];
    float wy = world[1] * lx + world[5] * ly + world[9] * lz + world[13];
    float wz = world[2] * lx + world[6] * ly + world[10] * lz + world[14];
    float ww = world[3] * lx + world[7] * ly + world[11] * lz + world[15];
    if (ww != 0.f && ww != 1.f) {
        wx /= ww;
        wy /= ww;
        wz /= ww;
    }
    float cx = vp[0] * wx + vp[4] * wy + vp[8] * wz + vp[12];
    float cy = vp[1] * wx + vp[5] * wy + vp[9] * wz + vp[13];
    float cw = vp[3] * wx + vp[7] * wy + vp[11] * wz + vp[15];
    if (cw <= 1e-4f)
        return false;
    float ndcx = cx / cw, ndcy = cy / cw;   // WebGPU NDC, y up
    px = (ndcx * 0.5f + 0.5f) * vpW;
    py = (0.5f - ndcy * 0.5f) * vpH;        // framebuffer y down
    return true;
}

// RB3_PROV_SKIN_SPHERE: skinned draws take the sphere rect (rectKind 1) instead
// of the bone-world rect (rectKind 3), the A/B control BandRnd offers.
bool ProvSkinSphere() {
    static int s = -1;
    if (s < 0)
        s = EnvFlag("RB3_PROV_SKIN_SPHERE") ? 1 : 0;
    return s != 0;
}

void MeshWorld(RndMesh *mesh, float out[16]) {
    const Transform &t = mesh->WorldXfm();
    out[0] = t.m.x.x; out[1] = t.m.x.y; out[2] = t.m.x.z; out[3] = 0;
    out[4] = t.m.y.x; out[5] = t.m.y.y; out[6] = t.m.y.z; out[7] = 0;
    out[8] = t.m.z.x; out[9] = t.m.z.y; out[10] = t.m.z.z; out[11] = 0;
    out[12] = t.v.x; out[13] = t.v.y; out[14] = t.v.z; out[15] = 1;
}

void RecordProv(const rndshape::DrawLogDraw &d) {
    if (sDrawProv.capacity() == 0)
        sDrawProv.reserve(512);
    RndMesh *mesh = d.mesh;
    RndMat *mat = d.mat;
    RB3DrawProv p;
    p.meshName = (mesh && mesh->Name()) ? mesh->Name() : "";
    p.matName = (mat && mat->Name()) ? mat->Name() : "";
    RndCam *cam = RndCam::sCurrent;
    p.camName = (cam && cam->Name()) ? cam->Name() : "";
    RndTransformable *tp = mesh ? mesh->TransParent() : nullptr;
    p.transParent = (tp && tp->Name()) ? tp->Name() : "";
    p.scopePanel = sProvScopePanel.empty() ? std::string() : sProvScopePanel.back();
    p.scopeOwner = sProvScopeOwner.empty() ? std::string() : sProvScopeOwner.back();
    if (mat) {
        const Hmx::Color &c = mat->GetColor();
        p.matColor[0] = c.red; p.matColor[1] = c.green; p.matColor[2] = c.blue; p.matColor[3] = c.alpha;
    } else {
        p.matColor[0] = p.matColor[1] = p.matColor[2] = p.matColor[3] = 1.f;
    }
    for (int i = 0; i < 4; i++)
        p.boundColor[i] = d.boundColor ? d.boundColor[i] : 1.f;
    p.passIdx = sProvCurPassIdx;
    p.passDepthLoadOp = sProvCurPassDepthOp;
    p.boneFallback = 0;

    static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float *vp = d.viewProj ? d.viewProj : kIdentity;
    float vpW = d.viewportW < 1.f ? 1.f : d.viewportW;
    float vpH = d.viewportH < 1.f ? 1.f : d.viewportH;
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    bool gotAny = false;
    uint8_t kind = 2;
    auto grow = [&](float x, float y) {
        minx = std::min(minx, x); miny = std::min(miny, y);
        maxx = std::max(maxx, x); maxy = std::max(maxy, y);
        gotAny = true;
    };

    // Skinned: bone world positions are world points, projected with an
    // identity model; each bone also gets the rect of its segment to a parent
    // that belongs to the same palette.
    if (d.skinned && mesh && !ProvSkinSphere()) {
        RndMesh *owner = mesh->GeomOwner();
        if (!owner)
            owner = mesh;
        int nb = owner->NumBones();
        if (nb > kMaxBones)
            nb = kMaxBones;
        RndTransformable *members[kMaxBones];
        for (int b = 0; b < nb; ++b)
            members[b] = owner->BoneTransAt(b);
        for (int b = 0; b < nb; ++b) {
            RndTransformable *bt = members[b];
            if (!bt)
                continue;
            const Transform &wt = bt->WorldXfm();
            if (!(std::fabs(wt.v.x) < 1e5f && std::fabs(wt.v.y) < 1e5f && std::fabs(wt.v.z) < 1e5f))
                continue;
            float bx, by;
            if (!ProjectToScreen(kIdentity, vp, wt.v.x, wt.v.y, wt.v.z, vpW, vpH, bx, by))
                continue;
            grow(bx, by);
            float sminx = bx, sminy = by, smaxx = bx, smaxy = by;
            RndTransformable *par = bt->TransParent();
            bool parIsMember = false;
            if (par)
                for (int k = 0; k < nb; ++k)
                    if (members[k] == par) {
                        parIsMember = true;
                        break;
                    }
            if (parIsMember) {
                const Transform &pw = par->WorldXfm();
                float px2, py2;
                if (std::fabs(pw.v.x) < 1e5f && std::fabs(pw.v.y) < 1e5f && std::fabs(pw.v.z) < 1e5f
                    && ProjectToScreen(kIdentity, vp, pw.v.x, pw.v.y, pw.v.z, vpW, vpH, px2, py2)) {
                    sminx = std::min(sminx, px2); sminy = std::min(sminy, py2);
                    smaxx = std::max(smaxx, px2); smaxy = std::max(smaxy, py2);
                }
            }
            RB3ProvBoneRect br;
            br.bone = bt->Name() ? bt->Name() : "";
            br.rect[0] = std::max(0.f, sminx);
            br.rect[1] = std::max(0.f, sminy);
            br.rect[2] = std::min(vpW, smaxx) - br.rect[0];
            br.rect[3] = std::min(vpH, smaxy) - br.rect[1];
            p.boneRects.push_back(std::move(br));
        }
        if (gotAny)
            kind = 3;
        else
            p.boneRects.clear();
    }
    float meshWorld[16];
    if (mesh)
        MeshWorld(mesh, meshWorld);
    RndMesh::VertVector *vv = (mesh && !d.skinned) ? &mesh->Verts() : nullptr;
    if (vv && !vv->empty() && vv->size() <= 4096) {
        // Static mesh with CPU verts: the exact bbox.
        kind = 0;
        int n = vv->size();
        for (int i = 0; i < n; ++i) {
            const Vector3 &pos = (*vv)[i].pos;
            float sx, sy;
            if (ProjectToScreen(meshWorld, vp, pos.x, pos.y, pos.z, vpW, vpH, sx, sy))
                grow(sx, sy);
        }
    } else if (mesh && kind != 3) {
        // The bounding sphere's box, corners behind the camera skipped.
        kind = 1;
        const Sphere &sp = mesh->GetSphere();
        float r = sp.radius > 0.f ? sp.radius : 1.f;
        for (int c = 0; c < 8; ++c) {
            float ox = (c & 1) ? r : -r, oy = (c & 2) ? r : -r, oz = (c & 4) ? r : -r;
            float sx, sy;
            if (ProjectToScreen(meshWorld, vp, sp.center.x + ox, sp.center.y + oy,
                                sp.center.z + oz, vpW, vpH, sx, sy))
                grow(sx, sy);
        }
    }
    if (gotAny) {
        minx = std::max(minx, 0.f); miny = std::max(miny, 0.f);
        maxx = std::min(maxx, vpW); maxy = std::min(maxy, vpH);
        p.rect[0] = minx; p.rect[1] = miny; p.rect[2] = maxx - minx; p.rect[3] = maxy - miny;
        p.rectKind = kind;
    } else {
        p.rect[0] = p.rect[1] = 0;
        p.rect[2] = p.rect[3] = -1;
        p.rectKind = 2;
    }
    sDrawProv.push_back(std::move(p));
}

} // namespace

bool RB3DrawProvEnabled() {
    static int s = -1;
    if (s < 0)
        s = EnvFlag("RB3_DRAWLOG_PROV") ? 1 : 0;
    return s != 0;
}

void RB3DrawScopePush(int kind, const char *name) {
    if (!RB3DrawProvEnabled())
        return;
    std::vector<std::string> &s = (kind == 0) ? sProvScopePanel : sProvScopeOwner;
    s.emplace_back(name ? name : "");
}

void RB3DrawScopePop(int kind) {
    if (!RB3DrawProvEnabled())
        return;
    std::vector<std::string> &s = (kind == 0) ? sProvScopePanel : sProvScopeOwner;
    if (!s.empty())
        s.pop_back();
}

const std::vector<RB3DrawRecord> &RB3DebugGetDrawLog() { return sDrawLog; }
const std::vector<RB3DrawProv> &RB3DebugGetDrawProv() { return sDrawProv; }
void RB3DebugSetDrawLogEnabled(bool on) { sDrawLogForced = on; }
bool RB3DebugDrawLogEnabled() { return rndshape::DrawLogActive(); }

namespace rndshape {

bool DrawLogActive() { return DrawLogEnvOn() || sDrawLogForced || RB3DrawProvEnabled(); }

void DrawLogFrameBegin() {
    sDrawLog.clear();
    sDrawProv.clear();
    sProvPassCounter = 0;
    sProvCurPassIdx = 0;
    sProvCurPassDepthOp = 0;
}

void DrawLogPassOpen(int depthOp) {
    if (!RB3DrawProvEnabled())
        return;
    sProvCurPassIdx = sProvPassCounter++;
    sProvCurPassDepthOp = (uint8_t)depthOp;
}

void DrawLogRecord(const DrawLogDraw &d) {
    if (sDrawLog.capacity() == 0)
        sDrawLog.reserve(512);
    RB3DrawRecord r;
    r.pipelineHash = d.pipelineHash;
    r.blend = d.blend;
    r.zMode = d.zMode;
    r.layout = d.layout;
    r.flags = (uint8_t)((d.hasDepth ? 1u : 0u) | (d.alphaCut ? 2u : 0u) | (d.alphaWrite ? 4u : 0u)
                        | (d.skinned ? 8u : 0u));
    r.targetFormat = d.targetFormat;
    r.indexCount = d.indexCount;
    r.triCount = d.triCount;
    r.vertCount = d.vertCount;
    r.meshNameHash = Fnv1a(d.mesh ? d.mesh->Name() : nullptr);
    for (int i = 0; i < 16; ++i)
        r.world[i] = d.world ? d.world[i] : ((i % 5) == 0 ? 1.f : 0.f);
    // Opaque identity tokens, never dereferenced.
    r.sceneBG = (const void *)(uintptr_t)d.sceneToken;
    r.matBG = (const void *)(uintptr_t)d.matToken;
    r.objBG = (const void *)(uintptr_t)d.objToken;
    r.boneBG = (const void *)(uintptr_t)d.boneToken;
    sDrawLog.push_back(r);
    if (RB3DrawProvEnabled())
        RecordProv(d);
}

void DrawLogFrameEnd(int frame) {
    if (!DrawLogActive())
        return;
    // A file only when RB3_DRAWLOG_DUMP names one; a test can record without it.
    const char *path = getenv("RB3_DRAWLOG_DUMP");
    if (!path || !path[0])
        return;
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "WgpuRnd: draw-log dump: cannot open %s\n", path);
        return;
    }
    // Each token stream gets dense ids in first-seen order, so the file is
    // host- and run-independent while keeping the sharing pattern.
    std::unordered_map<const void *, int> sceneIds, matIds, objIds, boneIds;
    auto denseId = [](std::unordered_map<const void *, int> &m, const void *p) -> int {
        auto it = m.find(p);
        if (it != m.end())
            return it->second;
        int id = (int)m.size();
        m.emplace(p, id);
        return id;
    };
    fprintf(f, "{ \"frame\": %d, \"count\": %d,\n  \"draws\": [", frame, (int)sDrawLog.size());
    for (size_t i = 0; i < sDrawLog.size(); ++i) {
        const RB3DrawRecord &r = sDrawLog[i];
        int sceneId = denseId(sceneIds, r.sceneBG);
        int matId = denseId(matIds, r.matBG);
        int objId = denseId(objIds, r.objBG);
        int boneId = denseId(boneIds, r.boneBG);
        fprintf(f,
                "%s\n    { \"i\":%d, \"name\":\"0x%llx\", \"pipe\":\"0x%llx\", "
                "\"blend\":%d, \"zmode\":%d, \"layout\":%d, \"fmt\":%u, "
                "\"hasDepth\":%s, \"alphaCut\":%s, \"alphaWrite\":%s, \"skinned\":%s, "
                "\"idx\":%u, \"tris\":%u, \"verts\":%u, "
                "\"scene\":%d, \"mat\":%d, \"obj\":%d, \"bone\":%d,\n"
                "      \"world\":[",
                (i == 0 ? "" : ","), (int)i, (unsigned long long)r.meshNameHash,
                (unsigned long long)r.pipelineHash, (int)r.blend, (int)r.zMode, (int)r.layout,
                (unsigned)r.targetFormat, (r.flags & 1) ? "true" : "false",
                (r.flags & 2) ? "true" : "false", (r.flags & 4) ? "true" : "false",
                (r.flags & 8) ? "true" : "false", (unsigned)r.indexCount, (unsigned)r.triCount,
                (unsigned)r.vertCount, sceneId, matId, objId, boneId);
        for (int e = 0; e < 16; ++e)
            fprintf(f, "%s%.6g", (e == 0 ? "" : ","), (double)r.world[e]);
        fprintf(f, "] }");
    }
    fprintf(f, "%s] }\n", sDrawLog.empty() ? "" : "\n  ");
    fclose(f);
}

} // namespace rndshape
