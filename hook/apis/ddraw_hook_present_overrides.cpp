#include "ddraw_hook_present_overrides.h"

#ifndef DIRECTDRAW_VERSION
#define DIRECTDRAW_VERSION 0x0700
#endif

#include <ddraw.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <utility>

#include "../../common/config.h"
#include "../common/perf_logger.h"
#include "../wrappers/vtable_hook.h"
#include "ddraw_hook_runtime_state.h"

extern std::atomic<bool> g_ShuttingDown;

void HookLog(const char* format, ...);
void HookLogImportant(const char* format, ...);
const GraphicsConfig& GetActiveGraphicsConfigCached();
uint32_t GetActiveGraphicsConfigVersion();
IDirectDrawSurface7* QuerySurface7(IUnknown* surfaceLike);

namespace {

namespace policy = ce::ddraw_present_policy;

bool PresentationHookIsShuttingDown() {
    return g_ShuttingDown.load(std::memory_order_acquire);
}

constexpr size_t kDirectDrawWaitSlot = 22;
constexpr size_t kMaxWaitHookVTables = 16;
constexpr size_t kMaxPrerenderDepth = 6;

static_assert(DDFLIP_WAIT == policy::kFlipWait);
static_assert(DDFLIP_NOVSYNC == policy::kFlipNoVsync);
static_assert(DDFLIP_DONOTWAIT == policy::kFlipDoNotWait);
static_assert(DDBLT_ASYNC == policy::kBltAsync);
static_assert(DDBLT_WAIT == policy::kBltWait);
static_assert(DDBLT_DONOTWAIT == policy::kBltDoNotWait);
static_assert(DDBLTFAST_WAIT == policy::kBltFastWait);
static_assert(DDBLTFAST_DONOTWAIT == policy::kBltFastDoNotWait);
static_assert(DDWAITVB_BLOCKBEGIN == policy::kWaitVblankBlockBegin);
static_assert(DDWAITVB_BLOCKBEGINEVENT == policy::kWaitVblankBlockBeginEvent);
static_assert(DDWAITVB_BLOCKEND == policy::kWaitVblankBlockEnd);

using WaitForVerticalBlankFn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, DWORD, HANDLE);

HRESULT DispatchDirectDrawWaitForVerticalBlank(size_t recordIndex, IUnknown* directDraw, DWORD flags,
                                               HANDLE eventHandle);

template <size_t RecordIndex>
HRESULT STDMETHODCALLTYPE IndexedDirectDrawWaitForVerticalBlankDetour(IUnknown* directDraw, DWORD flags,
                                                                      HANDLE eventHandle) {
    return DispatchDirectDrawWaitForVerticalBlank(RecordIndex, directDraw, flags, eventHandle);
}

template <size_t... RecordIndices>
constexpr auto MakeWaitDetours(std::index_sequence<RecordIndices...>) {
    return std::array<WaitForVerticalBlankFn, sizeof...(RecordIndices)>{
        &IndexedDirectDrawWaitForVerticalBlankDetour<RecordIndices>...};
}

constexpr auto kWaitDetours = MakeWaitDetours(std::make_index_sequence<kMaxWaitHookVTables>{});

struct WaitHookRecord {
    void** vtable = nullptr;
    WaitForVerticalBlankFn original = nullptr;
};

struct WaitHookRegistry {
    std::mutex mutex;
    std::array<WaitHookRecord, kMaxWaitHookVTables> records{};
    size_t count = 0;
};

WaitHookRegistry& WaitHooks() {
    static WaitHookRegistry registry;
    return registry;
}

thread_local unsigned g_presentationScopeDepth = 0;
thread_local unsigned g_internalVblankWaitDepth = 0;
thread_local bool g_applicationVblankWaitPending = false;
thread_local uint64_t g_applicationVblankWaitGeneration = 0;

struct PendingPresentation {
    IDirectDrawSurface7* surface = nullptr;
    policy::PresentOperation operation = policy::PresentOperation::None;
};

struct PresentationOverrideState {
    std::recursive_mutex mutex;
    std::array<PendingPresentation, kMaxPrerenderDepth> pending{};
    IUnknown* directDrawOwner = nullptr;
    uint64_t submissionIndex = 0;
    uint64_t generation = 1;
    std::atomic<uint64_t> publishedGeneration{1};
    std::atomic<int> publishedQueueDepth{-1};
    uint32_t configVersion = std::numeric_limits<uint32_t>::max();
    uint32_t loggedOperationMask = 0;
    policy::VsyncRequest loggedVsync = policy::VsyncRequest::ApplicationControlled;
    int queueDepth = -1;
    int loggedQueueDepth = -2;
};

PresentationOverrideState& OverrideState() {
    static PresentationOverrideState state;
    return state;
}

const char* OperationLabel(policy::PresentOperation operation) {
    switch (operation) {
        case policy::PresentOperation::Flip:
            return "flip";
        case policy::PresentOperation::Blt:
            return "blt";
        case policy::PresentOperation::BltFast:
            return "blt-fast";
        case policy::PresentOperation::None:
            break;
    }
    return "none";
}

const char* VsyncLabel(policy::VsyncRequest request) {
    switch (request) {
        case policy::VsyncRequest::Fifo:
            return "fifo";
        case policy::VsyncRequest::Immediate:
            return "immediate";
        case policy::VsyncRequest::ApplicationControlled:
            break;
    }
    return "application";
}

void UpdateMaximum(std::atomic<uint32_t>& destination, uint32_t value) {
    uint32_t previous = destination.load(std::memory_order_relaxed);
    while (previous < value &&
           !destination.compare_exchange_weak(previous, value, std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
    }
}

uint32_t SaturatingMicroseconds(int64_t elapsed) {
    if (elapsed <= 0)
        return 0;
    const auto maximum = static_cast<int64_t>(std::numeric_limits<uint32_t>::max());
    return static_cast<uint32_t>((std::min)(elapsed, maximum));
}

bool ReleasePending(PendingPresentation& pending) {
    const bool held = pending.surface != nullptr;
    if (held)
        pending.surface->Release();
    pending = {};
    return held;
}

uint32_t ClearPendingLocked(PresentationOverrideState& state) {
    uint32_t released = 0;
    for (auto& pending : state.pending)
        released += ReleasePending(pending) ? 1u : 0u;
    state.submissionIndex = 0;
    return released;
}

HRESULT QueryCompletion(IDirectDrawSurface7* surface, policy::PresentOperation operation) {
    if (!surface)
        return DDERR_INVALIDOBJECT;
    if (operation == policy::PresentOperation::Flip)
        return surface->GetFlipStatus(DDGFS_ISFLIPDONE);
    if (operation == policy::PresentOperation::Blt || operation == policy::PresentOperation::BltFast)
        return surface->GetBltStatus(DDGBS_ISBLTDONE);
    return DDERR_INVALIDPARAMS;
}

bool WaitForCompletion(IDirectDrawSurface7* surface, policy::PresentOperation operation, int queueDepth) {
    auto& diagnostics = ddraw_hook_g_PresentationDiagnostics;
    diagnostics.prerenderChecks.fetch_add(1, std::memory_order_relaxed);
    HRESULT status = QueryCompletion(surface, operation);
    if (status != DDERR_WASSTILLDRAWING) {
        if (FAILED(status)) {
            const uint32_t failure =
                diagnostics.prerenderWaitFailures.fetch_add(1, std::memory_order_relaxed) + 1;
            if (failure <= 8) {
                HookLogImportant(
                    "DDraw: CPU prerender completion query failed operation=%s depth=%d hr=0x%08X (#%u)",
                    OperationLabel(operation), queueDepth, static_cast<unsigned>(status), failure);
            }
        }
        return SUCCEEDED(status);
    }

    const int64_t waitStartUs = PerfLogger::GetQpcUs();
    do {
        if (PresentationHookIsShuttingDown())
            break;
        SwitchToThread();
        status = QueryCompletion(surface, operation);
    } while (status == DDERR_WASSTILLDRAWING);

    const uint32_t elapsedUs = SaturatingMicroseconds(PerfLogger::GetQpcUs() - waitStartUs);
    const uint32_t waitOrdinal = diagnostics.prerenderWaits.fetch_add(1, std::memory_order_relaxed) + 1;
    diagnostics.prerenderWaitMicrosecondsTotal.fetch_add(elapsedUs, std::memory_order_relaxed);
    UpdateMaximum(diagnostics.prerenderWaitMicrosecondsMax, elapsedUs);
    if (waitOrdinal <= 8) {
        HookLogImportant("DDraw: CPU prerender waited operation=%s depth=%d waitUs=%u result=0x%08X (#%u)",
                         OperationLabel(operation), queueDepth, elapsedUs, static_cast<unsigned>(status),
                         waitOrdinal);
    }
    if (status == DDERR_WASSTILLDRAWING || FAILED(status)) {
        diagnostics.prerenderWaitFailures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool EnsureDirectDrawOwnerLocked(PresentationOverrideState& state, IDirectDrawSurface7* surface) {
    if (state.directDrawOwner)
        return true;
    void* owner = nullptr;
    if (!surface || FAILED(surface->GetDDInterface(&owner)) || !owner)
        return false;
    state.directDrawOwner = static_cast<IUnknown*>(owner);
    return true;
}

HRESULT CallWaitForVerticalBlank(IUnknown* directDraw) {
    if (!directDraw)
        return DDERR_INVALIDOBJECT;
    void** vtable = *reinterpret_cast<void***>(directDraw);
    if (!vtable || !vtable[kDirectDrawWaitSlot])
        return DDERR_INVALIDOBJECT;
    auto wait = reinterpret_cast<WaitForVerticalBlankFn>(vtable[kDirectDrawWaitSlot]);
    ++g_internalVblankWaitDepth;
    const HRESULT result = wait(directDraw, DDWAITVB_BLOCKBEGIN, nullptr);
    --g_internalVblankWaitDepth;
    return result;
}

void WaitForFifoBltLocked(PresentationOverrideState& state, IDirectDrawSurface7* surface,
                          policy::PresentOperation operation, bool applicationAlreadyWaited) {
    auto& diagnostics = ddraw_hook_g_PresentationDiagnostics;
    if (applicationAlreadyWaited) {
        const uint32_t reuse =
            diagnostics.applicationVblankWaitsReused.fetch_add(1, std::memory_order_relaxed) + 1;
        if (reuse <= 4) {
            HookLogImportant("DDraw: FIFO %s reused the application's blocking vertical-blank wait (#%u)",
                             OperationLabel(operation), reuse);
        }
        return;
    }

    if (!EnsureDirectDrawOwnerLocked(state, surface)) {
        const uint32_t failure = diagnostics.vblankWaitFailures.fetch_add(1, std::memory_order_relaxed) + 1;
        if (failure <= 8) {
            HookLogImportant("DDraw: FIFO %s could not resolve its owning DirectDraw interface (#%u)",
                             OperationLabel(operation), failure);
        }
        return;
    }

    const int64_t waitStartUs = PerfLogger::GetQpcUs();
    const HRESULT result = CallWaitForVerticalBlank(state.directDrawOwner);
    const uint32_t elapsedUs = SaturatingMicroseconds(PerfLogger::GetQpcUs() - waitStartUs);
    if (SUCCEEDED(result)) {
        diagnostics.vblankWaits.fetch_add(1, std::memory_order_relaxed);
        diagnostics.vblankWaitMicrosecondsTotal.fetch_add(elapsedUs, std::memory_order_relaxed);
        UpdateMaximum(diagnostics.vblankWaitMicrosecondsMax, elapsedUs);
        return;
    }

    const uint32_t failure = diagnostics.vblankWaitFailures.fetch_add(1, std::memory_order_relaxed) + 1;
    if (failure <= 8) {
        HookLogImportant("DDraw: FIFO %s vertical-blank wait failed hr=0x%08X waitUs=%u (#%u)",
                         OperationLabel(operation), static_cast<unsigned>(result), elapsedUs, failure);
    }
}

void ConfigureQueueLocked(PresentationOverrideState& state, int queueDepth) {
    if (state.queueDepth == queueDepth)
        return;
    ClearPendingLocked(state);
    state.queueDepth = queueDepth;
    state.publishedQueueDepth.store(queueDepth, std::memory_order_release);
}

void LogContractLocked(PresentationOverrideState& state, uint32_t configVersion,
                       policy::VsyncRequest vsync, int queueDepth,
                       policy::PresentOperation operation, bool applicationAlreadyWaited,
                       DWORD originalFlags, DWORD effectiveFlags) {
    if (state.configVersion != configVersion || state.loggedVsync != vsync ||
        state.loggedQueueDepth != queueDepth) {
        state.configVersion = configVersion;
        state.loggedVsync = vsync;
        state.loggedQueueDepth = queueDepth;
        state.loggedOperationMask = 0;
    }

    const uint32_t operationBit = 1u << static_cast<unsigned>(operation);
    if ((state.loggedOperationMask & operationBit) != 0)
        return;
    state.loggedOperationMask |= operationBit;

    const char* route = "flags-only";
    if (vsync == policy::VsyncRequest::Fifo && operation != policy::PresentOperation::Flip)
        route = applicationAlreadyWaited ? "application-vblank" : "explicit-vblank";
    else if (vsync == policy::VsyncRequest::ApplicationControlled)
        route = "application";
    HookLogImportant(
        "DDraw: Presentation overrides operation=%s vsync=%s route=%s cpuPrerender=%d flags=0x%08X->0x%08X",
        OperationLabel(operation), VsyncLabel(vsync), route, queueDepth,
        static_cast<unsigned>(originalFlags), static_cast<unsigned>(effectiveFlags));
}

HRESULT DispatchDirectDrawWaitForVerticalBlank(size_t recordIndex, IUnknown* directDraw, DWORD flags,
                                               HANDLE eventHandle) {
    WaitForVerticalBlankFn original = nullptr;
    {
        auto& registry = WaitHooks();
        std::lock_guard<std::mutex> lock(registry.mutex);
        if (recordIndex < registry.count)
            original = registry.records[recordIndex].original;
    }
    if (!original) {
        static std::atomic<uint32_t> missingOriginalLogs{0};
        const uint32_t ordinal = missingOriginalLogs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ordinal <= 4) {
            HookLogImportant("DDraw: WaitForVerticalBlank detour has no original callback slot=%zu (#%u)",
                             recordIndex, ordinal);
        }
        return DDERR_GENERIC;
    }

    const bool applicationCall = g_internalVblankWaitDepth == 0 && g_presentationScopeDepth == 0;
    const uint64_t waitGeneration = OverrideState().publishedGeneration.load(std::memory_order_acquire);
    if (applicationCall) {
        g_applicationVblankWaitPending = false;
        g_applicationVblankWaitGeneration = 0;
    }

    const HRESULT result = original(directDraw, flags, eventHandle);
    if (!PresentationHookIsShuttingDown() && applicationCall && SUCCEEDED(result) &&
        policy::IsReusableVblankBeginWait(flags) &&
        OverrideState().publishedGeneration.load(std::memory_order_acquire) == waitGeneration) {
        g_applicationVblankWaitPending = true;
        g_applicationVblankWaitGeneration = waitGeneration;
    }
    return result;
}

}  // namespace

std::recursive_mutex& DirectDrawPresentationOverrideScope::PresentationMutex() {
    return OverrideState().mutex;
}

DirectDrawPresentationOverrideScope::DirectDrawPresentationOverrideScope(
    IDirectDrawSurface7* surface, policy::PresentOperation operation, bool isPresentation, DWORD& flags)
    : surface_(surface), operation_(operation), presentation_(isPresentation) {
    Initialize(nullptr, flags);
}

DirectDrawPresentationOverrideScope::DirectDrawPresentationOverrideScope(
    IUnknown* surface, policy::PresentOperation operation, bool isPresentation, DWORD& flags)
    : operation_(operation), presentation_(isPresentation) {
    Initialize(surface, flags);
}

void DirectDrawPresentationOverrideScope::Initialize(IUnknown* surfaceToUpgrade, DWORD& flags) {
    if (!presentation_ || operation_ == policy::PresentOperation::None || PresentationHookIsShuttingDown() ||
        ddraw_hook_g_DDrawBootstrapDepth != 0) {
        return;
    }

    const GraphicsConfig& config = GetActiveGraphicsConfigCached();
    const policy::VsyncRequest vsync = policy::ParseVsyncRequest(config.vsyncMode);
    queueDepth_ = policy::ResolvePrerenderQueueDepth(config.cpuPrerenderLimit);
    auto& state = OverrideState();
    const bool applicationWaitPending = std::exchange(g_applicationVblankWaitPending, false);
    const uint64_t applicationWaitGeneration = g_applicationVblankWaitGeneration;
    const DWORD originalFlags = flags;
    flags = policy::ApplyVsyncFlags(flags, operation_, vsync);
    if (vsync == policy::VsyncRequest::ApplicationControlled && queueDepth_ < 0 &&
        state.publishedQueueDepth.load(std::memory_order_acquire) < 0) {
        return;
    }

    depthEntered_ = true;
    if (g_presentationScopeDepth++ != 0)
        return;

    if (!surface_ && surfaceToUpgrade) {
        surface_ = QuerySurface7(surfaceToUpgrade);
        ownsSurface_ = surface_ != nullptr;
    }

    lock_ = std::unique_lock<std::recursive_mutex>(PresentationMutex());
    active_ = true;
    generation_ = state.generation;
    const bool applicationAlreadyWaited = applicationWaitPending && applicationWaitGeneration == generation_;
    ConfigureQueueLocked(state, queueDepth_);
    LogContractLocked(state, GetActiveGraphicsConfigVersion(), vsync, queueDepth_, operation_,
                      applicationAlreadyWaited, originalFlags, flags);
    fifoBltPacing_ = vsync == policy::VsyncRequest::Fifo &&
                     (operation_ == policy::PresentOperation::Blt ||
                      operation_ == policy::PresentOperation::BltFast);
    applicationVblankWait_ = applicationAlreadyWaited;

    if (queueDepth_ > 0 && state.submissionIndex >= static_cast<uint64_t>(queueDepth_)) {
        PendingPresentation& pending = state.pending[state.submissionIndex % static_cast<uint64_t>(queueDepth_)];
        if (pending.surface)
            WaitForCompletion(pending.surface, pending.operation, queueDepth_);
        ReleasePending(pending);
    }
}

void DirectDrawPresentationOverrideScope::PrepareForCall() {
    if (!active_ || callPrepared_)
        return;
    callPrepared_ = true;
    if (fifoBltPacing_)
        WaitForFifoBltLocked(OverrideState(), surface_, operation_, applicationVblankWait_);
}

void DirectDrawPresentationOverrideScope::Complete(HRESULT result) {
    if (completed_)
        return;
    completed_ = true;

    if (active_ && SUCCEEDED(result) && queueDepth_ >= 0) {
        auto& state = OverrideState();
        if (state.generation == generation_ && state.queueDepth == queueDepth_) {
            if (!surface_) {
                const uint32_t failure =
                    ddraw_hook_g_PresentationDiagnostics.prerenderWaitFailures.fetch_add(
                        1, std::memory_order_relaxed) +
                    1;
                if (failure <= 8) {
                    HookLogImportant(
                        "DDraw: CPU prerender has no Surface7 completion source operation=%s depth=%d (#%u)",
                        OperationLabel(operation_), queueDepth_, failure);
                }
            } else if (queueDepth_ == 0) {
                WaitForCompletion(surface_, operation_, queueDepth_);
            } else {
                PendingPresentation& pending =
                    state.pending[state.submissionIndex % static_cast<uint64_t>(queueDepth_)];
                ReleasePending(pending);
                surface_->AddRef();
                pending.surface = surface_;
                pending.operation = operation_;
                ++state.submissionIndex;
            }
        }
    }

    ReleaseExecutionOwnership();
}

void DirectDrawPresentationOverrideScope::ReleaseExecutionOwnership() {
    if (lock_.owns_lock())
        lock_.unlock();
    if (depthEntered_) {
        --g_presentationScopeDepth;
        depthEntered_ = false;
    }
}

DirectDrawPresentationOverrideScope::~DirectDrawPresentationOverrideScope() {
    ReleaseExecutionOwnership();
    if (ownsSurface_ && surface_)
        surface_->Release();
}

void InstallDirectDrawWaitForVerticalBlankHook(IUnknown* directDraw, const char* reason) {
    void** vtable = directDraw ? *reinterpret_cast<void***>(directDraw) : nullptr;
    if (!vtable)
        return;

    auto& registry = WaitHooks();
    std::lock_guard<std::mutex> lock(registry.mutex);
    for (size_t i = 0; i < registry.count; ++i) {
        if (registry.records[i].vtable == vtable)
            return;
    }
    if (registry.count == registry.records.size()) {
        HookLogImportant("DDraw: WaitForVerticalBlank hook registry is full; vtable=%p reason=%s", vtable,
                         reason ? reason : "unknown");
        return;
    }

    const size_t recordIndex = registry.count;
    WaitForVerticalBlankFn original = nullptr;
    const VTableHook::Status status = VTableHook::Create(
        reinterpret_cast<void*>(&vtable[kDirectDrawWaitSlot]),
        reinterpret_cast<void*>(kWaitDetours[recordIndex]), reinterpret_cast<void**>(&original));
    if (status == VTableHook::Success && original) {
        registry.records[registry.count++] = WaitHookRecord{vtable, original};
        HookLog("DDraw: WaitForVerticalBlank hook installed via %s (vtable=%p slot=%zu registry=%zu)",
                reason ? reason : "unknown", vtable, kDirectDrawWaitSlot, recordIndex);
        return;
    }
    HookLogImportant("DDraw: WaitForVerticalBlank hook failed via %s (%s)",
                     reason ? reason : "unknown", VTableHook::StatusToString(status));
}

uint32_t ResetDirectDrawPresentationOverrides() {
    g_applicationVblankWaitPending = false;
    g_applicationVblankWaitGeneration = 0;
    auto& state = OverrideState();
    std::lock_guard<std::recursive_mutex> lock(state.mutex);
    // A queued presentation holds the surface it presented - for a Flip, the
    // application's primary itself - until the next one retires it.
    uint32_t released = ClearPendingLocked(state);
    if (state.directDrawOwner) {
        state.directDrawOwner->Release();
        state.directDrawOwner = nullptr;
        ++released;
    }
    state.queueDepth = -1;
    state.publishedQueueDepth.store(-1, std::memory_order_release);
    state.loggedQueueDepth = -2;
    state.loggedOperationMask = 0;
    state.configVersion = std::numeric_limits<uint32_t>::max();
    ++state.generation;
    state.publishedGeneration.store(state.generation, std::memory_order_release);
    return released;
}
