// Included by dx12_fg_switch_test.cpp; shares that file's static configuration state.

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
