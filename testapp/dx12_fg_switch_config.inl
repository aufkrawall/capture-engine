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
    g_WindowWidth = GetPrivateProfileIntA("Display", "width", g_WindowWidth, configPath.c_str());
    g_WindowHeight = GetPrivateProfileIntA("Display", "height", g_WindowHeight, configPath.c_str());
    g_GpuLoadPasses = GetPrivateProfileIntA("Performance", "gpu_load", g_GpuLoadPasses, configPath.c_str());
    g_VSync = GetPrivateProfileIntA("Rendering", "vsync", g_VSync, configPath.c_str());
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
        GetPrivateProfileIntA("Stress", "fsr_suspend_resume_interval_seconds", g_FsrSuspendResumeIntervalSeconds,
                              configPath.c_str()),
        1, 60);
    g_DlssSuspendResumeStress = GetPrivateProfileIntA("Stress", "dlss_suspend_resume",
                                                      g_DlssSuspendResumeStress ? 1 : 0, configPath.c_str()) != 0;
    g_DlssSuspendResumeIntervalSeconds = ClampInt(
        GetPrivateProfileIntA("Stress", "dlss_suspend_resume_interval_seconds", g_DlssSuspendResumeIntervalSeconds,
                              configPath.c_str()),
        1, 60);
    g_DlssOffAfterActiveStress = GetPrivateProfileIntA("Stress", "dlss_off_after_active",
                                                       g_DlssOffAfterActiveStress ? 1 : 0, configPath.c_str()) != 0;
    g_FsrPresentCallbackStress =
        GetPrivateProfileIntA("Stress", "fsr_present_callback_toggle_stress",
                              g_FsrPresentCallbackStress ? 1 : 0, configPath.c_str()) != 0;
    g_FsrPresentCallbackToggleIntervalSeconds =
        ClampInt(GetPrivateProfileIntA("Stress", "fsr_present_callback_toggle_interval_seconds",
                                       g_FsrPresentCallbackToggleIntervalSeconds, configPath.c_str()),
                 1, 120);
    g_DxgiVideoMemoryQueryStress = GetPrivateProfileIntA("Stress", "dxgi_video_memory_query_stress",
                                                         g_DxgiVideoMemoryQueryStress ? 1 : 0, configPath.c_str()) != 0;
    g_DxgiVideoMemoryQueryCountPerFrame = ClampInt(
        GetPrivateProfileIntA("Stress", "dxgi_video_memory_query_count_per_frame",
                              g_DxgiVideoMemoryQueryCountPerFrame, configPath.c_str()),
        0, 512);
    g_BootstrapNativeSwapchainStressCount =
        ClampInt(GetPrivateProfileIntA("Stress", "bootstrap_native_swapchain_stress_count",
                                       g_BootstrapNativeSwapchainStressCount, configPath.c_str()),
                 0, 8);
    g_StartupNativeSwapchainRecreateCount =
        ClampInt(GetPrivateProfileIntA("Stress", "startup_native_swapchain_recreate_count",
                                       g_StartupNativeSwapchainRecreateCount, configPath.c_str()),
                 0, 8);
    g_AsyncRuntimePreload =
        GetPrivateProfileIntA("Stress", "async_runtime_preload", g_AsyncRuntimePreload ? 1 : 0,
                              configPath.c_str()) != 0;
    g_AutoExitSeconds = ClampInt(
        GetPrivateProfileIntA("Stress", "auto_exit_seconds", g_AutoExitSeconds, configPath.c_str()), 0, 3600);
    g_AutoFsrStartSeconds = ClampInt(
        GetPrivateProfileIntA("Stress", "auto_fsr_start_seconds", g_AutoFsrStartSeconds, configPath.c_str()), 0,
        3600);
    g_AutoDlssStartSeconds = ClampInt(
        GetPrivateProfileIntA("Stress", "auto_dlss_start_seconds", g_AutoDlssStartSeconds, configPath.c_str()), 0,
        3600);
    g_AutoReturnFsrSeconds = ClampInt(
        GetPrivateProfileIntA("Stress", "auto_return_fsr_seconds", g_AutoReturnFsrSeconds, configPath.c_str()), 0,
        3600);
}

static void NormalizeAutoSequenceTimings() {
    g_AutoFsrStartSeconds = ClampInt(g_AutoFsrStartSeconds, 0, 3598);
    g_AutoDlssStartSeconds = ClampInt(g_AutoDlssStartSeconds, g_AutoFsrStartSeconds + 1, 3599);
    g_AutoReturnFsrSeconds = ClampInt(g_AutoReturnFsrSeconds, g_AutoDlssStartSeconds + 1, 3600);
}

static bool TryParseIntOption(const char* arg, const char* prefix, int* valueOut) {
    const size_t prefixLength = strlen(prefix);
    if (strncmp(arg, prefix, prefixLength) != 0 || arg[prefixLength] != '=') {
        return false;
    }
    *valueOut = atoi(arg + prefixLength + 1);
    return true;
}

static void ParseCommandLine(int argc, char* argv[]) {
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        int value = 0;
        if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            g_AutoExitSeconds = ClampInt(atoi(argv[++i]), 0, 3600);
            continue;
        }
        if (TryParseIntOption(argv[i], "--duration", &value)) {
            g_AutoExitSeconds = ClampInt(value, 0, 3600);
            continue;
        }
        if (strcmp(argv[i], "--auto-fsr-start") == 0 && i + 1 < argc) {
            g_AutoFsrStartSeconds = ClampInt(atoi(argv[++i]), 0, 3600);
            continue;
        }
        if (TryParseIntOption(argv[i], "--auto-fsr-start", &value)) {
            g_AutoFsrStartSeconds = ClampInt(value, 0, 3600);
            continue;
        }
        if (strcmp(argv[i], "--auto-dlss-start") == 0 && i + 1 < argc) {
            g_AutoDlssStartSeconds = ClampInt(atoi(argv[++i]), 0, 3600);
            continue;
        }
        if (TryParseIntOption(argv[i], "--auto-dlss-start", &value)) {
            g_AutoDlssStartSeconds = ClampInt(value, 0, 3600);
            continue;
        }
        if (strcmp(argv[i], "--auto-return-fsr") == 0 && i + 1 < argc) {
            g_AutoReturnFsrSeconds = ClampInt(atoi(argv[++i]), 0, 3600);
            continue;
        }
        if (TryParseIntOption(argv[i], "--auto-return-fsr", &value)) {
            g_AutoReturnFsrSeconds = ClampInt(value, 0, 3600);
            continue;
        }
        if (strcmp(argv[i], "--startup-recreates") == 0 && i + 1 < argc) {
            g_StartupNativeSwapchainRecreateCount = ClampInt(atoi(argv[++i]), 0, 8);
            continue;
        }
        if (TryParseIntOption(argv[i], "--startup-recreates", &value)) {
            g_StartupNativeSwapchainRecreateCount = ClampInt(value, 0, 8);
            continue;
        }
        if (strcmp(argv[i], "--bootstrap-native-swaps") == 0 && i + 1 < argc) {
            g_BootstrapNativeSwapchainStressCount = ClampInt(atoi(argv[++i]), 0, 8);
            continue;
        }
        if (TryParseIntOption(argv[i], "--bootstrap-native-swaps", &value)) {
            g_BootstrapNativeSwapchainStressCount = ClampInt(value, 0, 8);
            continue;
        }
        if (strcmp(argv[i], "--fsr-suspend-interval") == 0 && i + 1 < argc) {
            g_FsrSuspendResumeIntervalSeconds = ClampInt(atoi(argv[++i]), 1, 60);
            continue;
        }
        if (TryParseIntOption(argv[i], "--fsr-suspend-interval", &value)) {
            g_FsrSuspendResumeIntervalSeconds = ClampInt(value, 1, 60);
            continue;
        }
        if (strcmp(argv[i], "--dlss-suspend-interval") == 0 && i + 1 < argc) {
            g_DlssSuspendResumeIntervalSeconds = ClampInt(atoi(argv[++i]), 1, 60);
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
        if (strcmp(argv[i], "--fsr-present-callback-interval") == 0 && i + 1 < argc) {
            g_FsrPresentCallbackToggleIntervalSeconds = ClampInt(atoi(argv[++i]), 1, 120);
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
                g_WindowWidth = atoi(argv[i]);
                break;
            case 1:
                g_WindowHeight = atoi(argv[i]);
                break;
            case 2:
                g_GpuLoadPasses = atoi(argv[i]);
                break;
            default:
                break;
        }
    }
    NormalizeAutoSequenceTimings();
}
