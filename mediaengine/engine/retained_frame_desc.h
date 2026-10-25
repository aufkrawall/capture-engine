#pragma once

#include <cstdint>

// Retained WGC/DXGI smoothness reservoir in the encoder's YUV format
// (llm-wiki/wgc-reservoir-vram-design.md). captureengine asks the media engine
// once per pool build which format to retain, then converts every admitted
// frame through MediaEngine_ConvertToRetainedFrame with the returned route.
// `size` is sizeof(...) of the caller's struct (ABI guard).

struct MediaEngineRetainedFrameQuery {
    uint32_t size = sizeof(MediaEngineRetainedFrameQuery);
    uint32_t sourceFormat = 0;  // DXGI_FORMAT of the captured frame
    uint32_t width = 0;
    uint32_t height = 0;
    bool sourceIsHdr = false;
    bool sameDevice = false;  // the pool lives on the encoder's D3D11 device
    // Split capture device: both devices can share an NV12 / P010 / packed P010 keyed-mutex slot.
    bool splitShareNv12 = false;
    bool splitShareP010 = false;
    bool splitSharePackedP010 = false;
    // The capture side may retain 10-bit frames as packed P010 (wgc_retained_packed_p010).
    bool allowPackedP010 = false;
};

struct MediaEngineRetainedFrameRoute {
    uint32_t size = sizeof(MediaEngineRetainedFrameRoute);
    // DXGI_FORMAT_NV12 / DXGI_FORMAT_P010 / ce::packed_p010::kSlotFormat (R16_UINT,
    // common/capture/packed_p010_layout.h); 0 = keep the RGB reservoir.
    uint32_t yuvFormat = 0;
    uint32_t normalizeTransform = 0;  // ce::video_format::RgbColorTransform before the YUV pass
    uint32_t rgbLevels = 0;           // 255 / 1023: RGB precision overlays blend at
    bool hdrOutput = false;           // PQ BT.2020 P010
    bool bt2020 = false;              // SDR matrix
    bool fullRange = false;
    char reason[48] = {};
};

struct MediaEngineRetainedFrameConversion {
    uint32_t size = sizeof(MediaEngineRetainedFrameConversion);
    void* sourceView = nullptr;  // ID3D11ShaderResourceView* of the captured RGB frame
    uint32_t width = 0;
    uint32_t height = 0;
    void* lumaTarget = nullptr;    // ID3D11RenderTargetView* of the slot's luma plane
    void* chromaTarget = nullptr;  // ID3D11RenderTargetView* of the slot's chroma plane
    void* packedTarget = nullptr;  // packed P010 slot: its R16_UINT ID3D11RenderTargetView*, planes unused
    MediaEngineRetainedFrameRoute route;
};
