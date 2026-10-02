#include "media_main_internal.h"

std::string GetLocalConfigPath() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string path = exePath;
    return path.substr(0, path.find_last_of("\\/")) + "\\config.ini";
}

void DisableCurrentThreadPowerThrottling(const char* role) {
    THREAD_POWER_THROTTLING_STATE throttlingState = {};
    throttlingState.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    throttlingState.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    throttlingState.StateMask = 0;
    if (!SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttlingState, sizeof(throttlingState))) {
        LogWarn("[%s] Failed to disable execution-speed power throttling (tid=%lu err=%lu)", role, GetCurrentThreadId(),
                GetLastError());
    }
}

void WaitUntilQpcTarget(HANDLE timer, int64_t targetQpc, int64_t qpcFrequency) {
    if (targetQpc <= 0 || qpcFrequency <= 0) {
        return;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    int64_t diff = targetQpc - now.QuadPart;
    if (diff <= 0) {
        return;
    }

    int64_t diffUs = (diff * 1000000) / qpcFrequency;
    constexpr int64_t kTimerTrimUs = 400;
    if (timer && diffUs > kTimerTrimUs) {
        LARGE_INTEGER dueTime;
        dueTime.QuadPart = -static_cast<int64_t>(static_cast<double>(diffUs - kTimerTrimUs) * 10.0);
        if (SetWaitableTimer(timer, &dueTime, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, static_cast<DWORD>(((diffUs - kTimerTrimUs) / 1000) + 5));
        }
    }

    for (;;) {
        QueryPerformanceCounter(&now);
        diff = targetQpc - now.QuadPart;
        if (diff <= 0) {
            break;
        }

        diffUs = (diff * 1000000) / qpcFrequency;
        if (diffUs > 3000) {
            Sleep(1);
        } else if (diffUs > 1000) {
            Sleep(0);
        } else if (diffUs > 200) {
            SwitchToThread();
        } else {
            YieldProcessor();
        }
    }
}

const char* Win32PriorityClassName(DWORD priorityClass) {
    switch (priorityClass) {
        case IDLE_PRIORITY_CLASS:
            return "idle";
        case BELOW_NORMAL_PRIORITY_CLASS:
            return "below_normal";
        case NORMAL_PRIORITY_CLASS:
            return "normal";
        case ABOVE_NORMAL_PRIORITY_CLASS:
            return "above_normal";
        case HIGH_PRIORITY_CLASS:
            return "high";
        case REALTIME_PRIORITY_CLASS:
            return "realtime";
        default:
            return "unknown";
    }
}

bool IsCurrentProcessElevatedForPriorityLog() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elevation = {};
    DWORD returned = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

void ApplyMediaProcessPriority(const AppConfig& config) {
    DWORD priorityClass = NORMAL_PRIORITY_CLASS;
    if (config.processPriority == "idle")
        priorityClass = IDLE_PRIORITY_CLASS;
    else if (config.processPriority == "below_normal")
        priorityClass = BELOW_NORMAL_PRIORITY_CLASS;
    else if (config.processPriority == "above_normal")
        priorityClass = ABOVE_NORMAL_PRIORITY_CLASS;
    else if (config.processPriority == "high")
        priorityClass = HIGH_PRIORITY_CLASS;
    else if (config.processPriority == "realtime")
        priorityClass = REALTIME_PRIORITY_CLASS;

    const DWORD currentClass = GetPriorityClass(GetCurrentProcess());
    if (currentClass == priorityClass) {
        return;
    }

    if (SetPriorityClass(GetCurrentProcess(), priorityClass)) {
        LogInfo("[Media] CPU process priority set to %s", Win32PriorityClassName(priorityClass));
    } else {
        LogWarn("[Media] Failed to set CPU process priority to %s: gle=%lu", Win32PriorityClassName(priorityClass),
                GetLastError());
    }
}

const char* D3dkmtSchedulingPriorityClassName(int priorityClass) {
    switch (priorityClass) {
        case 0:
            return "idle";
        case 1:
            return "below_normal";
        case 2:
            return "normal";
        case 3:
            return "above_normal";
        case 4:
            return "high";
        case 5:
            return "realtime";
        default:
            return "unknown";
    }
}

bool ResolveD3dkmtSchedulingPriorityClass(const std::string& value, int& priorityClass) {
    if (value == "idle") {
        priorityClass = 0;
    } else if (value == "below_normal") {
        priorityClass = 1;
    } else if (value == "normal") {
        priorityClass = 2;
    } else if (value == "above_normal") {
        priorityClass = 3;
    } else if (value == "high") {
        priorityClass = 4;
    } else if (value == "realtime") {
        priorityClass = 5;
    } else {
        return false;
    }
    return true;
}

void ApplyMediaGpuSchedulingPriority(const AppConfig& config, const LUID* adapterLuid ) {
    using D3dkmtSetProcessSchedulingPriorityClassFn = LONG(WINAPI*)(HANDLE, int);
    using D3dkmtGetProcessSchedulingPriorityClassFn = LONG(WINAPI*)(HANDLE, int*);

    static std::mutex s_priorityMutex;
    std::lock_guard<std::mutex> priorityLock(s_priorityMutex);
    static bool s_loggedDisabled = false;
    static bool s_loggedAutoDeferred = false;
    static bool s_appliedNonDefault = false;
    static std::string s_lastRequest;
    static LUID s_lastEnvironmentLuid{};
    static bool s_haveLastEnvironmentLuid = false;

    const bool disabled = config.gpuSchedulingPriority == "off";
    const bool automatic = config.gpuSchedulingPriority == "auto";
    int requestedClass = 2;

    if (disabled) {
        if (!s_appliedNonDefault) {
            if (!s_loggedDisabled) {
                LogInfo("[Media] GPU scheduling priority class disabled (gpu_scheduling_priority=off)");
                s_loggedDisabled = true;
            }
            return;
        }
    } else if (automatic) {
        if (!adapterLuid) {
            if (!s_loggedAutoDeferred) {
                LogInfo("[Media] GPU scheduling priority auto deferred until the capture adapter LUID is known");
                s_loggedAutoDeferred = true;
            }
            return;
        }

        ce::windows_gpu_scheduling::AdapterSchedulingEnvironment environment{};
        const bool queried = ce::windows_gpu_scheduling::QueryAdapterSchedulingEnvironment(*adapterLuid, environment);
        requestedClass = ce::gpu_scheduling::ResolveAutomaticProcessSchedulingPriority(environment.hags);
        const bool newEnvironment =
            !s_haveLastEnvironmentLuid || !ce::windows_gpu_scheduling::SameLuid(s_lastEnvironmentLuid, *adapterLuid);
        if (newEnvironment) {
            LogInfo(
                "[Media] GPU scheduling environment: adapter=%ls luid=%s vendor=0x%04X device=0x%04X "
                "driver=0x%016llX windowsBuild=%u hagsQuery=%d hagsEnabled=%d hagsDefault=%d hagsSupported=%d "
                "hagsSupport=%s open=0x%08lX caps27=0x%08lX caps29=0x%08lX close=0x%08lX autoClass=%s",
                environment.description.empty() ? L"unknown" : environment.description.c_str(),
                ce::windows_gpu_scheduling::FormatLuid(*adapterLuid).c_str(), environment.vendorId,
                environment.deviceId, static_cast<unsigned long long>(environment.driverVersion),
                environment.windowsBuild, queried ? 1 : 0, environment.hags.enabled ? 1 : 0,
                environment.hags.enabledByDefault ? 1 : 0, environment.hags.supported ? 1 : 0,
                ce::gpu_scheduling::HagsSupportStateName(environment.hags.supportState),
                static_cast<unsigned long>(environment.openStatus),
                static_cast<unsigned long>(environment.caps27Status),
                static_cast<unsigned long>(environment.caps29Status),
                static_cast<unsigned long>(environment.closeStatus), D3dkmtSchedulingPriorityClassName(requestedClass));
            s_lastEnvironmentLuid = *adapterLuid;
            s_haveLastEnvironmentLuid = true;
        }
        s_loggedAutoDeferred = false;
    } else if (!ResolveD3dkmtSchedulingPriorityClass(config.gpuSchedulingPriority, requestedClass)) {
        LogWarn("[Media] Ignoring invalid GPU scheduling priority class '%s'", config.gpuSchedulingPriority.c_str());
        return;
    }

    HMODULE gdi32 = GetModuleHandleA("gdi32.dll");
    if (!gdi32) {
        gdi32 = ce::security::LoadSystemLibrary(L"gdi32.dll");
    }
    if (!gdi32) {
        LogWarn("[Media] GPU scheduling priority class unavailable: failed to load gdi32.dll");
        return;
    }

    auto setPriority = reinterpret_cast<D3dkmtSetProcessSchedulingPriorityClassFn>(
        GetProcAddress(gdi32, "D3DKMTSetProcessSchedulingPriorityClass"));
    auto getPriority = reinterpret_cast<D3dkmtGetProcessSchedulingPriorityClassFn>(
        GetProcAddress(gdi32, "D3DKMTGetProcessSchedulingPriorityClass"));
    if (!setPriority) {
        LogWarn("[Media] GPU scheduling priority class unavailable: D3DKMTSetProcessSchedulingPriorityClass missing");
        return;
    }

    int currentClass = -1;
    LONG getStatus = getPriority ? 0 : static_cast<LONG>(ERROR_PROC_NOT_FOUND);
    const bool haveCurrent = getPriority && ((getStatus = getPriority(GetCurrentProcess(), &currentClass)) >= 0);
    std::string automaticRequest;
    const char* requestText = nullptr;
    if (disabled) {
        requestText = "off(reset_to_normal)";
    } else if (automatic) {
        automaticRequest = std::string("auto(") + D3dkmtSchedulingPriorityClassName(requestedClass) + ")@" +
                           ce::windows_gpu_scheduling::FormatLuid(*adapterLuid);
        requestText = automaticRequest.c_str();
    } else {
        requestText = config.gpuSchedulingPriority.c_str();
    }
    if (haveCurrent && currentClass == requestedClass && s_lastRequest == requestText) {
        return;
    }

    const LONG status = setPriority(GetCurrentProcess(), requestedClass);
    const bool elevated = IsCurrentProcessElevatedForPriorityLog();
    if (status < 0) {
        if (haveCurrent) {
            LogWarn(
                "[Media] Failed to set GPU scheduling priority class to %s (config=%s current=%s elevated=%d): "
                "ntstatus=0x%08lX",
                D3dkmtSchedulingPriorityClassName(requestedClass), requestText,
                D3dkmtSchedulingPriorityClassName(currentClass), elevated ? 1 : 0, (unsigned long)status);
        } else {
            LogWarn(
                "[Media] Failed to set GPU scheduling priority class to %s (config=%s current=unknown "
                "getStatus=0x%08lX elevated=%d): ntstatus=0x%08lX",
                D3dkmtSchedulingPriorityClassName(requestedClass), requestText, (unsigned long)getStatus,
                elevated ? 1 : 0, (unsigned long)status);
        }
        return;
    }

    int verifiedClass = -1;
    LONG verifyStatus = getPriority ? 0 : static_cast<LONG>(ERROR_PROC_NOT_FOUND);
    const bool haveVerified = getPriority && ((verifyStatus = getPriority(GetCurrentProcess(), &verifiedClass)) >= 0);
    const bool verified = haveVerified && verifiedClass == requestedClass;
    s_lastRequest = requestText;
    s_loggedDisabled = disabled;
    s_appliedNonDefault = requestedClass != 2;
    if (verified) {
        LogInfo(
            "[Media] GPU scheduling priority class set to %s (config=%s previous=%s current=%s verified=1 "
            "elevated=%d ntstatus=0x%08lX)",
            D3dkmtSchedulingPriorityClassName(requestedClass), requestText,
            haveCurrent ? D3dkmtSchedulingPriorityClassName(currentClass) : "unknown",
            D3dkmtSchedulingPriorityClassName(verifiedClass), elevated ? 1 : 0, (unsigned long)status);
    } else if (haveVerified) {
        LogWarn(
            "[Media] GPU scheduling priority class set call returned success but readback mismatch (config=%s "
            "requested=%s previous=%s current=%s verified=0 elevated=%d ntstatus=0x%08lX)",
            requestText, D3dkmtSchedulingPriorityClassName(requestedClass),
            haveCurrent ? D3dkmtSchedulingPriorityClassName(currentClass) : "unknown",
            D3dkmtSchedulingPriorityClassName(verifiedClass), elevated ? 1 : 0, (unsigned long)status);
    } else {
        LogInfo(
            "[Media] GPU scheduling priority class set call returned success but readback unavailable (config=%s "
            "requested=%s previous=%s getStatus=0x%08lX verifyStatus=0x%08lX verified=unknown elevated=%d "
            "ntstatus=0x%08lX)",
            requestText, D3dkmtSchedulingPriorityClassName(requestedClass),
            haveCurrent ? D3dkmtSchedulingPriorityClassName(currentClass) : "unknown", (unsigned long)getStatus,
            (unsigned long)verifyStatus, elevated ? 1 : 0, (unsigned long)status);
    }
}

void ApplyMediaPrioritySettings(const AppConfig& config) {
    ApplyMediaProcessPriority(config);
    ApplyMediaGpuSchedulingPriority(config);
}

bool ApplyMediaGpuSchedulingPriorityForDevice(const AppConfig& config, ID3D11Device* device) {
    LUID luid{};
    if (!ce::windows_gpu_scheduling::GetAdapterLuid(device, luid)) {
        LogWarn("[Media] Could not resolve D3D11 adapter LUID for GPU scheduling priority");
        return false;
    }
    ApplyMediaGpuSchedulingPriority(config, &luid);
    return true;
}

bool ApplyMediaGpuSchedulingPriorityForSharedAdapter(const AppConfig& config) {
    if (!media_main_g_pSharedMem) {
        return false;
    }
    LUID luid{};
    luid.LowPart = media_main_g_pSharedMem->GetLuidLowPart();
    luid.HighPart = static_cast<LONG>(media_main_g_pSharedMem->GetLuidHighPart());
    if (luid.LowPart == 0 && luid.HighPart == 0) {
        return false;
    }
    ApplyMediaGpuSchedulingPriority(config, &luid);
    return true;
}

void PublishMediaScreenGrabTarget(uint32_t processId, ID3D11Device* device, bool active,
                                         const char* reason) {
    if (!media_main_g_pSharedMem)
        return;

    LUID luid{};
    const bool haveLuid = active && device && ce::windows_gpu_scheduling::GetAdapterLuid(device, luid);
    media_main_g_pSharedMem->runtimeState.PublishScreenGrabTarget(
        processId, haveLuid ? static_cast<int32_t>(luid.LowPart) : 0, haveLuid ? luid.HighPart : 0, active);
    LogInfo("[Media] Screen-grab sensor target %s (pid=%lu adapter=%08lX:%08lX reason=%s)",
            active ? "published" : "cleared", static_cast<unsigned long>(active ? processId : 0),
            static_cast<unsigned long>(active && haveLuid ? static_cast<uint32_t>(luid.HighPart) : 0),
            static_cast<unsigned long>(active && haveLuid ? luid.LowPart : 0), reason ? reason : "unspecified");
}
