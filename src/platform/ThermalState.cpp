// The thermal level's sources and the governor's driver (platform/ThermalState.h).
#include "platform/ThermalState.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifndef __EMSCRIPTEN__
#include "gfx/GpuDevice.h"
#include "gfx/GpuTimers.h"  // the rate rule's headroom signal (the GPU's span)
#endif

#ifdef __APPLE__
extern "C" int MiloMacThermalState();  // ThermalState_Mac.mm
#endif

namespace milo {

int ThermalOsLevel() {
#ifdef __APPLE__
    const int v = MiloMacThermalState();
    return v >= kThermalNominal && v <= kThermalCritical ? v : kThermalUnknown;
#else
    return kThermalUnknown;
#endif
}

int ParseThermalStateKnob(const char* value) {
    if (!value || !value[0] || value[1]) return kThermalUnknown;
    return value[0] >= '0' && value[0] <= '3' ? value[0] - '0' : kThermalUnknown;
}

bool ParseThermalGovernorKnob(const char* value) {
    if (!value || !*value) return true;
    return !(strcmp(value, "0") == 0 || strcmp(value, "off") == 0 ||
             strcmp(value, "false") == 0 || strcmp(value, "no") == 0);
}

namespace {
constexpr int kNotSet = -2;
std::atomic<int> sSetLevel{kNotSet};
std::atomic<double> sTargetFps{0.0};
std::mutex sStatusMutex;
ThermalStatus sStatus;
bool sStatusPublished = false;
}  // namespace

void ThermalSetInjectedLevel(int level) {
    sSetLevel.store(level >= kThermalNominal && level <= kThermalCritical ? level : kNotSet);
}

int ThermalInjectedLevel() {
    const int set = sSetLevel.load();
    if (set != kNotSet) return set;
    static const int sEnv = ParseThermalStateKnob(getenv("MILO_THERMAL_STATE"));
    return sEnv;
}

void ThermalSetTargetFps(double fps) { sTargetFps.store(fps > 0.0 ? fps : 0.0); }

ThermalStatus ThermalGetStatus() {
    std::lock_guard<std::mutex> lock(sStatusMutex);
    if (!sStatusPublished) {
        // The governor has not run (no level yet): say what it would be given.
        static const bool sEnabled = ParseThermalGovernorKnob(getenv("MILO_THERMAL_GOVERNOR"));
        static const bool sRate = ParseThermalGovernorKnob(getenv("MILO_OUTPUT_RATE_GOVERNOR"));
        sStatus.enabled = sEnabled;  // read once: a consumer may ask every frame
        sStatus.rateRule = sEnabled && sRate;
        sStatus.level = ThermalInjectedLevel();
        sStatus.injected = sStatus.level != kThermalUnknown;
        sStatus.targetFps = sTargetFps.load();
    }
    return sStatus;
}

#ifndef __EMSCRIPTEN__
namespace {
ThermalGovernorConfig ConfigFromEnv() {
    ThermalGovernorConfig cfg;
    cfg.enabled = ParseThermalGovernorKnob(getenv("MILO_THERMAL_GOVERNOR"));
    cfg.rateRule = ParseThermalGovernorKnob(getenv("MILO_OUTPUT_RATE_GOVERNOR"));
    if (const char* v = getenv("MILO_THERMAL_COOL_SECONDS"); v && *v && atof(v) > 0.0)
        cfg.coolDwellSeconds = atof(v);
    return cfg;
}
double NowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

void ThermalGovernorFrame(GpuDevice& gpu) {
    static ThermalGovernor sGovernor(ConfigFromEnv());
    static double sOsReadAt = -1.0;
    static int sOsLevel = kThermalUnknown;
    static bool sEverActive = false;
    static long long sGpuWindowAtChange = -1;  // GpuTimers' window when the size last changed

    const bool enabled = sGovernor.Config().enabled;
    const double now = NowSeconds();
    int level = ThermalInjectedLevel();
    const bool injected = level != kThermalUnknown;
    if (!injected && !gpu.IsHeadless()) {
        if (sOsReadAt < 0.0 || now - sOsReadAt >= 1.0) {
            sOsLevel = ThermalOsLevel();
            sOsReadAt = now;
        }
        level = sOsLevel;
    }
    // No level, ever, and no output above the rate rule's floor: nothing to
    // run, nothing to say (the common case: every platform but macOS without
    // an injected level, at the default 1080-row cap).
    const int baseRows = gpu.BaseOutputRows();
    const bool oversized = enabled && sGovernor.Config().rateRule &&
                           baseRows > sGovernor.Config().rateFloorRows && sTargetFps.load() > 0.0;
    if (level == kThermalUnknown && !sEverActive && !oversized) return;
    sEverActive = true;

    // The headroom signal: the GPU's span for a frame, from a window that
    // began after the size last changed (an older one measured another size).
    double gpuMs = -1.0;
    long long gpuWindow = -1;
    if (GpuTimers::Active()) {
        const GpuTimers::Snapshot snap = GpuTimers::Get().Latest();
        gpuWindow = snap.windowIndex;
        if (snap.active && snap.frames > 0 && snap.windowIndex > sGpuWindowAtChange + 1)
            gpuMs = snap.spanMeanMs;
    }

    ThermalGovernorInput in;
    in.nowSeconds = now;
    in.level = level;
    in.targetFps = sTargetFps.load();
    in.baseRows = baseRows;
    in.paceMeaningful = gpu.IsWindowFocused();
    in.gpuFrameMs = gpuMs;
    const int cap = sGovernor.Frame(in);
    const GovernorChange why = sGovernor.LastChange();
    if (enabled && cap != gpu.RuntimeRowCap()) {
        const int wasW = gpu.OutputWidth(), wasH = gpu.OutputHeight(), wasCap = gpu.RuntimeRowCap();
        if (gpu.SetRuntimeRowCap(cap)) {
            sGpuWindowAtChange = gpuWindow;
            char capText[24] = "none";
            if (cap > 0) snprintf(capText, sizeof(capText), "%d", cap);
            if (why == kGovernorRateDown) {
                printf("GpuDevice: output rate rule, thermal level %d (%s), frames late: last "
                       "window %.1f fps against a %.1f fps target: output %dx%d -> %dx%d (rate "
                       "row cap %s; never below %d rows by this rule; MILO_OUTPUT_RATE_GOVERNOR=0 "
                       "to keep the size)\n",
                       level, ThermalLevelName(level), sGovernor.LastWindowFps(), in.targetFps,
                       wasW, wasH, gpu.OutputWidth(), gpu.OutputHeight(), capText,
                       sGovernor.Config().rateFloorRows);
            } else if (why == kGovernorRateUp) {
                printf("GpuDevice: output rate rule, headroom for %.0f s: GPU %.2f ms a frame, "
                       "%.2f ms predicted at the next size against a %.2f ms budget (%.0f %% of "
                       "%.1f fps): output %dx%d -> %dx%d (rate row cap %s)\n",
                       sGovernor.RateUpDwellSeconds(), gpuMs, sGovernor.LastPredictedMs(),
                       sGovernor.LastBudgetMs(), sGovernor.Config().rateHeadroomShare * 100.0,
                       in.targetFps, wasW, wasH, gpu.OutputWidth(), gpu.OutputHeight(), capText);
            } else {
                const bool down = cap > 0 && (wasCap <= 0 || cap < wasCap);
                printf("GpuDevice: thermal level %d (%s), %s: output %dx%d -> %dx%d (thermal row "
                       "cap %s)\n",
                       level, ThermalLevelName(level), down ? "frames late" : "cooled", wasW, wasH,
                       gpu.OutputWidth(), gpu.OutputHeight(), capText);
            }
            fflush(stdout);
        }
    }

    ThermalStatus s;
    s.enabled = enabled;
    s.rateRule = enabled && sGovernor.Config().rateRule;
    s.level = level;
    s.injected = injected;
    s.targetFps = in.targetFps;
    s.windowFps = sGovernor.LastWindowFps();
    s.lateWindows = sGovernor.LateWindows();
    s.rowCap = sGovernor.RowCap();
    s.baseRows = in.baseRows;
    s.outputWidth = gpu.OutputWidth();
    s.outputHeight = gpu.OutputHeight();
    s.stepsDown = sGovernor.StepsDown();
    s.stepsUp = sGovernor.StepsUp();
    s.thermalRowCap = sGovernor.ThermalCap();
    s.rateRowCap = sGovernor.RateCap();
    s.rateStepsDown = sGovernor.RateStepsDown();
    s.rateStepsUp = sGovernor.RateStepsUp();
    s.gpuFrameMs = gpuMs;
    s.coolSeconds = sGovernor.CoolSeconds(now);
    std::lock_guard<std::mutex> lock(sStatusMutex);
    sStatus = s;
    sStatusPublished = true;
}
#else
void ThermalGovernorFrame(GpuDevice&) {}
#endif

}  // namespace milo
