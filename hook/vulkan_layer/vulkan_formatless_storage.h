#pragma once

#include <vulkan/vulkan.h>

#include "vulkan_layer.h"

// Whether a shader may read or write a presentable image of `format` through a
// storage image declared without a format qualifier. The swapchain format is
// the application's choice, so CE's compute routes (the compute-present overlay
// composite and the compute sharpen pass) cannot name it in the shader and
// depend on these per-format capabilities instead.

namespace ce::vulkan_formatless_storage {

// The WSI-capable members of Vulkan's required "formats without shader storage
// format" list. Other formats can still opt in through VkFormatProperties3.
constexpr bool HasCoreGuarantee(VkFormat format) {
    return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
           format == VK_FORMAT_R16G16B16A16_SFLOAT;
}

struct Support {
    bool read = false;
    bool write = false;
};

inline Support Query(InstanceDispatch* instDisp, VkPhysicalDevice physicalDevice, VkFormat format,
                     bool formatFeatureFlags2Available) {
    const bool coreGuarantee = HasCoreGuarantee(format);
    VkFormatFeatureFlags2 optimalFeatures = 0;
    if (formatFeatureFlags2Available && instDisp && instDisp->fp_vkGetPhysicalDeviceFormatProperties2) {
        VkFormatProperties3 properties3 = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 properties2 = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
        properties2.pNext = &properties3;
        instDisp->fp_vkGetPhysicalDeviceFormatProperties2(physicalDevice, format, &properties2);
        optimalFeatures = properties3.optimalTilingFeatures;
    }
    Support support;
    support.read = coreGuarantee || (optimalFeatures & VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT) != 0;
    support.write = coreGuarantee || (optimalFeatures & VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT) != 0;
    return support;
}

}  // namespace ce::vulkan_formatless_storage
