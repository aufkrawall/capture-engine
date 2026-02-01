#include "fg_detection.h"
#include <cmath>
#include <cstring>
#include "hook_common.h"

// Use HookLog from hook_common - forward declared in header
// If not available, define a stub
#ifndef HOOKLOG_DEFINED
extern void HookLog(const char* fmt, ...);
#endif

// Forward declaration for swapchain invalidation when FG type changes
// Only available in DX12 builds, not Vulkan layer
#ifndef VK_LAYER_CE_OVERLAY
extern void DX12_InvalidateSwapchain();
#define HAS_DX12_INVALIDATE 1
#else
#define HAS_DX12_INVALIDATE 0
static void DX12_InvalidateSwapchain() {}  // Stub for Vulkan layer
#endif

FGCompatibility g_FGCompat;

int64_t FGCompatibility::GetCurrentTimeUs() const
{
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

const char* FGCompatibility::GetFGTypeName(FGType type) const
{
    switch (type) {
        case FGType::None:
            return "None";
        case FGType::DLSS_FG:
            return "DLSS_FG";
        case FGType::FSR_FG:
            return "FSR_FG";
        case FGType::DLSS_MSFG:
            return "DLSS_MSFG";
        case FGType::NVIDIA_SM:
            return "NVIDIA_SM";
        case FGType::Unknown:
            return "Unknown";
        default:
            return "Invalid";
    }
}

void FGCompatibility::DetectNvidiaSmoothMotion()
{
    // NVIDIA Smooth Motion is a driver-level feature that hooks DXGI Present calls
    // It doesn't load special DLLs like DLSS FG, so we detect it via:
    // 1. Check for NVIDIA driver properties (NVAPI or registry)
    // 2. Behavioral detection (2x FPS pattern without command list increases)
    
    // Check if we can access NVIDIA driver settings to see if SM is enabled
    // This is a lightweight check that can be called periodically
    
    // For now, we rely on behavioral detection in UpdateMetrics()
    // If we see 2x frame rate but no DLLs loaded, we flag potential SM
}

FGCompatibility::FGType FGCompatibility::DetectLoadedFGRuntime()
{
    int64_t now = GetCurrentTimeUs();
    int64_t last = lastRuntimeDetectUs.load();
    if (last != 0 && (now - last) < 500000) {
        return detectedRuntime.load();
    }
    lastRuntimeDetectUs.store(now);

    FGType result = FGType::None;

    // Check for FSR FG DLLs FIRST (Prioritize detection to avoid wrapper usage if FSR is present)
    HMODULE fsrFg = GetModuleHandleW(L"amd_fidelityfx_fg.dll");
    HMODULE ffxFsr3 = GetModuleHandleW(L"ffx_fsr3upscaler_x64.dll");
    HMODULE ffxFrameInterp = GetModuleHandleW(L"ffx_frameinterpolation_x64.dll");

    if (fsrFg || ffxFrameInterp) {
        result = FGType::FSR_FG;
        HookLog("FG: Detected FSR FG DLL (amd_fidelityfx_fg=%p, ffx_frameinterpolation=%p)", fsrFg, ffxFrameInterp);
    } else if (ffxFsr3) {
        // FSR3 upscaler often implies FG capability in FSR3 games
        result = FGType::FSR_FG;
        HookLog("FG: FSR3 upscaler detected (%p) - Treating as FSR FG for safety", ffxFsr3);
    }

    // Check for DLSS FG / MSFG DLLs
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
            result = FGType::DLSS_FG;
        }
    }

    FGType prev = detectedRuntime.exchange(result);
    if (prev != result) {
        HookLog("FG: Runtime changed: %s -> %s", GetFGTypeName(prev), GetFGTypeName(result));
        // CRITICAL: Invalidate swapchain when switching between FG types
        // This ensures clean state transition between different FG implementations
        if ((result == FGType::DLSS_FG && prev == FGType::FSR_FG) ||
            (result == FGType::FSR_FG && prev == FGType::DLSS_FG)) {
            DX12_InvalidateSwapchain();
            HookLog("FG: Invalidated swapchain for FG transition (%s -> %s)", 
                    GetFGTypeName(prev), GetFGTypeName(result));
        }
    }

    return result;
}

void FGCompatibility::RecordFrame(int commandListsExecuted)
{
    int64_t now = GetCurrentTimeUs();
    bool isRealFrame = (commandListsExecuted > 0);

    // Store last command list count for frame classification
    lastCmdListCount.store(commandListsExecuted);

    // Store in circular buffer
    int idx = historyIndex.fetch_add(1) % WINDOW_SIZE;
    frameHistory[idx] = {now, commandListsExecuted};

    int total = totalFramesRecorded.fetch_add(1) + 1;

    // Track consecutive patterns for quick detection
    if (isRealFrame) {
        consecutiveRealFrames.fetch_add(1);
        consecutiveInterpolatedFrames.store(0);
    } else {
        consecutiveInterpolatedFrames.fetch_add(1);
        consecutiveRealFrames.store(0);
    }

    // Debug logging (periodic to avoid spam)
    int logCount = debugLogCounter.fetch_add(1);
    // Log every 300 frames to reduce spam but still provide diagnostic info
    if (logCount < 20 || (logCount % 300 == 0)) {
        FGType type = detectedRuntime.load();
        EarlyLog("Frame #%d: cmdLists=%d, isReal=%d, fgRuntime=%s, consReal=%d, consInterp=%d", total,
                 commandListsExecuted, isRealFrame ? 1 : 0, GetFGTypeName(type), consecutiveRealFrames.load(),
                 consecutiveInterpolatedFrames.load());
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

void FGCompatibility::UpdateMetrics()
{
    int64_t now = GetCurrentTimeUs();
    int64_t windowStartUs = now - 1000000;  // 1 second window

    int totalFrames = 0;
    int realFrames = 0;
    int64_t minTs = INT64_MAX;
    int64_t maxTs = 0;

    // Calculate metrics using Chronological Analysis (Histogram of Intervals)
    int currentHead = historyIndex.load();
    int startIdx = currentHead % WINDOW_SIZE;

    // Dynamic Thresholding: Find max work per frame to filter out "partial" presents
    int maxCmdLists = 0;
    for (int i = 0; i < WINDOW_SIZE; i++) {
        if (frameHistory[i].timestampUs > windowStartUs) {
            if (frameHistory[i].commandLists > maxCmdLists) {
                maxCmdLists = frameHistory[i].commandLists;
            }
        }
    }

    // Threshold: 50% of peak work, but at least 2
    int workThreshold = (maxCmdLists > 4) ? (maxCmdLists / 2) : 2;

    int lastRealLogicalIdx = -1;
    int intervalCounts[16] = {0};  // Track intervals 1..15
    int totalIntervals = 0;

    for (int i = 0; i < WINDOW_SIZE; i++) {
        int idx = (startIdx + i) % WINDOW_SIZE;
        const auto& f = frameHistory[idx];

        // Skip invalid or out-of-window frames
        if (f.timestampUs <= windowStartUs || f.timestampUs > now) continue;

        totalFrames++;
        if (f.timestampUs < minTs) minTs = f.timestampUs;
        if (f.timestampUs > maxTs) maxTs = f.timestampUs;

        // Use dynamic threshold instead of just > 0
        if (f.commandLists > workThreshold) {
            realFrames++;

            // Interval Analysis
            if (lastRealLogicalIdx != -1) {
                int interval = i - lastRealLogicalIdx;
                if (interval > 0 && interval < 16) {
                    intervalCounts[interval]++;
                    totalIntervals++;
                }
            }
            lastRealLogicalIdx = i;
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

    // Determine Multiplier from Histogram Mode
    int mult = 1;
    if (totalIntervals > 5) {  // Need meaningful sample size
        int bestInterval = 1;
        int maxCount = 0;

        // Find dominant interval
        for (int k = 1; k < 16; k++) {
            if (intervalCounts[k] > maxCount) {
                maxCount = intervalCounts[k];
                bestInterval = k;
            }
        }

        // Confidence check: Dominant interval must be significant (> 50% of intervals)
        // Or at least significantly better than others.
        // For simplicity, just pick the winner if it has regular support
        float confidence = (float)maxCount / (float)totalIntervals;

        if (confidence > 0.4f) {  // Low threshold to allow switching, but prevents random noise
            mult = bestInterval;
        } else {
            // Fallback to ratio if histogram is too noisy?
            // Or keep previous multiplier to avoid flicker?
            mult = cachedMultiplier.load();
        }

        if (mult > 4) mult = 4;
    } else {
        // Fallback for low sample count (start up)
        if (realFrames > 0) {
            float ratio = (float)totalFrames / (float)realFrames;
            if (ratio >= 3.5f)
                mult = 4;
            else if (ratio >= 2.5f)
                mult = 3;
            else if (ratio >= 1.5f)
                mult = 2;
        }
    }

    int prevMult = cachedMultiplier.exchange(mult);

    // Log on multiplier change
    if (prevMult != mult) {
        HookLog(
            "FG: Multiplier changed: %dx -> %dx (total=%d, real=%d, outputFPS=%.1f, baseFPS=%.1f, ModeConfidence=%.2f)",
            prevMult, mult, totalFrames, realFrames, outputFPS, baseFPS,
            (totalIntervals > 0) ? ((float)intervalCounts[mult] / totalIntervals) : 0.0f);
    }
}

void FGCompatibility::DetectPattern()
{
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
        } else if (runtime == FGType::None && mult == 2) {
            // No FG DLLs loaded but we see 2x multiplier - likely NVIDIA Smooth Motion
            // Smooth Motion is driver-level and hooks Present calls without loading game DLLs
            newBehavior = FGType::NVIDIA_SM;
            static bool s_loggedSM = false;
            if (!s_loggedSM) {
                HookLog("FG: Detected NVIDIA Smooth Motion (2x multiplier, no FG DLLs)");
                s_loggedSM = true;
            }
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
        HookLog("FG: Behavior changed: %s -> %s (multiplier=%dx)", GetFGTypeName(prevBehavior),
                GetFGTypeName(newBehavior), mult);
    }
}

bool FGCompatibility::IsFGActive() const
{
    // FG is active if:
    // 1. FG DLLs are loaded (runtime detected)
    // 2. Output FPS is high enough to suggest frame generation (> 100 FPS typically)
    // 3. Behavioral detection indicates NVIDIA Smooth Motion (driver-level FG)

    auto behavior = activeBehavior.load();
    
    // Check if NVIDIA Smooth Motion is active (detected behaviorally)
    if (behavior == FGType::NVIDIA_SM) {
        return cachedMultiplier.load() >= 2;
    }

    auto runtime = detectedRuntime.load();
    if (runtime == FGType::None) {
        return false;  // No FG DLLs loaded
    }

    // Check if multiplier detected (traditional detection)
    if (cachedMultiplier.load() >= 2) {
        return true;
    }

    // FPS-based heuristic: if output FPS > 100 with FG DLLs loaded, likely active
    float outputFPS = cachedOutputFPS.load();
    if (outputFPS > 100.0f) {
        return true;
    }

    return false;
}

void FGCompatibility::OnSwapchainRecreation()
{
    int count = swapchainRecreationCount.fetch_add(1) + 1;
    int64_t now = GetCurrentTimeUs();
    int64_t lastTime = lastSwapchainRecreationTime.exchange(now);
    int64_t deltaMs = (now - lastTime) / 1000;

    // Re-detect runtime DLLs first (they might have loaded/unloaded)
    lastRuntimeDetectUs.store(0);
    FGType runtime = DetectLoadedFGRuntime();

    HookLog("FG: Swapchain recreation #%d (delta=%lldms, runtime=%s)", count, deltaMs, GetFGTypeName(runtime));

    // CRITICAL FIX: Only suspend overlay if FG runtime is actually detected
    // For non-FG games (like Strange Brigade), suspension caused crashes when overlay resumed
    // because the graphics state was unstable during the suspension period
    if (runtime != FGType::None) {
        // FG games need suspension to allow buffers to stabilize during FG toggle
        SuspendFor(500);  // 500ms to allow transition
    } else {
        // Non-FG games: Do NOT suspend. The overlay will naturally skip frames
        // during swapchain invalidation via g_SwapchainInvalid flag instead.
        HookLog("FG: No FG runtime detected - skipping overlay suspension for swapchain recreation");
    }

    // Reset consecutive counters to allow re-detection
    consecutiveRealFrames.store(0);
    consecutiveInterpolatedFrames.store(0);
}

void FGCompatibility::OnDeviceChange()
{
    HookLog("FG: Device change detected - re-detecting FG runtime");
    lastRuntimeDetectUs.store(0);
    DetectLoadedFGRuntime();
}

void FGCompatibility::LogStatus() const
{
    HookLog("FG: Status - runtime=%s, behavior=%s, mult=%dx, outputFPS=%.1f, baseFPS=%.1f, active=%d",
            GetFGTypeName(detectedRuntime.load()), GetFGTypeName(activeBehavior.load()), cachedMultiplier.load(),
            cachedOutputFPS.load(), cachedBaseFPS.load(), IsFGActive() ? 1 : 0);
}

void FGCompatibility::SuspendFor(int milliseconds)
{
    int64_t now = GetCurrentTimeUs();
    int64_t suspendUntil = now + ((int64_t)milliseconds * 1000);
    suspendUntilUs.store(suspendUntil);
    HookLog("FG: Suspending overlay for %d ms (until %lld us)", milliseconds, (long long)suspendUntil);
}

bool FGCompatibility::IsSuspended() const
{
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
