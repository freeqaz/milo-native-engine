// The machine's thermal pressure level, and the driver that runs the thermal
// governor (gfx/ThermalGovernor.h, which carries the rule and its constants)
// against a GpuDevice once per frame.
//
// The level, in the order asked:
//   1. ThermalSetInjectedLevel(0..3)  set while running (dc3: PUT /api/thermal);
//      kThermalUnknown withdraws it
//   2. MILO_THERMAL_STATE=0..3        the same, from the environment
//   3. the OS, windowed runs only     macOS: NSProcessInfo.thermalState
//      (ThermalState_Mac.mm), read once a second. Every other platform
//      reports kThermalUnknown, and so does a headless run: a headless frame
//      is the size it was asked for, and only an injected level moves it.
// With no level (kThermalUnknown) the governor never acts, so a platform
// without one, and every run with nothing injected on it, behaves as before.
//
// MILO_THERMAL_GOVERNOR=0 turns the governor off (default on): the level is
// still readable, nothing is ever resized.
// MILO_OUTPUT_RATE_GOVERNOR=0 turns off the governor's rate rule alone (default
// on; gfx/ThermalGovernor.h, THE RATE RULE): an output drawn above 1080 rows is
// then never made smaller on a cool machine, however late its frames. The rate
// rule runs whatever the level, headless included (a headless frame of 2160
// rows is as slow as a windowed one), but only for an output above 1080 rows.
// Its step up reads the GPU's time for a frame, which only exists with
// MILO_GPU_TIMERS=1 on an adapter with timestamp queries; without it a size
// given up stays given up until the output is resized.
// MILO_THERMAL_COOL_SECONDS=N replaces the governor's cool dwell (90 s) -- a
// test's knob, so a run can watch the size come back without waiting minutes.
//
// The consumer says what rate the game is meant to hold (ThermalSetTargetFps:
// its frame cap, else the display's rate). Until it does, no frame is late and
// the governor idles: a consumer that never calls it (rb3, rb3-xenon today)
// sees no change at any level.
//
// Desktop only. The web build compiles this file but WgpuRnd does not call
// ThermalGovernorFrame there, and the level is kThermalUnknown: a browser
// exposes no thermal state.
#pragma once

#include "gfx/ThermalGovernor.h"

class GpuDevice;

namespace milo {

// The OS's level now (kThermalUnknown where it reports none).
int ThermalOsLevel();
// The injected level: the setter's, else MILO_THERMAL_STATE's, else
// kThermalUnknown. Values outside 0..3 withdraw the setter's.
void ThermalSetInjectedLevel(int level);
int ThermalInjectedLevel();
// MILO_THERMAL_STATE's parse: "0".."3" -> the level, anything else unknown.
int ParseThermalStateKnob(const char* value);
// MILO_THERMAL_GOVERNOR's parse: unset/empty/anything else = on; "0", "off",
// "false", "no" = off.
bool ParseThermalGovernorKnob(const char* value);

// The rate the game is meant to hold, frames per second (0 = not given).
void ThermalSetTargetFps(double fps);

// Once per frame, before the frame is acquired (WgpuRnd::BeginDrawing).
void ThermalGovernorFrame(GpuDevice& gpu);

// What the governor last saw and did (a value copy; any thread).
struct ThermalStatus {
    bool enabled = true;        // MILO_THERMAL_GOVERNOR
    bool rateRule = true;       // ... and MILO_OUTPUT_RATE_GOVERNOR
    int level = kThermalUnknown;  // the level the governor was given
    bool injected = false;      // ... and whether it was injected
    double targetFps = 0.0;
    double windowFps = 0.0;     // the last judged window's rate
    int lateWindows = 0;        // late windows in a row
    int rowCap = 0;             // the governor's cap (0 = none)
    int baseRows = 0;           // the output's height without it
    int outputWidth = 0, outputHeight = 0;
    int stepsDown = 0, stepsUp = 0;
    double coolSeconds = 0.0;   // the level's time at nominal, unbroken
    int thermalRowCap = 0, rateRowCap = 0;  // each rule's own cap; rowCap is the tighter
    int rateStepsDown = 0, rateStepsUp = 0;  // the rate rule's changes (not in stepsDown/Up)
    double gpuFrameMs = -1.0;   // the headroom signal last given (< 0 = none)
};
ThermalStatus ThermalGetStatus();

}  // namespace milo
