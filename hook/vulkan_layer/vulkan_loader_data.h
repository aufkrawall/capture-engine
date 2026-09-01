/**
 * Vulkan loader dispatch initialization for layer-created device objects.
 */

#pragma once

#include <cstring>
#include <vulkan/vk_layer.h>

#include "vulkan_instance_registry.h"

namespace ce::vulkan_loader_data {

enum class InitializationOutcome {
    kLoaderCallback,
    kParentDispatchFallback,
    kNullObject,
    kMissingParentDispatch,
    kCallbackRejected,
    kCallbackDidNotInitialize,
};

struct InitializationResult {
    InitializationOutcome outcome = InitializationOutcome::kNullObject;
    VkResult callbackResult = VK_SUCCESS;

    bool initialized() const {
        return outcome == InitializationOutcome::kLoaderCallback ||
               outcome == InitializationOutcome::kParentDispatchFallback;
    }
};

inline const char* ToString(InitializationOutcome outcome) {
    switch (outcome) {
        case InitializationOutcome::kLoaderCallback:
            return "loader-callback";
        case InitializationOutcome::kParentDispatchFallback:
            return "parent-dispatch-fallback";
        case InitializationOutcome::kNullObject:
            return "null-object";
        case InitializationOutcome::kMissingParentDispatch:
            return "missing-parent-dispatch";
        case InitializationOutcome::kCallbackRejected:
            return "callback-rejected";
        case InitializationOutcome::kCallbackDidNotInitialize:
            return "callback-did-not-initialize";
        default:
            return "unknown";
    }
}

// A layer that obtains a dispatchable child object by calling down the chain
// itself bypasses the loader trampoline that would normally stamp the object.
// New loaders provide the callback for that job. The pointer copy is the
// loader-documented compatibility path for older loaders without the callback.
inline InitializationResult InitializeDeviceObject(PFN_vkSetDeviceLoaderData setDeviceLoaderData,
                                                   VkDevice parentDevice, void* object) {
    if (!object)
        return {InitializationOutcome::kNullObject, VK_SUCCESS};

    const void* parentDispatch = ce::vulkan_instance_registry::DispatchKey(parentDevice);
    if (!parentDispatch)
        return {InitializationOutcome::kMissingParentDispatch, VK_SUCCESS};

    if (setDeviceLoaderData) {
        const VkResult callbackResult = setDeviceLoaderData(parentDevice, object);
        if (callbackResult != VK_SUCCESS)
            return {InitializationOutcome::kCallbackRejected, callbackResult};
        if (ce::vulkan_instance_registry::DispatchKey(object) != parentDispatch)
            return {InitializationOutcome::kCallbackDidNotInitialize, callbackResult};
        return {InitializationOutcome::kLoaderCallback, callbackResult};
    }

    std::memcpy(object, static_cast<const void*>(&parentDispatch), sizeof(parentDispatch));
    if (ce::vulkan_instance_registry::DispatchKey(object) != parentDispatch)
        return {InitializationOutcome::kCallbackDidNotInitialize, VK_SUCCESS};
    return {InitializationOutcome::kParentDispatchFallback, VK_SUCCESS};
}

}  // namespace ce::vulkan_loader_data
