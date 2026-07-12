#pragma once

#include <d3d11.h>
#include <stdint.h>
#include <windows.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../common/wgc_pool_lease.h"

// Forward declarations to avoid including WinRT headers in public interface
struct ID3D11Texture2D;
struct IGraphicsCaptureSession;

// CapturedFrame structure for WGC
struct WGCCapturedFrame {
    ID3D11Texture2D* texture = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    int64_t timestamp = 0;  // QPC ticks (same unit as inject mode)
    int64_t rawTimestamp = 0;
    bool isHDR = false;  // True when the captured target is currently HDR/PQ
    bool cursorEmbedded = false;
    int32_t captureLeft = 0;
    int32_t captureTop = 0;
    bool duplicateSourceTimestamp = false;
    // Stable identity of the WGCCapture instance/source that produced this
    // frame. It is captured at delivery time and never inferred from a global
    // coordinator epoch, so retired callbacks cannot be relabeled as the new
    // source during a retarget.
    uint64_t sourceEpoch = 0;
    uint32_t poolSlot = std::numeric_limits<uint32_t>::max();
    uint64_t poolGeneration = 0;
    WgcPoolSlotLease poolLease;
};

// Screen-grab capture engine with two frame-source backends:
// - Windows Graphics Capture (window or monitor items; supports DirectFlip
//   window content that Desktop Duplication cannot scope to a window)
// - DXGI Desktop Duplication (monitor scope only; preferred desktop-recording
//   fallback in auto mode, see InitForMonitorDuplication)
// Both backends share the same copy pool, slot leases, ingress admission,
// smoothness reservoir budget, telemetry counters, and CFR scheduling
// downstream, so smoothness/sync guarantees are identical.
// Fully out-of-process - no code runs in game, compatible with anti-cheat
class WGCCapture {
public:
    WGCCapture();
    ~WGCCapture();

    // Check if WGC is available on this system (Windows 10 1803+)
    static bool IsSupported();

    // Check if a DXGI color space represents HDR (PQ or HLG)
    static bool IsHdrOutputColorSpace(int colorSpace);

    // Query DXGI_OUTPUT_DESC1 for a specific monitor (HDR detection, bits per color, etc.)
    static bool QueryOutputDesc1ForMonitor(HMONITOR monitor, struct DXGI_OUTPUT_DESC1& desc1);

    // Initialize with shared D3D11 device - captures primary monitor
    bool Init(ID3D11Device* device);

    // Initialize with window handle for specific window capture
    bool InitForWindow(ID3D11Device* device, void* hwnd);

    // Initialize with monitor handle for specific monitor capture
    bool InitForMonitor(ID3D11Device* device, void* hmonitor);

    // Initialize monitor capture through the DXGI Desktop Duplication backend
    // (null hmonitor = primary monitor). The duplication feeds the same copy
    // pool, ingress admission, smoothness reservoir, and CFR machinery as the
    // WGC backend. If duplication turns out to be unavailable at StartCapture
    // (format policy, API loss), capture falls back to a WGC monitor item in
    // place so recording start does not fail on a duplication-only problem.
    bool InitForMonitorDuplication(ID3D11Device* device, void* hmonitor);

    // True while the DXGI Desktop Duplication backend is the selected source.
    bool IsUsingDesktopDuplication() const;

    // Duplication-specific diagnostics (0 when the WGC backend is active).
    uint64_t GetDuplicationAcquireTimeoutCount() const;
    uint64_t GetDuplicationAccumulatedMissedFrameCount() const;

    // Duplication cursor-plane state (see DxgiDuplicationSource): when the
    // cursor is composed into the duplicated frames (software cursor),
    // encoder-side cursor composition must be suppressed to avoid a double
    // cursor. Defaults (WGC backend / no session): embedded=false,
    // separate=true.
    bool IsDuplicationCursorEmbedded() const;
    bool IsDuplicationSeparatePointerVisible() const;
    uint64_t GetDuplicationPointerStateTransitionCount() const;

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

    // Drain all currently pending captured frames, oldest to newest, keeping at
    // most the newest maxFrames entries when maxFrames > 0. Caller must release
    // textures in the returned frames when done.
    size_t DrainPendingFrames(std::vector<WGCCapturedFrame>& frames, size_t maxFrames = 0);

    // Set whether the cursor should be included in the WGC capture session.
    void SetCaptureCursor(bool enabled);

    // Get the top-left corner of the captured content in screen coordinates.
    bool GetCaptureOrigin(int32_t& left, int32_t& top) const;

    // Frame statistics for smoothness tracking
    std::atomic<uint32_t> droppedFrames{0};

    // Reset counters for new recording
    void ResetStats();

    // Event signaled when new frame arrives (for efficient polling)
    // Use WaitForSingleObject with timeout based on expected frame interval
    HANDLE GetFrameArrivedEvent() const;

    // Assign the coordinator-owned source identity carried by all subsequent
    // direct and pull-mode frames from this capture instance.
    void SetSourceEpoch(uint64_t sourceEpoch);
    uint64_t GetSourceEpoch() const;

    // OBS-style direct callback: frames processed directly in WinRT callback
    // Callback receives: texture, width, height, QPC timestamp, HDR flag, capture origin
    void SetDirectFrameCallback(std::function<void(ID3D11Texture2D*, uint32_t, uint32_t, int64_t, int64_t, bool, bool,
                                                   bool, int32_t, int32_t, uint64_t, WgcPoolSlotLease&&)>
                                    callback);

    // Get count of frames processed via direct callback
    uint32_t GetCallbackFrameCount() const;

    // Get total frames received from the WGC frame pool before any filtering.
    uint32_t GetInputFrameCount() const;

    // Get last GPU copy time in microseconds (for profiling)
    int64_t GetLastCopyTimeUs() const;

    // Get smoothed source-cadence timing diagnostics (for profiling)
    int64_t GetSourceIntervalAvgUs() const;
    int64_t GetSourceJitterAvgUs() const;
    int64_t GetSourceJitterMaxUs() const;
    int64_t GetSourceToCopyLatencyAvgUs() const;
    int64_t GetSourceToCopyLatencyMaxUs() const;
    uint32_t GetDeliveredRatePerSec() const;
    uint32_t GetDeliveredMin250Fps() const;
    uint32_t GetDeliveredMin500Fps() const;
    uint32_t GetInputMin250Fps() const;
    uint32_t GetInputMin500Fps() const;

    // Get counts of frames skipped for specific reasons (for profiling)
    uint32_t GetPacingSkipCount() const;
    uint32_t GetThrottleSkipCount() const;
    uint32_t GetStaleSkipCount() const;
    uint32_t GetStaleDuplicateTimestampCount() const;
    uint32_t GetStaleOutOfOrderTimestampCount() const;
    uint32_t GetCursorOnlySkipCount() const;
    uint32_t GetPoolDropCount() const;
    uint32_t GetNormalizedDuplicateTimestampCount() const;
    uint32_t GetDuplicateTimestampSkipCount() const;
    uint32_t GetKeyedMutexAcquireFailCount() const;
    uint32_t GetKeyedMutexReleaseFailCount() const;
    uint32_t GetKeyedMutexAbandonedReclaimCount() const;
    uint32_t GetSplitDeviceFlushCount() const;
    uint32_t GetSplitDeviceFlushSkippedCount() const;
    uint32_t GetPoolSlotFastRewriteCount() const;
    int64_t GetLastPoolSlotRewriteUs() const;
    uint32_t GetPoolSlotFreeCurrentCount() const;
    uint32_t GetPoolSlotLeasedMaxCount() const;
    uint32_t GetPoolSlotFreeMinCount() const;
    uint32_t GetPoolSlotOverwritePreventedCount() const;
    uint32_t GetPoolSaturatedDropCount() const;
    uint32_t GetPoolLeaseMismatchCount() const;
    int64_t GetCallbackGapAvgUs() const;
    int64_t GetCallbackGapMaxUs() const;
    int64_t GetCallbackProcessAvgUs() const;
    int64_t GetCallbackProcessMaxUs() const;
    uint32_t GetCallbackDrainMaxCount() const;
    bool IsUsingDedicatedCaptureDevice() const;
    uint32_t GetTexturePoolSlotCount() const;
    uint32_t GetSourceFramePoolBufferCount() const;
    uint32_t GetSmoothnessBudgetSurfaceCount() const;
    uint32_t GetSmoothnessSyncFrameCount() const;
    uint32_t GetSmoothnessSafetySlotCount() const;
    uint32_t GetSmoothnessRetainedFrameCount() const;
    uint32_t GetSmoothnessRetainedFrameCap() const;
    uint32_t GetSmoothnessReservedFreeSlotCount() const;
    uint64_t GetSmoothnessEstimatedVramBytes() const;
    uint64_t GetSmoothnessSourceEstimatedVramBytes() const;
    uint64_t GetSmoothnessCopyEstimatedVramBytes() const;
    uint64_t GetSmoothnessSourceBytesPerSurface() const;
    uint64_t GetSmoothnessCopyBytesPerSurface() const;
    uint32_t GetSmoothnessSourceDxgiFormat() const;
    uint32_t GetSmoothnessCopyDxgiFormat() const;
    bool IsCompactRetainedCopyActive() const;
    int64_t GetLastPoolConvertTimeUs() const;
    uint32_t GetIngressAcceptedCount() const;
    uint32_t GetIngressDecimatedCount() const;
    uint32_t GetIngressAcceptedLowWaterCount() const;
    uint32_t GetIngressAcceptedRecoveryCount() const;
    uint32_t GetIngressAcceptedSourceBelowCount() const;
    uint32_t GetIngressAcceptedHealthyCount() const;
    uint32_t GetIngressAcceptedUniformPlayoutSoftReserveCount() const;
    uint32_t GetIngressAcceptedUniformPlayoutCreditCount() const;
    uint32_t GetIngressDecimatedSoftReserveCount() const;
    uint32_t GetIngressDecimatedHardReserveCount() const;
    uint32_t GetIngressDecimatedCreditCount() const;
    uint32_t GetIngressSoftReservePressureCount() const;
    uint32_t GetIngressHardReservePressureCount() const;
    uint32_t GetIngressRetainedFrameCount() const;
    uint32_t GetIngressRetainedFrameCap() const;
    uint32_t GetIngressLowWaterFrameCount() const;
    uint32_t GetIngressAdmissionReasonCode() const;

    // Throttle local copies to avoid wasting GPU bandwidth on excess frames.
    // Set to target recording FPS. 0 disables local copy throttle.
    void SetTargetFps(uint32_t fps);
    uint32_t GetTargetFps() const;

    // Ask WGC/DWM to produce frames no faster than this cadence. 0 requests max-rate.
    // This does not enable the local copy throttle.
    void SetProducerTargetFps(uint32_t fps);
    uint32_t GetProducerTargetFps() const;

    // Get count of frames skipped by throttle (for profiling)
    uint32_t GetSkippedFrameCount() const;

    // Get count of in-flight OnFrameArrived callbacks (for shutdown synchronization)
    int32_t GetInflightCallbackCount() const;

    // Returns true when the captured display output runs at >8 bpc, even if
    // the actual capture texture fell back to an 8-bit format.
    bool IsHighPrecisionSource() const;

    bool IsWindowTarget() const;
    bool IsTargetWindowValid() const;
    void GetTargetIdentity(HWND* hwnd, HMONITOR* hmonitor) const;
    bool NeedsReset() const;
    std::string ConsumeResetReason();

    // Force-reset WGC session to stop in-flight callbacks (emergency cleanup)
    void ForceReset();

    // Set external throttle flag (e.g., encoder bottleneck indicator).
    // When *throttleFlag is true, CopyResource is skipped but TryGetNextFrame
    // is still drained to return WGC buffers.
    void SetThrottleFlag(const std::atomic<bool>* flag);

    // Experimental WGC performance controls. Defaults preserve legacy behavior.
    void SetSkipSplitDeviceFlush(bool enabled);
    void SetSameDeviceCapture(bool enabled);
    void SetAllowLossyBgra8Pool(bool enabled);
    void SetRequireHighPrecisionCapture(bool enabled);
    void SetAllowDuplicationFallback(bool enabled);
    void SetSmoothnessBufferBudget(bool enabled, uint32_t outputFps, uint32_t maxMs, uint32_t vramBudgetMb,
                                   uint32_t syncDelayFrames = 0);
    void SetVideoMemoryReservationMode(const std::string& mode);
    void SetRetainedFramePressure(uint32_t retainedFrames, uint32_t retainedFrameCap, uint32_t lowWaterFrames,
                                  bool recovering, bool uniformPlayoutOwnsSurplus);

    // Set GPU thread priority for the WGC capture D3D11 device.
    // Passes through to IDXGIDevice::SetGPUThreadPriority.
    // Applies whether using shared or dedicated capture device.
    // When 0 (default adaptive), encoder handles its own adaptive priority.
    void SetGpuPriority(int priority);

private:
    class Impl;  // PIMPL to hide WinRT dependencies
    std::unique_ptr<Impl> impl_;

    ID3D11Device* device_ = nullptr;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool initialized_ = false;
    bool capturing_ = false;
    bool captureCursor_ = false;
    bool roInitialized_ = false;  // true when RoInitialize succeeded and we must call RoUninitialize
};
