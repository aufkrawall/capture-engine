/**
 * VK_LAYER_CE_overlay - Main Layer Implementation
 * 
 * Entry points and dispatch table management.
 * Includes whitelist checking from config.ini.
 */

#include "layer_main.h"
#include <cstdio>
#include <cstdarg>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <vector>
#include <cstring>
#include "../../common/shared_defs.h"

// Global state
CELayerState g_LayerState;

// Dispatch table storage
std::mutex g_InstanceMapMutex;
std::unordered_map<VkInstance, CEInstanceDispatch> g_InstanceMap;
std::mutex g_DeviceMapMutex;
std::unordered_map<VkDevice, CEDeviceDispatch> g_DeviceMap;

// Logging
// Redirected to IPC Host Process
void LayerLog(const char* fmt, ...) {
    static char buf[4096];
    
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Send to Host
    LayerIPC_Log("%s", buf);

    // Keep stderr for local console debugging
    fprintf(stderr, "[CE Layer] %s\n", buf);
}

// Helper: Fast Process Whitelist Check using Shared Memory
// Avoids unsafe File I/O (config.ini) in arbitrary processes.
static bool IsProcessWhitelistedFast() {
    // Get process name
    char exePath[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string fullPath(exePath);
    
    // Extract just the filename
    size_t pos = fullPath.rfind('\\');
    std::string processName = (pos != std::string::npos) ? fullPath.substr(pos + 1) : fullPath;
    
    // Store for layer state
    g_LayerState.processName = processName;

    // 1. Internal Whitelist
    if (_stricmp(processName.c_str(), "captureengine.exe") == 0 || _stricmp(processName.c_str(), "captureengine_x86.exe") == 0) {
        LayerLog("IsProcessWhitelistedFast: Process '%s' matched internal whitelist", processName.c_str());
        return true;
    }
    
    // 2. Shared Memory Whitelist Cache
    // If shared memory is not available or empty, we default to NOT whitelisted.
    HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
    if (hDisc) {
        DiscoveryInfo* pDisc = (DiscoveryInfo*)MapViewOfFile(hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
        bool found = false;
        if (pDisc) {
            if (pDisc->magic == DISCOVERY_MAGIC) {
                const char* p = pDisc->processWhitelist;
                const char* end = pDisc->processWhitelist + sizeof(pDisc->processWhitelist);
                
                LayerLog("IsProcessWhitelistedFast: Checking SM whitelist for '%s'...", processName.c_str());

                while (p < end && *p != '\0') {
                    if (_stricmp(processName.c_str(), p) == 0) {
                        found = true;
                        break;
                    }
                    p += strlen(p) + 1;
                }
            } else {
                 LayerLog("IsProcessWhitelistedFast: Shared Memory Magic mismatch! (Expected %X, Got %X)", DISCOVERY_MAGIC, pDisc->magic);
            }
            UnmapViewOfFile(pDisc);
        } else {
            LayerLog("IsProcessWhitelistedFast: Failed to MapViewOfFile!");
        }
        CloseHandle(hDisc);
        if (found) {
            LayerLog("IsProcessWhitelistedFast: Process '%s' is WHITELISTED (SM Match)", processName.c_str());
            return true;
        } else {
            LayerLog("IsProcessWhitelistedFast: Process '%s' NOT found in SM whitelist", processName.c_str());
        }
    } else {
        LayerLog("IsProcessWhitelistedFast: Failed to OpenFileMappingW (%S), err=%d", SHARED_MEM_DISCOVERY, GetLastError());
    }
    
    LayerLog("Layer: Process '%s' is NOT whitelisted (SM check failed), layer inactive", processName.c_str());
    return false;
}


// Get dispatch table for instance
CEInstanceDispatch* GetInstanceDispatch(VkInstance instance) {
    std::lock_guard<std::mutex> lock(g_InstanceMapMutex);
    auto it = g_InstanceMap.find(instance);
    return (it != g_InstanceMap.end()) ? &it->second : nullptr;
}

// Get dispatch table for device
CEDeviceDispatch* GetDeviceDispatch(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_DeviceMapMutex);
    auto it = g_DeviceMap.find(device);
    return (it != g_DeviceMap.end()) ? &it->second : nullptr;
}

// Layer negotiation - required for Vulkan loader
extern "C" VkResult VKAPI_CALL 
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    fprintf(stderr, "[CE Layer] vkNegotiateLoaderLayerInterfaceVersion called\n");
    if (!pVersionStruct) return VK_ERROR_INITIALIZATION_FAILED;
    
    if (pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    // We support interface version 2
    if (pVersionStruct->loaderLayerInterfaceVersion < 2) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    pVersionStruct->loaderLayerInterfaceVersion = 2;
    
    pVersionStruct->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr; // Not needed
    
    // Initialize layer state on first call
    if (!g_LayerState.initialized) {
        g_LayerState.initialized = true;
        g_LayerState.whitelisted = IsProcessWhitelistedFast();
        g_LayerState.overlayEnabled = g_LayerState.whitelisted;
        g_LayerState.captureEnabled = g_LayerState.whitelisted;
        
        if (g_LayerState.whitelisted) {
            LayerLog("Layer: CE_vkNegotiateLoaderLayerInterfaceVersion - Layer ACTIVE");
            // Initialize IPC for communication with captureengine
            if (LayerIPC_Init()) {
                LayerIPC_SetOverlayActive(true);
            }
        }
    }
    
    return VK_SUCCESS;
}

// Instance proc addr lookup
extern "C" PFN_vkVoidFunction VKAPI_CALL 
vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (!pName) return nullptr;
    
    // Layer-level functions (no instance needed)
    if (strcmp(pName, "vkNegotiateLoaderLayerInterfaceVersion") == 0) {
        return (PFN_vkVoidFunction)vkNegotiateLoaderLayerInterfaceVersion;
    }
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0) {
        return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    }
    if (strcmp(pName, "vkCreateInstance") == 0) {
        return (PFN_vkVoidFunction)vkCreateInstance;
    }
    
    // Instance-level functions
    if (instance != VK_NULL_HANDLE) {
        if (strcmp(pName, "vkDestroyInstance") == 0) {
            return (PFN_vkVoidFunction)vkDestroyInstance;
        }
        if (strcmp(pName, "vkCreateDevice") == 0) {
            return (PFN_vkVoidFunction)vkCreateDevice;
        }
        if (strcmp(pName, "vkGetDeviceProcAddr") == 0) {
            return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
        }
        
        // Chain to next layer/driver
        CEInstanceDispatch* disp = GetInstanceDispatch(instance);
        if (disp && disp->GetInstanceProcAddr) {
            return disp->GetInstanceProcAddr(instance, pName);
        }
    }
    
    return nullptr;
}

// Device proc addr lookup
extern "C" PFN_vkVoidFunction VKAPI_CALL 
vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    if (!pName) return nullptr;
    
    // Device-level functions we intercept
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    }
    if (strcmp(pName, "vkDestroyDevice") == 0) {
        return (PFN_vkVoidFunction)vkDestroyDevice;
    }
    
    // Only intercept these if whitelisted
    if (g_LayerState.whitelisted) {
        if (strcmp(pName, "vkCreateSwapchainKHR") == 0) {
            return (PFN_vkVoidFunction)vkCreateSwapchainKHR;
        }
        if (strcmp(pName, "vkDestroySwapchainKHR") == 0) {
            return (PFN_vkVoidFunction)vkDestroySwapchainKHR;
        }
        if (strcmp(pName, "vkAcquireNextImageKHR") == 0) {
            return (PFN_vkVoidFunction)vkAcquireNextImageKHR;
        }
        if (strcmp(pName, "vkQueuePresentKHR") == 0) {
            return (PFN_vkVoidFunction)vkQueuePresentKHR;
        }
        if (strcmp(pName, "vkCreateSampler") == 0) {
            return (PFN_vkVoidFunction)vkCreateSampler;
        }
        if (strcmp(pName, "vkGetDeviceQueue") == 0) {
            return (PFN_vkVoidFunction)vkGetDeviceQueue;
        }
    }
    
    // Chain to next layer/driver
    if (device != VK_NULL_HANDLE) {
        CEDeviceDispatch* disp = GetDeviceDispatch(device);
        if (disp && disp->GetDeviceProcAddr) {
            return disp->GetDeviceProcAddr(device, pName);
        }
    }
    
    return nullptr;
}

// Include needed for shutdown
#include "../common/system_metrics.h"

// DLL entry point
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinstDLL);
            break;
        case DLL_PROCESS_DETACH:
            // Important: Shutdown metrics thread before IPC or static destruction
            // This prevents the background thread from accessing destroyed IPC objects
            SystemMetricsCollector::Get().Shutdown();
            
            LayerIPC_Shutdown();
            break;
    }
    return TRUE;
}
