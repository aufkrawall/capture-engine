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

namespace {

FGCompatibility::FGType RuntimeModeToFGType(ce::fg_runtime::RuntimeMode mode, int dlssMultiplier) {
    switch (mode) {
        case ce::fg_runtime::RuntimeMode::kDLSSFG:
            return dlssMultiplier >= 3 ? FGCompatibility::FGType::DLSS_MSFG : FGCompatibility::FGType::DLSS_FG;
        case ce::fg_runtime::RuntimeMode::kFSRFG:
            return FGCompatibility::FGType::FSR_FG;
        case ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion:
            return FGCompatibility::FGType::NVIDIA_SM;
        case ce::fg_runtime::RuntimeMode::kUnknown:
            return FGCompatibility::FGType::Unknown;
        case ce::fg_runtime::RuntimeMode::kOff:
        case ce::fg_runtime::RuntimeMode::kStreamlineNoFG:
        default:
            return FGCompatibility::FGType::None;
    }
}

}  // namespace

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

ce::fg_runtime::DetectionSnapshot FGCompatibility::CaptureDetectionSnapshot() const {
    ce::fg_runtime::DetectionSnapshot snapshot;
    snapshot.dormant = dormantMode.load(std::memory_order_acquire);
    snapshot.nvPresentLoaded = nvPresentDetected.load(std::memory_order_acquire);
    snapshot.streamlineLoaded = streamlineSupportPresent.load(std::memory_order_acquire);
    snapshot.streamlineFGSignaled = streamlineFGSignal.load(std::memory_order_acquire);
    snapshot.dlssFGApiActive = dlssFGApiActive.load(std::memory_order_acquire);
    snapshot.fsrFGApiActive = fsrFGApiActive.load(std::memory_order_acquire);
    snapshot.heuristicFSRFGActive = heuristicFSRFGActive.load(std::memory_order_acquire);
    snapshot.nvidiaSmoothMotionDetected = nvidiaSmoothMotionDetected.load(std::memory_order_acquire);
    snapshot.dlssFGMultiplier = dlssFGMultiplier.load(std::memory_order_acquire);
    return snapshot;
}

ce::fg_runtime::RuntimeMode FGCompatibility::GetRuntimeMode() const {
    return ce::fg_runtime::ClassifyRuntimeMode(CaptureDetectionSnapshot());
}

FGCompatibility::FGType FGCompatibility::GetActiveFGType() const {
    return RuntimeModeToFGType(GetRuntimeMode(), dlssFGMultiplier.load(std::memory_order_acquire));
}

bool FGCompatibility::IsFGActive() const {
    return ce::fg_runtime::IsRuntimeFGActive(GetRuntimeMode());
}

void FGCompatibility::SetDLSSFGActive(bool active) {
    if (active) {
        // If heuristic FSR FG is active BUT the Streamline hook has NOT
        // signaled FG, suppress — the SL API call might be spurious.
        // When streamlineFGSignal IS set, SL told us it's really running
        // FG, so trust that over the heuristic and clear the FSR flag.
        if (heuristicFSRFGActive.load(std::memory_order_acquire)) {
            if (!streamlineFGSignal.load(std::memory_order_acquire)) {
                HookLog("FG: Suppressing DLSS FG API activation — heuristic FSR FG is active, SL signal OFF");
                return;
            }

            // SL says FG is running AND heuristic says FSR FG — SL wins.
            heuristicFSRFGActive.store(false, std::memory_order_release);
            HookLog("FG: SL FG signal overrides heuristic FSR FG — clearing heuristic flag");
        }

        if (fsrFGApiActive.load(std::memory_order_acquire)) {
            HookLog("FG: Clearing FSR FG API state — DLSS FG API takes priority");
            fsrFGApiActive.store(false, std::memory_order_release);
            fsrFGMultiplier.store(0, std::memory_order_release);
        }
    }

    bool wasActive = dlssFGApiActive.exchange(active, std::memory_order_acq_rel);
    if (!active) {
        dlssFGMultiplier.store(0, std::memory_order_release);
    }
    if (wasActive != active) {
        HookLog("FG: DLSS FG API %s (dormant=%d)", active ? "ACTIVATED" : "DEACTIVATED", dormantMode.load() ? 1 : 0);

        // Enable metrics when FG is confirmed active
        if (active && dormantMode.load()) {
            dormantMode.store(false, std::memory_order_release);
            HookLog("FG: Disabled dormant mode for DLSS FG metrics");
        }

        HookLog("FG: Active type now %s", GetFGTypeName(GetActiveFGType()));
    }
}

void FGCompatibility::SetDLSSFGMultiplier(int multiplier) {
    const int normalizedMultiplier = NormalizeDLSSFGFactor(multiplier);
    const int previousMultiplier = dlssFGMultiplier.exchange(normalizedMultiplier, std::memory_order_acq_rel);
    if (previousMultiplier != normalizedMultiplier) {
        HookLog("FG: DLSS FG multiplier %d -> %d", previousMultiplier, normalizedMultiplier);
    }
}

void FGCompatibility::SetFSRFGMultiplier(int multiplier) {
    const int normalizedMultiplier = (multiplier >= 2 && multiplier <= 4) ? multiplier : 0;
    const int previousMultiplier = fsrFGMultiplier.exchange(normalizedMultiplier, std::memory_order_acq_rel);
    if (previousMultiplier != normalizedMultiplier) {
        HookLog("FG: FSR FG multiplier %d -> %d", previousMultiplier, normalizedMultiplier);
    }
}

void FGCompatibility::SetHeuristicFSRFGActive(bool active) {
    bool wasActive = heuristicFSRFGActive.exchange(active, std::memory_order_acq_rel);
    if (wasActive != active) {
        HookLog("FG: Heuristic FSR FG %s (dormant=%d)", active ? "ACTIVATED" : "DEACTIVATED",
                dormantMode.load() ? 1 : 0);

        // When heuristic FSR FG is confirmed, clear any DLSS FG API false positive.
        // Games like GTA5 Enhanced load Streamline modules even when using FSR FG,
        // and the DLSS FG API state may toggle transiently during the switch.
        if (active && dlssFGApiActive.load(std::memory_order_acquire)) {
            HookLog("FG: Clearing DLSS FG API state — heuristic FSR FG takes priority");
            dlssFGApiActive.store(false, std::memory_order_release);
            dlssFGMultiplier.store(0, std::memory_order_release);
        }

        // Enable metrics computation when FG is confirmed active.  Dormant mode
        // (default) suppresses UpdateMetrics/DetectPattern, so cachedOutputFPS,
        // cachedBaseFPS and cachedMultiplier are never computed.  Disabling dormant
        // mode lets the overlay display accurate base/display FPS for FSR FG.
        if (active && dormantMode.load()) {
            dormantMode.store(false, std::memory_order_release);
            HookLog("FG: Disabled dormant mode for heuristic FSR FG metrics");
        }

        HookLog("FG: Active type now %s", GetFGTypeName(GetActiveFGType()));
    }
}

void FGCompatibility::SetFSRFGActive(bool active) {
    bool wasActive = fsrFGApiActive.exchange(active, std::memory_order_acq_rel);
    if (active) {
        if (!wasActive) {
            directFFXApiConfirmed.store(false, std::memory_order_release);
        }
        if (fsrFGMultiplier.load(std::memory_order_acquire) < 2) {
            fsrFGMultiplier.store(2, std::memory_order_release);
        }
    } else {
        fsrFGMultiplier.store(0, std::memory_order_release);
        directFFXApiConfirmed.store(false, std::memory_order_release);
    }
    if (wasActive != active) {
        HookLog("FG: FSR FG API %s (dormant=%d)", active ? "ACTIVATED" : "DEACTIVATED", dormantMode.load() ? 1 : 0);

        if (active && dlssFGApiActive.load(std::memory_order_acquire)) {
            HookLog("FG: Clearing DLSS FG API state - FSR FG API takes priority");
            dlssFGApiActive.store(false, std::memory_order_release);
            dlssFGMultiplier.store(0, std::memory_order_release);
        }

        if (active && heuristicFSRFGActive.load(std::memory_order_acquire)) {
            heuristicFSRFGActive.store(false, std::memory_order_release);
            HookLog("FG: Clearing heuristic FSR FG state — authoritative FSR FG is active");
        }

        if (active && dormantMode.load()) {
            dormantMode.store(false, std::memory_order_release);
            HookLog("FG: Disabled dormant mode for FSR FG metrics");
        }

        HookLog("FG: Active type now %s", GetFGTypeName(GetActiveFGType()));
    }
}

void FGCompatibility::MarkDirectFFXApiConfirmation() {
    if (!fsrFGApiActive.load(std::memory_order_acquire)) {
        return;
    }

    if (!directFFXApiConfirmed.exchange(true, std::memory_order_acq_rel)) {
        HookLog("FG: Direct FFX API confirmation latched for current FSR FG activation");
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

    if (GetRuntimeMode() == ce::fg_runtime::RuntimeMode::kOff) {
        int mult = cachedMultiplier.load();

        if (mult == 2 && nvPresentDetected.load(std::memory_order_acquire)) {
            const int confirmCount = nvidiaSMConfirmCount.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (confirmCount >= NVIDIA_SM_CONFIRM_THRESHOLD) {
                if (!nvidiaSmoothMotionDetected.exchange(true, std::memory_order_acq_rel)) {
                    HookLog("FG: NVIDIA Smooth Motion detected (2x multiplier confirmed)");
                }
            }
        } else {
            nvidiaSMConfirmCount.store(0, std::memory_order_release);
            if (nvidiaSmoothMotionDetected.exchange(false, std::memory_order_acq_rel)) {
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
