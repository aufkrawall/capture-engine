#include "dxgi_shared_internal.h"
#include "fg_cost_probe.h"
#include "hook_cpu_cost.h"

#include "../wrappers/vulkan_dxgi_fifo_present.h"

#ifdef BUILDING_CAPTURE_HOOK
extern "C" void DX12_WaitForOverlayCompletion(ID3D12CommandQueue* pQueue);
extern "C" void DX12_FlushDeferredSignal();
extern "C" void DX12_SetDeferOverlaySubmitToSteamECL(bool defer);
extern "C" void DX12_SubmitSteamDeferredOverlay();
extern "C" bool DX12_IsDeferOverlaySubmitPending();
extern "C" void DX12_NoteWrappedD3D12PresentResult(const char* presentName, int callCount, UINT syncInterval,
                                                   UINT presentFlags, HRESULT presentHr, BOOL isFullscreen,
                                                   BOOL isIconic, BOOL hasZeroSize, HWND gameWindow);

void InvokeDX12WaitForOverlayCompletion(ID3D12CommandQueue* pQueue) {
    DX12_WaitForOverlayCompletion(pQueue);
}
void InvokeDX12FlushDeferredSignal() {
    DX12_FlushDeferredSignal();
}

// Feed the DX12 present result into the focus-transition / occlusion tracking for the vtable
// DetourPresent path. The CWrapDXGISwapChain wrapper already calls
// DX12_NoteWrappedD3D12PresentResult itself; IsInWrapperPresent() gives exactly-once
// semantics (the wrapper keeps g_InWrapperPresent set across its real Present, so a re-entry
// here is skipped; the delegated external-overlay path leaves it false and relies on this).
// Without this, vtable-hooked apps (e.g. dx12_test) never update g_SwapchainPresentOccluded
// and never engage the invisible-safe not-presentable backbuffer-work hold during the Alt+Tab
// iflip<->composited mode switch, so the overlay touches the backbuffer mid-switch and the GPU
// hangs (DEVICE_HUNG).
void NoteDX12PresentResultForVtablePath(IDXGISwapChain* pSwapChain, const char* presentName, UINT SyncInterval,
                                               UINT Flags, HRESULT hr) {
    if (!pSwapChain || IsInWrapperPresent()) {
        return;
    }
    static std::atomic<int> s_vtablePresentResultCount{0};
    const int callCount = s_vtablePresentResultCount.fetch_add(1, std::memory_order_relaxed) + 1;
    DXGI_SWAP_CHAIN_DESC desc = {};
    HWND hwnd = nullptr;
    BOOL isFullscreen = FALSE;
    BOOL isIconic = FALSE;
    BOOL hasZeroSize = FALSE;
    if (SUCCEEDED(pSwapChain->GetDesc(&desc))) {
        hwnd = desc.OutputWindow;
        isFullscreen = desc.Windowed ? FALSE : TRUE;  // borderless-fullscreen reports Windowed=TRUE
        hasZeroSize = (desc.BufferDesc.Width == 0 || desc.BufferDesc.Height == 0) ? TRUE : FALSE;
        isIconic = (hwnd && IsIconic(hwnd)) ? TRUE : FALSE;
    }
    DX12_NoteWrappedD3D12PresentResult(presentName, callCount, SyncInterval, Flags, hr, isFullscreen, isIconic,
                                       hasZeroSize, hwnd);
}

// Inline wrappers for the new Steam ECL deferred overlay functions.
// Since BUILDING_CAPTURE_HOOK is defined, these are direct calls to the exports.
static void InvokeDX12SetDeferOverlaySubmitToSteamECL(bool defer) {
    DX12_SetDeferOverlaySubmitToSteamECL(defer);
}
static void InvokeDX12SubmitSteamDeferredOverlay() {
    DX12_SubmitSteamDeferredOverlay();
}
static bool InvokeDX12IsDeferOverlaySubmitPending() {
    return DX12_IsDeferOverlaySubmitPending();
}
#else
using PFN_DX12WaitForOverlayCompletion = void (*)(ID3D12CommandQueue* pQueue);
using PFN_DX12FlushDeferredSignal = void (*)();

static PFN_DX12WaitForOverlayCompletion ResolveDX12WaitForOverlayCompletion() {
    static std::once_flag s_once;
    static PFN_DX12WaitForOverlayCompletion s_fn = nullptr;

    std::call_once(s_once, []() {
        HMODULE hHook = GetModuleHandleA("capture_hook_x64.dll");
        if (!hHook) {
            hHook = GetModuleHandleA("capture_hook_x86.dll");
        }
        if (hHook) {
            s_fn = reinterpret_cast<PFN_DX12WaitForOverlayCompletion>(
                GetProcAddress(hHook, "DX12_WaitForOverlayCompletion"));
        }
    });

    return s_fn;
}

static PFN_DX12FlushDeferredSignal ResolveDX12FlushDeferredSignal() {
    static std::once_flag s_once;
    static PFN_DX12FlushDeferredSignal s_fn = nullptr;

    std::call_once(s_once, []() {
        HMODULE hHook = GetModuleHandleA("capture_hook_x64.dll");
        if (!hHook) {
            hHook = GetModuleHandleA("capture_hook_x86.dll");
        }
        if (hHook) {
            s_fn = reinterpret_cast<PFN_DX12FlushDeferredSignal>(GetProcAddress(hHook, "DX12_FlushDeferredSignal"));
        }
    });

    return s_fn;
}

void InvokeDX12WaitForOverlayCompletion(ID3D12CommandQueue* pQueue) {
    PFN_DX12WaitForOverlayCompletion fn = ResolveDX12WaitForOverlayCompletion();
    if (fn) {
        fn(pQueue);
    }
}

void InvokeDX12FlushDeferredSignal() {
    PFN_DX12FlushDeferredSignal fn = ResolveDX12FlushDeferredSignal();
    if (fn) {
        fn();
    }
}

// Steam ECL deferred overlay functions (stubs for non-hook builds).
// In the test stub build these should never be called meaningfully.
using PFN_DX12SetDeferOverlay = void (*)(bool);
using PFN_DX12SubmitDeferredOverlay = void (*)();
using PFN_DX12IsDeferOverlayPending = bool (*)();

static PFN_DX12SetDeferOverlay ResolveDX12SetDeferOverlay() {
    static std::once_flag s_once;
    static PFN_DX12SetDeferOverlay s_fn = nullptr;
    std::call_once(s_once, []() {
        HMODULE hHook = GetModuleHandleA("capture_hook_x64.dll");
        if (!hHook)
            hHook = GetModuleHandleA("capture_hook_x86.dll");
        if (hHook) {
            s_fn = reinterpret_cast<PFN_DX12SetDeferOverlay>(
                GetProcAddress(hHook, "DX12_SetDeferOverlaySubmitToSteamECL"));
        }
    });
    return s_fn;
}

static PFN_DX12SubmitDeferredOverlay ResolveDX12SubmitSteamDeferredOverlay() {
    static std::once_flag s_once;
    static PFN_DX12SubmitDeferredOverlay s_fn = nullptr;
    std::call_once(s_once, []() {
        HMODULE hHook = GetModuleHandleA("capture_hook_x64.dll");
        if (!hHook)
            hHook = GetModuleHandleA("capture_hook_x86.dll");
        if (hHook) {
            s_fn = reinterpret_cast<PFN_DX12SubmitDeferredOverlay>(
                GetProcAddress(hHook, "DX12_SubmitSteamDeferredOverlay"));
        }
    });
    return s_fn;
}

static PFN_DX12IsDeferOverlayPending ResolveDX12IsDeferOverlayPending() {
    static std::once_flag s_once;
    static PFN_DX12IsDeferOverlayPending s_fn = nullptr;
    std::call_once(s_once, []() {
        HMODULE hHook = GetModuleHandleA("capture_hook_x64.dll");
        if (!hHook)
            hHook = GetModuleHandleA("capture_hook_x86.dll");
        if (hHook) {
            s_fn = reinterpret_cast<PFN_DX12IsDeferOverlayPending>(
                GetProcAddress(hHook, "DX12_IsDeferOverlaySubmitPending"));
        }
    });
    return s_fn;
}

static void InvokeDX12SetDeferOverlaySubmitToSteamECL(bool defer) {
    PFN_DX12SetDeferOverlay fn = ResolveDX12SetDeferOverlay();
    if (fn)
        fn(defer);
}
static void InvokeDX12SubmitSteamDeferredOverlay() {
    PFN_DX12SubmitDeferredOverlay fn = ResolveDX12SubmitSteamDeferredOverlay();
    if (fn)
        fn();
}
static bool InvokeDX12IsDeferOverlaySubmitPending() {
    PFN_DX12IsDeferOverlayPending fn = ResolveDX12IsDeferOverlayPending();
    return fn ? fn() : false;
}

// Stub for non-hook/test builds; present-result occlusion tracking lives in the hook DLL.
void NoteDX12PresentResultForVtablePath(IDXGISwapChain*, const char*, UINT, UINT, HRESULT) {}
#endif

namespace DXGIShared {
bool IsRecursiveResize() {
    DWORD currentId = GetCurrentThreadId();
    DWORD expected = dxgi_shared_g_resizeThreadId.load(std::memory_order_acquire);

    if (expected == currentId && dxgi_shared_g_resizeDepth.load(std::memory_order_acquire) > 0) {
        return true;
    }

    DWORD zero = 0;
    if (dxgi_shared_g_resizeThreadId.compare_exchange_strong(zero, currentId, std::memory_order_acq_rel)) {
        dxgi_shared_g_resizeDepth.store(1, std::memory_order_release);
        return false;
    }

    if (dxgi_shared_g_resizeThreadId.load(std::memory_order_acquire) == currentId) {
        dxgi_shared_g_resizeDepth.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }

    return false;
}
}

namespace DXGIShared {
void ReleaseResize() {
    if (dxgi_shared_g_resizeDepth.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        dxgi_shared_g_resizeThreadId.store(0, std::memory_order_release);
    }
}
}

namespace DXGIShared {
bool ShouldBypassDX12InvisibleWindowPresent(IDXGISwapChain* pSwapChain, const char* presentName) {
    if (!pSwapChain) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(pSwapChain->GetDesc(&desc))) {
        return false;
    }

    const bool hasOutputWindow = desc.OutputWindow != nullptr;
    const bool outputWindowVisible = hasOutputWindow && IsWindowVisible(desc.OutputWindow) != FALSE;
    if (!ce::dx12_overlay_policy::ShouldSkipDX12PresentProcessingForInvisibleWindowSwapchain(hasOutputWindow,
                                                                                             outputWindowVisible)) {
        return false;
    }

    static std::atomic<int> s_invisibleWindowPresentBypassLogCount{0};
    const int logCount = s_invisibleWindowPresentBypassLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 256) == 0) {
        HookLogImportant(
            "%s: Bypassing CE DX12 Present processing for invisible-window helper swapchain "
            "(sc=%p hwnd=%p size=%ux%u count=%d)",
            presentName ? presentName : "DetourPresent", pSwapChain, desc.OutputWindow, desc.BufferDesc.Width,
            desc.BufferDesc.Height, logCount + 1);
    }
    return true;
}
}

namespace DXGIShared {
PFN_Present dxgi_shared_oPresent = nullptr;
}

namespace DXGIShared {
PFN_Present1 dxgi_shared_oPresent1 = nullptr;
}

namespace DXGIShared {
PFN_ResizeBuffers dxgi_shared_oResizeBuffers = nullptr;
}

namespace DXGIShared {
PFN_ResizeBuffers1 dxgi_shared_oResizeBuffers1 = nullptr;
}

namespace DXGIShared {
// Inline hook trampolines - calling these bypasses the hook entirely
PFN_Present dxgi_shared_oPresentTrampoline = nullptr;
}

namespace DXGIShared {
PFN_Present1 dxgi_shared_oPresent1Trampoline = nullptr;
}

namespace DXGIShared {
std::atomic<PFN_SetColorSpace1> dxgi_shared_oSetColorSpace1Trampoline{nullptr};
}

namespace DXGIShared {
std::mutex dxgi_shared_s_setColorSpace1HookMutex;
}

namespace {
// The DXGI/D3D dispatch modules that can sit between two overlays in a Present chain. They are
// never the originator of a present, so the originator walk steps over them.
bool IsGraphicsDispatchModulePath(const char* modulePath) {
    if (!modulePath || !modulePath[0]) {
        return false;
    }
    const char* baseName = modulePath;
    for (const char* cursor = modulePath; *cursor; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            baseName = cursor + 1;
        }
    }
    static const char* const kDispatchModules[] = {"dxgi.dll",   "d3d12.dll",     "d3d12core.dll",
                                                   "d3d11.dll",  "d3d11on12.dll", "d3d10.dll",
                                                   "d3d9.dll",   "dcomp.dll",     "dwmapi.dll"};
    for (const char* candidate : kDispatchModules) {
        if (_stricmp(baseName, candidate) == 0) {
            return true;
        }
    }
    return false;
}

// CE's own module, resolved from a CE code address so this unit carries no link dependency on
// the injected DLL's entry point.
HMODULE CaptureEngineModuleHandle() {
    static HMODULE s_module = [] {
        HMODULE module = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&IsGraphicsDispatchModulePath), &module);
        return module;
    }();
    return s_module;
}
}  // namespace

namespace DXGIShared {
// Below a foreign Present chain, the immediate caller is always the last foreign overlay in
// it, so FG provenance has to come from the frames above them. Walking out past CE's own
// frames, DXGI, and the tracked foreign overlays lands on the ORIGINATOR — typically three to
// five frames — and one pass answers both FG questions. Deliberately not a full-stack scan
// per module: address->module resolution takes the loader lock, and this runs on the Present
// hot path.
void ResolvePresentOriginatorBelowForeignChain(bool* fromStreamlineOut, bool* fromFFXFrameGenerationOut) {
    if (fromStreamlineOut) {
        *fromStreamlineOut = false;
    }
    if (fromFFXFrameGenerationOut) {
        *fromFFXFrameGenerationOut = false;
    }

    constexpr USHORT kMaxFrames = 16;
    void* stackFrames[kMaxFrames] = {};
    const USHORT frameCount = CaptureStackBackTrace(1, kMaxFrames, stackFrames, nullptr);
    for (USHORT i = 0; i < frameCount; ++i) {
        HMODULE frameModule = nullptr;
        char framePath[MAX_PATH] = {};
        if (!TryGetModulePathFromCodeAddress(stackFrames[i], framePath, sizeof(framePath), &frameModule) ||
            !frameModule) {
            continue;  // Trampoline / JIT-style thunk: not a module frame, keep walking out.
        }
        if (frameModule == CaptureEngineModuleHandle()) {
            continue;  // CE's own wrapper/detour frames.
        }
        if (ce::overlay_compat::IsThirdPartyOverlayModulePath(framePath)) {
            continue;  // Steam / RTSS: the chain CE deliberately sits below.
        }
        if (IsGraphicsDispatchModulePath(framePath)) {
            continue;  // DXGI/D3D dispatch frames between the overlays.
        }

        if (fromStreamlineOut && IsStreamlineModuleHandle(frameModule)) {
            *fromStreamlineOut = true;
        }
        if (fromFFXFrameGenerationOut && ce::overlay_compat::IsFFXFrameGenerationModulePath(framePath)) {
            *fromFFXFrameGenerationOut = true;
        }
        return;  // First real originator decides; frames above it are its own callers.
    }
}
}

namespace DXGIShared {
PresentCallContext CapturePresentCallContext(IDXGISwapChain* pSwapChain,
                                                    const void* detourCallerAddress, APIType api,
                                                    bool presentBypassAvailable) {
    PresentCallContext ctx;
    ctx.api = api;
    ctx.wrappedSwapchain = IsWrappedSwapChainObject(pSwapChain);
    ctx.inWrapperPresent = IsInWrapperPresent();
    ctx.streamlineFGRunning = g_StreamlineFGRunning.load(std::memory_order_acquire);
    ctx.currentThreadId = GetCurrentThreadId();
    ctx.steamOverlayLoaded = IsSteamOverlayModule(ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName());
    ctx.presentBypassAvailable = presentBypassAvailable;
    // "A third-party overlay made this call" is an inference from the immediate caller, and it
    // only holds while CE is at the TOP of the Present chain. When CE intercepts below a
    // foreign chain (deep body hook), the caller of dxgi!Present is ALWAYS the last foreign
    // overlay in it — including for the game's own swapchain — so the inference is
    // structurally false and would bypass CE's overlay on every single frame (session
    // 20260812_144425: caller=RTSSHooks64.dll on the game swapchain, CE overlay gone from the
    // frame the deep hook took over). Swapchain identity stays authoritative there.
    ctx.callerFromThirdPartyOverlay =
        TryGetModulePathFromCodeAddress(detourCallerAddress, ctx.detourCallerModulePath,
                                        sizeof(ctx.detourCallerModulePath)) &&
        ce::overlay_compat::IsThirdPartyOverlayModulePath(ctx.detourCallerModulePath) &&
        !IsPresentInterceptedBelowForeignChain();
    ctx.streamlineStartupHandoffPending = (ctx.api == APIType::D3D12) && IsStreamlineStartupHandoffPending();
    ctx.streamlineStartupTransitionWindowActive = (ctx.api == APIType::D3D12) && IsStreamlineStartupTransitionWindowActive();
    ctx.streamlineStartupHandoffInProgress = ctx.streamlineStartupHandoffPending || ctx.streamlineStartupTransitionWindowActive;
    ctx.presentOwner = dxgi_shared_g_presentThreadId.load(std::memory_order_relaxed);
    ctx.presentDepthVal = dxgi_shared_g_presentDepth.load(std::memory_order_relaxed);
    ctx.presentOwnershipActive = ctx.presentOwner != 0 || ctx.presentDepthVal > 0;
    ctx.expectedPresentThreadId = g_RenderWatchdog.GetMonitoredThreadId();
    ctx.matchesExpectedPresentThread = ctx.expectedPresentThreadId == 0 || ctx.expectedPresentThreadId == ctx.currentThreadId;
    // FG provenance has the same problem as the overlay classification above, with the
    // opposite sign: below the chain the immediate caller is a foreign overlay, so an
    // interposer-originated present would read as NOT interposer-originated. The originator is
    // still on the stack, just a few frames further out, so resolve it there in that mode.
    bool originatorFromStreamline = false;
    bool originatorFromFFXFrameGeneration = false;
    if (IsPresentInterceptedBelowForeignChain()) {
        ResolvePresentOriginatorBelowForeignChain(&originatorFromStreamline, &originatorFromFFXFrameGeneration);
    }
    ctx.callerFromStreamlineModule = originatorFromStreamline || IsCodeAddressFromStreamlineModule(detourCallerAddress);
    ctx.callerFromFFXFrameGenerationModule =
        originatorFromFFXFrameGeneration ||
        ce::overlay_compat::IsCodeAddressFromFFXFrameGenerationModule(detourCallerAddress);
    ctx.recentLargePresentGap = HasRecentLargePresentGap(500);
    ctx.startupTopLevelPresentAlreadyConsumed = g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire);
    ctx.postSLStartupActivationPending = g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    ctx.postSLActiveButUnconfirmed = ctx.api == APIType::D3D12 && HookIsPostSLOverlayActiveButUnconfirmed();
    ctx.postSLStartupActivationEntered = ctx.api == APIType::D3D12 && HookHasPostSLSyntheticStartupActivationEntered();
    ctx.postSLConfirmedRendering = ctx.api == APIType::D3D12 && HookIsPostSLOverlayConfirmedRendering();
    ctx.postSLConfirmedButStartupSettling = ctx.api == APIType::D3D12 && HookIsPostSLOverlayConfirmedButStartupSettling();
    ctx.hadFSRFGPhase = ctx.api == APIType::D3D12 && HookHasFSRFGHistory();
    ctx.explicitSetOptionsActivation = ctx.api == APIType::D3D12 && HookHasExplicitStreamlineSetOptionsActivation();
    ctx.activeDLSSFGRuntimeSignalObserved = ctx.api == APIType::D3D12 && g_StreamlineFGRunning.load(std::memory_order_acquire);
    ctx.safePostFSRBootstrapPath = ctx.api == APIType::D3D12 && HookHasSafePostFSRBootstrapPath();
    ctx.runtimeOwnedSwapchainActive = ctx.api == APIType::D3D12 && DoesFGRuntimeOwnSwapchain();
    ctx.staleThirdPartyPresentHookRisk = ctx.api == APIType::D3D12 && ShouldForceSteamDX12Bypass(pSwapChain, ctx.presentBypassAvailable, IsSLInterposerLoaded());
    ctx.observerOnlyMode = HookOverlayObserverOnlyEnabled();
    ctx.observerStartupPresentOnlyMode = HookOverlayObserverStartupPresentOnlyEnabled();
    ctx.ffxStartupBypass = ShouldBypassFFXPresentDuringStreamlineStartup(
        ctx.api == APIType::D3D12, ctx.callerFromFFXFrameGenerationModule,
        ctx.streamlineStartupHandoffPending, ctx.streamlineStartupTransitionWindowActive, ctx.observerOnlyMode,
        ctx.observerStartupPresentOnlyMode);
    ctx.streamlineSyntheticReentrant = ShouldAllowSpecialStreamlinePresentRouting(ctx.observerOnlyMode) &&
        ShouldTreatStreamlinePresentAsSyntheticReentrant(
            ctx.api == APIType::D3D12, ctx.streamlineFGRunning, ctx.callerFromStreamlineModule, ctx.postSLConfirmedRendering,
            ctx.postSLConfirmedButStartupSettling, ctx.streamlineStartupHandoffInProgress, ctx.presentOwnershipActive,
            ctx.recentLargePresentGap, ctx.matchesExpectedPresentThread, ctx.startupTopLevelPresentAlreadyConsumed);
    ctx.startupTopLevelCandidate = DXGIShared::ShouldUseStreamlineStartupTopLevelCandidate(
        ctx.observerOnlyMode, ctx.streamlineSyntheticReentrant, ctx.callerFromStreamlineModule, ctx.api == APIType::D3D12,
        ctx.streamlineFGRunning, ctx.streamlineStartupHandoffInProgress, ctx.recentLargePresentGap, ctx.matchesExpectedPresentThread,
        ctx.postSLConfirmedRendering);
    ctx.stalePostFSRStartupHandoffPresentHookRisk = ctx.api == APIType::D3D12 && ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupHandoff(
                                     ctx.presentBypassAvailable, ctx.steamOverlayLoaded, ctx.api == APIType::D3D12,
                                     ctx.inWrapperPresent, ctx.wrappedSwapchain, ctx.hadFSRFGPhase, ctx.startupTopLevelCandidate);
    ctx.startupHandoffSteamRisk = ctx.staleThirdPartyPresentHookRisk || ctx.stalePostFSRStartupHandoffPresentHookRisk;
    ctx.postFSRRuntimeStartupHandoffRisk = ShouldBypassPresentForPostFSRStartupHandoffPresentOnNormalRoute(
        ctx.api == APIType::D3D12, ctx.hadFSRFGPhase, ctx.startupTopLevelCandidate, ctx.safePostFSRBootstrapPath,
        ctx.startupHandoffSteamRisk);
    ctx.streamlineStartupHandoffTransportRisk = ShouldTreatStreamlineStartupNormalRouteTransportAsUnsafe(
        ctx.api == APIType::D3D12, ctx.inWrapperPresent, ctx.wrappedSwapchain, ctx.presentBypassAvailable, ctx.callerFromStreamlineModule,
        ctx.streamlineStartupHandoffInProgress, ctx.runtimeOwnedSwapchainActive, ctx.startupTopLevelCandidate,
        ctx.postFSRRuntimeStartupHandoffRisk, ctx.startupHandoffSteamRisk);
    return ctx;
}
}

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (!pSwapChain)
        return DXGI_ERROR_INVALID_CALL;
    if (HookIsShuttingDown())
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    if (ce::fg_cost_probe::Active(ce::fg_cost_probe::kPresentPassthrough))
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    // Single parameter decision for the scoped Vulkan FIFO backstop: exactly one
    // physical Present hook (this one) consults the armed/lifecycle policy and
    // the registered live-WSI swapchain set, before any reentrancy or forwarding
    // choice below can act on the arguments. Disarmed calls and swapchains that
    // were never authorized pass through byte-identical.
    ce::vulkan_dxgi_fifo::ApplyFinalPresentPolicy(pSwapChain, SyncInterval, Flags,
                                                  ce::vulkan_dxgi_fifo::FinalPresentVariant::kPresent);
    g_PresentCallCounter.fetch_add(1, std::memory_order_relaxed);

    // DIAGNOSTIC: time the WHOLE DetourPresent call. The ECL diagnostic proved the Alt+Tab
    // freeze stall is NOT in ExecuteCommandLists, so it is in the present path. With the
    // ProcessFrame (overlay) and overlay-completion-wait phase timers, a slow total here with
    // NO matching slow ProcessFrame/wait log means the stall is the real Present call blocking
    // on the hung GPU (the iflip<->composited mode-switch GPU TDR). Compare 32-bit vs 64-bit.
    // What this present path takes from a CPU-bound game, which the wall-clock
    // timer below cannot separate from the runtime's own pacing block.
    ScopedHookCpuCost presentCpuCost(HookPresentCpuCost());
    ReportHookCpuCostIfDue();
    LARGE_INTEGER diagPresentT0;
    QueryPerformanceCounter(&diagPresentT0);
    auto diagPresentTimer = ce::make_scope_guard([&]() {
        LARGE_INTEGER diagPresentT1, diagPresentFreq;
        QueryPerformanceCounter(&diagPresentT1);
        QueryPerformanceFrequency(&diagPresentFreq);
        const double diagPresentMs =
            (double)(diagPresentT1.QuadPart - diagPresentT0.QuadPart) * 1000.0 / (double)diagPresentFreq.QuadPart;
        if (diagPresentMs >= 20.0) {
            static std::atomic<int> s_diagPresentLog{0};
            const int n = s_diagPresentLog.fetch_add(1, std::memory_order_relaxed);
            if (n < 200 || (n % 50) == 0) {
                HookLogImportant("DX12 DIAG: DetourPresent TOTAL SLOW %.1fms (tid=0x%04X)", diagPresentMs,
                                 GetCurrentThreadId());
            }
        }
    });

    // CRITICAL: Recursion guard using thread-local depth counter.
    // When using vtable hooking with Steam overlay, Steam's trampoline can call
    // back into DetourPresent via early-return paths (wrapped swapchain, shutdown, etc.)
    // before reaching the normal IsRecursivePresent() check at line ~530.
    // This caused stack overflow crashes (0xC00000FD) with ~500 recursive frames.
    static thread_local int s_presentRecurseDepth = 0;
    const bool wrappedSwapchain = IsWrappedSwapChainObject(pSwapChain);
    const bool inWrapperPresent = IsInWrapperPresent();
    const bool streamlineFGRunning = g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool isReentrant =
        (s_presentRecurseDepth > 0) && DXGIShared::ShouldTreatEarlyPresentRecursionAsForwardable(
                                           dxgi_shared_oPresentTrampoline != nullptr, dxgi_shared_oPresentBypass != nullptr, inWrapperPresent,
                                           wrappedSwapchain, streamlineFGRunning);
    s_presentRecurseDepth++;
    auto depthGuard = ce::make_scope_guard([]() { s_presentRecurseDepth--; });

    if (isReentrant) {
        // Re-entrant call - forward directly to bypass or return S_OK
        if (dxgi_shared_oPresentTrampoline) {
            return dxgi_shared_oPresentTrampoline(pSwapChain, SyncInterval, Flags);

        }
        if (dxgi_shared_oPresentBypass) {
            return dxgi_shared_oPresentBypass(pSwapChain, SyncInterval, Flags);
        }
        // No bypass available - return S_OK to break recursion loop
        return S_OK;
    }

    static int s_entryCount = 0;
    int entryNum = ++s_entryCount;

    // Present-call heartbeat diagnostic:
    // Logs periodically (every 1000th call) and whenever there's a gap >250ms.
    // Purpose: Detect whether the game stops calling Present during menus/pauses.
    //
    // GTA V Enhanced: During pause menu, Present calls stop entirely (10+ second
    // gaps observed).  This means our overlay can't render unless we detect the
    // gap and use an alternative rendering mechanism (like the pre-SL stall
    // fallback in ProcessFrame).
    //
    // Also logs: IsRecursivePresent (SL FG re-entrant calls), g_StreamlineFGRunning
    // (whether SL thinks FG is active), and thread ID (SL uses worker threads).
    {
        static LARGE_INTEGER s_lastPresentTime = {};
        static int s_heartbeatCount = 0;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (s_lastPresentTime.QuadPart != 0) {
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            double gapMs = (double)(now.QuadPart - s_lastPresentTime.QuadPart) * 1000.0 / freq.QuadPart;
            static constexpr double kLargePresentGapMs = 250.0;

            // Treat quarter-second Present gaps as scene/load transitions. This
            // is conservative enough to ignore ordinary jitter while still
            // catching save-load handoff disruptions.
            if (gapMs > kLargePresentGapMs || (s_heartbeatCount % 1000 == 0)) {
                if (gapMs > kLargePresentGapMs) {
                    MarkLargePresentGap();
                }
                // READ-ONLY state peek: DO NOT call IsRecursivePresent() here!
                // IsRecursivePresent() has side effects (CAS on g_presentThreadId)
                // and would permanently corrupt the present ownership tracking,
                // making ALL subsequent calls appear recursive and blocking
                // ProcessFrame from ever running again.
                DWORD presentOwner = dxgi_shared_g_presentThreadId.load(std::memory_order_relaxed);
                int presentDepthVal = dxgi_shared_g_presentDepth.load(std::memory_order_relaxed);
                HookLogImportant(
                    "DetourPresent: heartbeat #%d gap=%.0fms presentOwner=0x%04X depth=%d slFG=%d tid=0x%04X",
                    s_heartbeatCount, gapMs, presentOwner, presentDepthVal,
                    g_StreamlineFGRunning.load(std::memory_order_acquire) ? 1 : 0, GetCurrentThreadId());
            }
        }
        s_lastPresentTime = now;
        s_heartbeatCount++;
    }

    if (entryNum <= 10) {
        HookLog(
            "DetourPresent: ENTRY #%d (pSwapChain=%p, IsInWrapper=%d, "
            "trampoline=%p)",
            entryNum, pSwapChain, IsInWrapperPresent() ? 1 : 0, dxgi_shared_oPresentTrampoline);
    }

    // Foreign Present-entry ownership watch. Steam and RTSS both restore and
    // re-install the shared dxgi!Present entry bytes on their own schedule; a
    // re-hook that lands while CE's prepend is live records CE as that tool's
    // chain successor and silently drops whatever was below it. CE cannot undo
    // that from its side, so make the transition observable instead of leaving
    // an overlay disappearance to be re-diagnosed from scratch. Metered: a
    // short memcmp on the first presents and then rarely.
    if (dxgi_shared_oPresentTrampoline && dxgi_shared_s_originalVtable8Present &&
        (entryNum <= 3 || (entryNum % 512) == 0)) {
        void* currentEntryOwner = nullptr;
        if (!InlineHook::IsInstalledEntryPatchIntact(
                reinterpret_cast<void*>(dxgi_shared_s_originalVtable8Present), &currentEntryOwner)) {
            static std::atomic<int> s_entryOwnershipLostLogCount{0};
            const int n = s_entryOwnershipLostLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 5 || (n % 200) == 0) {
                char ownerPath[MAX_PATH] = {};
                const bool resolvedOwner =
                    currentEntryOwner && ResolveExternalPresentHookOwnerPath(currentEntryOwner, ownerPath,
                                                                             sizeof(ownerPath));
                HookLogImportant(
                    "DetourPresent: foreign re-hook took the Present entry from CE #%d (entry=%p newTarget=%p "
                    "owner=%s) - CE remains reachable through that tool's saved chain, but an overlay below it "
                    "may have been dropped",
                    n, (void*)dxgi_shared_s_originalVtable8Present, currentEntryOwner,
                    resolvedOwner ? ownerPath : "<unresolved thunk>");
            }
        }
    }

    const APIType api = DetectAPIType(pSwapChain);
    if (api == APIType::D3D12 && ShouldBypassDX12InvisibleWindowPresent(pSwapChain, "DetourPresent")) {
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    }
    BeginPostSLOffKeepAlivePresentScope();
    auto postSLOffKeepAlivePresentScopeGuard = ce::make_scope_guard([]() { EndPostSLOffKeepAlivePresentScope(); });
    if (api == APIType::D3D12) {
        DX12_TryRenderExactPostSLOffKeepAliveBeforePresent(pSwapChain,
                                                           "DXGIShared::DetourPresent pre-routing");
    }

    // Capture the caller here, not in a helper. We need the code that called
    // into DetourPresent, not the helper's own return address inside this DLL.
    const void* detourCallerAddress = CE_CAPTURE_RETURN_ADDRESS();
    const DWORD currentThreadId = GetCurrentThreadId();
    const bool steamOverlayLoaded = IsSteamOverlayModule(ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName());

    if (dxgi_shared_s_externalOverlayPresentInvokeDepth > 0) {
        PFN_Present recursiveBypass = EnsurePresentBypassTrampoline();
        if (DXGIShared::ShouldBypassRecursiveExternalOverlayPresent(true, recursiveBypass != nullptr)) {
            static std::atomic<int> s_recursiveExternalOverlayBypassLogCount{0};
            const int bypassNum = s_recursiveExternalOverlayBypassLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (bypassNum <= 10 || (bypassNum % 500) == 0) {
                HookLogImportant(
                    "DetourPresent: Bypassing recursive external-overlay Present #%d while guarded Steam hook is "
                    "active (depth=%d bypass=%p tid=0x%04X)",
                    bypassNum, dxgi_shared_s_externalOverlayPresentInvokeDepth, (void*)recursiveBypass, currentThreadId);
            }
            return recursiveBypass(pSwapChain, SyncInterval, Flags);
        }
    }
    // Log Steam overlay state once for diagnostics.
    static std::atomic<uint32_t> s_steamStateLogCount{0};
    if (s_steamStateLogCount.fetch_add(1, std::memory_order_relaxed) == 0) {
        const char* overlayModuleName = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
        HookLogImportant(
            "DetourPresent: Steam overlay state: steamLoaded=%d overlayModule=%s g_externalOverlayHook=%p "
            "oPresentTrampoline=%p oPresentBypass=%p slLoaded=%d streamlineFGRunning=%d",
            steamOverlayLoaded ? 1 : 0, overlayModuleName ? overlayModuleName : "none",
            (void*)dxgi_shared_g_externalOverlayPresentHook, (void*)dxgi_shared_oPresentTrampoline, (void*)dxgi_shared_oPresentBypass,
            IsSLInterposerLoaded() ? 1 : 0, g_StreamlineFGRunning.load(std::memory_order_acquire) ? 1 : 0);
    }
    const bool presentBypassAvailable = EnsurePresentBypassTrampoline() != nullptr;
    PresentCallContext ctx = CapturePresentCallContext(pSwapChain, detourCallerAddress, api,
                                                            presentBypassAvailable);
    if (ctx.ffxStartupBypass) {
        g_FGCompat.SetFSRFGSupportPresent(true);
        PFN_Present presentBypass = EnsurePresentBypassTrampoline();
        if (presentBypass) {
            static std::atomic<int> s_ffxStartupBypassLogCount{0};
            int bypassNum = s_ffxStartupBypassLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (bypassNum <= 10 || (bypassNum % 200) == 0) {
                HookLogImportant(
                    "DetourPresent: Treating FFX-originated Present as startup handoff bypass #%d (bypass=%p, "
                    "tid=0x%04X)",
                    bypassNum, (void*)presentBypass, GetCurrentThreadId());
            }
            return presentBypass(pSwapChain, SyncInterval, Flags);
        }
    }
    bool earlyReturn = false;
    const HRESULT routingResult = ExecuteStartupRouting(pSwapChain, SyncInterval, Flags, ctx, &earlyReturn);
    if (earlyReturn) {
        return routingResult;
    }
    return ExecutePresentCore(pSwapChain, SyncInterval, Flags, ctx);
}
}
