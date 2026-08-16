#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>

#include "fg_runtime_state.h"

// Forward declaration for HookLog
void HookLog(const char* fmt, ...);

class FGCompatibility {
public:
    enum class FGType { None, DLSS_FG, FSR_FG, DLSS_MSFG, NVIDIA_SM, Unknown };

    // =========================================================================
    // DORMANT MODE: FG detection is disabled by default to prevent false
    // positives and interference. Enable via SetDormantMode(false) if needed.
    // =========================================================================
    static constexpr bool kDefaultDormantMode = true;

    void SetDormantMode(bool dormant) {
        dormantMode.store(dormant);
    }
    bool IsDormant() const {
        return dormantMode.load();
    }

    // Per-frame tracking - call from DetourPresent with command list count
    // NOTE: In dormant mode, this only tracks basic stats, no pattern detection
    void RecordFrame(int commandListsExecuted);
    void RecordPresentForNvidiaSmoothMotion();

    // State queries
    bool IsFGActive() const;
    FGType GetActiveFGType() const;
    ce::fg_runtime::RuntimeMode GetRuntimeMode() const;
    bool HasStreamlineSupport() const {
        return streamlineSupportPresent.load(std::memory_order_acquire) ||
               streamlineFGSignal.load(std::memory_order_acquire);
    }
    bool HasFSRFGSupport() const {
        return fsrSupportPresent.load(std::memory_order_acquire) || fsrFGApiActive.load(std::memory_order_acquire) ||
               heuristicFSRFGActive.load(std::memory_order_acquire);
    }
    void SetStreamlineSupportPresent(bool present) {
        streamlineSupportPresent.store(present, std::memory_order_release);
    }
    void SetFSRFGSupportPresent(bool present) {
        fsrSupportPresent.store(present, std::memory_order_release);
    }
    int GetFGMultiplier() const {
        const auto runtimeMode = GetRuntimeMode();
        if (runtimeMode == ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion) {
            return 2;
        }
        if (runtimeMode == ce::fg_runtime::RuntimeMode::kFSRFG) {
            const int fsrMultiplier = fsrFGMultiplier.load(std::memory_order_acquire);
            if (fsrMultiplier >= 2) {
                return fsrMultiplier;
            }
            if (fsrFGApiActive.load(std::memory_order_acquire)) {
                return 2;
            }
        }

        const int apiMultiplier = dlssFGMultiplier.load(std::memory_order_acquire);
        return apiMultiplier >= 2 ? apiMultiplier : cachedMultiplier.load(std::memory_order_acquire);
    }

    // FPS metrics
    float GetOutputFPS() const {
        return cachedOutputFPS.load();
    }
    float GetBaseFPS() const {
        // When FG multiplier is known (from Streamline API or pattern analysis),
        // derive base FPS directly from output FPS.  This is more reliable than
        // ECL-count-based cachedBaseFPS for DLSS FG (where our ECL hook counts
        // ALL queues including Streamline's internal FG queue).  For heuristic
        // FSR FG, cachedMultiplier from pattern analysis provides the multiplier.
        const int mult = GetFGMultiplier();
        if (mult >= 2) {
            float output = cachedOutputFPS.load();
            if (output > 1.0f)
                return output / static_cast<float>(mult);
        }
        return cachedBaseFPS.load();
    }

    // Get last recorded command list count (for real vs interpolated frame
    // detection)
    int GetLastCmdListCount() const {
        return lastCmdListCount.load();
    }

    // Query if the current frame is a real frame (has command list work) or
    // interpolated
    bool IsCurrentFrameReal() const {
        return lastCmdListCount.load(std::memory_order_acquire) > 0;
    }

    // Get consecutive real/interpolated frame counts for pattern detection
    int GetConsecutiveRealFrames() const {
        return consecutiveRealFrames.load(std::memory_order_acquire);
    }
    int GetConsecutiveInterpolatedFrames() const {
        return consecutiveInterpolatedFrames.load(std::memory_order_acquire);
    }

    // Events
    void OnSwapchainRecreation();
    void OnDeviceChange();

    // Returns true if FSR FG is active (based on API detection)
    bool IsFSRActive() const {
        return fsrFGApiActive.load(std::memory_order_acquire);
    }

    // NVIDIA Smooth Motion specific
    bool IsNvidiaSmoothMotionActive() const {
        return nvidiaSmoothMotionDetected.load(std::memory_order_acquire);
    }
    void DetectNvidiaSmoothMotion();

    // NvPresent64.dll module detection (driver-level Smooth Motion)
    bool IsNvPresentLoaded() {
        if (!nvPresentDetected.load(std::memory_order_acquire)) {
            CheckForNvPresent();
        }
        return nvPresentDetected.load(std::memory_order_acquire);
    }
    void MarkNvPresentLoaded();
    void CheckForNvPresent();

    // Debug
    const char* GetFGTypeName(FGType type) const;
    void LogStatus() const;

    // =========================================================================
    // API-based FG activation (called by nvngx_hook and ffx_hook)
    // These are called when FG is actually created/destroyed via the APIs
    // NOTE: These still work in dormant mode - they just report API state
    // =========================================================================
    void SetDLSSFGActive(bool active);
    void SetDLSSFGMultiplier(int multiplier);
    void SetFSRFGActive(bool active);
    void SetFSRFGMultiplier(int multiplier);
    void SetHeuristicFSRFGActive(bool active);
    void MarkDirectFFXApiConfirmation();
    bool IsDLSSFGApiActive() const {
        return dlssFGApiActive.load(std::memory_order_acquire);
    }
    bool IsFSRFGApiActive() const {
        return fsrFGApiActive.load(std::memory_order_acquire);
    }
    bool IsHeuristicFSRFGActive() const {
        return heuristicFSRFGActive.load(std::memory_order_acquire);
    }
    bool HasDirectFFXApiConfirmation() const {
        return directFFXApiConfirmed.load(std::memory_order_acquire);
    }

    // Records an FFX-API frame-generation enable/disable that the game itself
    // performed (an ffxConfigure whose enabled state actually changed). This is
    // deliberately NOT inferred from SetFSRFGActive, because several callers
    // clear that flag heuristically - stale-latch recovery after a real-frame
    // streak, for instance - and a heuristic clear must not be mistaken for the
    // game saying "frame generation is off".
    void NotifyAuthoritativeFSRFGApiTransition(bool enabled) {
        fsrFGAuthoritativeApiOff.store(!enabled, std::memory_order_release);
    }

    // True while the most recent authoritative FFX-API transition was a
    // disable. The heuristic FSR detector must not contradict that.
    bool HasAuthoritativeFSRFGApiOff() const {
        return fsrFGAuthoritativeApiOff.load(std::memory_order_acquire);
    }

    // Clear NVIDIA Smooth Motion detection state.  Called when SL FG turns OFF
    // to prevent the cached 2× multiplier from falsely triggering NVIDIA_SM.
    void ClearNvidiaSMState() {
        nvidiaSMConfirmCount.store(0, std::memory_order_release);
        // Reset cached multiplier — after SL FG teardown the real multiplier
        // is 1×.  Without this reset, the stale 2× causes NVIDIA_SM to be
        // re-detected within 3 frames, triggering a phantom FG transition
        // and a second unnecessary cooldown.
        cachedMultiplier.store(1, std::memory_order_release);
        if (nvidiaSmoothMotionDetected.exchange(false, std::memory_order_acq_rel)) {
            HookLog("FG: Cleared NVIDIA_SM state + multiplier (SL FG transition cleanup)");
        }
    }

    // Direct signal from Streamline hook — faster than heuristic detection.
    void SetStreamlineFGSignal(bool active) {
        streamlineFGSignal.store(active, std::memory_order_release);
    }
    bool IsStreamlineFGSignaled() const {
        return streamlineFGSignal.load(std::memory_order_acquire);
    }

private:
    // Dormant mode flag - when true, skip all pattern-based detection
    std::atomic<bool> dormantMode{kDefaultDormantMode};

    // Frame history for pattern detection
    static constexpr int WINDOW_SIZE = 120;  // ~2 seconds at 60fps
    struct FrameRecord {
        std::atomic<int64_t> timestampUs{0};
        std::atomic<int> commandLists{0};
    };
    // NOTE: FrameRecord uses atomics so concurrent RecordFrame calls writing to
    // different slots and UpdateMetrics reading all slots do not race.
    std::array<FrameRecord, WINDOW_SIZE> frameHistory;
    std::atomic<int> historyIndex{0};
    std::atomic<int> totalFramesRecorded{0};

    // Cached metrics (updated periodically)
    std::atomic<float> cachedOutputFPS{0.0f};
    std::atomic<float> cachedBaseFPS{0.0f};
    std::atomic<int> cachedMultiplier{1};

    // Stability tracking
    std::atomic<int> swapchainRecreationCount{0};
    std::atomic<int64_t> lastSwapchainRecreationTime{0};

    // Debug counters
    std::atomic<int> debugLogCounter{0};
    std::atomic<int> consecutiveRealFrames{0};
    std::atomic<int> consecutiveInterpolatedFrames{0};
    std::atomic<int> lastCmdListCount{0};

    // NVIDIA SM detection persistence
    std::atomic<int> nvidiaSMConfirmCount{0};
    static constexpr int NVIDIA_SM_CONFIRM_THRESHOLD = 3;

    // NvPresent64.dll module detected in process
    std::atomic<bool> nvPresentDetected{false};
    std::atomic<bool> nvPresentChecked{false};

    // Usage-based FG activation flags (set by API hooks)
    std::atomic<bool> dlssFGApiActive{false};
    std::atomic<bool> fsrFGApiActive{false};
    std::atomic<bool> heuristicFSRFGActive{false};
    std::atomic<bool> directFFXApiConfirmed{false};
    std::atomic<bool> fsrFGAuthoritativeApiOff{false};
    std::atomic<int> dlssFGMultiplier{0};
    std::atomic<int> fsrFGMultiplier{0};
    std::atomic<bool> streamlineSupportPresent{false};
    std::atomic<bool> fsrSupportPresent{false};
    std::atomic<bool> nvidiaSmoothMotionDetected{false};

    // Direct signal from Streamline hook — set IMMEDIATELY when
    // slDLSSGSetOptions transitions FG on/off.  Faster than heuristic.
    std::atomic<bool> streamlineFGSignal{false};

    // Internal methods
    ce::fg_runtime::DetectionSnapshot CaptureDetectionSnapshot() const;
    void UpdateMetrics();
    void DetectPattern();
    int64_t GetCurrentTimeUs() const;
};

// Global instance
extern FGCompatibility g_FGCompat;

// C-linkage exports for cross-module hooks
extern "C" {
void FG_SetDLSSActive(bool active);
void FG_SetFSRActive(bool active);
}
