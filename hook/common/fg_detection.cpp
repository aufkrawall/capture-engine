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
    // FSR 3.0 DLLs
    HMODULE fsrFg = GetModuleHandleW(L"amd_fidelityfx_fg.dll");
    HMODULE ffxFrameInterp = GetModuleHandleW(L"ffx_frameinterpolation_x64.dll");
    HMODULE ffxFsr3 = GetModuleHandleW(L"ffx_fsr3upscaler_x64.dll");
    // FSR 3.1 / FSR 4 DLLs (UE5 native integration)
    HMODULE fsrFg4 = GetModuleHandleW(L"amd_fidelityfx_framegeneration_dx12.dll");
    HMODULE fsrFg4_vk = GetModuleHandleW(L"amd_fidelityfx_framegeneration_vk.dll");
    // Generic FSR FG DLLs
    HMODULE ffxFG = GetModuleHandleW(L"amd_fidelityfx_framegeneration.dll");
    // dlssg-to-fsr3 mod DLLs
    HMODULE dlssgToFsr3 = GetModuleHandleW(L"dlssg_to_fsr3_amd_is_better.dll");
    HMODULE dlssgToFsr3_2 = GetModuleHandleW(L"dlssg_to_fsr3.dll");

    if (fsrFg || ffxFrameInterp || fsrFg4 || fsrFg4_vk || ffxFG) {
        result = FGType::FSR_FG;
        static bool s_loggedFSR = false;
        if (!s_loggedFSR) {
            HookLog("FG: Detected FSR FG DLL (amd_fidelityfx_fg=%p, ffx_frameinterp=%p, fg_dx12=%p, fg_vk=%p, fg_generic=%p) - FSR CAPABLE", 
                    fsrFg, ffxFrameInterp, fsrFg4, fsrFg4_vk, ffxFG);
            s_loggedFSR = true;
        }
        // CRITICAL: FSR FG takes priority - don't check for DLSS to avoid conflicts
        // When both are loaded, trust FSR detection (game likely uses FSR FG)
    } else if (dlssgToFsr3 || dlssgToFsr3_2) {
        // dlssg-to-fsr3 mod detected - FSR FG is actually doing the work
        result = FGType::FSR_FG;
        static bool s_loggedDlssgFsr3 = false;
        if (!s_loggedDlssgFsr3) {
            HookLog("FG: Detected dlssg-to-fsr3 mod (%p, %p) - Treating as FSR FG", dlssgToFsr3, dlssgToFsr3_2);
            s_loggedDlssgFsr3 = true;
        }
    } else if (ffxFsr3) {
        // FSR3 upscaler often implies FG capability in FSR3 games
        result = FGType::FSR_FG;
        HookLog("FG: FSR3 upscaler detected (%p) - Treating as FSR FG for safety", ffxFsr3);
        // FSR takes priority - skip DLSS detection
    } else {
        // Only check for DLSS FG if FSR not detected
        HMODULE dlssg = GetModuleHandleW(L"nvngx_dlssg.dll");
        HMODULE slDlssG = GetModuleHandleW(L"sl.dlss_g.dll");
        HMODULE streamline = GetModuleHandleW(L"sl.interposer.dll");

        if (dlssg || slDlssG) {
            // CRITICAL: Only report DLSS FG if we actually see frame generation activity
            // Streamline/DLSS DLLs may be loaded for other features (upscaling, Reflex)
            // We'll do initial detection here but verify with behavioral check below
            result = FGType::DLSS_FG;
            static bool s_loggedDLSSG = false;
            if (!s_loggedDLSSG) {
                HookLog("FG: Detected DLSS FG DLL (nvngx_dlssg=%p, sl.dlss_g=%p) - DLSS FG CAPABLE", dlssg, slDlssG);
                HookLog("FG: Note: Streamline/DLSS loaded, but FG may not be active yet");
                s_loggedDLSSG = true;
            }
        }

        if (streamline) {
            // Streamline is a framework with many features (FG, upscaling, Reflex, etc.)
            // Don't assume FG is active just because Streamline is loaded
            static bool s_loggedStreamline = false;
            if (!s_loggedStreamline) {
                HookLog("FG: Streamline interposer detected (%p) - Framework loaded", streamline);
                s_loggedStreamline = true;
            }
            // Only set DLSS_FG if we haven't detected anything else AND we see actual FG activity
            if (result == FGType::None) {
                // Don't auto-set to DLSS_FG just because Streamline is present
                // Wait for behavioral detection to confirm FG is actually active
                HookLog("FG: Streamline present but no specific FG runtime detected yet");
            }
        }

        // BEHAVIORAL VERIFICATION: Don't trust DLL detection alone
        // Check if we actually see frame generation patterns (2x frame rate with interpolated frames)
        static int s_behavioralCheckCounter = 0;
        s_behavioralCheckCounter++;
        
        // Every 60 frames, verify if FG is actually active based on frame patterns
        if (s_behavioralCheckCounter >= 60) {
            s_behavioralCheckCounter = 0;
            
            int multiplier = cachedMultiplier.load();
            float outputFPS = cachedOutputFPS.load();
            float baseFPS = cachedBaseFPS.load();
            
            // FG is actually active if:
            // 1. Multiplier >= 2 (we see 2x or more frames)
            // 2. Output FPS is significantly higher than base FPS
            bool fgActuallyActive = (multiplier >= 2) && (outputFPS > baseFPS * 1.5f);
            
            if (result == FGType::DLSS_FG && !fgActuallyActive) {
                // DLSS FG DLLs loaded but no actual FG activity detected
                static bool s_loggedInactive = false;
                if (!s_loggedInactive) {
                    HookLog("FG: DLSS FG DLLs loaded but no FG activity detected (multiplier=%d, output=%.1f, base=%.1f)",
                            multiplier, outputFPS, baseFPS);
                    HookLog("FG: Treating as DLSS_FG INACTIVE - may be using upscaling only");
                    s_loggedInactive = true;
                }
                // Keep result as DLSS_FG but the overlay system should check behavioral metrics
            } else if (result == FGType::DLSS_FG && fgActuallyActive) {
                static bool s_loggedActive = false;
                if (!s_loggedActive) {
                    HookLog("FG: DLSS FG CONFIRMED ACTIVE (multiplier=%d, output=%.1f, base=%.1f)",
                            multiplier, outputFPS, baseFPS);
                    s_loggedActive = true;
                }
            }
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
        FGType dllType = detectedRuntime.load();
        FGType activeType = GetActiveFGType();
        // Log both: DLL-detected type and actual active type (API-confirmed)
        HookLog("Frame #%d: cmdLists=%d, isReal=%d, dllFG=%s, activeType=%s, apiDLSS=%d, apiFSR=%d, consReal=%d, consInterp=%d", 
                total, commandListsExecuted, isRealFrame ? 1 : 0, 
                GetFGTypeName(dllType), GetFGTypeName(activeType),
                dlssFGApiActive.load() ? 1 : 0, fsrFGApiActive.load() ? 1 : 0,
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
        // Fallback for low sample count (start up) - only for higher multipliers
        // CRITICAL: Don't use ratio-based detection for 2x to avoid false NVIDIA SM detection
        // A ratio of 1.5f can easily occur with normal frame timing variations
        // Only detect 3x and 4x through ratio (those are clearer indicators of actual FG)
        if (realFrames > 0) {
            float ratio = (float)totalFrames / (float)realFrames;
            if (ratio >= 3.5f)
                mult = 4;
            else if (ratio >= 2.5f)
                mult = 3;
            // Note: 2x multiplier requires proper histogram-based detection
            // This prevents false NVIDIA Smooth Motion detection in simple test apps
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
    float currentFPS = cachedOutputFPS.load();

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
        } else if (runtime == FGType::None && mult == 2 && currentFPS > 144.0f) {
            // No FG DLLs loaded but we see 2x multiplier AND high FPS - likely NVIDIA Smooth Motion
            // Smooth Motion is driver-level and hooks Present calls without loading game DLLs
            // CRITICAL: Require high FPS (>144) and multiple confirmations to avoid false detection
            int confirms = nvidiaSMConfirmCount.fetch_add(1) + 1;
            if (confirms >= NVIDIA_SM_CONFIRM_THRESHOLD) {
                newBehavior = FGType::NVIDIA_SM;
                static bool s_loggedSM = false;
                if (!s_loggedSM) {
                    HookLog("FG: Detected NVIDIA Smooth Motion (2x multiplier, outputFPS=%.1f, confirms=%d, no FG DLLs)", 
                            currentFPS, confirms);
                    s_loggedSM = true;
                }
            }
            // If not enough confirmations, keep previous behavior (don't set Unknown)
        } else if (runtime == FGType::None) {
            // No FG DLLs loaded and conditions not met for NVIDIA SM
            // This is likely a false positive from normal frame timing variations
            // CRITICAL FIX: Set to None, not Unknown, to prevent false FG display
            nvidiaSMConfirmCount.store(0);  // Reset SM confirmation counter
            newBehavior = FGType::None;
        } else {
            // Reset NVIDIA SM confirmation counter if conditions not met
            nvidiaSMConfirmCount.store(0);
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
    // 1. API hooks confirm FG is active (most reliable)
    // 2. Behavioral detection shows multiplier >= 2 (interpolated frames seen)
    // 3. NVIDIA Smooth Motion detected behaviorally
    //
    // IMPORTANT: DLL presence alone does NOT mean FG is active!
    // Games can load DLSS/FSR DLLs without enabling Frame Generation.
    // High FPS alone also doesn't mean FG is active (could just be fast GPU).

    // Check API-based detection first (most reliable)
    if (dlssFGApiActive.load(std::memory_order_acquire)) {
        return true;  // DLSS FG confirmed via NGX CreateFeature hook
    }
    if (fsrFGApiActive.load(std::memory_order_acquire)) {
        return true;  // FSR FG confirmed via FFX CreateContext hook
    }

    // Check behavioral detection (NVIDIA Smooth Motion)
    auto behavior = activeBehavior.load();
    if (behavior == FGType::NVIDIA_SM) {
        return cachedMultiplier.load() >= 2;
    }

    // Check if frame multiplier detected (interpolated frames seen)
    // This is the most reliable behavioral indicator
    if (cachedMultiplier.load() >= 2) {
        return true;
    }

    // REMOVED: FPS-based heuristic that assumed FG active if FPS > 100
    // This caused false positives when DLSS DLLs were loaded but FSR FG was active
    // High FPS alone doesn't mean FG is active!

    return false;
}

bool FGCompatibility::IsFSRActive() const
{
    // Prefer usage-based detection (API hooks) over DLL detection
    if (fsrFGApiActive.load(std::memory_order_acquire)) {
        return true;
    }
    // Fallback to DLL-based detection
    FGType detected = detectedRuntime.load(std::memory_order_acquire);
    return (detected == FGType::FSR_FG);
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

// =========================================================================
// Usage-based FG activation implementation
// These are called by API hooks (nvngx_hook, ffx_hook) when FG features
// are actually created/destroyed, providing accurate detection vs DLL presence
// =========================================================================

void FGCompatibility::SetDLSSFGActive(bool active)
{
    bool prev = dlssFGApiActive.exchange(active, std::memory_order_acq_rel);
    if (prev != active) {
        HookLog("FG: DLSS FG API activation changed: %s -> %s", 
                prev ? "ACTIVE" : "INACTIVE", 
                active ? "ACTIVE" : "INACTIVE");
        
        if (active) {
            // DLSS FG activated - update runtime type
            detectedRuntime.store(FGType::DLSS_FG, std::memory_order_release);
            HookLog("FG: DLSS Frame Generation ACTIVATED via NGX CreateFeature");
        } else {
            // DLSS FG deactivated - check if FSR FG is still active
            if (!fsrFGApiActive.load(std::memory_order_acquire)) {
                detectedRuntime.store(FGType::None, std::memory_order_release);
                HookLog("FG: DLSS Frame Generation DEACTIVATED via NGX ReleaseFeature");
            }
        }
    }
}

void FGCompatibility::SetFSRFGActive(bool active)
{
    bool prev = fsrFGApiActive.exchange(active, std::memory_order_acq_rel);
    if (prev != active) {
        HookLog("FG: FSR FG API activation changed: %s -> %s", 
                prev ? "ACTIVE" : "INACTIVE", 
                active ? "ACTIVE" : "INACTIVE");
        
        if (active) {
            // FSR FG activated - update runtime type
            // FSR takes priority over DLSS if both somehow active
            detectedRuntime.store(FGType::FSR_FG, std::memory_order_release);
            HookLog("FG: FSR Frame Generation ACTIVATED via FFX CreateContext");
        } else {
            // FSR FG deactivated - check if DLSS FG is still active
            if (!dlssFGApiActive.load(std::memory_order_acquire)) {
                detectedRuntime.store(FGType::None, std::memory_order_release);
                HookLog("FG: FSR Frame Generation DEACTIVATED via FFX DestroyContext");
            } else {
                // DLSS FG still active, switch back to it
                detectedRuntime.store(FGType::DLSS_FG, std::memory_order_release);
                HookLog("FG: FSR FG deactivated, but DLSS FG still active");
            }
        }
    }
}

FGCompatibility::FGType FGCompatibility::GetActiveFGType() const
{
    // Priority: Usage-based detection > DLL-based detection
    // This gives accurate results when DLLs are loaded but FG isn't active
    
    if (fsrFGApiActive.load(std::memory_order_acquire)) {
        return FGType::FSR_FG;
    }
    if (dlssFGApiActive.load(std::memory_order_acquire)) {
        return FGType::DLSS_FG;
    }
    
    // Fallback to behavioral/DLL-based detection
    return activeBehavior.load(std::memory_order_acquire);
}

// =========================================================================
// C-linkage exports for cross-module hooks
// These allow nvngx_hook and ffx_hook to call FG activation setters
// =========================================================================

extern "C" {

__declspec(dllexport) void FG_SetDLSSActive(bool active)
{
    g_FGCompat.SetDLSSFGActive(active);
}

__declspec(dllexport) void FG_SetFSRActive(bool active)
{
    g_FGCompat.SetFSRFGActive(active);
}

}
