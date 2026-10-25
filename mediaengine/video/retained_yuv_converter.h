#pragma once

#include <d3d11.h>
#include <cstdint>

#include "rgb_yuv_draw.h"
#include "video_format_policy.h"

// Converts a captured WGC/DXGI frame into a retained NV12/P010 or packed P010
// reservoir slot at ingress (llm-wiki/wgc-reservoir-vram-design.md). It owns
// its own copy of the encoder's conversion shaders and an RGB10 intermediate,
// and issues the same rgb_yuv_draw passes the encoder issues at encode time:
//   SDR FP16 -> PS_Main linear-to-sRGB RGB10 -> PS_YuvY/UV
//   SDR R10/BGRA8 -> PS_YuvY/UV
//   HDR FP16 -> PS_Main scRGB-to-PQ RGB10 -> PS_P010Y/UV
// A packed P010 slot (common/capture/packed_p010_layout.h) gets the same chroma
// pass into a half-size scratch and the luma pass fused with packing.
// The caller serializes immediate-context use (MediaEngine D3D11 lock).
class RetainedYuvConverter {
public:
    struct Request {
        ID3D11ShaderResourceView* source = nullptr;  // RGB view in the source's own format
        UINT width = 0;
        UINT height = 0;
        ID3D11RenderTargetView* lumaTarget = nullptr;  // plane RTVs of the NV12/P010 slot
        ID3D11RenderTargetView* chromaTarget = nullptr;
        ID3D11RenderTargetView* packedTarget = nullptr;  // R16_UINT RTV of a packed P010 slot instead
        bool eightBit = false;  // NV12
        ce::video_format::RgbColorTransform normalize = ce::video_format::RgbColorTransform::kNone;
        bool hdrOutput = false;
        bool bt2020 = false;  // SDR matrix
        bool fullRange = false;
    };

    RetainedYuvConverter() = default;
    ~RetainedYuvConverter();
    RetainedYuvConverter(const RetainedYuvConverter&) = delete;
    RetainedYuvConverter& operator=(const RetainedYuvConverter&) = delete;

    bool EnsureKernels(ID3D11Device* device);
    const ce::video_color::RgbYuvDrawKernels& Kernels() const { return kernels_; }
    bool Convert(ID3D11DeviceContext* context, const Request& request);
    void Release();
    uint64_t ConversionCount() const { return conversions_; }
    uint64_t PackedConversionCount() const { return packedConversions_; }

private:
    bool EnsureIntermediate(UINT width, UINT height);
    void ReleaseIntermediate();
    bool EnsureChromaScratch(UINT width, UINT height);
    void ReleaseChromaScratch();

    ID3D11Device* device_ = nullptr;
    ce::video_color::RgbYuvDrawKernels kernels_;
    ID3D11Texture2D* intermediate_ = nullptr;
    ID3D11RenderTargetView* intermediateRtv_ = nullptr;
    ID3D11ShaderResourceView* intermediateSrv_ = nullptr;
    UINT intermediateWidth_ = 0;
    UINT intermediateHeight_ = 0;
    // Half-size R16G16_UNORM chroma of the packed P010 route (one frame, not per slot).
    ID3D11Texture2D* chromaScratch_ = nullptr;
    ID3D11RenderTargetView* chromaScratchRtv_ = nullptr;
    ID3D11ShaderResourceView* chromaScratchSrv_ = nullptr;
    UINT chromaScratchWidth_ = 0;
    UINT chromaScratchHeight_ = 0;
    uint64_t conversions_ = 0;
    uint64_t packedConversions_ = 0;
};
