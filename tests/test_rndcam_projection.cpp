#include "test_helpers.h"

#include "math/Mtx.h"
#include "math/Rot.h"
#include "hamobj/HamListRibbon.h"
#include "obj/Object.h"
#include "rndobj/Cam.h"
#include "rndobj/Draw.h"
#include "rndobj/Mat.h"
#include "rndobj/Mesh.h"
#include "rndobj/Rnd.h"
#include "platform/UiRenderHeuristics.h"
#include "ui/UIListMesh.h"

#include <cmath>
#include <vector>

class RndCamProjectionTest : public EngineTestFixture {};

namespace {
constexpr float kEps = 1.0e-4f;

RndCam *MakeTestCam(const char *name, float nearPlane, float farPlane, float yFov) {
    RndCam *cam = Hmx::Object::New<RndCam>();
    cam->SetName(name, ObjectDir::Main());
    cam->SetLocalRot(Hmx::Matrix3::GetIdentity());
    cam->SetLocalPos(Vector3(0.0f, 0.0f, 0.0f));
    cam->SetScreenRect(Hmx::Rect(0.0f, 0.0f, 1.0f, 1.0f));
    cam->SetFrustum(nearPlane, farPlane, yFov, 1.0f);
    cam->UpdatedWorldXfm();
    return cam;
}
}

TEST_F(RndCamProjectionTest, PerspectiveIdentityProjectionMatchesExpectedMatrix) {
    const float nearPlane = 1.0f;
    const float farPlane = 1000.0f;
    const float yFov = 0.6024178f;
    RndCam *cam = MakeTestCam("test_cam_persp", nearPlane, farPlane, yFov);

    Transform viewXfm;
    Hmx::Matrix4 projMtx;
    cam->GetViewProjectXfms(viewXfm, projMtx);

    EXPECT_NEAR(viewXfm.m.x.x, 1.0f, kEps);
    EXPECT_NEAR(viewXfm.m.x.y, 0.0f, kEps);
    EXPECT_NEAR(viewXfm.m.x.z, 0.0f, kEps);
    EXPECT_NEAR(viewXfm.m.y.x, 0.0f, kEps);
    EXPECT_NEAR(viewXfm.m.y.y, 0.0f, kEps);
    EXPECT_NEAR(viewXfm.m.y.z, 1.0f, kEps);
    EXPECT_NEAR(viewXfm.m.z.x, 0.0f, kEps);
    EXPECT_NEAR(viewXfm.m.z.y, 1.0f, kEps);
    EXPECT_NEAR(viewXfm.m.z.z, 0.0f, kEps);
    EXPECT_NEAR(viewXfm.v.x, 0.0f, kEps);
    EXPECT_NEAR(viewXfm.v.y, 0.0f, kEps);
    EXPECT_NEAR(viewXfm.v.z, 0.0f, kEps);

    const float tanHalf = std::tanf(yFov * 0.5f);
    const float ratio = TheRnd.YRatio();
    const float farRatio = farPlane / (farPlane - nearPlane);

    EXPECT_NEAR(projMtx.x.x, ratio / tanHalf, kEps);
    EXPECT_NEAR(projMtx.y.y, 1.0f / tanHalf, kEps);
    EXPECT_NEAR(projMtx.z.x, 0.0f, kEps);
    EXPECT_NEAR(projMtx.z.y, 0.0f, kEps);
    EXPECT_NEAR(projMtx.z.z, farRatio, kEps);
    EXPECT_NEAR(projMtx.z.w, 1.0f, kEps);
    EXPECT_NEAR(projMtx.w.z, -(nearPlane * farRatio), kEps);
    EXPECT_NEAR(projMtx.w.w, 0.0f, kEps);
}

TEST_F(RndCamProjectionTest, PerspectiveWorldToScreenMatchesExpectedFrustumEdges) {
    const float nearPlane = 1.0f;
    const float farPlane = 1000.0f;
    const float yFov = 0.6024178f;
    const float depth = 10.0f;
    RndCam *cam = MakeTestCam("test_cam_frustum", nearPlane, farPlane, yFov);

    const float tanHalf = std::tanf(yFov * 0.5f);
    const float ratio = TheRnd.YRatio();

    Vector2 screen;

    cam->WorldToScreen(Vector3(0.0f, depth, 0.0f), screen);
    EXPECT_NEAR(screen.x, 0.5f, kEps);
    EXPECT_NEAR(screen.y, 0.5f, kEps);

    cam->WorldToScreen(Vector3(depth * tanHalf / ratio, depth, 0.0f), screen);
    EXPECT_NEAR(screen.x, 1.0f, 2.0e-3f);
    EXPECT_NEAR(screen.y, 0.5f, 2.0e-3f);

    cam->WorldToScreen(Vector3(0.0f, depth, depth * tanHalf), screen);
    EXPECT_NEAR(screen.x, 0.5f, 2.0e-3f);
    EXPECT_NEAR(screen.y, 1.0f, 2.0e-3f);
}

TEST_F(RndCamProjectionTest, TranslatedCameraKeepsForwardPointCentered) {
    const float yFov = 0.6024178f;
    RndCam *cam = MakeTestCam("test_cam_translated", 1.0f, 1000.0f, yFov);
    cam->SetLocalPos(Vector3(0.0f, -5.0f, 0.0f));
    cam->UpdatedWorldXfm();

    Vector2 screen;
    float depth = cam->WorldToScreen(Vector3(0.0f, 5.0f, 0.0f), screen);
    EXPECT_NEAR(depth, 10.0f, kEps);
    EXPECT_NEAR(screen.x, 0.5f, kEps);
    EXPECT_NEAR(screen.y, 0.5f, kEps);
}

TEST_F(RndCamProjectionTest, ScreenRectScalesProjectionIntoSubview) {
    const float yFov = 0.6024178f;
    const float depth = 10.0f;
    RndCam *cam = MakeTestCam("test_cam_subrect", 1.0f, 1000.0f, yFov);
    cam->SetScreenRect(Hmx::Rect(0.25f, 0.25f, 0.5f, 0.5f));
    cam->UpdatedWorldXfm();

    const float tanHalf = std::tanf(yFov * 0.5f);
    const float ratio = TheRnd.YRatio();

    Vector2 screen;
    cam->WorldToScreen(Vector3(0.0f, depth, 0.0f), screen);
    EXPECT_NEAR(screen.x, 0.5f, kEps);
    EXPECT_NEAR(screen.y, 0.5f, kEps);

    cam->WorldToScreen(Vector3(depth * tanHalf / ratio, depth, 0.0f), screen);
    EXPECT_NEAR(screen.x, 0.75f, 2.0e-3f);
    EXPECT_NEAR(screen.y, 0.5f, 2.0e-3f);
}

TEST_F(RndCamProjectionTest, ChooseModeApproximateLayoutFitsViewportWithoutDebugUiCamHack) {
    RndCam *cam = MakeTestCam("test_cam_choose_mode", 1.0f, 1000.0f, 0.6024178f);
    cam->SetLocalPos(Vector3(0.0f, -768.0f, 0.0f));
    cam->UpdatedWorldXfm();

    const Vector3 chooseModeLabelPositions[] = {
        Vector3(-107.6f, -8.0f, -62.5f),
        Vector3(-107.6f, -8.0f, -87.5f),
        Vector3(-107.6f, -8.0f, -112.5f),
        Vector3(-107.6f, -8.0f, -137.5f),
        Vector3(-107.6f, -8.0f, -162.5f),
    };

    for (size_t i = 0; i < std::size(chooseModeLabelPositions); ++i) {
        Vector2 screen;
        float depth = cam->WorldToScreen(chooseModeLabelPositions[i], screen);
        EXPECT_GT(depth, 0.0f);
        EXPECT_GE(screen.x, 0.0f);
        EXPECT_LE(screen.x, 1.0f);
        EXPECT_GE(screen.y, 0.0f);
        EXPECT_LE(screen.y, 1.0f);
    }
}

TEST_F(RndCamProjectionTest, ChooseModeDebugUiCamHackPushesApproximateLayoutOffscreen) {
    RndCam *cam = MakeTestCam("test_cam_choose_mode_debug_hack", 1.0f, 1000.0f, 0.6024178f);
    cam->SetLocalPos(Vector3(0.0f, -768.0f, 370.0f));
    cam->UpdatedWorldXfm();

    Vector2 screen;
    float depth = cam->WorldToScreen(Vector3(-107.6f, -8.0f, -62.5f), screen);
    EXPECT_GT(depth, 0.0f);
    EXPECT_LT(screen.y, 0.0f);
}

TEST_F(RndCamProjectionTest, OrthographicProjectionMatchesExpectedMatrix) {
    const float nearPlane = 2.0f;
    const float farPlane = 202.0f;
    RndCam *cam = MakeTestCam("test_cam_ortho", nearPlane, farPlane, 0.0f);

    Transform viewXfm;
    Hmx::Matrix4 projMtx;
    cam->GetViewProjectXfms(viewXfm, projMtx);

    const float ratio = TheRnd.YRatio();
    const float farRatio = 1.0f / (farPlane - nearPlane);

    EXPECT_NEAR(projMtx.x.x, 1.0f, kEps);
    EXPECT_NEAR(projMtx.y.y, 1.0f / ratio, kEps);
    EXPECT_NEAR(projMtx.z.x, 0.0f, kEps);
    EXPECT_NEAR(projMtx.z.y, 0.0f, kEps);
    EXPECT_NEAR(projMtx.z.z, farRatio, kEps);
    EXPECT_NEAR(projMtx.z.w, 0.0f, kEps);
    EXPECT_NEAR(projMtx.w.z, -(nearPlane * farRatio), kEps);
    EXPECT_NEAR(projMtx.w.w, 1.0f, kEps);
}

namespace {
class RibbonDrawRecorder : public RndDrawable {
public:
    explicit RibbonDrawRecorder(HamListRibbon *owner) : mOwner(owner) {}

    std::vector<float> mDrawZ;

    void DrawShowing() override {
        mDrawZ.push_back(mOwner->WorldXfm().v.z);
    }

private:
    HamListRibbon *mOwner;
};

class TestHamListRibbon : public HamListRibbon {
public:
    void AddTestDrawable(RndDrawable *d) {
        mDraws.push_back(d);
    }
};

class HiddenTemplateMesh : public RndMesh {
public:
    HiddenTemplateMesh() : mDrawShowingCalls(0), mShowingInsideDraw(false) {}

    void DrawShowing() override {
        mDrawShowingCalls++;
        mShowingInsideDraw = Showing();
    }

    int mDrawShowingCalls;
    bool mShowingInsideDraw;
};

class TestUIListMesh : public UIListMesh {
public:
    void SetMeshForTest(RndMesh *mesh) {
        mMesh = mesh;
    }
};
}

TEST_F(RndCamProjectionTest, HamListRibbonUsesUniformSpacingForFiveVisibleItems) {
    TestHamListRibbon ribbon;
    RibbonDrawRecorder recorder(&ribbon);
    ribbon.AddTestDrawable(&recorder);

    std::vector<HamListRibbonDrawState> states(HamListRibbon::sNumListSelectable);
    for (std::vector<HamListRibbonDrawState>::iterator it = states.begin(); it != states.end(); ++it) {
        it->mActive = false;
        it->mHidden = false;
        it->mSelected = false;
    }

    Transform xfm;
    xfm.Reset();
    ribbon.Draw(xfm, states, false, false);

    ASSERT_EQ(recorder.mDrawZ.size(), (size_t)HamListRibbon::sNumListSelectable);
    for (size_t i = 1; i < recorder.mDrawZ.size(); ++i) {
        EXPECT_NEAR(recorder.mDrawZ[i - 1] - recorder.mDrawZ[i], 25.0f, kEps);
    }
}

TEST_F(RndCamProjectionTest, UIListMeshDrawTemporarilyShowsHiddenTemplateMesh) {
    TestUIListMesh listMesh;
    HiddenTemplateMesh mesh;
    RndMat *mat = Hmx::Object::New<RndMat>();

    listMesh.SetMeshForTest(&mesh);

    UIListMeshElement element(&listMesh);
    element.mMat = mat;

    mesh.SetShowing(false);

    Transform tf;
    tf.Reset();
    element.Draw(tf, 1.0f, nullptr, nullptr);

    EXPECT_EQ(mesh.mDrawShowingCalls, 1);
    EXPECT_TRUE(mesh.mShowingInsideDraw);
    EXPECT_FALSE(mesh.Showing());
}

TEST_F(RndCamProjectionTest, TextAlphaFallbackOnlyAppliesToTransparentTextMeshes) {
    EXPECT_TRUE(NativeShouldForceTextAlpha(true, BaseMaterial::kBlendSrcAlpha, 0.0f));
    EXPECT_TRUE(NativeShouldForceTextAlpha(true, BaseMaterial::kBlendSrcAlpha, 0.009f));
    EXPECT_FALSE(NativeShouldForceTextAlpha(false, BaseMaterial::kBlendSrcAlpha, 0.0f));
    EXPECT_FALSE(NativeShouldForceTextAlpha(true, BaseMaterial::kBlendAdd, 0.0f));
    EXPECT_FALSE(NativeShouldForceTextAlpha(true, BaseMaterial::kBlendSrcAlpha, 0.5f));
}
