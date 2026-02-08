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
extern void SetSwapchainWrapperDisabled(bool disabled);
#define HAS_DX12_INVALIDATE 1
#else
#define HAS_DX12_INVALIDATE 0
static void DX12_InvalidateSwapchain() {}  // Stub for Vulkan layer
static void SetSwapchainWrapperDisabled(bool disabled) {}
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

FGCompatibility::FGType FGCompatibility::GetActiveFGType() const
{
    // Priority: FSR FG > DLSS FG > NVIDIA SM > None
    // FSR FG is detected via API hooks (ffx_frameinterpolation_createcontext)
    if (fsrFGApiActive.load(std::memory_order_acquire)) {
        return FGType::FSR_FG;
    }
    // DLSS FG is detected via API hooks (slCreateFeature with kFeatureDlssG)
    if (dlssFGApiActive.load(std::memory_order_acquire)) {
        return FGType::DLSS_FG;
    }
    // NVIDIA Smooth Motion is detected via behavioral analysis
    if (IsNvidiaSmoothMotionActive()) {
        return FGType::NVIDIA_SM;
    }
    return FGType::None;
}

bool FGCompatibility::IsFGActive() const { return GetActiveFGType() != FGType::None; }

void FGCompatibility::SetDLSSFGActive(bool active)
{
    bool wasActive = dlssFGApiActive.exchange(active, std::memory_order_acq_rel);
    if (wasActive != active) {
        HookLog("FG: DLSS FG API %s", active ? "ACTIVATED" : "DEACTIVATED");

        // Update the combined active behavior
        FGType newType = GetActiveFGType();
        FGType oldType = activeBehavior.exchange(newType);
        if (oldType != newType) {
            HookLog("FG: Active type changed: %s -> %s", GetFGTypeName(oldType), GetFGTypeName(newType));

            if (HAS_DX12_INVALIDATE && ((newType == FGType::DLSS_FG && oldType == FGType::FSR_FG) ||
                                        (newType == FGType::FSR_FG && oldType == FGType::DLSS_FG))) {
                DX12_InvalidateSwapchain();
                HookLog("FG: Invalidated swapchain for FG transition");
            }
        }
    }
}

void FGCompatibility::SetFSRFGActive(bool active)
{
    bool wasActive = fsrFGApiActive.exchange(active, std::memory_order_acq_rel);
    if (wasActive != active) {
        HookLog("FG: FSR FG API %s", active ? "ACTIVATED" : "DEACTIVATED");

        // Update the combined active behavior
        FGType newType = GetActiveFGType();
        FGType oldType = activeBehavior.exchange(newType);
        if (oldType != newType) {
            HookLog("FG: Active type changed: %s -> %s", GetFGTypeName(oldType), GetFGTypeName(newType));

            if (HAS_DX12_INVALIDATE && ((newType == FGType::DLSS_FG && oldType == FGType::FSR_FG) ||
                                        (newType == FGType::FSR_FG && oldType == FGType::DLSS_FG))) {
                DX12_InvalidateSwapchain();
                HookLog("FG: Invalidated swapchain for FG transition");
            }
        }
    }
}

void FGCompatibility::RecordFrame(int commandListsExecuted)
{
    int64_t now = GetCurrentTimeUs();
    bool isRealFrame = (commandListsExecuted > 0);

    // Store last command list count for frame classification
    lastCmdListCount.store(commandListsExecuted);

    // CRITICAL FIX: Use atomic index for thread-safe circular buffer
    int idx = historyIndex.fetch_add(1, std::memory_order_acquire) % WINDOW_SIZE;
    frameHistory[idx].timestampUs = now;
    frameHistory[idx].commandLists = commandListsExecuted;

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
    if (logCount < 20 || (logCount % 300 == 0)) {
        FGType activeType = GetActiveFGType();
        HookLog("Frame #%d: cmdLists=%d, isReal=%d, activeType=%s, apiDLSS=%d, apiFSR=%d, consReal=%d, consInterp=%d",
                total, commandListsExecuted, isRealFrame ? 1 : 0, GetFGTypeName(activeType),
                dlssFGApiActive.load() ? 1 : 0, fsrFGApiActive.load() ? 1 : 0, consecutiveRealFrames.load(),
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

    // CRITICAL FIX: Load historyIndex with acquire semantics
    int currentHead = historyIndex.load(std::memory_order_acquire);
    int startIdx = currentHead % WINDOW_SIZE;

    // Dynamic Thresholding: Find max work per frame to filter out "partial" presents
    int maxCmdLists = 0;
    for (int i = 0; i < WINDOW_SIZE; i++) {
        int64_t ts = frameHistory[i].timestampUs;
        int cmd = frameHistory[i].commandLists;
        if (ts > windowStartUs) {
            if (cmd > maxCmdLists) {
                maxCmdLists = cmd;
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

        int64_t ts = frameHistory[idx].timestampUs;
        int cmd = frameHistory[idx].commandLists;

        // Skip invalid or out-of-window frames
        if (ts <= windowStartUs || ts > now) continue;

        totalFrames++;
        if (ts < minTs) minTs = ts;
        if (ts > maxTs) maxTs = ts;

        // Use dynamic threshold instead of just > 0
        if (cmd > workThreshold) {
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
        float confidence = (float)maxCount / (float)totalIntervals;

        if (confidence > 0.4f) {  // Low threshold to allow switching, but prevents random noise
            mult = bestInterval;
        } else {
            mult = cachedMultiplier.load();
        }

        if (mult > 4) mult = 4;
    } else {
        // Fallback for low sample count (start up) - only for higher multipliers
        if (realFrames > 0) {
            float ratio = (float)totalFrames / (float)realFrames;
            if (ratio >= 3.5f)
                mult = 4;
            else if (ratio >= 2.5f)
                mult = 3;
            // Note: 2x multiplier requires proper histogram-based detection
        }
    }

    int prevMult = cachedMultiplier.exchange(mult);

    // Log on multiplier change
    if (prevMult != mult && (prevMult == 1 || mult == 1 || prevMult != mult)) {
        HookLog("FG: Multiplier changed %d -> %d (output=%.1f, base=%.1f, real=%d, total=%d)", prevMult, mult,
                outputFPS, baseFPS, realFrames, totalFrames);
    }
}

void FGCompatibility::DetectPattern()
{
    // Check for NVIDIA Smooth Motion if no API-based FG is active
    if (!IsFGActive()) {
        // NVIDIA Smooth Motion produces 2x frames with specific patterns
        // Detect it via consistent 2x multiplier without API hooks
        int mult = cachedMultiplier.load();

        // CRITICAL FIX: Use compare-and-swap to prevent race conditions
        // Only confirm SM when we consistently see 2x multiplier
        if (mult == 2) {
            // Atomically increment and check
            int expected = nvidiaSMConfirmCount.load(std::memory_order_acquire);
            while (expected < NVIDIA_SM_CONFIRM_THRESHOLD) {
                if (nvidiaSMConfirmCount.compare_exchange_weak(expected, expected + 1, std::memory_order_acq_rel,
                                                               std::memory_order_acquire)) {
                    expected++;
                } else {
                    // CAS failed, reload
                    expected = nvidiaSMConfirmCount.load(std::memory_order_acquire);
                }
            }

            // Now check if we reached threshold
            if (expected == NVIDIA_SM_CONFIRM_THRESHOLD) {
                FGType prev = activeBehavior.exchange(FGType::NVIDIA_SM);
                if (prev != FGType::NVIDIA_SM) {
                    HookLog("FG: NVIDIA Smooth Motion detected (2x multiplier confirmed)");
                }
            }
        } else {
            // Only reset if we're actually in SM mode and pattern broke
            FGType current = activeBehavior.load(std::memory_order_acquire);
            if (current == FGType::NVIDIA_SM) {
                // Atomically reset to prevent races
                nvidiaSMConfirmCount.store(0, std::memory_order_release);
                activeBehavior.store(FGType::None, std::memory_order_release);
                HookLog("FG: NVIDIA Smooth Motion pattern broken, resetting to None");
            }
        }
    }
}

void FGCompatibility::DetectNvidiaSmoothMotion()
{
    // Called externally to trigger SM detection
    // The actual detection happens in DetectPattern() based on frame analysis
}

void FGCompatibility::OnSwapchainRecreation()
{
    int count = swapchainRecreationCount.fetch_add(1) + 1;
    int64_t now = GetCurrentTimeUs();
    int64_t lastTime = lastSwapchainRecreationTime.exchange(now);
    int64_t deltaMs = (now - lastTime) / 1000;

    FGType runtime = GetActiveFGType();

    HookLog("FG: Swapchain recreation #%d (delta=%lldms, runtime=%s)", count, deltaMs, GetFGTypeName(runtime));

    // Reset consecutive counters to allow re-detection
    consecutiveRealFrames.store(0);
    consecutiveInterpolatedFrames.store(0);
}

void FGCompatibility::OnDeviceChange()
{
    HookLog("FG: Device change detected");
    // Reset counters and state
    consecutiveRealFrames.store(0);
    consecutiveInterpolatedFrames.store(0);
    nvidiaSMConfirmCount.store(0);
}

void FGCompatibility::LogStatus() const
{
    FGType active = GetActiveFGType();
    HookLog("FG Status: active=%s, apiDLSS=%d, apiFSR=%d, mult=%d, outputFPS=%.1f, baseFPS=%.1f", GetFGTypeName(active),
            dlssFGApiActive.load() ? 1 : 0, fsrFGApiActive.load() ? 1 : 0, cachedMultiplier.load(),
            cachedOutputFPS.load(), cachedBaseFPS.load());
}

// C-linkage exports
extern "C" {
void FG_SetDLSSActive(bool active) { g_FGCompat.SetDLSSFGActive(active); }

void FG_SetFSRActive(bool active) { g_FGCompat.SetFSRFGActive(active); }
}
