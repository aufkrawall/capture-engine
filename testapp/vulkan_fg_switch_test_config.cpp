#include "vulkan_fg_switch_test_internal.h"

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

void LoadConfig() {
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

void NormalizeAutoSequenceTimings() {
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

void ParseCommandLine(int argc, char* argv[]) {
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

std::string TestAppConfigPath() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string configPath = path;
    const size_t slash = configPath.find_last_of("\\/");
    return slash == std::string::npos ? std::string("testappconfig.ini")
                                      : configPath.substr(0, slash + 1) + "testappconfig.ini";
}

void ParseVulkanOptions(int argc, char* argv[]) {
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
