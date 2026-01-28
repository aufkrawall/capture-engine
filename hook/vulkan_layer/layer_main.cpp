/**
 * VK_LAYER_CE_overlay - Entry Points
 *
 * Implements vkGetInstanceProcAddr and vkGetDeviceProcAddr.
 * Handles layer negotiation and delegation to Capture_ hooks.
 */

#include "layer_main.h"
#include <cstring>

// Simple early logging to stderr before file logging is initialized
static void EarlyLog(const char* msg)
{
    fprintf(stderr, "[VulkanLayer-Init] %s\n", msg);
    fflush(stderr);
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        EarlyLog("DLL_PROCESS_ATTACH - Layer DLL loaded");
    } else if (reason == DLL_PROCESS_DETACH) {
        EarlyLog("DLL_PROCESS_DETACH - Layer DLL unloaded");
    }
    return TRUE;
}

// Global states
CELayerState g_LayerState;

// Forward declarations
static bool PerformEarlyWhitelistCheck();

// Forward declarations
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL CELayer_vkGetInstanceProcAddr(VkInstance instance,
                                                                                  const char* pName);
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL CELayer_vkGetDeviceProcAddr(VkDevice device, const char* pName);

// File logging for when IPC is not available
static FILE* g_LogFile = nullptr;
static bool g_LogFileInitialized = false;

static void InitLayerLogFile()
{
    if (g_LogFileInitialized) return;
    g_LogFileInitialized = true;

    g_LogFile = fopen("logs/vulkan_layer.log", "a");
    if (g_LogFile) {
        fprintf(g_LogFile, "\n=== Layer DLL Loaded ===\n");
        fflush(g_LogFile);
    }
    fprintf(stderr, "[VulkanLayer] InitLayerLogFile called\n");
    fflush(stderr);
}

// Logging implementation
void LayerLog(const char* fmt, ...)
{
    // OPTIMIZATION: Only process logs if debug logging is enabled in shared memory
    // or if the layer hasn't initialized IPC yet (for early errors).
    auto* shm = g_IPCClient.GetSharedMem();
    if (shm && !shm->debugLogging) return;

    va_list args;
    va_start(args, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Initialize log file on first call
    InitLayerLogFile();

    // Output to debug console
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");

    // Also to stderr
    fprintf(stderr, "[VulkanLayer] %s\n", buf);
    fflush(stderr);

    // Also to log file
    if (g_LogFile) {
        fprintf(g_LogFile, "[VulkanLayer] %s\n", buf);
        fflush(g_LogFile);
    }

    // Also to IPC if connected
    if (LayerIPC_IsConnected()) {
        LayerIPC_Log("%s", buf);
    }
}

// ============================================================================
// Layer Negotiation
// ============================================================================

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
CELayer_vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct)
{
    EarlyLog("NegotiateLoaderLayerInterfaceVersion called");

    if (pVersionStruct->loaderLayerInterfaceVersion < 2) {
        EarlyLog("Interface version too low");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    EarlyLog("Checking whitelist...");
    if (!PerformEarlyWhitelistCheck()) {
        EarlyLog("Process not whitelisted - layer entering passthrough mode");
        // Do NOT return error, just set flag (already done by check)
    }

    EarlyLog("Initializing IPC...");
    if (!LayerIPC_Init()) {
        EarlyLog("IPC connection failed - layer dormant");
        g_LayerState.whitelisted = false;
        // Do NOT return error, continue as passthrough
    }

    EarlyLog("IPC connected, initializing layer functions...");

    pVersionStruct->loaderLayerInterfaceVersion = 2;
    pVersionStruct->pfnGetInstanceProcAddr = CELayer_vkGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = CELayer_vkGetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;

    g_LayerState.initialized = true;
    LayerLog("Vulkan Layer: Negotiate Version Success - IPC connected");
    return VK_SUCCESS;
}

static bool g_WhitelistCheckDone = false;

static bool PerformEarlyWhitelistCheck()
{
    if (g_WhitelistCheckDone) return g_LayerState.whitelisted;
    g_WhitelistCheckDone = true;

    char fullPath[MAX_PATH];
    GetModuleFileNameA(NULL, fullPath, sizeof(fullPath));
    char* p = strrchr(fullPath, '\\');
    const char* processName = p ? p + 1 : fullPath;

    HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
    if (!hDisc) {
        g_LayerState.whitelisted = false;
        return false;
    }

    DiscoveryInfo* pDisc = (DiscoveryInfo*)MapViewOfFile(hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
    if (!pDisc) {
        CloseHandle(hDisc);
        g_LayerState.whitelisted = false;
        return false;
    }

    g_LayerState.whitelisted = false;
    if (pDisc->magic == DISCOVERY_MAGIC) {
        const char* pw = pDisc->processWhitelist;
        const char* end = pDisc->processWhitelist + sizeof(pDisc->processWhitelist);

        while (pw < end && *pw != '\0') {
            if (_stricmp(processName, pw) == 0) {
                g_LayerState.whitelisted = true;
                break;
            }
            pw += strlen(pw) + 1;
        }
    }

    UnmapViewOfFile(pDisc);
    CloseHandle(hDisc);

    if (!g_LayerState.whitelisted) {
        EarlyLog("Process not whitelisted - layer dormant");
    }

    return g_LayerState.whitelisted;
}

// ============================================================================
// ProcAddr Implementations
// ============================================================================

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL CELayer_vkGetInstanceProcAddr(VkInstance instance,
                                                                                  const char* pName)
{
    if (!pName) return nullptr;

    bool whitelisted = g_LayerState.whitelisted;

    // Layer-level functions (no instance needed)
    if (strcmp(pName, "vkNegotiateLoaderLayerInterfaceVersion") == 0) {
        return (PFN_vkVoidFunction)CELayer_vkNegotiateLoaderLayerInterfaceVersion;
    }
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0) {
        return (PFN_vkVoidFunction)CELayer_vkGetInstanceProcAddr;
    }
    if (strcmp(pName, "vkCreateInstance") == 0) {
        return (PFN_vkVoidFunction)Capture_vkCreateInstance;
    }
    // Always intercept CreateDevice to ensure dispatch table creation
    if (strcmp(pName, "vkCreateDevice") == 0) {
        return (PFN_vkVoidFunction)Capture_vkCreateDevice;
    }
    // Always intercept EnumeratePhysicalDevices to track instance-PD mapping
    if (strcmp(pName, "vkEnumeratePhysicalDevices") == 0) {
        return (PFN_vkVoidFunction)Capture_vkEnumeratePhysicalDevices;
    }

    if (strcmp(pName, "vkEnumerateInstanceLayerProperties") == 0 ||
        strcmp(pName, "vkEnumerateInstanceExtensionProperties") == 0) {
        return nullptr;
    }

    if (!instance) return nullptr;

    // Always intercept cleanup
    if (strcmp(pName, "vkDestroyInstance") == 0) return (PFN_vkVoidFunction)Capture_vkDestroyInstance;

    // Always intercept DeviceProcAddr to handle chain
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) return (PFN_vkVoidFunction)CELayer_vkGetDeviceProcAddr;

    // Conditional Hooks (Only if whitelisted)
    if (whitelisted) {
        if (strcmp(pName, "vkCreateWin32SurfaceKHR") == 0) return (PFN_vkVoidFunction)Capture_vkCreateWin32SurfaceKHR;
        if (strcmp(pName, "vkGetDeviceQueue") == 0) return (PFN_vkVoidFunction)Capture_vkGetDeviceQueue;
        if (strcmp(pName, "vkQueueSubmit") == 0) return (PFN_vkVoidFunction)Capture_vkQueueSubmit;
        if (strcmp(pName, "vkQueueSubmit2") == 0) return (PFN_vkVoidFunction)Capture_vkQueueSubmit2;
        if (strcmp(pName, "vkQueueSubmit2KHR") == 0) return (PFN_vkVoidFunction)Capture_vkQueueSubmit2KHR;
        if (strcmp(pName, "vkCreateSwapchainKHR") == 0) return (PFN_vkVoidFunction)Capture_vkCreateSwapchainKHR;
        if (strcmp(pName, "vkDestroySwapchainKHR") == 0) return (PFN_vkVoidFunction)Capture_vkDestroySwapchainKHR;
        if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0) return (PFN_vkVoidFunction)Capture_vkGetSwapchainImagesKHR;
        if (strcmp(pName, "vkAcquireNextImageKHR") == 0) return (PFN_vkVoidFunction)Capture_vkAcquireNextImageKHR;
        if (strcmp(pName, "vkQueuePresentKHR") == 0) return (PFN_vkVoidFunction)Capture_vkQueuePresentKHR;
        if (strcmp(pName, "vkCreateSampler") == 0) return (PFN_vkVoidFunction)Capture_vkCreateSampler;
    }

    // Default: chain to next layer
    InstanceDispatch* disp = VulkanLayerState::Get().GetInstanceDispatch(instance);
    if (disp && disp->fp_vkGetInstanceProcAddr) {
        return disp->fp_vkGetInstanceProcAddr(instance, pName);
    }

    return nullptr;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL CELayer_vkGetDeviceProcAddr(VkDevice device, const char* pName)
{
    if (!pName || !device) return nullptr;

    bool whitelisted = g_LayerState.whitelisted;

    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) return (PFN_vkVoidFunction)CELayer_vkGetDeviceProcAddr;
    if (strcmp(pName, "vkDestroyDevice") == 0) return (PFN_vkVoidFunction)Capture_vkDestroyDevice;

    if (whitelisted) {
        if (strcmp(pName, "vkGetDeviceQueue") == 0) return (PFN_vkVoidFunction)Capture_vkGetDeviceQueue;
        if (strcmp(pName, "vkCreateSwapchainKHR") == 0) return (PFN_vkVoidFunction)Capture_vkCreateSwapchainKHR;
        if (strcmp(pName, "vkDestroySwapchainKHR") == 0) return (PFN_vkVoidFunction)Capture_vkDestroySwapchainKHR;
        if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0) return (PFN_vkVoidFunction)Capture_vkGetSwapchainImagesKHR;
        if (strcmp(pName, "vkAcquireNextImageKHR") == 0) return (PFN_vkVoidFunction)Capture_vkAcquireNextImageKHR;
        if (strcmp(pName, "vkQueuePresentKHR") == 0) return (PFN_vkVoidFunction)Capture_vkQueuePresentKHR;
        if (strcmp(pName, "vkQueueSubmit") == 0) return (PFN_vkVoidFunction)Capture_vkQueueSubmit;
        if (strcmp(pName, "vkQueueSubmit2") == 0) return (PFN_vkVoidFunction)Capture_vkQueueSubmit2;
        if (strcmp(pName, "vkQueueSubmit2KHR") == 0) return (PFN_vkVoidFunction)Capture_vkQueueSubmit2KHR;
        if (strcmp(pName, "vkCreateSampler") == 0) return (PFN_vkVoidFunction)Capture_vkCreateSampler;
    }

    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (disp && disp->fp_vkGetDeviceProcAddr) {
        return disp->fp_vkGetDeviceProcAddr(device, pName);
    }

    return nullptr;
}
