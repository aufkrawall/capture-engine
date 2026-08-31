#pragma once

#include <d3d11.h>
#include <mfidl.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "../common/face_camera_config.h"

struct IMFSourceReader;

struct FaceCameraFrame {
    FaceCameraFrame() = default;
    ~FaceCameraFrame();
    FaceCameraFrame(const FaceCameraFrame&) = delete;
    FaceCameraFrame& operator=(const FaceCameraFrame&) = delete;
    FaceCameraFrame(FaceCameraFrame&&) = delete;
    FaceCameraFrame& operator=(FaceCameraFrame&&) = delete;

    uint64_t sequence = 0;
    uint64_t receivedTickMs = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t gpuSubresource = 0;
    IMFSample* gpuSample = nullptr;
    std::vector<uint8_t> bgra;
};

// Media Foundation camera ingest. ReadSample blocks only this private worker;
// the encoder consumes the newest immutable frame and never waits for it.
class FaceCameraCapture {
public:
    explicit FaceCameraCapture(ce::face_camera::Config config);
    ~FaceCameraCapture();

    bool Start(ID3D11Device* device);
    void Stop();
    std::shared_ptr<const FaceCameraFrame> LatestFrame() const;
    bool MayProduceFrames() const;

private:
    enum class State : uint8_t {
        kNotStarted = 0,
        kStarting,
        kRunning,
        kFailed,
        kStopped,
    };

    void CaptureThreadMain();
    bool PublishSample(IMFSample* sample, uint32_t width, uint32_t height, LONG stride);
    void PublishActiveReader(IMFSourceReader* reader);
    void ClearActiveReader(IMFSourceReader* reader);

    ce::face_camera::Config config_;
    ID3D11Device* device_ = nullptr;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<State> state_{State::kNotStarted};
    std::thread captureThread_;

    mutable std::shared_ptr<const FaceCameraFrame> latestFrame_;
    std::atomic<uint64_t> nextSequence_{1};
    std::mutex activeSourceMutex_;
    IMFSourceReader* activeReader_ = nullptr;
};
