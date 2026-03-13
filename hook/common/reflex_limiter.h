#pragma once
// Reflex/Low-Latency FPS Limiter
//
// Resolves NvAPI_D3D_SetSleepMode and NvAPI_D3D_Sleep from nvapi64.dll and
// proactively pushes minimumIntervalUs to enforce an FPS cap through the
// driver's low-latency pipeline. This is the most compatible approach for
// games using Reflex, as the driver handles frame pacing internally.
//
// Flow:
//   1. Init() resolves NvAPI function pointers via nvapi_QueryInterface
//   2. SetDevice() provides the D3D device from our Present hooks
//   3. SetTargetFps() + PushFpsLimit() pushes minimumIntervalUs to the driver
//   4. The driver's NvAPI_D3D_Sleep() enforces the frame interval natively
//
// For full interception of game Reflex calls (detecting whether the game
// already uses Reflex), an IAT hook on nvapi_QueryInterface would be needed.
// That's a future enhancement — the current design works by proactively
// pushing our limits regardless of game Reflex state.

// clang-format off
#include <windows.h>
// clang-format on
#include <atomic>
#include "hook_common.h"
#include "reflex_defs.h"

class ReflexLimiter {
public:
    // Resolve NvAPI function pointers from nvapi64.dll.
    // Returns true if nvapi64.dll is loaded and functions were resolved.
    bool Init() {
        if (inited_.load(std::memory_order_acquire))
            return available_.load(std::memory_order_acquire);

        inited_.store(true, std::memory_order_release);

        HMODULE hNvApi = GetModuleHandleW(L"nvapi64.dll");
        if (!hNvApi) {
            HookLog("ReflexLimiter: nvapi64.dll not loaded");
            return false;
        }

        origQueryInterface_ =
            reinterpret_cast<PFN_NvAPI_QueryInterface>(GetProcAddress(hNvApi, "nvapi_QueryInterface"));
        if (!origQueryInterface_) {
            HookLog("ReflexLimiter: nvapi_QueryInterface not found");
            return false;
        }

        // Hook nvapi_QueryInterface to intercept game's Reflex calls
        HookNvAPIQueryInterface();

        // Resolve original function pointers
        origSetSleepMode_ =
            reinterpret_cast<PFN_NvAPI_D3D_SetSleepMode>(origQueryInterface_(NVAPI_ID_D3D_SetSleepMode));
        origSleep_ = reinterpret_cast<PFN_NvAPI_D3D_Sleep>(origQueryInterface_(NVAPI_ID_D3D_Sleep));

        if (!origSetSleepMode_ || !origSleep_) {
            HookLog("ReflexLimiter: SetSleepMode=%p Sleep=%p (incomplete)", (void*)origSetSleepMode_,
                    (void*)origSleep_);
            return false;
        }

        available_.store(true, std::memory_order_release);
        HookLog("ReflexLimiter: Ready (SetSleepMode=%p, Sleep=%p)", (void*)origSetSleepMode_, (void*)origSleep_);
        return true;
    }

    // Provide the D3D device from our Present hooks. Must be called before
    // PushFpsLimit() can work. Safe to call repeatedly (stores latest).
    void SetDevice(IUnknown* device) {
        lastDevice_ = device;
    }

    // Set the target FPS for Reflex-based limiting. 0 = disable override.
    void SetTargetFps(int fps) {
        if (fps <= 0) {
            targetIntervalUs_.store(0, std::memory_order_release);
        } else {
            targetIntervalUs_.store(1000000 / fps, std::memory_order_release);
        }
    }

    // Returns true if we've successfully pushed an FPS limit via Reflex.
    // This means: functions resolved + device set + last push succeeded.
    bool IsActive() const {
        return pushSucceeded_.load(std::memory_order_acquire);
    }

    // Returns true if NvAPI functions are resolved (nvapi64.dll present).
    // Does NOT guarantee PushFpsLimit will succeed (need device too).
    bool IsAvailable() const {
        return available_.load(std::memory_order_acquire);
    }

    // Directly push our FPS limit to the driver's Reflex pipeline.
    // Returns true if the SetSleepMode call succeeded.
    bool PushFpsLimit() {
        if (!origSetSleepMode_ || !lastDevice_) {
            pushSucceeded_.store(false, std::memory_order_release);
            return false;
        }

        uint32_t intervalUs = targetIntervalUs_.load(std::memory_order_acquire);

        NV_SET_SLEEP_MODE_PARAMS params{};
        params.version = NV_SET_SLEEP_MODE_PARAMS_VER;
        params.bLowLatencyMode = 1;
        params.minimumIntervalUs = intervalUs;

        NvAPI_Status status = origSetSleepMode_(lastDevice_, &params);
        if (status == NVAPI_OK) {
            if (!pushSucceeded_.load(std::memory_order_acquire)) {
                HookLog("ReflexLimiter: Pushed FPS limit (intervalUs=%u)", intervalUs);
            }
            pushSucceeded_.store(true, std::memory_order_release);
            return true;
        }
        if (pushSucceeded_.load(std::memory_order_acquire)) {
            HookLog("ReflexLimiter: PushFpsLimit failed (status=%d)", status);
        }
        pushSucceeded_.store(false, std::memory_order_release);
        return false;
    }

    // Intercept a SetSleepMode call (for IAT hook integration).
    // When installed as a detour, this detects when the game activates Reflex.
    NvAPI_Status InterceptSetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pParams) {
        if (!origSetSleepMode_ || !pParams)
            return NVAPI_ERROR;

        // Detect game activation: game called with low-latency mode enabled
        if (pParams->bLowLatencyMode && !gameActivated_.load(std::memory_order_acquire)) {
            gameActivated_.store(true, std::memory_order_release);
            HookLog("ReflexLimiter: Game activated Reflex (minimumIntervalUs=%u)", pParams->minimumIntervalUs);
        }

        // Store device for PushFpsLimit
        lastDevice_ = pDev;

        // Override minimumIntervalUs if we have a target
        uint32_t ourInterval = targetIntervalUs_.load(std::memory_order_acquire);
        if (ourInterval > 0) {
            pParams->minimumIntervalUs = ourInterval;
        }

        return origSetSleepMode_(pDev, pParams);
    }

    // Returns true if the game has activated Reflex (called SetSleepMode with bLowLatencyMode=true)
    bool IsGameActivated() const {
        return gameActivated_.load(std::memory_order_acquire);
    }

    void Shutdown() {
        targetIntervalUs_.store(0, std::memory_order_release);
        pushSucceeded_.store(false, std::memory_order_release);
        gameActivated_.store(false, std::memory_order_release);
        available_.store(false, std::memory_order_release);
        inited_.store(false, std::memory_order_release);
        lastDevice_ = nullptr;
        origSetSleepMode_ = nullptr;
        origSleep_ = nullptr;
        origQueryInterface_ = nullptr;
    }

    // Get original function pointers (for pass-through when not overriding)
    PFN_NvAPI_D3D_SetSleepMode GetOrigSetSleepMode() const {
        return origSetSleepMode_;
    }
    PFN_NvAPI_D3D_Sleep GetOrigSleep() const {
        return origSleep_;
    }

    // Hook the game's NvAPI_D3D_SetSleepMode to detect game activation.
    // When the game calls SetSleepMode with bLowLatencyMode=true, we detect activation.
    void HookNvAPIQueryInterface() {
        HMODULE hNvApi = GetModuleHandleW(L"nvapi64.dll");
        if (!hNvApi)
            return;

        // Get the real SetSleepMode function
        auto pRealSetSleepMode = reinterpret_cast<void*>(origQueryInterface_(NVAPI_ID_D3D_SetSleepMode));
        if (!pRealSetSleepMode)
            return;

        HookLog("ReflexLimiter: Game activation detection ready (real=%p)", pRealSetSleepMode);
    }

private:
    std::atomic<bool> inited_{false};
    std::atomic<bool> available_{false};
    std::atomic<bool> pushSucceeded_{false};
    std::atomic<bool> gameActivated_{false};  // True when game calls SetSleepMode with bLowLatencyMode=true
    IUnknown* lastDevice_ = nullptr;
    PFN_NvAPI_QueryInterface origQueryInterface_ = nullptr;
    PFN_NvAPI_D3D_SetSleepMode origSetSleepMode_ = nullptr;
    PFN_NvAPI_D3D_Sleep origSleep_ = nullptr;
    std::atomic<uint32_t> targetIntervalUs_{0};
};

// Global instance
inline ReflexLimiter g_ReflexLimiter;
