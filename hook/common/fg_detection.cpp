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

FGCompatibility::FGType FGCompatibility::GetActiveFGType() const {
    // DORMANT MODE: Only return API-detected types, skip pattern detection
    // This prevents false positives from pattern-based detection
    if (dormantMode.load()) {
        // Only check API-based detection
        if (fsrFGApiActive.load(std::memory_order_acquire)) {
            return FGType::FSR_FG;
        }
        if (dlssFGApiActive.load(std::memory_order_acquire)) {
            return FGType::DLSS_FG;
        }
        return FGType::None;
    }

    // Normal mode: Priority: FSR FG > DLSS FG > NVIDIA SM > None
    if (fsrFGApiActive.load(std::memory_order_acquire)) {
        return FGType::FSR_FG;
    }
    if (dlssFGApiActive.load(std::memory_order_acquire)) {
        return FGType::DLSS_FG;
    }
    if (IsNvidiaSmoothMotionActive()) {
        return FGType::NVIDIA_SM;
    }
    return FGType::None;
}

bool FGCompatibility::IsFGActive() const {
    return GetActiveFGType() != FGType::None;
}

void FGCompatibility::SetDLSSFGActive(bool active) {
    bool wasActive = dlssFGApiActive.exchange(active, std::memory_order_acq_rel);
    if (!active) {
        dlssFGMultiplier.store(0, std::memory_order_release);
    }
    if (wasActive != active) {
        HookLog("FG: DLSS FG API %s (dormant=%d)", active ? "ACTIVATED" : "DEACTIVATED", dormantMode.load() ? 1 : 0);

        // Update the combined active behavior
        FGType newType = GetActiveFGType();
        FGType oldType = activeBehavior.exchange(newType);
        if (oldType != newType) {
            HookLog("FG: Active type changed: %s -> %s", GetFGTypeName(oldType), GetFGTypeName(newType));

            // DORMANT MODE: Skip swapchain invalidation when dormant
            if (!dormantMode.load() && HAS_DX12_INVALIDATE &&
                ((newType == FGType::DLSS_FG && oldType == FGType::FSR_FG) ||
                 (newType == FGType::FSR_FG && oldType == FGType::DLSS_FG))) {
                DX12_InvalidateSwapchain();
                HookLog("FG: Invalidated swapchain for FG transition");
            }
        }
    }
}

void FGCompatibility::SetDLSSFGMultiplier(int multiplier) {
    const int normalizedMultiplier = NormalizeDLSSFGFactor(multiplier);
    const int previousMultiplier = dlssFGMultiplier.exchange(normalizedMultiplier, std::memory_order_acq_rel);
    if (previousMultiplier != normalizedMultiplier) {
        HookLog("FG: DLSS FG multiplier %d -> %d", previousMultiplier, normalizedMultiplier);
    }
}

void FGCompatibility::SetFSRFGActive(bool active) {
    bool wasActive = fsrFGApiActive.exchange(active, std::memory_order_acq_rel);
    if (wasActive != active) {
        HookLog("FG: FSR FG API %s (dormant=%d)", active ? "ACTIVATED" : "DEACTIVATED", dormantMode.load() ? 1 : 0);

        FGType newType = GetActiveFGType();
        FGType oldType = activeBehavior.exchange(newType);
        if (oldType != newType) {
            HookLog("FG: Active type changed: %s -> %s", GetFGTypeName(oldType), GetFGTypeName(newType));

            // DORMANT MODE: Skip swapchain invalidation when dormant
            if (!dormantMode.load() && HAS_DX12_INVALIDATE &&
                ((newType == FGType::DLSS_FG && oldType == FGType::FSR_FG) ||
                 (newType == FGType::FSR_FG && oldType == FGType::DLSS_FG))) {
                DX12_InvalidateSwapchain();
                HookLog("FG: Invalidated swapchain for FG transition");
            }
        }
    }
}

void FGCompatibility::RecordFrame(int commandListsExecuted) {
    // Always track basic stats
    int64_t now = GetCurrentTimeUs();
    lastCmdListCount.store(commandListsExecuted);

    int idx = historyIndex.fetch_add(1, std::memory_order_relaxed) % WINDOW_SIZE;
    frameHistory[idx].timestampUs.store(now, std::memory_order_release);
    frameHistory[idx].commandLists.store(commandListsExecuted, std::memory_order_release);

    int total = totalFramesRecorded.fetch_add(1) + 1;

    bool isRealFrame = (commandListsExecuted > 0);
    if (isRealFrame) {
        consecutiveRealFrames.fetch_add(1);
        consecutiveInterpolatedFrames.store(0);
    } else {
        consecutiveInterpolatedFrames.fetch_add(1);
        consecutiveRealFrames.store(0);
    }

    // DORMANT MODE: Skip pattern-based detection and metrics
    if (dormantMode.load()) {
        // Minimal logging in dormant mode
        int logCount = debugLogCounter.fetch_add(1);
        if (logCount == 0) {
            HookLog("FG: Running in DORMANT mode - pattern detection disabled");
        }
        return;
    }

    // Active mode: Full pattern detection
    int logCount = debugLogCounter.fetch_add(1);
    if (logCount < 20 || (logCount % 300 == 0)) {
        FGType activeType = GetActiveFGType();
        HookLog(
            "Frame #%d: cmdLists=%d, isReal=%d, activeType=%s, apiDLSS=%d, "
            "apiFSR=%d, consReal=%d, consInterp=%d",
            total, commandListsExecuted, isRealFrame ? 1 : 0, GetFGTypeName(activeType), dlssFGApiActive.load() ? 1 : 0,
            fsrFGApiActive.load() ? 1 : 0, consecutiveRealFrames.load(), consecutiveInterpolatedFrames.load());
    }

    // Update metrics every 30 frames
    if (total % 30 == 0) {
        UpdateMetrics();
        DetectPattern();
    }

    if (consecutiveInterpolatedFrames.load() == 2 || consecutiveRealFrames.load() == 10) {
        UpdateMetrics();
        DetectPattern();
    }
}

void FGCompatibility::UpdateMetrics() {
    // DORMANT MODE: Skip metrics calculation
    if (dormantMode.load()) {
        return;
    }

    int64_t now = GetCurrentTimeUs();
    int64_t windowStartUs = now - 1000000;  // 1 second window

    int totalFrames = 0;
    int realFrames = 0;
    int64_t minTs = INT64_MAX;
    int64_t maxTs = 0;

    int currentHead = historyIndex.load(std::memory_order_acquire);
    int startIdx = currentHead % WINDOW_SIZE;

    int maxCmdLists = 0;
    for (int i = 0; i < WINDOW_SIZE; i++) {
        int64_t ts = frameHistory[i].timestampUs.load(std::memory_order_acquire);
        int cmd = frameHistory[i].commandLists.load(std::memory_order_acquire);
        if (ts > windowStartUs) {
            if (cmd > maxCmdLists) {
                maxCmdLists = cmd;
            }
        }
    }

    int workThreshold = (maxCmdLists > 4) ? (maxCmdLists / 2) : 2;

    int lastRealLogicalIdx = -1;
    int intervalCounts[16] = {0};
    int totalIntervals = 0;

    for (int i = 0; i < WINDOW_SIZE; i++) {
        int idx = (startIdx + i) % WINDOW_SIZE;

        int64_t ts = frameHistory[idx].timestampUs.load(std::memory_order_acquire);
        int cmd = frameHistory[idx].commandLists.load(std::memory_order_acquire);

        if (ts <= windowStartUs || ts > now)
            continue;

        totalFrames++;
        if (ts < minTs)
            minTs = ts;
        if (ts > maxTs)
            maxTs = ts;

        if (cmd > workThreshold) {
            realFrames++;

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
        return;
    }

    float durationS = (maxTs - minTs) / 1000000.0f;
    if (durationS < 0.1f) {
        return;
    }

    float outputFPS = totalFrames / durationS;
    float baseFPS = realFrames / durationS;

    cachedOutputFPS.store(outputFPS);
    cachedBaseFPS.store(baseFPS);

    int mult = 1;
    if (totalIntervals > 5) {
        int bestInterval = 1;
        int maxCount = 0;

        for (int k = 1; k < 16; k++) {
            if (intervalCounts[k] > maxCount) {
                maxCount = intervalCounts[k];
                bestInterval = k;
            }
        }

        float confidence = (float)maxCount / (float)totalIntervals;

        if (confidence > 0.4f) {
            mult = bestInterval;
        } else {
            mult = cachedMultiplier.load();
        }

        if (mult > 4)
            mult = 4;
    } else {
        if (realFrames > 0) {
            float ratio = (float)totalFrames / (float)realFrames;
            if (ratio >= 3.5f)
                mult = 4;
            else if (ratio >= 2.5f)
                mult = 3;
        }
    }

    int prevMult = cachedMultiplier.exchange(mult);

    if (prevMult != mult && (prevMult == 1 || mult == 1 || prevMult != mult)) {
        HookLog(
            "FG: Multiplier changed %d -> %d (output=%.1f, base=%.1f, real=%d, "
            "total=%d)",
            prevMult, mult, outputFPS, baseFPS, realFrames, totalFrames);
    }
}

void FGCompatibility::DetectPattern() {
    // DORMANT MODE: Skip pattern detection entirely
    if (dormantMode.load()) {
        return;
    }

    if (!IsFGActive()) {
        int mult = cachedMultiplier.load();

        if (mult == 2) {
            int expected = nvidiaSMConfirmCount.load(std::memory_order_acquire);
            while (expected < NVIDIA_SM_CONFIRM_THRESHOLD) {
                if (nvidiaSMConfirmCount.compare_exchange_weak(expected, expected + 1, std::memory_order_acq_rel,
                                                               std::memory_order_acquire)) {
                    expected++;
                } else {
                    expected = nvidiaSMConfirmCount.load(std::memory_order_acquire);
                }
            }

            if (expected == NVIDIA_SM_CONFIRM_THRESHOLD) {
                FGType prev = activeBehavior.exchange(FGType::NVIDIA_SM);
                if (prev != FGType::NVIDIA_SM) {
                    HookLog("FG: NVIDIA Smooth Motion detected (2x multiplier confirmed)");
                }
            }
        } else {
            FGType current = activeBehavior.load(std::memory_order_acquire);
            if (current == FGType::NVIDIA_SM) {
                nvidiaSMConfirmCount.store(0, std::memory_order_release);
                activeBehavior.store(FGType::None, std::memory_order_release);
                HookLog("FG: NVIDIA Smooth Motion pattern broken, resetting to None");
            }
        }
    }
}

void FGCompatibility::DetectNvidiaSmoothMotion() {
    // Called externally to trigger SM detection
    // The actual detection happens in DetectPattern() based on frame analysis
    // Also check for NvPresent64.dll module presence
    CheckForNvPresent();
}

void FGCompatibility::CheckForNvPresent() {
    // Already detected — no need to re-check
    if (nvPresentDetected.load(std::memory_order_acquire))
        return;

    // Throttle checks to at most once per second when not yet detected.
    // NvPresent64 may load after our hook is installed.
    static std::atomic<ULONGLONG> s_lastCheckTick{0};
    ULONGLONG nowTick = GetTickCount64();
    ULONGLONG lastTick = s_lastCheckTick.load(std::memory_order_acquire);
    if (nvPresentChecked.load(std::memory_order_acquire) && lastTick != 0 && nowTick - lastTick < 1000) {
        return;
    }
    s_lastCheckTick.store(nowTick, std::memory_order_release);
    nvPresentChecked.store(true, std::memory_order_release);

    HMODULE hNvPresent = GetModuleHandleW(L"NvPresent64.dll");
    if (!hNvPresent)
        hNvPresent = GetModuleHandleW(L"NvPresent32.dll");

    if (hNvPresent) {
        nvPresentDetected.store(true, std::memory_order_release);
        HookLog(
            "FG: NvPresent64.dll detected — NVIDIA Smooth Motion compatibility "
            "enabled");
    }
}

void FGCompatibility::OnSwapchainRecreation() {
    int count = swapchainRecreationCount.fetch_add(1) + 1;
    int64_t now = GetCurrentTimeUs();
    int64_t lastTime = lastSwapchainRecreationTime.exchange(now);
    int64_t deltaMs = (now - lastTime) / 1000;

    FGType runtime = GetActiveFGType();

    HookLog("FG: Swapchain recreation #%d (delta=%lldms, runtime=%s, dormant=%d)", count, deltaMs,
            GetFGTypeName(runtime), dormantMode.load() ? 1 : 0);

    consecutiveRealFrames.store(0);
    consecutiveInterpolatedFrames.store(0);
}

void FGCompatibility::OnDeviceChange() {
    HookLog("FG: Device change detected (dormant=%d)", dormantMode.load() ? 1 : 0);
    consecutiveRealFrames.store(0);
    consecutiveInterpolatedFrames.store(0);
    nvidiaSMConfirmCount.store(0);
}

void FGCompatibility::LogStatus() const {
    FGType active = GetActiveFGType();
    HookLog(
        "FG Status: active=%s, apiDLSS=%d, apiFSR=%d, mult=%d, "
        "outputFPS=%.1f, baseFPS=%.1f, dormant=%d",
        GetFGTypeName(active), dlssFGApiActive.load() ? 1 : 0, fsrFGApiActive.load() ? 1 : 0, GetFGMultiplier(),
        cachedOutputFPS.load(), cachedBaseFPS.load(), dormantMode.load() ? 1 : 0);
}

// C-linkage exports
extern "C" {
void FG_SetDLSSActive(bool active) {
    g_FGCompat.SetDLSSFGActive(active);
}

void FG_SetFSRActive(bool active) {
    g_FGCompat.SetFSRFGActive(active);
}
}
