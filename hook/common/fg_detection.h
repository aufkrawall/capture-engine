#pragma once
#include <windows.h>
#include <atomic>
#include <chrono>

class FGCompatibility {
public:
    enum class FGType { None, DLSS_FG, FSR_FG, Unknown };
    
    // DLL-based detection (call once at init or periodically)
    static FGType DetectLoadedFGRuntime();
    
    // Behavioral detection (call on each relevant event)
    void OnDeviceChange();
    void OnSwapchainRecreation();
    
    // State queries
    bool IsFGLikelyActive() const;
    FGType GetDetectedType() const;
    int GetRecommendedInitDelayFrames() const;
    
    // FPS tracking
    void RecordPresentCall();  // Called on EVERY Present, including FG frames
    void RecordRealFrame();    // Called only for detected real frames
    
    float GetOutputFPS() const;  // Returns post-FG FPS (all presents)
    float GetBaseFPS() const;    // Returns base FPS (real frames only)
    
    // Safety suspend
    void SuspendFor(int milliseconds);
    
private:
    FGType detectedType = FGType::None;

    // Safety suspend timestamp (implementation detail)
    std::atomic<int64_t> suspendUntil{0}; 
    
    // Behavioral tracking
    std::atomic<int> deviceChangeCount{0};
    std::atomic<int> swapchainRecreationCount{0};
    std::chrono::steady_clock::time_point lastCheck;
    
    // FPS tracking
    std::atomic<int> presentCount{0};
    std::atomic<int> realFrameCount{0};
    std::chrono::steady_clock::time_point fpsWindowStart;
    
    // Cached FPS values (updated periodically)
    std::atomic<float> cachedOutputFPS{0.0f};
    std::atomic<float> cachedBaseFPS{0.0f};
    
    void CheckBehavioralPatterns();
    
    // Safety suspend
    // std::atomic<int64_t> suspendUntil{0}; // Moved up
};

// Global instance
extern FGCompatibility g_FGCompat;
