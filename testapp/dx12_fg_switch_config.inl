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
}
