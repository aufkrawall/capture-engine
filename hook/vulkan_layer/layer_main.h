/**
 * VK_LAYER_CE_overlay - CaptureEngine Vulkan Layer
 *
 * Main header for the layer.
 * Delegates to VulkanLayerState for state management.
 */

#pragma once

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>
#include <windows.h>
#include <winver.h>
#include <mutex>
#include <string>

#include "../common/ipc_client.h"
#include "../common/shared_defs.h"
#include "vulkan_layer.h"

// Forward declarations of IPC functions
bool LayerIPC_Init();
void LayerIPC_Shutdown();
bool LayerIPC_IsConnected();
void LayerIPC_StartHostLifecycleWatcher();
void LayerIPC_SetTextures(const HANDLE* handles, uint32_t count, uint32_t width, uint32_t height, uint32_t format);
void LayerIPC_SetFence(HANDLE fenceHandle);
void LayerIPC_SignalFrameReady(int32_t textureIndex, uint64_t fenceValue, int64_t timestampQpc = 0);
uint32_t VkFormatToDXGI(uint32_t vkFormat);
bool IsVkFormatCompatibleWithDXGI(VkFormat vkFormat);
void LayerIPC_UpdateFrameTiming(uint64_t frameCount, float fps, float avgFps);
bool LayerIPC_ShouldShowOverlay();
bool LayerIPC_IsCaptureRequested();
void LayerIPC_SetCaptureActive(bool active);
void LayerIPC_SetOverlayActive(bool active);
uint32_t LayerIPC_GetWriteIndex();
void LayerIPC_IncrementWriteIndex(uint64_t timestamp);
void LayerIPC_Log(const char* fmt, ...);
void LayerIPC_SetLUID(int32_t low, int32_t high);

// SHMEM mode functions (for Vulkan CPU staging)
void* LayerIPC_GetShmemBuffer();
void LayerIPC_SetShmemDimensions(uint32_t width, uint32_t height, uint32_t format);
// void LayerIPC_SignalFrameReady(int32_t textureIndex); // Replaced by overload
// above

// Logging
void LayerLog(const char* fmt, ...);

// ============================================================================
// DXVK / VKD3D-Proton Detection Utilities
// ============================================================================

// Returns true if any version-resource string field of the DLL at dllPath
// contains needle (case-insensitive). Used to fingerprint DXVK ("dxvk") and
// VKD3D-Proton ("vkd3d") beyond a mere path check.
static inline bool DllVersionStringContains(const char* dllPath, const char* needle) {
    DWORD dummy = 0;
    DWORD verSize = GetFileVersionInfoSizeA(dllPath, &dummy);
    if (verSize == 0)
        return false;

    std::string buf(verSize, '\0');
    if (!GetFileVersionInfoA(dllPath, 0, verSize, &buf[0]))
        return false;

    // Walk all language/codepage translations
    struct LangCP {
        WORD lang, cp;
    }* trans = nullptr;
    UINT transLen = 0;
    if (!VerQueryValueA(buf.data(), "\\VarFileInfo\\Translation", reinterpret_cast<void**>(&trans), &transLen) ||
        !trans || transLen == 0)
        return false;

    const char* fields[] = {"ProductName", "FileDescription", "InternalName", "OriginalFilename"};
    UINT count = transLen / sizeof(LangCP);
    size_t needleLen = strlen(needle);
    for (UINT i = 0; i < count; i++) {
        for (const char* field : fields) {
            char subkey[128];
            snprintf(subkey, sizeof(subkey), "\\StringFileInfo\\%04x%04x\\%s", trans[i].lang, trans[i].cp, field);
            char* val = nullptr;
            UINT len = 0;
            if (!VerQueryValueA(buf.data(), subkey, reinterpret_cast<void**>(&val), &len) || !val || len <= 1)
                continue;
            // Manual case-insensitive substring search (no std::transform needed)
            for (size_t j = 0; val[j] && j + needleLen <= len; j++) {
                if (_strnicmp(val + j, needle, needleLen) == 0)
                    return true;
            }
        }
    }
    return false;
}

// Returns true if dllName is currently loaded AND its path is outside System32
// (i.e. a non-system replacement DLL in the game directory).
static inline bool IsDllOutsideSystem32(const char* dllName) {
    HMODULE hMod = GetModuleHandleA(dllName);
    if (!hMod)
        return false;
    char loadedPath[MAX_PATH] = {};
    char systemDir[MAX_PATH] = {};
    GetModuleFileNameA(hMod, loadedPath, MAX_PATH);
    GetSystemDirectoryA(systemDir, MAX_PATH);
    size_t sysLen = strlen(systemDir);
    return !(_strnicmp(loadedPath, systemDir, sysLen) == 0 &&
             (loadedPath[sysLen] == '\\' || loadedPath[sysLen] == '/'));
}

// Returns true if dllName is loaded from outside System32 AND its version
// resource identifies it as originating from project identified by needle
// (e.g. "dxvk" for DXVK, "vkd3d" for VKD3D-Proton).
static inline bool IsDllFromProject(const char* dllName, const char* versionNeedle) {
    HMODULE hMod = GetModuleHandleA(dllName);
    if (!hMod)
        return false;
    char loadedPath[MAX_PATH] = {};
    char systemDir[MAX_PATH] = {};
    GetModuleFileNameA(hMod, loadedPath, MAX_PATH);
    GetSystemDirectoryA(systemDir, MAX_PATH);
    size_t sysLen = strlen(systemDir);
    if (_strnicmp(loadedPath, systemDir, sysLen) == 0 && (loadedPath[sysLen] == '\\' || loadedPath[sysLen] == '/'))
        return false;  // From System32 — not a replacement
    return DllVersionStringContains(loadedPath, versionNeedle);
}

// Global IPC Client
extern IPCClient g_IPCClient;

// Layer State (legacy shim or integrated into VulkanLayerState)
// For now, let's keep a simplified version info here if needed
struct CELayerState {
    bool initialized = false;
    std::atomic<bool> whitelisted{false};
    bool overlayEnabled = true;
    bool captureEnabled = false;
    std::string processName;
};
extern CELayerState g_LayerState;
inline std::atomic<uint64_t> g_LayerHostGeneration{0};
