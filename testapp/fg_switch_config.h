#pragma once

#include "fg_upscale_policy.h"

namespace testapp::fg {

// API-neutral configuration shared by the DX12 and Vulkan FG switch applications. Runtime state
// (contexts, suspension flags, transition epochs) deliberately lives outside this structure.
struct FgSwitchConfig {
    int windowWidth = 1920;
    int windowHeight = 1080;
    int gpuLoadPasses = 40;
    int vsync = 0;
    int fullscreen = 0;

    bool fsrReloadRuntimeOnSwitch = true;
    bool streamlinePreloadInitialOff = false;
    bool fsrKeepRuntimeLoadedInitialOff = false;
    bool fsrStartupDisabledContextStress = false;
    bool fsrSuspendResumeStress = true;
    int fsrSuspendResumeIntervalSeconds = 3;
    bool dlssSuspendResumeStress = false;
    int dlssSuspendResumeIntervalSeconds = 3;
    bool dlssOffAfterActiveStress = false;
    bool apiDebug = false;
    bool fsrPresentCallbackStress = true;
    int fsrPresentCallbackToggleIntervalSeconds = 6;
    bool fsrDegenerateUiResource = false;
    bool videoMemoryQueryStress = true;
    int videoMemoryQueryCountPerFrame = 96;
    int bootstrapNativeSwapchainStressCount = 0;
    int startupNativeSwapchainRecreateCount = 0;
    bool asyncRuntimePreload = true;

    int autoExitSeconds = 0;
    int autoFsrStartSeconds = 3;
    int autoDlssStartSeconds = 12;
    int autoReturnFsrSeconds = 30;

    bool upscalingEnabled = true;
    UpscaleQuality upscaleQuality = UpscaleQuality::Quality;
    int upscaleScalePercent = 0;
    char dlssPreset = 0;
    bool dlssHdrInput = false;
    int fsrUpscaleVersion = 0;
    bool fsrSharpeningEnabled = false;
    int fsrSharpnessPercent = 80;
};

inline int ClampSwitchConfigInt(int value, int minValue, int maxValue) {
    if (value < minValue) {
        return minValue;
    }
    return value > maxValue ? maxValue : value;
}

inline void NormalizeAutoSequenceTimings(FgSwitchConfig* config) {
    if (!config) {
        return;
    }
    config->autoFsrStartSeconds = ClampSwitchConfigInt(config->autoFsrStartSeconds, 0, 3598);
    config->autoDlssStartSeconds =
        ClampSwitchConfigInt(config->autoDlssStartSeconds, config->autoFsrStartSeconds + 1, 3599);
    config->autoReturnFsrSeconds =
        ClampSwitchConfigInt(config->autoReturnFsrSeconds, config->autoDlssStartSeconds + 1, 3600);
}

}  // namespace testapp::fg
