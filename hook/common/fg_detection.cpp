#include "fg_detection.h"
#include <cstring>
#include <cmath>

// Use HookLog from hook_common - forward declared in header
// If not available, define a stub
#ifndef HOOKLOG_DEFINED
extern void HookLog(const char* fmt, ...);
#endif

FGCompatibility g_FGCompat;

int64_t FGCompatibility::GetCurrentTimeUs() const {
    static int64_t qpcFreq = 0;
    if (qpcFreq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcFreq = f.QuadPart;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    return (qpc.QuadPart * 1000000) / qpcFreq;
}

const char* FGCompatibility::GetFGTypeName(FGType type) const {
    switch (type) {
        case FGType::None: return "None";
        case FGType::DLSS_FG: return "DLSS_FG";
        case FGType::FSR_FG: return "FSR_FG";
        case FGType::DLSS_MSFG: return "DLSS_MSFG";
        case FGType::Unknown: return "Unknown";
        default: return "Invalid";
    }
}

FGCompatibility::FGType FGCompatibility::DetectLoadedFGRuntime() {
    FGType result = FGType::None;
    
    // Check for DLSS FG / MSFG DLLs
    // nvngx_dlssg.dll - DLSS Frame Generation
    // sl.dlss_g.dll - Streamline DLSS-G plugin
    // nvngx.dll with specific features - general DLSS
    HMODULE dlssg = GetModuleHandleW(L"nvngx_dlssg.dll");
    HMODULE slDlssG = GetModuleHandleW(L"sl.dlss_g.dll");
    HMODULE streamline = GetModuleHandleW(L"sl.interposer.dll");
    
    if (dlssg || slDlssG) {
        result = FGType::DLSS_FG;
        HookLog("FG: Detected DLSS FG DLL (nvngx_dlssg=%p, sl.dlss_g=%p)", dlssg, slDlssG);
    }
    
    if (streamline) {
        HookLog("FG: Streamline interposer detected (%p) - DLSS FG likely", streamline);
        if (result == FGType::None) {
            result = FGType::DLSS_FG;  // Assume DLSS FG if streamline present
        }
    }
    
    // Check for FSR FG DLLs
    // amd_fidelityfx_fg.dll - FSR Frame Generation
    // ffx_fsr3upscaler_x64.dll - FSR 3 upscaler (often comes with FG)
    // ffx_frameinterpolation_x64.dll - Explicit FG DLL
    HMODULE fsrFg = GetModuleHandleW(L"amd_fidelityfx_fg.dll");
    HMODULE ffxFsr3 = GetModuleHandleW(L"ffx_fsr3upscaler_x64.dll");
    HMODULE ffxFrameInterp = GetModuleHandleW(L"ffx_frameinterpolation_x64.dll");
    
    if (fsrFg || ffxFrameInterp) {
        result = FGType::FSR_FG;
        HookLog("FG: Detected FSR FG DLL (amd_fidelityfx_fg=%p, ffx_frameinterpolation=%p)", fsrFg, ffxFrameInterp);
    } else if (ffxFsr3) {
        // FSR3 upscaler present - FG might be available
        HookLog("FG: FSR3 upscaler detected (%p) - FG may be available", ffxFsr3);
        if (result == FGType::None) {
            // Don't assume FG is active, just note it's possible
        }
    }
    
    detectedRuntime.store(result);
    
    if (result != FGType::None) {
        HookLog("FG: Runtime detection complete: %s", GetFGTypeName(result));
    }
    
    return result;
}

void FGCompatibility::RecordFrame(int commandListsExecuted) {
    int64_t now = GetCurrentTimeUs();
    bool isRealFrame = (commandListsExecuted > 0);
    
    // Store in circular buffer
    int idx = historyIndex.fetch_add(1) % WINDOW_SIZE;
    frameHistory[idx] = { now, commandListsExecuted };
    
    int total = totalFramesRecorded.fetch_add(1) + 1;
    
    // Track consecutive patterns for quick detection
    if (isRealFrame) {
        consecutiveRealFrames.fetch_add(1);
        consecutiveInterpolatedFrames.store(0);
    } else {
        consecutiveInterpolatedFrames.fetch_add(1);
        consecutiveRealFrames.store(0);
    }
    
    // Debug logging (rate limited)
    int logCount = debugLogCounter.fetch_add(1);
    if (logCount < 10 || (logCount % 120 == 0)) {
        HookLog("FG: Frame #%d: cmdLists=%d, isReal=%d, consReal=%d, consInterp=%d",
                total, commandListsExecuted, isRealFrame ? 1 : 0,
                consecutiveRealFrames.load(), consecutiveInterpolatedFrames.load());
    }
    
    // Update metrics every 30 frames
    if (total % 30 == 0) {
        UpdateMetrics();
        DetectPattern();
    }
    
    // Also update on potential FG toggle (sudden pattern change)
    if (consecutiveInterpolatedFrames.load() == 2 || consecutiveRealFrames.load() == 10) {
        UpdateMetrics();
        DetectPattern();
    }
}

void FGCompatibility::UpdateMetrics() {
    int64_t now = GetCurrentTimeUs();
    int64_t windowStartUs = now - 1000000;  // 1 second window
    
    int totalFrames = 0;
    int realFrames = 0;
    int64_t minTs = INT64_MAX;
    int64_t maxTs = 0;
    
    // Scan the window
    for (int i = 0; i < WINDOW_SIZE; i++) {
        const auto& f = frameHistory[i];
        if (f.timestampUs > windowStartUs && f.timestampUs <= now) {
            totalFrames++;
            if (f.commandLists > 0) {
                realFrames++;
            }
            if (f.timestampUs < minTs) minTs = f.timestampUs;
            if (f.timestampUs > maxTs) maxTs = f.timestampUs;
        }
    }
    
    if (totalFrames < 10) {
        // Not enough data yet
        return;
    }
    
    float durationS = (maxTs - minTs) / 1000000.0f;
    if (durationS < 0.1f) {
        return;  // Window too short
    }
    
    float outputFPS = totalFrames / durationS;
    float baseFPS = realFrames / durationS;
    
    cachedOutputFPS.store(outputFPS);
    cachedBaseFPS.store(baseFPS);
    
    // Calculate multiplier
    int mult = 1;
    if (realFrames > 0 && totalFrames > realFrames) {
        float ratio = (float)totalFrames / (float)realFrames;
        mult = (int)std::round(ratio);
        if (mult < 1) mult = 1;
        if (mult > 4) mult = 4;  // Cap at 4x (MSFG max)
    }
    
    int prevMult = cachedMultiplier.exchange(mult);
    
    // Log on multiplier change
    if (prevMult != mult) {
        HookLog("FG: Multiplier changed: %dx -> %dx (total=%d, real=%d, outputFPS=%.1f, baseFPS=%.1f)",
                prevMult, mult, totalFrames, realFrames, outputFPS, baseFPS);
    }
}

void FGCompatibility::DetectPattern() {
    int mult = cachedMultiplier.load();
    FGType runtime = detectedRuntime.load();
    FGType prevBehavior = activeBehavior.load();
    FGType newBehavior = FGType::None;
    
    if (mult >= 2) {
        // FG is active
        if (runtime == FGType::DLSS_FG) {
            if (mult >= 3) {
                newBehavior = FGType::DLSS_MSFG;  // 3x or 4x = MSFG
            } else {
                newBehavior = FGType::DLSS_FG;  // 2x = regular DLSS FG
            }
        } else if (runtime == FGType::FSR_FG) {
            newBehavior = FGType::FSR_FG;
        } else {
            // No runtime detected but behavioral pattern suggests FG
            newBehavior = FGType::Unknown;
            HookLog("FG: WARNING - FG pattern detected (mult=%dx) but no FG runtime DLL found!", mult);
        }
    } else {
        newBehavior = FGType::None;
    }
    
    if (prevBehavior != newBehavior) {
        activeBehavior.store(newBehavior);
        HookLog("FG: Behavior changed: %s -> %s (multiplier=%dx)",
                GetFGTypeName(prevBehavior), GetFGTypeName(newBehavior), mult);
    }
}

bool FGCompatibility::IsFGActive() const {
    return cachedMultiplier.load() >= 2;
}

void FGCompatibility::OnSwapchainRecreation() {
    int count = swapchainRecreationCount.fetch_add(1) + 1;
    int64_t now = GetCurrentTimeUs();
    int64_t lastTime = lastSwapchainRecreationTime.exchange(now);
    int64_t deltaMs = (now - lastTime) / 1000;
    
    HookLog("FG: Swapchain recreation #%d (delta=%lldms) - FG may have toggled", count, deltaMs);
    
    // CRITICAL: Suspend overlay during swapchain recreation to prevent stale resource access
    // This prevents E_ABORT crashes when Present is called with old resources
    SuspendFor(5000);  // 5 seconds to allow transition
    
    // Re-detect runtime DLLs (they might have loaded/unloaded)
    DetectLoadedFGRuntime();
    
    // Reset consecutive counters to allow re-detection
    consecutiveRealFrames.store(0);
    consecutiveInterpolatedFrames.store(0);
}

void FGCompatibility::OnDeviceChange() {
    HookLog("FG: Device change detected - re-detecting FG runtime");
    DetectLoadedFGRuntime();
}

void FGCompatibility::LogStatus() const {
    HookLog("FG: Status - runtime=%s, behavior=%s, mult=%dx, outputFPS=%.1f, baseFPS=%.1f, active=%d",
            GetFGTypeName(detectedRuntime.load()),
            GetFGTypeName(activeBehavior.load()),
            cachedMultiplier.load(),
            cachedOutputFPS.load(),
            cachedBaseFPS.load(),
            IsFGActive() ? 1 : 0);
}

void FGCompatibility::SuspendFor(int milliseconds) {
    int64_t now = GetCurrentTimeUs();
    int64_t suspendUntil = now + ((int64_t)milliseconds * 1000);
    suspendUntilUs.store(suspendUntil);
    HookLog("FG: Suspending overlay for %d ms (until %lld us)", milliseconds, (long long)suspendUntil);
}

bool FGCompatibility::IsSuspended() const {
    int64_t suspendUntil = suspendUntilUs.load();
    if (suspendUntil == 0) return false;
    
    // Inline QPC to avoid non-const issue
    static int64_t qpcFreq = 0;
    if (qpcFreq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcFreq = f.QuadPart;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    int64_t now = (qpc.QuadPart * 1000000) / qpcFreq;
    
    return now < suspendUntil;
}
