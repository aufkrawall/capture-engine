// Freeze detection for FreezeWatchdog: heartbeat bookkeeping, the liveness
// evidence a freeze claim needs, engine-dependent timeouts, and the watchdog
// thread. Writing the dump itself lives in freeze_watchdog_dump.cpp.

#include "freeze_watchdog.h"
#include <tlhelp32.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include "../../common/secure_dll_loading.h"
#include "dxgi_shared.h"
#include "fg_detection.h"
#include "hook_common.h"

FreezeWatchdog g_RenderWatchdog;

static uint64_t GetCurrentMicros() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

static bool ShouldAllowImmediateDumpRequest(std::atomic<uint64_t>& lastDumpRequestMicros, uint64_t nowMicros,
                                            uint64_t dedupWindowMicros = 10'000'000) {
    uint64_t previousRequest = lastDumpRequestMicros.load(std::memory_order_acquire);
    while (true) {
        if (previousRequest != 0 && nowMicros > previousRequest && (nowMicros - previousRequest) < dedupWindowMicros) {
            return false;
        }
        if (lastDumpRequestMicros.compare_exchange_weak(previousRequest, nowMicros, std::memory_order_acq_rel,
                                                        std::memory_order_acquire)) {
            return true;
        }
    }
}

static bool IsProcessInForeground(DWORD processId) {
    HWND fgWindow = GetForegroundWindow();
    if (!fgWindow) {
        return false;
    }

    DWORD fgPid = 0;
    GetWindowThreadProcessId(fgWindow, &fgPid);
    return fgPid == processId;
}

namespace {
struct DialogInfo {
    HWND hwnd = nullptr;
    DWORD threadId = 0;
    bool visible = false;
    char title[256] = {};
};

struct DialogSearchContext {
    DWORD processId = 0;
    DialogInfo* info = nullptr;
};

BOOL CALLBACK FindProcessDialogWindowProc(HWND hwnd, LPARAM lParam) {
    auto* context = reinterpret_cast<DialogSearchContext*>(lParam);
    if (!context || !context->info) {
        return TRUE;
    }

    DWORD windowProcessId = 0;
    const DWORD windowThreadId = GetWindowThreadProcessId(hwnd, &windowProcessId);
    if (windowProcessId != context->processId) {
        return TRUE;
    }

    char className[64] = {};
    if (!GetClassNameA(hwnd, className, static_cast<int>(sizeof(className)))) {
        return TRUE;
    }

    if (_stricmp(className, "#32770") != 0) {
        return TRUE;
    }

    context->info->hwnd = hwnd;
    context->info->threadId = windowThreadId;
    context->info->visible = IsWindowVisible(hwnd) != FALSE;
    GetWindowTextA(hwnd, context->info->title, static_cast<int>(sizeof(context->info->title)));
    return FALSE;
}

bool FindBlockingDialogWindow(DWORD processId, DialogInfo& info) {
    DialogSearchContext context = {};
    context.processId = processId;
    context.info = &info;
    EnumWindows(FindProcessDialogWindowProc, reinterpret_cast<LPARAM>(&context));
    return info.hwnd != nullptr;
}

std::string DescribeDialog(const DialogInfo& info) {
    std::string description = info.visible ? "visible dialog" : "hidden dialog";
    if (info.title[0] != '\0') {
        description += " '";
        description += info.title;
        description += "'";
    }
    return description;
}

std::string GetDialogIdentity(const DialogInfo& info) {
    if (info.title[0] != '\0') {
        return info.title;
    }
    return "#32770";
}
}  // namespace

FreezeWatchdog::FreezeWatchdog() noexcept : processId_(GetCurrentProcessId()) {
    try {
    char name[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, name, MAX_PATH);
    processName_ = std::filesystem::path(name).filename().string();
    InitializeDbgHelp();
    } catch (...) {
        processName_.clear();
    }
}

FreezeWatchdog::~FreezeWatchdog() {
    Stop();
    if (hDbgHelp_) {
        FreeLibrary(hDbgHelp_);
        hDbgHelp_ = nullptr;
        pMiniDumpWriteDump_ = nullptr;
    }
}

bool FreezeWatchdog::InitializeDbgHelp() {
    std::call_once(dbgHelpInitOnce_, [this]() {
        hDbgHelp_ = ce::security::LoadSystemLibrary(L"dbghelp.dll");
        if (!hDbgHelp_) {
            OutputDebugStringA("[FreezeWatchdog] Failed to load dbghelp.dll\n");
            dbgHelpInitialized_ = true;
            return;
        }

        pMiniDumpWriteDump_ =
            reinterpret_cast<decltype(MiniDumpWriteDump)*>(GetProcAddress(hDbgHelp_, "MiniDumpWriteDump"));

        if (!pMiniDumpWriteDump_) {
            OutputDebugStringA("[FreezeWatchdog] Failed to get MiniDumpWriteDump\n");
            FreeLibrary(hDbgHelp_);
            hDbgHelp_ = nullptr;
        } else {
            OutputDebugStringA("[FreezeWatchdog] DbgHelp initialized successfully\n");
        }

        dbgHelpInitialized_ = true;
    });
    return pMiniDumpWriteDump_ != nullptr;
}

bool FreezeWatchdog::Start(double timeoutSeconds) {
    if (running_.exchange(true)) {
        return false;
    }

    if (!dbgHelpInitialized_) {
        InitializeDbgHelp();
    }

    double finalTimeout = timeoutSeconds;

    const bool ue5Active = IsUE5Active();
    const bool dlssFGActive = IsDLSSFGActive();

    if (ue5Active) {
        finalTimeout = UE5_TIMEOUT;
        OutputDebugStringA("[FreezeWatchdog] UE5 detected, using extended timeout\n");
    } else if (dlssFGActive) {
        finalTimeout = DLSS_FG_TIMEOUT;
        OutputDebugStringA("[FreezeWatchdog] DLSS FG detected, using extended timeout\n");
    }

    timeoutSeconds_.store(finalTimeout);
    uint64_t now = GetCurrentMicros();
    lastHeartbeat_.store(now);
    startupTime_.store(now);
    lastDumpRequestMicros_.store(0, std::memory_order_release);
    dumpCapturedForCurrentRun_.store(false, std::memory_order_release);
    loggedMissingRenderLoop_.store(false, std::memory_order_release);

    char logMsg[256];
    snprintf(logMsg, sizeof(logMsg), "[FreezeWatchdog] Starting: timeout=%.1fs, grace=%.1fs\n", finalTimeout,
             STARTUP_GRACE_PERIOD);
    OutputDebugStringA(logMsg);
    HookLogImportant("FreezeWatchdog: Started (timeout=%.1fs, monitoredTid=%lu, ue5=%d, dlssFg=%d)", finalTimeout,
                     monitoredThreadId_.load(std::memory_order_acquire), ue5Active ? 1 : 0, dlssFGActive ? 1 : 0);

    watchdogThread_ = std::thread(&FreezeWatchdog::WatchdogThread, this);

    return true;
}

void FreezeWatchdog::Stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (watchdogThread_.joinable()) {
        watchdogThread_.join();
    }
}

void FreezeWatchdog::Heartbeat() {
    const DWORD heartbeatTid = GetCurrentThreadId();
    DWORD monitoredTid = monitoredThreadId_.load(std::memory_order_acquire);
    if (monitoredTid == 0) {
        // Heartbeat() is the "I am presenting" entry point, so its first caller
        // establishes the thread a freeze dump has to capture — the watchdog is
        // otherwise armed from a hook-install worker thread that never presents,
        // which named the wrong thread in every log line and dump target.
        // Adopt only while no frame-generation runtime owns presentation:
        // Streamline and FFX both present from their own workers, and one of
        // those must never be mistaken for the game's render thread. DX12's
        // provenance-checked source Present keeps republishing the real one.
        const bool fgRuntimePresenting =
            DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.load(std::memory_order_acquire) ||
            DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        DWORD expected = 0;
        if (!fgRuntimePresenting && monitoredThreadId_.compare_exchange_strong(expected, heartbeatTid,
                                                                              std::memory_order_acq_rel,
                                                                              std::memory_order_acquire)) {
            HookLogImportant("FreezeWatchdog: Adopted render thread tid=%lu from the first present heartbeat",
                             heartbeatTid);
        }
        monitoredTid = monitoredThreadId_.load(std::memory_order_acquire);
    }
    if (monitoredTid != 0 && monitoredTid != heartbeatTid) {
        static std::atomic<int> s_threadSwitchLogCount{0};
        if (s_threadSwitchLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
            HookLog("FreezeWatchdog: Helper heartbeat from tid=%lu while monitoring tid=%lu", heartbeatTid,
                    monitoredTid);
        }
    }
    NoteRenderLoopObserved(RenderLoopSource::D3DPresent);
    lastHeartbeat_.store(GetCurrentMicros(), std::memory_order_release);
}

void FreezeWatchdog::HeartbeatFromHelperThread() {
    NoteRenderLoopObserved(RenderLoopSource::D3DPresent);
    lastHeartbeat_.store(GetCurrentMicros(), std::memory_order_release);
}

void FreezeWatchdog::NoteRenderLoopObserved(RenderLoopSource source) {
    std::atomic<bool>& sourceObserved =
        source == RenderLoopSource::VulkanLayerPresent ? vulkanLayerRenderLoopObserved_ : d3dRenderLoopObserved_;
    sourceObserved.store(true, std::memory_order_release);
    if (!renderLoopObserved_.exchange(true, std::memory_order_acq_rel)) {
        HookLogImportant("FreezeWatchdog: Render loop observed via %s - freeze assertions armed (monitoredTid=%lu)",
                         source == RenderLoopSource::VulkanLayerPresent ? "the Vulkan layer" : "a D3D present",
                         monitoredThreadId_.load(std::memory_order_acquire));
    }
}

bool FreezeWatchdog::HasLiveRenderLoopEvidence() const {
    bool vulkanLayerStillActive = false;
    if (vulkanLayerRenderLoopObserved_.load(std::memory_order_acquire)) {
        const SharedMemoryLayout* sharedMemory = GetHookSharedMemory();
        vulkanLayerStillActive =
            sharedMemory &&
            sharedMemory->runtimeState.IsVulkanLayerOwnedByProcess(GetCurrentProcessId());
    }
    return ce::freeze_watchdog_policy::HasLiveRenderLoopEvidence(
        d3dRenderLoopObserved_.load(std::memory_order_acquire),
        vulkanLayerRenderLoopObserved_.load(std::memory_order_acquire), vulkanLayerStillActive);
}

// The CE Vulkan layer presents from a separate DLL and cannot call Heartbeat(),
// so it publishes every present into shared memory instead. Folding that into
// the ordinary heartbeat keeps one liveness currency: elapsed time, the freeze
// gate and the dump's target thread then all mean the same thing whether the
// game presents through D3D or through the layer.
//
// The old code instead special-cased "vulkan-1.dll loaded && d3d12.dll not
// loaded" inside IsFrozen(). A Vulkan game defeats that the moment anything
// pulls d3d12.dll into the process — CE's own DX12 interop does — which is how
// Strange Brigade `20260818_190149` got a freeze dump at 144 FPS.
void FreezeWatchdog::PollCrossApiPresentLiveness() {
    const SharedMemoryLayout* sharedMemory = GetHookSharedMemory();
    if (!sharedMemory) {
        return;
    }
    const uint32_t currentPid = GetCurrentProcessId();
    if (!sharedMemory->runtimeState.IsVulkanLayerOwnedByProcess(currentPid)) {
        return;
    }

    // Same 2 s recency window the hook-install Vulkan-ownership check uses, and
    // four watchdog polls wide, so the heartbeat tracks the layer's real present
    // rate instead of the poll rate. A stale tick deliberately produces no
    // heartbeat: that is a Vulkan render loop that stopped, which is the one
    // case where this watchdog should still be allowed to fire.
    if (!sharedMemory->runtimeState.IsVulkanPresentRecentForProcess(
            currentPid, GetTickCount64(), static_cast<uint32_t>(kCrossApiPresentMaxAgeMs))) {
        return;
    }

    // Claim the dump's target thread only while no D3D present path owns it.
    // DXVK titles present through both the layer and CE's DXGI wrapper, and the
    // two run on different threads; whichever proves itself first keeps the
    // claim instead of the two overwriting each other every poll. Refreshing a
    // claim this poll already owns still tracks Vulkan swapchain recreation.
    const DWORD presentThreadId =
        sharedMemory->runtimeState.GetVulkanPresentThreadForProcess(currentPid);
    const DWORD monitoredTid = monitoredThreadId_.load(std::memory_order_acquire);
    if (presentThreadId != 0 && presentThreadId != monitoredTid &&
        (monitoredTid == 0 || monitoredThreadFromVulkanLayer_.load(std::memory_order_acquire))) {
        monitoredThreadId_.store(presentThreadId, std::memory_order_release);
        monitoredThreadFromVulkanLayer_.store(true, std::memory_order_release);
        HookLogImportant("FreezeWatchdog: Monitoring the Vulkan layer's present thread tid=%lu (was %lu)",
                         presentThreadId, monitoredTid);
    }
    NoteRenderLoopObserved(RenderLoopSource::VulkanLayerPresent);
    lastHeartbeat_.store(GetCurrentMicros(), std::memory_order_release);
}

void FreezeWatchdog::SetFreezeCallback(FreezeCallback callback) {
    freezeCallback_ = std::move(callback);
}

bool FreezeWatchdog::IsFrozen() const {
    uint64_t last = lastHeartbeat_.load();
    uint64_t now = GetCurrentMicros();
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    double elapsed = (now - last) / 1'000'000.0;

    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    double sinceStartup = (now - startupTime_.load()) / 1'000'000.0;
    if (sinceStartup < STARTUP_GRACE_PERIOD) {
        return false;
    }

    bool inPresentCall = DXGIShared::g_SharedState.presentInFlightDepth.load(std::memory_order_acquire) > 0;

    // Alt+Tab/minimized games can legitimately stop presenting for a while.
    // Suppress background freezes only when no Present is in flight and no FG
    // runtime currently owns presentation. If an FSR/DLSS presenter path is in
    // charge, the hang can sit inside the runtime presenter thread while focus
    // heuristics stay quiet, so continue monitoring.
    const bool processForeground = IsProcessInForeground(processId_);
    const bool forceMonitor = forceMonitor_.load(std::memory_order_relaxed);
    const bool runtimePresentationMonitor = runtimePresentationMonitor_.load(std::memory_order_relaxed);

    // Never claim "render thread frozen" without ever having seen that render
    // thread. The watchdog is armed before any present is observed, so without
    // this gate the elapsed timer measures the wrong thing entirely for every
    // game whose presents do not pass through CE's D3D hooks.
    if (!ce::freeze_watchdog_policy::ShouldAssertRenderThreadFreeze(HasLiveRenderLoopEvidence(), inPresentCall,
                                                                    forceMonitor, runtimePresentationMonitor)) {
        if (elapsed > timeoutSeconds_.load() && !loggedMissingRenderLoop_.exchange(true, std::memory_order_acq_rel)) {
            HookLogImportant(
                "FreezeWatchdog: Not asserting a freeze after %.1fs — CE has never observed a present on this "
                "process's render loop (monitoredTid=%lu). The game most likely renders through an API CE does not "
                "present for; a hang here would be indistinguishable from a healthy frame.",
                elapsed, monitoredThreadId_.load(std::memory_order_acquire));
        }
        return false;
    }

    if (ce::freeze_watchdog_policy::ShouldSuppressFreezeCheckForBackgroundProcess(
            processForeground, forceMonitor, inPresentCall, runtimePresentationMonitor)) {
        return false;
    }

    // When forceMonitor_ is set (device removed), always check — the GPU
    // driver may be stuck in a kernel call even though the window is unfocused.
    if (!processForeground && !forceMonitor) {
        double inFlightTimeout = timeoutSeconds_.load();
        if (inFlightTimeout > 15.0) {
            inFlightTimeout = 15.0;
        }
        return elapsed > inFlightTimeout;
    }

    return elapsed > timeoutSeconds_.load();
}

bool FreezeWatchdog::IsDLSSFGActive() const {
    return g_FGCompat.IsDLSSFGApiActive() || g_FGCompat.GetActiveFGType() == FGCompatibility::FGType::DLSS_FG;
}

bool FreezeWatchdog::IsUE5Active() const {
    // Check for common UE5 DLLs
    if (GetModuleHandleW(L"ue5.dll") || GetModuleHandleW(L"UnrealEditor.dll")) {
        return true;
    }

    // Check for UE5 engine patterns in loaded modules
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me;
        me.dwSize = sizeof(me);
        if (Module32FirstW(hSnapshot, &me)) {
            do {
                std::wstring moduleName = me.szModule;
                std::transform(moduleName.begin(), moduleName.end(), moduleName.begin(), ::towlower);
                if (moduleName.find(L"unreal") != std::wstring::npos || moduleName.find(L"ue4") != std::wstring::npos ||
                    moduleName.find(L"ue5") != std::wstring::npos) {
                    CloseHandle(hSnapshot);
                    return true;
                }
            } while (Module32NextW(hSnapshot, &me));
        }
        CloseHandle(hSnapshot);
    }

    return false;
}

double FreezeWatchdog::GetRecommendedTimeout() const {
    if (IsUE5Active())
        return UE5_TIMEOUT;
    if (IsDLSSFGActive())
        return DLSS_FG_TIMEOUT;
    return DEFAULT_TIMEOUT;
}

void FreezeWatchdog::RequestImmediateDump(const std::string& reason, DWORD preferredThreadId) {
    if (!running_.load(std::memory_order_acquire) || reason.empty()) {
        return;
    }

    bool expectedDumpCaptured = false;
    if (!ce::freeze_watchdog_policy::ShouldCaptureWatchdogDump(expectedDumpCaptured) ||
        !dumpCapturedForCurrentRun_.compare_exchange_strong(expectedDumpCaptured, true, std::memory_order_acq_rel,
                                                            std::memory_order_acquire)) {
        HookLogImportant("FreezeWatchdog: Suppressing duplicate watchdog dump request for '%s'", reason.c_str());
        return;
    }

    const uint64_t nowMicros = GetCurrentMicros();
    if (!ShouldAllowImmediateDumpRequest(lastDumpRequestMicros_, nowMicros)) {
        HookLog("FreezeWatchdog: Suppressing duplicate immediate dump request for '%s'", reason.c_str());
        return;
    }

    DWORD targetTid = preferredThreadId;
    if (targetTid == 0) {
        if (FreezeWatchdog::PreferredThreadProvider provider =
                preferredThreadProvider_.load(std::memory_order_acquire)) {
            const DWORD preferredRenderTid = provider();
            if (preferredRenderTid != 0) {
                targetTid = preferredRenderTid;
            }
        }
    }
    if (targetTid == 0) {
        targetTid = monitoredThreadId_.load(std::memory_order_acquire);
    }
    if (targetTid == 0) {
        const DWORD presentHookThreadId = DXGIShared::GetThreadStuckInsideCePresentHook();
        targetTid = ce::freeze_watchdog_policy::ResolveFreezeDumpTargetThread(
            targetTid, presentHookThreadId != 0, presentHookThreadId);
        if (targetTid != 0) {
            HookLogImportant(
                "FreezeWatchdog: No monitored render thread — targeting tid=%lu, the thread still inside "
                "CE's Present hook",
                targetTid);
        }
    }

    HookLogImportant("FreezeWatchdog: Immediate dump requested (%s, targetTid=%lu)", reason.c_str(), targetTid);
    if (freezeCallback_) {
        freezeCallback_(reason);
    }

    if (ce::freeze_watchdog_policy::ShouldDeferImmediateDumpToWatchdogThread(GetCurrentThreadId(), targetTid)) {
        {
            std::lock_guard<std::mutex> lock(pendingImmediateDumpMutex_);
            pendingImmediateDumpReason_ = reason;
            pendingImmediateDumpTargetTid_ = targetTid;
        }
        pendingImmediateDump_.store(true, std::memory_order_release);
        HookLogImportant(
            "FreezeWatchdog: Deferring immediate dump to watchdog thread because caller matches targetTid=%lu",
            targetTid);
        return;
    }

    CreateMinidumpWithThreadContext(reason, targetTid);
}

void FreezeWatchdog::WatchdogThread() {
    const auto checkInterval = std::chrono::milliseconds(500);
    constexpr double kDialogDumpDelaySeconds = 5.0;
    constexpr double kDialogStartupWindowSeconds = 90.0;
    constexpr uint64_t kDialogDumpDedupWindowMicros = 10'000'000;
    uint64_t lastLogTime = 0;
    uint64_t dialogSeenSince = 0;
    DWORD dialogThreadId = 0;
    std::string dialogDescription;
    std::string dialogIdentity;
    bool dialogDumpWritten = false;
    bool freezeDumpSuppressionLogged = false;
    uint64_t lastDialogDumpTime = 0;
    std::string lastDialogDumpIdentity;

    OutputDebugStringA("[FreezeWatchdog] Watchdog thread started\n");

    while (running_.load(std::memory_order_acquire) && !HookIsShuttingDown()) {
        std::this_thread::sleep_for(checkInterval);

        if (!running_.load(std::memory_order_acquire)) {
            OutputDebugStringA("[FreezeWatchdog] Watchdog stopping\n");
            break;
        }

        if (pendingImmediateDump_.exchange(false, std::memory_order_acq_rel)) {
            std::string pendingReason;
            DWORD pendingTargetTid = 0;
            {
                std::lock_guard<std::mutex> lock(pendingImmediateDumpMutex_);
                pendingReason = pendingImmediateDumpReason_;
                pendingTargetTid = pendingImmediateDumpTargetTid_;
                pendingImmediateDumpReason_.clear();
                pendingImmediateDumpTargetTid_ = 0;
            }

            if (!pendingReason.empty()) {
                HookLogImportant("FreezeWatchdog: Processing deferred immediate dump request (%s, targetTid=%lu)",
                                 pendingReason.c_str(), pendingTargetTid);
                CreateMinidumpWithThreadContext(pendingReason, pendingTargetTid);
                continue;
            }
        }

        // Present paths that cannot reach Heartbeat() themselves (the Vulkan
        // layer DLL) publish their liveness into shared memory; fold it in
        // before any elapsed time is derived from the heartbeat.
        PollCrossApiPresentLiveness();

        uint64_t now = GetCurrentMicros();
        uint64_t lastBeat = lastHeartbeat_.load(std::memory_order_acquire);
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        double elapsed = (now - lastBeat) / 1'000'000.0;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        double sinceStartup = (now - startupTime_.load(std::memory_order_acquire)) / 1'000'000.0;
        DialogInfo dialogInfo = {};
        bool hasDialog = FindBlockingDialogWindow(processId_, dialogInfo);
        if (hasDialog) {
            std::string currentDialogDescription = DescribeDialog(dialogInfo);
            std::string currentDialogIdentity = GetDialogIdentity(dialogInfo);
            if (dialogSeenSince == 0 || dialogIdentity != currentDialogIdentity) {
                dialogSeenSince = now;
                dialogThreadId = dialogInfo.threadId;
                dialogDescription = currentDialogDescription;
                dialogIdentity = currentDialogIdentity;
                dialogDumpWritten = false;
                freezeDumpSuppressionLogged = false;
                HookLogImportant("FreezeWatchdog: Detected %s (hwnd=%p tid=%lu)", dialogDescription.c_str(),
                                 dialogInfo.hwnd, dialogThreadId);
            } else if (dialogThreadId != dialogInfo.threadId || dialogDescription != currentDialogDescription) {
                HookLog("FreezeWatchdog: Dialog state changed to %s (hwnd=%p tid=%lu)",
                        currentDialogDescription.c_str(), dialogInfo.hwnd, dialogInfo.threadId);
                dialogThreadId = dialogInfo.threadId;
                dialogDescription = currentDialogDescription;
            }
        } else if (dialogSeenSince != 0) {
            HookLog("FreezeWatchdog: Blocking dialog cleared");
            dialogSeenSince = 0;
            dialogThreadId = 0;
            dialogDescription.clear();
            dialogIdentity.clear();
            dialogDumpWritten = false;
            freezeDumpSuppressionLogged = false;
        }

        if (now - lastLogTime > 10'000'000) {
            lastLogTime = now;
            char logMsg[256];
            const int renderLoopObserved = renderLoopObserved_.load(std::memory_order_acquire) ? 1 : 0;
            snprintf(logMsg, sizeof(logMsg),
                     "[FreezeWatchdog] Status: elapsed=%.1fs, timeout=%.1fs, monitoredTid=%lu, dialogTid=%lu, "
                     "runtimePresentation=%d, renderLoopObserved=%d\n",
                     elapsed, timeoutSeconds_.load(), monitoredThreadId_.load(std::memory_order_acquire),
                     dialogThreadId, runtimePresentationMonitor_.load(std::memory_order_relaxed) ? 1 : 0,
                     renderLoopObserved);
            OutputDebugStringA(logMsg);
            HookLog(
                "FreezeWatchdog: Status elapsed=%.1fs timeout=%.1fs monitoredTid=%lu dialogTid=%lu "
                "runtimePresentation=%d renderLoopObserved=%d",
                elapsed, timeoutSeconds_.load(), monitoredThreadId_.load(std::memory_order_acquire), dialogThreadId,
                runtimePresentationMonitor_.load(std::memory_order_relaxed) ? 1 : 0, renderLoopObserved);
        }

        if (dialogSeenSince != 0 && !dialogDumpWritten) {
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            double dialogElapsed = (now - dialogSeenSince) / 1'000'000.0;
            const bool isErrGfxStateDialog = dialogDescription.find("ERR_GFX_STATE") != std::string::npos;
            const double requiredDialogDumpDelay = isErrGfxStateDialog ? 0.0 : kDialogDumpDelaySeconds;
            const bool withinDialogDumpWindow = sinceStartup <= kDialogStartupWindowSeconds || isErrGfxStateDialog;
            if (dialogElapsed >= requiredDialogDumpDelay && withinDialogDumpWindow) {
                if (!dialogIdentity.empty() && dialogIdentity == lastDialogDumpIdentity &&
                    (now - lastDialogDumpTime) < kDialogDumpDedupWindowMicros) {
                    HookLogImportant(
                        "FreezeWatchdog: Suppressing duplicate dialog dump for %s because a dump was already captured "
                        "%.1fs ago",
                        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                        dialogDescription.c_str(), (now - lastDialogDumpTime) / 1'000'000.0);
                    dialogDumpWritten = true;
                    continue;
                }

                std::string reason = dialogDescription.empty() ? "Blocking dialog detected"
                                                               : ("Blocking " + dialogDescription + " detected");
                if (isErrGfxStateDialog) {
                    HookLogImportant("FreezeWatchdog: Critical dialog %s detected - capturing dump immediately",
                                     dialogDescription.c_str());
                } else {
                    HookLogImportant("FreezeWatchdog: Persistent dialog detected after %.1fs - capturing dump",
                                     dialogElapsed);
                }
                RequestImmediateDump(reason, dialogThreadId);
                dialogDumpWritten = true;
                lastDialogDumpTime = now;
                lastDialogDumpIdentity = dialogIdentity;
            }
        }

        if (IsFrozen()) {
            if (dialogSeenSince != 0 && dialogDumpWritten) {
                if (!freezeDumpSuppressionLogged) {
                    HookLogImportant(
                        "FreezeWatchdog: Suppressing redundant freeze dump because a dialog dump was already captured "
                        "for %s",
                        dialogDescription.c_str());
                    freezeDumpSuppressionLogged = true;
                }
                continue;
            }
            freezeDumpSuppressionLogged = false;

            std::string reason = "Render thread frozen for " + std::to_string(static_cast<int>(elapsed)) + " seconds";

            OutputDebugStringA("[FreezeWatchdog] FREEZE DETECTED!\n");
            OutputDebugStringA(reason.c_str());
            OutputDebugStringA("\n");
            HookLogImportant("FreezeWatchdog: Freeze detected (%s)", reason.c_str());

            RequestImmediateDump(reason);

            // Do NOT terminate the process: the game may recover on its own,
            // and forcefully killing it loses unsaved data and prevents the
            // game's own crash-handling from running.  The minidump and the
            // callback (which can notify the host) are sufficient action.
            OutputDebugStringA("[FreezeWatchdog] Dump created. Not terminating - watchdog stopping.\n");
            return;
        }
    }

    OutputDebugStringA("[FreezeWatchdog] Watchdog thread exiting\n");
}
