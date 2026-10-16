#pragma once

#include <dxgiformat.h>
#include <cstdint>

#include "video_format_policy.h"

// Retained WGC/DXGI smoothness reservoir route (llm-wiki/wgc-reservoir-vram-design.md).
//
// The reservoir holds the fixed smoothness delay as raw frames. Storing them
// already converted to the encoder's NV12/P010 input costs 1.5/3 bytes per
// pixel instead of 4 (R10/BGRA8) or 8 (HDR FP16), with the same frame count.
// The ingress conversion runs the encoder's own shaders and parameters, so a
// frame without overlays reaches the encoder with the codes today's
// encode-time conversion would produce. Cursor and face camera are then
// composited per output slot in YUV (YuvOverlayPatcher).
//
// Every case whose encode-time conversion depends on live state or a
// different pipeline keeps the RGB reservoir: output scaling (VideoProcessor
// scaler), HDR-to-SDR tone mapping (live SDR white), split capture devices
// (planar keyed-mutex sharing), odd sizes (4:2:0) and HDR sources that are not
// FP16 scRGB (cursor blending domain).

namespace ce::video_format {

struct RetainedFrameRouteInputs {
    DXGI_FORMAT sourceFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    bool sourceIsHdr = false;
    bool hdrOutput = false;
    bool use10BitOutput = false;
    bool formatResolved = false;     // the encoder could resolve a 4:2:0 D3D11 input format
    bool encoderUsesYuvInput = false;  // NV12/P010 frames (not the direct RGB path)
    bool scaling = false;
    bool sameDevice = false;
    bool enabled = false;
    bool geometryMismatch = false;   // the encoder already locked a different frame size
};

struct RetainedFrameRouteDecision {
    bool yuv = false;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    RgbColorTransform normalizeTransform = RgbColorTransform::kNone;
    // Precision of the RGB image the encode-time conversion used to read:
    // overlays blend at this precision before the YUV conversion.
    uint32_t rgbLevels = 0;
    const char* reason = "";
};

inline uint32_t RetainedSourceRgbLevels(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return 255u;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            // FP16 is normalized to an RGB10 intermediate before conversion.
            return 1023u;
        default:
            return 0u;
    }
}

inline RetainedFrameRouteDecision DecideRetainedFrameRoute(const RetainedFrameRouteInputs& in) {
    RetainedFrameRouteDecision out;
    if (!in.enabled) {
        out.reason = "disabled";
    } else if (!in.sameDevice) {
        out.reason = "split capture device";
    } else if (in.scaling) {
        out.reason = "output scaling";
    } else if (in.width == 0 || in.height == 0 || (in.width & 1u) != 0 || (in.height & 1u) != 0) {
        out.reason = "odd frame size";
    } else if (in.geometryMismatch) {
        out.reason = "encoder geometry locked to another size";
    } else if (!in.formatResolved) {
        out.reason = "encoder format unresolved";
    } else if (!in.encoderUsesYuvInput) {
        out.reason = "direct RGB encoder input";
    } else if (RetainedSourceRgbLevels(in.sourceFormat) == 0u) {
        out.reason = "source format";
    } else if (in.sourceIsHdr && !in.hdrOutput) {
        out.reason = "HDR-to-SDR tone map";
    } else if (in.hdrOutput && (!IsFp16RgbInputFormat(in.sourceFormat) || !in.use10BitOutput)) {
        out.reason = "HDR source is not FP16 scRGB";
    } else {
        out.yuv = true;
        out.format = in.use10BitOutput ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
        out.normalizeTransform = GetRgbColorTransform(in.sourceFormat, in.sourceIsHdr, in.hdrOutput);
        out.rgbLevels = RetainedSourceRgbLevels(in.sourceFormat);
        out.reason = in.hdrOutput ? "yuv (scRGB to PQ P010)" : "yuv";
    }
    return out;
}

inline bool IsRetainedYuvFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_NV12 || format == DXGI_FORMAT_P010;
}

}  // namespace ce::video_format
