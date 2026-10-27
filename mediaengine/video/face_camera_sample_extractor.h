// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include <d3d11.h>
#include <mfidl.h>
#include <cstdint>
#include <memory>
#include <vector>
#include "face_camera_format.h"

struct FaceCameraFrame {
    FaceCameraFrame() = default;
    ~FaceCameraFrame();
    FaceCameraFrame(const FaceCameraFrame&) = delete;
    FaceCameraFrame& operator=(const FaceCameraFrame&) = delete;
    FaceCameraFrame(FaceCameraFrame&&) = delete;
    FaceCameraFrame& operator=(FaceCameraFrame&&) = delete;

    uint64_t sequence = 0;
    uint64_t receivedTickMs = 0;
    // When the picture was taken: the device's sample time when it is a
    // plausible QPC time (Media Foundation capture clocks are QPC based),
    // otherwise when ReadSample returned it.
    int64_t captureQpc = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t gpuSubresource = 0;
    ce::face_camera::PixelFormat format = ce::face_camera::PixelFormat::kBgra8;
    ce::face_camera::YuvMatrix matrix = ce::face_camera::YuvMatrix::kBt709;
    ce::face_camera::YuvRange range = ce::face_camera::YuvRange::kLimited;
    IMFSample* gpuSample = nullptr;
    // Primary CPU plane: packed BGRA8 rows, NV12 luma (Y) rows, or packed YUY2 rows.
    std::vector<uint8_t> bgra;
    // Secondary CPU plane: NV12 interleaved Cb,Cr rows; empty for BGRA8 and YUY2.
    std::vector<uint8_t> chroma;
};

namespace ce::face_camera {

enum class SampleStatus : uint8_t { kPublished, kCancelled, kFailed };

struct SampleExtraction {
    SampleStatus status = SampleStatus::kFailed;
    std::shared_ptr<const FaceCameraFrame> frame;
};

// Capture-worker-owned extraction of an immutable GPU/CPU camera frame. Cancellation is sampled before the
// transaction: a real extraction error stays a failure even if stop arrives while the buffer is being read.
class SampleExtractor {
public:
    SampleExtraction Extract(IMFSample* sample, uint32_t width, uint32_t height, LONG stride, PixelFormat format,
                             YuvMatrix matrix, YuvRange range, LONGLONG sampleTime100ns, bool cancelled);

private:
    uint64_t nextSequence_ = 1;
    bool timeBasisLogged_ = false;
};

}  // namespace ce::face_camera
