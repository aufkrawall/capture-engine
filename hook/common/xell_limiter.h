#pragma once
// Intel XeLL (Xe Low Latency) FPS Limiter
//
// XeLL is Intel's latency reduction technology for Arc GPUs (Reflex equivalent).
// Uses libxell.dll (Intel Arc driver). All functions loaded at runtime.
//
// Per-frame flow:
//   Init(device)         - one-time init (safe to retry after first failure)
//   SetTargetFps(fps)    - update FPS target (-> minimumIntervalUs)
//   Sleep()              - blocks until next frame window (enforces FPS cap)
//
// Only works with DX12 on Intel Arc. Init fails gracefully on other systems.

// clang-format off
#include <windows.h>
// clang-format on
#include <atomic>
#include <d3d12.h>
#include "xell_defs.h"
#include "hook_common.h"

class XeLLLimiter {
public:
    // Initialize XeLL with the given DX12 device.
    // Returns true if XeLL is available and initialized.
    bool Init(ID3D12Device* device) {
        if (inited_.load(std::memory_order_acquire))
            return available_.load(std::memory_order_acquire);

        inited_.store(true, std::memory_order_release);

        if (!device) {
            HookLog("XeLLLimiter: no DX12 device");
            return false;
        }

        if (!LoadDLL()) {
            HookLog("XeLLLimiter: libxell.dll not available (Intel Arc required)");
            return false;
        }

        xell_result_t result = xellD3D12CreateContext_(device, &ctx_);
        if (result != XELL_RESULT_SUCCESS) {
            HookLog("XeLLLimiter: CreateContext failed (%d)", (int)result);
            ctx_ = nullptr;
            return false;
        }

        available_.store(true, std::memory_order_release);
        HookLog("XeLLLimiter: Initialized (Intel XeLL active)");
        return true;
    }

    // Set FPS cap. 0 = disable cap (latency reduction still active).
    void SetTargetFps(int fps) {
        uint32_t newInterval = (fps > 0) ? (1000000u / static_cast<uint32_t>(fps)) : 0u;
        targetIntervalUs_.store(newInterval, std::memory_order_release);
    }

    // True if successfully initialized on this Intel Arc system.
    bool IsAvailable() const {
        return available_.load(std::memory_order_acquire);
    }

    // True after at least one successful Sleep() call.
    bool IsActive() const {
        return sleepSucceeded_.load(std::memory_order_acquire);
    }

    // Call once per frame before Present.
    // Updates sleep mode params if changed, then blocks until next frame window.
    // Returns true on success.
    bool Sleep() {
        if (!available_.load(std::memory_order_acquire)) {
            sleepSucceeded_.store(false, std::memory_order_release);
            return false;
        }

        // Update driver params only when interval changes
        uint32_t intervalUs = targetIntervalUs_.load(std::memory_order_acquire);
        if (intervalUs != lastIntervalUs_) {
            lastIntervalUs_          = intervalUs;
            xell_sleep_params_t params = {};
            params.minimumIntervalUs = intervalUs;
            params.bLowLatencyMode   = 1;
            params.bLowLatencyBoost  = 0;
            xellSetSleepMode_(ctx_, &params);
        }

        uint32_t      frameId = frameCounter_.fetch_add(1, std::memory_order_relaxed);
        xell_result_t result  = xellSleep_(ctx_, frameId);
        bool          ok      = (result == XELL_RESULT_SUCCESS);
        if (!ok && sleepSucceeded_.load(std::memory_order_acquire))
            HookLog("XeLLLimiter: Sleep failed (%d)", (int)result);
        sleepSucceeded_.store(ok, std::memory_order_release);
        return ok;
    }

    void Shutdown() {
        if (ctx_ && xellDestroyContext_) {
            xellDestroyContext_(ctx_);
            ctx_ = nullptr;
        }
        available_.store(false, std::memory_order_release);
        inited_.store(false, std::memory_order_release);
        sleepSucceeded_.store(false, std::memory_order_release);
        targetIntervalUs_.store(0, std::memory_order_release);
        lastIntervalUs_ = UINT32_MAX;
        frameCounter_.store(0, std::memory_order_relaxed);
        // Don't FreeLibrary — the game may also use libxell.dll
    }

private:
    bool LoadDLL() {
        // Prefer already-loaded instance (e.g. loaded by game or driver)
        HMODULE hMod = GetModuleHandleW(L"libxell.dll");
        if (!hMod)
            hMod = LoadLibraryW(L"libxell.dll");
        if (!hMod)
            return false;

        xellD3D12CreateContext_ = reinterpret_cast<PFN_xellD3D12CreateContext>(
            GetProcAddress(hMod, "xellD3D12CreateContext"));
        xellDestroyContext_ = reinterpret_cast<PFN_xellDestroyContext>(
            GetProcAddress(hMod, "xellDestroyContext"));
        xellSetSleepMode_ = reinterpret_cast<PFN_xellSetSleepMode>(
            GetProcAddress(hMod, "xellSetSleepMode"));
        xellSleep_ = reinterpret_cast<PFN_xellSleep>(GetProcAddress(hMod, "xellSleep"));

        return xellD3D12CreateContext_ && xellDestroyContext_ && xellSetSleepMode_ && xellSleep_;
    }

    xell_context_handle_t      ctx_                    = nullptr;
    PFN_xellD3D12CreateContext xellD3D12CreateContext_  = nullptr;
    PFN_xellDestroyContext     xellDestroyContext_      = nullptr;
    PFN_xellSetSleepMode       xellSetSleepMode_        = nullptr;
    PFN_xellSleep              xellSleep_               = nullptr;

    std::atomic<bool>     inited_{false};
    std::atomic<bool>     available_{false};
    std::atomic<bool>     sleepSucceeded_{false};
    std::atomic<uint32_t> targetIntervalUs_{0};
    std::atomic<uint32_t> frameCounter_{0};
    uint32_t              lastIntervalUs_ = UINT32_MAX;
};

inline XeLLLimiter g_XeLLLimiter;
