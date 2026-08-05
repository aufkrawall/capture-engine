#pragma once

struct ScopedAvGuard;

struct ScopedResizeGuard;

#include "dxgi_swapchain_wrap.h"

#include <d3d10.h>

#include <d3d11.h>

#include <windows.h>

#include <atomic>

#include <cmath>

#include <cstring>

#include <mutex>

#include "../../common/logging.h"

#include "../../common/raii_helpers.h"

#include "../apis/graphics_hook.h"

// Declared here for the DXGI wrapper TU (defined in dx12_hook_internal_helpers10.cpp);
// the dx12 hook internal header carries the same declaration for the hook TUs.
bool ResolveCurrentProcessForeground(HWND* foregroundWindowOut = nullptr, DWORD* foregroundPidOut = nullptr);



#include "../common/dx12_overlay_policy.h"

#include "../common/dxgi_shared.h"

#include "../common/overlay_compat.h"

#include "../common/perf_logger.h"

#include "../common/performance_metrics.h"

#include "hook_common.h"

// External overlay functions (implemented in dx11_hook.cpp / dx12_hook.cpp)
extern void DrawDX11Overlay(IDXGISwapChain* pSwapChain);

extern void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain);

extern void DX11_ProcessFrameExternal(IDXGISwapChain* pSwapChain);

extern void DX12_OnSwapchainResizeBegin();

extern void DX12_OnSwapchainResizeEnd();

extern bool DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration();

extern "C" __declspec(dllimport) void DX12_SetCommandQueue(ID3D12CommandQueue* pQueue);

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

// Query-based CPU prerender limit for D3D11 (implemented in dx11_hook.cpp)
extern void ApplyPrerenderLimit(IDXGISwapChain* pSwapChain, float limit);

// FG detection for FSR FG/DLSS FG compatibility
#include "../common/fg_detection.h"

#include "../common/fps_limiter.h"

#include "../common/freeze_watchdog.h"

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
    SharedMemoryLayout* shm = nullptr;
    if (g_IPC && g_IPC->GetSharedMem()) {
        shm = g_IPC->GetSharedMem();
    } else {
        shm = g_pSharedMem;
    }
    if (!shm) {
        return false;
    }

    const uint64_t lastVulkan = shm->runtimeState.vulkanPresentTick.load(std::memory_order_acquire);
    if (lastVulkan == 0) {
        return false;
    }

    const uint64_t now = GetTickCount64();
    return shm->runtimeState.vulkanLayerActive.load(std::memory_order_acquire) && now >= lastVulkan &&
           (now - lastVulkan) < 200;
}

inline const char* DetectWrappedSwapchainApi(IUnknown* pDevice, bool isD3D12) {
    if (isD3D12)
        return "DX12";
    if (!pDevice)
        return "DXGI";

    ID3D10Device* device10 = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D10Device), (void**)&device10))) {
        device10->Release();
        return "DX10";
    }

    ID3D11Device* device11 = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D11Device), (void**)&device11))) {
        device11->Release();
        return "DX11";
    }

    return "DXGI";
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
inline bool ShouldDelegateDX12PresentToDetourHook(const char** overlayModuleOut = nullptr) {
    const char* overlayModule = GetDX12PresentDelegationOverlayModuleName();
    if (overlayModuleOut) {
        *overlayModuleOut = overlayModule;
    }
    if (!overlayModule || !DXGIShared::HasPresentDetourHooks()) {
        return false;
    }
    return !g_FGCompat.IsDLSSFGApiActive() && !g_FGCompat.IsFSRFGApiActive();
}

inline bool IsD3D12PresentDeviceLostHRESULT(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG;
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

inline void LogD3D12PresentDeviceLostHRESULT(bool isD3D12, const char* presentName, int callCount, HRESULT hr) {
    if (!isD3D12 || !IsD3D12PresentDeviceLostHRESULT(hr)) {
        return;
    }

    DXGIShared::g_SharedState.deviceRemovedFatal.store(true, std::memory_order_release);
    static std::atomic<int> s_deviceLostLogCount{0};
    const int n = s_deviceLostLogCount.fetch_add(1, std::memory_order_relaxed);
    if (n < 20) {
        WrapperLog("%s#%d: D3D12 Present returned device-lost hr=0x%08X", presentName, callCount, (unsigned)hr);
    }
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

