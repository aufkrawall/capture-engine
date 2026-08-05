#include "dx12_hook_internal.h"


void WaitForInFlightPostSLCallbacks(const char* reason) {
for (int spin = 0; spin < 200; ++spin) {
    uint32_t inFlight = dx12_hook_g_PostSLCallbackInFlight.load(std::memory_order_acquire);
    if (inFlight == 0) {
        return;
    }

    if (spin == 0 || spin == 10 || spin == 50) {
        HookLogImportant("%s — waiting for %u in-flight PostSL callback(s)", reason, inFlight);
    }
    Sleep(1);
}

uint32_t remaining = dx12_hook_g_PostSLCallbackInFlight.load(std::memory_order_acquire);
if (remaining != 0) {
    HookLogImportant("%s — timed out waiting for %u in-flight PostSL callback(s)", reason, remaining);
}
}


void WaitForOverlayGpuIdle(const char* reason) {
if (!dx12_hook_g_State.fence || dx12_hook_g_State.currentFenceValue == 0) {
    return;
}

const UINT64 lastVal = dx12_hook_g_State.currentFenceValue;
HANDLE drainEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
if (!drainEvent) {
    return;
}

HRESULT drainHr = dx12_hook_g_State.fence->SetEventOnCompletion(lastVal, drainEvent);
if (SUCCEEDED(drainHr)) {
    DWORD waitResult = WaitForSingleObject(drainEvent, 200);
    HookLogImportant("%s — drained overlay GPU work (fenceVal=%llu wait=%u)", reason, (unsigned long long)lastVal,
                     waitResult);
} else {
    HookLogImportant("%s — fence drain failed hr=0x%08X", reason, drainHr);
}
CloseHandle(drainEvent);
}


void ClearPostSLPinnedSLWrapperQueue(const char* reason) {
ID3D12CommandQueue* oldPinnedWrapperQueue = nullptr;
{
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    oldPinnedWrapperQueue = dx12_hook_g_PostSLPinnedSLWrapperQueue;
    dx12_hook_g_PostSLPinnedSLWrapperQueue = nullptr;
}

if (oldPinnedWrapperQueue) {
    HookLogImportant("%s — releasing PostSL pinned SL wrapper queue %p", reason, oldPinnedWrapperQueue);
    oldPinnedWrapperQueue->Release();
}
}


void DetachPostSLQueuesLocked(ID3D12CommandQueue** lockedQueueOut, ID3D12CommandQueue** dedicatedQueueOut) {
if (lockedQueueOut) {
    *lockedQueueOut = nullptr;
}
if (dedicatedQueueOut) {
    *dedicatedQueueOut = nullptr;
}

std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
if (lockedQueueOut) {
    *lockedQueueOut = dx12_hook_g_PostSLLockedQueue;
}
if (dedicatedQueueOut) {
    *dedicatedQueueOut = dx12_hook_g_PostSLDedicatedQueue;
}
dx12_hook_g_PostSLLockedQueue = nullptr;
dx12_hook_g_PostSLDedicatedQueue = nullptr;
}


void ReleaseDetachedPostSLQueues(const char* reason, ID3D12CommandQueue* lockedQueue, ID3D12CommandQueue* dedicatedQueue) {
if (lockedQueue) {
    HookLogImportant("%s — releasing PostSL locked queue %p", reason, lockedQueue);
    lockedQueue->Release();
}

if (dedicatedQueue) {
    HookLogImportant("%s — releasing PostSL dedicated queue %p", reason, dedicatedQueue);
    dedicatedQueue->Release();
}
}


void ClearPostSLQueues(const char* reason) {
ID3D12CommandQueue* oldLockedQueue = nullptr;
ID3D12CommandQueue* oldDedicatedQueue = nullptr;
DetachPostSLQueuesLocked(&oldLockedQueue, &oldDedicatedQueue);
ReleaseDetachedPostSLQueues(reason, oldLockedQueue, oldDedicatedQueue);
}


void CleanupDeferredPostSLQueuesIfSafe(const char* reason) {
ID3D12CommandQueue* deferredLockedQueue =
    dx12_hook_g_DeferredPostSLLockedQueueRelease.exchange(nullptr, std::memory_order_acq_rel);
if (deferredLockedQueue) {
    HookLogImportant("%s - releasing deferred PostSL locked queue %p", reason, deferredLockedQueue);
    deferredLockedQueue->Release();
}

ID3D12CommandQueue* deferredCommandQueue =
    dx12_hook_g_DeferredCommandQueueRelease.exchange(nullptr, std::memory_order_acq_rel);
if (deferredCommandQueue) {
    HookLogImportant("%s - releasing deferred stale command queue %p", reason, deferredCommandQueue);
    deferredCommandQueue->Release();
}

if (!dx12_hook_g_PostSLDeferredQueueCleanupPending.load(std::memory_order_acquire)) {
    return;
}

if (DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) {
    return;
}

if (dx12_hook_g_PostSLCallbackInFlight.load(std::memory_order_acquire) != 0) {
    return;
}

if (!dx12_hook_g_PostSLDeferredQueueCleanupPending.exchange(false, std::memory_order_acq_rel)) {
    return;
}

ID3D12CommandQueue* oldLockedQueue = nullptr;
ID3D12CommandQueue* oldDedicatedQueue = nullptr;
DetachPostSLQueuesLocked(&oldLockedQueue, &oldDedicatedQueue);

if (oldLockedQueue) {
    ID3D12CommandQueue* previouslyDeferred =
        dx12_hook_g_DeferredPostSLLockedQueueRelease.exchange(oldLockedQueue, std::memory_order_acq_rel);
    if (previouslyDeferred) {
        HookLogImportant("%s - releasing superseded deferred PostSL locked queue %p", reason, previouslyDeferred);
        previouslyDeferred->Release();
    }
    HookLogImportant("%s - deferred PostSL locked queue release %p", reason, oldLockedQueue);
}
if (oldDedicatedQueue) {
    HookLogImportant("%s — releasing PostSL dedicated queue %p", reason, oldDedicatedQueue);
    oldDedicatedQueue->Release();
}

RealignInactiveCommandQueueToSwapchainQueue(reason);
}


void MarkPostSLRecentTeardownActivity(const char* reason, ID3D12CommandQueue* queue) {
if (!queue) {
    return;
}

constexpr ULONGLONG kPostSLRecentTeardownActivityMs = 250;
dx12_hook_g_PostSLRecentTeardownActivityUntilMs.store(GetTickCount64() + kPostSLRecentTeardownActivityMs,
                                            std::memory_order_release);
static std::atomic<int> s_postSLRecentTeardownLogCount{0};
const int logCount = s_postSLRecentTeardownLogCount.fetch_add(1, std::memory_order_relaxed);
if (logCount < 10 || (logCount % 128) == 0) {
    HookLogImportant("%s - marking PostSL queue %p as recently active during Streamline teardown (%llums)", reason,
                     queue, (unsigned long long)kPostSLRecentTeardownActivityMs);
}
}


void InvalidateAllOverlayCachedFrames() {
g_OverlayAdapter.InvalidateCachedFrame();
dx12_hook_g_D3D11On12Adapter.InvalidateCachedFrame();
dx12_hook_g_SLFGAdapter.InvalidateCachedFrame();
}


void ResetPostSLLifecycleForTransition(const char* reason, bool clearRealQueueBehindSLWrapper, bool deferQueueReleaseUntilCallbacksDrain) {
dx12_hook_g_PostSLLifecycleEpoch.fetch_add(1, std::memory_order_acq_rel);
dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
dx12_hook_g_LastSuccessfulPostSLSwapchain.store(nullptr, std::memory_order_release);

if (deferQueueReleaseUntilCallbacksDrain) {
    SetPostSLCallbackInstalled(false, reason);
    WaitForInFlightPostSLCallbacks(reason);
    WaitForOverlayGpuIdle(reason);
    dx12_hook_g_PostSLDeferredQueueCleanupPending.store(true, std::memory_order_release);
} else {
    dx12_hook_g_PostSLDeferredQueueCleanupPending.store(false, std::memory_order_release);
    ClearPostSLQueues(reason);
}

ClearPostSLPinnedSLWrapperQueue(reason);

if (clearRealQueueBehindSLWrapper) {
    ID3D12CommandQueue* oldRealQueue = dx12_hook_g_RealQueueBehindSLWrapper.exchange(nullptr, std::memory_order_acq_rel);
    if (oldRealQueue) {
        HookLogImportant("%s — cleared cached real queue behind SL wrapper %p", reason, oldRealQueue);
    }
}
}


void RealignInactiveCommandQueueToSwapchainQueue(const char* reason) {
ID3D12CommandQueue* oldCommandQueue = nullptr;
ID3D12CommandQueue* swapchainQueue = nullptr;
ID3D12CommandQueue* originalGameQueue = nullptr;
bool realignedCommandQueue = false;
{
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    swapchainQueue = dx12_hook_g_SwapchainQueue;
    originalGameQueue = dx12_hook_g_OriginalGameQueue;
    ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
    bool actualFGActive = IsActualFrameGenerationActive();
    bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    if (ce::dx12_overlay_policy::ShouldRealignInactiveCommandQueueToSwapchainQueue(
            actualFGActive, streamlineFGRunning, swapchainQueue != nullptr, originalGameQueue != nullptr,
            currentCommandQueue != nullptr, currentCommandQueue == swapchainQueue,
            currentCommandQueue == originalGameQueue,
            currentCommandQueue == dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire))) {
        oldCommandQueue = currentCommandQueue;
        g_CommandQueue.store(swapchainQueue, std::memory_order_release);
        swapchainQueue->AddRef();
        realignedCommandQueue = true;
    }
}

if (realignedCommandQueue) {
    HookLogImportant("%s - realigned stale command queue %p -> swapchain queue %p (origGame=%p)", reason,
                     oldCommandQueue, swapchainQueue, originalGameQueue);
    if (oldCommandQueue) {
        ID3D12CommandQueue* previouslyDeferred =
            dx12_hook_g_DeferredCommandQueueRelease.exchange(oldCommandQueue, std::memory_order_acq_rel);
        if (previouslyDeferred) {
            HookLogImportant("%s - releasing superseded deferred stale command queue %p", reason,
                             previouslyDeferred);
            previouslyDeferred->Release();
        }
    }
}
}


void MarkForwardedCreateSwapchainForHwndInlineSideEffectsHandled() {
if (dx12_hook_s_forwardedCreateSwapchainForHwndInlineDepth <= 0) {
    return;
}
dx12_hook_s_forwardedCreateSwapchainForHwndInlineHandled = true;
}


bool TryGetModulePathFromCodeAddress(const void* codeAddress, char* modulePathOut, size_t modulePathOutCount, HMODULE* moduleHandleOut) {
if (moduleHandleOut) {
    *moduleHandleOut = nullptr;
}
if (modulePathOut && modulePathOutCount > 0) {
    modulePathOut[0] = '\0';
}
if (!codeAddress) {
    return false;
}

HMODULE callerModule = nullptr;
if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCSTR>(codeAddress), &callerModule) ||
    !callerModule) {
    return false;
}

if (moduleHandleOut) {
    *moduleHandleOut = callerModule;
}
if (modulePathOut && modulePathOutCount > 0) {
    GetModuleFileNameA(callerModule, modulePathOut, static_cast<DWORD>(modulePathOutCount));
}
return true;
}


// ===================== DX12 API call trace diagnostic =====================
// Gated by env CE_DX12_TRACE=1 or a flag file "ce_dx12_trace" next to the hook DLL. When enabled, logs
// caller-attributed D3D12 device/queue calls (CreateCommandQueue / CreateCommittedResource /
// CreateDescriptorHeap / CreateSwapChain[ForHwnd] / ExecuteCommandLists / Signal) so the overlay's --
// and any co-resident injected module's -- interaction with the app's D3D12 device can be inspected
// (queue usage, resource/descriptor footprint, per-frame submission/fence pattern). This is a
// diagnostic aid for focus/mode-switch and overlay-coexistence investigations. Zero impact when
// disabled: the extra vtable hooks are not installed and no logging runs.
bool Dx12TraceEnabled() {
static const bool s_enabled = []() -> bool {
    char buf[16] = {};
    DWORD n = GetEnvironmentVariableA("CE_DX12_TRACE", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        if (buf[0] == '1' || _stricmp(buf, "on") == 0 || _stricmp(buf, "true") == 0 || _stricmp(buf, "yes") == 0) {
            return true;
        }
    }
    HMODULE self = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&Dx12TraceEnabled), &self) &&
        self) {
        char path[MAX_PATH] = {};
        DWORD len = GetModuleFileNameA(self, path, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            for (DWORD i = len; i > 0; --i) {
                if (path[i - 1] == '\\' || path[i - 1] == '/') {
                    path[i] = '\0';
                    break;
                }
            }
            char flagPath[MAX_PATH] = {};
            _snprintf_s(flagPath, sizeof(flagPath), _TRUNCATE, "%sce_dx12_trace", path);
            HANDLE h = CreateFileA(flagPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
                return true;
            }
        }
    }
    return false;
}();
return s_enabled;
}


bool Dx12TraceIsInfraModule(const char* base) {
return _stricmp(base, "capture_hook_x86.dll") == 0 || _stricmp(base, "capture_hook_x64.dll") == 0 ||
       _stricmp(base, "d3d12.dll") == 0 || _stricmp(base, "d3d12core.dll") == 0 ||
       _stricmp(base, "dxgi.dll") == 0 || _strnicmp(base, "nvwgf2um", 8) == 0 ||
       _stricmp(base, "kernelbase.dll") == 0 || _stricmp(base, "kernel32.dll") == 0 ||
       _stricmp(base, "ntdll.dll") == 0 || _stricmp(base, "win32u.dll") == 0;
}


// Capture the call stack, identify the originating module (first non-infra frame), and log a compact
// module trail. The trail + the queue/resource pointers in `details` are the ground truth for analysis
// (e.g. correlate ExecuteCommandLists/Signal queue pointers with the queue a given module created).
void Dx12TraceLog(const char* api, const char* details) {
constexpr USHORT kMaxFrames = 24;
void* frames[kMaxFrames] = {};
const USHORT frameCount = CaptureStackBackTrace(1, kMaxFrames, frames, nullptr);
char originator[64] = "?";
bool foundOriginator = false;
char trail[256] = {};
size_t trailLen = 0;
char lastBase[64] = {};
for (USHORT i = 0; i < frameCount; ++i) {
    char modPath[MAX_PATH] = {};
    if (!TryGetModulePathFromCodeAddress(frames[i], modPath, sizeof(modPath))) {
        continue;  // trampoline / non-module code region
    }
    const char* slash = strrchr(modPath, '\\');
    const char* base = slash ? slash + 1 : modPath;
    if (!foundOriginator && !Dx12TraceIsInfraModule(base)) {
        _snprintf_s(originator, sizeof(originator), _TRUNCATE, "%s", base);
        foundOriginator = true;
    }
    if (_stricmp(base, lastBase) != 0) {  // collapse consecutive duplicate frames
        int w =
            _snprintf_s(trail + trailLen, sizeof(trail) - trailLen, _TRUNCATE, "%s%s", trailLen ? ">" : "", base);
        if (w > 0) {
            trailLen += static_cast<size_t>(w);
        }
        _snprintf_s(lastBase, sizeof(lastBase), _TRUNCATE, "%s", base);
    }
    if (trailLen + 16 >= sizeof(trail)) {
        break;
    }
}
HookLogImportant("DX12 TRACE: %s orig=%s | %s | trail=%s", api, originator, details ? details : "", trail);
}


bool IsCurrentECLCallerFromThirdPartyOverlay(char* modulePathOut, size_t modulePathOutCount) {
if (modulePathOut && modulePathOutCount > 0) {
    modulePathOut[0] = '\0';
}

const void* callerAddress = CE_RETURN_ADDRESS();
if (!callerAddress) {
    return false;
}

char localModulePath[MAX_PATH] = {};
char* targetBuffer = (modulePathOut && modulePathOutCount > 0) ? modulePathOut : localModulePath;
const size_t targetCount = (modulePathOut && modulePathOutCount > 0) ? modulePathOutCount : sizeof(localModulePath);
if (!TryGetModulePathFromCodeAddress(callerAddress, targetBuffer, targetCount)) {
    return false;
}

return ce::overlay_compat::IsThirdPartyOverlayModulePath(targetBuffer);
}


CreateSwapchainQueueCaptureEvidence BuildCreateSwapchainQueueCaptureEvidence(const void* callerAddress, bool callerFromThirdPartyOverlay, bool callerFromFFXFGModule, bool ffxFrameGenerationInStack, bool callerFromStreamlineFGModule, bool streamlineFrameGenerationInStack, const char* callerModulePath, const char* ffxModulePath) {
CreateSwapchainQueueCaptureEvidence evidence = {};
evidence.callerAddress = callerAddress;
evidence.callerFromThirdPartyOverlay = callerFromThirdPartyOverlay;
evidence.authoritativeFFXRuntimeCreator =
    ce::dx12_overlay_policy::ShouldTreatCreateSwapchainCallerAsAuthoritativeFFX(callerFromFFXFGModule,
                                                                                ffxFrameGenerationInStack);
evidence.authoritativeStreamlineRuntimeCreator = callerFromStreamlineFGModule || streamlineFrameGenerationInStack;
evidence.callerFromStreamlineFGModule = callerFromStreamlineFGModule;
evidence.streamlineFrameGenerationInStack = streamlineFrameGenerationInStack;
if (callerModulePath && *callerModulePath) {
    strncpy_s(evidence.callerModulePath, sizeof(evidence.callerModulePath), callerModulePath, _TRUNCATE);
}
const char* authoritativeFFXPath = (ffxModulePath && *ffxModulePath)
                                       ? ffxModulePath
                                       : (callerFromFFXFGModule && callerModulePath ? callerModulePath : nullptr);
if (authoritativeFFXPath && *authoritativeFFXPath) {
    strncpy_s(evidence.ffxModulePath, sizeof(evidence.ffxModulePath), authoritativeFFXPath, _TRUNCATE);
    evidence.officialAMDFFXRuntimeCreator = ce::ffx_api::IsOfficialAMDFFXRuntimeModuleName(authoritativeFFXPath);
}
return evidence;
}


CreateSwapchainForHwndCallerContext ResolveCreateSwapchainForHwndCallerContext() {
CreateSwapchainForHwndCallerContext context = {};

char immediateCallerModulePath[MAX_PATH] = {};
const void* immediateCallerAddress = CE_RETURN_ADDRESS();
TryGetModulePathFromCodeAddress(immediateCallerAddress, immediateCallerModulePath,
                                sizeof(immediateCallerModulePath));

const char* effectiveCallerModulePath = ce::overlay_compat::GetEffectiveCreateSwapchainCallerModulePath(
    dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext.callerModulePath, immediateCallerModulePath);
if (effectiveCallerModulePath && *effectiveCallerModulePath) {
    strncpy_s(context.callerModulePath, sizeof(context.callerModulePath), effectiveCallerModulePath, _TRUNCATE);
}

context.callerAddress = dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext.callerModulePath[0]
                            ? dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext.callerAddress
                            : immediateCallerAddress;
context.callerFromFFXFGModule = ce::overlay_compat::IsFFXFrameGenerationModulePath(context.callerModulePath);
context.callerFromThirdPartyOverlay = ce::overlay_compat::IsEffectiveCreateSwapchainCallerFromThirdPartyOverlay(
    dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext.callerModulePath, immediateCallerModulePath);
return context;
}


bool ShouldTreatCreateSwapchainCallerAsThirdPartyOverlay(const char* context, bool rawCallerFromThirdPartyOverlay, bool callerFromFFXFGModule, bool ffxFrameGenerationInStack, bool callerFromStreamlineFGModule, bool streamlineFrameGenerationInStack, const char* callerModulePath) {
const bool authoritativeFGRuntimeSwapchainCreator =
    ce::dx12_overlay_policy::ShouldTreatCreateSwapchainCallerAsAuthoritativeFrameGenerationRuntime(
        callerFromFFXFGModule, ffxFrameGenerationInStack, callerFromStreamlineFGModule,
        streamlineFrameGenerationInStack);
if (rawCallerFromThirdPartyOverlay && authoritativeFGRuntimeSwapchainCreator) {
    static std::atomic<int> s_wrappedFGCreateCallerLogCount{0};
    const int logCount = s_wrappedFGCreateCallerLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20) {
        const char* runtimeKind = (callerFromFFXFGModule || ffxFrameGenerationInStack) ? "FFX" : "Streamline";
        if (callerFromStreamlineFGModule || callerFromFFXFGModule || ffxFrameGenerationInStack) {
            HookLogImportant(
                "%s: %s frame-generation stack detected behind third-party overlay caller %s — treating "
                "swapchain as authoritative runtime takeover",
                context ? context : "CreateSwapChain", runtimeKind,
                callerModulePath && *callerModulePath ? callerModulePath : "unknown");
        } else {
            HookLogImportant(
                "%s: Streamline stack detected behind third-party overlay caller %s — deferring takeover "
                "classification until queue identity is known",
                context ? context : "CreateSwapChain",
                callerModulePath && *callerModulePath ? callerModulePath : "unknown");
        }
    }
}

return rawCallerFromThirdPartyOverlay && !authoritativeFGRuntimeSwapchainCreator;
}


bool ShouldUseProtectedOfficialFFXStartupSwapchainCreatePath(const CreateSwapchainQueueCaptureEvidence& captureEvidence) {
return ce::dx12_overlay_policy::ShouldProtectOfficialFFXStartupSwapchainCreateFromCESideEffects(
    captureEvidence.authoritativeFFXRuntimeCreator, captureEvidence.officialAMDFFXRuntimeCreator,
    HasResolvedOfficialFFXStartupPath());
}


void StageProtectedOfficialFFXStartupQueueFromCreateDevice(IUnknown* createDevice, const CreateSwapchainQueueCaptureEvidence& captureEvidence, const char* context) {
ID3D12CommandQueue* queue = nullptr;
HRESULT qiHr = E_POINTER;
if (createDevice) {
    qiHr = createDevice->QueryInterface(IID_PPV_ARGS(&queue));
}

bool hasDirectQueue = false;
D3D12_COMMAND_QUEUE_DESC queueDesc = {};
if (SUCCEEDED(qiHr) && queue) {
    queueDesc = queue->GetDesc();
    hasDirectQueue = queueDesc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT;
}

const bool shouldStage =
    ce::dx12_overlay_policy::ShouldStageProtectedOfficialFFXStartupQueueForDeferredTakeover(true, hasDirectQueue);
const char* modulePath =
    captureEvidence.ffxModulePath[0] ? captureEvidence.ffxModulePath : captureEvidence.callerModulePath;
if (shouldStage) {
    StoreDeferredOfficialFFXTakeoverSideEffects(queue,
                                                modulePath && modulePath[0] ? modulePath : "official FFX runtime",
                                                "protected official FFX swapchain create queue staging");

    HookLogImportant(
        "%s: Protected official FFX startup staged runtime queue %p until enabled ffxConfigure "
        "(module=%s caller=%s)",
        context && context[0] ? context : "CreateSwapChain", queue,
        modulePath && modulePath[0] ? modulePath : "unknown",
        captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "stack");
} else {
    static std::atomic<int> s_stageQueueFailLogCount{0};
    const int logCount = s_stageQueueFailLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 300) == 0) {
        HookLogImportant(
            "%s: Protected official FFX startup could not stage runtime queue "
            "(createDevice=%p queue=%p qiHr=0x%08X queueType=%d module=%s log=%d)",
            context && context[0] ? context : "CreateSwapChain", createDevice, queue, (unsigned)qiHr,
            queue ? static_cast<int>(queueDesc.Type) : -1, modulePath && modulePath[0] ? modulePath : "unknown",
            logCount + 1);
    }
}

if (queue) {
    queue->Release();
}
}


bool ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup() {
return ce::dx12_overlay_policy::ShouldQuiesceCESideEffectsDuringProtectedOfficialFFXStartup(
    dx12_hook_g_ProtectedOfficialFFXStartupSwapchainPending.load(std::memory_order_acquire),
    HasResolvedOfficialFFXStartupPath());
}


bool ShouldDeferPresentHookRefreshForPostFSRStreamlineRuntimeHandoff(IUnknown* pDevice, const CreateSwapchainQueueCaptureEvidence& captureEvidence, ID3D12CommandQueue** queueOut) {
if (queueOut) {
    *queueOut = nullptr;
}
if (!pDevice) {
    return false;
}

ID3D12CommandQueue* pQueue = nullptr;
if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&pQueue))) || !pQueue) {
    return false;
}

ID3D12CommandQueue* originalGameQueue = nullptr;
{
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    originalGameQueue = dx12_hook_g_OriginalGameQueue;
}
const bool streamlineRuntimeAvailable = IsStreamlineLoaded() || g_FGCompat.HasStreamlineSupport() ||
                                        DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) ||
                                        captureEvidence.authoritativeStreamlineRuntimeCreator;
const bool deferRefresh = ce::dx12_overlay_policy::ShouldDeferPresentHookRefreshForPostFSRStreamlineRuntimeHandoff(
    originalGameQueue != nullptr, pQueue == originalGameQueue, streamlineRuntimeAvailable, dx12_hook_g_HadFSRFGPhase,
    g_FGCompat.IsFSRFGApiActive(), g_FGCompat.GetRuntimeMode());
if (deferRefresh && queueOut) {
    *queueOut = pQueue;
} else {
    pQueue->Release();
}
return deferRefresh;
}


bool ShouldApplySwapchainDescriptorOverridesForCreate(const CreateSwapchainQueueCaptureEvidence& captureEvidence) {
return ce::dx12_overlay_policy::ShouldApplySwapchainDescriptorOverridesForCreate(
    captureEvidence.callerFromThirdPartyOverlay,
    captureEvidence.authoritativeFFXRuntimeCreator || captureEvidence.authoritativeStreamlineRuntimeCreator);
}


void PrepareForAuthoritativeFFXSwapchainCreate(const CreateSwapchainQueueCaptureEvidence& captureEvidence, const char* context) {
if (!ce::dx12_overlay_policy::ShouldReleaseRetainedStreamlineStartupActivationSwapchainForAuthoritativeFFXCreate(
        captureEvidence.authoritativeFFXRuntimeCreator, HasRetainedStreamlineStartupActivationSwapchain())) {
    return;
}

HookLogImportant(
    "%s: Authoritative FFX swapchain create is replacing a Streamline startup handoff — releasing retained "
    "Streamline activation swapchain before DXGI CreateSwapChainForHwnd to avoid stale HWND references "
    "(ffxModule=%s caller=%s)",
    context && context[0] ? context : "CreateSwapChainForHwnd",
    captureEvidence.ffxModulePath[0] ? captureEvidence.ffxModulePath : "unknown",
    captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "stack");
ReleaseStreamlineStartupActivationSwapchain("DX12: authoritative FFX swapchain create");
}


void LogSkippedSwapchainDescriptorOverridesForRuntimeCreate(const char* context, const CreateSwapchainQueueCaptureEvidence& captureEvidence, UINT bufferCount, UINT flags, DXGI_SWAP_EFFECT swapEffect) {
if (!captureEvidence.authoritativeFFXRuntimeCreator && !captureEvidence.authoritativeStreamlineRuntimeCreator) {
    return;
}

static std::atomic<int> s_runtimeDescriptorPassthroughLogCount{0};
const int logCount = s_runtimeDescriptorPassthroughLogCount.fetch_add(1, std::memory_order_relaxed);
if (logCount < 20 || (logCount % 128) == 0) {
    HookLogImportant(
        "%s: Preserving swapchain descriptor for authoritative FG runtime create "
        "(ffx=%d officialFFX=%d streamline=%d caller=%s BufferCount=%u Flags=0x%X SwapEffect=%d count=%d)",
        context && context[0] ? context : "CreateSwapChain", captureEvidence.authoritativeFFXRuntimeCreator ? 1 : 0,
        captureEvidence.officialAMDFFXRuntimeCreator ? 1 : 0,
        captureEvidence.authoritativeStreamlineRuntimeCreator ? 1 : 0,
        captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "stack", bufferCount, flags,
        static_cast<int>(swapEffect), logCount + 1);
}
}


bool ShouldBypassInvisibleWindowCreateSwapchainSideEffects(HWND hWnd, IDXGISwapChain* swapchain, const char* context, HRESULT hr) {
if (FAILED(hr) || !swapchain || !hWnd) {
    return false;
}

const bool outputWindowVisible = IsWindowVisible(hWnd) != FALSE;
if (!ce::dx12_overlay_policy::ShouldSkipDX12CreateSwapchainSideEffectsForInvisibleWindowSwapchain(
        true, outputWindowVisible)) {
    return false;
}

static std::atomic<int> s_invisibleWindowCreateSkipLogCount{0};
const int logCount = s_invisibleWindowCreateSkipLogCount.fetch_add(1, std::memory_order_relaxed);
if (logCount < 20 || (logCount % 128) == 0) {
    HookLogImportant(
        "%s: Invisible-window swapchain %p for HWND=%p — bypassing CE swapchain side-effects "
        "(queue capture, Present refresh, cooldown, wrapper decisions skipped; hr=0x%08X count=%d)",
        context && context[0] ? context : "CreateSwapChainForHwnd", swapchain, hWnd, hr, logCount + 1);
}
return true;
}


void QuiesceStreamlinePostSLForProtectedOfficialFFXStartup(IDXGISwapChain* swapchain, const CreateSwapchainQueueCaptureEvidence& captureEvidence, const char* context) {
const bool callbackInstalled = DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr;
const bool postSLActive = dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire);
const bool postSLConfirmed = dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
const bool startupActivationPending =
    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
if (!ce::dx12_overlay_policy::ShouldQuiesceStreamlinePostSLDuringProtectedOfficialFFXStartup(
        true, HasResolvedOfficialFFXStartupPath(), callbackInstalled, postSLActive, postSLConfirmed,
        streamlineFGRunning, startupActivationPending)) {
    return;
}

const char* source = context && context[0] ? context : "protected official FFX startup";
SetPostSLCallbackInstalled(false, "DX12: protected official FFX startup");
const bool staleStreamlineSignal = DXGIShared::g_StreamlineFGRunning.exchange(false, std::memory_order_acq_rel);
g_FGCompat.SetStreamlineFGSignal(false);
g_FGCompat.SetDLSSFGActive(false);
dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
dx12_hook_g_PostSLStallCounter.store(0, std::memory_order_release);
dx12_hook_g_PostSLStableFrameCount.store(0, std::memory_order_release);
dx12_hook_g_PostSLRuntimeStateStabilizationLogged.store(false, std::memory_order_release);
dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
dx12_hook_g_PostSLSyntheticStartupTakeoverLogged.store(false, std::memory_order_release);
ResetPostSLLifecycleForTransition("DX12: protected official FFX startup", true, true);
ReleaseStreamlineStartupActivationSwapchain("DX12: protected official FFX startup");
StreamlineHook::OnAuthoritativeFFXTakeover();
DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
DXGIShared::ResetStreamlineStartupTransitionState();
DXGIShared::DisableSLPresentRouting();

HookLogImportant(
    "%s: Protected official FFX startup immediately quiesced Streamline/PostSL before AMD swapchain takeover "
    "(sc=%p callback=%d active=%d confirmed=%d startupPending=%d staleSL=%d module=%s caller=%s)",
    source, swapchain, callbackInstalled ? 1 : 0, postSLActive ? 1 : 0, postSLConfirmed ? 1 : 0,
    startupActivationPending ? 1 : 0, staleStreamlineSignal ? 1 : 0,
    captureEvidence.ffxModulePath[0] ? captureEvidence.ffxModulePath : "unknown",
    captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "stack");
}


bool HandleProtectedOfficialFFXStartupSwapchainCreate(const CreateSwapchainQueueCaptureEvidence& captureEvidence, IUnknown* createDevice, IDXGISwapChain* swapchain, const char* context) {
if (!ShouldUseProtectedOfficialFFXStartupSwapchainCreatePath(captureEvidence)) {
    return false;
}

g_FGCompat.SetFSRFGSupportPresent(true);
g_FGCompat.SetFSRFGMultiplier(2);
ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
SetNativeFSRStartupConfigureArmingPending(true, "protected official FFX swapchain create");
dx12_hook_g_ProtectedOfficialFFXStartupSwapchainPending.store(true, std::memory_order_release);
ArmProtectedOfficialFFXStartupProgressTracking("protected official FFX swapchain create");
ResetAuthoritativeFSRRealFrameOnlyStreak();
if (!dx12_hook_g_HadFSRFGPhase) {
    dx12_hook_g_HadFSRFGPhase = true;
    HookLogImportant(
        "DX12: Protected official FFX swapchain create implies FSR FG history — latching post-FSR handoff state");
}

StageProtectedOfficialFFXStartupQueueFromCreateDevice(createDevice, captureEvidence, context);
QuiesceStreamlinePostSLForProtectedOfficialFFXStartup(swapchain, captureEvidence, context);

HookLogImportant(
    "DX12: Protected official FFX startup swapchain pass-through via %s (sc=%p module=%s caller=%s) — "
    "deferring Present hook refresh, queue ownership, FFX export inspection, and heavy takeover side effects "
    "until enabled ffxConfigure; live Streamline/PostSL routing was quiesced immediately when present",
    context && context[0] ? context : "CreateSwapChain", swapchain,
    captureEvidence.ffxModulePath[0] ? captureEvidence.ffxModulePath : "unknown",
    captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "stack");
return true;
}

