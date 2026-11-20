// The thermal governor: gives up output resolution, in steps, when the machine
// reports thermal pressure AND the game is no longer holding its frame rate,
// and gives it back, one step at a time, once the machine has cooled.
//
// Pure: no GPU, no OS, no clock of its own. The level, the frame's time and
// the frame rate the game is meant to hold are all given (ThermalGovernorInput);
// the answer is a row cap for the frame's output (0 = none), which
// platform/ThermalState.cpp hands to GpuDevice::SetRuntimeRowCap.
//
// WHY (dc3 PLAN H-128; MEASURED 2026-11-19 on a fanless M3 MacBook Air drawing
// 2560x1440 with the GPU 92-95 % busy): about 12 minutes in, macOS went from
// thermal state 0 to 2 and throttled. The same song-select screen fell from
// 62 fps to 32-42, a song from 64-69 fps to 45-51, the pose tracker's
// inference from 21 ms to 50-80 ms.
//
// THE RULE
//   permission  the level is fair (1) or worse. At nominal (0), or where the
//               platform reports no level (kThermalUnknown), nothing is ever
//               given up, however late the frames are: a slow scene on a cool
//               machine is not this governor's business.
//   trigger     late frames, judged in windows of windowSeconds (1 s). A
//               window is late when
//                 - its rate is under lateRatio (0.90) of the target: 54 fps
//                   against 60. Not 0.85: the throttled song above read 45-51,
//                   and 0.85 * 60 = 51 would let its upper end pass; and
//                 - at least lateFrameShare (10 %) of its frames, and at least
//                   minLateFrames (3), each took longer than the same margin.
//                   That is what keeps a hitch out: a load's one 3 s frame, or
//                   two half-second ones, halve a window's rate with one or
//                   two late frames, while a throttled 51 fps under vsync has
//                   18 % of its frames a refresh late (45 fps: 33 %).
//               A window the caller marks not meaningful (paceMeaningful
//               false: the window has no focus, so the compositor paces it)
//               is not judged and breaks the run of late windows.
//   step down   lateWindowsFair (5) late windows in a row at fair,
//               lateWindowsSerious (2) at serious or critical: the OS is
//               already throttling there, so waiting costs the player frames.
//   steps       row caps kThermalRowSteps = 1080, 900, 720, each time the
//               largest one below the output's present height. Never below
//               floorRows (720: the UI is authored at 1280x720, and under it
//               text loses legibility). The existing cap rules (OutputRowCap,
//               the window's own size) still hold; the tighter cap wins
//               (TighterRowCap), so with the default 1080 cap in force the
//               first step is 900, an uncapped 1440-row frame steps to 1080
//               first, and a frame already at 720 rows or fewer has no step.
//   settle      after any change, stepSettleSeconds (5) pass before a window
//               is judged again: every size change re-creates the frame's
//               render targets, which is a hitch of its own, and the new size
//               has to show what it does before another is tried. So two
//               steps down are at least 7 s apart at serious, 10 s at fair.
//   step up     ONE step once the level has been nominal (or unknown: the
//               injected level was withdrawn) for coolDwellSeconds (90)
//               without a break; each further step needs a whole dwell again.
//               Any reading of fair or worse restarts the dwell, so a level
//               that wobbles between 0 and 1 holds the size where it is. 90 s:
//               a size change is a hitch, the machine took 12 minutes to heat
//               at the full size, and a restored size that reheats it costs a
//               step down again -- with 90 s the worst case is one change per
//               minute and a half, not a flicker. Lateness is not asked for a
//               step up: the frame is given back because the machine cooled.
//   off         ThermalGovernorConfig::enabled false (MILO_THERMAL_GOVERNOR=0):
//               Frame() always answers 0, and keeps no state.
//
// Not in the rule: the render scale. A thermal cap never raises it
// (ResolveFrameSizesThermal, gfx/GpuDevice.h), so a capped frame is never
// drawn with more pixels than the uncapped one; a supersampled frame of 720
// rows or fewer keeps its supersampling (nothing is given up there yet).
#pragma once

namespace milo {

// NSProcessInfoThermalState's four values, and "the platform reports none".
enum ThermalLevel {
    kThermalUnknown = -1,
    kThermalNominal = 0,
    kThermalFair = 1,
    kThermalSerious = 2,
    kThermalCritical = 3,
};
// "unknown", "nominal", "fair", "serious", "critical".
const char* ThermalLevelName(int level);

// The steps, largest first.
constexpr int kThermalRowSteps[] = {1080, 900, 720};
constexpr int kThermalRowStepCount = 3;

struct ThermalGovernorConfig {
    bool enabled = true;
    double windowSeconds = 1.0;
    double lateRatio = 0.90;
    double lateFrameShare = 0.10;
    int minLateFrames = 3;
    int lateWindowsFair = 5;
    int lateWindowsSerious = 2;
    double stepSettleSeconds = 5.0;
    double coolDwellSeconds = 90.0;
    int floorRows = 720;
};

struct ThermalGovernorInput {
    double nowSeconds = 0.0;      // any monotonic origin; one call per frame
    int level = kThermalUnknown;  // ThermalLevel
    double targetFps = 0.0;       // the rate the game is meant to hold; 0 = not given: never late
    int baseRows = 0;             // the output's height under the existing rules alone; 0 = no steps
    bool paceMeaningful = true;   // false: this frame's period says nothing (see header)
};

class ThermalGovernor {
public:
    ThermalGovernor() = default;
    explicit ThermalGovernor(const ThermalGovernorConfig& cfg) : mCfg(cfg) {}

    // Once per frame, at its start. The row cap to hold (0 = none).
    int Frame(const ThermalGovernorInput& in);

    int RowCap() const { return mCfg.enabled ? mCap : 0; }
    const ThermalGovernorConfig& Config() const { return mCfg; }
    // Totals and the last judged window, for a status line.
    int StepsDown() const { return mStepsDown; }
    int StepsUp() const { return mStepsUp; }
    int LateWindows() const { return mLateWindows; }
    double LastWindowFps() const { return mLastWindowFps; }
    // Seconds the level has been nominal without a break (0 when it is not).
    double CoolSeconds(double nowSeconds) const {
        return mCoolSince >= 0.0 ? nowSeconds - mCoolSince : 0.0;
    }

    // The next cap down from `cap` (0 = none in force) for an output of
    // baseRows, or `cap` itself when there is no step left; and the next up
    // (0 = the cap is released).
    static int StepDown(int cap, int baseRows, int floorRows);
    static int StepUp(int cap, int baseRows);

private:
    void ResetWindow(double now);
    void Changed(double now);

    ThermalGovernorConfig mCfg;
    int mCap = 0;
    double mLastFrame = -1.0;
    double mWindowStart = -1.0;
    int mWindowFrames = 0;
    int mWindowLateFrames = 0;
    int mLateWindows = 0;
    double mLastWindowFps = 0.0;
    double mSettleUntil = -1.0;
    double mCoolSince = -1.0;
    int mStepsDown = 0;
    int mStepsUp = 0;
};

}  // namespace milo
