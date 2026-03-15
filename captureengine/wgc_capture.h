#pragma once

#include <d3d11.h>
#include <stdint.h>
#include <windows.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

// Forward declarations to avoid including WinRT headers in public interface
struct ID3D11Texture2D;
struct IGraphicsCaptureSession;

// CapturedFrame structure for WGC
struct WGCCapturedFrame {
    ID3D11Texture2D* texture = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    int64_t timestamp = 0;  // QPC ticks (same unit as inject mode)
};

// Windows Graphics Capture implementation
// Supports DirectFlip capture that Desktop Duplication cannot handle
// Fully out-of-process - no code runs in game, compatible with anti-cheat
class WGCCapture {
public:
    WGCCapture();
    ~WGCCapture();

    // Check if WGC is available on this system (Windows 10 1803+)
    static bool IsSupported();

    // Initialize with shared D3D11 device - captures primary monitor
    bool Init(ID3D11Device* device);

    // Initialize with window handle for specific window capture
    bool InitForWindow(ID3D11Device* device, void* hwnd);

    // Initialize with monitor handle for specific monitor capture
    bool InitForMonitor(ID3D11Device* device, void* hmonitor);

    // Start/stop capture session
    bool StartCapture();
    void StopCapture();
    bool IsCapturing() const {
        return capturing_;
    }

    // Get dimensions
    uint32_t GetWidth() const {
        return width_;
    }
    uint32_t GetHeight() const {
        return height_;
    }

    // Get next captured frame (non-blocking, returns false if no frame available)
    // Caller must release texture when done
    // Timestamp is in QPC ticks (consistent with inject mode)
    bool GetNextFrame(WGCCapturedFrame& frame);

    // Set whether to capture the system cursor natively (default: false)
    // Must be called before StartCapture
    void SetCaptureCursor(bool enabled);

    // Optional: Set callback for frame arrival (for async processing)
    using FrameCallback = std::function<void(WGCCapturedFrame&)>;
    void SetFrameCallback(FrameCallback callback) {
        frameCallback_ = callback;
    }

    // Frame statistics for smoothness tracking
    std::atomic<uint32_t> droppedFrames{0};

    // Reset counters for new recording
    void ResetStats() {
        droppedFrames.store(0, std::memory_order_relaxed);
    }

    // Event signaled when new frame arrives (for efficient polling)
    // Use WaitForSingleObject with timeout based on expected frame interval
    HANDLE GetFrameArrivedEvent() const;

    // OBS-style direct callback: frames processed directly in WinRT callback
    // Callback receives: texture, width, height, timestampMs
    void SetDirectFrameCallback(std::function<void(ID3D11Texture2D*, uint32_t, uint32_t, int64_t)> callback);

    // Get count of frames processed via direct callback
    uint32_t GetCallbackFrameCount() const;

    // Get last GPU copy time in microseconds (for profiling)
    int64_t GetLastCopyTimeUs() const;

    // Throttle capture rate to avoid wasting GPU bandwidth on excess frames.
    // Set to target recording FPS. 0 disables throttle.
    void SetTargetFps(uint32_t fps);

    // Get count of frames skipped by throttle (for profiling)
    uint32_t GetSkippedFrameCount() const;

    // Get count of in-flight OnFrameArrived callbacks (for shutdown synchronization)
    int32_t GetInflightCallbackCount() const;

    // Force-reset WGC session to stop in-flight callbacks (emergency cleanup)
    void ForceReset();

    // Set external throttle flag (e.g., encoder bottleneck indicator).
    // When *throttleFlag is true, CopyResource is skipped but TryGetNextFrame
    // is still drained to return WGC buffers.
    void SetThrottleFlag(const std::atomic<bool>* flag);

private:
    class Impl;  // PIMPL to hide WinRT dependencies
    std::unique_ptr<Impl> impl_;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool initialized_ = false;
    bool capturing_ = false;
    bool captureCursor_ = false;
    bool roInitialized_ = false;  // true when RoInitialize succeeded and we must call RoUninitialize

    FrameCallback frameCallback_;
    std::mutex frameMutex_;
};
