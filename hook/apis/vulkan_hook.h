#pragma once
#include "graphics_hook.h"
#define VK_USE_PLATFORM_WIN32_KHR
#define USE_VKDISPATCH  // Use our custom dispatch instead of Volk
#include <mutex>
#include <vector>
#include "../common/capture_base.h"
#include "../wrappers/vk_dispatch.h"

class VulkanHook : public GraphicsHook {
public:
    void Init() override;
    void Shutdown() override;
    void OnHostDisconnect() override;  // Called when captureengine disconnects
};

// Detour functions - called when game calls vkGet*ProcAddr
// These wrappers are exported for IAT patching
extern "C" PFN_vkVoidFunction VKAPI_CALL VK_DetourGetInstanceProcAddr(VkInstance instance, const char* pName);
extern "C" PFN_vkVoidFunction VKAPI_CALL VK_DetourGetDeviceProcAddr(VkDevice device, const char* pName);

// Original function pointers (set by IAT patching)
// These are defined in vulkan_hook.cpp using the Vulkan header types
extern PFN_vkGetInstanceProcAddr o_vkGetInstanceProcAddr;
extern PFN_vkGetDeviceProcAddr o_vkGetDeviceProcAddr;
