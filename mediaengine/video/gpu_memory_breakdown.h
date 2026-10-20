#pragma once

#include <d3d11.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "common/logging/log_meter.h"

// Per-component video memory of a recording ([GpuMemory] log line). The
// process total (DXGI QueryVideoMemoryInfo, local segment, and Task Manager's
// per-process "Dedicated GPU memory") counts a shared texture once per D3D11
// device that uses it, although it is one allocation: on an RTX 5070, 13
// shared 4K P010 textures opened on a second device added 311 MB to both
// per-process counters and 0 MB to the adapter's dedicated usage. The
// split-device reservoir (DXGI duplication) and the encoder device bridge's
// frame pool are such textures (`countedTwice`). The total also includes
// memory CE cannot see: the hardware encoder's internal buffers, driver
// allocations of each device and desktop-duplication surfaces. CE's own
// textures are estimated from their descriptions (no pitch or allocation
// alignment), so `unaccounted` is the process total minus the double count
// minus everything CE can name.

namespace ce::gpu_memory {

// Approximate bytes of one texture: planar NV12/P010 count luma plus the
// half-size chroma plane; no mips, pitch or alignment padding.
inline uint64_t EstimateTextureBytes(uint32_t width, uint32_t height, DXGI_FORMAT format, uint32_t arraySize = 1) {
    const uint64_t pixels = static_cast<uint64_t>(width) * height * (arraySize == 0 ? 1u : arraySize);
    switch (format) {
        case DXGI_FORMAT_NV12:
            return pixels * 3 / 2;
        case DXGI_FORMAT_P010:
            return pixels * 3;
        case DXGI_FORMAT_R8_UNORM:
            return pixels;
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R8G8_UNORM:
            return pixels * 2;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return pixels * 8;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return pixels * 16;
        default:
            return pixels * 4;  // BGRA8/RGBA8/R10G10B10A2/R16G16
    }
}

inline uint64_t TextureBytes(ID3D11Texture2D* texture) {
    if (!texture)
        return 0;
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    return EstimateTextureBytes(desc.Width, desc.Height, desc.Format, desc.ArraySize);
}

struct Breakdown {
    // Process totals on the encoder's adapter (0 when the query failed).
    uint64_t processLocal = 0;
    uint64_t processNonLocal = 0;
    uint64_t budget = 0;
    // WGC/DXGI capture (reported by captureengine for its current pool).
    uint64_t captureReservoir = 0;
    uint32_t captureReservoirSlots = 0;
    uint64_t captureOther = 0;  // conversion staging, WGC frame-pool buffers
    bool captureSharedAcrossDevices = false;  // split capture device: two devices use the pool
    // Encoder input frames (FFmpeg hw frame pool textures created so far).
    uint64_t encoderFrames = 0;
    uint32_t encoderFrameCount = 0;
    uint64_t repeat = 0;      // repeat caches
    uint64_t conversion = 0;  // RGB staging, intermediates, VideoProcessor input copies
    uint64_t overlays = 0;    // cursor/camera restore and composite copies, YUV layers, camera history
    // Shared textures the process counter includes twice (see above).
    uint64_t countedTwice = 0;
    // Process usage added by opening the codec (encoder session buffers that
    // were resident right away); informational, part of `unaccounted`.
    int64_t codecOpenDelta = 0;
};

inline uint64_t AccountedBytes(const Breakdown& b) {
    return b.captureReservoir + b.captureOther + b.encoderFrames + b.repeat + b.conversion + b.overlays;
}

// The process counter without the double count of shared textures.
inline int64_t PhysicalEstimateBytes(const Breakdown& b) {
    return static_cast<int64_t>(b.processLocal) - static_cast<int64_t>(b.countedTwice);
}

inline int64_t UnaccountedBytes(const Breakdown& b) {
    return PhysicalEstimateBytes(b) - static_cast<int64_t>(AccountedBytes(b));
}

inline std::string FormatBreakdown(const Breakdown& b, const char* stage) {
    constexpr double kMb = 1024.0 * 1024.0;
    auto mb = [](uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); };
    char line[720];
    std::snprintf(line, sizeof(line),
                  "[GpuMemory] stage=%s processLocal=%.0fMB countedTwice=%.0fMB physical~%.0fMB nonLocal=%.0fMB "
                  "budget=%.0fMB | captureReservoir=%.0fMB (%u slots%s) captureOther=%.0fMB | encoderFrames=%.0fMB "
                  "(%u frames) repeat=%.0fMB conversion=%.0fMB overlays=%.0fMB | accounted=%.0fMB unaccounted=%.0fMB "
                  "(encoder session, drivers, devices) codecOpenDelta=%+.0fMB",
                  stage ? stage : "periodic", mb(b.processLocal), mb(b.countedTwice),
                  static_cast<double>(PhysicalEstimateBytes(b)) / kMb, mb(b.processNonLocal), mb(b.budget),
                  mb(b.captureReservoir), b.captureReservoirSlots, b.captureSharedAcrossDevices ? ", 2 devices" : "",
                  mb(b.captureOther), mb(b.encoderFrames), b.encoderFrameCount, mb(b.repeat), mb(b.conversion),
                  mb(b.overlays), mb(AccountedBytes(b)), static_cast<double>(UnaccountedBytes(b)) / kMb,
                  static_cast<double>(b.codecOpenDelta) / kMb);
    return line;
}

// Process video memory on the device's adapter (local = dedicated VRAM).
inline bool QueryProcessVideoMemory(ID3D11Device* device, uint64_t* local, uint64_t* nonLocal, uint64_t* budget) {
    *local = *nonLocal = *budget = 0;
    if (!device)
        return false;
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIAdapter3* adapter3 = nullptr;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (SUCCEEDED(hr))
        hr = dxgiDevice->GetAdapter(&adapter);
    if (SUCCEEDED(hr))
        hr = adapter->QueryInterface(IID_PPV_ARGS(&adapter3));
    DXGI_QUERY_VIDEO_MEMORY_INFO localInfo = {};
    DXGI_QUERY_VIDEO_MEMORY_INFO nonLocalInfo = {};
    if (SUCCEEDED(hr))
        hr = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &localInfo);
    if (SUCCEEDED(hr))
        hr = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocalInfo);
    if (adapter3)
        adapter3->Release();
    if (adapter)
        adapter->Release();
    if (dxgiDevice)
        dxgiDevice->Release();
    if (FAILED(hr))
        return false;
    *local = localInfo.CurrentUsage;
    *nonLocal = nonLocalInfo.CurrentUsage;
    *budget = localInfo.Budget;
    return true;
}

// Encoder-side bookkeeping for the [GpuMemory] line (VideoEncoder::LogGpuMemory).
struct EncoderGpuMemoryState {
    static constexpr size_t kMaxTrackedPoolTextures = 256;
    // Hardware frame pool textures seen without the encoder device bridge
    // (the bridge counts its own aliases); identity only, never dereferenced.
    std::vector<const void*> poolTextures;
    int64_t codecOpenDelta = 0;
    uint64_t peakLocal = 0;
    // Logs while the totals move by 64 MB, at least once a minute otherwise.
    ce::log_meter::ChangeGate gate{60000};

    void TrackPoolTexture(const void* texture) {
        if (texture && poolTextures.size() < kMaxTrackedPoolTextures &&
            std::find(poolTextures.begin(), poolTextures.end(), texture) == poolTextures.end())
            poolTextures.push_back(texture);
    }
    void ResetPool() { poolTextures.clear(); }
};

inline uint64_t BreakdownLogKey(const Breakdown& b) {
    return ce::log_meter::FieldKey(b.processLocal >> 26, AccountedBytes(b) >> 26);
}

// The WGC/DXGI capture pool as last reported by captureengine
// (MediaEngine_ReportCaptureGpuMemory). Owned by a MediaEngineInstance and written only by that report.
struct CaptureGpuGauges {
    std::atomic<uint64_t> reservoirBytes{0};
    std::atomic<uint32_t> reservoirSlots{0};
    std::atomic<uint64_t> otherBytes{0};
    std::atomic<bool> sharedAcrossDevices{false};
};

// Copies the gauges of the pipeline that owns the encoder into `breakdown` (mediaengine_retained_frame.cpp).
void LoadCaptureReport(const CaptureGpuGauges& gauges, Breakdown* breakdown);

}  // namespace ce::gpu_memory
