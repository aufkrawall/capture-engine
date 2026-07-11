#pragma once

// DXGI Desktop Duplication frame source.
//
// This is a monitor-scope frame SOURCE only: it acquires desktop frames via
// IDXGIOutputDuplication on a dedicated capture thread and hands each frame
// texture to a sink callback. All downstream policy (copy pool, slot leases,
// ingress admission, CFR smoothness reservoir, telemetry) is owned by the
// shared screen-grab engine in wgc_capture.cpp, which drives this source as an
// alternative backend to the WinRT WGC session. That keeps every validated CFR
// smoothness/sync guarantee identical across WGC and duplication capture.
//
// Duplication semantics that differ from WGC and are handled here:
// - Frames are delivered only when the desktop image changes (frame-on-change).
//   CFR repeats downstream cover static content; AcquireNextFrame timeouts are
//   normal idle, not errors.
// - The hardware cursor is NOT composed into duplicated frames, which matches
//   the project cursor policy exactly (encoder-side cursor composition from
//   GetCursorInfo; the live cursor stays in the hardware plane).
// - Pointer-only updates arrive with LastPresentTime == 0 and are reported to
//   the sink as cursor-only updates (no frame content change).
// - DXGI_ERROR_ACCESS_LOST (mode change, HDR toggle, secure desktop, desktop
//   switch) requires full re-initialization; the sink is told to request a
//   reset and the existing retarget machinery re-primes the backend.
// - The delivered surface format is the native composed desktop format; there
//   is no server-side format conversion. Format acceptability against the
//   requested bit-depth policy is validated at Init.

#include <d3d11.h>
#include <dxgi1_6.h>
#include <stdint.h>
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

struct DxgiDuplicationFrameSink {
    // Called on the duplication capture thread. The texture is only valid for
    // the duration of the call (it is released back to DXGI immediately after
    // the sink returns), so the sink must synchronously submit its GPU copy.
    std::function<void(ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC& desc, int64_t rawSourceQpc,
                       uint32_t accumulatedFrames)>
        onFrame;
    // Pointer-shape/position-only desktop update (no content change).
    std::function<void()> onCursorOnlyUpdate;
    // Unrecoverable duplication loss; the owner must stop and re-initialize.
    std::function<void(const char* reason)> onResetNeeded;
};

class DxgiDuplicationSource {
public:
    DxgiDuplicationSource() = default;
    ~DxgiDuplicationSource();

    DxgiDuplicationSource(const DxgiDuplicationSource&) = delete;
    DxgiDuplicationSource& operator=(const DxgiDuplicationSource&) = delete;

    // Creates the duplication for the output that owns `monitor` (primary
    // monitor when null) on `device`'s adapter. The format list offered to
    // DuplicateOutput1 offers only formats which satisfy the requested
    // precision contract: R10/FP16 for 10-bit SDR and FP16 for HDR. Ground
    // truth is the first acquired frame's texture desc; ModeDesc.Format only
    // reflects the display mode and may understate the delivered precision.
    // On failure `failureReason` receives a stable, log-friendly reason string.
    bool Init(ID3D11Device* device, HMONITOR monitor, bool requireHighPrecision, bool outputIsHdr,
              std::string* failureReason);

    // Starts the acquisition thread. Sink callbacks fire on that thread.
    bool Start(DxgiDuplicationFrameSink sink);

    // Stable diagnostic populated when Start cannot prove an acceptable first
    // frame. Valid until the next Start call.
    std::string GetStartFailureReason() const;

    // Stops the acquisition thread and releases the duplication.
    void Stop();

    bool IsRunning() const {
        return running_.load(std::memory_order_acquire);
    }

    uint32_t GetWidth() const {
        return width_;
    }
    uint32_t GetHeight() const {
        return height_;
    }
    DXGI_FORMAT GetFormat() const {
        return static_cast<DXGI_FORMAT>(format_.load(std::memory_order_acquire));
    }
    HMONITOR GetMonitor() const {
        return monitor_;
    }

    // Diagnostics (thread-safe counters).
    uint64_t GetAcquireTimeoutCount() const {
        return acquireTimeoutCount_.load(std::memory_order_relaxed);
    }
    uint64_t GetCursorOnlyUpdateCount() const {
        return cursorOnlyUpdateCount_.load(std::memory_order_relaxed);
    }
    uint64_t GetAccumulatedMissedFrameCount() const {
        return accumulatedMissedFrameCount_.load(std::memory_order_relaxed);
    }
    uint64_t GetProtectedContentMaskedFrameCount() const {
        return protectedContentMaskedFrameCount_.load(std::memory_order_relaxed);
    }
    uint32_t GetConsecutiveAcquireFailureCount() const {
        return consecutiveAcquireFailures_.load(std::memory_order_relaxed);
    }
    int32_t GetLastAcquireHr() const {
        return lastAcquireHr_.load(std::memory_order_relaxed);
    }

    // Cursor plane state, derived from DXGI_OUTDUPL_FRAME_INFO.PointerPosition:
    // - separate pointer visible  => the live cursor is on the hardware cursor
    //   plane and is NOT part of the duplicated image (consumer must draw it —
    //   our encoder-side composition does).
    // - separate pointer NOT visible while GetCursorInfo says the cursor is
    //   showing inside this monitor => Windows composes the cursor into the
    //   desktop image (software cursor); the duplicated frames already CONTAIN
    //   the cursor and encoder-side composition must be suppressed to avoid a
    //   double cursor.
    // This doubles as a live hardware/software-cursor-plane detector for
    // diagnostics.
    bool IsSeparatePointerVisible() const {
        return separatePointerVisible_.load(std::memory_order_relaxed);
    }
    bool IsCursorEmbeddedInFrames() const {
        return cursorEmbeddedInFrames_.load(std::memory_order_relaxed);
    }
    uint64_t GetPointerStateTransitionCount() const {
        return pointerStateTransitions_.load(std::memory_order_relaxed);
    }

private:
    void CaptureThreadFunc();
    void ReleaseDuplication();

    void UpdatePointerState(bool separatePointerVisible);

    ID3D11Device* device_ = nullptr;  // Not owned.
    IDXGIOutputDuplication* duplication_ = nullptr;
    HMONITOR monitor_ = nullptr;
    RECT monitorRect_ = {};
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    // The capture thread replaces the ModeDesc hint with the first delivered
    // texture's ground-truth format. Consumers may query concurrently.
    std::atomic<uint32_t> format_{static_cast<uint32_t>(DXGI_FORMAT_UNKNOWN)};
    bool desktopImageInSystemMemory_ = false;
    bool requireHighPrecision_ = false;
    bool outputIsHdr_ = false;
    bool deliveredFormatLogged_ = false;

    enum class FirstFrameState : uint8_t { Pending, Valid, Invalid };
    mutable std::mutex firstFrameMutex_;
    std::condition_variable firstFrameCv_;
    FirstFrameState firstFrameState_ = FirstFrameState::Pending;
    std::string firstFrameFailureReason_;

    std::thread captureThread_;
    std::atomic<bool> shutdown_{false};
    std::atomic<bool> running_{false};
    std::mutex shutdownMutex_;
    std::condition_variable shutdownCv_;
    DxgiDuplicationFrameSink sink_;

    std::atomic<uint64_t> acquireTimeoutCount_{0};
    std::atomic<uint64_t> cursorOnlyUpdateCount_{0};
    std::atomic<uint64_t> accumulatedMissedFrameCount_{0};
    std::atomic<uint64_t> protectedContentMaskedFrameCount_{0};
    std::atomic<uint32_t> consecutiveAcquireFailures_{0};
    std::atomic<int32_t> lastAcquireHr_{0};
    std::atomic<uint64_t> deliveredFrameCount_{0};
    // Safe defaults: assume hardware cursor (separate pointer) until frame
    // info says otherwise. A wrong initial "not embedded" can only overdraw
    // the identical cursor shape at the identical position (invisible).
    std::atomic<bool> separatePointerVisible_{true};
    std::atomic<bool> cursorEmbeddedInFrames_{false};
    std::atomic<uint64_t> pointerStateTransitions_{0};
};
