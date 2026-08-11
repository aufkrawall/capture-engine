#include "dx12_hook_internal.h"


void ResetAuthoritativeFSRRealFrameOnlyStreak() {
dx12_hook_g_AuthoritativeFSRRealFrameOnlyStreak.store(0, std::memory_order_release);
}


void ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak() {
dx12_hook_g_StaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak.store(0, std::memory_order_release);
}


bool CanUseFSRFGHeuristics(const char** blockedReason) {
if (dx12_hook_g_QueueChangeHeuristicAuthoritativeBaseline.load(std::memory_order_acquire) != nullptr) {
    if (blockedReason) {
        *blockedReason = "normal swapchain return is awaiting its authoritative queue baseline";
    }
    return false;
}

if (g_FGCompat.IsFSRFGApiActive()) {
    if (blockedReason) {
        *blockedReason = "authoritative FSR FG state is already active";
    }
    return false;
}

// Block when Streamline FG is running — SL creates internal queues that
// trigger queue-change heuristics.  Without this check, enabling DLSS FG
// causes false FSR FG detection (SL's queue ≠ origGame → "queue change"
// heuristic fires → pre-SL renders on wrong queue → DEVICE_HUNG).
if (DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) {
    if (blockedReason) {
        *blockedReason = "Streamline FG is running (queue changes are from SL, not FSR)";
    }
    return false;
}

// Block during grace period after SL FG turns OFF.  The queue naturally
// changes from SL's internal queue back to origGame — this must not be
// misinterpreted as FSR FG.  The heuristic runs BEFORE the outer block in
// ProcessFrame, so g_StreamlineFGRunning alone can't prevent the false
// positive on the same frame SL OFF fires.
// NOTE: Do NOT decrement here — this function is called per-ECL (thousands/sec).
// The counter is decremented once per ProcessFrame in the queue-change heuristic.
if (dx12_hook_g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0) {
    if (blockedReason) {
        *blockedReason = "SL FG just turned OFF (grace period)";
    }
    return false;
}

const auto runtimeMode = g_FGCompat.GetRuntimeMode();
const bool streamlineStartupHandoffPending = DXGIShared::IsStreamlineStartupHandoffPending();
if (ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRActivationDuringAuthoritativeStreamlineStartupHandoff(
        dx12_hook_g_FGRuntimeOwnsSwapchain, streamlineStartupHandoffPending, runtimeMode)) {
    if (blockedReason) {
        *blockedReason = "fresh authoritative Streamline startup handoff is still runtime-inactive";
    }
    return false;
}

ID3D12CommandQueue* currentSwapchainQueue = nullptr;
{
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    currentSwapchainQueue = dx12_hook_g_SwapchainQueue;
}
const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
const bool postFSRNonFGRecovery = ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(
    dx12_hook_g_HadFSRFGPhase, dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG, IsActualFrameGenerationActive(), streamlineFGRunning,
    currentSwapchainQueue != nullptr);
const bool postSLLastWorkingQueueStillActiveDuringRecentTeardown =
    dx12_hook_g_PostSLLastWorkingQueue != nullptr &&
    GetTickCount64() < dx12_hook_g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);
if (ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRActivationDuringPostFSRNonFGRecovery(
        postFSRNonFGRecovery, false, postSLLastWorkingQueueStillActiveDuringRecentTeardown)) {
    if (blockedReason) {
        *blockedReason = "post-FSR non-FG recovery is still seeing preserved PostSL teardown traffic";
    }
    return false;
}

// Only block when DLSS FG is confirmed active WITH a known multiplier.
// When DLSS modules are merely loaded but FG is off (or API state is transiently
// toggling — common when switching to FSR FG), heuristics are safe.  The
// g_PrimaryGameQueue filter ensures only game-queue ECL calls are counted,
// preventing false positives from FG runtime queues.
if (g_FGCompat.IsDLSSFGApiActive()) {
    int mult = g_FGCompat.GetFGMultiplier();
    if (mult >= 2) {
        if (blockedReason) {
            *blockedReason = "DLSS FG is actively generating frames";
        }
        return false;
    }
}

if (blockedReason) {
    *blockedReason = nullptr;
}
return true;
}


bool IsFFXPresentCallbackStalled() {
if (!dx12_hook_g_FFXPresentCallbackBridgeExpected.load(std::memory_order_acquire)) {
    return false;
}

const ULONGLONG now = GetTickCount64();
const ULONGLONG lastCallback = dx12_hook_g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire);
if (lastCallback != 0) {
    constexpr ULONGLONG kStallThresholdMs = 2000;
    return (now - lastCallback) > kStallThresholdMs;
}
// The callback has never fired since hook init.  If the runtime has owned
// the swapchain for several seconds without a single callback, treat it as
// stalled so the overlay does not stay invisible indefinitely.
if (dx12_hook_g_FGRuntimeOwnsSwapchain && dx12_hook_g_FGRuntimeOwnsSwapchainSince != 0) {
    constexpr ULONGLONG kNeverFiredStallThresholdMs = 3000;
    return (now - dx12_hook_g_FGRuntimeOwnsSwapchainSince) > kNeverFiredStallThresholdMs;
}
const ULONGLONG assumedSince = dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedSinceMs.load(std::memory_order_acquire);
if (dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.load(std::memory_order_acquire) && assumedSince != 0) {
    constexpr ULONGLONG kProgressFallbackNeverFiredStallThresholdMs = 1500;
    return (now - assumedSince) > kProgressFallbackNeverFiredStallThresholdMs;
}
return false;
}


ProgressResolvedOfficialFFXOverlayFallbackProof EvaluateProgressResolvedOfficialFFXOverlayFallbackProof() {
ProgressResolvedOfficialFFXOverlayFallbackProof result{};
result.progressResolved = dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.load(std::memory_order_acquire);

const ULONGLONG assumedSince = dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedSinceMs.load(std::memory_order_acquire);
if (result.progressResolved && assumedSince != 0) {
    const ULONGLONG now = GetTickCount64();
    result.stableMs = (now >= assumedSince) ? (now - assumedSince) : 0;
}

{
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    result.hasSwapchainQueue = dx12_hook_g_SwapchainQueue != nullptr;
    result.hasOriginalGameQueue = dx12_hook_g_OriginalGameQueue != nullptr;
    result.swapchainQueueMatchesOriginalGameQueue =
        result.hasSwapchainQueue && result.hasOriginalGameQueue && dx12_hook_g_SwapchainQueue == dx12_hook_g_OriginalGameQueue;
}

ID3D12Device* device = g_Device.load(std::memory_order_acquire);
result.hasDevice = device != nullptr;
result.deviceHr = device ? device->GetDeviceRemovedReason() : E_POINTER;

result.proof = result.progressResolved && result.stableMs >= dx12_hook_kProgressResolvedOfficialFFXOverlayFallbackStableMs &&
               result.swapchainQueueMatchesOriginalGameQueue && result.hasDevice && SUCCEEDED(result.deviceHr);
return result;
}


void ResetFFXPresentCallbackFirstStallDetection() {
dx12_hook_g_FFXPresentCallbackFirstStallEverDetectedMs.store(0, std::memory_order_release);
}


ULONGLONG GetFFXPresentCallbackStallDurationMs() {
const ULONGLONG firstStallMs = dx12_hook_g_FFXPresentCallbackFirstStallEverDetectedMs.load(std::memory_order_acquire);
if (firstStallMs == 0) {
    return 0;
}
const ULONGLONG now = GetTickCount64();
return (now >= firstStallMs) ? (now - firstStallMs) : 0;
}


void UpdateFFXPresentCallbackFirstStallDetection(bool ffxPresentCallbackStalled) {
if (!ffxPresentCallbackStalled) {
    return;
}
const bool callbackEverFired = dx12_hook_g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire) != 0;
if (callbackEverFired) {
    // The callback fired at least once — the stall is transient, not a
    // never-fired scenario.  Do not arm the long-timeout escape hatch.
    return;
}
ULONGLONG expected = 0;
dx12_hook_g_FFXPresentCallbackFirstStallEverDetectedMs.compare_exchange_strong(expected, GetTickCount64(),
                                                                     std::memory_order_acq_rel);
}


bool ShouldAllowNormalOverlayFallbackForCurrentFFXPresentCallbackStall(bool ffxPresentCallbackStalled) {
const bool explicitNativeFSROffPending =
    dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire);
const bool evaluateFFXCallbackFallback = ce::dx12_overlay_policy::ShouldEvaluateFFXPresentCallbackFallback(
    ffxPresentCallbackStalled, explicitNativeFSROffPending);
UpdateFFXPresentCallbackFirstStallDetection(ffxPresentCallbackStalled);
const bool progressResolvedOfficialFFXPresentPath =
    dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.load(std::memory_order_acquire);
const bool directFFXApiConfirmation = g_FGCompat.HasDirectFFXApiConfirmation();
const ULONGLONG lastCallback = dx12_hook_g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire);
const ULONGLONG assumedSince = dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedSinceMs.load(std::memory_order_acquire);
const bool currentFFXPresentCallbackProof = ce::dx12_overlay_policy::IsFFXPresentCallbackProofCurrent(
    lastCallback, dx12_hook_g_SwapchainQueueCaptureTime, assumedSince);
const ProgressResolvedOfficialFFXOverlayFallbackProof progressProof =
    EvaluateProgressResolvedOfficialFFXOverlayFallbackProof();
const ULONGLONG stallDurationMs = GetFFXPresentCallbackStallDurationMs();
return ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(
    evaluateFFXCallbackFallback, progressResolvedOfficialFFXPresentPath, directFFXApiConfirmation,
    currentFFXPresentCallbackProof, progressProof.proof, stallDurationMs, explicitNativeFSROffPending);
}


void LogSuppressedFFXPresentCallbackStallNormalOverlayFallback() {
static std::atomic<int> s_suppressedStallFallbackLogCount{0};
const int logCount = s_suppressedStallFallbackLogCount.fetch_add(1, std::memory_order_relaxed);
if (logCount >= 5 && (logCount % 600) != 0) {
    return;
}

const ULONGLONG now = GetTickCount64();
const ULONGLONG lastCallback = dx12_hook_g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire);
const ULONGLONG assumedSince = dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedSinceMs.load(std::memory_order_acquire);
const ProgressResolvedOfficialFFXOverlayFallbackProof progressProof =
    EvaluateProgressResolvedOfficialFFXOverlayFallbackProof();
HookLogImportant(
    "DX12: FFX present callback appears stalled but normal overlay fallback is unsafe for "
    "this native FSR handoff until direct ffxConfigure/present-callback proof exists "
    "(lastCallback=%llu progressAssumedFor=%llums directFFX=%d explicitNativeOff=%d runtimeOwns=%d "
    "runtime=%s apiFSR=%d nativeFGPath=%d stableProof=%d stableFor=%llums requiredStable=%llums "
    "hasScQ=%d hasOrig=%d sameQueue=%d "
    "hasDevice=%d deviceHr=0x%08X scQueue=%p origGame=%p cmdQ=%p log=%d)",
    lastCallback, assumedSince ? (now - assumedSince) : 0, g_FGCompat.HasDirectFFXApiConfirmation() ? 1 : 0,
    dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire) ? 1 : 0,
    dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0, ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()),
    g_FGCompat.IsFSRFGApiActive() ? 1 : 0, HookHasRuntimeOwnedNativeFGPresentPath() ? 1 : 0,
    progressProof.proof ? 1 : 0, progressProof.stableMs, dx12_hook_kProgressResolvedOfficialFFXOverlayFallbackStableMs,
    progressProof.hasSwapchainQueue ? 1 : 0, progressProof.hasOriginalGameQueue ? 1 : 0,
    progressProof.swapchainQueueMatchesOriginalGameQueue ? 1 : 0, progressProof.hasDevice ? 1 : 0,
    static_cast<unsigned>(progressProof.deviceHr), dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue,
    g_CommandQueue.load(std::memory_order_acquire), logCount + 1);
}


bool UpdateHeuristicFSRFGState(bool active, const char* source) {
if (active && ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRAfterExplicitNativeFSROff(
                  dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire),
                  dx12_hook_g_FGRuntimeOwnsSwapchain)) {
    g_FGCompat.SetHeuristicFSRFGActive(false);

    static std::atomic<int> s_explicitOffSuppressedLogCount{0};
    if (s_explicitOffSuppressedLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
        HookLogImportant(
            "DX12: Suppressing %s FSR FG heuristic because native FSR explicitly turned FG off while the "
            "runtime-owned "
            "swapchain teardown is still active",
            source ? source : "unknown");
    }
    return false;
}

const char* blockedReason = nullptr;
if (!CanUseFSRFGHeuristics(&blockedReason)) {
    g_FGCompat.SetHeuristicFSRFGActive(false);

    if (active) {
        static std::atomic<int> s_suppressedLogCount{0};
        if (s_suppressedLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            HookLog("DX12: Suppressing %s FSR FG heuristic because %s", source,
                    blockedReason ? blockedReason : "it is unsafe");
        }
    }
    return false;
}

g_FGCompat.SetHeuristicFSRFGActive(active);
return true;
}


bool IsActualFrameGenerationActive() {
const auto runtimeMode = g_FGCompat.GetRuntimeMode();
return runtimeMode == ce::fg_runtime::RuntimeMode::kDLSSFG || runtimeMode == ce::fg_runtime::RuntimeMode::kFSRFG;
}


bool IsFSRFrameGenerationActive() {
return g_FGCompat.GetRuntimeMode() == ce::fg_runtime::RuntimeMode::kFSRFG;
}


bool IsDLSSFrameGenerationActive() {
// Planner-classified NVIDIA DLSS FG. This is the only DLSS evidence late
// injection can have: sl.dlssg / sl.interposer were already loaded before hook
// installation, so the Streamline FG signal and the runtime-ownership latch
// are never published (session 20260811_221202).
return g_FGCompat.GetRuntimeMode() == ce::fg_runtime::RuntimeMode::kDLSSFG;
}


bool IsNvidiaSmoothMotionActiveRuntime() {
return g_FGCompat.GetRuntimeMode() == ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion;
}


bool ShouldReserveInactiveFGOverlaySpaceNow() {
const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
ID3D12CommandQueue* currentSwapchainQueue = nullptr;
{
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    currentSwapchainQueue = dx12_hook_g_SwapchainQueue;
}

const bool postFSRNonFGRecovery = ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(
    dx12_hook_g_HadFSRFGPhase, dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG, IsActualFrameGenerationActive(), streamlineFGRunning,
    currentSwapchainQueue != nullptr);
const bool recentStreamlineTeardown = dx12_hook_g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0;
const bool postSLRecentTeardownActivity =
    GetTickCount64() < dx12_hook_g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);
return ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceForCurrentFrame(
    postFSRNonFGRecovery, recentStreamlineTeardown, postSLRecentTeardownActivity);
}


ID3D12CommandQueue* GetFrameClassificationQueue() {
ID3D12CommandQueue* primaryQueue = dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire);
ID3D12CommandQueue* originalQueue = dx12_hook_g_OriginalGameQueue;
ID3D12CommandQueue* swapchainQueue = nullptr;
bool actualFGActive = false;
bool streamlineFGRunning = false;
bool recoveringPostFSRNonFG = false;
{
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    swapchainQueue = dx12_hook_g_SwapchainQueue;
    actualFGActive = IsActualFrameGenerationActive();
    streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    recoveringPostFSRNonFG = ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(
        dx12_hook_g_HadFSRFGPhase, dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG, actualFGActive, streamlineFGRunning,
        swapchainQueue != nullptr);
}

if (ce::dx12_overlay_policy::ShouldUsePrimaryQueueForFrameClassificationDuringPostFSRNonFGRecovery(
        recoveringPostFSRNonFG, actualFGActive, streamlineFGRunning, swapchainQueue != nullptr,
        originalQueue != nullptr, primaryQueue != nullptr, originalQueue == primaryQueue)) {
    static std::atomic<int> s_postFSRClassificationPrimaryLogCount{0};
    int logCount = s_postFSRClassificationPrimaryLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 300) == 0) {
        HookLogImportant(
            "DX12: Frame classification using primary queue %p during post-FSR non-FG recovery "
            "(origGame=%p scQ=%p lastWorking=%p offscreen=%d)",
            primaryQueue, originalQueue, swapchainQueue, dx12_hook_g_PostSLLastWorkingQueue,
            dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG ? 1 : 0);
    }
    return primaryQueue;
}

if (originalQueue) {
    return originalQueue;
}

return primaryQueue;
}


bool ShouldSuppressLikelyDuplicateTopLevelPresent(IDXGISwapChain3* sc3, UINT backBufferIdx) {
if (!sc3 || !g_IPC || !g_IPC->IsCaptureRequested()) {
    return false;
}

SharedMemoryLayout* shm = g_IPC->GetSharedMem();
if (!shm) {
    return false;
}

const int captureFps = shm->fpsLimiter.GetCaptureFps();
if (captureFps <= 0) {
    return false;
}

const int64_t targetIntervalUs = 1000000LL / static_cast<int64_t>(captureFps);
const int64_t suppressWindowUs = std::clamp((targetIntervalUs * 3) / 4, 1500LL, 7000LL);
const int64_t nowUs = PerfLogger::GetQpcUs();
IDXGISwapChain* swapchain = static_cast<IDXGISwapChain*>(sc3);

static std::atomic<IDXGISwapChain*> s_lastAcceptedSwapchain{nullptr};
static std::atomic<uint32_t> s_lastAcceptedBackBufferIdx{UINT32_MAX};
static std::atomic<int64_t> s_lastAcceptedPresentUs{0};
static std::atomic<uint64_t> s_suppressedPresentCount{0};

IDXGISwapChain* lastSwapchain = s_lastAcceptedSwapchain.load(std::memory_order_acquire);
uint32_t lastBackBufferIdx = s_lastAcceptedBackBufferIdx.load(std::memory_order_acquire);
int64_t lastAcceptedPresentUs = s_lastAcceptedPresentUs.load(std::memory_order_acquire);
int64_t sinceLastUs = nowUs - lastAcceptedPresentUs;

if (lastSwapchain == swapchain && lastBackBufferIdx == backBufferIdx && lastAcceptedPresentUs != 0 &&
    sinceLastUs > 0 && sinceLastUs < suppressWindowUs) {
    uint64_t suppressCount = s_suppressedPresentCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (suppressCount <= 10 || (suppressCount % 1000) == 0) {
        HookLogImportant(
            "DX12: Suppressing likely duplicate top-level Present #%llu "
            "(sc=%p bb=%u since=%lldus window=%lldus captureFps=%d)",
            static_cast<unsigned long long>(suppressCount), swapchain, backBufferIdx,
            static_cast<long long>(sinceLastUs), static_cast<long long>(suppressWindowUs), captureFps);
    }
    return true;
}

s_lastAcceptedSwapchain.store(swapchain, std::memory_order_release);
s_lastAcceptedBackBufferIdx.store(backBufferIdx, std::memory_order_release);
s_lastAcceptedPresentUs.store(nowUs, std::memory_order_release);
return false;
}


bool ShouldSkipCaptureForTargetCadence() {
SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
return ShouldSkipCaptureForTargetCadence(shm, "DX12");
}
