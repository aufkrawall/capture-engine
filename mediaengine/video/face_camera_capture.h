#pragma once

#include <d3d11.h>
#include <mfidl.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "common/config/face_camera_config.h"
#include "face_camera_sample_extractor.h"
#include "face_camera_history.h"

struct IMFSourceReader;

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
    void PublishActiveReader(IMFSourceReader* reader);
    void ClearActiveReader(IMFSourceReader* reader);

    ce::face_camera::Config config_;
    ID3D11Device* device_ = nullptr;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<State> state_{State::kNotStarted};
    std::thread captureThread_;

    mutable std::shared_ptr<const FaceCameraFrame> latestFrame_;
    ce::face_camera::SampleExtractor sampleExtractor_;
    std::mutex activeSourceMutex_;
    IMFSourceReader* activeReader_ = nullptr;
};
