#pragma once

#include <dxgiformat.h>
#include <cstdint>

#include "video_format_policy.h"

// Encode-time RGB -> NV12/P010 conversion with CE's own shaders
// (VideoEncoder::ConvertRgbWithShaders). Every capture source the encoder can
// take has a shader route; the D3D11 VideoProcessor is left only as the SDR
// fallback after a shader failure (llm-wiki/gpu-api-plan.md). The steps run in
// this order, each only when needed:
//   1. a sampleable copy: the source has no SRV bind flag, or is multisampled
//      (resolved);
//   2. the RGB10 normalization (PS_Main): FP16 linear SDR to sRGB (clipping
//      like the VideoProcessor's linear input), scRGB to PQ for HDR output,
//      HDR to SDR tone mapping; the retained-YUV route normalizes the same way;
//   3. the shader scaler (frame_scaler.h) when the output size differs;
//   4. the YUV plane draws (rgb_yuv_draw.h).

namespace ce::video_format {

struct ShaderConversionRouteInputs {
    DXGI_FORMAT sourceFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    bool sourceHasShaderResourceBind = false;
    uint32_t sourceSampleCount = 1;
    DXGI_FORMAT outputFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    bool sourceIsHdr = false;
    bool outputIsHdr = false;
};

struct ShaderConversionRoute {
    bool convertible = false;
    const char* refusal = "";  // why not, when !convertible
    bool sampleableCopy = false;
    bool resolveMultisample = false;
    RgbColorTransform normalize = RgbColorTransform::kNone;
    bool scale = false;
    uint32_t scaleSourceBits = 8;  // precision of the image the scaler reads
    uint32_t outputBits = 8;
};

inline ShaderConversionRoute DecideShaderConversionRoute(const ShaderConversionRouteInputs& in) {
    ShaderConversionRoute route;
    const bool p010 = in.outputFormat == DXGI_FORMAT_P010;
    if (!p010 && in.outputFormat != DXGI_FORMAT_NV12) {
        route.refusal = "output format";
    } else if (in.outputIsHdr && (!p010 || !in.sourceIsHdr)) {
        route.refusal = "HDR output needs an HDR source and P010";
    } else if (GetRgbShaderResourceViewFormat(in.sourceFormat) == DXGI_FORMAT_UNKNOWN) {
        route.refusal = "source format";
    } else if (in.sourceIsHdr && !IsFp16RgbInputFormat(in.sourceFormat) && !IsHdr10RgbInputFormat(in.sourceFormat)) {
        route.refusal = "HDR source format";
    } else if (in.sourceWidth == 0 || in.sourceHeight == 0 || in.outputWidth < 2 || in.outputHeight < 2 ||
               (in.outputWidth & 1u) != 0 || (in.outputHeight & 1u) != 0) {
        route.refusal = "frame size";
    } else {
        route.convertible = true;
        route.resolveMultisample = in.sourceSampleCount > 1;
        route.sampleableCopy = route.resolveMultisample || !in.sourceHasShaderResourceBind;
        route.normalize = GetRgbColorTransform(in.sourceFormat, in.sourceIsHdr, in.outputIsHdr);
        route.scale = in.sourceWidth != in.outputWidth || in.sourceHeight != in.outputHeight;
        // The normalization renders RGB10.
        route.scaleSourceBits =
            (route.normalize != RgbColorTransform::kNone || IsHighPrecisionRgbInputFormat(in.sourceFormat)) ? 10u
                                                                                                           : 8u;
        route.outputBits = p010 ? 10u : 8u;
    }
    return route;
}

}  // namespace ce::video_format
