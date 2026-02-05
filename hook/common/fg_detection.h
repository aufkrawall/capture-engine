#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>

// Forward declaration for HookLog
void HookLog(const char* fmt, ...);

class FGCompatibility {
public:
    enum class FGType { None, DLSS_FG, FSR_FG, DLSS_MSFG, NVIDIA_SM, Unknown };

    // Per-frame tracking - call from DetourPresent with command list count
    void RecordFrame(int commandListsExecuted);

    // State queries
    bool IsFGActive() const;
    FGType GetActiveFGType() const;
    int GetFGMultiplier() const { return cachedMultiplier.load(); }

    // FPS metrics
    float GetOutputFPS() const { return cachedOutputFPS.load(); }
    float GetBaseFPS() const { return cachedBaseFPS.load(); }

    // Get last recorded command list count (for real vs interpolated frame detection)
    int GetLastCmdListCount() const { return lastCmdListCount.load(); }
    
    // Query if the current frame is a real frame (has command list work) or interpolated
    bool IsCurrentFrameReal() const { 
        return lastCmdListCount.load(std::memory_order_acquire) > 0; 
    }
    
    // Get consecutive real/interpolated frame counts for pattern detection
    int GetConsecutiveRealFrames() const { return consecutiveRealFrames.load(std::memory_order_acquire); }
    int GetConsecutiveInterpolatedFrames() const { return consecutiveInterpolatedFrames.load(std::memory_order_acquire); }

    // Events
    void OnSwapchainRecreation();
    void OnDeviceChange();

    // Safety suspend - delays overlay initialization after FG detection
    void SuspendFor(int milliseconds);
    bool IsSuspended() const;
    
    // Returns true if FSR FG is active (based on API detection)
    bool IsFSRActive() const { return fsrFGApiActive.load(std::memory_order_acquire); }

    // Returns true if overlay rendering should be skipped for FSR FG only
    // FSR FG is more sensitive to external GPU commands than DLSS FG
    // DLSS FG can handle overlay rendering on all frames
    bool ShouldSkipOverlayForFG() const { return IsFSRActive(); }

    // Returns true if FG needs COMPLETE pass-through
    // (no COM operations, no overlay, nothing during Present)
    bool NeedsCompletePassthrough() const { return IsFSRActive() || IsDLSSFGApiActive(); }

    // Frame counter for FSR FG stabilization period
    int GetFramesSinceFSRActivation() const { return framesSinceFSRActivation.load(); }
    void OnFSRFGActivated();

    // NVIDIA Smooth Motion specific
    bool IsNvidiaSmoothMotionActive() const { return activeBehavior.load() == FGType::NVIDIA_SM; }
    void DetectNvidiaSmoothMotion();

    // Debug
    const char* GetFGTypeName(FGType type) const;
    void LogStatus() const;
    
    // =========================================================================
    // API-based FG activation (called by nvngx_hook and ffx_hook)
    // These are called when FG is actually created/destroyed via the APIs
    // =========================================================================
    void SetDLSSFGActive(bool active);
    void SetFSRFGActive(bool active);
    bool IsDLSSFGApiActive() const { return dlssFGApiActive.load(std::memory_order_acquire); }
    bool IsFSRFGApiActive() const { return fsrFGApiActive.load(std::memory_order_acquire); }

private:
    // Active FG type based on API activation (not DLL detection)
    std::atomic<FGType> activeBehavior{FGType::None};

    // Frame history for pattern detection
    static constexpr int WINDOW_SIZE = 120;  // ~2 seconds at 60fps
    struct FrameRecord {
        int64_t timestampUs = 0;
        int commandLists = 0;
    };
    std::array<FrameRecord, WINDOW_SIZE> frameHistory{};
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

    // Safety suspend
    std::atomic<int64_t> suspendUntilUs{0};
    
    // NVIDIA SM detection persistence
    std::atomic<int> nvidiaSMConfirmCount{0};
    static constexpr int NVIDIA_SM_CONFIRM_THRESHOLD = 3;

    // Usage-based FG activation flags (set by API hooks)
    std::atomic<bool> dlssFGApiActive{false};
    std::atomic<bool> fsrFGApiActive{false};

    // FSR FG activation tracking for stabilization period
    std::atomic<int64_t> fsrActivationTimeUs{0};
    std::atomic<int> framesSinceFSRActivation{0};

    // Internal methods
    void UpdateMetrics();
    void DetectPattern();
    int64_t GetCurrentTimeUs() const;
    
    // DEPRECATED: Kept for compatibility but does nothing
    FGType DetectLoadedFGRuntime();
};

// Global instance
extern FGCompatibility g_FGCompat;

// C-linkage exports for cross-module hooks
extern "C" {
    void FG_SetDLSSActive(bool active);
    void FG_SetFSRActive(bool active);
}
