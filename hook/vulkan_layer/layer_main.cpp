/**
 * VK_LAYER_CE_overlay - Entry Points
 * 
 * Implements vkGetInstanceProcAddr and vkGetDeviceProcAddr.
 * Handles layer negotiation and delegation to Capture_ hooks.
 */

#include "layer_main.h"
#include <cstring>

// Global states
CELayerState g_LayerState;

// Forward declarations
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL CELayer_vkGetInstanceProcAddr(VkInstance instance, const char* pName);
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL CELayer_vkGetDeviceProcAddr(VkDevice device, const char* pName);

// Logging implementation
void LayerLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Output to debug console
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");

    // Also to stderr
    fprintf(stderr, "[VulkanLayer] %s\n", buf);
    fflush(stderr);

    // Also to IPC if connected
    if (LayerIPC_IsConnected()) {
        LayerIPC_Log("%s", buf);
    }
}

// ============================================================================
// Layer Negotiation
// ============================================================================

extern "C" VKAPI_ATTR VkResult VKAPI_CALL 
CELayer_vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (pVersionStruct->loaderLayerInterfaceVersion < 2) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    // Whitelist check: If not whitelisted, return initialization failure 
    // to let the loader skip our layer entirely for this process.
    if (!LayerIPC_Init()) {
        // LayerIPC_Init returns true if connected (and sets g_LayerState.whitelisted)
        // If it fails to connect, we might want to check the standalone config.
        // But for transparency and preventing crashes in browsers, returning failure here is safest.
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (!g_LayerState.whitelisted) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    pVersionStruct->loaderLayerInterfaceVersion = 2;
    pVersionStruct->pfnGetInstanceProcAddr = CELayer_vkGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = CELayer_vkGetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;

    // One-time initialization logic
    if (!g_LayerState.initialized) {
        g_LayerState.initialized = true;
        LayerLog("Vulkan Layer Negotiated for whitelisted process. Interface Version: %d", pVersionStruct->loaderLayerInterfaceVersion);
    }

    LayerLog("Vulkan Layer: Negotiate Version Success");
    return VK_SUCCESS;
}

// ============================================================================
// ProcAddr Implementations
// ============================================================================

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL 
CELayer_vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (!pName) return nullptr;
    // LayerLog("CELayer_vkGetInstanceProcAddr(inst=%p, name=%s)", instance, pName);

    // Layer-level functions (no instance needed)
    if (strcmp(pName, "vkNegotiateLoaderLayerInterfaceVersion") == 0) {
        return (PFN_vkVoidFunction)CELayer_vkNegotiateLoaderLayerInterfaceVersion;
    }
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0) {
        return (PFN_vkVoidFunction)CELayer_vkGetInstanceProcAddr;
    }
    if (strcmp(pName, "vkCreateInstance") == 0) {
        LayerLog("CELayer_vkGetInstanceProcAddr: %s", pName);
        return (PFN_vkVoidFunction)Capture_vkCreateInstance;
    }
    if (strcmp(pName, "vkCreateDevice") == 0) {
        LayerLog("CELayer_vkGetInstanceProcAddr: %s", pName);
        return (PFN_vkVoidFunction)Capture_vkCreateDevice;
    }
    if (strcmp(pName, "vkEnumeratePhysicalDevices") == 0) {
        return (PFN_vkVoidFunction)Capture_vkEnumeratePhysicalDevices;
    }
    if (strcmp(pName, "vkEnumerateInstanceLayerProperties") == 0 ||
        strcmp(pName, "vkEnumerateInstanceExtensionProperties") == 0) {
        return nullptr; // Let loader handle these or implement if needed
    }

    if (!instance) return nullptr;

    // Instance-level functions
    if (strcmp(pName, "vkDestroyInstance") == 0) return (PFN_vkVoidFunction)Capture_vkDestroyInstance;
    if (strcmp(pName, "vkCreateDevice") == 0) return (PFN_vkVoidFunction)Capture_vkCreateDevice;
    if (strcmp(pName, "vkCreateWin32SurfaceKHR") == 0) return (PFN_vkVoidFunction)Capture_vkCreateWin32SurfaceKHR;

    // Device-level functions obtained through instance GIPA
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) return (PFN_vkVoidFunction)CELayer_vkGetDeviceProcAddr;
    if (strcmp(pName, "vkGetDeviceQueue") == 0) return (PFN_vkVoidFunction)Capture_vkGetDeviceQueue;
    if (strcmp(pName, "vkCreateSwapchainKHR") == 0) return (PFN_vkVoidFunction)Capture_vkCreateSwapchainKHR;
    if (strcmp(pName, "vkDestroySwapchainKHR") == 0) return (PFN_vkVoidFunction)Capture_vkDestroySwapchainKHR;
    if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0) return (PFN_vkVoidFunction)Capture_vkGetSwapchainImagesKHR;
    if (strcmp(pName, "vkAcquireNextImageKHR") == 0) return (PFN_vkVoidFunction)Capture_vkAcquireNextImageKHR;
    if (strcmp(pName, "vkQueuePresentKHR") == 0) return (PFN_vkVoidFunction)Capture_vkQueuePresentKHR;
    if (strcmp(pName, "vkCreateSampler") == 0) return (PFN_vkVoidFunction)Capture_vkCreateSampler;

    // Default: chain to next layer
    InstanceDispatch* disp = VulkanLayerState::Get().GetInstanceDispatch(instance);
    if (disp && disp->fp_vkGetInstanceProcAddr) {
        return disp->fp_vkGetInstanceProcAddr(instance, pName);
    }

    return nullptr;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL 
CELayer_vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    if (!pName || !device) return nullptr;
    // LayerLog("CELayer_vkGetDeviceProcAddr(dev=%p, name=%s)", device, pName);

    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) return (PFN_vkVoidFunction)CELayer_vkGetDeviceProcAddr;
    if (strcmp(pName, "vkGetDeviceQueue") == 0) return (PFN_vkVoidFunction)Capture_vkGetDeviceQueue;
    if (strcmp(pName, "vkDestroyDevice") == 0) return (PFN_vkVoidFunction)Capture_vkDestroyDevice;
    if (strcmp(pName, "vkCreateSwapchainKHR") == 0) return (PFN_vkVoidFunction)Capture_vkCreateSwapchainKHR;
    if (strcmp(pName, "vkDestroySwapchainKHR") == 0) return (PFN_vkVoidFunction)Capture_vkDestroySwapchainKHR;
    if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0) return (PFN_vkVoidFunction)Capture_vkGetSwapchainImagesKHR;
    if (strcmp(pName, "vkAcquireNextImageKHR") == 0) return (PFN_vkVoidFunction)Capture_vkAcquireNextImageKHR;
    if (strcmp(pName, "vkQueuePresentKHR") == 0) return (PFN_vkVoidFunction)Capture_vkQueuePresentKHR;
    if (strcmp(pName, "vkCreateSampler") == 0) return (PFN_vkVoidFunction)Capture_vkCreateSampler;

    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (disp && disp->fp_vkGetDeviceProcAddr) {
        return disp->fp_vkGetDeviceProcAddr(device, pName);
    }

    return nullptr;
}
