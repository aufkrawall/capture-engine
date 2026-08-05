#include "vulkan_fg_switch_test_internal.h"

namespace testapp::vkfg {
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
AppState g_App;
}

// The existing configuration parser is API-neutral after these aliases. Keeping one parser is what
// guarantees positional arguments, INI keys, command-line switches, and defaults remain identical.
static testapp::fg::FgSwitchConfig& g_SwitchConfig = g_App.config;

static int& g_WindowWidth = g_SwitchConfig.windowWidth;

static int& g_WindowHeight = g_SwitchConfig.windowHeight;

static int& g_GpuLoadPasses = g_SwitchConfig.gpuLoadPasses;

static int& g_VSync = g_SwitchConfig.vsync;

static int& g_Fullscreen = g_SwitchConfig.fullscreen;

static bool& g_FsrReloadRuntimeOnSwitch = g_SwitchConfig.fsrReloadRuntimeOnSwitch;

static bool& g_StreamlinePreloadInitialOff = g_SwitchConfig.streamlinePreloadInitialOff;

static bool& g_FsrKeepRuntimeLoadedInitialOff = g_SwitchConfig.fsrKeepRuntimeLoadedInitialOff;

static bool& g_FsrStartupDisabledContextStress = g_SwitchConfig.fsrStartupDisabledContextStress;

static bool& g_FsrSuspendResumeStress = g_SwitchConfig.fsrSuspendResumeStress;

static int& g_FsrSuspendResumeIntervalSeconds = g_SwitchConfig.fsrSuspendResumeIntervalSeconds;

static bool& g_DlssSuspendResumeStress = g_SwitchConfig.dlssSuspendResumeStress;

static int& g_DlssSuspendResumeIntervalSeconds = g_SwitchConfig.dlssSuspendResumeIntervalSeconds;

static bool& g_DlssOffAfterActiveStress = g_SwitchConfig.dlssOffAfterActiveStress;

static bool& g_EnableDred = g_SwitchConfig.apiDebug;

static bool& g_FsrPresentCallbackStress = g_SwitchConfig.fsrPresentCallbackStress;

static int& g_FsrPresentCallbackToggleIntervalSeconds = g_SwitchConfig.fsrPresentCallbackToggleIntervalSeconds;

static bool& g_FsrDegenerateUiResource = g_SwitchConfig.fsrDegenerateUiResource;

static bool& g_DxgiVideoMemoryQueryStress = g_SwitchConfig.videoMemoryQueryStress;

static int& g_DxgiVideoMemoryQueryCountPerFrame = g_SwitchConfig.videoMemoryQueryCountPerFrame;

static int& g_BootstrapNativeSwapchainStressCount = g_SwitchConfig.bootstrapNativeSwapchainStressCount;

static int& g_StartupNativeSwapchainRecreateCount = g_SwitchConfig.startupNativeSwapchainRecreateCount;

static bool& g_AsyncRuntimePreload = g_SwitchConfig.asyncRuntimePreload;

static int& g_AutoExitSeconds = g_SwitchConfig.autoExitSeconds;

static int& g_AutoFsrStartSeconds = g_SwitchConfig.autoFsrStartSeconds;

static int& g_AutoDlssStartSeconds = g_SwitchConfig.autoDlssStartSeconds;

static int& g_AutoReturnFsrSeconds = g_SwitchConfig.autoReturnFsrSeconds;

static bool& g_UpscalingEnabled = g_SwitchConfig.upscalingEnabled;

static testapp::fg::UpscaleQuality& g_UpscaleQuality = g_SwitchConfig.upscaleQuality;

static int& g_UpscaleScalePercent = g_SwitchConfig.upscaleScalePercent;

static char& g_DlssPresetConfig = g_SwitchConfig.dlssPreset;

static bool& g_DlssHdrInput = g_SwitchConfig.dlssHdrInput;

static int& g_FsrUpscaleVersionConfig = g_SwitchConfig.fsrUpscaleVersion;

static bool& g_FsrSharpeningEnabled = g_SwitchConfig.fsrSharpeningEnabled;

static int& g_FsrSharpnessPercent = g_SwitchConfig.fsrSharpnessPercent;

static int ClampInt(int value, int minValue, int maxValue) {
    if (value < minValue) {
        return minValue;
    }
    if (value > maxValue) {
        return maxValue;
    }
    return value;
}

static void LoadConfig() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string configPath = path;
    size_t pos = configPath.find_last_of("\\/");
    if (pos != std::string::npos) {
        configPath = configPath.substr(0, pos + 1) + "testappconfig.ini";
    }
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_WindowWidth = GetPrivateProfileIntA("Display", "width", g_WindowWidth, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_WindowHeight = GetPrivateProfileIntA("Display", "height", g_WindowHeight, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_GpuLoadPasses = GetPrivateProfileIntA("Performance", "gpu_load", g_GpuLoadPasses, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_VSync = GetPrivateProfileIntA("Rendering", "vsync", g_VSync, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_Fullscreen = GetPrivateProfileIntA("Display", "fullscreen", g_Fullscreen, configPath.c_str());
    g_FsrReloadRuntimeOnSwitch = GetPrivateProfileIntA("Stress", "fsr_reload_runtime_on_switch",
                                                       g_FsrReloadRuntimeOnSwitch ? 1 : 0, configPath.c_str()) != 0;
    g_StreamlinePreloadInitialOff =
        GetPrivateProfileIntA("Stress", "streamline_preload_initial_off", g_StreamlinePreloadInitialOff ? 1 : 0,
                              configPath.c_str()) != 0;
    g_FsrKeepRuntimeLoadedInitialOff =
        GetPrivateProfileIntA("Stress", "fsr_keep_runtime_loaded_initial_off", g_FsrKeepRuntimeLoadedInitialOff ? 1 : 0,
                              configPath.c_str()) != 0;
    g_FsrStartupDisabledContextStress =
        GetPrivateProfileIntA("Stress", "fsr_startup_disabled_context", g_FsrStartupDisabledContextStress ? 1 : 0,
                              configPath.c_str()) != 0;
    g_FsrSuspendResumeStress = GetPrivateProfileIntA("Stress", "fsr_suspend_resume", g_FsrSuspendResumeStress ? 1 : 0,
                                                     configPath.c_str()) != 0;
    g_FsrSuspendResumeIntervalSeconds = ClampInt(
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        GetPrivateProfileIntA("Stress", "fsr_suspend_resume_interval_seconds", g_FsrSuspendResumeIntervalSeconds,
                              configPath.c_str()),
        1, 60);
    g_DlssSuspendResumeStress = GetPrivateProfileIntA("Stress", "dlss_suspend_resume",
                                                      g_DlssSuspendResumeStress ? 1 : 0, configPath.c_str()) != 0;
    g_DlssSuspendResumeIntervalSeconds = ClampInt(
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        GetPrivateProfileIntA("Stress", "dlss_suspend_resume_interval_seconds", g_DlssSuspendResumeIntervalSeconds,
                              configPath.c_str()),
        1, 60);
    g_DlssOffAfterActiveStress = GetPrivateProfileIntA("Stress", "dlss_off_after_active",
                                                       g_DlssOffAfterActiveStress ? 1 : 0, configPath.c_str()) != 0;
    g_FsrPresentCallbackStress =
        GetPrivateProfileIntA("Stress", "fsr_present_callback_toggle_stress",
                              g_FsrPresentCallbackStress ? 1 : 0, configPath.c_str()) != 0;
    g_FsrPresentCallbackToggleIntervalSeconds =
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        ClampInt(GetPrivateProfileIntA("Stress", "fsr_present_callback_toggle_interval_seconds",
                                       g_FsrPresentCallbackToggleIntervalSeconds, configPath.c_str()),
                 1, 120);
    g_FsrDegenerateUiResource = GetPrivateProfileIntA("Stress", "fsr_degenerate_ui_resource",
                                                      g_FsrDegenerateUiResource ? 1 : 0, configPath.c_str()) != 0;
    g_DxgiVideoMemoryQueryStress = GetPrivateProfileIntA("Stress", "dxgi_video_memory_query_stress",
                                                         g_DxgiVideoMemoryQueryStress ? 1 : 0, configPath.c_str()) != 0;
    g_DxgiVideoMemoryQueryCountPerFrame = ClampInt(
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        GetPrivateProfileIntA("Stress", "dxgi_video_memory_query_count_per_frame",
                              g_DxgiVideoMemoryQueryCountPerFrame, configPath.c_str()),
        0, 512);
    g_BootstrapNativeSwapchainStressCount =
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        ClampInt(GetPrivateProfileIntA("Stress", "bootstrap_native_swapchain_stress_count",
                                       g_BootstrapNativeSwapchainStressCount, configPath.c_str()),
                 0, 8);
    g_StartupNativeSwapchainRecreateCount =
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        ClampInt(GetPrivateProfileIntA("Stress", "startup_native_swapchain_recreate_count",
                                       g_StartupNativeSwapchainRecreateCount, configPath.c_str()),
                 0, 8);
    g_AsyncRuntimePreload =
        GetPrivateProfileIntA("Stress", "async_runtime_preload", g_AsyncRuntimePreload ? 1 : 0,
                              configPath.c_str()) != 0;
    g_AutoExitSeconds = ClampInt(
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        GetPrivateProfileIntA("Stress", "auto_exit_seconds", g_AutoExitSeconds, configPath.c_str()), 0, 3600);
    g_AutoFsrStartSeconds = ClampInt(
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        GetPrivateProfileIntA("Stress", "auto_fsr_start_seconds", g_AutoFsrStartSeconds, configPath.c_str()), 0,
        3600);
    g_AutoDlssStartSeconds = ClampInt(
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        GetPrivateProfileIntA("Stress", "auto_dlss_start_seconds", g_AutoDlssStartSeconds, configPath.c_str()), 0,
        3600);
    g_AutoReturnFsrSeconds = ClampInt(
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        GetPrivateProfileIntA("Stress", "auto_return_fsr_seconds", g_AutoReturnFsrSeconds, configPath.c_str()), 0,
        3600);

    // [Upscaling]: super-resolution configuration (fixed for the run).
    g_UpscalingEnabled =
        GetPrivateProfileIntA("Upscaling", "upscaling", g_UpscalingEnabled ? 1 : 0, configPath.c_str()) != 0;
    char textValue[64] = {};
    GetPrivateProfileStringA("Upscaling", "quality", testapp::fg::UpscaleQualityName(g_UpscaleQuality), textValue,
                             sizeof(textValue), configPath.c_str());
    if (!testapp::fg::ParseUpscaleQuality(textValue, &g_UpscaleQuality)) {
        testapp::Log("[FG-DIAG] WARN unknown [Upscaling] quality '%s'; keeping %s\n", textValue,
                     testapp::fg::UpscaleQualityName(g_UpscaleQuality));
    }
    g_UpscaleScalePercent =
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        ClampInt(GetPrivateProfileIntA("Upscaling", "scale", g_UpscaleScalePercent, configPath.c_str()), 0, 100);
    GetPrivateProfileStringA("Upscaling", "dlss_preset", "default", textValue, sizeof(textValue), configPath.c_str());
    g_DlssPresetConfig = (textValue[0] && strcmp(textValue, "default") != 0) ? textValue[0] : 0;
    if (g_DlssPresetConfig && (g_DlssPresetConfig < 'j' || g_DlssPresetConfig > 'm')) {
        testapp::Log("[FG-DIAG] WARN unknown [Upscaling] dlss_preset '%s'; using SL default\n", textValue);
        g_DlssPresetConfig = 0;
    }
    g_DlssHdrInput =
        GetPrivateProfileIntA("Upscaling", "dlss_hdr", g_DlssHdrInput ? 1 : 0, configPath.c_str()) != 0;
    g_FsrUpscaleVersionConfig =
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        GetPrivateProfileIntA("Upscaling", "fsr_version", g_FsrUpscaleVersionConfig, configPath.c_str());
    if (g_FsrUpscaleVersionConfig != 0 && g_FsrUpscaleVersionConfig != 3 && g_FsrUpscaleVersionConfig != 4) {
        testapp::Log("[FG-DIAG] WARN [Upscaling] fsr_version must be 0(auto)/3/4; using auto\n");
        g_FsrUpscaleVersionConfig = 0;
    }
    g_FsrSharpeningEnabled = GetPrivateProfileIntA("Upscaling", "fsr_sharpening", g_FsrSharpeningEnabled ? 1 : 0,
                                                   configPath.c_str()) != 0;
    g_FsrSharpnessPercent = ClampInt(
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        GetPrivateProfileIntA("Upscaling", "fsr_sharpness_percent", g_FsrSharpnessPercent, configPath.c_str()), 0,
        100);
}

static void NormalizeAutoSequenceTimings() {
    testapp::fg::NormalizeAutoSequenceTimings(&g_SwitchConfig);
}

static bool TryParseIntOption(const char* arg, const char* prefix, int* valueOut) {
    const size_t prefixLength = strlen(prefix);
    if (strncmp(arg, prefix, prefixLength) != 0 || arg[prefixLength] != '=') {
        return false;
    }
    *valueOut = testapp::ParseIntOrZero(arg + prefixLength + 1);
    return true;
}

static void ParseCommandLine(int argc, char* argv[]) {
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        int value = 0;
        if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            g_AutoExitSeconds = ClampInt(testapp::ParseIntOrZero(argv[++i]), 0, 3600);
            continue;
        }
        if (TryParseIntOption(argv[i], "--duration", &value)) {
            g_AutoExitSeconds = ClampInt(value, 0, 3600);
            continue;
        }
        if (strcmp(argv[i], "--auto-fsr-start") == 0 && i + 1 < argc) {
            g_AutoFsrStartSeconds = ClampInt(testapp::ParseIntOrZero(argv[++i]), 0, 3600);
            continue;
        }
        if (TryParseIntOption(argv[i], "--auto-fsr-start", &value)) {
            g_AutoFsrStartSeconds = ClampInt(value, 0, 3600);
            continue;
        }
        if (strcmp(argv[i], "--auto-dlss-start") == 0 && i + 1 < argc) {
            g_AutoDlssStartSeconds = ClampInt(testapp::ParseIntOrZero(argv[++i]), 0, 3600);
            continue;
        }
        if (TryParseIntOption(argv[i], "--auto-dlss-start", &value)) {
            g_AutoDlssStartSeconds = ClampInt(value, 0, 3600);
            continue;
        }
        if (strcmp(argv[i], "--auto-return-fsr") == 0 && i + 1 < argc) {
            g_AutoReturnFsrSeconds = ClampInt(testapp::ParseIntOrZero(argv[++i]), 0, 3600);
            continue;
        }
        if (TryParseIntOption(argv[i], "--auto-return-fsr", &value)) {
            g_AutoReturnFsrSeconds = ClampInt(value, 0, 3600);
            continue;
        }
        if (strcmp(argv[i], "--startup-recreates") == 0 && i + 1 < argc) {
            g_StartupNativeSwapchainRecreateCount = ClampInt(testapp::ParseIntOrZero(argv[++i]), 0, 8);
            continue;
        }
        if (TryParseIntOption(argv[i], "--startup-recreates", &value)) {
            g_StartupNativeSwapchainRecreateCount = ClampInt(value, 0, 8);
            continue;
        }
        if (strcmp(argv[i], "--bootstrap-native-swaps") == 0 && i + 1 < argc) {
            g_BootstrapNativeSwapchainStressCount = ClampInt(testapp::ParseIntOrZero(argv[++i]), 0, 8);
            continue;
        }
        if (TryParseIntOption(argv[i], "--bootstrap-native-swaps", &value)) {
            g_BootstrapNativeSwapchainStressCount = ClampInt(value, 0, 8);
            continue;
        }
        if (strcmp(argv[i], "--fsr-suspend-interval") == 0 && i + 1 < argc) {
            g_FsrSuspendResumeIntervalSeconds = ClampInt(testapp::ParseIntOrZero(argv[++i]), 1, 60);
            continue;
        }
        if (TryParseIntOption(argv[i], "--fsr-suspend-interval", &value)) {
            g_FsrSuspendResumeIntervalSeconds = ClampInt(value, 1, 60);
            continue;
        }
        if (strcmp(argv[i], "--dlss-suspend-interval") == 0 && i + 1 < argc) {
            g_DlssSuspendResumeIntervalSeconds = ClampInt(testapp::ParseIntOrZero(argv[++i]), 1, 60);
            continue;
        }
        if (TryParseIntOption(argv[i], "--dlss-suspend-interval", &value)) {
            g_DlssSuspendResumeIntervalSeconds = ClampInt(value, 1, 60);
            continue;
        }
        if (strcmp(argv[i], "--dlss-suspend-stress") == 0) {
            g_DlssSuspendResumeStress = true;
            continue;
        }
        if (strcmp(argv[i], "--fsr-suspend-stress") == 0) {
            g_FsrSuspendResumeStress = true;
            continue;
        }
        if (strcmp(argv[i], "--no-fsr-suspend-stress") == 0) {
            g_FsrSuspendResumeStress = false;
            continue;
        }
        if (strcmp(argv[i], "--no-dlss-suspend-stress") == 0) {
            g_DlssSuspendResumeStress = false;
            continue;
        }
        if (strcmp(argv[i], "--dlss-off-stress") == 0) {
            g_DlssOffAfterActiveStress = true;
            continue;
        }
        if (strcmp(argv[i], "--no-dlss-off-stress") == 0) {
            g_DlssOffAfterActiveStress = false;
            continue;
        }
        if (strcmp(argv[i], "--dred") == 0) {
            g_EnableDred = true;
            continue;
        }
        if (strcmp(argv[i], "--vk-debug") == 0) {
            // Vulkan compatibility spelling. Harmless on DX12 and keeps one shared command-line policy.
            g_EnableDred = true;
            continue;
        }
        if (strcmp(argv[i], "--fullscreen") == 0) {
            g_Fullscreen = 1;
            continue;
        }
        if (strcmp(argv[i], "--windowed") == 0) {
            g_Fullscreen = 0;
            continue;
        }
        if (strcmp(argv[i], "--upscaling") == 0) {
            g_UpscalingEnabled = true;
            continue;
        }
        if (strcmp(argv[i], "--no-upscaling") == 0) {
            g_UpscalingEnabled = false;
            continue;
        }
        if (strcmp(argv[i], "--upscale-quality") == 0 && i + 1 < argc) {
            if (!testapp::fg::ParseUpscaleQuality(argv[++i], &g_UpscaleQuality)) {
                testapp::Log("[FG-DIAG] WARN unknown --upscale-quality '%s'; keeping %s\n", argv[i],
                             testapp::fg::UpscaleQualityName(g_UpscaleQuality));
            }
            continue;
        }
        if (strcmp(argv[i], "--upscale-scale") == 0 && i + 1 < argc) {
            g_UpscaleScalePercent = ClampInt(testapp::ParseIntOrZero(argv[++i]), 0, 100);
            continue;
        }
        if (TryParseIntOption(argv[i], "--upscale-scale", &value)) {
            g_UpscaleScalePercent = ClampInt(value, 0, 100);
            continue;
        }
        if (strcmp(argv[i], "--dlss-preset") == 0 && i + 1 < argc) {
            const char* preset = argv[++i];
            g_DlssPresetConfig = (preset[0] && strcmp(preset, "default") != 0) ? preset[0] : 0;
            if (g_DlssPresetConfig && (g_DlssPresetConfig < 'j' || g_DlssPresetConfig > 'm')) {
                testapp::Log("[FG-DIAG] WARN unknown --dlss-preset '%s'; using SL default\n", preset);
                g_DlssPresetConfig = 0;
            }
            continue;
        }
        if (strcmp(argv[i], "--fsr-upscale-version") == 0 && i + 1 < argc) {
            const int version = testapp::ParseIntOrZero(argv[++i]);
            g_FsrUpscaleVersionConfig = (version == 3 || version == 4) ? version : 0;
            continue;
        }
        if (strcmp(argv[i], "--dlss-hdr") == 0 && i + 1 < argc) {
            g_DlssHdrInput = testapp::ParseIntOrZero(argv[++i]) != 0;
            continue;
        }
        if (strcmp(argv[i], "--fsr-present-callback-interval") == 0 && i + 1 < argc) {
            g_FsrPresentCallbackToggleIntervalSeconds = ClampInt(testapp::ParseIntOrZero(argv[++i]), 1, 120);
            continue;
        }
        if (TryParseIntOption(argv[i], "--fsr-present-callback-interval", &value)) {
            g_FsrPresentCallbackToggleIntervalSeconds = ClampInt(value, 1, 120);
            continue;
        }
        if (strcmp(argv[i], "--fsr-present-callback-stress") == 0) {
            g_FsrPresentCallbackStress = true;
            continue;
        }
        if (strcmp(argv[i], "--no-fsr-present-callback-stress") == 0) {
            g_FsrPresentCallbackStress = false;
            continue;
        }
        if (strcmp(argv[i], "--fsr-degenerate-ui") == 0) {
            g_FsrDegenerateUiResource = true;
            continue;
        }
        if (strcmp(argv[i], "--no-fsr-degenerate-ui") == 0) {
            g_FsrDegenerateUiResource = false;
            continue;
        }
        if (strcmp(argv[i], "--startup-preload-fg") == 0) {
            g_StreamlinePreloadInitialOff = true;
            g_FsrKeepRuntimeLoadedInitialOff = true;
            g_FsrStartupDisabledContextStress = true;
            continue;
        }
        if (strcmp(argv[i], "--preload-streamline") == 0) {
            g_StreamlinePreloadInitialOff = true;
            continue;
        }
        if (strcmp(argv[i], "--no-preload-streamline") == 0) {
            g_StreamlinePreloadInitialOff = false;
            continue;
        }
        if (strcmp(argv[i], "--preload-fsr") == 0) {
            g_FsrKeepRuntimeLoadedInitialOff = true;
            continue;
        }
        if (strcmp(argv[i], "--no-preload-fsr") == 0) {
            g_FsrKeepRuntimeLoadedInitialOff = false;
            g_FsrStartupDisabledContextStress = false;
            continue;
        }
        if (strcmp(argv[i], "--startup-disabled-fsr-context") == 0) {
            g_FsrKeepRuntimeLoadedInitialOff = true;
            g_FsrStartupDisabledContextStress = true;
            continue;
        }
        if (strcmp(argv[i], "--no-startup-disabled-fsr-context") == 0) {
            g_FsrStartupDisabledContextStress = false;
            continue;
        }
        if (strcmp(argv[i], "--async-runtime-preload") == 0) {
            g_AsyncRuntimePreload = true;
            continue;
        }
        if (strcmp(argv[i], "--no-async-runtime-preload") == 0) {
            g_AsyncRuntimePreload = false;
            continue;
        }

        switch (positional++) {
            case 0:
                g_WindowWidth = testapp::ParseIntOrZero(argv[i]);
                break;
            case 1:
                g_WindowHeight = testapp::ParseIntOrZero(argv[i]);
                break;
            case 2:
                g_GpuLoadPasses = testapp::ParseIntOrZero(argv[i]);
                break;
            default:
                break;
        }
    }
    NormalizeAutoSequenceTimings();
}

static std::string TestAppConfigPath() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string configPath = path;
    const size_t slash = configPath.find_last_of("\\/");
    return slash == std::string::npos ? std::string("testappconfig.ini")
                                      : configPath.substr(0, slash + 1) + "testappconfig.ini";
}

static void ParseVulkanOptions(int argc, char* argv[]) {
    const std::string configPath = TestAppConfigPath();
    g_App.asyncPresentRequested =
        GetPrivateProfileIntA("Vulkan", "async_present", g_App.asyncPresentRequested ? 1 : 0, configPath.c_str()) != 0;
    g_App.config.apiDebug =
        GetPrivateProfileIntA("Vulkan", "debug", g_App.config.apiDebug ? 1 : 0, configPath.c_str()) != 0;

    // Feed the shared DX12/Vulkan parser only its own arguments so Vulkan-only switches cannot be
    // mistaken for the legacy positional width/height/load arguments.
    std::vector<char*> sharedArguments;
    sharedArguments.reserve(static_cast<size_t>(argc));
    sharedArguments.push_back(argv[0]);
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--vk-async-present") == 0) {
            g_App.asyncPresentRequested = true;
        } else if (std::strcmp(argv[index], "--no-vk-async-present") == 0) {
            g_App.asyncPresentRequested = false;
        } else if (std::strcmp(argv[index], "--vk-vsync") == 0) {
            g_App.config.vsync = 1;
        } else if (std::strcmp(argv[index], "--vk-no-vsync") == 0) {
            g_App.config.vsync = 0;
        } else {
            sharedArguments.push_back(argv[index]);
        }
    }
    ParseCommandLine(static_cast<int>(sharedArguments.size()), sharedArguments.data());
}

namespace {
constexpr wchar_t kWindowClass[] = L"CaptureProjectVulkanFgSwitchTest";
}

namespace {
bool g_ManualMode = false;
}

namespace {
bool g_AutoFsrRequested = false;
}

namespace {
bool g_AutoDlssRequested = false;
}

namespace {
bool g_AutoReturnFsrRequested = false;
}

namespace {
bool g_DlssOffStressRequested = false;
}

namespace {
testapp::vkfg::FgMode g_LastStressMode = testapp::vkfg::FgMode::Off;
}

namespace {
std::chrono::steady_clock::time_point g_LastStressToggle = std::chrono::steady_clock::now();
}

namespace {
WINDOWPLACEMENT g_WindowedPlacement = {sizeof(WINDOWPLACEMENT)};
}

namespace {
void UpdateWindowTitle() {
    if (!g_App.hwnd) {
        return;
    }
    wchar_t title[320] = {};
    std::swprintf(title, std::size(title), L"Vulkan FG Switch Test - %ux%u (render %ux%u) - %hs%hs - %hs - Reflex %hs",
                  g_App.swapchain.extent.width, g_App.swapchain.extent.height, g_App.renderer.renderWidth,
                  g_App.renderer.renderHeight, testapp::vkfg::ModeName(g_App.transition.currentMode),
                  g_App.transition.suspended ? " (suspended)" : "", testapp::vkfg::OwnerName(g_App.swapchain.owner),
                  g_App.sl.reflexActive ? "low-latency" : "off");
    SetWindowTextW(g_App.hwnd, title);
}
}

namespace {
void RequestManualMode(testapp::vkfg::FgMode mode, const char* reason) {
    g_ManualMode = true;
    testapp::vkfg::RequestMode(mode, reason);
    // A repeated 2/3 changes only suspension state, so the mode-change watcher cannot refresh the
    // title for us. Keep the title and rendered status text synchronized for every manual request.
    UpdateWindowTitle();
}
}

namespace {
void ApplyFullscreenToggle() {
    if (!g_App.hwnd || !g_App.fullscreenPending) {
        return;
    }
    g_App.fullscreenPending = false;
    g_App.config.fullscreen = g_App.config.fullscreen ? 0 : 1;
    if (g_App.config.fullscreen) {
        g_WindowedPlacement.length = sizeof(g_WindowedPlacement);
        GetWindowPlacement(g_App.hwnd, &g_WindowedPlacement);
        SetWindowLongPtrW(g_App.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        const RECT monitor = testapp::GetPrimaryMonitorRect();
        SetWindowPos(g_App.hwnd, HWND_TOPMOST, monitor.left, monitor.top, monitor.right - monitor.left,
                     monitor.bottom - monitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongPtrW(g_App.hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(g_App.hwnd, &g_WindowedPlacement);
        SetWindowPos(g_App.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    g_App.swapchainRecreatePending = true;
    testapp::Log("[FG-TRANSITION] fullscreen changed enabled=%d; transactional recreation requested\n",
                 g_App.config.fullscreen);
    testapp::LogFlush();
}
}

namespace {
LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CLOSE:
            g_App.running = false;
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            g_App.running = false;
            PostQuitMessage(0);
            return 0;
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                g_App.pendingWidth = std::max<uint32_t>(LOWORD(lParam), 1u);
                g_App.pendingHeight = std::max<uint32_t>(HIWORD(lParam), 1u);
                g_App.resizePending = true;
                if (g_App.swapchain.handle != VK_NULL_HANDLE) {
                    g_App.swapchainRecreatePending = true;
                }
            }
            return 0;
        case WM_SYSKEYDOWN:
            if (wParam == VK_RETURN && (lParam & (1u << 29)) != 0) {
                g_App.fullscreenPending = true;
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                g_App.running = false;
                DestroyWindow(window);
            } else if (wParam == VK_F11) {
                g_App.fullscreenPending = true;
            } else if (wParam == '1') {
                RequestManualMode(testapp::vkfg::FgMode::Off, "key 1");
            } else if (wParam == '2') {
                RequestManualMode(testapp::vkfg::FgMode::Dlss, "key 2");
            } else if (wParam == '3') {
                RequestManualMode(testapp::vkfg::FgMode::Fsr, "key 3");
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

namespace {
bool CreateApplicationWindow() {
    g_App.instanceHandle = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = g_App.instanceHandle;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        testapp::Log("[FG-DIAG] RegisterClassExW failed error=%lu\n", GetLastError());
        return false;
    }
    RECT monitor = testapp::GetPrimaryMonitorRect();
    if (g_App.config.fullscreen) {
        g_App.config.windowWidth = monitor.right - monitor.left;
        g_App.config.windowHeight = monitor.bottom - monitor.top;
    }
    const DWORD style = g_App.config.fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    RECT windowRect =
        testapp::AdjustWindowRectForClientSize(style, 0, g_App.config.windowWidth, g_App.config.windowHeight);
    g_App.hwnd = CreateWindowExW(
        0, kWindowClass, L"Vulkan FG Switch Test", style, g_App.config.fullscreen ? monitor.left : CW_USEDEFAULT,
        g_App.config.fullscreen ? monitor.top : CW_USEDEFAULT, windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top, nullptr, nullptr, g_App.instanceHandle, nullptr);
    if (!g_App.hwnd) {
        testapp::Log("[FG-DIAG] CreateWindowExW failed error=%lu\n", GetLastError());
        return false;
    }
    ShowWindow(g_App.hwnd, SW_SHOW);
    UpdateWindow(g_App.hwnd);
    return true;
}
}

namespace {
void UpdateAutomaticSequenceAndStress() {
    const auto now = std::chrono::steady_clock::now();
    const float elapsed = std::chrono::duration<float>(now - g_App.startTime).count();
    if (!g_ManualMode) {
        if (!g_AutoFsrRequested && elapsed >= static_cast<float>(g_App.config.autoFsrStartSeconds)) {
            g_AutoFsrRequested = true;
            testapp::vkfg::RequestMode(testapp::vkfg::FgMode::Fsr, "automatic FSR stage");
        }
        if (!g_AutoDlssRequested && elapsed >= static_cast<float>(g_App.config.autoDlssStartSeconds)) {
            g_AutoDlssRequested = true;
            testapp::vkfg::RequestMode(testapp::vkfg::FgMode::Dlss, "automatic DLSS stage after FSR residency");
        }
        if (!g_AutoReturnFsrRequested && elapsed >= static_cast<float>(g_App.config.autoReturnFsrSeconds)) {
            g_AutoReturnFsrRequested = true;
            testapp::vkfg::RequestMode(testapp::vkfg::FgMode::Fsr, "automatic return-to-FSR stage");
        }
    }

    if (g_App.transition.currentMode != g_LastStressMode) {
        g_LastStressMode = g_App.transition.currentMode;
        g_LastStressToggle = now;
        UpdateWindowTitle();
    }
    if (g_ManualMode || g_App.transition.stage != testapp::vkfg::TransitionStage::Idle ||
        g_App.transition.currentMode == testapp::vkfg::FgMode::Off) {
        return;
    }
    bool stressEnabled = false;
    int interval = 1;
    if (g_App.transition.currentMode == testapp::vkfg::FgMode::Fsr) {
        stressEnabled = g_App.config.fsrSuspendResumeStress;
        interval = g_App.config.fsrSuspendResumeIntervalSeconds;
    } else if (g_App.transition.currentMode == testapp::vkfg::FgMode::Dlss) {
        stressEnabled = g_App.config.dlssSuspendResumeStress;
        interval = g_App.config.dlssSuspendResumeIntervalSeconds;
    }
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    if (stressEnabled && std::chrono::duration<float>(now - g_LastStressToggle).count() >= interval) {
        g_LastStressToggle = now;
        testapp::vkfg::RequestMode(g_App.transition.currentMode, "automatic suspend/resume stress");
        UpdateWindowTitle();
    }
    if (g_App.config.dlssOffAfterActiveStress && !g_DlssOffStressRequested &&
        g_App.transition.currentMode == testapp::vkfg::FgMode::Dlss &&
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        std::chrono::duration<float>(now - g_LastStressToggle).count() >= interval) {
        g_DlssOffStressRequested = true;
        testapp::vkfg::RequestMode(testapp::vkfg::FgMode::Off, "DLSS off-after-active stress");
    }
}
}

namespace {
bool ProcessPendingSwapchainRecreation() {
    if (!g_App.swapchainRecreatePending || g_App.vk.deviceLost) {
        return true;
    }
    RECT client{};
    GetClientRect(g_App.hwnd, &client);
    if (client.right <= client.left || client.bottom <= client.top || IsIconic(g_App.hwnd)) {
        return true;
    }
    using testapp::vkfg::TransitionStage;
    if (g_App.transition.stage == TransitionStage::OldPassthroughPending ||
        g_App.transition.stage == TransitionStage::PreparingReplacement) {
        const testapp::vkfg::FgMode queuedTarget = g_App.transition.targetMode;
        testapp::vkfg::RequestMode(g_App.transition.currentMode, "cancel owner switch for surface recreation");
        g_App.requestedMode = queuedTarget;
    }
    bool recreated = false;
    if (g_App.transition.stage == TransitionStage::ReplacementPresentPending) {
        recreated = testapp::vkfg::DrainSwapchainBoundWork("recreate replacement surface") &&
                    testapp::vkfg::CreateOrReplaceSwapchain(g_App.swapchain.owner, "recreate replacement surface");
    } else if (g_App.transition.stage == TransitionStage::Idle) {
        recreated = testapp::vkfg::RecreateCurrentSwapchain(g_App.resizePending ? "window resize/out-of-date"
                                                                                : "fullscreen/suboptimal");
    } else {
        return true;
    }
    if (recreated) {
        g_App.swapchainRecreatePending = false;
        g_App.resizePending = false;
        UpdateWindowTitle();
    } else if (!g_App.vk.deviceLost) {
        testapp::Log("[FG-DIAG] Swapchain recreation failed; retaining retry request owner=%s stage=%s\n",
                     testapp::vkfg::OwnerName(g_App.swapchain.owner),
                     testapp::vkfg::TransitionStageName(g_App.transition.stage));
        testapp::LogFlush();
    }
    return recreated || !g_App.vk.deviceLost;
}
}

namespace {
void Cleanup() {
    if (g_App.vk.device != VK_NULL_HANDLE && !g_App.vk.deviceLost) {
        testapp::vkfg::SetModeFeatureState(g_App.transition.currentMode, false, "final cleanup");
        testapp::vkfg::DrainSwapchainBoundWork("final cleanup");
    }
    const bool fidelityFxOwned = g_App.swapchain.owner == testapp::vkfg::SwapchainOwner::FidelityFX;
    if (g_App.vk.device != VK_NULL_HANDLE) {
        testapp::vkfg::DestroySwapchainState(!fidelityFxOwned);
    }
    if (g_App.ffx.runtimeLoaded || g_App.ffx.preloadStarted.load()) {
        testapp::vkfg::DestroyFidelityFxContexts(true, "final cleanup");
        if (fidelityFxOwned) {
            g_App.swapchain.handle = VK_NULL_HANDLE;
        }
    }
    if (g_App.vk.device != VK_NULL_HANDLE) {
        testapp::vkfg::ShutdownRenderer();
    }
    if (g_App.swapchain.owner == testapp::vkfg::SwapchainOwner::Streamline) {
        testapp::vkfg::RetireStreamlinePresentation(testapp::vkfg::SwapchainOwner::Native, "final cleanup");
    }
    // Surface creation/destruction is a mandatory Streamline Vulkan manual hook. Retire it while
    // the interposer is callable, then shut down the core while the device and instance remain live.
    testapp::vkfg::ReleaseVulkanSurfaceBeforeStreamlineShutdown();
    // Streamline's core must shut down while its Vulkan device/instance are still live.
    testapp::vkfg::ShutdownStreamline();
    testapp::vkfg::ShutdownVulkanDevice();
    if (g_App.hwnd && IsWindow(g_App.hwnd)) {
        DestroyWindow(g_App.hwnd);
    }
    UnregisterClassW(kWindowClass, g_App.instanceHandle);
}
}

    // NOLINTNEXTLINE(bugprone-exception-escape) - standalone test harness: an unexpected exception terminating the process is acceptable and yields a nonzero exit
int main(int argc, char* argv[]) {
    testapp::EnableGameDpiAwareness();
    testapp::ApplyGameScheduling();
    testapp::OpenLogFile();
    LoadConfig();
    ParseVulkanOptions(argc, argv);
    const testapp::vkfg::VulkanFsrVersionResolution fsrResolution =
        testapp::vkfg::ResolveVulkanFsrVersion(g_App.config.fsrUpscaleVersion);

    testapp::Log("Vulkan FG Switch Test App\n");
    testapp::Log("=========================\n");
    testapp::Log("Process=%lu resolution=%dx%d fullscreen=%d gpuLoad=%d vsync=%d framesInFlight=%u\n",
                 GetCurrentProcessId(), g_App.config.windowWidth, g_App.config.windowHeight, g_App.config.fullscreen,
                 g_App.config.gpuLoadPasses, g_App.config.vsync, testapp::vkfg::kFramesInFlight);
    testapp::Log(
        "Presentation: requestedSeparateQueue=%d presentModePolicy=%s colorPath="
        "SDR-8bit/sRGB-nonlinear (dlss_hdr configures SR input semantics, not HDR10 output)\n",
        g_App.asyncPresentRequested ? 1 : 0, g_App.config.vsync ? "FIFO" : "IMMEDIATE->MAILBOX->FIFO_RELAXED->FIFO");
    testapp::Log(
        "SDK policy: Streamline=2.11.1 FidelityFX Vulkan=1.1.4 FSR=3.1.4 "
        "requestedFsrVersion=%d resolved=%d mlFallback=%d\n",
        fsrResolution.requested, fsrResolution.resolved, fsrResolution.mlFallback ? 1 : 0);
    if (fsrResolution.mlFallback) {
        testapp::Log(
            "[FG-DIAG] WARN fsr_version=4 requests ML SR/FG, which is unsupported by Vulkan; "
            "falling back to non-ML FSR 3.1.4 from FidelityFX SDK 1.1.4\n");
    } else if (fsrResolution.invalidRequest) {
        testapp::Log("[FG-DIAG] WARN invalid Vulkan fsr_version=%d; using FSR 3.1.4\n", fsrResolution.requested);
    }
    testapp::Log("Upscaling enabled=%d quality=%s scale=%d%% DLSS preset=%c hdr=%d FSR sharpening=%d/%d%%\n",
                 g_App.config.upscalingEnabled ? 1 : 0, testapp::fg::UpscaleQualityName(g_App.config.upscaleQuality),
                 g_App.config.upscaleScalePercent, g_App.config.dlssPreset ? g_App.config.dlssPreset : '-',
                 g_App.config.dlssHdrInput ? 1 : 0, g_App.config.fsrSharpeningEnabled ? 1 : 0,
                 g_App.config.fsrSharpnessPercent);
    testapp::Log("Automatic sequence: OFF -> FSR at %ds -> DLSS at %ds -> FSR at %ds; exit=%ds\n",
                 g_App.config.autoFsrStartSeconds, g_App.config.autoDlssStartSeconds, g_App.config.autoReturnFsrSeconds,
                 g_App.config.autoExitSeconds);
    testapp::Log(
        "Stress: FSR suspend=%d/%ds DLSS suspend=%d/%ds callbackToggle=%d/%ds "
        "memoryBudget=%d/%d startupRecreates=%d bootstrapSwaps=%d\n",
        g_App.config.fsrSuspendResumeStress ? 1 : 0, g_App.config.fsrSuspendResumeIntervalSeconds,
        g_App.config.dlssSuspendResumeStress ? 1 : 0, g_App.config.dlssSuspendResumeIntervalSeconds,
        g_App.config.fsrPresentCallbackStress ? 1 : 0, g_App.config.fsrPresentCallbackToggleIntervalSeconds,
        g_App.config.videoMemoryQueryStress ? 1 : 0, g_App.config.videoMemoryQueryCountPerFrame,
        g_App.config.startupNativeSwapchainRecreateCount, g_App.config.bootstrapNativeSwapchainStressCount);
    testapp::Log("Keys: 1=OFF 2=DLSS FG 3=FSR FG (repeat 2/3=suspend/resume) F11/Alt+Enter=fullscreen ESC=exit\n");
    testapp::Log(
        "Debug: --vk-debug/--dred enable validation/debug-utils/device-fault; "
        "--vk-async-present selects a distinct same-family graphics+compute+present queue; "
        "--vk-vsync/--vk-no-vsync override the INI for controlled WSI tests\n");
    testapp::LogFlush();

    bool initialized = CreateApplicationWindow();
    if (initialized) {
        // Required ordering: slInit and feature requirements precede every Vulkan object.
        testapp::vkfg::InitializeStreamlineBeforeVulkan();
        initialized = testapp::vkfg::InitializeVulkanDevice();
    }
    if (initialized) {
        initialized =
            testapp::vkfg::CreateOrReplaceSwapchain(testapp::vkfg::SwapchainOwner::Native, "initial native owner");
    }
    if (initialized) {
        initialized = testapp::vkfg::InitializeRenderer();
    }
    const int startupRecreates =
        g_App.config.startupNativeSwapchainRecreateCount + g_App.config.bootstrapNativeSwapchainStressCount;
    for (int index = 0; initialized && index < startupRecreates; ++index) {
        initialized = testapp::vkfg::DrainSwapchainBoundWork("startup native recreate stress") &&
                      testapp::vkfg::CreateOrReplaceSwapchain(testapp::vkfg::SwapchainOwner::Native,
                                                              "startup native recreate stress");
    }
    if (initialized && !g_App.config.streamlinePreloadInitialOff && g_App.sl.featuresLoaded) {
        testapp::vkfg::SetStreamlineFeaturesLoaded(false, "initial OFF preload disabled");
    }
    if (initialized && (g_App.config.fsrKeepRuntimeLoadedInitialOff || g_App.config.fsrStartupDisabledContextStress)) {
        initialized = testapp::vkfg::LoadFidelityFxRuntime("initial OFF preload");
        if (initialized && g_App.config.fsrStartupDisabledContextStress) {
            initialized = testapp::vkfg::PrepareFidelityFxMode();
            if (initialized) {
                testapp::vkfg::SetFsrFrameGeneration(false, "startup disabled context", true);
            }
        }
    }
    if (!initialized) {
        testapp::Log("[FG-DIAG] Initialization failed; exiting cleanly\n");
        Cleanup();
        testapp::CloseLogFile();
        return 1;
    }

    g_App.transition.owner = testapp::vkfg::SwapchainOwner::Native;
    g_App.transition.currentMode = testapp::vkfg::FgMode::Off;
    g_App.requestedMode = testapp::vkfg::FgMode::Off;
    g_App.startTime = std::chrono::steady_clock::now();
    g_App.previousFrameTime = g_App.startTime;
    g_App.fpsSampleTime = g_App.startTime;
    g_App.fpsSampleFrame = 0;
    g_App.heartbeatTime = g_App.startTime;
    g_LastStressToggle = g_App.startTime;
    UpdateWindowTitle();
    testapp::Log(
        "[FG-DIAG] Startup complete owner=native route=loader; automatic clock begins now "
        "support(dlssSR=%d dlssFG=%d reflex=%d ffxQueues=%d)\n",
        g_App.sl.dlssSrSupported ? 1 : 0, g_App.sl.dlssFgSupported ? 1 : 0, g_App.sl.reflexSupported ? 1 : 0,
        g_App.vk.queuePlan.fidelityFxAvailable ? 1 : 0);
    testapp::LogFlush();

    MSG message{};
    while (g_App.running && !g_App.vk.deviceLost) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                g_App.running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!g_App.running) {
            break;
        }
        if (IsIconic(g_App.hwnd)) {
            WaitMessage();
            continue;
        }
        ApplyFullscreenToggle();
        UpdateAutomaticSequenceAndStress();
        testapp::vkfg::DriveTransitionBeforeFrame();
        ProcessPendingSwapchainRecreation();
        if (!testapp::vkfg::RenderFrame()) {
            if (!g_App.vk.deviceLost) {
                testapp::Log("[FG-DIAG] RenderFrame failed without device loss; stopping\n");
            }
            break;
        }
        if (g_App.config.autoExitSeconds > 0 &&
            std::chrono::duration<float>(std::chrono::steady_clock::now() - g_App.startTime).count() >=
                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                g_App.config.autoExitSeconds) {
            testapp::Log("[FG-DIAG] Auto-exit deadline reached\n");
            break;
        }
    }

    Cleanup();
    testapp::Log(
        "[FG-SUMMARY] frames=%llu presented=%llu generated=%llu transitions=%llu failures=%llu "
        "validationErrors=%llu pacingSpikes=%llu deviceLost=%d reflexSleep(calls=%llu,failures=%llu) "
        "callbacks(ffxPresent=%llu,ffxFG=%llu)\n",
        static_cast<unsigned long long>(g_App.frameId), static_cast<unsigned long long>(g_App.presentedFrames),
        static_cast<unsigned long long>(g_App.generatedFrames), static_cast<unsigned long long>(g_App.transition.epoch),
        static_cast<unsigned long long>(g_App.transitionFailures),
        static_cast<unsigned long long>(g_App.validationErrors), static_cast<unsigned long long>(g_App.pacingSpikes),
        g_App.vk.deviceLost ? 1 : 0, static_cast<unsigned long long>(g_App.sl.reflexSleepCalls),
        static_cast<unsigned long long>(g_App.sl.reflexSleepFailures),
        static_cast<unsigned long long>(g_App.ffx.presentCallbackCount.load()),
        static_cast<unsigned long long>(g_App.ffx.frameGenerationCallbackCount.load()));
    testapp::LogFlush();
    testapp::CloseLogFile();
    return g_App.vk.deviceLost ? 2 : 0;
}
