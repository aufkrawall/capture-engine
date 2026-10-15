#pragma once

#include <cstdint>

// Per-Present accounting of DX9 capture work for the perf CSV.
//
// CaptureFrame runs either inside the Present detour's timed capture window or,
// when a nested EndScene draws the overlay, deferred into that EndScene. Only the
// deferred work is outside the window, so it is reported separately and added by
// the Present that consumes it. Consume() resets, so a Present without a capture
// (cadence skip) reports zero instead of repeating the previous frame's values.
// All calls happen on the presenting thread.
namespace ce::dx9_capture_cost {

struct Snapshot {
    int32_t deferredCaptureUs = 0;  // CaptureFrame time spent outside the Present window
    int32_t ringDrainUs = 0;        // shared ring: nonblocking completion polling + publish
    int32_t ringSubmitUs = 0;       // shared ring: StretchRect + event-query issue
    uint32_t ringSubmits = 0;       // shared-ring frames submitted since the last consume
};

class Accumulator {
public:
    void AddDeferredCapture(int64_t us) { deferredUs_ += Clamp(us); }

    void AddRingFrame(int64_t drainUs, int64_t submitUs, bool submitted) {
        drainUs_ += Clamp(drainUs);
        submitUs_ += Clamp(submitUs);
        if (submitted)
            ++submits_;
    }

    Snapshot Consume() {
        Snapshot snapshot;
        snapshot.deferredCaptureUs = Saturate(deferredUs_);
        snapshot.ringDrainUs = Saturate(drainUs_);
        snapshot.ringSubmitUs = Saturate(submitUs_);
        snapshot.ringSubmits = submits_;
        deferredUs_ = drainUs_ = submitUs_ = 0;
        submits_ = 0;
        return snapshot;
    }

private:
    static int64_t Clamp(int64_t us) { return us > 0 ? us : 0; }
    static int32_t Saturate(int64_t us) { return us > INT32_MAX ? INT32_MAX : static_cast<int32_t>(us); }

    int64_t deferredUs_ = 0;
    int64_t drainUs_ = 0;
    int64_t submitUs_ = 0;
    uint32_t submits_ = 0;
};

}  // namespace ce::dx9_capture_cost
