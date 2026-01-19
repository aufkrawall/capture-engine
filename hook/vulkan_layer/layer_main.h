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
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <windows.h>
#include <string>
#include <mutex>

#include "vulkan_layer.h"
#include "../common/ipc_client.h"

// Forward declarations of IPC functions
bool LayerIPC_Init();
void LayerIPC_Shutdown();
bool LayerIPC_IsConnected();
void LayerIPC_SetTextures(HANDLE* handles, uint32_t count, uint32_t width, uint32_t height, uint32_t format);
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
