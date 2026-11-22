#pragma once
// The MILO_PERF line: one every `windowSec` of WALL time, saying how many
// frames were drawn in it (the rate the screen got) and how long each took to
// render (BeginFrame to the end of EndFrame, present included).
//
// It used to print "N fps" where N was frames / summed render time -- 1 / the
// render time, not a rate anything was shown at -- and its window was 5 s of
// summed render TIME, so on a fast GPU one line covered ~30 s of play (GPD
// playtest 2026-11-22: "433.6 fps" for a game presenting at 72, and a 642 ms
// frame that could only be placed somewhere in the first half minute).
// Header-only and free of wgpu so a test can drive it with made-up clocks.
#include <cstdio>

namespace milo {

class PerfWindow {
public:
    explicit PerfWindow(double windowSec = 5.0, double budgetMs = 1000.0 / 60.0)
        : mWindowSec(windowSec), mBudgetMs(budgetMs) {}

    // One drawn frame: its render started at `startSec` and ended at `endSec`
    // (same clock). Returns true when the window is full; Format() then says
    // what it held and Reset() starts the next one.
    bool AddFrame(double startSec, double endSec, int draws, int invisible) {
        // The first window opens at the first frame's start; every later one
        // at the end of the frame that closed the previous one, so N frames
        // ending in a window of W s read N / W.
        if (!mOpen) {
            mWindowStart = startSec;
            mOpen = true;
        }
        const double ms = (endSec - startSec) * 1000.0;
        if (ms > mMaxMs) mMaxMs = ms;
        if (ms > mBudgetMs) mOverBudget++;
        mRenderSec += endSec - startSec;
        mDraws += draws;
        mInvisible += invisible;
        mFrames++;
        mWindowEnd = endSec;
        return WallSec() >= mWindowSec;
    }

    double WallSec() const { return mFrames ? mWindowEnd - mWindowStart : 0.0; }
    int Frames() const { return mFrames; }
    // Frames drawn per second of wall time: what the screen got.
    double FramesPerSec() const { return WallSec() > 0.0 ? mFrames / WallSec() : 0.0; }
    double RenderAvgMs() const { return mFrames ? mRenderSec * 1000.0 / mFrames : 0.0; }
    double RenderMaxMs() const { return mMaxMs; }
    int OverBudget() const { return mOverBudget; }

    // "[PERF] 300 frames in 5.00 s (60.0 a second) | render avg 15.33 ms, max
    //  19.70 ms, 64 over 16.67 ms | 326 draws/frame (150 not sent: invisible)"
    int Format(char* out, size_t size) const {
        char notSent[48] = "";
        if (mFrames && mInvisible > 0)
            snprintf(notSent, sizeof(notSent), " (%.0f not sent: invisible)",
                     (double)mInvisible / mFrames);
        return snprintf(out, size,
                        "[PERF] %d frames in %.2f s (%.1f a second) | render avg %.2f ms, max %.2f ms, "
                        "%d over %.2f ms | %.0f draws/frame%s",
                        mFrames, WallSec(), FramesPerSec(), RenderAvgMs(), mMaxMs, mOverBudget,
                        mBudgetMs, mFrames ? (double)mDraws / mFrames : 0.0, notSent);
    }

    void Reset() {
        mFrames = 0;
        mRenderSec = 0.0;
        mMaxMs = 0.0;
        mOverBudget = 0;
        mDraws = 0;
        mInvisible = 0;
        mWindowStart = mWindowEnd;  // the next window opens where this one closed
    }

private:
    double mWindowSec, mBudgetMs;
    bool mOpen = false;
    double mWindowStart = 0.0, mWindowEnd = 0.0;
    double mRenderSec = 0.0, mMaxMs = 0.0;
    int mFrames = 0, mOverBudget = 0;
    long long mDraws = 0, mInvisible = 0;
};

} // namespace milo
