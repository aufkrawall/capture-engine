#pragma once

#include <dxgi1_6.h>

#include "presentation_color.h"

namespace ce::presentation_color {

inline Encoding ResolveDXGI(DXGI_FORMAT format, bool hasColorSpace, DXGI_COLOR_SPACE_TYPE colorSpace) {
    // DXGI defines RGB_FULL_G22_NONE_P709 as the default swapchain color
    // space. Missing tracking therefore fails closed to SDR instead of treating
    // an HDR-capable storage format as proof of HDR content.
    if (!hasColorSpace) {
        colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    }

    switch (colorSpace) {
        case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709:
            switch (format) {
                case DXGI_FORMAT_B8G8R8A8_UNORM:
                case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                case DXGI_FORMAT_B8G8R8X8_UNORM:
                case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
                case DXGI_FORMAT_R8G8B8A8_UNORM:
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                case DXGI_FORMAT_R10G10B10A2_UNORM:
                    return Encoding::Sdr709;
                default:
                    return Encoding::Unsupported;
            }

        case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
            return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? Encoding::LinearScRgb : Encoding::Unsupported;

        case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
            return format == DXGI_FORMAT_R10G10B10A2_UNORM ? Encoding::Hdr10Pq : Encoding::Unsupported;

        default:
            return Encoding::Unsupported;
    }
}

// A wrapped swapchain owns publication for calls that it forwards to the real
// IDXGISwapChain3. If the real method is also inline-hooked, the detour still
// forwards through its trampoline but must not publish the same transition a
// second time. Unwrapped calls are owned by the detour.
inline bool ShouldRecordDetouredColorSpaceChange(unsigned wrapperForwardDepth) {
    return wrapperForwardDepth == 0;
}

}  // namespace ce::presentation_color
