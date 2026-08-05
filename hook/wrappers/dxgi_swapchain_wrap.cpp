#include "dxgi_swapchain_wrap_internal.h"

#ifndef BUILDING_CAPTURE_HOOK
// Dynamically import from capture_hook to update shared state across DLL
// boundaries (for d3d12_wrappers.dll)
typedef void (*PFN_AdjustDepth)(int);
static PFN_AdjustDepth pAdjustDepth = nullptr;
static std::once_flag pAdjustDepthInitOnce;

void DX12_AdjustWrapperResizeDepth(int delta) {
    std::call_once(pAdjustDepthInitOnce, []() {
        HMODULE hHook = GetModuleHandleA("capture_hook_x64.dll");
        if (!hHook)
            hHook = GetModuleHandleA("capture_hook_x86.dll");
        if (hHook) {
            pAdjustDepth = (PFN_AdjustDepth)GetProcAddress(hHook, "DX12_AdjustWrapperResizeDepth_C");
        }
    });
    if (pAdjustDepth)
        pAdjustDepth(delta);
}
#else
// Internal build: Symbol provided by dx12_hook.cpp
extern void DX12_AdjustWrapperResizeDepth(int delta);
#endif

// Thread-local flag to track when we're inside the wrapper's Present
// This prevents vtable hooks from also processing the frame
thread_local bool g_InWrapperPresent = false;

// Function to check if we're in wrapper Present (same DLL, no export needed)
bool IsInWrapperPresent() {
    return g_InWrapperPresent;
}

// FSR Frame Generation detection helpers
static bool IsFSRFrameGenerationActive() {
    static HMODULE fsrFgDll = nullptr;
    static std::once_flag fsrFgCheckOnce;
    std::call_once(fsrFgCheckOnce, []() {
        fsrFgDll = GetModuleHandleW(L"amd_fidelityfx_fg.dll");
        if (!fsrFgDll)
            fsrFgDll = GetModuleHandleW(L"ffx_fsr3upscaler_x64.dll");
        if (!fsrFgDll)
            fsrFgDll = GetModuleHandleW(L"ffx_frameinterpolation_x64.dll");
    });
    return fsrFgDll != nullptr;
}

// Check if this swapchain is likely an FSR internal swapchain
bool CWrapDXGISwapChain::IsFSRInternalSwapchain() {
    // If FSR FG is not active, this can't be an FSR internal swapchain
    if (!IsFSRFrameGenerationActive())
        return false;

    // CRITICAL FIX: Enhanced FSR internal swapchain detection
    // FSR creates internal swapchains for frame generation that we should not
    // intercept

    // 1. Check for null window handle (primary indicator)
    if (!m_hWnd || m_hWnd == nullptr) {
        WrapperLog("Swapchain %p has no window handle, possible FSR internal", this);
        return true;
    }

    // 2. Check for very small dimensions (FSR internal swapchains often have
    // small intermediate buffers) FSR 3 uses 1/3 resolution buffers for upscaling
    if (m_State.width > 0 && m_State.height > 0) {
        // Check if dimensions suggest an internal buffer (not a main display
        // resolution) Common FSR internal resolutions are typically not standard
        // display sizes
        bool isStandardResolution =
            (m_State.width == 1920 && m_State.height == 1080) || (m_State.width == 2560 && m_State.height == 1440) ||
            (m_State.width == 3840 && m_State.height == 2160) || (m_State.width == 2560 && m_State.height == 1080) ||
            (m_State.width == 3440 && m_State.height == 1440) || (m_State.width == 1280 && m_State.height == 720);

        // Also check for common upscaling ratios from common base resolutions
        // FSR typically scales from 360p, 540p, 720p to 1080p/4K
        bool isCommonBaseResolution = (m_State.width == 640 && m_State.height == 360) ||
                                      (m_State.width == 960 && m_State.height == 540) ||
                                      (m_State.width == 1280 && m_State.height == 720);

        // If it's not a standard display resolution and not a common base, it might
        // be internal
        if (!isStandardResolution && !isCommonBaseResolution && (m_State.width < 800 || m_State.height < 600)) {
            WrapperLog("Swapchain %p has unusual dimensions %ux%u, possible FSR internal", this, m_State.width,
                       m_State.height);
            return true;
        }
    }

    // 3. FSR internal swapchains often have flip model but no actual presentation
    // (they're used for intermediate buffering)
    if (m_FlipModel.active && m_State.width == 0 && m_State.height == 0) {
        WrapperLog("Swapchain %p has flip model with zero dimensions, possible FSR internal", this);
        return true;
    }

    return false;
}
