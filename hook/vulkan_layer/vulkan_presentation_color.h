#pragma once

#include <vulkan/vulkan.h>

#include "../common/presentation_color.h"

namespace ce::presentation_color {

inline Encoding ResolveVulkan(VkFormat format, VkColorSpaceKHR colorSpace) {
    switch (colorSpace) {
        case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
            switch (format) {
                case VK_FORMAT_B8G8R8A8_UNORM:
                case VK_FORMAT_B8G8R8A8_SRGB:
                case VK_FORMAT_R8G8B8A8_UNORM:
                case VK_FORMAT_R8G8B8A8_SRGB:
                case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
                case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
                    return Encoding::Sdr709;
                default:
                    return Encoding::Unsupported;
            }

#ifdef VK_EXT_swapchain_colorspace
        case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
            return format == VK_FORMAT_R16G16B16A16_SFLOAT ? Encoding::LinearScRgb : Encoding::Unsupported;
        case VK_COLOR_SPACE_HDR10_ST2084_EXT:
            return (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
                    format == VK_FORMAT_A2R10G10B10_UNORM_PACK32)
                       ? Encoding::Hdr10Pq
                       : Encoding::Unsupported;
#endif
        default:
            return Encoding::Unsupported;
    }
}

}  // namespace ce::presentation_color
