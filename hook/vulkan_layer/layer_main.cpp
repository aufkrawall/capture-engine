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

// Get path to config.ini (same directory as captureengine.exe)
std::string GetConfigPath() {
    // First check environment variable set by captureengine
    char envPath[MAX_PATH] = {0};
    if (GetEnvironmentVariableA("CE_CONFIG_PATH", envPath, MAX_PATH) > 0) {
        return std::string(envPath);
    }
    
    // Fallback: look for config.ini in layer DLL directory
    char dllPath[MAX_PATH] = {0};
    HMODULE hModule = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&GetConfigPath, &hModule);
    if (hModule) {
        GetModuleFileNameA(hModule, dllPath, MAX_PATH);
        std::string path(dllPath);
        size_t pos = path.rfind('\\');
        if (pos != std::string::npos) {
            return path.substr(0, pos + 1) + "config.ini";
        }
    }
    
    return "config.ini";
}

// Parse whitelist from config.ini
static std::vector<std::string> ParseWhitelist(const std::string& configPath) {
    std::vector<std::string> whitelist;
    std::ifstream file(configPath);
    if (!file.is_open()) {
        return whitelist;
    }
    
    std::string line;
    bool inInjectionSection = false;
    bool inWhitelist = false;
    
    while (std::getline(file, line)) {
        // Trim whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        size_t end = line.find_last_not_of(" \t\r\n");
        line = line.substr(start, end - start + 1);
        
        // Skip comments
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        
        // Check for section headers
        if (line[0] == '[') {
            inInjectionSection = (line == "[Injection]");
            inWhitelist = false;
            continue;
        }
        
        // Parse whitelist in [Injection] section
        if (inInjectionSection) {
            if (line.find("whitelist=") == 0 || line.find("whitelist=(") == 0) {
                inWhitelist = true;
                // Check if it's a single-line or multi-line whitelist
                size_t eqPos = line.find('=');
                std::string value = line.substr(eqPos + 1);
                // Remove leading (
                if (!value.empty() && value[0] == '(') {
                    value = value.substr(1);
                }
                // Trim
                size_t vs = value.find_first_not_of(" \t\"");
                size_t ve = value.find_last_not_of(" \t\")");
                if (vs != std::string::npos && ve != std::string::npos && vs <= ve) {
                    std::string entry = value.substr(vs, ve - vs + 1);
                    if (!entry.empty() && entry != "(") {
                        whitelist.push_back(entry);
                    }
                }
                continue;
            }
            
            if (inWhitelist) {
                // End of whitelist
                if (line[0] == ')') {
                    inWhitelist = false;
                    continue;
                }
                // Parse entry
                size_t vs = line.find_first_not_of(" \t\"");
                size_t ve = line.find_last_not_of(" \t\")");
                if (vs != std::string::npos && ve != std::string::npos && vs <= ve) {
                    std::string entry = line.substr(vs, ve - vs + 1);
                    if (!entry.empty()) {
                        whitelist.push_back(entry);
                    }
                }
            }
        }
    }
    
    return whitelist;
}

// Check if current process is in whitelist
bool CheckProcessWhitelist() {
    // Get process name
    char exePath[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string fullPath(exePath);
    
    // Extract just the filename
    size_t pos = fullPath.rfind('\\');
    g_LayerState.processName = (pos != std::string::npos) ? fullPath.substr(pos + 1) : fullPath;
    
    // Get config path
    g_LayerState.configPath = GetConfigPath();
    
    // Parse whitelist
    std::vector<std::string> whitelist = ParseWhitelist(g_LayerState.configPath);
    
    if (whitelist.empty()) {
        LayerLog("Layer: No whitelist found in config, disabling");
        return false;
    }
    
    // Case-insensitive comparison
    std::string processLower = g_LayerState.processName;
    std::transform(processLower.begin(), processLower.end(), processLower.begin(), 
                   [](unsigned char c) { return std::tolower(c); });
    
    for (const auto& entry : whitelist) {
        std::string entryLower = entry;
        std::transform(entryLower.begin(), entryLower.end(), entryLower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (processLower == entryLower) {
            LayerLog("Layer: Process '%s' is WHITELISTED", g_LayerState.processName.c_str());
            return true;
        }
    }
    
    LayerLog("Layer: Process '%s' is NOT whitelisted, layer inactive", g_LayerState.processName.c_str());
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
extern "C" __declspec(dllexport) VkResult VKAPI_CALL 
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
        g_LayerState.whitelisted = CheckProcessWhitelist();
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
extern "C" __declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL 
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
extern "C" __declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL 
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

// DLL entry point
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinstDLL);
            break;
        case DLL_PROCESS_DETACH:
            LayerIPC_Shutdown();
            break;
    }
    return TRUE;
}
