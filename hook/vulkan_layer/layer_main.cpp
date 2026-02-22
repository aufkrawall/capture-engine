/**
 * VK_LAYER_CE_overlay - Entry Points
 *
 * Implements vkGetInstanceProcAddr and vkGetDeviceProcAddr.
 * Handles layer negotiation and delegation to Capture_ hooks.
 */

#include "layer_main.h"
#include <cstring>
#include <filesystem>

// Get the directory where this DLL is located
static std::string GetLayerDllDirectory() {
  char dllPath[MAX_PATH];
  HMODULE hModule = NULL;
  
  // Get handle to this DLL
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | 
                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCSTR)&GetLayerDllDirectory, &hModule);
  
  if (hModule && GetModuleFileNameA(hModule, dllPath, MAX_PATH)) {
    std::string path(dllPath);
    size_t lastSlash = path.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
      return path.substr(0, lastSlash);
    }
  }
  return ".";
}

// Simple early logging to file before file logging is initialized
// Uses absolute path based on DLL location to avoid CWD issues
static void EarlyLog(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  
  // Build absolute path to log file (next to the DLL)
  static std::string logPath;
  if (logPath.empty()) {
    logPath = GetLayerDllDirectory() + "\\logs\\vulkan_layer_early.log";
  }
  
  // Log to file immediately (don't rely on stderr for GUI apps)
  FILE *earlyLog = fopen(logPath.c_str(), "a");
  if (earlyLog) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(earlyLog, "[%02d:%02d:%02d.%03d] [VulkanLayer-Init] ", 
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    vfprintf(earlyLog, fmt, args);
    fprintf(earlyLog, "\n");
    fflush(earlyLog);
    fclose(earlyLog);
  }
  
  // Also try stderr (for console apps)
  fprintf(stderr, "[VulkanLayer-Init] ");
  vfprintf(stderr, fmt, args);
  fprintf(stderr, "\n");
  fflush(stderr);
  va_end(args);
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    EarlyLog("DLL_PROCESS_ATTACH - Layer DLL loaded");
  } else if (reason == DLL_PROCESS_DETACH) {
    EarlyLog("DLL_PROCESS_DETACH - Layer DLL unloading, clearing vulkanLayerActive flag");
    // CRITICAL: Clear the vulkanLayerActive flag so other APIs (OpenGL, DX) know
    // Vulkan is no longer active. Without this, the flag persists in shared memory
    // and causes OpenGL/DX overlays to skip rendering because they think Vulkan is primary.
    extern IPCClient g_IPCClient;
    auto *sharedMem = g_IPCClient.GetSharedMem();
    if (sharedMem) {
      sharedMem->runtimeState.vulkanLayerActive.store(false, std::memory_order_release);
      EarlyLog("Cleared vulkanLayerActive flag in shared memory");
    }
  }
  return TRUE;
}

// Global states
CELayerState g_LayerState;

// Forward declarations
static bool PerformEarlyWhitelistCheck();

// Forward declarations
extern "C" __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName);
extern "C" __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName);

// File logging for when IPC is not available
static FILE *g_LogFile = nullptr;
static bool g_LogFileInitialized = false;

static void InitLayerLogFile() {
  if (g_LogFileInitialized)
    return;
  g_LogFileInitialized = true;

  const std::filesystem::path logsDir =
      std::filesystem::path(GetLayerDllDirectory()) / "logs";
  std::error_code ec;
  std::filesystem::create_directories(logsDir, ec);
  const std::filesystem::path logPath = logsDir / "vulkan_layer.log";

  g_LogFile = fopen(logPath.string().c_str(), "a");
  if (g_LogFile) {
    fprintf(g_LogFile, "\n=== Layer DLL Loaded ===\n");
    fflush(g_LogFile);
  }
  fprintf(stderr, "[VulkanLayer] InitLayerLogFile called\n");
  fflush(stderr);
}

// Logging implementation
void LayerLog(const char *fmt, ...) {
  // OPTIMIZATION: Only process logs if debug logging is enabled in shared
  // memory or if the layer hasn't initialized IPC yet (for early errors).
  auto *shm = g_IPCClient.GetSharedMem();
  if (shm && !shm->GetDebugLogging())
    return;

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

extern "C" __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(
    VkNegotiateLayerInterface *pVersionStruct) {
  EarlyLog("NegotiateLoaderLayerInterfaceVersion called");

  if (!pVersionStruct) {
    EarlyLog("ERROR: pVersionStruct is NULL");
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  uint32_t requestedVersion = pVersionStruct->loaderLayerInterfaceVersion;
  EarlyLog("Loader interface version requested: %u", requestedVersion);
  LayerLog("Vulkan Layer: vkNegotiateLoaderLayerInterfaceVersion called, "
           "requested version=%u, sType=%u, struct size=%zu",
           requestedVersion, pVersionStruct->sType, sizeof(*pVersionStruct));

  if (pVersionStruct->loaderLayerInterfaceVersion < 2) {
    EarlyLog("Interface version too low (need >= 2, got %u)",
             pVersionStruct->loaderLayerInterfaceVersion);
    LayerLog("Vulkan Layer: Negotiation FAILED - interface version %u too old, "
             "minimum required is 2",
             pVersionStruct->loaderLayerInterfaceVersion);
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  EarlyLog("Checking whitelist...");
  if (!PerformEarlyWhitelistCheck()) {
    EarlyLog("Process not whitelisted - layer entering passthrough mode");
    LayerLog("Vulkan Layer: Process not whitelisted - entering passthrough mode");
    // Do NOT return error, just set flag (already done by check)
  } else {
    LayerLog("Vulkan Layer: Process whitelisted - full layer mode enabled");
  }

  EarlyLog("Initializing IPC...");
  if (!LayerIPC_Init()) {
    EarlyLog("IPC connection failed - layer dormant");
    LayerLog("Vulkan Layer: IPC initialization FAILED - layer will be dormant");
    g_LayerState.whitelisted = false;
    // Do NOT return error, continue as passthrough
  } else {
    LayerLog("Vulkan Layer: IPC initialized successfully");
  }

  if (LayerIPC_IsConnected()) {
    EarlyLog("IPC connected, initializing layer functions...");
  }

  pVersionStruct->loaderLayerInterfaceVersion = 2;
  pVersionStruct->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
  pVersionStruct->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
  pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;

  g_LayerState.initialized = true;
  LayerLog("Vulkan Layer: Negotiation SUCCESS - version set to 2, "
           "pfnGetInstanceProcAddr=%p, pfnGetDeviceProcAddr=%p",
           (void *)vkGetInstanceProcAddr, (void *)vkGetDeviceProcAddr);
  return VK_SUCCESS;
}

static bool g_WhitelistCheckDone = false;

static bool PerformEarlyWhitelistCheck() {
  if (g_WhitelistCheckDone)
    return g_LayerState.whitelisted;
  g_WhitelistCheckDone = true;

  char fullPath[MAX_PATH];
  GetModuleFileNameA(NULL, fullPath, sizeof(fullPath));
  char *p = strrchr(fullPath, '\\');
  const char *processName = p ? p + 1 : fullPath;

  HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
  if (!hDisc) {
    g_LayerState.whitelisted = false;
    return false;
  }

  DiscoveryInfo *pDisc = (DiscoveryInfo *)MapViewOfFile(
      hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
  if (!pDisc) {
    CloseHandle(hDisc);
    g_LayerState.whitelisted = false;
    return false;
  }

  g_LayerState.whitelisted = false;
  if (pDisc->GetMagic() == DISCOVERY_MAGIC) {
    const char *pw = pDisc->processWhitelist;
    const char *end = pDisc->processWhitelist + sizeof(pDisc->processWhitelist);

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

extern "C" __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName) {
  if (!pName)
    return nullptr;

  bool whitelisted = g_LayerState.whitelisted;

  // Layer-level functions (no instance needed)
  if (strcmp(pName, "vkNegotiateLoaderLayerInterfaceVersion") == 0) {
    return (PFN_vkVoidFunction)vkNegotiateLoaderLayerInterfaceVersion;
  }
  if (strcmp(pName, "vkGetInstanceProcAddr") == 0) {
    return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
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

  if (!instance)
    return nullptr;

  // Always intercept cleanup
  if (strcmp(pName, "vkDestroyInstance") == 0)
    return (PFN_vkVoidFunction)Capture_vkDestroyInstance;

  // Always intercept DeviceProcAddr to handle chain
  if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
    return (PFN_vkVoidFunction)vkGetDeviceProcAddr;

  // Conditional Hooks (Only if whitelisted)
  if (whitelisted) {
    if (strcmp(pName, "vkCreateWin32SurfaceKHR") == 0)
      return (PFN_vkVoidFunction)Capture_vkCreateWin32SurfaceKHR;
    if (strcmp(pName, "vkGetDeviceQueue") == 0)
      return (PFN_vkVoidFunction)Capture_vkGetDeviceQueue;
    if (strcmp(pName, "vkQueueSubmit") == 0)
      return (PFN_vkVoidFunction)Capture_vkQueueSubmit;
    if (strcmp(pName, "vkQueueSubmit2") == 0)
      return (PFN_vkVoidFunction)Capture_vkQueueSubmit2;
    if (strcmp(pName, "vkQueueSubmit2KHR") == 0)
      return (PFN_vkVoidFunction)Capture_vkQueueSubmit2KHR;
    if (strcmp(pName, "vkCreateSwapchainKHR") == 0)
      return (PFN_vkVoidFunction)Capture_vkCreateSwapchainKHR;
    if (strcmp(pName, "vkDestroySwapchainKHR") == 0)
      return (PFN_vkVoidFunction)Capture_vkDestroySwapchainKHR;
    if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0)
      return (PFN_vkVoidFunction)Capture_vkGetSwapchainImagesKHR;
    if (strcmp(pName, "vkAcquireNextImageKHR") == 0)
      return (PFN_vkVoidFunction)Capture_vkAcquireNextImageKHR;
    if (strcmp(pName, "vkQueuePresentKHR") == 0)
      return (PFN_vkVoidFunction)Capture_vkQueuePresentKHR;
    if (strcmp(pName, "vkCreateSampler") == 0)
      return (PFN_vkVoidFunction)Capture_vkCreateSampler;
  }

  // Default: chain to next layer
  InstanceDispatch *disp =
      VulkanLayerState::Get().GetInstanceDispatch(instance);
  if (disp && disp->fp_vkGetInstanceProcAddr) {
    return disp->fp_vkGetInstanceProcAddr(instance, pName);
  }

  return nullptr;
}

extern "C" __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName) {
  if (!pName || !device)
    return nullptr;

  bool whitelisted = g_LayerState.whitelisted;

  if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
    return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
  if (strcmp(pName, "vkDestroyDevice") == 0)
    return (PFN_vkVoidFunction)Capture_vkDestroyDevice;

  if (whitelisted) {
    if (strcmp(pName, "vkGetDeviceQueue") == 0)
      return (PFN_vkVoidFunction)Capture_vkGetDeviceQueue;
    if (strcmp(pName, "vkCreateSwapchainKHR") == 0)
      return (PFN_vkVoidFunction)Capture_vkCreateSwapchainKHR;
    if (strcmp(pName, "vkDestroySwapchainKHR") == 0)
      return (PFN_vkVoidFunction)Capture_vkDestroySwapchainKHR;
    if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0)
      return (PFN_vkVoidFunction)Capture_vkGetSwapchainImagesKHR;
    if (strcmp(pName, "vkAcquireNextImageKHR") == 0)
      return (PFN_vkVoidFunction)Capture_vkAcquireNextImageKHR;
    if (strcmp(pName, "vkQueuePresentKHR") == 0)
      return (PFN_vkVoidFunction)Capture_vkQueuePresentKHR;
    if (strcmp(pName, "vkQueueSubmit") == 0)
      return (PFN_vkVoidFunction)Capture_vkQueueSubmit;
    if (strcmp(pName, "vkQueueSubmit2") == 0)
      return (PFN_vkVoidFunction)Capture_vkQueueSubmit2;
    if (strcmp(pName, "vkQueueSubmit2KHR") == 0)
      return (PFN_vkVoidFunction)Capture_vkQueueSubmit2KHR;
    if (strcmp(pName, "vkCreateSampler") == 0)
      return (PFN_vkVoidFunction)Capture_vkCreateSampler;
  }

  DeviceDispatch *disp = VulkanLayerState::Get().GetDeviceDispatch(device);
  if (disp && disp->fp_vkGetDeviceProcAddr) {
    return disp->fp_vkGetDeviceProcAddr(device, pName);
  }

  return nullptr;
}
