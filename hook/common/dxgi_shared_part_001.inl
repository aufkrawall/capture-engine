#include "dxgi_shared.h"
#include "../../common/raii_helpers.h"
#include "../apis/dx11_hook.h"
#include "../apis/dx12_hook.h"
#include "../apis/streamline_hook.h"
#include "../wrappers/inline_hook.h"
#include "../wrappers/vtable_hook.h"
#include "../wrappers/wrapper_base.h"
#include "config.h"
#include "dx12_overlay_policy.h"
#include "fg_detection.h"
#include "fg_session_state.h"
#include "fps_limiter.h"
#include "freeze_watchdog.h"
#include "hook_common.h"
#include "logging.h"
#include "overlay_compat.h"
#include "overlay_metrics_publisher.h"
#include "perf_logger.h"
#include "performance_metrics.h"

#include <d3d10.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <intrin.h>
#include <psapi.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_set>

// Vulkan is handled by VK_LAYER_CE_overlay (ICD layer approach)
// No global hook pointer needed - extern void* g_VulkanHook;

// Put shutdown check outside the DXGIShared namespace
static bool IsShuttingDown() {
    return HookIsShuttingDown();
}

// Check if we're inside a CWrapDXGISwapChain Present call
extern bool IsInWrapperPresent();

#ifdef BUILDING_CAPTURE_HOOK
extern "C" void DX12_WaitForOverlayCompletion(ID3D12CommandQueue* pQueue);
extern "C" void DX12_FlushDeferredSignal();
extern "C" void DX12_SetDeferOverlaySubmitToSteamECL(bool defer);
extern "C" void DX12_SubmitSteamDeferredOverlay();
extern "C" bool DX12_IsDeferOverlaySubmitPending();
extern "C" void DX12_NoteWrappedD3D12PresentResult(const char* presentName, int callCount, UINT syncInterval,
                                                   UINT presentFlags, HRESULT presentHr, BOOL isFullscreen,
                                                   BOOL isIconic, BOOL hasZeroSize, HWND gameWindow);

static void InvokeDX12WaitForOverlayCompletion(ID3D12CommandQueue* pQueue) {
    DX12_WaitForOverlayCompletion(pQueue);
}
static void InvokeDX12FlushDeferredSignal() {
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
static void NoteDX12PresentResultForVtablePath(IDXGISwapChain* pSwapChain, const char* presentName, UINT SyncInterval,
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

static void InvokeDX12WaitForOverlayCompletion(ID3D12CommandQueue* pQueue) {
    PFN_DX12WaitForOverlayCompletion fn = ResolveDX12WaitForOverlayCompletion();
    if (fn) {
        fn(pQueue);
    }
}

static void InvokeDX12FlushDeferredSignal() {
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
static void NoteDX12PresentResultForVtablePath(IDXGISwapChain*, const char*, UINT, UINT, HRESULT) {}
#endif

namespace DXGIShared {

namespace {

// Private-data storage follows the swapchain COM identity without a global raw
// pointer map or a lifetime race. CWrapDXGISwapChain forwards private-data calls
// to the real object, so wrapped and unwrapped Present paths observe one value.
constexpr GUID kCESwapChainColorSpaceGuid = {
    0x72034fd8, 0xd63a, 0x4a6b, {0x98, 0x61, 0x4f, 0x6d, 0x99, 0xb7, 0x88, 0x21}};

}  // namespace

bool QuerySwapChainColorSpace(IDXGISwapChain* swapChain, DXGI_COLOR_SPACE_TYPE& colorSpace) {
    if (!swapChain)
        return false;
    int storedColorSpace = 0;
    UINT storedSize = sizeof(storedColorSpace);
    if (FAILED(swapChain->GetPrivateData(kCESwapChainColorSpaceGuid, &storedSize, &storedColorSpace)) ||
        storedSize != sizeof(storedColorSpace) || storedColorSpace < 0 ||
        storedColorSpace > static_cast<int>(DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020)) {
        return false;
    }
    colorSpace = static_cast<DXGI_COLOR_SPACE_TYPE>(storedColorSpace);
    return true;
}

bool RecordSwapChainColorSpace(IDXGISwapChain* swapChain, DXGI_COLOR_SPACE_TYPE colorSpace, bool* changed) {
    if (changed) {
        *changed = false;
    }
    if (!swapChain) {
        return false;
    }

    DXGI_COLOR_SPACE_TYPE previousColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    if (QuerySwapChainColorSpace(swapChain, previousColorSpace) && previousColorSpace == colorSpace) {
        return true;
    }

    const int storedColorSpace = static_cast<int>(colorSpace);
    const HRESULT result =
        swapChain->SetPrivateData(kCESwapChainColorSpaceGuid, sizeof(storedColorSpace), &storedColorSpace);
    if (FAILED(result)) {
        static std::atomic<int> s_failureLogCount{0};
        if (s_failureLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            HookLogImportant("DXGI: Failed to retain swapchain color-space contract sc=%p cs=%d hr=0x%08X",
                             swapChain, storedColorSpace, static_cast<unsigned>(result));
        }
        return false;
    }

    if (changed) {
        *changed = true;
    }
    return true;
}

ce::presentation_color::Encoding ResolveSwapChainPresentationEncoding(IDXGISwapChain* swapChain,
                                                                      DXGI_FORMAT format,
                                                                      DXGI_COLOR_SPACE_TYPE* trackedColorSpace,
                                                                      bool* hasTrackedColorSpace) {
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    const bool tracked = QuerySwapChainColorSpace(swapChain, colorSpace);
    if (trackedColorSpace)
        *trackedColorSpace = colorSpace;
    if (hasTrackedColorSpace)
        *hasTrackedColorSpace = tracked;
    return ce::presentation_color::ResolveDXGI(format, tracked, colorSpace);
}

SharedState g_SharedState;
std::mutex g_SharedMutex;
static std::mutex s_thirdPartyOverlaySwapchainMutex;
static std::unordered_set<IDXGISwapChain*> s_thirdPartyOverlaySwapchains;
static std::unordered_set<IDXGISwapChain*> s_startupBlockingOverlayTaggedSwapchains;

// Post-SL FG overlay callback (set by dx12_hook.cpp when SL FG is active).
std::atomic<PostSLOverlayRenderFn> g_PostSLOverlayRenderCallback{nullptr};

// Post-SL startup activation service (set by dx12_hook.cpp).
std::atomic<PostSLStartupActivationServiceFn> g_PostSLStartupActivationService{nullptr};

// Direct Streamline FG running signal (set by streamline_hook.cpp).
std::atomic<bool> g_StreamlineFGRunning{false};

// Present call counter — incremented by DetourPresent and DetourPresent1, read by
// SL hook to detect bypass.
std::atomic<uint64_t> g_PresentCallCounter{0};

void DX12_RegisterThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain, const char* creatorModulePath) {
    if (!pSwapChain) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_thirdPartyOverlaySwapchainMutex);
    s_thirdPartyOverlaySwapchains.insert(pSwapChain);
    if (ce::overlay_compat::IsStartupBlockingOverlayModulePath(creatorModulePath)) {
        s_startupBlockingOverlayTaggedSwapchains.insert(pSwapChain);
    } else {
        s_startupBlockingOverlayTaggedSwapchains.erase(pSwapChain);
    }
}

void DX12_UnregisterThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_thirdPartyOverlaySwapchainMutex);
    s_thirdPartyOverlaySwapchains.erase(pSwapChain);
    s_startupBlockingOverlayTaggedSwapchains.erase(pSwapChain);
}

bool DX12_IsThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) {
        return false;
    }

    std::lock_guard<std::mutex> lock(s_thirdPartyOverlaySwapchainMutex);
    return s_thirdPartyOverlaySwapchains.find(pSwapChain) != s_thirdPartyOverlaySwapchains.end();
}

bool DX12_IsStartupBlockingOverlayTaggedSwapchain(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) {
        return false;
    }

    std::lock_guard<std::mutex> lock(s_thirdPartyOverlaySwapchainMutex);
    return s_startupBlockingOverlayTaggedSwapchains.find(pSwapChain) != s_startupBlockingOverlayTaggedSwapchains.end();
}

// Global metrics for DXGI-based APIs
static PerformanceMetrics g_DXGIPerfMetrics;
static std::atomic<uint32_t> g_LatestSourceFrameIndex{0};

// Recursion detection globals (avoiding thread_local which requires runtime
// init)
static std::atomic<DWORD> g_presentThreadId{0};
static std::atomic<int> g_presentDepth{0};
static std::atomic<DWORD> g_resizeThreadId{0};
static std::atomic<int> g_resizeDepth{0};
// Present-scoped duplicate suppression for the explicit-OFF exact-proxy route.
// This is thread-local because a different Streamline worker Present represents
// a distinct output frame and must retain its own PostSL draw.
static thread_local uint32_t s_postSLOffKeepAlivePresentScopeDepth = 0;
static thread_local bool s_postSLOffKeepAlivePrePresentDrawn = false;

void BeginPostSLOffKeepAlivePresentScope() {
    if (s_postSLOffKeepAlivePresentScopeDepth++ == 0) {
        s_postSLOffKeepAlivePrePresentDrawn = false;
    }
}

void EndPostSLOffKeepAlivePresentScope() {
    if (s_postSLOffKeepAlivePresentScopeDepth != 0 && --s_postSLOffKeepAlivePresentScopeDepth == 0) {
        s_postSLOffKeepAlivePrePresentDrawn = false;
    }
}

void MarkPostSLOffKeepAlivePrePresentDrawn() {
    if (s_postSLOffKeepAlivePresentScopeDepth != 0) {
        s_postSLOffKeepAlivePrePresentDrawn = true;
    }
}

bool WasPostSLOffKeepAlivePrePresentDrawn() {
    return s_postSLOffKeepAlivePresentScopeDepth != 0 && s_postSLOffKeepAlivePrePresentDrawn;
}

// Helper to check if we're recursively entering from the same thread
static bool IsRecursivePresent() {
    DWORD currentId = GetCurrentThreadId();
    DWORD expected = g_presentThreadId.load(std::memory_order_acquire);

    // Fast path: same thread re-entering (recursion)
    if (expected == currentId && g_presentDepth.load(std::memory_order_acquire) > 0) {
        return true;
    }

    // Try to claim ownership atomically — only one thread can succeed
    DWORD zero = 0;
    if (g_presentThreadId.compare_exchange_strong(zero, currentId, std::memory_order_acq_rel)) {
        g_presentDepth.store(1, std::memory_order_release);
        return false;
    }

    // Another thread owns it — if it's us (race between check and CAS), re-check
    if (g_presentThreadId.load(std::memory_order_acquire) == currentId) {
        g_presentDepth.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }

    // Different thread owns it — during Streamline DLSS FG, SL calls Present
    // from worker threads for generated frames while the game thread is still
    // inside oPresent (SL's wrapper).  These cross-thread Present calls MUST
    // be treated as re-entrant so that:
    //   1. PostSL overlay callback fires (correct timing: after SL's GPU work)
    //   2. Pre-SL overlay rendering is skipped (would conflict with SL's GPU work)
    //   3. oPresentBypass is used (avoids re-entering SL's hook chain)
    // Without this, the cross-thread call runs the full non-re-entrant path
    // including DX12ProcessFrame, causing DEVICE_HUNG from concurrent backbuffer
    // access between our overlay and SL's FG pipeline.
    if (expected != 0 && g_StreamlineFGRunning.load(std::memory_order_acquire)) {
        return true;
    }

    // Different thread owns it — treat as non-recursive, let it proceed
    // (this matches original behavior: each thread processes Present
    // independently)
    return false;
}

static void ReleasePresent() {
    if (g_presentDepth.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        g_presentThreadId.store(0, std::memory_order_release);
    }
}

static bool IsRecursiveResize() {
    DWORD currentId = GetCurrentThreadId();
    DWORD expected = g_resizeThreadId.load(std::memory_order_acquire);

    if (expected == currentId && g_resizeDepth.load(std::memory_order_acquire) > 0) {
        return true;
    }

    DWORD zero = 0;
    if (g_resizeThreadId.compare_exchange_strong(zero, currentId, std::memory_order_acq_rel)) {
        g_resizeDepth.store(1, std::memory_order_release);
        return false;
    }

    if (g_resizeThreadId.load(std::memory_order_acquire) == currentId) {
        g_resizeDepth.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }

    return false;
}

static void ReleaseResize() {
    if (g_resizeDepth.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        g_resizeThreadId.store(0, std::memory_order_release);
    }
}

static bool ShouldBypassDX12InvisibleWindowPresent(IDXGISwapChain* pSwapChain, const char* presentName) {
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

// Original function pointers
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers1)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                                       const UINT*, IUnknown* const*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetColorSpace1)(IDXGISwapChain*, DXGI_COLOR_SPACE_TYPE);

static PFN_Present oPresent = nullptr;
static PFN_Present1 oPresent1 = nullptr;
static PFN_ResizeBuffers oResizeBuffers = nullptr;
static PFN_ResizeBuffers1 oResizeBuffers1 = nullptr;

// Inline hook trampolines - calling these bypasses the hook entirely
static PFN_Present oPresentTrampoline = nullptr;
static PFN_Present1 oPresent1Trampoline = nullptr;
static std::atomic<PFN_SetColorSpace1> oSetColorSpace1Trampoline{nullptr};
static std::mutex s_setColorSpace1HookMutex;
static thread_local unsigned s_wrapperSetColorSpaceForwardDepth = 0;

// Bypass trampolines — skip external E9/FF25 hooks (e.g. Streamline) at the
// function entry point by executing original prologue bytes read from disk.
// Used in re-entrant Present calls to actually present the frame without
// re-entering the external hook chain.
static PFN_Present oPresentBypass = nullptr;
static PFN_Present1 oPresent1Bypass = nullptr;

// Saved target of the external E9 JMP on dxgi!Present, installed by Steam overlay
// (gameoverlayrenderer64!OverlayHookD3D3).  Captured during InstallPresentInlineHooks
// BEFORE Streamline overwrites it with its own JMP.  CE may invoke this target
// from SL-originated Present stacks only while the Streamline plugin-lookup guard
// is active; otherwise those paths use the bypass trampoline.
static PFN_Present g_externalOverlayPresentHook = nullptr;
static thread_local int s_externalOverlayPresentInvokeDepth = 0;

// Stored vtable pointer for unhooking Present when COM wrapper takes over
static void** s_hookedVTable = nullptr;

// Saved original vtable[8] Present COM method captured from the temp swapchain
// at InstallPresentInlineHooks time, before any vtable modifications.  This is
// the real IDXGISwapChain::Present COM method (dxgi!CDXGISwapChain::Present or
// equivalent), not the inner dxgi!Present function that Steam hooks with an E9
// JMP.  Used in the E9 JMP path of CallOriginalPresent and
// AttemptSteamDX12OverlayInit to ensure DXGI COM method state management runs
// before dxgi!Present is called with Steam's E9 JMP.  Without this, calling
// dxgi!Present directly skips COM state management, which causes black screen
// on some DX12 games (e.g. Strange Brigade).
static PFN_Present s_originalVtable8Present = nullptr;

// State for one-time Steam DX12 overlay initialization.
// Steam's OverlayHookD3D3 lazily initializes its internal "next" Present handler
// on first E9 JMP entry by reading vtable[8].  When vtable[8] = DetourPresent
// (our vtable hook), Steam's init fails and sets "next" = NULL, causing RIP=0.
//
// Fix: temporarily restore vtable[8] to the original dxgi!Present on the very
// first non-SL Steam overlay Present call, allowing Steam's init to complete.
// Re-hook vtable[8] to DetourPresent after Steam returns.
static std::atomic<bool> s_steamDX12InitAttempted{false};
static bool s_steamInitCrashed = false;

// Fallback only: Steam's NULL Present-shaped callbacks should normally be
// patched to CE's DXGI bypass trampoline so Steam can keep chaining to a real
// Present.  This no-op is used only when a bypass trampoline is not available.
static HRESULT WINAPI SteamDummyRenderingCallback(IDXGISwapChain* /*pSwapChain*/, UINT /*SyncInterval*/,
                                                  UINT /*Flags*/) {
    return S_OK;
}

struct SteamNullCallbackRecoveryContext {
    const char* context = "unknown";
    const char* reason = nullptr;
    void* hook = nullptr;
    void* bypass = nullptr;
    bool streamlineStackActive = false;
    bool pluginLookupGuardReady = false;
};

static thread_local SteamNullCallbackRecoveryContext s_steamNullCallbackRecoveryContext;

// Forward declaration (defined at line ~817)
static bool IsReadableMemory(const void* ptr, size_t size);

static void* SelectSteamNullCallbackRecoveryTarget(const SteamNullCallbackRecoveryContext& recoveryContext) {
    const bool hasBypass = recoveryContext.bypass != nullptr;
    return DXGIShared::SelectSteamNullCallbackRecoveryPatchTarget(hasBypass) ==
                   DXGIShared::SteamNullCallbackRecoveryPatchTarget::DXGIBypassPresent
               ? recoveryContext.bypass
               : reinterpret_cast<void*>(SteamDummyRenderingCallback);
}

static void** ResolveSteamNullCallbackSlotFromFault(uintptr_t returnAddress, uintptr_t steamStart, uintptr_t steamEnd) {
