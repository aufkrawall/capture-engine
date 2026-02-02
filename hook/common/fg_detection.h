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

    // DLL-based detection (call once at init or periodically)
    FGType DetectLoadedFGRuntime();

    // Per-frame tracking - call from DetourPresent with command list count
    void RecordFrame(int commandListsExecuted);

    // State queries
    bool IsFGActive() const;
    FGType GetDetectedType() const { return activeBehavior.load(); }
    FGType GetDllDetectedType() const { return detectedRuntime.load(); }  // DLL-based, not behavioral
    int GetFGMultiplier() const { return cachedMultiplier.load(); }

    // FPS metrics
    float GetOutputFPS() const { return cachedOutputFPS.load(); }
    float GetBaseFPS() const { return cachedBaseFPS.load(); }

    // Get last recorded command list count (for real vs interpolated frame detection)
    int GetLastCmdListCount() const { return lastCmdListCount.load(); }
    
    // Query if the current frame is a real frame (has command list work) or interpolated
    // Call this after RecordFrame() to get the classification for the just-recorded frame
    bool IsCurrentFrameReal() const { 
        // Real frames have command list work; interpolated frames don't
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
    
    // FSR FG detection
    bool IsFSRActive() const;

    // NVIDIA Smooth Motion specific
    bool IsNvidiaSmoothMotionActive() const { return detectedRuntime.load() == FGType::NVIDIA_SM; }
    void DetectNvidiaSmoothMotion();  // Call to check for SM specifically

    // Debug
    const char* GetFGTypeName(FGType type) const;
    void LogStatus() const;

private:
    // DLL-based detection result
    std::atomic<FGType> detectedRuntime{FGType::None};

    std::atomic<int64_t> lastRuntimeDetectUs{0};

    // Behavioral detection result
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
    
    // NVIDIA SM detection persistence - require multiple confirmations
    std::atomic<int> nvidiaSMConfirmCount{0};
    static constexpr int NVIDIA_SM_CONFIRM_THRESHOLD = 3;  // Need 3 consecutive detections

    // Internal methods
    void UpdateMetrics();
    void DetectPattern();
    int64_t GetCurrentTimeUs() const;
};

// Global instance
extern FGCompatibility g_FGCompat;
