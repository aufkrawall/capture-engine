#pragma once

#include <dxgiformat.h>
#include <cstdint>

namespace ce::video_format {

enum class RgbColorTransform : uint32_t {
    kNone = 0,
    kLinearToSrgb = 1,
    kScRgbToHdr10 = 2,
};

inline bool IsTypelessDxgiFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return true;
        default:
            return false;
    }
}

inline bool IsFp16RgbInputFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R16G16B16A16_TYPELESS;
}

inline bool IsHighPrecisionRgbInputFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R10G10B10A2_TYPELESS ||
           IsFp16RgbInputFormat(format);
}

inline DXGI_FORMAT GetRgbShaderResourceViewFormat(DXGI_FORMAT textureFormat) {
    switch (textureFormat) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

inline bool ShouldApplySdrLinearToSrgbBeforeRgb10(DXGI_FORMAT textureFormat, bool isHdr) {
    return !isHdr && IsFp16RgbInputFormat(textureFormat);
}

inline bool IsOutputBitDepthCompatibleWithHdr(bool isHdr, bool use10Bit) {
    return !isHdr || use10Bit;
}

inline RgbColorTransform GetRgbColorTransform(DXGI_FORMAT textureFormat, bool isHdr) {
    if (!IsFp16RgbInputFormat(textureFormat)) {
        return RgbColorTransform::kNone;
    }
    return isHdr ? RgbColorTransform::kScRgbToHdr10 : RgbColorTransform::kLinearToSrgb;
}

inline const char* DescribeRgbColorTransform(RgbColorTransform transform) {
    switch (transform) {
        case RgbColorTransform::kLinearToSrgb:
            return "scRGB-linear-to-sRGB";
        case RgbColorTransform::kScRgbToHdr10:
            return "scRGB-linear-P709-to-PQ-P2020";
        case RgbColorTransform::kNone:
        default:
            return "passthrough";
    }
}

}  // namespace ce::video_format
