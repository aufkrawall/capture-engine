#include "fg_detection.h"
#include "hook_common.h"
#include <vector>
#include <string>

FGCompatibility g_FGCompat;

FGCompatibility::FGType FGCompatibility::DetectLoadedFGRuntime() {
    // Check for NVIDIA DLSS Frame Generation
    if (GetModuleHandleA("sl.dlss_g.dll") || GetModuleHandleA("nvngx_dlssg.dll")) {
        HookLog("FG: Detected NVIDIA DLSS Frame Generation runtime");
        return FGType::DLSS_FG;
    }
    
    // Check for AMD FSR 3 Frame Generation
    if (GetModuleHandleA("amd_fidelityfx_fsr3.dll") || GetModuleHandleA("fsr3_upscaler.dll")) {
        HookLog("FG: Detected AMD FSR 3 Frame Generation runtime");
        return FGType::FSR_FG;
    }
    
    return FGType::None;
}

bool FGCompatibility::IsFGLikelyActive() const {
    // 0. Safety Suspend Check
    auto now = std::chrono::steady_clock::now();
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    if (nowMs < suspendUntil.load()) {
        return true; // Treat as "FG Active" (unsafe to render) during suspend period
    }

    // DLL presence alone is NOT enough - the DLL stays loaded even when FG is disabled.
    // We need behavioral detection: FG doubles (or more) the output FPS compared to base FPS.
    
    // First, check if we have enough data
    if (cachedBaseFPS < 10.0f || cachedOutputFPS < 10.0f) {
        // Not enough data yet - fall back to DLL check for initial safety
        return detectedType != FGType::None;
    }
    
    // FG is considered active if:
    // 1. An FG runtime is loaded, AND
    // 2. Output FPS is significantly higher than Base FPS (ratio > 1.4 to account for measurement jitter)
    if (detectedType != FGType::None) {
        float ratio = cachedOutputFPS / cachedBaseFPS;
        if (ratio > 1.4f) {
            return true;
        }
    }
    
    // If we get here with an FG DLL loaded but ratio is ~1.0, FG is probably disabled in-game
    // Also check behavioral heuristics as fallback
    if (deviceChangeCount > 2) return true;
    if (swapchainRecreationCount > 2) return true;
    
    return false;
}

void FGCompatibility::SuspendFor(int milliseconds) {
    auto now = std::chrono::steady_clock::now();
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    int64_t target = nowMs + milliseconds;
    
    // Only extend, never reduce wait time
    int64_t current = suspendUntil.load();
    while (target > current) {
        if (suspendUntil.compare_exchange_weak(current, target)) {
            break;
        }
    }
}

void FGCompatibility::OnDeviceChange() {
    deviceChangeCount++;
    HookLog("FG: Device change detected (Count: %d)", deviceChangeCount.load());
    CheckBehavioralPatterns();
    // Suspend for 3 seconds on device change - this handles the turbulent period when enabling FG
    SuspendFor(3000); 
}

void FGCompatibility::OnSwapchainRecreation() {
    swapchainRecreationCount++;
    HookLog("FG: Swapchain recreation detected (Count: %d)", swapchainRecreationCount.load());
    CheckBehavioralPatterns();
    // Suspend for 3 seconds on swapchain recreation - FG often recreates swapchains when enabled
    SuspendFor(3000);
}

void FGCompatibility::CheckBehavioralPatterns() {
    // Basic heuristic: if we see rapid changes, it might be FG toggling or active proxying
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastCheck).count();
    
    if (elapsed > 5) {
        // Reset counters periodically to avoid false positives from long sessions
        deviceChangeCount = 0;
        swapchainRecreationCount = 0;
        lastCheck = now;
    }
}

FGCompatibility::FGType FGCompatibility::GetDetectedType() const {
    return detectedType;
}

int FGCompatibility::GetRecommendedInitDelayFrames() const {
    return IsFGLikelyActive() ? 5 : 3;
}

void FGCompatibility::RecordPresentCall() {
    auto now = std::chrono::steady_clock::now();
    
    // First call initialization
    if (presentCount == 0 && realFrameCount == 0 && cachedOutputFPS == 0.0f) {
        fpsWindowStart = now;
    }
    
    presentCount++;
    
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - fpsWindowStart).count();
    if (elapsed >= 500) { // Update every 500ms
        float seconds = elapsed / 1000.0f;
        if (seconds > 0) {
            cachedOutputFPS = (float)presentCount / seconds;
            cachedBaseFPS = (float)realFrameCount / seconds;
            
            // Allow re-detection of DLLs periodically if not yet detected
            if (detectedType == FGType::None) {
                 FGType check = DetectLoadedFGRuntime();
                 if (check != FGType::None) {
                     detectedType = check;
                     // const_cast because this is technically a logical const operation (state update) 
                     // but in a const method context usually. Wait, detectedType is member. 
                     // DetectLoadedFGRuntime is static but logic is fine.
                     // Making detectedType mutable or accessible here is fine.
                 }
            }
        }
        
        presentCount = 0;
        realFrameCount = 0;
        fpsWindowStart = now;
    }
}

void FGCompatibility::RecordRealFrame() {
    realFrameCount++;
}

float FGCompatibility::GetOutputFPS() const {
    return cachedOutputFPS;
}

float FGCompatibility::GetBaseFPS() const {
    return cachedBaseFPS;
}
