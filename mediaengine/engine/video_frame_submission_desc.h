#pragma once

#include <cstdint>
#include "common/capture/cursor_capture_state.h"

// Structured descriptor for submitting a video frame to MediaEngine (inject mode).
struct VideoFrameSubmissionDesc {
    uint64_t textureHandle = 0;
    uint64_t fenceHandle = 0;
    uint64_t fenceValue = 0;
    int64_t timestamp = 0;
    int32_t luidLow = 0;
    int32_t luidHigh = 0;
    uint32_t sourcePid = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    bool isHDR = false;
    bool isShmem = false;
    int shmemSlot = 0;
    const ce::cursor::CaptureState* cursorState = nullptr;
};
