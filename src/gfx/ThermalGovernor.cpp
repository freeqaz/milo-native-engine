// The thermal governor's rule (gfx/ThermalGovernor.h). Pure.
#include "gfx/ThermalGovernor.h"

namespace milo {

const char* ThermalLevelName(int level) {
    switch (level) {
    case kThermalNominal: return "nominal";
    case kThermalFair: return "fair";
    case kThermalSerious: return "serious";
    case kThermalCritical: return "critical";
    default: return "unknown";
    }
}

int ThermalGovernor::StepDown(int cap, int baseRows, int floorRows) {
    if (baseRows <= 0) return cap;
    const int from = cap > 0 && cap < baseRows ? cap : baseRows;
    for (int i = 0; i < kThermalRowStepCount; i++) {
        const int rows = kThermalRowSteps[i];
        if (rows < from && rows >= floorRows) return rows;
    }
    return cap;
}

int ThermalGovernor::StepUp(int cap, int baseRows) {
    if (cap <= 0) return 0;
    for (int i = kThermalRowStepCount - 1; i >= 0; i--) {
        const int rows = kThermalRowSteps[i];
        if (rows > cap) return baseRows > 0 && rows >= baseRows ? 0 : rows;
    }
    return 0;
}

void ThermalGovernor::ResetWindow(double now) {
    mWindowStart = now;
    mWindowFrames = 0;
    mWindowLateFrames = 0;
}

void ThermalGovernor::Changed(double now) {
    mSettleUntil = now + mCfg.stepSettleSeconds;
    mLateWindows = 0;
    ResetWindow(mSettleUntil);
}

int ThermalGovernor::Frame(const ThermalGovernorInput& in) {
    if (!mCfg.enabled) return 0;
    const double now = in.nowSeconds;
    const double began = mLastFrame;  // this frame's period runs from here to now
    const double period = began >= 0.0 ? now - began : -1.0;
    mLastFrame = now;

    // The cool dwell: nominal (or no level at all) without a break.
    const bool cool = in.level <= kThermalNominal;
    if (!cool)
        mCoolSince = -1.0;
    else if (mCoolSince < 0.0)
        mCoolSince = now;

    // Step up: one step per whole dwell.
    if (mCap > 0 && cool && now - mCoolSince >= mCfg.coolDwellSeconds) {
        mCap = StepUp(mCap, in.baseRows);
        mStepsUp++;
        mCoolSince = now;
        Changed(now);
        return mCap;
    }

    // The pace: windows of windowSeconds, none while a change settles.
    const bool judged = in.paceMeaningful && in.targetFps > 0.0 && period >= 0.0;
    if (!judged) {
        mLateWindows = 0;
        ResetWindow(now);
        return mCap;
    }
    if (now < mSettleUntil) return mCap;
    if (mWindowStart < 0.0 || mWindowStart > now) ResetWindow(now);
    const double lateFps = in.targetFps * mCfg.lateRatio;
    // A frame that began before the window did belongs to no window.
    if (began >= mWindowStart) {
        mWindowFrames++;
        if (period > 1.0 / lateFps) mWindowLateFrames++;
    }
    const double elapsed = now - mWindowStart;
    if (elapsed < mCfg.windowSeconds || mWindowFrames == 0) return mCap;

    mLastWindowFps = mWindowFrames / elapsed;
    const bool late = mLastWindowFps < lateFps && mWindowLateFrames >= mCfg.minLateFrames &&
                      mWindowLateFrames >= mCfg.lateFrameShare * mWindowFrames;
    mLateWindows = late ? mLateWindows + 1 : 0;
    ResetWindow(now);

    // Step down: the level permits, the late windows trigger.
    if (in.level >= kThermalFair) {
        const int need =
            in.level >= kThermalSerious ? mCfg.lateWindowsSerious : mCfg.lateWindowsFair;
        if (mLateWindows >= need) {
            const int next = StepDown(mCap, in.baseRows, mCfg.floorRows);
            if (next != mCap) {
                mCap = next;
                mStepsDown++;
                Changed(now);
            } else {
                mLateWindows = 0;  // nothing left to give; start counting again
            }
        }
    }
    return mCap;
}

}  // namespace milo
