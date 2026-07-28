#include <combaseapi.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <unknwn.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../common/frame_timing.h"
#include "../../common/raii_helpers.h"
#include "../capture/shared_capture.h"
#include "../common/capture_base.h"
#include "../common/capture_pacing.h"
#include "../common/custom_overlay_dx12.h"
#include "../common/dx12_process_frame_diagnostics.h"
#include "../common/dx12_fg_transition_model.h"
#include "../common/fg_detection.h"
#include "../common/fg_session_state.h"
#include "../common/hook_common.h"
#include "../common/input_manager.h"
#include "../common/overlay_adapter.h"
#include "../common/overlay_compat.h"
#include "../common/overlay_metrics_publisher.h"
#include "../common/performance_metrics.h"
#include "../common/screenshot_hook.h"
#include "../common/streamline_compat.h"
#include "../common/streamline_runtime_policy.h"
#include "../../common/secure_dll_loading.h"

#include "../common/fps_limiter.h"
#include "../common/freeze_watchdog.h"
#include "../common/perf_logger.h"

static bool IsActualFrameGenerationActive();
static bool IsStreamlineLoaded();
static bool DX12_SetSwapchainQueue(ID3D12CommandQueue* pQueue, bool authoritativeStreamlineRuntimeQueue,
                                   bool authoritativeFFXRuntimeQueue, bool gameCreatedSwapchain = false,
                                   IDXGISwapChain* associatedSwapchain = nullptr,
                                   bool authoritativeNormalSwapchainReturn = false);
#include "../common/swapchain_wrapper.h"
#include "../common/system_metrics.h"
#include "../wrappers/dxgi_swapchain_wrap.h"
#include "../wrappers/wrapper_hooks.h"
#include "dx11_hook.h"
#include "dx12_ffx_suspend_overlay.h"
#include "dx12_hook.h"
#include "dx12_sampler_hooks.h"
#include "dx12_streamline_ui_overlay.h"
#include "ffx_hook.h"
#include "graphics_hook.h"
#include "lod_helper.h"
#include "streamline_hook.h"

#include "../common/custom_overlay.h"
#include "../common/dx12_dred.h"
#include "../common/dx12_overlay_policy.h"
#include "../common/ffx_api_parsing.h"
#include "../common/overlay_shader_bytecode.h"

#include "../wrappers/inline_hook.h"
#include "../wrappers/vtable_hook.h"
#include "../wrappers/wrapper_base.h"
#include "dxgi_shared.h"

// ============================================================================
// SpecialK-style Streamline Handling
// ============================================================================

// ============================================================================
// Typedefs for D3D12 functions
typedef void(STDMETHODCALLTYPE* ExecuteCommandListsPtr)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
typedef HRESULT(STDMETHODCALLTYPE* SignalPtr)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
typedef HRESULT(STDMETHODCALLTYPE* CreateCommittedResourcePtr)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*,
                                                               D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*,
                                                               D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID,
                                                               void**);
// Global Function Pointers for detours (Visible to other modules)
ExecuteCommandListsPtr oExecuteCommandLists = nullptr;
CreateCommittedResourcePtr oCreateCommittedResource = nullptr;
// --- DX12 API call trace diagnostic (gated by Dx12TraceEnabled; off by default) ---
typedef HRESULT(STDMETHODCALLTYPE* CreateCommandQueuePtr)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID,
                                                          void**);
typedef HRESULT(STDMETHODCALLTYPE* CreateDescriptorHeapPtr)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID,
                                                            void**);
typedef HRESULT(STDMETHODCALLTYPE* CommandQueueSignalPtr)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
CreateCommandQueuePtr oTraceCreateCommandQueue = nullptr;
CreateDescriptorHeapPtr oTraceCreateDescriptorHeap = nullptr;
CommandQueueSignalPtr oTraceCommandQueueSignal = nullptr;
HRESULT STDMETHODCALLTYPE DetourCreateCommittedResource(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                        const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                        const D3D12_CLEAR_VALUE*, REFIID, void**);
HRESULT STDMETHODCALLTYPE DetourTraceCreateCommandQueue(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
HRESULT STDMETHODCALLTYPE DetourTraceCreateDescriptorHeap(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID,
                                                          void**);
HRESULT STDMETHODCALLTYPE DetourTraceCommandQueueSignal(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
// --- end DX12 API call trace diagnostic ---
static std::recursive_mutex g_ExecuteCommandListsHookStateMutex;
static std::map<void**, ExecuteCommandListsPtr> g_ExecuteCommandListsOriginalByVTable;
// ExecuteCommandLists runs many times per frame in CPU-bound workloads, so keep a
// lock-free cache for the most recently used vtable/original pair.
static std::atomic<void**> g_LastExecuteCommandListsVTable{nullptr};
static std::atomic<ExecuteCommandListsPtr> g_LastExecuteCommandListsOriginal{nullptr};

// Bypass trampoline for ECL that skips Streamline's hook.
// When SL FG is active, overlay ECLs are submitted through this trampoline
// so SL's internal frame tracking doesn't see our extra command lists.
static std::atomic<ExecuteCommandListsPtr> g_SLBypassECL{nullptr};

// Real D3D12 ExecuteCommandLists function pointer obtained by probing a
// COMPUTE queue (which SL doesn't hook for FG).  Used to bypass SL's
// vtable ECL hook when submitting overlay command lists.
static std::atomic<ExecuteCommandListsPtr> g_RealD3D12ECL{nullptr};

// Real D3D12 Signal function pointer for the command queue.  Probed alongside
// g_RealD3D12ECL.  Used to signal an overlay completion fence that ensures
// all overlay GPU work is finished before the FG runtime processes the
// swapchain backbuffer.  Bypasses any FG-runtime hooks on the Signal
// vtable entry.
static std::atomic<SignalPtr> g_RealD3D12Signal{nullptr};

// Separate fence for tracking overlay GPU completion during FG.  When FSR or
// DLSS frame generation is active, the main g_State.fence signal is skipped
// to avoid desyncing the FG pipeline.  This completion fence is signaled via
// the raw D3D12 Signal pointer and CPU-waited to ensure overlay GPU work
// completes before the FG runtime reads the swapchain backbuffer.
static std::atomic<ID3D12Fence*> g_OverlayCompletionFence{nullptr};

// Deferred ECL probe flag: set when ProbeRealD3D12ECL is skipped due to
// the Streamline startup window being active.  The probe runs after the
// startup window expires to avoid creating a temporary COMPUTE queue
// during Streamline's critical initialization (which can crash Streamline
// with a null pointer call on some games/configs).
static std::atomic<bool> g_ProbeRealD3D12ECLDeferred{false};

#if defined(__clang__) || defined(__GNUC__)
#define CE_RETURN_ADDRESS() __builtin_extract_return_addr(__builtin_return_address(0))
#elif defined(_MSC_VER)
#include <intrin.h>
#define CE_RETURN_ADDRESS() _ReturnAddress()
#else
#define CE_RETURN_ADDRESS() nullptr
#endif

// SwapChain Detour Pointers
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                        IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND,
                                                               const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                               IDXGISwapChain1**);

static PFN_CreateSwapChain oCreateSwapChain = nullptr;
static PFN_CreateSwapChainForHwnd oCreateSwapChainForHwnd = nullptr;

// ---------------------------------------------------------------------------
// Descriptor-free DX12 overlay backend.
//
// Renders the overlay using root constants + root SRV (structured uint buffer
// for the font atlas).  No descriptor heaps are bound, so SetDescriptorHeaps is
// never called.  This avoids the NVIDIA driver stall triggered by
// SetDescriptorHeaps + OMSetRenderTargets(swapchain backbuffer) in the same
// command list.
//
// The command list and RTV are set externally before each Render() call via
// the static pointers below.
// ---------------------------------------------------------------------------

static ID3D12GraphicsCommandList* s_descFreeCmdList = nullptr;
static D3D12_CPU_DESCRIPTOR_HANDLE s_descFreeRtv = {};

// Per-slot GPU-completion guard for the DescFree UPLOAD ring (vb_/ib_).
//
// Overlay render sites can set these two statics right before calling
// RenderOverlay() when a slot-reuse guard is intentionally enabled:
//   * s_descFreeSlotFence      = g_State.fence (the fence signaled for this
//                                frame's overlay GPU work)
//   * s_descFreeSlotGuardValue = the value that fence will reach once this
//                                frame's overlay ECL has executed on the GPU,
//                                or 0 to disable the guard for this frame.
//
// The DescFree backend round-robins through kPoolSize persistently-mapped
// UPLOAD vertex/index buffers.  Without a per-slot fence it would memcpy new
// geometry into a slot the GPU might still be reading from a previous frame.
// That is harmless while the GPU keeps up, but during long GPU pauses the CPU
// can wrap and overwrite in-flight vertex/index data.  Guard 0 leaves the
// current behavior unchanged and is used unless a caller publishes a concrete
// fence/value pair for this frame.
static ID3D12Fence* s_descFreeSlotFence = nullptr;
static UINT64 s_descFreeSlotGuardValue = 0;

static void PostSLOverlayRender(IDXGISwapChain* pSwapChain);

// Post-SL overlay rendering state.  Controls whether the re-entrant Present
// callback should actually render or skip (e.g. during FG cooldown / resize).
static std::atomic<bool> g_PostSLOverlayActive{false};
static std::atomic<int> g_PostSLCooldownRemaining{0};

// Make-before-break across explicit Streamline FG OFF (suspend/menu): while
// the DLSS-G proxy keeps presenting after slDLSSGSetOptions(off), confirmed
// PostSL stays armed-and-rendering on the same proven queue/swapchain until
// an authoritative normal swapchain/queue return is observed (or the proxy/SL
// stack dies). Cleared on: authoritative normal-route recovery, Streamline FG
// ON (warm resume), protected-FFX-startup quiesce, FFX takeover, swapchain
// invalidation/resize, Streamline modules gone, shutdown. Session
// 20260613_032326: the suspend/resume handoff seams were the last visible
// 3-4-present DLSS blanks.
static std::atomic<bool> g_PostSLExplicitOffKeepAlive{false};
// A warm resume consumes the explicit-OFF latch, but the first wrapper
// ProcessFrame can report its outer/original swapchain before PostSL performs
// the next real submit on the still-live proxy. Preserve the confirmed backend
// across those bookkeeping pointer changes until an active PostSL submit
// proves the resumed route again. This is event-driven, not time-based.
static std::atomic<bool> g_PostSLWarmResumePreservationPending{false};
// Raw identity only; never AddRef'd or dereferenced. A successful PostSL submit
// proves that this exact swapchain is compatible with the retained PostSL route
// for the current lifecycle epoch. The explicit-OFF keep-alive must not apply
// that route proof to any other swapchain pointer.
static std::atomic<IDXGISwapChain*> g_LastSuccessfulPostSLSwapchain{nullptr};
// Per-calling-thread proof that PostSL completed a real, device-healthy submit.
// The explicit-OFF top-level route snapshots this around its direct callback so
// it only suppresses a nested callback when that same thread/Present was
// actually covered; a distinct Streamline worker frame cannot create a false
// success or be de-duplicated.
static thread_local uint64_t s_PostSLSuccessfulSubmitSequence = 0;
static std::atomic<ULONGLONG> g_LastProcessFrameTickMs{0};
static std::atomic<ULONGLONG> g_LastFFXPresentCallbackTickMs{0};
static std::atomic<bool> g_FFXPresentCallbackBridgeExpected{false};
static std::atomic<bool> g_NativeFSRInternalNoCallbackComposition{false};
enum class DX12OverlayRenderRoute : uint32_t {
    kNone = 0,
    kNormal = 1,
    kPostSL = 2,
    kFFXPresentCallback = 3,
    kStreamlineUI = 4,
};
static std::atomic<ULONGLONG> g_LastDX12OverlayRenderTickMs{0};
static std::atomic<uint32_t> g_LastDX12OverlayRenderRoute{static_cast<uint32_t>(DX12OverlayRenderRoute::kNone)};
// NOTE: DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending is now in DXGIShared::g_SharedState
// so streamline_hook.cpp can check it from FlushSuppressedSetOptionsOffIfNeeded().
static std::atomic<bool> g_PostSLSyntheticStartupTakeoverLogged{false};
static std::atomic<int> g_PostSLSyntheticStartupWrapperProgressCount{0};
static std::atomic<bool> g_PostSLSyntheticStartupWrapperOnlyDumpRequested{false};
static std::atomic<uint32_t> g_PostSLLifecycleEpoch{0};
static std::mutex g_PostSLRenderMutex;
static std::atomic<uint32_t> g_StreamlineEnableCallsInFlight{0};

// Set to true when PostSLOverlayRender has confirmed it can render (i.e., re-entrant
// Present calls are actually happening).  In games like GTA V, SL FG bypasses our
// vtable hook for interpolated frames, so PostSL never fires.  When this is false,
// pre-SL rendering is NOT suppressed, allowing the overlay to render before SL.
static std::atomic<bool> g_PostSLConfirmedRendering{false};
// True once PostSL has performed at least one CONFIRMED render (devRemoved=0) in the
// CURRENT reactivation epoch. The reactivation warmup exists only to protect the FIRST
// ECL submit on DLSS-FG's fragile init state; once a confirmed render lands, that first
// ECL already succeeded, so the remaining warmup must not re-blank a live overlay (the
// no-blank principle). Strictly epoch-scoped: reset where s_callsSinceReactivation=0 in
// the genuine-reactivation block, set at the confirmed-render edge. The preserve/keep-alive
// warm-resume paths do not bump the epoch, so the flag correctly persists through them.
static std::atomic<bool> g_PostSLConfirmedRenderInCurrentReactivationEpoch{false};
static std::atomic<bool> g_PostSLSyntheticStartupActivatedButUnconfirmed{false};
static std::atomic<bool> g_PostSLRuntimeStateStabilizationLogged{false};
// A reactivated PostSL startup that had already confirmed a few frames but had
// not yet reached the repo's broader warmup proof threshold can still inherit
// stale Streamline OFF churn from the earlier epoch. Keep only the narrow
// runtime-state stale-OFF guard extended for that new epoch.
static std::atomic<bool> g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch{false};
static std::atomic<bool> g_PreferredOverlayFGPublicationStateValid{false};
static std::atomic<bool> g_PreferredOverlayFGPublicationStateActive{false};
static std::atomic<int> g_PreferredOverlayFGPublicationStateRuntimeMode{
    static_cast<int>(ce::fg_runtime::RuntimeMode::kOff)};
static std::atomic<uint64_t> g_OverlayFGPublicationSequence{0};
static std::atomic<uint64_t> g_PreferredOverlayFGPublicationStateSequence{0};

uint64_t HookAllocateOverlayFGPublicationSequence() {
    return g_OverlayFGPublicationSequence.fetch_add(1, std::memory_order_acq_rel) + 1;
}

void HookUpdatePreferredOverlayFGPublicationState(bool active, ce::fg_runtime::RuntimeMode runtimeMode,
                                                  const char* source) {
    const bool previousValid = g_PreferredOverlayFGPublicationStateValid.load(std::memory_order_acquire);
    const bool previousActive = g_PreferredOverlayFGPublicationStateActive.load(std::memory_order_acquire);
    const auto previousRuntimeMode = static_cast<ce::fg_runtime::RuntimeMode>(
        g_PreferredOverlayFGPublicationStateRuntimeMode.load(std::memory_order_acquire));
    const uint64_t nextSequence = HookAllocateOverlayFGPublicationSequence();

    g_PreferredOverlayFGPublicationStateActive.store(active, std::memory_order_release);
    g_PreferredOverlayFGPublicationStateRuntimeMode.store(static_cast<int>(runtimeMode), std::memory_order_release);
    g_PreferredOverlayFGPublicationStateSequence.store(nextSequence, std::memory_order_release);
    g_PreferredOverlayFGPublicationStateValid.store(true, std::memory_order_release);

    if (!previousValid || previousActive != active || previousRuntimeMode != runtimeMode) {
        HookLogImportant("FG publication preferred state: source=%s runtime=%s active=%d sequence=%llu",
                         source ? source : "unknown", ce::fg_runtime::GetRuntimeModeName(runtimeMode), active ? 1 : 0,
                         static_cast<unsigned long long>(nextSequence));
    }
}

bool HookTryGetPreferredOverlayFGPublicationState(PreferredOverlayFGPublicationState* state) {
    if (!state) {
        return false;
    }

    if (g_PreferredOverlayFGPublicationStateValid.load(std::memory_order_acquire)) {
        state->valid = true;
        state->active = g_PreferredOverlayFGPublicationStateActive.load(std::memory_order_acquire);
        state->runtimeMode = static_cast<ce::fg_runtime::RuntimeMode>(
            g_PreferredOverlayFGPublicationStateRuntimeMode.load(std::memory_order_acquire));
        state->sequence = g_PreferredOverlayFGPublicationStateSequence.load(std::memory_order_acquire);
        return true;
    }

    state->valid = true;
    state->active = g_FGCompat.IsFGActive();
    state->runtimeMode = g_FGCompat.GetRuntimeMode();
    state->sequence = 0;
    return true;
}

static const char* DX12OverlayRenderRouteName(uint32_t route) {
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

// ---------------------------------------------------------------------------
// [OVERLAY COVERAGE] per-present overlay-coverage telemetry (regression gate).
// ---------------------------------------------------------------------------
// Every overlay draw of any route (normal, PostSL, FFX present callback) bumps
// g_OverlayCoverageDrawCount via NoteDX12OverlayRendered. Two accounting event
// streams consume that counter and classify each present as covered/uncovered:
//   1. DX12_ProcessFrameExternal — top-level processed presents (normal, FSR
//      no-callback, FFX-callback and post-FG recovery transports).
//   2. PostSLOverlayRenderGated — SL-routed presents (synthetic re-entrant /
//      startup normal-route callbacks), which bypass ProcessFrameExternal.
// Presents whose visible overlay is composed by the FG runtime from a previous
// covered present (zero-ECL interpolated frames; SL-owned top-level transport
// presents) inherit coverage while no uncovered streak is active, so healthy FG
// sessions stay noise-free and real blanks form one continuous streak.
// Gate attribution: skip sites record a static reason string; the gate captured
// at streak start is reported when the streak ends. Atomics + a tiny spin lock
// only; logging happens at streak boundaries and FG transition edges, never per
// frame.
static std::atomic<uint64_t> g_OverlayCoverageDrawCount{0};
static std::atomic<uint64_t> g_OverlayCoverageLastSeenDrawCount{0};
static std::atomic<const char*> g_OverlayCoverageLastGate{nullptr};
static std::atomic<const char*> g_OverlayCoverageStreakGate{nullptr};
// Streak-onset markers so a blank window is bracketed start+end in the log: wall-clock
// tick (ms) and whether PostSL was already CONFIRMED rendering when the streak began
// (confirmed=1 at start means a live overlay got blanked — a no-blank-principle violation).
static std::atomic<uint64_t> g_OverlayCoverageStreakStartTickMs{0};
static std::atomic<bool> g_OverlayCoverageStreakStartConfirmed{false};
static ce::dx12_overlay_policy::OverlayPresentCoverageTracker g_OverlayCoverageTracker;
static std::atomic_flag g_OverlayCoverageLock = ATOMIC_FLAG_INIT;
static thread_local bool g_RequireExactPostSLStartupTransportDraw = false;
static thread_local bool g_PostSLDrawBelongsToEnclosingProcessFramePresent = false;

// Verbose overlay-handoff diagnostic window. The [OVERLAY COVERAGE] streak gate only reports a blank
// when a present is UNCOVERED, but an off->DLSS engage flash can sit BELOW that: every present is
// "covered" (real draw or FG-composed inheritance) yet a brand-new DLSS-G proxy can still show a
// generated frame whose overlay history is empty. When a PostSL reactivation arms this window, the
// next N presents log per-present coverage detail (real-draw vs inherited-if-no-draw vs uncovered,
// route, source) so the exact handoff frame is attributable. prevRoute captured at arming separates
// off->DLSS (prevRoute=normal, native->fresh-proxy) from FSR->DLSS (prevRoute=post-sl/ffx, warm proxy).
static std::atomic<int> g_OverlayHandoffVerboseLogPresents{0};
static std::atomic<uint32_t> g_OverlayHandoffVerbosePrevRoute{0};

static void NoteDX12OverlayCoverageGate(const char* gate) {
    g_OverlayCoverageLastGate.store(gate, std::memory_order_relaxed);
}

struct DX12OverlayCoverageSnapshot {
    uint64_t totalPresents = 0;
    uint64_t uncoveredPresents = 0;
    uint64_t currentStreak = 0;
    uint64_t longestStreak = 0;
};

static DX12OverlayCoverageSnapshot GetOverlayCoverageSnapshot() {
    DX12OverlayCoverageSnapshot snapshot;
    while (g_OverlayCoverageLock.test_and_set(std::memory_order_acquire)) {
        YieldProcessor();
    }
    snapshot.totalPresents = g_OverlayCoverageTracker.TotalPresents();
    snapshot.uncoveredPresents = g_OverlayCoverageTracker.UncoveredPresents();
    snapshot.currentStreak = g_OverlayCoverageTracker.CurrentUncoveredStreak();
    snapshot.longestStreak = g_OverlayCoverageTracker.LongestUncoveredStreak();
    g_OverlayCoverageLock.clear(std::memory_order_release);
    return snapshot;
}

static const char* DX12OverlayRenderRouteName(uint32_t route);

// Accounts one presented frame. covered = draw-counter delta since the previous
// accounted present (any route), with FG-composed inheritance (see block comment).
static void AccountPresentForOverlayCoverage(bool inheritCoverageIfNoDraw, const char* source) {
    const uint64_t draws = g_OverlayCoverageDrawCount.load(std::memory_order_acquire);
    // Visibility cannot be interrupted before CE has established its first
    // visible overlay draw. Excluding pre-initialization Presents keeps later
    // transition summaries and interruption markers semantically precise.
    if (!ce::dx12_overlay_policy::ShouldAccountOverlayVisibilityPresent(draws)) {
        return;
    }
    const uint64_t lastSeen = g_OverlayCoverageLastSeenDrawCount.exchange(draws, std::memory_order_acq_rel);
    const bool drawObserved = draws != lastSeen;

    ce::dx12_overlay_policy::OverlayPresentCoverageResult result;
    DX12OverlayCoverageSnapshot snapshot;
    while (g_OverlayCoverageLock.test_and_set(std::memory_order_acquire)) {
        YieldProcessor();
    }
    result = g_OverlayCoverageTracker.NotePresent(drawObserved, inheritCoverageIfNoDraw);
    snapshot.totalPresents = g_OverlayCoverageTracker.TotalPresents();
    snapshot.uncoveredPresents = g_OverlayCoverageTracker.UncoveredPresents();
    snapshot.currentStreak = g_OverlayCoverageTracker.CurrentUncoveredStreak();
    snapshot.longestStreak = g_OverlayCoverageTracker.LongestUncoveredStreak();
    g_OverlayCoverageLock.clear(std::memory_order_release);

    // Verbose overlay-handoff diagnostic: per-present detail for the first N presents after a PostSL
    // reactivation. `drawObserved=0 inheritIfNoDraw=1` (covered ONLY by FG-composed inheritance) is the
    // smoking gun for an off->DLSS fresh-proxy flash — DLSS-G presented a generated frame relying on a
    // proxy whose overlay history is still empty. A real draw shows `drawObserved=1`.
    {
        int verboseRemaining = g_OverlayHandoffVerboseLogPresents.load(std::memory_order_relaxed);
        if (verboseRemaining > 0) {
            g_OverlayHandoffVerboseLogPresents.store(verboseRemaining - 1, std::memory_order_relaxed);
            const uint32_t route = g_LastDX12OverlayRenderRoute.load(std::memory_order_acquire);
            const uint32_t prevRoute = g_OverlayHandoffVerbosePrevRoute.load(std::memory_order_relaxed);
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
        const char* streakGate = g_OverlayCoverageLastGate.load(std::memory_order_relaxed);
        g_OverlayCoverageStreakGate.store(streakGate, std::memory_order_relaxed);
        const uint64_t startTick = GetTickCount64();
        g_OverlayCoverageStreakStartTickMs.store(startTick, std::memory_order_relaxed);
        const bool startConfirmed = g_PostSLConfirmedRendering.load(std::memory_order_acquire);
        g_OverlayCoverageStreakStartConfirmed.store(startConfirmed, std::memory_order_relaxed);
        // Bracket the onset of every blank window with a timestamped marker so even a
        // single-present gap is fully attributable from the log alone.
        static std::atomic<int> s_streakStartLogCount{0};
        const int startLogCount = s_streakStartLogCount.fetch_add(1, std::memory_order_relaxed);
        if (startLogCount < 100 || (startLogCount % 20) == 0) {
            const uint32_t route = g_LastDX12OverlayRenderRoute.load(std::memory_order_acquire);
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
            const char* streakGate = g_OverlayCoverageStreakGate.load(std::memory_order_relaxed);
            const char* lastGate = g_OverlayCoverageLastGate.load(std::memory_order_relaxed);
            const uint32_t route = g_LastDX12OverlayRenderRoute.load(std::memory_order_acquire);
            const uint64_t startTick = g_OverlayCoverageStreakStartTickMs.load(std::memory_order_relaxed);
            const uint64_t durationMs = startTick ? (GetTickCount64() - startTick) : 0;
            const bool confirmedDuringStreak = g_OverlayCoverageStreakStartConfirmed.load(std::memory_order_relaxed);
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

void DX12_AccountOverlayTransportPresent(bool inheritCoverageIfNoDraw, const char* gate, const char* source) {
    NoteDX12OverlayCoverageGate(gate ? gate : "transport-present-uncovered");
    AccountPresentForOverlayCoverage(inheritCoverageIfNoDraw, source ? source : "transport-present");
}

bool DX12_TryRenderExactPostSLBeforeStartupHandoffPresent(IDXGISwapChain* pSwapChain, const char* source) {
    auto postSLCallback = DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
    if (!pSwapChain || !postSLCallback || g_RequireExactPostSLStartupTransportDraw) {
        return false;
    }

    const uint64_t drawsBefore = g_OverlayCoverageDrawCount.load(std::memory_order_acquire);
    g_RequireExactPostSLStartupTransportDraw = true;
    postSLCallback(pSwapChain);
    g_RequireExactPostSLStartupTransportDraw = false;
    const bool drawn = g_OverlayCoverageDrawCount.load(std::memory_order_acquire) != drawsBefore;

    static std::atomic<int> s_exactStartupTransportDrawLogCount{0};
    const int logCount = s_exactStartupTransportDrawLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (logCount <= 20 || (logCount % 120) == 0 || !drawn) {
        HookLogImportant(
            "[OVERLAY VISIBILITY] exact PostSL startup-transport draw %s before Present "
            "(source=%s swapchain=%p activeOfficialUiCoverage=%d log=%d)",
            drawn ? "SUBMITTED" : "MISSED", source ? source : "unknown", pSwapChain,
            ce::dx12_streamline_ui_overlay::HasActiveCoverage() ? 1 : 0, logCount);
    }
    return drawn;
}

// Logs a coverage summary line. Called at FG transition edges and shutdown so
// the scripted transition matrix can gate on "no uncovered streak > 1 present".
static void LogOverlayCoverageSummary(const char* edge) {
    const DX12OverlayCoverageSnapshot snapshot = GetOverlayCoverageSnapshot();
    const char* lastGate = g_OverlayCoverageLastGate.load(std::memory_order_relaxed);
    const uint32_t route = g_LastDX12OverlayRenderRoute.load(std::memory_order_acquire);
    HookLogImportant(
        "[OVERLAY COVERAGE] %s: presents=%llu uncovered=%llu currentStreak=%llu longestStreak=%llu lastGate=%s "
        "lastRoute=%s",
        edge ? edge : "summary", static_cast<unsigned long long>(snapshot.totalPresents),
        static_cast<unsigned long long>(snapshot.uncoveredPresents),
        static_cast<unsigned long long>(snapshot.currentStreak),
        static_cast<unsigned long long>(snapshot.longestStreak), lastGate ? lastGate : "none",
        DX12OverlayRenderRouteName(route));
}

static void NoteDX12OverlayRendered(DX12OverlayRenderRoute route) {
    const uint64_t drawsBefore = g_OverlayCoverageDrawCount.fetch_add(1, std::memory_order_acq_rel);
    const uint32_t previousRoute =
        g_LastDX12OverlayRenderRoute.exchange(static_cast<uint32_t>(route), std::memory_order_acq_rel);
    g_LastDX12OverlayRenderTickMs.store(GetTickCount64(), std::memory_order_release);
    // [OVERLAY DOUBLE-DRAW] detector: a draw already happened since the last ACCOUNTED present
    // (drawsBefore > lastSeen) and it came from a DIFFERENT route — i.e. two overlay routes rendered
    // within the same present window. One route re-drawing is benign; two different routes can show the
    // overlay TWICE on screen (e.g. the FFX UI-composite prework and PostSL backbuffer rendering were both
    // live for ~3.5s during the GTA FSR->DLSS pre-apply window, session 20260702_092933). Diagnostic only —
    // makes route-arbitration overlaps attributable from one run; visible flicker/dimming correlates here.
    const uint64_t lastAccountedDraws = g_OverlayCoverageLastSeenDrawCount.load(std::memory_order_acquire);
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

static void LogDX12OverlayVisibilityGap(const char* context, const char* reason, ULONGLONG warnAfterMs = 250) {
    const ULONGLONG lastRenderMs = g_LastDX12OverlayRenderTickMs.load(std::memory_order_acquire);
    if (!lastRenderMs) {
        return;
    }

    const ULONGLONG nowMs = GetTickCount64();
    if (nowMs < lastRenderMs || nowMs - lastRenderMs < warnAfterMs) {
        return;
    }

    static std::atomic<int> s_visibilityGapLogCount{0};
    const int logCount = s_visibilityGapLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 300) == 0) {
        const uint32_t route = g_LastDX12OverlayRenderRoute.load(std::memory_order_acquire);
        HookLogImportant("DX12: Overlay visibility gap while %s (%s) — lastRenderAge=%llums lastRoute=%s log=%d",
                         context && context[0] ? context : "transitioning",
                         reason && reason[0] ? reason : "waiting for safe render route",
                         static_cast<unsigned long long>(nowMs - lastRenderMs), DX12OverlayRenderRouteName(route),
                         logCount + 1);
    }
}

bool HookIsPostSLOverlayActiveButUnconfirmed() {
    return g_PostSLSyntheticStartupActivatedButUnconfirmed.load(std::memory_order_acquire) ||
           (g_PostSLOverlayActive.load(std::memory_order_acquire) &&
            !g_PostSLConfirmedRendering.load(std::memory_order_acquire));
}

bool HookHasPostSLSyntheticStartupActivationEntered() {
    return g_PostSLSyntheticStartupActivatedButUnconfirmed.load(std::memory_order_acquire);
}

bool HookIsPostSLOverlayConfirmedRendering() {
    return g_PostSLConfirmedRendering.load(std::memory_order_acquire);
}

// Counts Present calls where PostSL was expected but didn't fire.
// ProcessFrame increments this; PostSLOverlayRender resets it to 0.
// When it exceeds kPostSLStallThreshold (5), pre-SL rendering is allowed
// as a fallback for "FG suspension" (SL FG nominally on, but not generating
// re-entrant Present calls — e.g., game menu/pause).
//
// CONTEXT: During DLSS FG, SL generates re-entrant Present calls from worker
// threads for each interpolated frame.  Our PostSL callback renders the overlay
// in these re-entrant calls.  But when the game enters a menu or pause state,
// SL may stop generating FG frames while g_StreamlineFGRunning stays true
// (slDLSSGSetOptions isn't called with mode=0).  In this state:
//   - Pre-SL rendering is suppressed (g_StreamlineFGRunning = true)
//   - PostSL never fires (no re-entrant Present from SL)
//   - Result: overlay gap with BOTH paths blocked
//
// The stall counter detects this gap and temporarily allows pre-SL rendering.
// When PostSL fires again (FG resumes), it resets the counter and takes over.
//
// COMPATIBILITY: Tested in GTA V Enhanced (menu pauses FG) and Talos Principle
// Reawakened (continuous FG).  The threshold of 5 frames ensures PostSL has
// enough time to fire during normal FG before pre-SL takes over.
static std::atomic<int> g_PostSLStallCounter{0};

// Counts consecutive successful PostSL renders since the last FG transition.
// Incremented by PostSLOverlayRender, reset to 0 by FG transition handler.
// The stall fallback is only enabled once this exceeds kPostSLWarmupThreshold,
// preventing pre-SL rendering during FG warmup (SL pipeline still initializing).
//
// PROBLEM: After FG OFF→ON (menu close), SL generates ONE re-entrant Present
// immediately (setting PostSLConfirmed=true), then stalls briefly while the FG
// pipeline warms up.  Without this counter, the stall fallback fires during
// warmup and renders on origGame → DEVICE_HUNG (cross-queue backbuffer access).
static std::atomic<int> g_PostSLStableFrameCount{0};

bool HookIsPostSLOverlayConfirmedButStartupSettling() {
    return ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsStartupSettling(
        g_PostSLConfirmedRendering.load(std::memory_order_acquire),
        g_PostSLStableFrameCount.load(std::memory_order_acquire));
