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
        sStatus.enabled = sEnabled;  // read once: a consumer may ask every frame
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
    // No level, ever: nothing to run, nothing to say (the common case, and
    // every platform but macOS without an injected level).
    if (level == kThermalUnknown && !sEverActive) return;
    sEverActive = true;

    ThermalGovernorInput in;
    in.nowSeconds = now;
    in.level = level;
    in.targetFps = sTargetFps.load();
    in.baseRows = gpu.BaseOutputRows();
    in.paceMeaningful = gpu.IsWindowFocused();
    const int cap = sGovernor.Frame(in);
    if (enabled && cap != gpu.RuntimeRowCap()) {
        const int wasW = gpu.OutputWidth(), wasH = gpu.OutputHeight(), wasCap = gpu.RuntimeRowCap();
        if (gpu.SetRuntimeRowCap(cap)) {
            char capText[24] = "none";
            if (cap > 0) snprintf(capText, sizeof(capText), "%d", cap);
            const bool down = cap > 0 && (wasCap <= 0 || cap < wasCap);
            printf("GpuDevice: thermal level %d (%s), %s: output %dx%d -> %dx%d (thermal row "
                   "cap %s)\n",
                   level, ThermalLevelName(level), down ? "frames late" : "cooled", wasW, wasH,
                   gpu.OutputWidth(), gpu.OutputHeight(), capText);
            fflush(stdout);
        }
    }

    ThermalStatus s;
    s.enabled = enabled;
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
    s.coolSeconds = sGovernor.CoolSeconds(now);
    std::lock_guard<std::mutex> lock(sStatusMutex);
    sStatus = s;
    sStatusPublished = true;
}
#else
void ThermalGovernorFrame(GpuDevice&) {}
#endif

}  // namespace milo
