/**
 * VK_LAYER_CE_overlay - IPC Implementation
 *
 * Uses standard IPCClient to communicate with CaptureEngine.
 * Determines overlay config, styling, and publishes texture handles.
 */

#include <dxgiformat.h>
#include <vulkan/vulkan.h>
#include <cstdarg>
#include <cstdio>
#include "../common/ipc_client.h"
#include "layer_main.h"

// Global IPC Client
IPCClient g_IPCClient;
// Shared globals needed by system_metrics.cpp and other common files
IPCClient* g_IPC = &g_IPCClient;
char g_ProcessName[260] = "CaptureLayer";
// Dummy config to satisfy EarlyLog if it checks g_LocalConfig (though our shim doesn't)
// Actually system_metrics uses EarlyLog which we redirect.
// But does system_metrics use g_LocalConfig directly?
// No, it uses EarlyLog.

#include "../common/hook_common.h"  // For definitions if needed
// Define globals from hook_common.h that we are missing because we don't link hook_common.cpp
std::atomic<bool> g_ShuttingDown{false};
// g_GraphicsOverridesActive is definition in hook_common.cpp, but do we need it?
// Only if we use GetActiveGraphicsConfig. Overlay.cpp doesn't uses it.

// Log shims moved to layer_bridge.cpp

/**
 * Convert Vulkan format to DXGI format for shared resource export
 * Includes support for common formats used in games
 */
uint32_t VkFormatToDXGI(uint32_t vkFormat)
{
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
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:   // 65
            return DXGI_FORMAT_R10G10B10A2_UNORM;  // Remap to DXGI standard

        // BGRX formats (no alpha) - use BGRA for shared resources
        // Note: B8G8R8X8_UNORM not commonly used with Vulkan external memory
        // case VK_FORMAT_B8G8R8X8_UNORM:
        //     return DXGI_FORMAT_B8G8R8X8_UNORM;  // 88

        // RGB 565 (legacy)
        case VK_FORMAT_R5G6B5_UNORM_PACK16:   // 56
            return DXGI_FORMAT_B5G6R5_UNORM;  // 85 (swizzle)

        // Default fallback - BGRA8 is most common
        default:
            return DXGI_FORMAT_B8G8R8A8_UNORM;  // 87
    }
}

/**
 * Check if a Vulkan format is compatible with DXGI shared resources
 * Returns true if the format can be exported via external memory
 */
bool IsVkFormatCompatibleWithDXGI(VkFormat vkFormat)
{
    switch (vkFormat) {
        // Standard 8-bit formats - fully compatible
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_SRGB:
            return true;

        // HDR formats - compatible with D3D12
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
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
bool LayerIPC_Init()
{
    if (g_IPCClient.GetSharedMem()) return g_LayerState.whitelisted;

    // Get process name
    char fullPath[MAX_PATH];
    GetModuleFileNameA(NULL, fullPath, sizeof(fullPath));
    char* p = strrchr(fullPath, '\\');
    if (p)
        strcpy(g_ProcessName, p + 1);
    else
        strcpy(g_ProcessName, fullPath);

    // Connect to host
    if (g_IPCClient.Connect()) {
        LayerLog("Layer IPC: Connected to Host PID %d", g_IPCClient.GetSharedMem()->hostPID);

        // Check Whitelist Cache in Discovery Shared Memory
        // Replicating isProcessWhitelistedFast logic
        bool whitelisted = false;

        HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
        if (hDisc) {
            DiscoveryInfo* pDisc = (DiscoveryInfo*)MapViewOfFile(hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
            if (pDisc) {
                if (pDisc->magic == DISCOVERY_MAGIC) {
                    const char* pw = pDisc->processWhitelist;
                    const char* end = pDisc->processWhitelist + sizeof(pDisc->processWhitelist);

                    while (pw < end && *pw != '\0') {
                        if (_stricmp(g_ProcessName, pw) == 0) {
                            whitelisted = true;
                            break;
                        }
                        pw += strlen(pw) + 1;
                    }
                }
                UnmapViewOfFile(pDisc);
            }
            CloseHandle(hDisc);
        }

        g_LayerState.whitelisted = whitelisted;
        if (!whitelisted) {
            LayerLog("Layer IPC: Process '%s' NOT whitelisted. Layer dormant.", g_ProcessName);
            g_IPCClient.Disconnect();  // Don't stay connected if dormant
            return false;
        }

        LayerLog("Layer IPC: Process '%s' whitelisted. Layer active.", g_ProcessName);
        return true;
    }

    LayerLog("Layer IPC: Failed to connect to host. Layer dormant.");
    return false;
}

// Shutdown layer IPC
void LayerIPC_Shutdown()
{
    g_IPCClient.Disconnect();
    LayerLog("Layer IPC: Shutdown");
}

// Check if IPC is connected
bool LayerIPC_IsConnected() { return g_IPCClient.GetSharedMem() != nullptr; }

// Global texture count
static uint32_t g_PublishedTextureCount = 2;

// Update shared texture handles (called when swapchain created)
void LayerIPC_SetTextures(HANDLE* handles, uint32_t count, uint32_t width, uint32_t height, uint32_t format)
{
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem) return;

    g_PublishedTextureCount = (count > 0 && count <= 8) ? count : 2;

    // Write metadata
    mem->width = width;
    mem->height = height;
    mem->format = format;

    // Write handles (up to 8 supported by layout)
    uint32_t maxHandles = 8;
    for (uint32_t i = 0; i < count && i < maxHandles; i++) {
        mem->sharedHandles[i] = (uint64_t)handles[i];
    }

    LayerLog("Layer IPC: Published %d textures (%dx%d)", count, width, height);
}

// Update frame timing (called each present)
void LayerIPC_UpdateFrameTiming(uint64_t frameCount, float fps, float avgFps)
{
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem) return;

    // Update runtime state directly (atomics)
    // mem->runtimeState.currentFPS.store(fps, std::memory_order_relaxed); // Missing in CaptureState?
    // Wait, CaptureState has currentFPS (atomic double).
    mem->runtimeState.currentFPS.store(fps, std::memory_order_relaxed);
    mem->runtimeState.gameFPS.store(avgFps, std::memory_order_relaxed);
}

// Check if overlay should be shown
bool LayerIPC_ShouldShowOverlay()
{
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem) return true;
    return mem->overlayConfig.showOverlay;
}

// Check if capture is requested
bool LayerIPC_IsCaptureRequested()
{
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem) return false;
    return mem->runtimeState.isRecording.load(std::memory_order_relaxed);
}

// Set capture active status
void LayerIPC_SetCaptureActive(bool active)
{
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem) return;
    // ...
}

// Set overlay active status
void LayerIPC_SetOverlayActive(bool active)
{
    // No specific field in SharedMemoryLayout for "Overlay Active Ack"
}

void LayerIPC_SetLUID(int32_t low, int32_t high)
{
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem) return;

    mem->luidLowPart = low;
    mem->luidHighPart = high;
    // LayerLog("Layer IPC: Set LUID %08x:%08x", high, low);
}

uint32_t LayerIPC_GetWriteIndex()
{
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem) return 0;
    return mem->frameRing.writeIndex.load(std::memory_order_acquire);  // Use frameRing
}

void LayerIPC_IncrementWriteIndex(uint64_t timestamp)
{
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem) return;

    // Update timestamp for the current slot
    auto& ring = mem->frameRing;
    uint32_t wIdx = ring.writeIndex.load(std::memory_order_relaxed);

    uint32_t slot = wIdx % FRAME_RING_SIZE;
    if (slot < FRAME_RING_SIZE) {
        ring.slots[slot].timestamp = (int64_t)timestamp;
        ring.slots[slot].frameIndex = wIdx;
        ring.slots[slot].textureIndex = wIdx % g_PublishedTextureCount;  // Use tracked count
        ring.slots[slot].sourcePid = GetCurrentProcessId();
        ring.slots[slot].valid.store(1, std::memory_order_release);
    }

    // Increment write index to signal new frame
    ring.writeIndex.store(wIdx + 1, std::memory_order_release);
}

// Get pointer to the ShmemBuffer for CPU staging
void* LayerIPC_GetShmemBuffer() { return g_IPCClient.GetShmem(); }

// Set dimensions for SHMEM mode (updates SharedMemoryLayout)
void LayerIPC_SetShmemDimensions(uint32_t width, uint32_t height, uint32_t format)
{
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem) return;

    mem->width = width;
    mem->height = height;
    mem->format = format;
    LayerLog("Layer IPC: Set SHMEM dimensions %ux%u format=%u", width, height, format);
}

void LayerIPC_SetFence(HANDLE fenceHandle)
{
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem) return;
    mem->fenceShareHandle = (uint64_t)fenceHandle;
    LayerLog("Layer IPC: Set Fence Handle %p", fenceHandle);
}

// Signal frame ready for SHMEM mode (textureIndex >= 100)
void LayerIPC_SignalFrameReady(int32_t textureIndex, uint64_t fenceValue)
{
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem) return;

    auto& ring = mem->frameRing;
    uint32_t wIdx = ring.writeIndex.load(std::memory_order_relaxed);
    uint32_t rIdx = ring.readIndex.load(std::memory_order_acquire);

    // Check if ring buffer has space
    if ((uint32_t)(wIdx - rIdx) >= (uint32_t)FRAME_RING_SIZE) {
        ring.droppedFrames.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    uint32_t slot = wIdx % FRAME_RING_SIZE;

    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);

    ring.slots[slot].timestamp = qpc.QuadPart;
    ring.slots[slot].frameIndex = wIdx;
    ring.slots[slot].textureIndex = textureIndex;  // >= 100 indicates SHMEM mode
    ring.slots[slot].sourcePid = GetCurrentProcessId();
    ring.slots[slot].fenceValue = fenceValue;
    ring.slots[slot].valid.store(1, std::memory_order_release);

    ring.writeIndex.store(wIdx + 1, std::memory_order_release);
}

// IPC Logging implementation
// Replicates hook_common.cpp LogToFileAtomic/HookLog logic but purely over IPC
void LayerIPC_Log(const char* fmt, ...)
{
    if (!g_IPCClient.GetSharedMem()) return;

    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem->debugLogging) return;  // Hook respects debug flag

    static char s_formatBuffer[4096];
    static char s_lineBuffer[8192];
    static std::mutex s_logMutex;

    std::lock_guard<std::mutex> lock(s_logMutex);

    va_list args;
    va_start(args, fmt);
    int len_buf = vsnprintf(s_formatBuffer, sizeof(s_formatBuffer), fmt, args);
    va_end(args);

    if (len_buf < 0) len_buf = 0;

    SYSTEMTIME st;
    GetLocalTime(&st);
    DWORD tid = GetCurrentThreadId();

    int len = snprintf(s_lineBuffer, sizeof(s_lineBuffer), "[%02d:%02d:%02d.%03d] [T:%04lX] [%s] %s", st.wHour,
                       st.wMinute, st.wSecond, st.wMilliseconds, (unsigned long)tid, g_ProcessName, s_formatBuffer);

    if (len > 0) {
        auto& logs = mem->logs;
        uint32_t wIdx = logs.writeIndex.load(std::memory_order_relaxed);
        uint32_t rIdx = logs.readIndex.load(std::memory_order_acquire);

        if ((uint32_t)(wIdx - rIdx) < SharedMemoryLayout::LogBuffer::SLOT_COUNT) {
            char* slot = logs.buffer[wIdx % SharedMemoryLayout::LogBuffer::SLOT_COUNT];
            // Uses fixed baseFilename "vulkan-layer.log" for identification on host side
            snprintf(slot, SharedMemoryLayout::LogBuffer::SLOT_SIZE, "[vulkan-layer.log] %s", s_lineBuffer);
            logs.writeIndex.store(wIdx + 1, std::memory_order_release);
        } else {
            logs.overflowCount.fetch_add(1, std::memory_order_relaxed);
        }
    }
}
