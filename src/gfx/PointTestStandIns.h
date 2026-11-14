// PointTestStandIns — the answer a point test gets when its readback is late.
//
// Retail DxRnd::DoPointTests blocks on the previous frame's fence before it
// reads that frame's occlusion queries back, so every flare answer arrives one
// frame after its test. A renderer that does not block (WgpuRnd under a shape
// whose rndshape::kPointTestWaitAfterSubmit is false, and the web) sometimes
// finds a batch still in flight at world end. The flare then sees no answer at
// its next DrawShowing, and retail RndFlare::DrawShowing treats a frame with no
// answer as a flare that has just come into view: the frame after, it snaps
// mStep to the end of its fade (`mStep = mVisible ? mSteps : 0`). A flare with
// steps 5 (DCI's CouchFlare*, rollerrink's food and disco-ball flares) then
// pops from full to nothing, or back, instead of fading over five frames.
//
// A stand-in is the key's last real answer, delivered again in the late
// answer's place: the flare keeps stepping toward what it last saw and takes
// the real answer a frame later. The occasional extra frame of latency is the
// whole cost.
//
// Rndobj-free and GPU-free: WgpuRnd notes each real answer and says which keys
// are late; the tests drive it directly.
#pragma once
#include "gfx/PointTestPass.h"

#include <algorithm>
#include <vector>

class PointTestStandIns {
public:
    // A real answer was delivered: it is the stand-in for its key from now on.
    void Note(const PointTestPass::Answer& a) {
        if (!a.key) return;
        for (PointTestPass::Answer& e : mLast) {
            if (e.key != a.key) continue;
            if (a.pointDone) {
                e.pointDone = true;
                e.visible = a.visible;
            }
            if (a.areaDone) {
                e.areaDone = true;
                e.area = a.area;
            }
            e.seq = a.seq;
            return;
        }
        mLast.push_back(a);
    }
    // No stand-in is delivered for `key` after this returns.
    void Forget(const void* key) {
        mLast.erase(std::remove_if(mLast.begin(), mLast.end(),
                                   [key](const PointTestPass::Answer& e) { return e.key == key; }),
                    mLast.end());
    }
    // Delivers each late key's stand-in to `fn`, once per key. A key that has
    // never had a real answer gets none: retail's first frame in view has none
    // either. Returns the number delivered.
    int Deliver(const std::vector<const void*>& lateKeys, PointTestPass::AnswerFn fn,
                void* user) const {
        int n = 0;
        for (size_t i = 0; i < lateKeys.size(); i++) {
            const void* key = lateKeys[i];
            if (!key || std::find(lateKeys.begin(), lateKeys.begin() + i, key) !=
                            lateKeys.begin() + i)
                continue;
            for (const PointTestPass::Answer& e : mLast) {
                if (e.key != key) continue;
                if (fn) fn(e, user);
                n++;
                break;
            }
        }
        return n;
    }
    size_t Size() const { return mLast.size(); }

private:
    std::vector<PointTestPass::Answer> mLast;
};
