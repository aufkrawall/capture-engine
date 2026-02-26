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
#include <mutex>
#include <string>

#include "../common/ipc_client.h"
#include "../common/shared_defs.h"
#include "vulkan_layer.h"

// Forward declarations of IPC functions
bool LayerIPC_Init();
void LayerIPC_Shutdown();
bool LayerIPC_IsConnected();
void LayerIPC_SetTextures(HANDLE* handles, uint32_t count, uint32_t width, uint32_t height, uint32_t format);
void LayerIPC_SetFence(HANDLE fenceHandle);
void LayerIPC_SignalFrameReady(int32_t textureIndex, uint64_t fenceValue);
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

// Global IPC Client
extern IPCClient g_IPCClient;

// Layer State (legacy shim or integrated into VulkanLayerState)
// For now, let's keep a simplified version info here if needed
struct CELayerState {
    bool initialized = false;
    bool whitelisted = false;
    bool overlayEnabled = true;
    bool captureEnabled = false;
    std::string processName;
};
extern CELayerState g_LayerState;
