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

int ThermalGovernor::RateStepDown(int cap, int baseRows, int rateFloorRows) {
    if (baseRows <= 0) return cap;
    const int from = cap > 0 && cap < baseRows ? cap : baseRows;
    for (int i = 0; i < kRateRowStepCount; i++) {
        const int rows = kRateRowSteps[i];
        if (rows < from && rows >= rateFloorRows) return rows;
    }
    return cap;
}

int ThermalGovernor::RateStepUp(int cap, int baseRows) {
    if (cap <= 0) return 0;
    for (int i = kRateRowStepCount - 1; i >= 0; i--) {
        const int rows = kRateRowSteps[i];
        if (rows > cap) return baseRows > 0 && rows >= baseRows ? 0 : rows;
    }
    return 0;
}

namespace {
int Tighter(int a, int b) {
    if (a <= 0) return b > 0 ? b : 0;
    if (b <= 0) return a;
    return a < b ? a : b;
}
}  // namespace

void ThermalGovernor::SetCap(double now, int thermalCap, int rateCap, GovernorChange why) {
    mThermalCap = thermalCap;
    mRateCap = rateCap;
    mCap = Tighter(thermalCap, rateCap);
    mLastChange = why;
    mHeadroomSince = -1.0;
    Changed(now);
}

void ThermalGovernor::ResetWindow(double now) {
    mWindowStart = now;
    mWindowFrames = 0;
    mWindowLateFrames = 0;
}

void ThermalGovernor::Changed(double now) {
    mSettleUntil = now + mCfg.stepSettleSeconds;
    mLateWindows = 0;
    mRateLateWindows = 0;
    ResetWindow(mSettleUntil);
}

int ThermalGovernor::Frame(const ThermalGovernorInput& in) {
    if (!mCfg.enabled) return 0;
    mLastChange = kGovernorNoChange;
    const double now = in.nowSeconds;
    const double began = mLastFrame;  // this frame's period runs from here to now
    const double period = began >= 0.0 ? now - began : -1.0;
    mLastFrame = now;
    if (mRateUpDwell <= 0.0) mRateUpDwell = mCfg.rateUpDwellSeconds;

    // The output was made smaller than the rate cap (a window resized by the
    // player): the cap no longer binds, and a size chosen afresh starts afresh.
    // Not a change: the output is the size it already is.
    if (mRateCap > 0 && (!mCfg.rateRule || (in.baseRows > 0 && in.baseRows <= mRateCap))) {
        mRateCap = 0;
        mCap = Tighter(mThermalCap, mRateCap);
        mHeadroomSince = -1.0;
    }

    // The cool dwell: nominal (or no level at all) without a break.
    const bool cool = in.level <= kThermalNominal;
    if (!cool)
        mCoolSince = -1.0;
    else if (mCoolSince < 0.0)
        mCoolSince = now;

    // Thermal step up: one step per whole dwell.
    if (mThermalCap > 0 && cool && now - mCoolSince >= mCfg.coolDwellSeconds) {
        SetCap(now, StepUp(mThermalCap, in.baseRows), mRateCap, kGovernorThermalUp);
        mStepsUp++;
        mCoolSince = now;
        return mCap;
    }

    // The pace: windows of windowSeconds, none while a change settles.
    const bool judged = in.paceMeaningful && in.targetFps > 0.0 && period >= 0.0;
    if (!judged) {
        mLateWindows = 0;
        mRateLateWindows = 0;
        mHeadroomSince = -1.0;
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

    const double windowStart = mWindowStart;
    mLastWindowFps = mWindowFrames / elapsed;
    const bool late = mLastWindowFps < lateFps && mWindowLateFrames >= mCfg.minLateFrames &&
                      mWindowLateFrames >= mCfg.lateFrameShare * mWindowFrames;
    mLateWindows = late ? mLateWindows + 1 : 0;
    ResetWindow(now);
    mLastPredictedMs = mLastBudgetMs = 0.0;

    if (in.level >= kThermalFair) {
        // Thermal step down: the level permits, the late windows trigger.
        mHeadroomSince = -1.0;
        mRateLateWindows = 0;
        const int need =
            in.level >= kThermalSerious ? mCfg.lateWindowsSerious : mCfg.lateWindowsFair;
        if (mLateWindows >= need) {
            const int next = StepDown(mCap, in.baseRows, mCfg.floorRows);
            if (next != mCap) {
                SetCap(now, next, mRateCap, kGovernorThermalDown);
                mStepsDown++;
            } else {
                mLateWindows = 0;  // nothing left to give; start counting again
            }
        }
        return mCap;
    }
    if (!mCfg.rateRule) return mCap;

    // Rate step down: any cooler level; only ever above rateFloorRows; only
    // when the pixels are what is slow (the GPU measured near the budget, or
    // unmeasured on an adapter that is not discrete).
    if (late) {
        mHeadroomSince = -1.0;
        const bool pixels = in.gpuFrameMs >= 0.0
                                ? in.gpuFrameMs >= mCfg.rateGpuBoundShare * 1000.0 / in.targetFps
                                : !in.discreteGpu;
        if (!pixels) mRateLateWindows = 0;
        else mRateLateWindows++;
        if (mRateLateWindows >= mCfg.lateWindowsRate) {
            mRateLateWindows = 0;
            const int next = RateStepDown(mCap, in.baseRows, mCfg.rateFloorRows);
            if (next != mCap) {
                // A step up that did not hold: the next try waits longer.
                if (mRateUpAt >= 0.0 && now - mRateUpAt <= mCfg.rateRetrySeconds) {
                    mRateUpDwell = mRateUpDwell * 2.0;
                    if (mRateUpDwell > mCfg.rateUpDwellMaxSeconds)
                        mRateUpDwell = mCfg.rateUpDwellMaxSeconds;
                }
                SetCap(now, mThermalCap, next, kGovernorRateDown);
                mRateStepsDown++;
            }
        }
        return mCap;
    }
    mRateLateWindows = 0;

    // Rate step up: a positive headroom signal, unbroken for the dwell.
    if (mRateCap > 0 && mThermalCap == 0 && cool) {
        const int next = RateStepUp(mRateCap, in.baseRows);
        const int nextRows = next > 0 ? next : in.baseRows;
        bool headroom = false;
        if (in.gpuFrameMs >= 0.0 && nextRows > mRateCap) {
            const double ratio = (double)nextRows / mRateCap;
            mLastPredictedMs = in.gpuFrameMs * ratio * ratio;
            mLastBudgetMs = mCfg.rateHeadroomShare * 1000.0 / in.targetFps;
            headroom = mLastPredictedMs <= mLastBudgetMs;
        }
        if (!headroom) {
            mHeadroomSince = -1.0;
        } else {
            if (mHeadroomSince < 0.0) mHeadroomSince = windowStart;
            if (now - mHeadroomSince >= mRateUpDwell) {
                SetCap(now, mThermalCap, next, kGovernorRateUp);
                mRateStepsUp++;
                mRateUpAt = now;
            }
        }
    }
    return mCap;
}

}  // namespace milo
