#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include "graphics_hook.h"

namespace ce::ffx_api {
struct CallbackDescFrameGenerationPresent;
using PresentCallback = uint32_t (*)(CallbackDescFrameGenerationPresent*, void*);
}  // namespace ce::ffx_api

// =============================================================================
// DX12 Debug Logging Infrastructure
// =============================================================================
// These macros provide step-by-step logging for diagnosing DX12 overlay issues.
// Enable via config.ini: debug_logging = true

extern std::atomic<bool> g_DebugLoggingEnabled;

#define DX12_DEBUG(fmt, ...)                       \
    do {                                           \
        if (g_DebugLoggingEnabled) {               \
            HookLog("[DX12] " fmt, ##__VA_ARGS__); \
        }                                          \
    } while (0)

#define DX12_DEBUG_STEP(step, fmt, ...) DX12_DEBUG("[%s] " fmt, step, ##__VA_ARGS__)

#define DX12_DEBUG_HR(step, op, hr) DX12_DEBUG("[%s] %s hr=0x%08X (%s)", step, op, hr, SUCCEEDED(hr) ? "OK" : "FAILED")

#define DX12_DEBUG_PTR(step, name, ptr) DX12_DEBUG("[%s] %s=%p", step, name, ptr)

// Frame logging (throttled - every N frames)
#define DX12_DEBUG_FRAME(frameNum, fmt, ...)                                        \
    do {                                                                            \
        if (g_DebugLoggingEnabled && ((frameNum) % 300 == 0)) {                     \
            HookLog("[DX12:FRAME:%llu] " fmt, (uint64_t)(frameNum), ##__VA_ARGS__); \
        }                                                                           \
    } while (0)

// Error logging (always on)
#define DX12_ERROR(fmt, ...) HookLog("[DX12:ERROR] " fmt, ##__VA_ARGS__)

// Warning logging
#define DX12_WARN(fmt, ...)                             \
    do {                                                \
        if (g_DebugLoggingEnabled) {                    \
            HookLog("[DX12:WARN] " fmt, ##__VA_ARGS__); \
        }                                               \
    } while (0)

class DX12Hook : public GraphicsHook {
    std::vector<IUnknown*> trackedResources;
    std::recursive_mutex resourceMutex;

public:
    void Init() override;
    void Shutdown() override;
    void OnHostDisconnect() override;
    void EnsurePresentHooks();  // Called after D3D12 device creation is confirmed

    void TrackResource(IUnknown* res);
    void CleanupResources();

    // Frame classification for FG support
    bool IsRealFrame() const;
    void ClassifyFrame(int commandListCount);
};

extern DX12Hook* g_dx12HookInstance;

void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain);
void DX12_HookQueueVTable(ID3D12CommandQueue* queue);
void DX12_HookDeviceVTable(ID3D12Device* device);
void DX12_OnSwapchainResizeBegin();
void DX12_OnSwapchainResizeEnd();
void DX12_InvalidateSwapchain();
void DX12_SignalFSR4SwapchainRecreated();
void DX12_AdjustWrapperResizeDepth(int delta);
void DX12_StartTransitionCooldown();
void DX12_OnStreamlineFGStateChanged(bool active);
bool DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration();
void DX12_PrepareForStreamlineEnableTransition();
void DX12_RetainStreamlineStartupActivationSwapchain(IDXGISwapChain* swapchain, const char* source);
bool DX12_TryInvokePostSLStartupActivationCallback(const char* source, bool clearStartupWindow,
                                                   bool allowConfirmedWarmupService = false);
DWORD DX12_GetGamePresentThreadId();
void DX12_SetFFXPresentCallbackBridge(void* bridgeKey, ce::ffx_api::PresentCallback originalCallback,
                                      void* originalUserContext);
bool DX12_HasFFXPresentCallbackBridge(void* bridgeKey);
bool DX12_IsFFXPresentCallbackBridgeCallback(ce::ffx_api::PresentCallback callback);
void DX12_ClearFFXPresentCallbackBridge(void* bridgeKey);
void DX12_TryCacheRuntimeOwnedCallbackHDRStateFromSwapchain(void* swapChain);
void DX12_ServiceDeferredECLProbe();
void DX12_OnNativeFSRFrameGenerationConfigured(bool enabled, bool retainedPresentCallbackBridge = false);
void DX12_ClearNativeFSRRuntimeOwnedTeardown(const char* reason);
bool DX12_IsNativeFSRStartupConfigureArmingPending();
void DX12_ClearNativeFSRStartupConfigureArming(const char* reason);
void DX12_ClearOfficialFFXRuntimeOwnedPresentPathAssumption(const char* reason);
uint32_t DX12_RenderOverlayViaFFXPresentCallback(ce::ffx_api::CallbackDescFrameGenerationPresent* callbackDesc,
                                                 void* userCtx);
void RemoveGlobalVTableHooks();

extern "C" {
void DX12_SetCommandQueue(ID3D12CommandQueue* pQueue);
void DX12_AdjustWrapperResizeDepth_C(int delta);
}
