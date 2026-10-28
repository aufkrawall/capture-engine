#pragma once

class ScopedAvGuard;

struct ScopedResizeGuard;

#include "dxgi_swapchain_wrap.h"
#include "hook/present/dxgi_shared_detail/types_and_state.h"
#include "hook/runtime/hook_clock.h"

#include <d3d10.h>

#include <d3d11.h>

#include <windows.h>

#include <atomic>

#include <cmath>

#include <cstring>

#include <mutex>

#include "common/logging/logging.h"

#include "common/platform/raii_helpers.h"

#include "hook/runtime/graphics_hook.h"

// ResolveCurrentProcessForeground is declared once, with default arguments, in
// common/hook_common.h (defined in hook/d3d12/dx12_hook_helpers.cpp) so a translation unit that
// includes both this wrapper-internal header and the dx12 hook internal header no longer trips
// over duplicate default arguments.



#include "hook/d3d12/dx12_overlay_policy.h"

#include "hook/present/dxgi_shared.h"
#include "hook/present/device_loss_latch.h"
#include "hook/present/dxgi_limiter_stage.h"

#include "hook/present/swapchain_liveness.h"

#include "hook/overlay/overlay_compat.h"

// Defined in dxgi_swapchain_wrap_diagnostics.cpp. Diagnostic-only and rate-limited. CALLER
// CONTRACT: the caller must hold a reference on realChain for the duration of the call. The
// refcount probe inside is net-zero on a LIVE object only; nothing can make it safe on a released
// one, because a freed COM object's heap block stays committed and keeps a plausible vtable (see
// hook/present/swapchain_liveness.h).
void LogSwapChainLifetimeDiagnostics(IDXGISwapChain* realChain, const char* stage);

// Defined in dxgi_swapchain_wrap_attribution.cpp. Diagnostic-only AddRef/Release vtable forwarding
// hooks that attribute every remaining reference on the real chain to the module that took it
// (installed only while no third-party overlay module is loaded, so foreign vtable hooks are never
// disturbed). The finish call logs the per-module nets; the hooks stay installed for the process.
void TryInstallSwapchainLifetimeAttribution(IDXGISwapChain* realChain);
void FinishSwapchainLifetimeAttribution(IDXGISwapChain* realChain);

#include "hook/metrics/perf_logger.h"

#include "hook/metrics/performance_metrics.h"

#include "hook/runtime/hook_common.h"

// External overlay functions (implemented in dx11_hook.cpp / dx12_hook.cpp)
extern void DrawDX11Overlay(IDXGISwapChain* pSwapChain);

extern void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain);

extern void DX11_ProcessFrameExternal(IDXGISwapChain* pSwapChain);

#include "hook/present/dx11_present_perf_row.h"

extern bool DX11Hook_ShouldPassThroughCurrentPresent();

extern void DX12_OnSwapchainResizeBegin();

extern void DX12_OnSwapchainResizeEnd();

extern bool DX12_ReleaseCaptureForSwapChainResize(IDXGISwapChain* swapChain, char* out, size_t outSize);

extern bool DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration();

#ifdef BUILDING_CAPTURE_HOOK
extern "C" void DX12_SetCommandQueue(ID3D12CommandQueue* pQueue);
#else
extern "C" __declspec(dllimport) void DX12_SetCommandQueue(ID3D12CommandQueue* pQueue);
#endif

extern "C" __declspec(dllimport) void DX12_WaitForOverlayCompletion(ID3D12CommandQueue* pQueue);

extern "C" __declspec(dllimport) bool DX12_FlushDeferredSignalWithInfo(
    ce::dx12_overlay_policy::D3D12DeferredOverlaySignalFlushInfo* outInfo);

extern "C" __declspec(dllimport) bool DX12_WaitForFocusLossOverlayFenceAfterPresent(
    const ce::dx12_overlay_policy::D3D12FocusLossOverlayFenceWaitContext* context,
    const ce::dx12_overlay_policy::D3D12DeferredOverlaySignalFlushInfo* flushInfo);

extern "C" __declspec(dllimport) void DX12_SetWrappedPresentFocusLossContext(const char* presentName, int callCount,
                                                                             UINT syncInterval, UINT presentFlags);

extern "C" __declspec(dllimport) void DX12_ClearWrappedPresentFocusLossContext();

extern "C" __declspec(dllimport) void DX12_NoteWrappedD3D12PresentResult(const char* presentName, int callCount,
                                                                         UINT syncInterval, UINT presentFlags,
                                                                         HRESULT presentHr, BOOL isFullscreen,
                                                                         BOOL isIconic, BOOL hasZeroSize,
                                                                         HWND gameWindow);

// CPU prerender limit for D3D10/11 (implemented in dx11_hook_prerender.cpp)
extern void ApplyPrerenderLimit(IDXGISwapChain* pSwapChain, float limit, bool d3d10Swapchain);

// FG detection for FSR FG/DLSS FG compatibility
#include "hook/fg/fg_detection.h"

#include "hook/pacing/fps_limiter.h"

#include "hook/runtime/freeze_watchdog.h"

#include <cstdint>

extern void DX11_UpdatePerformanceMetrics(int64_t qpcUs);

bool IsInWrapperPresent();

void SetSwapchainWrapperShutdown();

// Thread-local VEH guard for safely calling DXGI methods that might AV
// during shutdown (the swapchain's internal hash table may have been freed).
class ScopedAvGuard {
    PVOID handle_;
    static LONG CALLBACK Handler(PEXCEPTION_POINTERS ep) {
        if (ep->ExceptionRecord->ExceptionCode == STATUS_ACCESS_VIOLATION) {
            // Skip the crashing instruction (5 bytes for MOV with SIB+disp8)
            // and set rax/Rax=0 so the caller sees "entry not found" and handles
            // it gracefully (creates a new entry or returns error).
#ifdef _WIN64
            ep->ContextRecord->Rax = 0;
            ep->ContextRecord->Rip += 5;
#else
            ep->ContextRecord->Eax = DXGI_ERROR_DEVICE_REMOVED;
            ep->ContextRecord->Eip += 5;
#endif
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

public:
    ScopedAvGuard() {
        handle_ = AddVectoredExceptionHandler(1, Handler);
    }
    ~ScopedAvGuard() {
        RemoveVectoredExceptionHandler(handle_);
    }
};

// RAII Guard for Resize Scope
struct ScopedResizeGuard {
    ScopedResizeGuard() {
        DX12_AdjustWrapperResizeDepth(1);
    }
    ~ScopedResizeGuard() {
        DX12_AdjustWrapperResizeDepth(-1);
    }
};

// Shutdown safety flag to prevent accessing freed memory during process exit
inline std::atomic<bool> dxgi_swapchain_wrap_g_WrapperShutdown{false};

inline bool dxgi_swapchain_wrap_g_OverlayEnabled = true;

inline bool ShouldYieldToVulkanLayer() {
    SharedMemoryLayout* shm = GetHookSharedMemory();
    if (!shm) {
        return false;
    }

    return shm->runtimeState.IsVulkanPresentRecentForProcess(
        GetCurrentProcessId(), ce::hook_clock::TickCount64(), 200);
}

inline const char* DetectWrappedSwapchainApi(IUnknown* pDevice, bool isD3D12) {
    if (isD3D12)
        return "DX12";
    if (!pDevice)
        return "DXGI";

    // The same decision as the swapchain classifier (swapchain_api_classifier.h): a DXVK D3D11
    // device answers ID3D10Device too.
    ID3D11Device* device11 = nullptr;
    const bool has11 = SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D11Device), (void**)&device11)) && device11;
    ID3D10Device* device10 = nullptr;
    const bool has10 = SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D10Device), (void**)&device10)) && device10;
    if (has10) {
        device10->Release();
    }
    const unsigned featureLevel = has10 && has11 ? static_cast<unsigned>(device11->GetFeatureLevel()) : 0;
    if (has11) {
        device11->Release();
    }
    const bool d3d10Runtime = has10 && has11 && DXGIShared::IsD3D10RuntimeModuleLoaded();
    switch (DXGIShared::SelectPrimarySwapChainAPIType(false, has11, has10, d3d10Runtime, featureLevel)) {
        case DXGIShared::APIType::D3D10:
            return "DX10";
        case DXGIShared::APIType::D3D11:
            return "DX11";
        default:
            return "DXGI";
    }
}

inline const char* GetDX12PresentDelegationOverlayModuleName() {
    const char* overlayModule = ce::overlay_compat::GetStartupBlockingOverlayModuleName();
    if (overlayModule) {
        return overlayModule;
    }
    return ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
}

// When a third-party overlay already owns the DXGI Present chain, route the
// wrapper through our detour path instead of running a second wrapper-managed
// Present path. This avoids wrapper -> detour -> external overlay re-entry.
// The Streamline-runtime (non-retaining) wrapper is a pure passthrough while
// CE owns the Present entry: delegating reproduces the exact non-wrapped flow
// (sl.dlss_g -> entry hook -> DetourPresent) instead of double-processing each
// runtime present through both the wrapper and the detour.
inline bool ShouldDelegateDX12PresentToDetourHook(const char** overlayModuleOut = nullptr,
                                                  bool streamlineRuntimeNonRetainingWrapper = false,
                                                  bool presentInvisibleToDetourHook = false,
                                                  bool delegatedMethodIsPresent1 = false) {
    const char* overlayModule = GetDX12PresentDelegationOverlayModuleName();
    if (overlayModuleOut) {
        *overlayModuleOut = overlayModule;
    }
    // Delegating hands the call to the real method and relies on CE's detour catching it, so the
    // detour must exist for THIS method: a Present1-only view left delegated Presents unseen.
    if (!DXGIShared::HasPresentMethodDetourHook(delegatedMethodIsPresent1)) {
        return false;
    }
    if (DXGIShared::HasCompositablePresentInterposerOutputChain()) {
        // A present interposer (NVIDIA Smooth Motion) owns this object's Present and composites its
        // own output chain, which CE draws on through the detour: below every overlay that patched
        // the dxgi entry, and after the frame was generated. This wrapper must stay a pure
        // pass-through so the frame is composited exactly once, there.
        return true;
    }
    if (presentInvisibleToDetourHook) {
        // A present interposer owns this object's Present, but CE never observed the queue of its
        // output chain, so the detour has nowhere safe to draw. Delegating would hand the frame to a
        // hook that will deliberately pass it through and the overlay would never be composited at
        // all (Strange Brigade DX12 + NVIDIA Smooth Motion, session 20260914_105242).
        return false;
    }
    if (streamlineRuntimeNonRetainingWrapper) {
        // The Streamline-runtime wrapper exists only as CE's non-entry view for a multi-overlay
        // foreign chain. While CE still owns the Present entry (transition aborted, or entry not
        // yet left), it must stay a pure passthrough so presents are processed exactly once via
        // the detour hook, never twice (wrapper + detour).
        return true;
    }
    if (!overlayModule) {
        return false;
    }
    return !g_FGCompat.IsDLSSFGApiActive() && !g_FGCompat.IsFSRFGApiActive();
}

inline bool IsD3D12PresentDeviceLostHRESULT(HRESULT hr) {
    return ce::device_loss::IsDeviceLostPresentResult(hr);
}


inline ce::dx12_overlay_policy::D3D12DeferredOverlaySignalFlushInfo FlushDeferredDX12OverlaySignalAfterWrappedPresent(
    bool isD3D12, const char* presentName, int callCount, bool focusLostForSwapchain) {
    ce::dx12_overlay_policy::D3D12DeferredOverlaySignalFlushInfo flushInfo = {};
    if (!ce::dx12_overlay_policy::ShouldFlushDeferredOverlaySignalAfterPresent(isD3D12)) {
        return flushInfo;
    }

    DX12_FlushDeferredSignalWithInfo(&flushInfo);

    if (flushInfo.hadDeferredSignal) {
        static std::atomic<int> s_flushLogCount{0};
        const int n = s_flushLogCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 24 || (n % 1000) == 0) {
            WrapperLog(
                "%s#%d: flushed deferred DX12 overlay fence signal after wrapped Present "
                "(signal=%d hr=0x%08X fence=%p event=%p value=%llu completed=%llu queue=%p)",
                presentName, callCount, flushInfo.signalSucceeded ? 1 : 0, (unsigned)flushInfo.signalHr,
                flushInfo.fence, flushInfo.fenceEvent, (unsigned long long)flushInfo.fenceValue,
                (unsigned long long)flushInfo.completedValue, flushInfo.queue);
        }
    } else if (focusLostForSwapchain) {
        static std::atomic<int> s_noDeferredFocusLossLogCount{0};
        const int n = s_noDeferredFocusLossLogCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 40 || (n % 300) == 0) {
            WrapperLog(
                "%s#%d: no deferred DX12 overlay fence signal after wrapped Present "
                "(focus-loss; expected when background backbuffer work was held or same-frame immediate-fence path "
                "already waited before Present, fence=%p event=%p completed=%llu)",
                presentName, callCount, flushInfo.fence, flushInfo.fenceEvent,
                (unsigned long long)flushInfo.completedValue);
        }
    }
    return flushInfo;
}

inline const char* WaitResultName(DWORD waitResult) {
    switch (waitResult) {
        case WAIT_OBJECT_0:
            return "signaled";
        case WAIT_TIMEOUT:
            return "timeout";
        case WAIT_ABANDONED:
            return "abandoned";
        case WAIT_FAILED:
            return "failed";
        default:
            return "unknown";
    }
}

extern // Thread-local flag to track when we're inside the wrapper's Present
// This prevents vtable hooks from also processing the frame
thread_local bool g_InWrapperPresent;
