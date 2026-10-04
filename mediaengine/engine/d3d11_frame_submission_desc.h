#pragma once

#include <cstdint>
#include "common/capture/cursor_capture_state.h"

// Structured descriptor for submitting a D3D11 frame to MediaEngine (framegrab / WGC mode).
struct D3D11FrameSubmissionDesc {
    void* texture = nullptr;  // caller retains the D3D11 texture; consumed synchronously
    int64_t timestamp = 0;    // source timestamp in QPC ticks
    uint32_t width = 0;
    uint32_t height = 0;
    bool isHDR = false;
    int32_t captureLeft = 0;
    int32_t captureTop = 0;
    int64_t timelineElapsedUs = -1;  // CFR microseconds from first output; -1 derives timing
    const ce::cursor::CaptureState* cursorState = nullptr;  // borrowed for this submission only
};
