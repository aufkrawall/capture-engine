/**
 * VK_LAYER_CE_overlay - Entry Points
 *
 * Implements vkGetInstanceProcAddr and vkGetDeviceProcAddr.
 * Handles layer negotiation and delegation to Capture_ hooks.
 */

#include "layer_main.h"
#include <cstring>
#include <filesystem>

#include "../../common/log_privacy.h"

// Get the directory where this DLL is located
static std::string GetLayerDllDirectory() {
    char dllPath[MAX_PATH];
    HMODULE hModule = NULL;

    // Get handle to this DLL
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
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

// Resolve the session-specific logs directory.
// Reads DiscoveryInfo.logsPath from shared memory (set by captureengine host).
// Falls back to {DLL directory}\logs if DiscoveryInfo is not yet available.
static std::string GetSessionLogsDirectory() {
    HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
    if (hDisc) {
        DiscoveryInfo* pDisc = (DiscoveryInfo*)MapViewOfFile(hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
        if (ValidateDiscoveryInfo(pDisc) && pDisc->logsPath[0] != '\0') {
            std::string dir(pDisc->logsPath);
            UnmapViewOfFile(pDisc);
            CloseHandle(hDisc);
            CreateDirectoryA(dir.c_str(), nullptr);
            return dir;
        }
        if (pDisc)
            UnmapViewOfFile(pDisc);
        CloseHandle(hDisc);
    }
    return GetLayerDllDirectory() + "\\logs";
}

// Simple early logging to file before file logging is initialized
// Uses absolute path based on DLL location to avoid CWD issues
static bool IsLayerDebugLoggingEnabled() {
    auto* shm = g_IPCClient.GetSharedMem();
    if (shm) {
        return shm->GetDebugLogging();
    }

    HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
    if (!hDisc) {
        return false;
    }

    bool debugLoggingEnabled = false;
    DiscoveryInfo* pDisc = (DiscoveryInfo*)MapViewOfFile(hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
    if (ValidateDiscoveryInfo(pDisc)) {
        const uint32_t hostPid = pDisc->GetInjectPid();
        if (hostPid != 0) {
            wchar_t sharedMemName[64] = {};
            GenerateSharedMemName(sharedMemName, _countof(sharedMemName), hostPid);

            HANDLE hSharedMem = OpenFileMappingW(FILE_MAP_READ, FALSE, sharedMemName);
            if (hSharedMem) {
                SharedMemoryLayout* pSharedMem =
                    (SharedMemoryLayout*)MapViewOfFile(hSharedMem, FILE_MAP_READ, 0, 0, sizeof(SharedMemoryLayout));
                if (pSharedMem && ValidateSharedMemory(pSharedMem)) {
                    debugLoggingEnabled = pSharedMem->GetDebugLogging();
                }
                if (pSharedMem) {
                    UnmapViewOfFile(pSharedMem);
                }
                CloseHandle(hSharedMem);
            }
        }
    }

    if (pDisc) {
        UnmapViewOfFile(pDisc);
    }
    CloseHandle(hDisc);
    return debugLoggingEnabled;
}

// The layer never writes to the host process's stdout/stderr. Those streams
// belong to the game, not to CE, and an injected DLL cannot know what is on the
// other end of them. DOOM Eternal session `20260819_034454` is the failure that
// proves it: the game's stderr was an inherited pipe nobody drained, the pipe
// buffer filled, and one `fprintf(stderr, ...)` carrying an FPS-limiter stats
// line left the game's present thread parked in NtWriteFile forever - a hard
// game freeze caused entirely by CE diagnostics. Every line already reaches
// vulkan_layer_early.log / vulkan_layer.log, OutputDebugString, and the IPC log,
// so staying out of the host's streams loses no diagnostic.
static void EarlyLog(const char* fmt, ...) {
    if (!IsLayerDebugLoggingEnabled())
        return;

    // One initialization, not a check-then-assign: EarlyLog runs on several
    // threads before the IPC log exists, and two of them racing on the same
    // std::string is a data race, not a harmless duplicate assignment.
    static const std::string logPath = GetSessionLogsDirectory() + "\\vulkan_layer_early.log";

    FILE* earlyLog = fopen(logPath.c_str(), "a");
    if (!earlyLog)
        return;

    va_list args;
    va_start(args, fmt);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[2048];
    vsnprintf(line, sizeof(line), fmt, args);
    ce::privacy::RedactUserAccountComponents(line);
    fprintf(earlyLog, "[%02d:%02d:%02d.%03d] [VulkanLayer-Init] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    fputs(line, earlyLog);
    fprintf(earlyLog, "\n");
    fflush(earlyLog);
    fclose(earlyLog);
    va_end(args);
}

// A resident layer whose layout no longer matches the running CaptureEngine can
// reach none of the usual diagnostics: the whitelist, the session logs path, and
// the debug-logging flag all live behind a discovery mapping it must not parse.
// Write one line next to the layer image instead, so the failure names itself
// rather than presenting as "the overlay simply never appeared".
void LayerReportIncompatibleDiscovery(const DiscoveryInfo* discovery) {
    if (!discovery)
        return;
    // Only the compatibility prefix may be read here; its offsets are fixed.
    if (discovery->GetMagic() != DISCOVERY_MAGIC)
        return;
    if (discovery->GetAbiSignature() == SHARED_MEMORY_ABI_SIGNATURE)
        return;

    static std::atomic<bool> reported{false};
    if (reported.exchange(true, std::memory_order_acq_rel))
        return;

    const std::string logsDir = GetLayerDllDirectory() + "\\logs";
    CreateDirectoryA(logsDir.c_str(), nullptr);
    const std::string logPath = logsDir + "\\vulkan_layer_incompatible.log";
    FILE* file = fopen(logPath.c_str(), "a");
    if (!file)
        return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char processPath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, processPath, sizeof(processPath));
    char line[1024];
    snprintf(line, sizeof(line),
             "[%04d-%02d-%02d %02d:%02d:%02d.%03d] Resident CE Vulkan layer build %u (layout 0x%08X) cannot attach to "
             "CaptureEngine build %u (layout 0x%08X) in '%s'. This title was started against a different CaptureEngine "
             "layout; restart it to load the current layer.\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
             static_cast<unsigned>(GetCurrentBuildNumber()), SHARED_MEMORY_ABI_SIGNATURE,
             static_cast<unsigned>(discovery->GetBuildNumber()), discovery->GetAbiSignature(), processPath);
    ce::privacy::RedactUserAccountComponents(line);
    fputs(line, file);
    fclose(file);
}

static bool IsExecutableFunctionPointer(const void* ptr) {
    if (!ptr)
        return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// File logging for when IPC is not available (declared before DllMain for cleanup)
static FILE* g_LogFile = nullptr;
static bool g_LogFileInitialized = false;

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        HMODULE pinnedLayer = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(hInst), &pinnedLayer)) {
            return FALSE;
        }
        EarlyLog("DLL_PROCESS_ATTACH - Layer DLL loaded");
    } else if (reason == DLL_PROCESS_DETACH) {
        // Never call IPC, logging, Vulkan, or C++ cleanup while the loader lock is
        // held. Cooperative host deactivation is performed by the lifecycle
        // watcher; process termination leaves remaining resources to the OS.
        g_LayerState.whitelisted.store(false, std::memory_order_release);
    }
    return TRUE;
}

// Global states
CELayerState g_LayerState;

// Forward declarations
static bool PerformEarlyWhitelistCheck();

// Forward declarations
extern "C" __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                                                                const char* pName);
extern "C" __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,
                                                                                              const char* pName);

static void InitLayerLogFile() {
    if (g_LogFileInitialized || !IsLayerDebugLoggingEnabled())
        return;
    g_LogFileInitialized = true;

    const std::filesystem::path logsDir = GetSessionLogsDirectory();
    std::error_code ec;
    std::filesystem::create_directories(logsDir, ec);
    const std::filesystem::path logPath = logsDir / "vulkan_layer.log";

    g_LogFile = fopen(logPath.string().c_str(), "a");
    if (g_LogFile) {
        fprintf(g_LogFile, "\n=== Layer DLL Loaded ===\n");
        fflush(g_LogFile);
    }
}

// Logging implementation
void LayerLog(const char* fmt, ...) {
    if (!IsLayerDebugLoggingEnabled())
        return;

    va_list args;
    va_start(args, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    // Log privacy: strip user-profile account names before fan-out.
    ce::privacy::RedactUserAccountComponents(buf);

    // Initialize log file on first call
    InitLayerLogFile();

    // Output to debug console
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");

    // Deliberately no write to the host process's stdout/stderr - see EarlyLog.

    // Also to log file
    if (g_LogFile) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_LogFile, "[%02u:%02u:%02u.%03u] [VulkanLayer] %s\n", st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds, buf);
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
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    EarlyLog("NegotiateLoaderLayerInterfaceVersion called");

    if (!pVersionStruct) {
        EarlyLog("ERROR: pVersionStruct is NULL");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    uint32_t requestedVersion = pVersionStruct->loaderLayerInterfaceVersion;
    EarlyLog("Loader interface version requested: %u", requestedVersion);
    LayerLog(
        "Vulkan Layer: vkNegotiateLoaderLayerInterfaceVersion called, "
        "requested version=%u, sType=%u, struct size=%zu",
        requestedVersion, pVersionStruct->sType, sizeof(*pVersionStruct));

    if (pVersionStruct->loaderLayerInterfaceVersion < 2) {
        EarlyLog("Interface version too low (need >= 2, got %u)", pVersionStruct->loaderLayerInterfaceVersion);
        LayerLog(
            "Vulkan Layer: Negotiation FAILED - interface version %u too old, "
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
        if (g_LayerState.whitelisted) {
            EarlyLog("IPC connection failed - layer dormant");
            LayerLog("Vulkan Layer: IPC initialization FAILED - layer will be dormant");
            g_LayerState.whitelisted = false;
            // Do NOT return error, continue as passthrough
        } else {
            EarlyLog("Skipping IPC connection in passthrough mode (not whitelisted)");
            LayerLog("Vulkan Layer: Passthrough mode active (process not whitelisted)");
        }
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
    LayerIPC_StartHostLifecycleWatcher();
    LayerLog(
        "Vulkan Layer: Negotiation SUCCESS - version set to 2, "
        "pfnGetInstanceProcAddr=%p, pfnGetDeviceProcAddr=%p",
        (void*)vkGetInstanceProcAddr, (void*)vkGetDeviceProcAddr);
    return VK_SUCCESS;
}

static bool g_WhitelistCheckDone = false;

static bool PerformEarlyWhitelistCheck() {
    if (g_WhitelistCheckDone)
        return g_LayerState.whitelisted;
    g_WhitelistCheckDone = true;

    DWORD inheritedParentPid = 0;
    g_LayerState.whitelisted = LayerIPC_IsProcessEligibleByCurrentHost(&inheritedParentPid);
    if (inheritedParentPid != 0) {
        EarlyLog("Process inherited Vulkan eligibility from published parent target/source PID %lu",
                 inheritedParentPid);
        LayerLog("Vulkan Layer: Inherited profile eligibility from published parent target/source PID %lu",
                 inheritedParentPid);
    }

    if (!g_LayerState.whitelisted) {
        EarlyLog("Process not whitelisted - layer dormant");
    }

    return g_LayerState.whitelisted;
}

// ============================================================================
// ProcAddr Implementations
// ============================================================================

extern "C" __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                                                                const char* pName) {
    if (!pName)
        return nullptr;

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
    // Always intercept EnumeratePhysicalDevices to track instance-PD mapping.
    // The device-group entry points produce the same handles and must be
    // tracked too: a multi-GPU-aware Vulkan 1.1 title may use only those, and
    // vkCreateDevice needs the owning instance for whichever route was taken.
    if (strcmp(pName, "vkEnumeratePhysicalDevices") == 0) {
        return (PFN_vkVoidFunction)Capture_vkEnumeratePhysicalDevices;
    }
    if (strcmp(pName, "vkEnumeratePhysicalDeviceGroups") == 0) {
        return (PFN_vkVoidFunction)Capture_vkEnumeratePhysicalDeviceGroups;
    }
    if (strcmp(pName, "vkEnumeratePhysicalDeviceGroupsKHR") == 0) {
        return (PFN_vkVoidFunction)Capture_vkEnumeratePhysicalDeviceGroupsKHR;
    }

    // Capabilities CE withholds have to be absent from what the application can
    // discover, not only from the calls it makes afterwards: hardware present
    // metering replaces the swapchain's vertical-blank rate contract for the
    // life of the device (vulkan_layer_capabilities.cpp).
    if (strcmp(pName, "vkEnumerateDeviceExtensionProperties") == 0) {
        return (PFN_vkVoidFunction)Capture_vkEnumerateDeviceExtensionProperties;
    }
    if (strcmp(pName, "vkGetPhysicalDeviceFeatures2") == 0) {
        return (PFN_vkVoidFunction)Capture_vkGetPhysicalDeviceFeatures2;
    }
    if (strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR") == 0) {
        return (PFN_vkVoidFunction)Capture_vkGetPhysicalDeviceFeatures2KHR;
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

    // Always expose forwarding hooks. Each hook performs only the minimal chain
    // bookkeeping while dormant; the stable address can be cached before CE starts.
    if (strcmp(pName, "vkCreateWin32SurfaceKHR") == 0)
        return (PFN_vkVoidFunction)Capture_vkCreateWin32SurfaceKHR;
    if (strcmp(pName, "vkDestroySurfaceKHR") == 0)
        return (PFN_vkVoidFunction)Capture_vkDestroySurfaceKHR;
    if (strcmp(pName, "vkGetDeviceQueue") == 0)
        return (PFN_vkVoidFunction)Capture_vkGetDeviceQueue;
    if (strcmp(pName, "vkGetDeviceQueue2") == 0)
        return (PFN_vkVoidFunction)Capture_vkGetDeviceQueue2;
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
    if (strcmp(pName, "vkSetLatencySleepModeNV") == 0)
        return (PFN_vkVoidFunction)Capture_vkSetLatencySleepModeNV;
    if (strcmp(pName, "vkLatencySleepNV") == 0)
        return (PFN_vkVoidFunction)Capture_vkLatencySleepNV;

    // Default: chain to next layer
    InstanceDispatch* disp = VulkanLayerState::Get().GetInstanceDispatch(instance);
    if (disp && disp->fp_vkGetInstanceProcAddr) {
        return disp->fp_vkGetInstanceProcAddr(instance, pName);
    }

    return nullptr;
}

extern "C" __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,
                                                                                              const char* pName) {
    if (!pName || !device)
        return nullptr;

    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (strcmp(pName, "vkDestroyDevice") == 0)
        return (PFN_vkVoidFunction)Capture_vkDestroyDevice;

    if (strcmp(pName, "vkGetDeviceQueue") == 0)
        return (PFN_vkVoidFunction)Capture_vkGetDeviceQueue;
    if (strcmp(pName, "vkGetDeviceQueue2") == 0)
        return (PFN_vkVoidFunction)Capture_vkGetDeviceQueue2;
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
    if (strcmp(pName, "vkSetLatencySleepModeNV") == 0)
        return (PFN_vkVoidFunction)Capture_vkSetLatencySleepModeNV;
    if (strcmp(pName, "vkLatencySleepNV") == 0)
        return (PFN_vkVoidFunction)Capture_vkLatencySleepNV;

    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (disp && disp->fp_vkGetDeviceProcAddr) {
        PFN_vkGetDeviceProcAddr nextGetDeviceProcAddr = disp->fp_vkGetDeviceProcAddr;
        if (!IsExecutableFunctionPointer((const void*)nextGetDeviceProcAddr)) {
            LayerLog("Vulkan Layer: [Warn] Invalid next vkGetDeviceProcAddr=%p for device=%p name=%s",
                     (void*)nextGetDeviceProcAddr, (void*)device, pName ? pName : "(null)");
            return nullptr;
        }
        return nextGetDeviceProcAddr(device, pName);
    }

    return nullptr;
}
