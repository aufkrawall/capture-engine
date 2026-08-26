/**
 * VK_LAYER_CE_overlay - IPC Implementation
 *
 * Uses standard IPCClient to communicate with CaptureEngine.
 * Determines overlay config, styling, and publishes texture handles.
 */

#include <dxgiformat.h>
#include <tlhelp32.h>
#include <vulkan/vulkan.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include "../common/ipc_client.h"
#include "../common/perf_logger.h"
#include "../common/vulkan_renderer_policy.h"
#include "layer_main.h"
#include "vulkan_layer.h"

// Global IPC Client
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
IPCClient g_IPCClient;
// Shared globals needed by system_metrics.cpp and other common files
IPCClient* g_IPC = &g_IPCClient;
char g_ProcessName[260] = "CaptureLayer";
// Dummy config to satisfy EarlyLog if it checks g_LocalConfig (though our shim
// doesn't) Actually system_metrics uses EarlyLog which we redirect. But does
// system_metrics use g_LocalConfig directly? No, it uses EarlyLog.

#include "../common/hook_common.h"  // For definitions if needed
// Define globals from hook_common.h that we are missing because we don't link
// hook_common.cpp
std::atomic<bool> g_ShuttingDown{false};
// g_GraphicsOverridesActive is definition in hook_common.cpp, but do we need
// it? Only if we use GetActiveGraphicsConfig. Overlay.cpp doesn't uses it.

// Log shims moved to layer_bridge.cpp

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

// Initialize layer IPC
namespace {
HANDLE g_LayerReactivateEvent = nullptr;
HANDLE g_LayerHostStoppingEvent = nullptr;
HANDLE g_LayerDormantEvent = nullptr;
}

static void RefreshLayerProcessName() {
    char fullPath[MAX_PATH] = {};
    GetModuleFileNameA(NULL, fullPath, sizeof(fullPath));
    const char* base = strrchr(fullPath, '\\');
    strncpy(g_ProcessName, base ? base + 1 : fullPath, sizeof(g_ProcessName) - 1);
    g_ProcessName[sizeof(g_ProcessName) - 1] = '\0';
}

static bool IsProcessNameWhitelisted(const DiscoveryInfo* info, const char* processName) {
    if (!info || !processName)
        return false;

    const char* entry = info->processWhitelist;
    const char* end = entry + sizeof(info->processWhitelist);
    while (entry < end && *entry != '\0') {
        if (_stricmp(processName, entry) == 0)
            return true;
        const size_t remaining = static_cast<size_t>(end - entry);
        const size_t length = strnlen(entry, remaining);
        if (length == remaining)
            break;
        entry += length + 1;
    }
    return false;
}

static bool GetCurrentParentIdentity(DWORD* parentPid, char* parentName, size_t parentNameSize) {
    if (!parentPid || !parentName || parentNameSize == 0)
        return false;
    *parentPid = 0;
    parentName[0] = '\0';

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    bool foundCurrent = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == GetCurrentProcessId()) {
                *parentPid = entry.th32ParentProcessID;
                foundCurrent = *parentPid != 0;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    bool foundParent = false;
    entry = {};
    entry.dwSize = sizeof(entry);
    if (foundCurrent && Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == *parentPid) {
                foundParent = WideCharToMultiByte(CP_UTF8, 0, entry.szExeFile, -1, parentName,
                                                  static_cast<int>(parentNameSize), nullptr, nullptr) > 0;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return foundParent;
}

static uint32_t ReadActiveSourcePid(const DiscoveryInfo* info) {
    if (!info || info->GetInjectPid() == 0)
        return 0;

    wchar_t sharedMemName[64] = {};
    GenerateSharedMemName(sharedMemName, _countof(sharedMemName), info->GetInjectPid());
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, sharedMemName);
    if (!mapping)
        return 0;

    auto* sharedMemory = static_cast<SharedMemoryLayout*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    const uint32_t sourcePid = ValidateSharedMemory(sharedMemory) ? sharedMemory->GetSourcePid() : 0;
    if (sharedMemory)
        UnmapViewOfFile(sharedMemory);
    CloseHandle(mapping);
    return sourcePid;
}

bool LayerIPC_IsProcessEligibleByCurrentHost(DWORD* inheritedParentPid) {
    if (inheritedParentPid)
        *inheritedParentPid = 0;
    RefreshLayerProcessName();

    HANDLE discovery = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
    if (!discovery)
        return false;
    auto* info = static_cast<DiscoveryInfo*>(MapViewOfFile(discovery, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo)));
    bool eligible = false;
    // A host that appeared after this layer loaded may be an incompatible build;
    // that is exactly the late-injection wake path, so report it from here too.
    LayerReportIncompatibleDiscovery(info);
    if (ValidateDiscoveryInfo(info)) {
        const bool currentProcessWhitelisted = IsProcessNameWhitelisted(info, g_ProcessName);
        eligible = currentProcessWhitelisted;
        if (!eligible) {
            DWORD parentPid = 0;
            char parentName[MAX_PATH] = {};
            const bool parentKnown = GetCurrentParentIdentity(&parentPid, parentName, sizeof(parentName));
            const uint32_t activeSourcePid = parentKnown ? ReadActiveSourcePid(info) : 0;
            const uint32_t profileTargetPid = info->GetProfileTargetPid();
            const bool parentProcessWhitelisted = parentKnown && IsProcessNameWhitelisted(info, parentName);
            eligible = ce::vulkan_renderer_policy::ShouldEnableVulkanLayerForProfile(
                false, parentPid, activeSourcePid, profileTargetPid, parentProcessWhitelisted);
            if (eligible && inheritedParentPid) {
                *inheritedParentPid = parentPid;
            }
        }
    }
    if (info)
        UnmapViewOfFile(info);
    CloseHandle(discovery);
    return eligible;
}

bool LayerIPC_Init() {
    static std::mutex initMutex;
    std::lock_guard<std::mutex> initLock(initMutex);

    SharedMemoryLayout* current = g_IPCClient.GetSharedMem();
    if (current && !current->GetRequestExit() && g_LayerState.whitelisted.load(std::memory_order_acquire))
        return true;

    DWORD inheritedParentPid = 0;
    if (!LayerIPC_IsProcessEligibleByCurrentHost(&inheritedParentPid)) {
        g_LayerState.whitelisted.store(false, std::memory_order_release);
        LayerLog("Layer IPC: Process '%s' is not whitelisted by the published host", g_ProcessName);
        return false;
    }
    if (inheritedParentPid != 0) {
        LayerLog("Layer IPC: Process '%s' inherited Vulkan eligibility from published parent target/source PID %lu",
                 g_ProcessName, inheritedParentPid);
    }

    // Connect to host
    if (current ? g_IPCClient.Reconnect() : g_IPCClient.Connect()) {
        const uint64_t hostGeneration = g_LayerHostGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        LayerLog("Layer IPC: Connected to Host PID %d", g_IPCClient.GetSharedMem()->GetHostPID());
        g_LayerState.whitelisted.store(true, std::memory_order_release);
        g_ShuttingDown.store(false, std::memory_order_release);
        LayerLog("Layer IPC: Process '%s' whitelisted. Layer active (generation=%llu).", g_ProcessName,
                 static_cast<unsigned long long>(hostGeneration));

        // Set vulkanLayerActive flag so other APIs (OpenGL, DX) know Vulkan is primary
        g_IPCClient.GetSharedMem()->runtimeState.vulkanLayerActive.store(true, std::memory_order_release);
        LayerLog("Set vulkanLayerActive flag in shared memory");

        // Update graphics config from shared memory
        VulkanLayerState::Get().UpdateFromSharedMemory(&g_IPCClient);

        // Initialize performance logging if debug logging is enabled
        if (g_IPCClient.GetSharedMem()->GetDebugLogging()) {
            // Use logsPath from DiscoveryInfo (set by captureengine)
            char logPath[MAX_PATH] = {0};
            bool pathFound = false;

            HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
            if (hDisc) {
                DiscoveryInfo* pDisc = (DiscoveryInfo*)MapViewOfFile(hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
                if (ValidateDiscoveryInfo(pDisc) && pDisc->logsPath[0] != '\0') {
                    CreateDirectoryA(pDisc->logsPath, nullptr);
                    snprintf(logPath, sizeof(logPath), "%s\\perf_metrics_%d.csv", pDisc->logsPath,
                             GetCurrentProcessId());
                    pathFound = true;
                    LayerLog("Using logsPath from discovery: %s", pDisc->logsPath);
                }
                if (pDisc)
                    UnmapViewOfFile(pDisc);
                CloseHandle(hDisc);
            }

            // Fallback to game directory if discovery path not available
            if (!pathFound) {
                char gameDir[MAX_PATH];
                GetModuleFileNameA(NULL, gameDir, MAX_PATH);
                char* lastSlash = strrchr(gameDir, '\\');
                if (lastSlash) {
                    *lastSlash = '\0';
                    char logsDir[MAX_PATH];
                    snprintf(logsDir, sizeof(logsDir), "%s\\logs", gameDir);
                    CreateDirectoryA(logsDir, nullptr);
                    snprintf(logPath, sizeof(logPath), "%s\\perf_metrics_%d.csv", logsDir, GetCurrentProcessId());
                    LayerLog("[Hook] PerfLogger: Using fallback logs path: %s", logsDir);
                }
            }

            if (logPath[0] != '\0') {
                PerfLogger::Get().Init(logPath, true);
            }
        }

        return true;
    }

    LayerLog("Layer IPC: Failed to connect to host. Layer dormant.");
    return false;
}

// Shutdown layer IPC
void LayerIPC_Shutdown() {
    g_IPCClient.Disconnect();
    LayerLog("Layer IPC: Shutdown");
}

// Check if IPC is connected
bool LayerIPC_IsConnected() {
    return g_IPCClient.GetSharedMem() != nullptr;
}

namespace {

uint32_t GetAdvertisedHostPid() {
    uint32_t hostPid = 0;
    HANDLE discovery = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
    if (!discovery)
        return 0;
    auto* info = static_cast<DiscoveryInfo*>(MapViewOfFile(discovery, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo)));
    if (ValidateDiscoveryInfo(info))
        hostPid = info->GetInjectPid();
    if (info)
        UnmapViewOfFile(info);
    CloseHandle(discovery);
    return hostPid;
}

void SetLayerDormant(const char* reason) {
    g_ShuttingDown.store(true, std::memory_order_release);
    g_LayerState.whitelisted.store(false, std::memory_order_release);
    if (SharedMemoryLayout* sharedMemory = g_IPCClient.GetSharedMem()) {
        sharedMemory->runtimeState.vulkanLayerActive.store(false, std::memory_order_release);
        sharedMemory->runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagVulkanOverlayActive, false);
    }
    if (g_LayerDormantEvent)
        SetEvent(g_LayerDormantEvent);
    LayerLog("[InjectLifecycle] Vulkan layer dormant (%s)", reason ? reason : "host unavailable");
}

bool TryReactivateLayer() {
    // Consume this generation before inspecting discovery so a newer host's
    // concurrent signal remains set for the next attempt.
    if (g_LayerReactivateEvent)
        ResetEvent(g_LayerReactivateEvent);
    if (!LayerIPC_Init())
        return false;
    if (g_LayerDormantEvent)
        ResetEvent(g_LayerDormantEvent);
    SharedMemoryLayout* sharedMemory = g_IPCClient.GetSharedMem();
    LayerLog("[InjectLifecycle] Vulkan layer reactivated for host PID %u",
             sharedMemory ? sharedMemory->GetHostPID() : 0);
    return true;
}

DWORD WINAPI LayerHostLifecycleThread(LPVOID) {
    for (;;) {
        if (g_LayerState.whitelisted.load(std::memory_order_acquire)) {
            SharedMemoryLayout* sharedMemory = g_IPCClient.GetSharedMem();
            const uint32_t hostPid = sharedMemory ? sharedMemory->GetHostPID() : 0;
            HANDLE hostProcess = hostPid ? OpenProcess(SYNCHRONIZE, FALSE, hostPid) : nullptr;
            if (!hostProcess) {
                SetLayerDormant(hostPid ? "host process inaccessible" : "host identity unavailable");
            } else {
                HANDLE waits[3] = {};
                DWORD count = 0;
                DWORD hostStopIndex = MAXDWORD;
                DWORD hostProcessIndex = MAXDWORD;
                DWORD reactivateIndex = MAXDWORD;
                if (g_LayerHostStoppingEvent) {
                    hostStopIndex = count;
                    waits[count++] = g_LayerHostStoppingEvent;
                }
                hostProcessIndex = count;
                waits[count++] = hostProcess;
                if (g_LayerReactivateEvent) {
                    reactivateIndex = count;
                    waits[count++] = g_LayerReactivateEvent;
                }
                const DWORD wait = count ? WaitForMultipleObjects(count, waits, FALSE, INFINITE) : WAIT_FAILED;
                const DWORD signaledIndex = wait >= WAIT_OBJECT_0 && wait < WAIT_OBJECT_0 + count
                                                ? wait - WAIT_OBJECT_0
                                                : MAXDWORD;
                const bool hostDied = signaledIndex == hostProcessIndex;
                CloseHandle(hostProcess);

                if (signaledIndex == reactivateIndex && GetAdvertisedHostPid() == hostPid) {
                    // The injector signals before a newly loaded hook can connect.
                    // If this layer was already active for that same host, consume
                    // the redundant wakeup without toggling its runtime resources.
                    ResetEvent(g_LayerReactivateEvent);
                    continue;
                }

                const char* reason = hostDied                           ? "host process exited"
                                     : signaledIndex == reactivateIndex ? "replacement host signaled"
                                     : signaledIndex == hostStopIndex   ? "host requested shutdown"
                                                                        : "host wait failed";
                SetLayerDormant(reason);
                if ((hostDied || signaledIndex == reactivateIndex) && TryReactivateLayer())
                    continue;
            }
        }

        if (!g_LayerReactivateEvent || WaitForSingleObject(g_LayerReactivateEvent, INFINITE) != WAIT_OBJECT_0)
            return 0;
        if (TryReactivateLayer())
            continue;

        // The wakeup was reset before this attempt. A later target signal can
        // now retry a transient mapping/whitelist race without waiting for the
        // currently advertised host process to terminate.
        LayerLog("[InjectLifecycle] Vulkan reactivation unavailable; waiting for the next target signal");
    }
}

}  // namespace

void LayerIPC_StartHostLifecycleWatcher() {
    static std::atomic<bool> started{false};
    if (started.exchange(true, std::memory_order_acq_rel))
        return;
    wchar_t eventName[64] = {};
    GenerateInjectHostStoppingEventName(eventName, _countof(eventName));
    g_LayerHostStoppingEvent = CreateEventW(nullptr, TRUE, FALSE, eventName);
    GenerateVulkanReactivateEventName(eventName, _countof(eventName), GetCurrentProcessId());
    g_LayerReactivateEvent = CreateEventW(nullptr, TRUE, FALSE, eventName);
    GenerateVulkanDormantEventName(eventName, _countof(eventName), GetCurrentProcessId());
    g_LayerDormantEvent = CreateEventW(nullptr, TRUE, FALSE, eventName);
    if (g_LayerState.whitelisted.load(std::memory_order_acquire)) {
        // A late-injection signal can predate this watcher. The already-live IPC
        // connection consumed that generation; do not reuse it when this host
        // subsequently requests deactivation.
        if (g_LayerReactivateEvent)
            ResetEvent(g_LayerReactivateEvent);
        if (g_LayerDormantEvent)
            ResetEvent(g_LayerDormantEvent);
    }

    HANDLE thread = CreateThread(nullptr, 0, LayerHostLifecycleThread, nullptr, 0, nullptr);
    if (thread)
        CloseHandle(thread);
    else
        LayerLog("[InjectLifecycle] Failed to start Vulkan host watcher (error=%lu)", GetLastError());
}

// Global texture count
static uint32_t g_PublishedTextureCount = 2;

static void LogFrameRingFull(uint32_t writeIndex, uint32_t readIndex) {
    static std::atomic<uint32_t> fullCount{0};
    const uint32_t count = fullCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 4 || count % 1000u == 0u) {
        LayerLog("Layer IPC: Frame ring full (w=%u r=%u distance=%u count=%u)", writeIndex, readIndex,
                 static_cast<uint32_t>(writeIndex - readIndex), count);
    }
}

static void LogPublishedFrame(uint32_t writeIndex, uint32_t readIndex, int32_t textureIndex, uint64_t fenceValue) {
    if (writeIndex <= 3 || writeIndex % 10000u == 0u) {
        LayerLog("Layer IPC: Published capture frame (w=%u r=%u texture=%d fence=%llu)", writeIndex, readIndex,
                 textureIndex, static_cast<unsigned long long>(fenceValue));
    }
}

// Update shared texture handles (called when swapchain created)
void LayerIPC_SetTextures(const HANDLE* handles, uint32_t count, uint32_t width, uint32_t height, uint32_t format) {
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem)
        return;

    g_PublishedTextureCount = (count > 0 && count <= SHARED_TEXTURE_SLOT_COUNT) ? count : 2;

    // Write metadata
    mem->SetWidth(width);
    mem->SetHeight(height);
    mem->SetFormat(format);

    // Write handles up to the shared-memory layout limit.
    uint32_t maxHandles = SHARED_TEXTURE_SLOT_COUNT;
    for (uint32_t i = 0; i < count && i < maxHandles; i++) {
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        mem->SetSharedHandle(i, (uint64_t)handles[i]);
        LayerLog("Layer IPC: Wrote handle %d = %p to shared memory", i, handles[i]);
    }

    // Memory fence: ensure handles are written before encoder sees them
    std::atomic_thread_fence(std::memory_order_seq_cst);

    LayerLog("Layer IPC: Published %d textures (%dx%d)", count, width, height);
}

// Update frame timing (called each present)
void LayerIPC_UpdateFrameTiming(uint64_t frameCount, float fps, float avgFps) {
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem)
        return;

    // Update runtime state directly (atomics)
    // mem->runtimeState.currentFPS.store(fps, std::memory_order_relaxed); //
    // Missing in CaptureState? Wait, CaptureState has currentFPS (atomic double).
    mem->runtimeState.currentFPS.store(fps, std::memory_order_relaxed);
    mem->runtimeState.gameFPS.store(avgFps, std::memory_order_relaxed);
}

// Check if overlay should be shown
bool LayerIPC_ShouldShowOverlay() {
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem)
        return true;
    return mem->ReadOverlayConfig().showOverlay;
}

// Check if capture is requested
bool LayerIPC_IsCaptureRequested() {
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem)
        return false;
    return mem->runtimeState.IsInjectVideoCaptureRequested();
}

// Set capture active status
void LayerIPC_SetCaptureActive(bool active) {
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem)
        return;
    // ...
}

// Set overlay active status
void LayerIPC_SetOverlayActive(bool active) {
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem)
        return;
    mem->runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagVulkanOverlayActive, active);
}

void LayerIPC_SetLUID(int32_t low, int32_t high) {
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem)
        return;

    const uint32_t publisherPid = GetCurrentProcessId();
    const bool changed = mem->GetLuidLowPart() != low || mem->GetLuidHighPart() != high ||
                         mem->GetLuidSourcePid() != publisherPid;
    mem->SetLuidLowPart(low);
    mem->SetLuidHighPart(high);
    mem->SetLuidSourcePid(publisherPid);
    if (changed) {
        LayerLog("Layer IPC: Published renderer LUID %08x:%08x (publisherPid=%u profileSourcePid=%u)",
                 static_cast<uint32_t>(high), static_cast<uint32_t>(low), publisherPid, mem->GetSourcePid());
    }
}

uint32_t LayerIPC_GetWriteIndex() {
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem)
        return 0;
    return mem->frameRing.writeIndex.load(std::memory_order_acquire);  // Use frameRing
}

void LayerIPC_IncrementWriteIndex(uint64_t timestamp) {
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem)
        return;

    // Update timestamp for the current slot
    auto& ring = mem->frameRing;
    // CRITICAL FIX: Use acquire ordering to see consumer's readIndex updates
    uint32_t wIdx = ring.writeIndex.load(std::memory_order_acquire);
    uint32_t rIdx = ring.readIndex.load(std::memory_order_acquire);
    if ((uint32_t)(wIdx - rIdx) >= (uint32_t)FRAME_RING_SIZE) {
        ring.droppedFrames.fetch_add(1, std::memory_order_relaxed);
        mem->runtimeState.injectProducerMetadataFullDrops.fetch_add(1, std::memory_order_relaxed);
        LogFrameRingFull(wIdx, rIdx);
        return;
    }
    const bool ringWasEmpty = wIdx == mem->frameRing.ingestIndex.load(std::memory_order_acquire);

    uint32_t slot = wIdx % FRAME_RING_SIZE;
    if (slot < FRAME_RING_SIZE) {
        ring.slots[slot].timestamp = (int64_t)timestamp;
        ring.slots[slot].frameIndex = wIdx;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        ring.slots[slot].textureIndex = wIdx % g_PublishedTextureCount;  // Use tracked count
        ring.slots[slot].sourcePid = GetCurrentProcessId();
        ring.slots[slot].valid.store(1, std::memory_order_release);
    }

    // Increment write index to signal new frame
    ring.writeIndex.store(wIdx + 1, std::memory_order_release);
    LogPublishedFrame(wIdx + 1, rIdx, static_cast<int32_t>(ring.slots[slot].textureIndex), 0);
    if (ringWasEmpty) {
        g_IPCClient.SignalInjectFrameReady();
    }
}

// Get pointer to the ShmemBuffer for CPU staging
void* LayerIPC_GetShmemBuffer() {
    return g_IPCClient.GetShmem();
}

// Set dimensions for SHMEM mode (updates SharedMemoryLayout)
void LayerIPC_SetShmemDimensions(uint32_t width, uint32_t height, uint32_t format) {
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem)
        return;

    mem->SetWidth(width);
    mem->SetHeight(height);
    mem->SetFormat(format);
    LayerLog("Layer IPC: Set SHMEM dimensions %ux%u format=%u", width, height, format);
}

void LayerIPC_SetFence(HANDLE fenceHandle) {
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem)
        return;
    mem->SetFenceShareHandle((uint64_t)fenceHandle);
    LayerLog("Layer IPC: Set Fence Handle %p", fenceHandle);
}

// Signal frame ready for SHMEM mode (textureIndex >= 100)
void LayerIPC_SignalFrameReady(int32_t textureIndex, uint64_t fenceValue, int64_t timestampQpc) {
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem)
        return;

    auto& ring = mem->frameRing;
    // CRITICAL FIX: Use acquire ordering to see consumer's readIndex updates
    uint32_t wIdx = ring.writeIndex.load(std::memory_order_acquire);
    uint32_t rIdx = ring.readIndex.load(std::memory_order_acquire);

    // Check if ring buffer has space
    if ((uint32_t)(wIdx - rIdx) >= (uint32_t)FRAME_RING_SIZE) {
        ring.droppedFrames.fetch_add(1, std::memory_order_relaxed);
        mem->runtimeState.injectProducerMetadataFullDrops.fetch_add(1, std::memory_order_relaxed);
        LogFrameRingFull(wIdx, rIdx);
        return;
    }
    const bool ringWasEmpty = wIdx == mem->frameRing.ingestIndex.load(std::memory_order_acquire);

    uint32_t slot = wIdx % FRAME_RING_SIZE;

    if (timestampQpc <= 0) {
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        timestampQpc = qpc.QuadPart;
    }

    ring.slots[slot].timestamp = timestampQpc;
    ring.slots[slot].frameIndex = wIdx;
    ring.slots[slot].textureIndex = textureIndex;  // >= 100 indicates SHMEM mode
    ring.slots[slot].sourcePid = GetCurrentProcessId();
    ring.slots[slot].fenceValue = fenceValue;
    ring.slots[slot].valid.store(1, std::memory_order_release);

    ring.writeIndex.store(wIdx + 1, std::memory_order_release);
    LogPublishedFrame(wIdx + 1, rIdx, textureIndex, fenceValue);
    if (ringWasEmpty) {
        g_IPCClient.SignalInjectFrameReady();
    }
}

// IPC Logging implementation
// Replicates hook_common.cpp LogToFileAtomic/HookLog logic but purely over IPC
void LayerIPC_Log(const char* fmt, ...) {
    if (!g_IPCClient.GetSharedMem())
        return;

    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem->GetDebugLogging())
        return;  // Hook respects debug flag

    static char s_formatBuffer[4096];
    static char s_lineBuffer[8192];
    static std::mutex s_logMutex;

    std::lock_guard<std::mutex> lock(s_logMutex);

    va_list args;
    va_start(args, fmt);
    int len_buf = vsnprintf(s_formatBuffer, sizeof(s_formatBuffer), fmt, args);
    va_end(args);

    if (len_buf < 0)
        len_buf = 0;

    SYSTEMTIME st;
    GetLocalTime(&st);
    DWORD tid = GetCurrentThreadId();

    int len = snprintf(s_lineBuffer, sizeof(s_lineBuffer), "[%02d:%02d:%02d.%03d] [T:%04lX] [%s] %s", st.wHour,
                       st.wMinute, st.wSecond, st.wMilliseconds, (unsigned long)tid, g_ProcessName, s_formatBuffer);

    if (len > 0) {
        auto& logs = mem->logs;
        uint32_t wIdx = logs.writeIndex.load(std::memory_order_relaxed);
        bool reservedSlot = false;
        for (;;) {
            const uint32_t rIdx = logs.readIndex.load(std::memory_order_acquire);
            if ((uint32_t)(wIdx - rIdx) >= SharedMemoryLayout::LogBuffer::SLOT_COUNT) {
                logs.overflowCount.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            if (logs.writeIndex.compare_exchange_weak(wIdx, wIdx + 1, std::memory_order_acq_rel,
                                                       std::memory_order_relaxed)) {
                reservedSlot = true;
                break;
            }
        }

        if (reservedSlot) {
            uint32_t slotIdx = wIdx % SharedMemoryLayout::LogBuffer::SLOT_COUNT;
            char* slot = logs.buffer[slotIdx];
            // Uses fixed baseFilename "vulkan-layer.log" for identification on host
            // side
            snprintf(slot, SharedMemoryLayout::LogBuffer::SLOT_SIZE, "[vulkan-layer.log] %s", s_lineBuffer);
            logs.committed[slotIdx].store(1, std::memory_order_release);
        }
    }
}
