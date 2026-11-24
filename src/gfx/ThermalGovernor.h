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
//               given up by this rule, however late the frames are: a slow
//               scene on a cool machine is not its business. (An output drawn
//               ABOVE 1080 rows is the one exception, and it is the rate rule's,
//               below: it never goes under 1080.)
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
//               Frame() always answers 0, and keeps no state (the rate rule
//               below included).
//
// THE RATE RULE (oversized outputs; engine branch np-resgov, 2026-11-24)
// WHY (MEASURED 2026-11-23, the same M3 Air on a 4K TV that runs at 30 Hz, its
// launcher exporting MILO_OUTPUT_MAX_ROWS=0, so the frame was drawn at
// 3840x2160): 163 five-second windows of songs read 12.4-23.8 fps (min-mean)
// against the display's 30, song select 18.7-26.1, results 21.7-24.9, and the
// thermal rule above did nothing for half an hour because macOS still said
// nominal; when it finally said fair, the step to 1920x1080 brought song select
// to a steady 30.0. On that machine 1440 rows cost 15.5 ms a frame and 1080
// rows 5.8 (Break It Down, full clock); 2160 rows are 2.25x the pixels of 1440
// and cannot hold 30. Rows above 1080 are only ever drawn because someone
// lifted the cap (or a big window on a discrete GPU) -- the cap's own default
// is 1080 -- so they are the one size the game may give up on a cool machine.
//   permission  the output is drawn above rateFloorRows (1080) at ANY level,
//               nominal and unknown included. 1080 rows and under stay the
//               thermal rule's: this rule never takes a frame below 1080.
//   trigger     the same late windows as above (the same lateRatio, share and
//               hitch rules), against the same target -- which the consumer
//               gives as the rate the display can actually show: a 30 Hz
//               display's steady 30 fps is on time (dc3's Dc3ThermalTargetFps).
//               lateWindowsRate (8) in a row: more evidence than the fair rule
//               (5) asks, because nothing is suffering but the frame rate, and
//               a screen that is slow for a few seconds (a venue streaming in)
//               should not cost the player the size they asked for.
//   ... and the pixels must be the cause. Late frames on a CPU-bound stretch
//               are not made faster by fewer pixels. MEASURED 2026-11-24 on this
//               box's RTX 3090, headless at 3840x2160 under a load of 30: boot
//               ran 28.7 fps against 60 with the GPU's span at 0.6-5.7 ms, and a
//               first build of this rule gave up 2160 rows for it. So when the
//               GPU's time is known (gpuFrameMs) a window only counts as late
//               for this rule if the GPU alone takes rateGpuBoundShare (0.75)
//               of the target frame time or more. When it is not known (GPU
//               timers off, the default), lateness alone counts on an adapter
//               that is not discrete (the M3 Air's case: an integrated GPU is
//               what runs out at 2160 rows), and never on a discrete one: a
//               discrete adapter's big window keeps its size unless the GPU is
//               measured to be what is slow.
//   steps       kRateRowSteps = 1440, then 1080: each time the largest one below
//               the size in force. One step at a time: 2160 -> 1440 is already
//               2.25x fewer pixels, enough for the measured Mac, and a machine
//               that holds 1440 keeps it.
//   settle      shared with the thermal rule: stepSettleSeconds after any change
//               of either rule, before either judges a window again.
//   step up     needs a POSITIVE headroom signal, not just frames on time: under
//               vsync every frame is on time whenever it fits, so "on time" says
//               nothing about whether the next size up would fit. The signal is
//               the GPU's time for a frame (ThermalGovernorInput::gpuFrameMs:
//               GpuTimers' first-begin-to-last-end span, never the sum of passes,
//               which on Apple/Metal overlap), scaled by the pixel ratio of the
//               next size up (pixels are what the size changes, and what the
//               GPU-bound Mac pays for), within rateHeadroomShare (0.70) of the
//               frame's time at the target rate: 30 % kept back for a heavier
//               screen than the one measured. Every judged window must show it,
//               unbroken, for rateUpDwellSeconds (30); a late window or one
//               without the signal restarts the dwell. No signal at all (GPU
//               timers off -- MILO_GPU_TIMERS is off by default -- or no
//               timestamp queries): no step up; the size stays until the output
//               is resized or the game relaunched. A cool machine that could
//               not hold the size is not given it back on a guess.
//               A step up that is late again (stepped down within
//               rateRetrySeconds, 120, of it) doubles the dwell, up to
//               rateUpDwellMaxSeconds (480): a size that cannot be held costs
//               one hitch, then rarer and rarer ones, never a flicker.
//               While a thermal cap binds below the rate cap, the rate rule does
//               not step up: the thermal rule gives its own steps back first.
//   off         rateRule false (MILO_OUTPUT_RATE_GOVERNOR=0): the thermal rule
//               alone, exactly as before.
//   not rows first?  Supersampling is the cheaper thing to give up where there
//               is any (MEASURED on the GPD's 890M, 2026-11-24: 1080 rows at
//               render scale 2 cost 19.6 ms against 6.28 at scale 1, ~3x), but
//               above 1080 rows the default scale is already 1: 2x of 1081 rows
//               passes kRenderScaleCap's 3840x2160 (ResolveRenderScale), and the
//               M3 Air's 2160-row frame logged "render scale 1.00". The only
//               supersampling this rule could meet is an explicit
//               MILO_RENDER_SCALE, which it leaves alone (a follow-up: give that
//               up before rows). MSAA is not a step either: its sample count is
//               baked into every render pipeline, so changing it is a pipeline
//               rebuild, not a between-frames resize.
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
// The rate rule's steps, largest first (its floor is the last).
constexpr int kRateRowSteps[] = {1440, 1080};
constexpr int kRateRowStepCount = 2;

// Which rule made the last change (ThermalGovernor::LastChange).
enum GovernorChange {
    kGovernorNoChange = 0,
    kGovernorThermalDown,
    kGovernorThermalUp,
    kGovernorRateDown,
    kGovernorRateUp,
};

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
    // The rate rule.
    bool rateRule = true;
    int rateFloorRows = 1080;
    int lateWindowsRate = 8;
    double rateHeadroomShare = 0.70;
    double rateUpDwellSeconds = 30.0;
    double rateUpDwellMaxSeconds = 480.0;
    double rateRetrySeconds = 120.0;
    double rateGpuBoundShare = 0.75;
};

struct ThermalGovernorInput {
    double nowSeconds = 0.0;      // any monotonic origin; one call per frame
    int level = kThermalUnknown;  // ThermalLevel
    double targetFps = 0.0;       // the rate the game is meant to hold; 0 = not given: never late
    int baseRows = 0;             // the output's height under the existing rules alone; 0 = no steps
    bool paceMeaningful = true;   // false: this frame's period says nothing (see header)
    double gpuFrameMs = -1.0;     // the GPU's time for a frame at the present size; < 0 = unknown
    bool discreteGpu = false;     // the adapter is discrete (the rate rule, without gpuFrameMs)
};

class ThermalGovernor {
public:
    ThermalGovernor() = default;
    explicit ThermalGovernor(const ThermalGovernorConfig& cfg) : mCfg(cfg) {}

    // Once per frame, at its start. The row cap to hold (0 = none).
    int Frame(const ThermalGovernorInput& in);

    int RowCap() const { return mCfg.enabled ? mCap : 0; }
    // Each rule's own cap (0 = none); RowCap() is the tighter of the two.
    int ThermalCap() const { return mCfg.enabled ? mThermalCap : 0; }
    int RateCap() const { return mCfg.enabled ? mRateCap : 0; }
    // The rule that made the change Frame() last answered with.
    GovernorChange LastChange() const { return mLastChange; }
    int RateStepsDown() const { return mRateStepsDown; }
    int RateStepsUp() const { return mRateStepsUp; }
    // The last judged window's headroom: the GPU time predicted at the next
    // size up, and the budget it was held to (0 when not judged).
    double LastPredictedMs() const { return mLastPredictedMs; }
    double LastBudgetMs() const { return mLastBudgetMs; }
    double RateUpDwellSeconds() const { return mRateUpDwell; }
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
    // The rate rule's: the next of kRateRowSteps below the size in force (or
    // `cap` itself: nothing above rateFloor to give), and the next above `cap`
    // (0 = released: the step would not bind on an output of baseRows).
    static int RateStepDown(int cap, int baseRows, int rateFloorRows);
    static int RateStepUp(int cap, int baseRows);

private:
    void ResetWindow(double now);
    void Changed(double now);

    ThermalGovernorConfig mCfg;
    void SetCap(double now, int thermalCap, int rateCap, GovernorChange why);

    int mCap = 0;
    int mThermalCap = 0;
    int mRateCap = 0;
    GovernorChange mLastChange = kGovernorNoChange;
    int mRateStepsDown = 0;
    int mRateStepsUp = 0;
    double mHeadroomSince = -1.0;
    double mRateUpAt = -1.0;  // the last rate step up
    double mRateUpDwell = 0.0;  // 0 = the config's
    double mLastPredictedMs = 0.0;
    double mLastBudgetMs = 0.0;
    double mLastFrame = -1.0;
    double mWindowStart = -1.0;
    int mWindowFrames = 0;
    int mWindowLateFrames = 0;
    int mLateWindows = 0;
    int mRateLateWindows = 0;  // late windows in a row that the pixels are blamed for
    double mLastWindowFps = 0.0;
    double mSettleUntil = -1.0;
    double mCoolSince = -1.0;
    int mStepsDown = 0;
    int mStepsUp = 0;
};

}  // namespace milo
