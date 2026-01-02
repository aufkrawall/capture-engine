#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <memory>
#include <mutex>
#include "wgc_capture.h" // Reuse WGCCapturedFrame struct

class DxgiCapture {
public:
    DxgiCapture();
    ~DxgiCapture();

    bool Init(ID3D11Device* device, int monitorIndex = 0, std::mutex* d3dMutex = nullptr);
    bool StartCapture();
    void StopCapture();
    bool GetNextFrame(WGCCapturedFrame& frame, bool skipCopy = false);
    void ResetStats();

    // Stats
    uint32_t GetDroppedFrames() const { return droppedFrames_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    std::mutex* d3dMutex_ = nullptr;
    bool initialized_ = false;
    bool capturing_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    
    // Config
    bool captureCursor_ = true;
    uint32_t droppedFrames_ = 0;
};
