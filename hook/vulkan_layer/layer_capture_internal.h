#pragma once

struct D3D11InteropDevice;

struct SharedTextureEntry;

struct VulkanCaptureState;

#include <d3d11.h>

#include <d3d11_1.h>

#include <d3d11_4.h>

#include <dxgi.h>

#include <dxgi1_4.h>

#include <wrl/client.h>

#include <algorithm>

#include <array>

#include <vector>

#include "../../common/capture_base.h"

#include "../../common/secure_dll_loading.h"

#include "../../common/shared_defs.h"

#include "../common/hook_common.h"

#include "../common/screenshot_hook.h"

#include "layer_main.h"

#include "vulkan_layer.h"

#include "vulkan_presentation_color.h"

using Microsoft::WRL::ComPtr;

enum class VulkanCaptureInteropMode {
    kNative,
    kDxvkD3D11,
    kDxvkD3D9,
};

void InitializeCapture(VkDevice device, VkSwapchainKHR swapchain, VkFormat format, VkColorSpaceKHR colorSpace, VkExtent2D extent, uint32_t imageCount);

void NoteCaptureSwapchainImagePresented(VkDevice device, VkSwapchainKHR swapchain, uint32_t imageIndex);

void RetireCaptureSwapchain(VkDevice device, VkSwapchainKHR swapchain);

void CleanupCapture(VkDevice device);

VkSemaphore GetCaptureSemaphore(VkDevice device, VkSwapchainKHR swapchain, uint32_t imageIndex);

bool CaptureFrame(VkDevice device, VkSwapchainKHR swapchain, VkQueue queue, VkImage srcImage, const VkSemaphore* waitSemaphores, uint32_t waitSemaphoreCount, VkSemaphore signalSemaphore);

bool TakeVulkanScreenshot(DeviceDispatch* disp, VkDevice device, VkQueue queue, VkImage srcImage, uint32_t width, uint32_t height, VkFormat format, VkColorSpaceKHR colorSpace, const VkSemaphore* waitSemaphores, uint32_t waitSemaphoreCount, SharedMemoryLayout* sharedMemory, uint64_t requestId);

struct D3D11InteropDevice {
    uint64_t luidKey = 0;  // Combined LUID as single key
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    bool valid = false;
};

struct SharedTextureEntry {
    VkDevice vkDevice = VK_NULL_HANDLE;
    uint64_t luidKey = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t vkFormat = 0;

    std::vector<ID3D11Texture2D*> textures;  // 4 textures for double/triple buffering
    std::vector<HANDLE> textureHandles;      // Vulkan import handles (KMT or NT)
    bool textureHandlesAreNt = false;
    std::vector<VkImage> vkImages;           // Vulkan imported images
    std::vector<VkDeviceMemory> vkMemories;  // Vulkan imported memories

    // IPC relay: separate shared textures for encoder (not Vulkan-imported)
    // When KMT handles are used for Vulkan import, they become un-openable cross-process.
    // These relay textures use independent KMT handles that remain openable by the encoder.
    std::vector<ID3D11Texture2D*> ipcTextures;
    std::vector<HANDLE> ipcHandles;
    bool ipcHandlesAreNt = false;
    bool hasIpcRelay = false;

    bool valid = false;
};

// Global caches
inline std::mutex layer_capture_g_InteropMutex;

inline std::vector<D3D11InteropDevice> layer_capture_g_D3D11Devices;

inline std::vector<SharedTextureEntry> layer_capture_g_TextureCache;

bool SelectImportedWin32MemoryType(DeviceDispatch* disp, VkDevice device,
                                          VkExternalMemoryHandleTypeFlagBits handleType, HANDLE handle,
                                          uint32_t imageMemoryTypeBits,
                                          const VkPhysicalDeviceMemoryProperties& memoryProperties,
                                          uint32_t* layer_capture_memoryTypeIndex);bool CreateD3D11InteropDevice(IDXGIAdapter* adapter, ID3D11Device** ppDevice, ID3D11DeviceContext** ppContext);uint64_t MakeLuidKey(const LUID& luid);bool IsSpecificDxvkWrapperLoaded(const char* dllName);VulkanCaptureInteropMode DetectVulkanInteropMode();const char* VulkanInteropModeToString(VulkanCaptureInteropMode mode);D3D11InteropDevice* GetOrCreateD3D11Device(const LUID& luid);uint32_t VkFormatToDXGI(VkFormat vkFormat);

// Normalize SRGB swapchain formats to their UNORM equivalents for D3D11 interop.
// D3D11 KMT textures are created as UNORM (SRGB isn't needed for byte-level copies),
// so the VkImage used to import them must also be UNORM for a valid format match.
// vkCmdCopyImage between compatible 32-bit format classes (SRGB↔UNORM) is spec-valid.
VkFormat NormalizeVkFormat(VkFormat fmt);bool CreateSharedTextures(D3D11InteropDevice* interopDev, VkDevice vkDev, DeviceDispatch* disp,
                                 VkPhysicalDevice physDev, const LUID& luid, uint32_t width, uint32_t height,
                                 uint32_t vkFormat, SharedTextureEntry& entry);

// Create Vulkan-native images with D3D11_TEXTURE NT export for cross-process sharing.
// Used when DXVK is active: bypasses D3D11 entirely since DXVK's D3D11 produces
// internal handles that native D3D11 in the encoder can't open.
// Uses NT handles (not KMT) because KMT handles from vkGetMemoryWin32HandleKHR are raw
// WDDM allocation handles without D3D11 resource metadata - D3D11's OpenSharedResource
// returns E_INVALIDARG for them. NT handles via D3D11_TEXTURE_BIT carry proper resource
// metadata and are openable by D3D11's OpenSharedResource1 after DuplicateHandle.
bool CreateVulkanNativeSharedTextures(VkDevice vkDev, DeviceDispatch* disp, VkPhysicalDevice physDev,
                                             const LUID& luid, uint32_t width, uint32_t height, uint32_t vkFormat,
                                             SharedTextureEntry& entry);SharedTextureEntry* GetOrCreateSharedTextures(VkDevice vkDev, DeviceDispatch* disp, VkPhysicalDevice physDev,
                                                     const LUID& luid, uint32_t width, uint32_t height,
                                                     uint32_t vkFormat);void DestroySharedTextureEntryResources(SharedTextureEntry& entry, DeviceDispatch* disp);

struct VulkanCaptureState {
    bool initialized = false;
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    uint64_t luidKey = 0;
    uint32_t captureWidth = 0;
    uint32_t captureHeight = 0;
    uint32_t captureFormat = 0;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

    struct CommandResources {
        VkCommandPool pool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> buffers;
    };
    std::unordered_map<uint32_t, CommandResources> commandResourcesByQueueFamily;
    std::vector<VkFence> copyFences;
    std::vector<VkSemaphore> signalSemaphores;
    std::vector<bool> presentedImages;
    std::array<uint64_t, SHARED_TEXTURE_SLOT_COUNT> relayCompletionValues{};
    std::array<bool, SHARED_TEXTURE_SLOT_COUNT> sharedImageInitialized{};
    bool relayCompletionUnknown = false;

    // Cross-API Synchronization
    VkSemaphore timelineSemaphore = VK_NULL_HANDLE;
    uint64_t currentFenceValue = 0;
    uint64_t captureFrameCounter = 0;  // Monotonic counter for slot rotation
    HANDLE sharedFenceHandle = NULL;

    // D3D11 relay: fence and context for IPC relay copy (KMT→NT)
    ID3D11Fence* d3d11Fence = nullptr;
    ID3D11DeviceContext4* d3d11Context4 = nullptr;

    // D3D11-native shared fence for cross-process IPC with encoder
    // The Vulkan opaque fence (d3d11Fence) works in-process but can't be opened cross-process.
    // This separate D3D11 fence has a standard shared handle the encoder can open.
    ID3D11Fence* d3d11IpcFence = nullptr;
    HANDLE ipcFenceHandle = nullptr;

    uint64_t nextEncoderImportRetryFrame = 0;
    VulkanCaptureInteropMode interopMode = VulkanCaptureInteropMode::kNative;
};

inline std::mutex layer_capture_g_CaptureMutex;

inline std::unordered_map<VkDevice, VulkanCaptureState> layer_capture_g_CaptureStates;

inline std::vector<VulkanCaptureState> layer_capture_g_RetiredCaptureStates;bool CaptureStateCopiesComplete(const VulkanCaptureState& state, DeviceDispatch* disp);void DestroyCaptureStateResources(VulkanCaptureState& state, DeviceDispatch* disp);bool SelectImportedWin32MemoryType(DeviceDispatch* disp, VkDevice device,
                                          VkExternalMemoryHandleTypeFlagBits handleType, HANDLE handle,
                                          uint32_t imageMemoryTypeBits,
                                          const VkPhysicalDeviceMemoryProperties& memoryProperties,
                                          uint32_t* layer_capture_memoryTypeIndex);VulkanCaptureState::CommandResources* EnsureCaptureCommandResources(VulkanCaptureState& state,
                                                                           DeviceDispatch* disp, VkDevice device,
                                                                           uint32_t queueFamilyIndex);

// Helper to get LUID from Vulkan Physical Device
bool GetLUIDFromPhysicalDevice(VkPhysicalDevice physDev, LUID* outLuid);bool ImportEncoderKmtTextures(VkDevice device, DeviceDispatch* disp, uint64_t luidKey, uint32_t width,
                                     uint32_t height, uint32_t vkFormat, SharedMemoryLayout* mem,
                                     SharedTextureEntry* outEntry,
                                     HANDLE outKmtHandles[ENCODER_TEXTURE_SLOT_COUNT]);
