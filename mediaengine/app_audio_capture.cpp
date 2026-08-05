#include "app_audio_capture_internal.h"

// ============================================================================
// AppAudioCapture Implementation
// ============================================================================
AppAudioCapture::AppAudioCapture() {
    captureEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!captureEvent_) {
        DLL_Log("[AppAudioCapture] CreateEventW for capture callback failed: 0x%lx; polling will be used",
                GetLastError());
    }
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) {
        DLL_Log("[AppAudioCapture] CreateEventW for activation cancellation failed: 0x%lx", GetLastError());
    }
    packetReadyEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!packetReadyEvent_) {
        DLL_Log("[AppAudioCapture] CreateEventW for packet notification failed: 0x%lx", GetLastError());
    }
}

AppAudioCapture::~AppAudioCapture() {
    Stop();
    if (captureEvent_) {
        CloseHandle(captureEvent_);
        captureEvent_ = nullptr;
    }
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
    if (packetReadyEvent_) {
        CloseHandle(packetReadyEvent_);
        packetReadyEvent_ = nullptr;
    }
}

void AppAudioCapture::SetRequestedFormat(int sampleRate, int channels, uint32_t channelMask) {
    requestedSampleRate = sampleRate > 0 ? sampleRate : 48000;
    requestedChannels = std::clamp(channels > 0 ? channels : 2, 1, 8);
    requestedChannelMask = channelMask;
    if (requestedChannelMask == 0) {
        if (requestedChannels == 1) {
            requestedChannelMask = SPEAKER_FRONT_CENTER;
        } else if (requestedChannels == 2) {
            requestedChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        }
    }
}

bool AppAudioCapture::IsSupported() {
    // Check Windows build version
    // Per-process loopback requires Windows 10 build 20348+
    typedef LONG(WINAPI * RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtdll)
        return false;

    RtlGetVersionPtr RtlGetVersion = (RtlGetVersionPtr)GetProcAddress(hNtdll, "RtlGetVersion");
    if (!RtlGetVersion)
        return false;

    RTL_OSVERSIONINFOW osvi = {};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    if (RtlGetVersion(&osvi) != 0)
        return false;

    // Windows 10 = 10.0, Windows 11 = 10.0 with build >= 22000
    // Per-process loopback added in build 20348
    if (osvi.dwMajorVersion > 10)
        return true;
    if (osvi.dwMajorVersion == 10 && osvi.dwBuildNumber >= 20348)
        return true;

    return false;
}

bool AppAudioCapture::StartByPID(DWORD processId) {
    if (!IsSupported()) {
        DLL_Log(
            "[AppAudioCapture] Per-process audio not supported on this "
            "Windows version");
        return false;
    }

    if (isCapturing.load() || isMonitoring.load() || asyncStartInProgress.load(std::memory_order_acquire)) {
        DLL_Log("[AppAudioCapture] Already running, call Stop() first");
        return false;
    }

    if (captureThread.joinable() || pAudioClient || pCaptureClient || pwfx) {
        DLL_Log("[AppAudioCapture] Cleaning up completed/partial capture state before PID start");
        Stop(true);
    }

    if (!IsProcessRunning(processId)) {
        DLL_Log("[AppAudioCapture] Process %lu not found", processId);
        return false;
    }

    DLL_Log("[AppAudioCapture] Starting capture for PID %lu", processId);
    if (stopEvent_ && !ResetEvent(stopEvent_)) {
        DLL_Log("[AppAudioCapture] ResetEvent for activation cancellation failed: 0x%lx", GetLastError());
        return false;
    }
    shouldStop.store(false);
    workerRecycleRequested.store(false, std::memory_order_release);
    targetPID.store(processId);
    targetProcessName.clear();

    if (!BeginAsyncStartForPID(processId)) {
        targetPID.store(0, std::memory_order_release);
        return false;
    }
    return true;
}

bool AppAudioCapture::StartByName(const std::string& processName) {
    if (!IsSupported()) {
        DLL_Log(
            "[AppAudioCapture] Per-process audio not supported on this "
            "Windows version");
        return false;
    }

    if (isCapturing.load() || isMonitoring.load() || asyncStartInProgress.load(std::memory_order_acquire)) {
        DLL_Log("[AppAudioCapture] Already running, call Stop() first");
        return false;
    }

    if (captureThread.joinable() || pAudioClient || pCaptureClient || pwfx) {
        DLL_Log("[AppAudioCapture] Cleaning up completed/partial capture state before process monitor start");
        Stop(true);
    }

    DLL_Log("[AppAudioCapture] Starting monitor for process '%s'", processName.c_str());
    targetProcessName = processName;
    if (stopEvent_ && !ResetEvent(stopEvent_)) {
        DLL_Log("[AppAudioCapture] ResetEvent for activation cancellation failed: 0x%lx", GetLastError());
        targetProcessName.clear();
        return false;
    }
    shouldStop.store(false);
    workerRecycleRequested.store(false, std::memory_order_release);
    isMonitoring.store(true);

    // Start the process monitor thread
    try {
        monitorThread = std::thread(&AppAudioCapture::ProcessMonitorLoop, this);
    } catch (const std::exception& error) {
        DLL_Log("[AppAudioCapture] Failed to create process monitor thread: %s", error.what());
        isMonitoring.store(false, std::memory_order_release);
        shouldStop.store(true, std::memory_order_release);
        targetProcessName.clear();
        return false;
    }

    return true;
}

void AppAudioCapture::Stop(bool discardPendingPackets) {
    shouldStop.store(true);
    if (stopEvent_) {
        SetEvent(stopEvent_);
    }
    if (captureEvent_) {
        SetEvent(captureEvent_);
    }

    {
        std::lock_guard<std::mutex> lock(startMutex);
        if (pendingStartFuture.valid()) {
            pendingStartFuture.wait();
            try {
                (void)pendingStartFuture.get();
            } catch (const std::exception& error) {
                DLL_Log("[AppAudioCapture] Async start raised during Stop: %s", error.what());
            }
        }
        asyncStartInProgress.store(false, std::memory_order_release);
        startPendingValid.store(false, std::memory_order_release);
    }

    // Stop monitoring
    isMonitoring.store(false);
    if (monitorThread.joinable()) {
        monitorThread.join();
    }

    // Stop capturing
    isCapturing.store(false);
    if (captureEvent_) {
        SetEvent(captureEvent_);
    }
    if (captureThread.joinable()) {
        captureThread.join();
    }

    const bool sessionMonitorWasRunning = audioSessionMonitor_.IsRunning();
    audioSessionMonitor_.Stop();
    const size_t monitoredEndpoints = audioSessionMonitor_.RegisteredEndpointCount();
    const uint64_t sessionNotifications = audioSessionMonitor_.Generation();
    const uint64_t droppedSessionNotifications = audioSessionMonitor_.DroppedNotificationCount();
    if (sessionMonitorWasRunning) {
        DLL_Log("[AppAudioCapture] Audio-session monitor stopped: endpoints=%zu notifications=%llu dropped=%llu",
                monitoredEndpoints, static_cast<unsigned long long>(sessionNotifications),
                static_cast<unsigned long long>(droppedSessionNotifications));
    }

    if (discardPendingPackets) {
        DiscardPendingPackets();
    }
    CleanupCapture();
    targetPID.store(0);
    targetProcessName.clear();
}

ce::process_loopback::ProcessNameSelection AppAudioCapture::FindProcessByName(const std::string& name,
                                                                              bool logSelection) {
    const auto processes = SnapshotProcessTree();
    const auto selection = ce::process_loopback::SelectProcessTreeRootByName(processes, name);
    if (logSelection && selection.selectedProcessId != 0) {
        DLL_Log(
            "[AppAudioCapture] Process-name tree resolution '%s': matches=%zu roots=%zu firstPID=%lu "
            "selectedRootPID=%lu selectedParentPID=%lu selectedNameMembers=%zu selectedProcessTreeMembers=%zu "
            "firstMatchWasRoot=%d",
            name.c_str(), selection.matchingProcessCount, selection.rootCandidateCount,
            static_cast<unsigned long>(selection.firstMatchProcessId),
            static_cast<unsigned long>(selection.selectedProcessId),
            static_cast<unsigned long>(selection.selectedParentProcessId), selection.selectedTreeSize,
            selection.selectedProcessTreeSize, selection.firstMatchProcessId == selection.selectedProcessId ? 1 : 0);
    }
    return selection;
}

bool AppAudioCapture::IsProcessRunning(DWORD pid) {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) {
        return false;
    }

    DWORD exitCode = 0;
    BOOL result = GetExitCodeProcess(hProcess, &exitCode);
    CloseHandle(hProcess);

    return result && exitCode == STILL_ACTIVE;
}
