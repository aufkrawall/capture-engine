    int32_t activeCaptureLeft = 0;
    int32_t activeCaptureTop = 0;
    const bool haveCaptureOrigin = capture->GetCaptureOrigin(activeCaptureLeft, activeCaptureTop);
    LogInfo(
        "[Media] WGC recording target: target=%s backend=%s hwnd=0x%p hmon=0x%p originOk=%d origin=(%d,%d) "
        "captureCursor=%d nativeWgcCursor=%d encoderCursor=%d",
        activeWgcWindow ? "window" : "monitor", capture->IsUsingDesktopDuplication() ? "DxgiDuplication" : "WGC",
        activeWgcWindow, activeWgcMonitor, haveCaptureOrigin ? 1 : 0, activeCaptureLeft, activeCaptureTop,
        config.video.captureCursor ? 1 : 0,
        ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor) ? 1 : 0,
        config.video.captureCursor ? 1 : 0);
    capture->SetSkipSplitDeviceFlush(config.wgcSkipSplitDeviceFlush);
    capture->SetSameDeviceCapture(config.wgcSameDeviceCapture);
    capture->SetAllowLossyBgra8Pool(config.wgcAllowLossyBgra8Pool);
    const bool explicitTenBit = IsExplicitTenBitVideo(config.video);
    capture->SetRequireHighPrecisionCapture(explicitTenBit);
    capture->SetAllowDuplicationFallback(ce::capture_policy::ShouldAllowWgcFallbackAfterDxgiFailure(
        IsDxgiDupCaptureMethod(config.captureMethod), explicitTenBit));
    const uint32_t initialWgcTargetFps = GetInitialWgcCfrTargetFps(config.video);
    float maxAudioCaptureLatencyMs = 0.0f;
    for (const auto& audioSrc : config.audioSources) {
        if (audioSrc.captureLatencyMs > maxAudioCaptureLatencyMs) {
            maxAudioCaptureLatencyMs = audioSrc.captureLatencyMs;
        }
    }
    const uint32_t outputFps = static_cast<uint32_t>(std::max(0, config.video.fps));
    const bool hasWgcContentDelayBudget = maxAudioCaptureLatencyMs > 0.0f;
    // Smoothness FLOOR: when configured (auto or explicit > 0) the reservoir/copy-pool budget must
    // be allocated even with no audio-latency content delay, otherwise a video-only / low-confidence
    // capture would have no buffer to engage the active-delay jitter-absorbing playout. The floor
    // delay itself is realized within the retained-extra reservoir (not the sync-delay frames), so
    // syncDelayFramesForBudget stays audio-latency-driven (0 here when there is no audio latency).
    const bool wgcSmoothnessFloorBudgetDesired = config.wgcSmoothnessBufferEnabled && !config.video.useVFR &&
                                                 (config.wgcSmoothnessFloorAuto || config.wgcSmoothnessFloorMs > 0);
    const uint32_t syncDelayFramesForBudget =
        hasWgcContentDelayBudget ? ce::capture_policy::GetWgcEstimatedSyncDelayFramesForBudget(
                                       outputFps, static_cast<uint32_t>(std::ceil(maxAudioCaptureLatencyMs)))
                                 : 0u;
    capture->SetSmoothnessBufferBudget(config.wgcSmoothnessBufferEnabled && !config.video.useVFR &&
                                           (hasWgcContentDelayBudget || wgcSmoothnessFloorBudgetDesired),
                                       outputFps, config.wgcSmoothnessBufferMaxMs,
                                       config.wgcSmoothnessBufferVramBudgetMb, syncDelayFramesForBudget);
    capture->SetVideoMemoryReservationMode(config.wgcVideoMemoryReservation);
    if (config.video.useVFR) {
        capture->SetDirectFrameCallback(QueueWgcFrame);
    } else {
        capture->SetDirectFrameCallback(nullptr);
    }
    capture->SetDirectCursorCallback(config.video.captureCursor ? QueueWgcCursorObservation : nullptr);
    capture->ResetStats();
    // Explicitly reset both the cache and the encoder-side state. A prior
    // duplication session may have ended while its software cursor was embedded.
    ResetDuplicationCursorSuppression("WGC recording start");
    g_IsEncoderBottlenecked.store(false, std::memory_order_relaxed);
    g_WgcProducerTargetFps.store(initialWgcTargetFps, std::memory_order_relaxed);
    // A finite WGC MinUpdateInterval aliases variable-rate sources and can turn
    // 138 fps into about 69 fps. CFR therefore receives every compositor update
    // and leaves surplus-frame selection to the timestamp scheduler. DXGI
    // duplication has no producer interval; it shares the same zero target so
    // the screen-grab contract is backend-independent.
    capture->SetTargetFps(0);
    capture->SetProducerTargetFps(initialWgcTargetFps);
    LogInfo(
        "[WGC CFR] Producer contract: backend=%s outputFps=%u producerTargetFps=%u minUpdateInterval100ns=0 "
        "policy=max-rate-variable-input localThrottleFps=0",
        capture->IsUsingDesktopDuplication() ? "dxgi_dup" : "wgc", outputFps, initialWgcTargetFps);
    // Persist before StartCapture so any device rebuild and the first WGC/DXGI
    // submissions inherit the configured relative GPU priority.
    capture->SetGpuPriority(config.video.gpuPriority);

    // For CFR recording, disable the encoder-bottleneck throttle at the WGC
    // callback level.  The throttle is all-or-nothing (bang-bang) and its slow
    // EMA causes boom-bust oscillation that starves the Bresenham credit
    // accumulator, producing irregular frame-hold patterns (visible judder).
    // The encoder thread's buffer cap + Bresenham skip already provide smooth
    // backpressure, so the throttle is both unnecessary and harmful for CFR.
    if (!config.video.useVFR) {
        capture->SetThrottleFlag(nullptr);
        LogInfo("[Media] WGC CFR mode: pull-latest sampling enabled, callback queue bypassed");
        LogInfo("[Media] WGC CFR mode: encoder-bottleneck throttle disabled (buffer cap provides backpressure)");
    }
    if (g_pSharedMem) {
        g_pSharedMem->runtimeState.wgcTargetFps.store(initialWgcTargetFps, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcCaptureHealthFlags.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcCaptureHealthFps.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcSelectionErrorAvgUs.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcSelectionErrorMaxUs.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcSelectionErrorSignedAvgUs.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcSelectionEarlyMaxUs.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcSelectionLateMaxUs.store(0, std::memory_order_relaxed);
    }
    if (!capture->StartCapture()) {
        capture->SetDirectFrameCallback(nullptr);
        capture->SetDirectCursorCallback(nullptr);
        return false;
    }
    SnapshotWgcRuntimeLogState(capture);

    // Tell the encoder whether the capture source runs at >8 bpc so that
    // bit_depth=auto resolves to 10-bit even when the WGC frame pool fell
    // back to BGRA8 (e.g. R10G10B10A2 pool creation failed).
    if (MediaEngine_SetSourcePrefers10Bit) {
        const bool hiPrec = capture->IsHighPrecisionSource();
        LogInfo("[Media] WGC source high-precision=%s, notifying encoder", hiPrec ? "YES" : "NO");
        MediaEngine_SetSourcePrefers10Bit(hiPrec);
    } else {
        LogWarn("[Media] MediaEngine_SetSourcePrefers10Bit not available (old mediaengine.dll?)");
    }

    g_WgcCaptureShutdown = false;
    // Recording-lifetime config snapshot: the main thread reassigns `config`
    // on late hook connects and IPC config reloads (refreshActiveConfig),
    // which would be a use-after-free race against a by-reference reader on
    // this thread. Recording settings must not change live mid-session anyway.
    {
        auto configSnapshot = std::make_shared<const AppConfig>(config);
        g_WgcCaptureThread = std::thread([configSnapshot]() { WgcCaptureThreadFunc(*configSnapshot); });
    }
    return true;
}

// Defined in inject_main.cpp
extern std::string GetProcessNameFromPID(DWORD pid);

// Window finding helper
struct WindowSearch {
    DWORD pid;
    HWND hwnd;
};

static BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
    WindowSearch* search = (WindowSearch*)lParam;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == search->pid) {
        // Look for the main visible window
        // Checks: Visible, not child
        if (IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == 0) {
            // Check styles to avoid tool windows
            LONG_PTR styles = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
            if (!(styles & WS_EX_TOOLWINDOW)) {
                search->hwnd = hwnd;
                return FALSE;  // Found, stop
            }
        }
    }
    return TRUE;
}

static HWND GetMainWindowForProcess(DWORD pid) {
    WindowSearch search = {pid, NULL};
    EnumWindows(EnumWindowsCallback, (LPARAM)&search);
    return search.hwnd;
}

struct ForegroundWgcWindowCandidate {
    HWND hwnd = NULL;
    DWORD pid = 0;
    std::string processName;
    bool usable = false;
    bool fullscreenLike = false;
};

static std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

static bool IsIgnoredForegroundWgcClass(HWND hwnd) {
    char className[128] = {};
    if (GetClassNameA(hwnd, className, static_cast<int>(sizeof(className))) <= 0) {
        return false;
    }

    const std::string lowerClass = ToLowerAscii(className);
    return lowerClass == "progman" || lowerClass == "workerw" || lowerClass == "shell_traywnd";
}

static bool IsIgnoredForegroundWgcProcess(const std::string& processName) {
    const std::string lowerName = ToLowerAscii(processName);
    return lowerName.empty() || lowerName == "unknown" || lowerName == "explorer.exe" ||
           lowerName == "applicationframehost.exe" || lowerName == "shellexperiencehost.exe" ||
           lowerName == "searchhost.exe" || lowerName == "startmenuexperiencehost.exe" ||
           lowerName == "textinputhost.exe" || lowerName == "captureengine.exe";
}

static ForegroundWgcWindowCandidate GetForegroundWgcWindowCandidate() {
    ForegroundWgcWindowCandidate candidate;
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) {
        return candidate;
    }

    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root) {
        hwnd = root;
    }

    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd) || hwnd == GetDesktopWindow() ||
        GetWindow(hwnd, GW_OWNER) != 0) {
        return candidate;
    }

    const LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    const LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if ((style & WS_CHILD) || (exStyle & WS_EX_TOOLWINDOW) || IsIgnoredForegroundWgcClass(hwnd)) {
        return candidate;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) {
        return candidate;
    }

    std::string processName = GetProcessNameFromPID(pid);
    if (IsIgnoredForegroundWgcProcess(processName)) {
        return candidate;
    }

    candidate.hwnd = hwnd;
    candidate.pid = pid;
    candidate.processName = processName;
    candidate.usable = true;
    candidate.fullscreenLike = IsWindowFullscreenLike(hwnd);
    return candidate;
}

static bool MatchesProcessEntry(const WhitelistEntry& entry, const std::string& lowerProcessName) {
    return MatchesProcessName(entry, lowerProcessName);
}

static bool MatchesProcessEntries(const std::vector<WhitelistEntry>& entries, const std::string& processName) {
    if (processName.empty()) {
        return false;
    }

    std::string lowerName = processName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    for (const auto& entry : entries) {
        if (MatchesProcessEntry(entry, lowerName)) {
            return true;
        }
    }

    return false;
}

static const ApplicationProfile* FindApplicationProfileForProcess(const AppConfig& config,
                                                                  const std::string& processName) {
    if (processName.empty())
        return nullptr;

    std::string lowerName = processName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    for (const ApplicationProfile& profile : config.applicationProfiles) {
        if (!profile.target.HasProcess())
            continue;
        std::string lowerTarget = profile.target.pattern;
        std::transform(lowerTarget.begin(), lowerTarget.end(), lowerTarget.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if ((!profile.legacy && lowerName == lowerTarget) ||
            (profile.legacy && MatchesProcessEntry(profile.target, lowerName)))
            return &profile;
    }
    return nullptr;
}

static const ApplicationProfile* FindApplicationProfileForTarget(const AppConfig& config,
                                                                 const WhitelistEntry& target) {
    auto found = std::find_if(config.applicationProfiles.begin(), config.applicationProfiles.end(),
                              [&](const ApplicationProfile& profile) { return profile.target == target; });
    return found == config.applicationProfiles.end() ? nullptr : &*found;
}

static int64_t RectArea(const RECT& rect) {
    const int64_t width = std::max<LONG>(0, rect.right - rect.left);
    const int64_t height = std::max<LONG>(0, rect.bottom - rect.top);
    return width * height;
}

static HWND FindMatchingWgcWindow(const std::vector<WhitelistEntry>& targets, int* selectedScore = nullptr,
                                  bool requireExactProcessNames = false, uint32_t* selectedPid = nullptr,
                                  std::string* selectedProcessName = nullptr,
                                  WhitelistEntry* selectedTarget = nullptr) {
    struct WgcSearchContext {
        const std::vector<WhitelistEntry>* targets;
        HWND result;
        HWND foregroundRoot;
        int checked;
        int matched;
        int bestScore;
        bool requireExactProcessNames;
        uint32_t bestPid;
        std::string bestProcessName;
        WhitelistEntry bestTarget;
        bool hasBestTarget;
    };

    HWND foregroundRoot = GetForegroundWindow();
    if (foregroundRoot) {
        HWND root = GetAncestor(foregroundRoot, GA_ROOT);
        if (root) {
            foregroundRoot = root;
        }
    }

    WgcSearchContext ctx = {&targets, NULL, foregroundRoot, 0, 0, std::numeric_limits<int>::min(),
                            requireExactProcessNames, 0, {}, {}, false};
    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            WgcSearchContext* context = (WgcSearchContext*)lParam;
            if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
                return TRUE;
            }
            if (GetWindow(hwnd, GW_OWNER) != 0) {
                return TRUE;
            }
            const LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
            const LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
            if ((style & WS_CHILD) || (exStyle & WS_EX_TOOLWINDOW)) {
                return TRUE;
            }

            context->checked++;

            char title[256];
            GetWindowTextA(hwnd, title, sizeof(title));
            std::string titleStr = title;
            std::transform(titleStr.begin(), titleStr.end(), titleStr.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

            char className[256];
            GetClassNameA(hwnd, className, sizeof(className));
            std::string classStr = className;
            std::transform(classStr.begin(), classStr.end(), classStr.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            std::string procName;
            if (pid != 0) {
                procName = GetProcessNameFromPID(pid);
                std::transform(procName.begin(), procName.end(), procName.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            }

            for (const auto& entry : *context->targets) {
                MatchMode mode = entry.mode;
                bool matched = false;
                bool matchedByTitleOrClass = false;
                bool matchedByProcess = false;

                if (entry.HasWindow()) {
                    std::string winLower = entry.windowName;
                    std::transform(winLower.begin(), winLower.end(), winLower.begin(),
                                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

                    if (mode == MatchMode::kExact) {
                        matched = !titleStr.empty() && titleStr == winLower;
                    } else {
                        matched = !titleStr.empty() && titleStr.find(winLower) != std::string::npos;
                        if (!matched && mode == MatchMode::kTitleType && !classStr.empty()) {
                            matched = classStr.find(winLower) != std::string::npos;
                        }
                    }
                    matchedByTitleOrClass = matched;
                }

                if (!matched && MatchesProcessName(entry, procName, context->requireExactProcessNames)) {
                    matched = true;
                    matchedByProcess = true;
                }

                if (matched) {
                    RECT windowRect = {};
                    RECT clientRect = {};
                    const bool haveWindowRect = GetWindowRect(hwnd, &windowRect) != FALSE;
                    const bool haveClientRect = GetWindowClientRectInScreen(hwnd, clientRect);
                    const int64_t area =
                        std::max(haveWindowRect ? RectArea(windowRect) : 0, haveClientRect ? RectArea(clientRect) : 0);
                    int score = 1000;
                    if (context->foregroundRoot && hwnd == context->foregroundRoot) {
                        score += 100000;
                    }
                    if (IsWindowFullscreenLike(hwnd)) {
                        score += 50000;
                    }
                    if (matchedByTitleOrClass) {
                        score += 5000;
                    }
                    if (matchedByProcess) {
                        score += 2000;
                    }
                    score += static_cast<int>(std::min<int64_t>(area / 1000, 40000));

                    ++context->matched;
                    if (!context->result || score > context->bestScore) {
                        context->result = hwnd;
                        context->bestScore = score;
                        context->bestPid = pid;
                        context->bestProcessName = procName;
                        context->bestTarget = entry;
                        context->hasBestTarget = true;
                    }
                    break;
                }
            }
            return TRUE;
        },
        (LPARAM)&ctx);

    if (ctx.result) {
        DWORD pid = 0;
        GetWindowThreadProcessId(ctx.result, &pid);
        LogDebug(
            "[Media] WGC window detection selected hwnd=0x%p pid=%lu fullscreenLike=%d score=%d "
            "(matched=%d checked=%d foreground=%d)",
            ctx.result, static_cast<unsigned long>(pid), IsWindowFullscreenLike(ctx.result) ? 1 : 0, ctx.bestScore,
            ctx.matched, ctx.checked, (ctx.foregroundRoot && ctx.result == ctx.foregroundRoot) ? 1 : 0);
    }

    if (selectedScore)
        *selectedScore = ctx.result ? ctx.bestScore : std::numeric_limits<int>::min();
    if (selectedPid)
        *selectedPid = ctx.result ? ctx.bestPid : 0;
    if (selectedProcessName)
        *selectedProcessName = ctx.result ? ctx.bestProcessName : std::string{};
    if (selectedTarget)
        *selectedTarget = ctx.hasBestTarget ? ctx.bestTarget : WhitelistEntry{};

    return ctx.result;
}

static std::string GetLocalConfigPath() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string path = exePath;
    return path.substr(0, path.find_last_of("\\/")) + "\\config.ini";
}

class ScopedMmcssTask {
public:
    ScopedMmcssTask(const wchar_t* taskName, AVRT_PRIORITY priority, const char* role) : role_(role) {
        DWORD taskIndex = 0;
        handle_ = AvSetMmThreadCharacteristicsW(taskName, &taskIndex);
        if (handle_) {
            if (!AvSetMmThreadPriority(handle_, priority)) {
                LogWarn("[%s] AvSetMmThreadPriority failed (tid=%lu err=%lu)", role_, GetCurrentThreadId(),
                        GetLastError());
            } else {
                LogInfo("[%s] Thread QoS enabled (tid=%lu task=%ls priority=%d)", role_, GetCurrentThreadId(), taskName,
                        static_cast<int>(priority));
            }
        } else {
            const DWORD mmcssError = GetLastError();
            if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)) {
                LogWarn("[%s] MMCSS and THREAD_PRIORITY_HIGHEST setup failed (tid=%lu mmcssErr=%lu priorityErr=%lu)",
                        role_, GetCurrentThreadId(), mmcssError, GetLastError());
            } else {
                LogWarn("[%s] MMCSS setup failed; using THREAD_PRIORITY_HIGHEST (tid=%lu err=%lu)", role_,
                        GetCurrentThreadId(), mmcssError);
            }
        }
    }

    ~ScopedMmcssTask() {
        if (handle_) {
            if (!AvRevertMmThreadCharacteristics(handle_)) {
                LogWarn("[%s] AvRevertMmThreadCharacteristics failed (tid=%lu err=%lu)", role_, GetCurrentThreadId(),
                        GetLastError());
            }
        }
    }

    ScopedMmcssTask(const ScopedMmcssTask&) = delete;
    ScopedMmcssTask& operator=(const ScopedMmcssTask&) = delete;

private:
    HANDLE handle_ = nullptr;
    const char* role_ = "Thread";
};

static void DisableCurrentThreadPowerThrottling(const char* role) {
    THREAD_POWER_THROTTLING_STATE throttlingState = {};
    throttlingState.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    throttlingState.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    throttlingState.StateMask = 0;
    if (!SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttlingState, sizeof(throttlingState))) {
        LogWarn("[%s] Failed to disable execution-speed power throttling (tid=%lu err=%lu)", role, GetCurrentThreadId(),
                GetLastError());
    }
}

static void WaitUntilQpcTarget(HANDLE timer, int64_t targetQpc, int64_t qpcFrequency) {
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

static const char* Win32PriorityClassName(DWORD priorityClass) {
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

static bool IsCurrentProcessElevatedForPriorityLog() {
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

static void ApplyMediaProcessPriority(const AppConfig& config) {
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

static const char* D3dkmtSchedulingPriorityClassName(int priorityClass) {
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

static bool ResolveD3dkmtSchedulingPriorityClass(const std::string& value, int& priorityClass) {
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

static void ApplyMediaGpuSchedulingPriority(const AppConfig& config, const LUID* adapterLuid = nullptr) {
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
