/**
 * VK_LAYER_CE_overlay - Minimal IPC Client
 * 
 * Simplified IPC for the Vulkan layer to communicate with captureengine.
 * Shares texture handles and receives capture commands.
 */

#include "layer_main.h"
#include <windows.h>
#include <atomic>

// Shared memory structure (must match captureengine's version)
#pragma pack(push, 1)
struct LayerSharedMem {
    // Connection info
    uint32_t magic;                     // 'CEVL' for CaptureEngine Vulkan Layer
    uint32_t version;
    DWORD layerPid;
    
    // Texture export handles (for zero-copy capture)
    HANDLE textureHandles[4];           // Up to 4 swapchain images
    uint32_t textureCount;
    uint32_t width;
    uint32_t height;
    uint32_t format;                    // VkFormat
    
    // Frame timing
    uint64_t frameCount;
    uint64_t lastPresentTime;
    float currentFPS;
    float avgFPS;
    
    // Capture commands (from captureengine)
    uint32_t captureRequested;          // 0 = none, 1 = start, 2 = stop
    uint32_t overlayVisible;            // 1 = show overlay
    
    // Status
    uint32_t captureActive;
    uint32_t overlayActive;
};
#pragma pack(pop)

// IPC state
static HANDLE g_LayerSharedMemHandle = nullptr;
static LayerSharedMem* g_LayerSharedMem = nullptr;
static std::atomic<bool> g_IPCConnected{false};

// Create unique shared memory name for this process
static std::string GetSharedMemName() {
    char name[64];
    snprintf(name, sizeof(name), "Local\\CE_VK_LAYER_%08X", GetCurrentProcessId());
    return std::string(name);
}

// Initialize layer IPC
bool LayerIPC_Init() {
    if (g_IPCConnected) return true;
    
    std::string shmName = GetSharedMemName();
    
    // Create shared memory
    g_LayerSharedMemHandle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        sizeof(LayerSharedMem),
        shmName.c_str()
    );
    
    if (!g_LayerSharedMemHandle) {
        LayerLog("Layer IPC: Failed to create shared memory: %d", GetLastError());
        return false;
    }
    
    // Map view
    g_LayerSharedMem = (LayerSharedMem*)MapViewOfFile(
        g_LayerSharedMemHandle,
        FILE_MAP_ALL_ACCESS,
        0, 0,
        sizeof(LayerSharedMem)
    );
    
    if (!g_LayerSharedMem) {
        LayerLog("Layer IPC: Failed to map shared memory: %d", GetLastError());
        CloseHandle(g_LayerSharedMemHandle);
        g_LayerSharedMemHandle = nullptr;
        return false;
    }
    
    // Initialize
    memset(g_LayerSharedMem, 0, sizeof(LayerSharedMem));
    g_LayerSharedMem->magic = 0x4C564543; // 'CEVL'
    g_LayerSharedMem->version = 1;
    g_LayerSharedMem->layerPid = GetCurrentProcessId();
    g_LayerSharedMem->overlayVisible = 1; // Show overlay by default
    
    g_IPCConnected = true;
    LayerLog("Layer IPC: Initialized shared memory '%s'", shmName.c_str());
    
    return true;
}

// Shutdown layer IPC
void LayerIPC_Shutdown() {
    if (g_LayerSharedMem) {
        UnmapViewOfFile(g_LayerSharedMem);
        g_LayerSharedMem = nullptr;
    }
    if (g_LayerSharedMemHandle) {
        CloseHandle(g_LayerSharedMemHandle);
        g_LayerSharedMemHandle = nullptr;
    }
    g_IPCConnected = false;
    LayerLog("Layer IPC: Shutdown");
}

// Check if IPC is connected
bool LayerIPC_IsConnected() {
    return g_IPCConnected;
}

// Update shared texture handles (called when swapchain created)
void LayerIPC_SetTextures(HANDLE* handles, uint32_t count, uint32_t width, uint32_t height, uint32_t format) {
    if (!g_LayerSharedMem) return;
    
    g_LayerSharedMem->textureCount = count > 4 ? 4 : count;
    g_LayerSharedMem->width = width;
    g_LayerSharedMem->height = height;
    g_LayerSharedMem->format = format;
    
    for (uint32_t i = 0; i < g_LayerSharedMem->textureCount; i++) {
        g_LayerSharedMem->textureHandles[i] = handles[i];
    }
    
    LayerLog("Layer IPC: Published %d textures (%dx%d)", count, width, height);
}

// Update frame timing (called each present)
void LayerIPC_UpdateFrameTiming(uint64_t frameCount, float fps, float avgFps) {
    if (!g_LayerSharedMem) return;
    
    g_LayerSharedMem->frameCount = frameCount;
    g_LayerSharedMem->currentFPS = fps;
    g_LayerSharedMem->avgFPS = avgFps;
    QueryPerformanceCounter((LARGE_INTEGER*)&g_LayerSharedMem->lastPresentTime);
}

// Check if overlay should be shown (from captureengine command)
bool LayerIPC_ShouldShowOverlay() {
    if (!g_LayerSharedMem) return true; // Default to showing
    return g_LayerSharedMem->overlayVisible != 0;
}

// Check if capture is requested (from captureengine command)  
bool LayerIPC_IsCaptureRequested() {
    if (!g_LayerSharedMem) return false;
    return g_LayerSharedMem->captureRequested == 1;
}

// Set capture active status
void LayerIPC_SetCaptureActive(bool active) {
    if (!g_LayerSharedMem) return;
    g_LayerSharedMem->captureActive = active ? 1 : 0;
}

// Set overlay active status
void LayerIPC_SetOverlayActive(bool active) {
    if (!g_LayerSharedMem) return;
    g_LayerSharedMem->overlayActive = active ? 1 : 0;
}
