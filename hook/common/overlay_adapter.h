/**
 * Overlay Adapter
 *
 * Bridges the existing Overlay class with CustomOverlay backends.
 * This allows gradual migration from ImGui to CustomOverlay.
 *
 * Usage:
 * 1. Create appropriate backend based on graphics API
 * 2. Call InitCustomOverlay() instead of/after InitImGui()
 * 3. Call RenderCustomOverlay() instead of/after RenderUI()
 *
 * The adapter reads the same IPC config and metrics to render identical
 * content.
 */

#pragma once

#include <atomic>
#include "ipc_client.h"
#include "performance_metrics.h"
#include "system_metrics.h"

// Forward declarations for backends
namespace CustomOverlay {
class Renderer;
class RendererBackend;
}  // namespace CustomOverlay

// Backend type enum
enum class OverlayBackendType { None, DX9, DX10, DX11, DX12, OpenGL, Vulkan };

class OverlayAdapter {
public:
    OverlayAdapter();
    ~OverlayAdapter();

    // Initialize with graphics API-specific parameters
    bool InitDX9(void* device);   // IDirect3DDevice9*
    bool InitDX10(void* device);  // ID3D10Device*
    bool InitDX11(void* device,
                  void* context);  // ID3D11Device*, ID3D11DeviceContext*
    bool InitDX12(void* device, void* queue,
                  int rtvFormat);  // ID3D12Device*, ID3D12CommandQueue*, DXGI_FORMAT
    bool InitOpenGL();
    bool InitVulkan(void* device, void* physDevice, void* queue, uint32_t queueFamily, void* deviceDispatch = nullptr,
                    void* instanceDispatch = nullptr);  // VkDevice, VkPhysicalDevice, VkQueue,
                                                        // DeviceDispatch*, InstanceDispatch*

    // Initialize with a pre-created backend (for descriptor-free DX12, etc.)
    bool InitCustom(CustomOverlay::RendererBackend* customBackend,
                    OverlayBackendType type = OverlayBackendType::DX12);

    void Shutdown();
    void SetShutdownMode(bool skipDeviceRelease);  // Call before Shutdown when
                                                   // device is being destroyed
    bool IsInitialized() const {
        return initialized;
    }
    OverlayBackendType GetBackendType() const {
        return backendType;
    }

    // Get the backend for Vulkan-specific operations (SetRenderContext, etc.)
    CustomOverlay::RendererBackend* GetBackend() {
        return backend;
    }

    // Set external data sources
    void SetMetrics(PerformanceMetrics* m) {
        metrics = m;
    }
    void SetIPCClient(IPCClient* ipc) {
        this->ipc = ipc;
    }
    void SetHwnd(void* hwnd) {
        this->hwnd = hwnd;
    }
    void SetGraphicsAPI(const char* api);
    void SetDroppedFrames(uint32_t count) {
        droppedFrames = count;
    }
    void SetHDR(bool enabled, int rtvFormat = 0) {
        isHDR = enabled;
        renderTargetFormat = rtvFormat;
    }

    // DX12-specific: Set render target before RenderOverlay
    void SetDX12RenderTarget(void* cmdList, void* rtvHandle);
    bool PrimeDX12Resources(void* cmdList);
    bool HasPendingDX12Resources() const;

    // Render the overlay (called after BeginFrame in hook's render path)
    void RenderOverlay(int viewportWidth, int viewportHeight);

private:
    void RenderContent(int viewportWidth, int viewportHeight, const OverlayConfig& cfg, bool shouldUpdate);
    uint32_t GetLoadColor(float load);

    CustomOverlay::Renderer* renderer = nullptr;
    CustomOverlay::RendererBackend* backend = nullptr;
    OverlayBackendType backendType = OverlayBackendType::None;

    PerformanceMetrics* metrics = nullptr;
    IPCClient* ipc = nullptr;
    void* hwnd = nullptr;
    char graphicsAPI[32] = "";
    uint32_t droppedFrames = 0;
    bool isHDR = false;
    int renderTargetFormat = 0;
    std::atomic<bool> initialized{false};
    bool skipDeviceRelease = false;  // When true, Shutdown won't release device refs (app is closing)

    // Cached values for throttled updates
    DWORD lastUpdateTime = 0;
    float cachedFPS = 0.0f;
    float cachedAvgFPS = 0.0f;
    float cached1PercentLow = 0.0f;
    float cached01PercentLow = 0.0f;
    SystemMetrics cachedSystemMetrics;

    // Encoder overload warning tracking (5-second display with extension)
    uint64_t lastEncoderOverloadTick = 0;

    // Cached layout measurement (recomputed only on content updates, avoids
    // per-frame snprintf+CalcTextSize overhead)
    float cachedContentWidth = 0.0f;
    bool layoutDirty = true;

    // Throttled frame time display values (updated every 2 seconds)
    DWORD lastMaxFrameTimeUpdateTime = 0;
    float cachedMaxFrameTime = 0.0f;
    float cachedAvgFrameTimeForColor = 0.0f;

    // Cached overlay draw data can be reused between meaningful content updates.
    int lastViewportWidth = 0;
    int lastViewportHeight = 0;
    bool hasCachedFrame = false;
    bool hasRenderedConfig = false;
    bool lastFGActive = false;
    bool lastRecordingActive = false;
    bool lastShowOverloadWarning = false;
    uint64_t lastRecordingSeconds = 0;
    OverlayConfig lastRenderedConfig = {};
};
extern OverlayAdapter g_OverlayAdapter;
