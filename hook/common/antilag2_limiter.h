#pragma once
// AMD Anti-Lag 2 FPS Limiter
//
// Anti-Lag 2 is AMD's latency reduction technology (Reflex equivalent).
// Uses amdxc64.dll (AMD driver), which is loaded automatically when a DX12
// device is created on AMD GPU systems.
//
// Per-frame flow:
//   Init(device)         - one-time init (lazy, safe to call every frame)
//   SetTargetFps(fps)    - update FPS target
//   Update()             - blocks to enforce FPS cap + reduce latency
//
// Only works with DX12 on AMD GPU. Init fails gracefully on other systems.

// clang-format off
#include <windows.h>
// clang-format on
#include <atomic>
#include "antilag2_defs.h"
#include "hook_common.h"

class AntiLag2Limiter {
public:
    // Initialize with the given DX12 device.
    // Returns true if Anti-Lag 2 is available and initialized.
    bool Init(ID3D12Device* device) {
        if (inited_.load(std::memory_order_acquire))
            return available_.load(std::memory_order_acquire);

        inited_.store(true, std::memory_order_release);

        // Detect game activation: amd_ags_x64.dll loaded means game uses AGS
        if (GetModuleHandleA("amd_ags_x64.dll")) {
            gameActivated_.store(true, std::memory_order_release);
            HookLog("AntiLag2Limiter: Game detected using AMD AGS (amd_ags_x64.dll loaded)");
        }

        if (!device) {
            HookLog("AntiLag2Limiter: no DX12 device");
            return false;
        }

        HRESULT hr = AMD::AntiLag2DX12::Initialize(&context_, device);
        if (hr != S_OK) {
            HookLog("AntiLag2Limiter: Initialize failed hr=0x%08X (AMD GPU required)", (unsigned)hr);
            return false;
        }

        available_.store(true, std::memory_order_release);
        HookLog("AntiLag2Limiter: Initialized (AMD Anti-Lag 2 active)");
        return true;
    }

    // Set FPS cap. 0 = disable cap (latency reduction still active).
    void SetTargetFps(int fps) {
        targetFps_.store(fps > 0 ? static_cast<unsigned>(fps) : 0u, std::memory_order_release);
    }

    // True if successfully initialized on this AMD system.
    bool IsAvailable() const {
        return available_.load(std::memory_order_acquire);
    }

    // True after at least one successful per-frame Update() call.
    bool IsActive() const {
        return updateSucceeded_.load(std::memory_order_acquire);
    }

    // Returns true if the game is using AMD AGS (amd_ags_x64.dll loaded).
    // Used by auto mode to determine if Anti-Lag 2 should be active.
    bool IsGameActivated() const {
        return gameActivated_.load(std::memory_order_acquire);
    }

    // Call once per frame before Present.
    // Blocks to enforce the FPS cap via AMD driver latency pipeline.
    // Returns true on success.
    bool Update() {
        if (!available_.load(std::memory_order_acquire)) {
            updateSucceeded_.store(false, std::memory_order_release);
            return false;
        }

        unsigned fps = targetFps_.load(std::memory_order_acquire);
        HRESULT hr = AMD::AntiLag2DX12::Update(&context_, fps > 0, fps);
        bool ok = (hr == S_OK);
        if (!ok && updateSucceeded_.load(std::memory_order_acquire))
            HookLog("AntiLag2Limiter: Update failed hr=0x%08X", (unsigned)hr);
        updateSucceeded_.store(ok, std::memory_order_release);
        return ok;
    }

    void Shutdown() {
        if (available_.load(std::memory_order_acquire))
            AMD::AntiLag2DX12::DeInitialize(&context_);
        available_.store(false, std::memory_order_release);
        inited_.store(false, std::memory_order_release);
        updateSucceeded_.store(false, std::memory_order_release);
        gameActivated_.store(false, std::memory_order_release);
        targetFps_.store(0, std::memory_order_release);
    }

private:
    AMD::AntiLag2DX12::Context context_{};
    std::atomic<bool> inited_{false};
    std::atomic<bool> available_{false};
    std::atomic<bool> updateSucceeded_{false};
    std::atomic<bool> gameActivated_{false};  // True when amd_ags_x64.dll is loaded
    std::atomic<unsigned> targetFps_{0};
};

inline AntiLag2Limiter g_AntiLag2Limiter;
