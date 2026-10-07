// SpotBeamHook — the seam between a consumer's spotlight drawer and the GPU
// backend's volumetric beam pass.
//
// Retail RB3 (Xbox 360) runs kNewGfx, so SpotlightDrawer::DrawWorld does not
// draw beam meshes with their materials; NgSpotlightDrawer (rb3-xenon
// world/SpotlightDrawer_NG.cpp) draws them in post instead:
//   RenderScene   each spotlight's NG shaft mesh (Spotlight::BuildNGShaft) is
//                 drawn with the depthvolume shader into a half-size target;
//                 the pixel shader intersects the view ray with the analytic
//                 cone (or sheet, or sphere) and clips it by the scene depth.
//   BlurRT        a 5-tap blur of that target.
//   post          postprocess.ps option bit 51 adds the blurred target to the
//                 scene, after the bloom screen blend and before the colour
//                 transform.
// The engine side is gfx/SpotBeamPass (constants and GPU passes) and the
// backend that drives it (today the dc3 WgpuRnd).
//
// The consumer keeps the Spotlight side (its class shapes differ per decomp)
// and hands the backend one NativeSpotBeamFrame per frame, at world end, while
// the world camera is current. With no renderer registered (headless builds,
// the rb3 BandRnd flavor) the consumer keeps drawing beam meshes the old way.
//
// This header includes nothing and names no WebGPU type. `mesh`, `xsection`,
// `fogTexture` and `camera` are the consumer's RndMesh / RndTex / RndCam
// pointers, which the backend (compiled in the consumer's context) resolves.

#ifndef MILO_ENGINE_PLATFORM_SPOTBEAMHOOK_H
#define MILO_ENGINE_PLATFORM_SPOTBEAMHOOK_H

// One spotlight's beam, as NgSpotlightDrawer::RenderBeams reads it.
struct NativeSpotBeam {
    const void* mesh;       // RndMesh*: BeamDef::mBeam, an NG shaft
    const void* xsection;   // RndTex*: BeamDef::mXSection, or null (retail: white)
    float meshXfm[12];      // the mesh's world transform: rows m.x, m.y, m.z, then v
    int shape;              // BeamDef::mShape: 0/1 cone, 2 sheet, 3/4 sphere
    float lightPos[3];      // GetLightPosition: spotlight position + beam offset
    float axis[3];          // spotlight WorldXfm().m.y (the beam direction)
    float sheetDir[3];      // spotlight WorldXfm().m.z (RenderSheet's c91)
    float ngRadii[2];       // BeamDef::NGRadii(): top, bottom
    float topRadius;        // BeamDef::mTopRadius (RenderSphere)
    float length;           // BeamDef::mLength
    float brighten;         // BeamDef::mBrighten
    float color[4];         // colour owner's colour, 0..1
    float intensity;        // colour owner's intensity
    float matColor[4];      // beam material colour; (1,1,1,1) when there is no
                            // material or the colour animates from a preset
};

// The drawer, as NgSpotlightDrawer reads its SpotDrawParams.
struct NativeSpotBeamFrame {
    const void* camera;     // RndCam*: retail CheckCam (TheWorld's camera, else
                            // the current one)
    float intensity;        // mIntensity (post: c91.x = intensity * 32)
    float baseIntensity;    // mBaseIntensity, percent (post: c127.x)
    float smokeIntensity;   // mSmokeIntensity, percent (post: c127.y)
    const void* fogTexture; // RndTex*: mTexture, or null (retail: black)
    bool hasProxy;          // mProxy set: retail renders a fog density map from
                            // it, which this seam does not carry (density 0)
};

class NativeSpotBeamRenderer {
public:
    virtual ~NativeSpotBeamRenderer() {}
    // This frame's beams, drawn in the world's post. Replaces any beams already
    // submitted this frame. False when the backend cannot draw them now.
    virtual bool SubmitSpotBeams(const NativeSpotBeamFrame& frame,
                                 const NativeSpotBeam* beams, int count) = 0;
};

// The backend's renderer; null when the linked backend has no beam pass.
void SetNativeSpotBeamRenderer(NativeSpotBeamRenderer* renderer);
NativeSpotBeamRenderer* GetNativeSpotBeamRenderer();

#endif // MILO_ENGINE_PLATFORM_SPOTBEAMHOOK_H
