/**
 * VK_LAYER_CE_overlay - Vulkan to DXGI format mapping for the capture transport.
 */

#include <dxgiformat.h>
#include <vulkan/vulkan.h>

#include <cstdint>

/**
 * Convert Vulkan format to DXGI format for shared resource export
 * Includes support for common formats used in games
 */
uint32_t VkFormatToDXGI(uint32_t vkFormat) {
    // Use Vulkan format constants for clarity
    switch (vkFormat) {
        // 8-bit UNORM formats
        case VK_FORMAT_B8G8R8A8_UNORM:          // 44
            return DXGI_FORMAT_B8G8R8A8_UNORM;  // 87
        case VK_FORMAT_R8G8B8A8_UNORM:          // 37
            return DXGI_FORMAT_R8G8B8A8_UNORM;  // 28

        // SRGB formats - map to UNORM for D3D11 shared resources
        // (D3D11 shared resources don't support SRGB natively)
        case VK_FORMAT_B8G8R8A8_SRGB:  // 50
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB:  // 43
            return DXGI_FORMAT_R8G8B8A8_UNORM;

        // 16-bit floating point (HDR/scRGB)
        case VK_FORMAT_R16G16B16A16_SFLOAT:         // 97
            return DXGI_FORMAT_R16G16B16A16_FLOAT;  // 10

        // 10-bit formats (HDR10)
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:   // 64
            return DXGI_FORMAT_R10G10B10A2_UNORM;  // 30

        // BGRX formats (no alpha) - use BGRA for shared resources
        // Note: B8G8R8X8_UNORM not commonly used with Vulkan external memory
        // case VK_FORMAT_B8G8R8X8_UNORM:
        //     return DXGI_FORMAT_B8G8R8X8_UNORM;  // 88

        // RGB 565 (legacy)
        case VK_FORMAT_R5G6B5_UNORM_PACK16:   // 56
            return DXGI_FORMAT_B5G6R5_UNORM;  // 85 (swizzle)

        // Never relabel unknown byte layouts as BGRA; doing so produces valid
        // handles containing corrupt colors/row interpretation.
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

/**
 * Check if a Vulkan format is compatible with DXGI shared resources
 * Returns true if the format can be exported via external memory
 */
bool IsVkFormatCompatibleWithDXGI(VkFormat vkFormat) {
    switch (vkFormat) {
        // Standard 8-bit formats - fully compatible
        // NOLINTNEXTLINE(bugprone-branch-clone) - fallthrough group intentionally returns true for every 8-bit format
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_SRGB:
            return true;

        // HDR formats - compatible with D3D12
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return true;

        // Unsupported formats for export
        case VK_FORMAT_UNDEFINED:
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D32_SFLOAT:
            return false;

        // Unknown format - attempt but log warning
        default:
            return true;
    }
}
