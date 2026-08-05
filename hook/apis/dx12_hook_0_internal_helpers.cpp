#include "dx12_hook_internal.h"


const char* DX12OverlayRenderRouteName(uint32_t route) {
switch (static_cast<DX12OverlayRenderRoute>(route)) {
    case DX12OverlayRenderRoute::kNormal:
        return "normal";
    case DX12OverlayRenderRoute::kPostSL:
        return "post-sl";
    case DX12OverlayRenderRoute::kFFXPresentCallback:
        return "ffx-present-callback";
    case DX12OverlayRenderRoute::kStreamlineUI:
        return "streamline-ui";
    default:
        return "none";
}
}


void NoteDX12OverlayCoverageGate(const char* gate) {
dx12_hook_g_OverlayCoverageLastGate.store(gate, std::memory_order_relaxed);
}


DX12OverlayCoverageSnapshot GetOverlayCoverageSnapshot() {
DX12OverlayCoverageSnapshot snapshot;
while (dx12_hook_g_OverlayCoverageLock.test_and_set(std::memory_order_acquire)) {
    YieldProcessor();
}
snapshot.totalPresents = dx12_hook_g_OverlayCoverageTracker.TotalPresents();
snapshot.uncoveredPresents = dx12_hook_g_OverlayCoverageTracker.UncoveredPresents();
snapshot.currentStreak = dx12_hook_g_OverlayCoverageTracker.CurrentUncoveredStreak();
snapshot.longestStreak = dx12_hook_g_OverlayCoverageTracker.LongestUncoveredStreak();
dx12_hook_g_OverlayCoverageLock.clear(std::memory_order_release);
return snapshot;
}


// Accounts one presented frame. covered = draw-counter delta since the previous
// accounted present (any route), with FG-composed inheritance (see block comment).
void AccountPresentForOverlayCoverage(bool inheritCoverageIfNoDraw, const char* source) {
const uint64_t draws = dx12_hook_g_OverlayCoverageDrawCount.load(std::memory_order_acquire);
// Visibility cannot be interrupted before CE has established its first
// visible overlay draw. Excluding pre-initialization Presents keeps later
// transition summaries and interruption markers semantically precise.
if (!ce::dx12_overlay_policy::ShouldAccountOverlayVisibilityPresent(draws)) {
    return;
}
const uint64_t lastSeen = dx12_hook_g_OverlayCoverageLastSeenDrawCount.exchange(draws, std::memory_order_acq_rel);
const bool drawObserved = draws != lastSeen;

ce::dx12_overlay_policy::OverlayPresentCoverageResult result;
DX12OverlayCoverageSnapshot snapshot;
while (dx12_hook_g_OverlayCoverageLock.test_and_set(std::memory_order_acquire)) {
    YieldProcessor();
}
result = dx12_hook_g_OverlayCoverageTracker.NotePresent(drawObserved, inheritCoverageIfNoDraw);
snapshot.totalPresents = dx12_hook_g_OverlayCoverageTracker.TotalPresents();
snapshot.uncoveredPresents = dx12_hook_g_OverlayCoverageTracker.UncoveredPresents();
snapshot.currentStreak = dx12_hook_g_OverlayCoverageTracker.CurrentUncoveredStreak();
snapshot.longestStreak = dx12_hook_g_OverlayCoverageTracker.LongestUncoveredStreak();
dx12_hook_g_OverlayCoverageLock.clear(std::memory_order_release);

// Verbose overlay-handoff diagnostic: per-present detail for the first N presents after a PostSL
// reactivation. `drawObserved=0 inheritIfNoDraw=1` (covered ONLY by FG-composed inheritance) is the
// smoking gun for an off->DLSS fresh-proxy flash — DLSS-G presented a generated frame relying on a
// proxy whose overlay history is still empty. A real draw shows `drawObserved=1`.
{
    int verboseRemaining = dx12_hook_g_OverlayHandoffVerboseLogPresents.load(std::memory_order_relaxed);
    if (verboseRemaining > 0) {
        dx12_hook_g_OverlayHandoffVerboseLogPresents.store(verboseRemaining - 1, std::memory_order_relaxed);
        const uint32_t route = dx12_hook_g_LastDX12OverlayRenderRoute.load(std::memory_order_acquire);
        const uint32_t prevRoute = dx12_hook_g_OverlayHandoffVerbosePrevRoute.load(std::memory_order_relaxed);
        HookLogImportant(
            "[OVERLAY HANDOFF] present=%llu drawObserved=%d inheritIfNoDraw=%d covered=%d route=%s prevRoute=%s "
            "source=%s currentStreak=%llu remaining=%d",
            static_cast<unsigned long long>(snapshot.totalPresents), drawObserved ? 1 : 0,
            inheritCoverageIfNoDraw ? 1 : 0, (drawObserved || inheritCoverageIfNoDraw) ? 1 : 0,
            DX12OverlayRenderRouteName(route), DX12OverlayRenderRouteName(prevRoute), source ? source : "unknown",
            static_cast<unsigned long long>(snapshot.currentStreak), verboseRemaining - 1);
    }
}

if (result.uncoveredStreakStarted) {
    const char* streakGate = dx12_hook_g_OverlayCoverageLastGate.load(std::memory_order_relaxed);
    dx12_hook_g_OverlayCoverageStreakGate.store(streakGate, std::memory_order_relaxed);
    const uint64_t startTick = GetTickCount64();
    dx12_hook_g_OverlayCoverageStreakStartTickMs.store(startTick, std::memory_order_relaxed);
    const bool startConfirmed = dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
    dx12_hook_g_OverlayCoverageStreakStartConfirmed.store(startConfirmed, std::memory_order_relaxed);
    // Bracket the onset of every blank window with a timestamped marker so even a
    // single-present gap is fully attributable from the log alone.
    static std::atomic<int> s_streakStartLogCount{0};
    const int startLogCount = s_streakStartLogCount.fetch_add(1, std::memory_order_relaxed);
    if (startLogCount < 100 || (startLogCount % 20) == 0) {
        const uint32_t route = dx12_hook_g_LastDX12OverlayRenderRoute.load(std::memory_order_acquire);
        HookLogImportant(
            "[OVERLAY COVERAGE] [OVERLAY VISIBILITY] INTERRUPTED/UNPROVEN: no overlay draw belongs to the "
            "current presentation route (gate=%s route=%s source=%s confirmed=%d present=%llu uncovered=%llu)",
            streakGate ? streakGate : "unknown", DX12OverlayRenderRouteName(route), source ? source : "unknown",
            startConfirmed ? 1 : 0, static_cast<unsigned long long>(snapshot.totalPresents),
            static_cast<unsigned long long>(snapshot.uncoveredPresents));
    }
}
if (result.uncoveredStreakEnded) {
    static std::atomic<int> s_streakEndLogCount{0};
    const int logCount = s_streakEndLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 100 || (logCount % 20) == 0) {
        const char* streakGate = dx12_hook_g_OverlayCoverageStreakGate.load(std::memory_order_relaxed);
        const char* lastGate = dx12_hook_g_OverlayCoverageLastGate.load(std::memory_order_relaxed);
        const uint32_t route = dx12_hook_g_LastDX12OverlayRenderRoute.load(std::memory_order_acquire);
        const uint64_t startTick = dx12_hook_g_OverlayCoverageStreakStartTickMs.load(std::memory_order_relaxed);
        const uint64_t durationMs = startTick ? (GetTickCount64() - startTick) : 0;
        const bool confirmedDuringStreak = dx12_hook_g_OverlayCoverageStreakStartConfirmed.load(std::memory_order_relaxed);
        HookLogImportant(
            "[OVERLAY COVERAGE] [OVERLAY VISIBILITY] RESTORED after uncovered route: missed=%llu durationMs=%llu "
            "confirmedDuringStreak=%d longestStreak=%llu gate=%s lastGate=%s route=%s source=%s totals: "
            "presents=%llu uncovered=%llu",
            static_cast<unsigned long long>(result.endedStreakLength), static_cast<unsigned long long>(durationMs),
            confirmedDuringStreak ? 1 : 0, static_cast<unsigned long long>(snapshot.longestStreak),
            streakGate ? streakGate : "unknown", lastGate ? lastGate : "unknown", DX12OverlayRenderRouteName(route),
            source ? source : "unknown", static_cast<unsigned long long>(snapshot.totalPresents),
            static_cast<unsigned long long>(snapshot.uncoveredPresents));
    }
}
}


// Logs a coverage summary line. Called at FG transition edges and shutdown so
// the scripted transition matrix can gate on "no uncovered streak > 1 present".
void LogOverlayCoverageSummary(const char* edge) {
const DX12OverlayCoverageSnapshot snapshot = GetOverlayCoverageSnapshot();
const char* lastGate = dx12_hook_g_OverlayCoverageLastGate.load(std::memory_order_relaxed);
const uint32_t route = dx12_hook_g_LastDX12OverlayRenderRoute.load(std::memory_order_acquire);
HookLogImportant(
    "[OVERLAY COVERAGE] %s: presents=%llu uncovered=%llu currentStreak=%llu longestStreak=%llu lastGate=%s "
    "lastRoute=%s",
    edge ? edge : "summary", static_cast<unsigned long long>(snapshot.totalPresents),
    static_cast<unsigned long long>(snapshot.uncoveredPresents),
    static_cast<unsigned long long>(snapshot.currentStreak),
    static_cast<unsigned long long>(snapshot.longestStreak), lastGate ? lastGate : "none",
    DX12OverlayRenderRouteName(route));
}


void NoteDX12OverlayRendered(DX12OverlayRenderRoute route) {
const uint64_t drawsBefore = dx12_hook_g_OverlayCoverageDrawCount.fetch_add(1, std::memory_order_acq_rel);
const uint32_t previousRoute =
    dx12_hook_g_LastDX12OverlayRenderRoute.exchange(static_cast<uint32_t>(route), std::memory_order_acq_rel);
dx12_hook_g_LastDX12OverlayRenderTickMs.store(GetTickCount64(), std::memory_order_release);
// [OVERLAY DOUBLE-DRAW] detector: a draw already happened since the last ACCOUNTED present
// (drawsBefore > lastSeen) and it came from a DIFFERENT route — i.e. two overlay routes rendered
// within the same present window. One route re-drawing is benign; two different routes can show the
// overlay TWICE on screen (e.g. the FFX UI-composite prework and PostSL backbuffer rendering were both
// live for ~3.5s during the GTA FSR->DLSS pre-apply window, session 20260702_092933). Diagnostic only —
// makes route-arbitration overlaps attributable from one run; visible flicker/dimming correlates here.
const uint64_t lastAccountedDraws = dx12_hook_g_OverlayCoverageLastSeenDrawCount.load(std::memory_order_acquire);
if (drawsBefore > lastAccountedDraws && previousRoute != static_cast<uint32_t>(route)) {
    static std::atomic<int> s_doubleDrawLogCount{0};
    const int n = s_doubleDrawLogCount.fetch_add(1, std::memory_order_relaxed);
    if (n < 20 || (n % 300) == 0) {
        HookLogImportant(
            "[OVERLAY DOUBLE-DRAW] two overlay routes rendered in the same present window: %s then %s "
            "(pendingDraws=%llu log=%d)",
            DX12OverlayRenderRouteName(previousRoute), DX12OverlayRenderRouteName(static_cast<uint32_t>(route)),
            static_cast<unsigned long long>(drawsBefore + 1 - lastAccountedDraws), n + 1);
    }
}
}


void RequestFGDetectionHeuristicReset(ID3D12CommandQueue* authoritativeBaseline) {
dx12_hook_g_QueueChangeHeuristicAuthoritativeBaseline.store(authoritativeBaseline, std::memory_order_release);
dx12_hook_g_ResetQueueChangeHeuristic.store(true, std::memory_order_release);
dx12_hook_g_ResetECLPatternHeuristic.store(true, std::memory_order_release);
}


void SetPostSLLastWorkingQueue(ID3D12CommandQueue* queue) {
if (queue == dx12_hook_g_PostSLLastWorkingQueue)
    return;
if (queue)
    queue->AddRef();
if (dx12_hook_g_PostSLLastWorkingQueue)
    dx12_hook_g_PostSLLastWorkingQueue->Release();
dx12_hook_g_PostSLLastWorkingQueue = queue;
}


void ShutdownDescFreeBackend(const char* reason, bool shutdownMode) {
dx12_hook_g_DescFreeBackendDevice = nullptr;
dx12_hook_g_DescFreeBackendFormat = DXGI_FORMAT_UNKNOWN;
DX12DescFreeBackend* backend = dx12_hook_g_DescFreeBackend;
const bool adapterInitialized = dx12_hook_g_D3D11On12Adapter.IsInitialized();
if (!backend && !adapterInitialized) {
    return;
}

CustomOverlay::RendererBackend* adapterBackend = adapterInitialized ? dx12_hook_g_D3D11On12Adapter.GetBackend() : nullptr;
const OverlayBackendType adapterType =
    adapterInitialized ? dx12_hook_g_D3D11On12Adapter.GetBackendType() : OverlayBackendType::None;
const bool adapterOwnsBackend = backend && adapterBackend == backend;

if (ce::dx12_overlay_policy::ShouldShutdownDescFreeBackendViaOverlayAdapter(backend != nullptr, adapterInitialized,
                                                                            adapterOwnsBackend)) {
    HookLogImportant(
        "DX12: Shutting down adapter-owned DescFree backend (reason=%s backend=%p adapterBackend=%p "
        "adapterType=%d shutdownMode=%d)",
        reason ? reason : "unknown", backend, adapterBackend, (int)adapterType, shutdownMode ? 1 : 0);
    if (shutdownMode) {
        dx12_hook_g_D3D11On12Adapter.SetShutdownMode(true);
    }
    dx12_hook_g_D3D11On12Adapter.Shutdown();
    dx12_hook_g_DescFreeBackend = nullptr;
    return;
}

if (adapterInitialized) {
    HookLogImportant(
        "DX12: Shutting down DX12 overlay adapter without tracked DescFree ownership "
        "(reason=%s backend=%p adapterBackend=%p adapterType=%d shutdownMode=%d)",
        reason ? reason : "unknown", backend, adapterBackend, (int)adapterType, shutdownMode ? 1 : 0);
    if (shutdownMode) {
        dx12_hook_g_D3D11On12Adapter.SetShutdownMode(true);
    }
    dx12_hook_g_D3D11On12Adapter.Shutdown();
}

if (backend) {
    HookLogImportant("DX12: Shutting down standalone DescFree backend (reason=%s backend=%p)",
                     reason ? reason : "unknown", backend);
    backend->Shutdown();
    delete backend;
    if (dx12_hook_g_DescFreeBackend == backend) {
        dx12_hook_g_DescFreeBackend = nullptr;
    }
}
}


// Lazily (re)builds the device-scoped descriptor-free overlay backend for the
// requested device/format pair. A live backend is reused as-is when both
// match (the warm path that closes the first-present blank after FG
// transitions); a device or format change is the only rebuild trigger.
// Returns true when a ready backend is bound to (device, format).
bool EnsureDescFreeBackendForDeviceAndFormat(ID3D12Device* dev, DXGI_FORMAT format, const char* context) {
if (!dev) {
    return dx12_hook_g_DescFreeBackend != nullptr && dx12_hook_g_D3D11On12Adapter.IsInitialized();
}
if (dx12_hook_g_DescFreeBackend && (dx12_hook_g_DescFreeBackendDevice != dev || dx12_hook_g_DescFreeBackendFormat != format)) {
    HookLogImportant("DX12: DescFree backend stale (device %p->%p fmt %d->%d) — rebuilding (%s)",
                     dx12_hook_g_DescFreeBackendDevice, dev, static_cast<int>(dx12_hook_g_DescFreeBackendFormat),
                     static_cast<int>(format), context ? context : "unknown");
    ShutdownDescFreeBackend(context);
}
if (!dx12_hook_g_DescFreeBackend) {
    auto* backend = new DX12DescFreeBackend();
    if (backend->InitDevice(dev, format)) {
        dx12_hook_g_DescFreeBackend = backend;
        dx12_hook_g_DescFreeBackendDevice = dev;
        dx12_hook_g_DescFreeBackendFormat = format;
        dx12_hook_g_D3D11On12Adapter.InitCustom(dx12_hook_g_DescFreeBackend, OverlayBackendType::DX12);
        HookLogImportant("DX12: Descriptor-free overlay backend ready (%s, device=%p fmt=%d)",
                         context ? context : "unknown", dev, static_cast<int>(format));
    } else {
        delete backend;
        HookLogImportant("DX12: Descriptor-free backend init FAILED (%s, fmt=%d)", context ? context : "unknown",
                         static_cast<int>(format));
    }
}
return dx12_hook_g_DescFreeBackend != nullptr && dx12_hook_g_D3D11On12Adapter.IsInitialized();
}


void EnsureOverlayBreadcrumbBuffer(ID3D12Device* device) {
if (dx12_hook_g_OverlayBcBuffer || !device) {
    return;
}
// NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
D3D12_HEAP_PROPERTIES hp = {};
hp.Type = D3D12_HEAP_TYPE_CUSTOM;
hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;  // system memory, CPU-cached, GPU-writable
hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
D3D12_RESOURCE_DESC rd = {};
rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
rd.Width = static_cast<UINT64>(kOverlayBcSlotCount) * sizeof(uint32_t);
rd.Height = 1;
rd.DepthOrArraySize = 1;
rd.MipLevels = 1;
rd.Format = DXGI_FORMAT_UNKNOWN;
rd.SampleDesc.Count = 1;
rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
ID3D12Resource* buf = nullptr;
HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                             IID_PPV_ARGS(&buf));
if (FAILED(hr) || !buf) {
    static std::atomic<int> s_bcCreateFailLog{0};
    if (s_bcCreateFailLog.fetch_add(1, std::memory_order_relaxed) < 3) {
        HookLogImportant("DX12: Overlay GPU breadcrumb buffer create failed hr=0x%08X", (unsigned)hr);
    }
    return;
}
void* mapped = nullptr;
if (FAILED(buf->Map(0, nullptr, &mapped)) || !mapped) {
    buf->Release();
    return;
}
memset(mapped, 0, static_cast<size_t>(rd.Width));
dx12_hook_g_OverlayBcMapped = static_cast<volatile uint32_t*>(mapped);
dx12_hook_g_OverlayBcGpuVA = buf->GetGPUVirtualAddress();
dx12_hook_g_OverlayBcBuffer = buf;
HookLogImportant("DX12: Overlay GPU breadcrumb buffer armed (slots=%d gpuVA=0x%llX)",
                 static_cast<int>(kOverlayBcSlotCount), static_cast<unsigned long long>(dx12_hook_g_OverlayBcGpuVA));
}


// Call once per overlay submit (before recording) to bump the sequence the GPU will stamp into each slot.
void BeginOverlayGpuBreadcrumbFrame(ID3D12Device* device) {
EnsureOverlayBreadcrumbBuffer(device);
if (dx12_hook_g_OverlayBcMapped) {
    dx12_hook_g_OverlayBcSeq.fetch_add(1, std::memory_order_relaxed);
}
}


void WriteOverlayGpuBreadcrumb(ID3D12GraphicsCommandList* list, OverlayGpuBreadcrumbOp op) {
if (!list || !dx12_hook_g_OverlayBcMapped || dx12_hook_g_OverlayBcGpuVA == 0 || op == 0 || op >= kOverlayBcSlotCount) {
    return;
}
ID3D12GraphicsCommandList2* list2 = nullptr;
if (FAILED(list->QueryInterface(IID_PPV_ARGS(&list2))) || !list2) {
    return;
}
D3D12_WRITEBUFFERIMMEDIATE_PARAMETER param = {};
param.Dest = dx12_hook_g_OverlayBcGpuVA + static_cast<UINT64>(op) * sizeof(uint32_t);
param.Value = dx12_hook_g_OverlayBcSeq.load(std::memory_order_relaxed);
D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
list2->WriteBufferImmediate(1, &param, &mode);
list2->Release();
}


DX12Context GetDX12PrerenderContext(bool preferOriginalGameQueue, bool* usesOriginalGameQueue, ID3D12CommandQueue** currentQueueSnapshot) {
std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
ID3D12CommandQueue* currentQueue = g_CommandQueue.load(std::memory_order_acquire);
const bool useOriginalGameQueue = preferOriginalGameQueue && dx12_hook_g_OriginalGameQueue != nullptr;
ID3D12CommandQueue* selectedQueue = useOriginalGameQueue ? dx12_hook_g_OriginalGameQueue : currentQueue;
if (usesOriginalGameQueue) {
    *usesOriginalGameQueue = useOriginalGameQueue;
}
if (currentQueueSnapshot) {
    *currentQueueSnapshot = currentQueue;
}
if (!selectedQueue) {
    return {};
}

// Streamline can use a second D3D12 device. Derive the fence device from
// the selected queue instead of pairing origGame with a volatile runtime
// device pointer.
ID3D12Device* queueDevice = nullptr;
const HRESULT deviceHr = selectedQueue->GetDevice(IID_PPV_ARGS(&queueDevice));
if (FAILED(deviceHr) || !queueDevice) {
    static std::atomic<int> s_prerenderQueueDeviceLogs{0};
    if (s_prerenderQueueDeviceLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
        HookLog("DX12: Prerender queue GetDevice failed queue=%p hr=0x%08X", selectedQueue, deviceHr);
    }
    return {};
}
DX12Context context(queueDevice, selectedQueue);
queueDevice->Release();
return context;
}


// Keep native driver limiters in sync with the DX12 device we already discover
// from queue/swapchain hooks. Ownership remains with the existing DX12 globals.
void DX12_PublishNativeLimiterDevice(ID3D12Device* device, ID3D12CommandQueue* queue, const char* source) {
if (!device)
    return;

g_ReflexLimiter.SetDevice(static_cast<IUnknown*>(device));

bool ctxUpdated = false;
bool ctxApiConflict = false;
if (auto* ctx = ce::GetHookContext()) {
    std::lock_guard<std::mutex> ctxLock(ctx->initMutex);
    if (!ctx->shuttingDown.load(std::memory_order_acquire)) {
        if (ctx->activeAPI == ce::ActiveGraphicsAPI::None) {
            ctx->activeAPI = ce::ActiveGraphicsAPI::DX12;
        }
        if (ctx->activeAPI == ce::ActiveGraphicsAPI::DX12) {
            ctx->graphicsData.dx12.device = device;
            ctx->graphicsData.dx12.commandQueue = queue;
            ctxUpdated = true;
        } else {
            ctxApiConflict = true;
        }
    }
}

static std::atomic<ID3D12Device*> s_lastPublishedDevice{nullptr};
static std::atomic<ID3D12CommandQueue*> s_lastPublishedQueue{nullptr};
static std::atomic<uint64_t> s_nativeLimiterPublishChangeCount{0};
ID3D12Device* previousDevice = s_lastPublishedDevice.exchange(device, std::memory_order_acq_rel);
ID3D12CommandQueue* previousQueue = s_lastPublishedQueue.exchange(queue, std::memory_order_acq_rel);
const bool deviceChanged = previousDevice != device;
const bool queueChanged = previousQueue != queue;
if (deviceChanged || queueChanged) {
    const uint64_t changeCount = s_nativeLimiterPublishChangeCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (deviceChanged || changeCount <= 32 || (changeCount % 512) == 0) {
        HookLogImportant(
            "DX12: Published native limiter device from %s (device=%p queue=%p ctxUpdated=%d ctxApiConflict=%d "
            "deviceChanged=%d queueChanged=%d changeCount=%llu)",
            source && source[0] ? source : "unknown", device, queue, ctxUpdated ? 1 : 0, ctxApiConflict ? 1 : 0,
            deviceChanged ? 1 : 0, queueChanged ? 1 : 0, static_cast<unsigned long long>(changeCount));
    }
}
}


void ResetAuthoritativeFSRRealFrameOnlyStreak() {
dx12_hook_g_AuthoritativeFSRRealFrameOnlyStreak.store(0, std::memory_order_release);
}


void ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak() {
dx12_hook_g_StaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak.store(0, std::memory_order_release);
}


void ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown() {
dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.store(false, std::memory_order_release);
}


bool HasResolvedOfficialFFXStartupPath() {
return g_FGCompat.HasDirectFFXApiConfirmation() ||
       dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.load(std::memory_order_acquire);
}


void ResetProtectedOfficialFFXStartupProgressCounters() {
dx12_hook_g_ProtectedOfficialFFXStartupProcessFrameSkips.store(0, std::memory_order_release);
dx12_hook_g_ProtectedOfficialFFXStartupECLPassThroughs.store(0, std::memory_order_release);
dx12_hook_g_ProtectedOfficialFFXStartupBeginMs.store(0, std::memory_order_release);
}


void ArmProtectedOfficialFFXStartupProgressTracking(const char* reason) {
dx12_hook_g_ProtectedOfficialFFXStartupProcessFrameSkips.store(0, std::memory_order_release);
dx12_hook_g_ProtectedOfficialFFXStartupECLPassThroughs.store(0, std::memory_order_release);
dx12_hook_g_ProtectedOfficialFFXStartupBeginMs.store(GetTickCount64(), std::memory_order_release);
HookLogImportant("DX12: Protected official FFX startup progress tracking armed (%s)",
                 reason && reason[0] ? reason : "unknown");
}


void ClearOfficialFFXRuntimeOwnedPresentPathAssumption(const char* reason) {
dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedSinceMs.store(0, std::memory_order_release);
if (dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.exchange(false, std::memory_order_acq_rel)) {
    HookLogImportant("DX12: Cleared progress-resolved official FFX runtime-owned Present path assumption (%s)",
                     reason && reason[0] ? reason : "unknown");
}
}


void StoreDeferredOfficialFFXTakeoverSideEffects(ID3D12CommandQueue* queue, const char* modulePath, const char* reason) {
{
    std::lock_guard<std::mutex> lock(dx12_hook_g_DeferredOfficialFFXTakeoverMutex);
    if (dx12_hook_g_DeferredOfficialFFXTakeoverQueue) {
        dx12_hook_g_DeferredOfficialFFXTakeoverQueue->Release();
        dx12_hook_g_DeferredOfficialFFXTakeoverQueue = nullptr;
    }
    if (queue) {
        queue->AddRef();
        dx12_hook_g_DeferredOfficialFFXTakeoverQueue = queue;
    }
    if (modulePath && modulePath[0]) {
        strncpy_s(dx12_hook_g_DeferredOfficialFFXTakeoverModulePath, sizeof(dx12_hook_g_DeferredOfficialFFXTakeoverModulePath),
                  modulePath, _TRUNCATE);
    } else {
        dx12_hook_g_DeferredOfficialFFXTakeoverModulePath[0] = '\0';
    }
}
dx12_hook_g_DeferredOfficialFFXTakeoverSideEffectsPending.store(true, std::memory_order_release);
HookLogImportant(
    "DX12: Official FFX takeover side-effects staged until enabled ffxConfigure "
    "(queue=%p module=%s reason=%s)",
    queue, modulePath && modulePath[0] ? modulePath : "unknown", reason && reason[0] ? reason : "unknown");
}


ID3D12CommandQueue* ConsumeDeferredOfficialFFXTakeoverSideEffects(char* modulePathOut, size_t modulePathOutCount) {
if (modulePathOut && modulePathOutCount > 0) {
    modulePathOut[0] = '\0';
}
if (!dx12_hook_g_DeferredOfficialFFXTakeoverSideEffectsPending.exchange(false, std::memory_order_acq_rel)) {
    return nullptr;
}

std::lock_guard<std::mutex> lock(dx12_hook_g_DeferredOfficialFFXTakeoverMutex);
ID3D12CommandQueue* queue = dx12_hook_g_DeferredOfficialFFXTakeoverQueue;
dx12_hook_g_DeferredOfficialFFXTakeoverQueue = nullptr;
if (modulePathOut && modulePathOutCount > 0) {
    strncpy_s(modulePathOut, modulePathOutCount, dx12_hook_g_DeferredOfficialFFXTakeoverModulePath, _TRUNCATE);
}
dx12_hook_g_DeferredOfficialFFXTakeoverModulePath[0] = '\0';
return queue;
}


ID3D12CommandQueue* ReferenceDeferredOfficialFFXTakeoverQueue() {
if (!dx12_hook_g_DeferredOfficialFFXTakeoverSideEffectsPending.load(std::memory_order_acquire)) {
    return nullptr;
}

std::lock_guard<std::mutex> lock(dx12_hook_g_DeferredOfficialFFXTakeoverMutex);
if (!dx12_hook_g_DeferredOfficialFFXTakeoverQueue) {
    return nullptr;
}

dx12_hook_g_DeferredOfficialFFXTakeoverQueue->AddRef();
return dx12_hook_g_DeferredOfficialFFXTakeoverQueue;
}


void ClearDeferredOfficialFFXTakeoverSideEffects(const char* reason) {
ID3D12CommandQueue* queue = nullptr;
bool hadPending = dx12_hook_g_DeferredOfficialFFXTakeoverSideEffectsPending.exchange(false, std::memory_order_acq_rel);
{
    std::lock_guard<std::mutex> lock(dx12_hook_g_DeferredOfficialFFXTakeoverMutex);
    queue = dx12_hook_g_DeferredOfficialFFXTakeoverQueue;
    dx12_hook_g_DeferredOfficialFFXTakeoverQueue = nullptr;
    dx12_hook_g_DeferredOfficialFFXTakeoverModulePath[0] = '\0';
}
if (queue) {
    queue->Release();
}
if (hadPending) {
    HookLogImportant("DX12: Cleared staged official FFX takeover side-effects (%s)",
                     reason && reason[0] ? reason : "unknown");
}
}


void ClearProtectedOfficialFFXStartupSwapchainPending(const char* reason) {
if (dx12_hook_g_ProtectedOfficialFFXStartupSwapchainPending.exchange(false, std::memory_order_acq_rel)) {
    HookLogImportant("DX12: Cleared protected official FFX startup swapchain pass-through (%s)",
                     reason && reason[0] ? reason : "unknown");
}
ResetProtectedOfficialFFXStartupProgressCounters();
}


void SetNativeFSRStartupConfigureArmingPending(bool pending, const char* reason) {
const bool previous = dx12_hook_g_NativeFSRStartupConfigureArmingPending.exchange(pending, std::memory_order_acq_rel);
if (previous != pending) {
    HookLogImportant("DX12: Native FSR startup configure arming %s (%s)", pending ? "pending" : "cleared",
                     reason && reason[0] ? reason : "unknown");
}
}


void RememberOriginalQueueSwapchainIdentity(IDXGISwapChain* swapchain, const char* reason) {
if (!swapchain) {
    return;
}

IDXGISwapChain* previous = dx12_hook_g_LastProvenOriginalQueueSwapchain.load(std::memory_order_acquire);
if (previous != swapchain) {
    previous = dx12_hook_g_LastProvenOriginalQueueSwapchain.exchange(swapchain, std::memory_order_acq_rel);
    HookLogImportant("DX12: Remembered exact original-queue swapchain identity %p (previous=%p reason=%s)",
                     swapchain, previous, reason ? reason : "unspecified");
}

// The newest explicit association wins when one COM identity has served
// both runtime and native routes at different points in its lifetime.
IDXGISwapChain* expectedPostSLSwapchain = dx12_hook_g_LastSuccessfulPostSLSwapchain.load(std::memory_order_acquire);
if (expectedPostSLSwapchain == swapchain &&
    dx12_hook_g_LastSuccessfulPostSLSwapchain.compare_exchange_strong(expectedPostSLSwapchain, nullptr,
                                                            std::memory_order_acq_rel, std::memory_order_acquire)) {
    HookLogImportant("DX12: Original-queue association superseded remembered PostSL ownership for swapchain %p",
                     swapchain);
}
}


void UpdateLastKnownSwapchainHDRStateCache(DXGI_FORMAT format, bool isActualHDR, int swapChainColorSpace, bool presentationContractSupported) {
(void)format;
dx12_hook_g_LastKnownSwapchainColorSpace.store(swapChainColorSpace, std::memory_order_release);
dx12_hook_g_LastKnownSwapchainIsHDR.store(isActualHDR, std::memory_order_release);
dx12_hook_g_LastKnownSwapchainHDRStateValid.store(presentationContractSupported, std::memory_order_release);
}


bool IsReadableSwapchainPointer(const void* ptr) {
if (!ptr) {
    return false;
}

MEMORY_BASIC_INFORMATION mbi = {};
if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) {
    return false;
}
if (mbi.State != MEM_COMMIT) {
    return false;
}
if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) {
    return false;
}

return true;
}


bool IsExecutableCodePointer(const void* ptr) {
if (!ptr) {
    return false;
}

MEMORY_BASIC_INFORMATION mbi = {};
if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) {
    return false;
}
if (mbi.State != MEM_COMMIT) {
    return false;
}
if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) {
    return false;
}

const DWORD executableProtection =
    PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
return (mbi.Protect & executableProtection) != 0;
}


void* ResolveLoadedOrLoadableExport(const char* moduleName, const char* functionName) {
HMODULE module = GetModuleHandleA(moduleName);
if (!module) {
    module = LoadLibraryExA(moduleName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}
return module ? reinterpret_cast<void*>(GetProcAddress(module, functionName)) : nullptr;
}


bool IsCrtPurecallFunctionPointer(const void* ptr) {
static void* s_ucrtPurecall = ResolveLoadedOrLoadableExport("ucrtbase.dll", "_purecall");
static void* s_msvcrtPurecall = ResolveLoadedOrLoadableExport("msvcrt.dll", "_purecall");
return ptr && (ptr == s_ucrtPurecall || ptr == s_msvcrtPurecall);
}


bool IsUsableStartupActivationSwapchainPointer(IDXGISwapChain* swapchain) {
if (!IsReadableSwapchainPointer(swapchain) || !IsReadableSwapchainPointer(reinterpret_cast<const void*>(*(void***)swapchain))) {
    return false;
}

void** vtable = *(void***)swapchain;
if (!vtable || !vtable[0] || !vtable[1] || !vtable[2] || !vtable[8]) {
    return false;
}

if (!IsExecutableCodePointer(vtable[0]) || !IsExecutableCodePointer(vtable[1]) ||
    !IsExecutableCodePointer(vtable[2]) || !IsExecutableCodePointer(vtable[8])) {
    return false;
}

if (IsCrtPurecallFunctionPointer(vtable[0]) || IsCrtPurecallFunctionPointer(vtable[1]) ||
    IsCrtPurecallFunctionPointer(vtable[2]) || IsCrtPurecallFunctionPointer(vtable[8])) {
    static std::atomic<int> s_purecallSwapchainRejectLogCount{0};
    const int logCount = s_purecallSwapchainRejectLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 100) == 0) {
        HookLogImportant(
            "DX12: Rejecting startup activation swapchain %p because its vtable resolves to CRT _purecall "
            "(qi=%p addRef=%p release=%p present=%p log=%d)",
            swapchain, vtable[0], vtable[1], vtable[2], vtable[8], logCount + 1);
    }
    return false;
}

return true;
}


void SafeReleaseStartupActivationSwapchain(IDXGISwapChain* swapchain, const char* source) {
if (!swapchain) {
    return;
}

if (!IsUsableStartupActivationSwapchainPointer(swapchain)) {
    static std::atomic<int> s_skipUnsafeSwapchainReleaseLogCount{0};
    const int logCount = s_skipUnsafeSwapchainReleaseLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 100) == 0) {
        HookLogImportant(
            "DX12: Skipping unsafe startup activation swapchain Release for stale pointer %p "
            "(source=%s log=%d)",
            swapchain, source ? source : "unknown", logCount + 1);
    }
    return;
}

swapchain->Release();
}


void ReleaseStreamlineStartupActivationSwapchain(const char* source) {
IDXGISwapChain* oldSwapchain = nullptr;
{
    std::lock_guard<std::mutex> lock(dx12_hook_g_StreamlineStartupActivationSwapchainMutex);
    oldSwapchain = dx12_hook_g_StreamlineStartupActivationSwapchain;
    dx12_hook_g_StreamlineStartupActivationSwapchain = nullptr;
}

if (oldSwapchain) {
    HookLogImportant("DX12: Released retained Streamline startup activation swapchain %p (source=%s)", oldSwapchain,
                     source ? source : "unknown");
    SafeReleaseStartupActivationSwapchain(oldSwapchain, source);
}
}


bool HasRetainedStreamlineStartupActivationSwapchain() {
std::lock_guard<std::mutex> lock(dx12_hook_g_StreamlineStartupActivationSwapchainMutex);
return dx12_hook_g_StreamlineStartupActivationSwapchain != nullptr;
}


bool HasUsableRetainedStreamlineStartupActivationSwapchainCandidate() {
std::lock_guard<std::mutex> lock(dx12_hook_g_StreamlineStartupActivationSwapchainMutex);
return IsUsableStartupActivationSwapchainPointer(dx12_hook_g_StreamlineStartupActivationSwapchain);
}


bool HasStartupActivationSwapchainCandidateForECLProbe() {
return HasUsableRetainedStreamlineStartupActivationSwapchainCandidate();
}


void SetPostSLCallbackInstalled(bool installed, const char* reason) {
if (installed) {
    dx12_hook_g_PostSLCallbackExecutionEnabled.store(true, std::memory_order_release);
    if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != &PostSLOverlayRenderGated) {
        DXGIShared::g_PostSLOverlayRenderCallback.store(&PostSLOverlayRenderGated, std::memory_order_release);
        HookLogImportant("%s — installed gated PostSL callback", reason);
        ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kPostSLCallbackInstalled,
                                    reason ? reason : "SetPostSLCallbackInstalled", nullptr, nullptr,
                                    g_FGCompat.GetRuntimeMode(), g_FGCompat.IsFGActive(), false);
    }
    return;
}

// Any authoritative disable (protected FFX quiesce, FFX takeover, resize,
// shutdown, retirement itself) ends the make-before-break keep-alive: the
// keep-alive paths deliberately skip calling this, so a call here means a
// stronger teardown authority owns the transition now.
dx12_hook_g_PostSLExplicitOffKeepAlive.store(false, std::memory_order_release);
dx12_hook_g_PostSLWarmResumePreservationPending.store(false, std::memory_order_release);
dx12_hook_g_PostSLCallbackExecutionEnabled.store(false, std::memory_order_release);

if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != nullptr) {
    DXGIShared::g_PostSLOverlayRenderCallback.store(nullptr, std::memory_order_release);
    HookLogImportant("%s — disabled PostSL callback", reason);
    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kPostSLCallbackRemoved,
                                reason ? reason : "SetPostSLCallbackInstalled", nullptr, nullptr,
                                g_FGCompat.GetRuntimeMode(), g_FGCompat.IsFGActive(), false);
}
}

