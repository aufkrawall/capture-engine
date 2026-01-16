/**
 * VK_LAYER_CE_overlay - IPC Implementation
 * 
 * Uses standard IPCClient to communicate with CaptureEngine.
 * Determines overlay config, styling, and publishes texture handles.
 */

#include "layer_main.h"
#include "../common/ipc_client.h"
#include <cstdio>
#include <cstdarg>

// Global IPC Client
IPCClient g_IPCClient;
// Shared globals needed by system_metrics.cpp and other common files
IPCClient* g_IPC = &g_IPCClient;
char g_ProcessName[260] = "CaptureLayer";
// Dummy config to satisfy EarlyLog if it checks g_LocalConfig (though our shim doesn't)
// Actually system_metrics uses EarlyLog which we redirect.
// But does system_metrics use g_LocalConfig directly?
// No, it uses EarlyLog.

#include "../common/hook_common.h" // For definitions if needed
// Define globals from hook_common.h that we are missing because we don't link hook_common.cpp
std::atomic<bool> g_ShuttingDown{false};
// g_GraphicsOverridesActive is definition in hook_common.cpp, but do we need it?
// Only if we use GetActiveGraphicsConfig. Overlay.cpp doesn't uses it.


// Shim for IPCClient and FGDetection logging
void EarlyLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    LayerLog("%s", buf);
}

void HookLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    LayerLog("%s", buf);
}

// Initialize layer IPC
bool LayerIPC_Init() {
    if (g_IPCClient.GetSharedMem()) return true;
    
    // Get process name
    GetModuleFileNameA(NULL, g_ProcessName, sizeof(g_ProcessName));
    char* p = strrchr(g_ProcessName, '\\');
    if (p) strcpy(g_ProcessName, p + 1);
    
    // Connect to host
    if (g_IPCClient.Connect()) {
        LayerLog("Layer IPC: Connected to Host PID %d", g_IPCClient.GetSharedMem()->hostPID);
        return true;
    }
    
    // Fallback: If host not running, we might still want to succeed init 
    // but we can't render overlay without config.
    LayerLog("Layer IPC: Failed to connect to host.");
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

// Update shared texture handles (called when swapchain created)
void LayerIPC_SetTextures(HANDLE* handles, uint32_t count, uint32_t width, uint32_t height, uint32_t format) {
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem) return;
    
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
void LayerIPC_UpdateFrameTiming(uint64_t frameCount, float fps, float avgFps) {
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem) return;
    
    // Update runtime state directly (atomics)
    mem->runtimeState.currentFPS.store(fps, std::memory_order_relaxed);
    mem->runtimeState.gameFPS.store(avgFps, std::memory_order_relaxed);
    
    // We don't have a reliable QPC sync here yet without hooking QueryPerformanceCounter
    // but overlay uses GetTickCount64 usually for recording time
}

// Check if overlay should be shown
bool LayerIPC_ShouldShowOverlay() {
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem) return true; // Default to showing if no IPC? Or default hidden? Default hidden is safer. 
                           // But if user just launched game without captureengine, they won't see overlay anyway.
                           // Actually standard behavior is to default to hidden if no config.
                           // But for testing 'TRUE' is better.
    return mem->overlayConfig.showOverlay;
}

// Check if capture is requested
bool LayerIPC_IsCaptureRequested() {
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem) return false;
    return mem->runtimeState.isRecording.load(std::memory_order_relaxed);
}

// Set capture active status
void LayerIPC_SetCaptureActive(bool active) {
    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem) return;
    // Layer doesn't strictly own 'isRecording', the host does. 
    // But we might want to flag that we are actively capturing?
    // 'runtimeState.isRecording' is authoritative from Host.
    // The previous implementation used a local flag?
    // Let's leave this as no-op for now unless we need to ack.
}

// Set overlay active status
void LayerIPC_SetOverlayActive(bool active) {
    // No specific field in SharedMemoryLayout for "Overlay Active Ack"
}

// IPC Logging implementation
// Replicates hook_common.cpp LogToFileAtomic/HookLog logic but purely over IPC
void LayerIPC_Log(const char* fmt, ...) {
    if (!g_IPCClient.GetSharedMem()) return;

    auto* mem = g_IPCClient.GetSharedMem();
    if (!mem->debugLogging) return; // Hook respects debug flag

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

    int len = snprintf(s_lineBuffer, sizeof(s_lineBuffer),
                        "[%02d:%02d:%02d.%03d] [T:%04X] [%s] %s",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                        tid, g_ProcessName, s_formatBuffer);
    
    if (len > 0) {
        auto& logs = mem->logs;
        uint32_t wIdx = logs.writeIndex.load(std::memory_order_relaxed);
        uint32_t rIdx = logs.readIndex.load(std::memory_order_acquire);
        
        if ((uint32_t)(wIdx - rIdx) < SharedMemoryLayout::LogBuffer::SLOT_COUNT) {
            char* slot = logs.buffer[wIdx % SharedMemoryLayout::LogBuffer::SLOT_COUNT];
            // Uses fixed baseFilename "Layer" for identification on host side
            snprintf(slot, SharedMemoryLayout::LogBuffer::SLOT_SIZE, "[Layer] %s", s_lineBuffer);
            logs.writeIndex.store(wIdx + 1, std::memory_order_release);
        } else {
            logs.overflowCount.fetch_add(1, std::memory_order_relaxed);
        }
    }
}
