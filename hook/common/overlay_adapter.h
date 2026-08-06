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
#include <mutex>
#include "../../common/recording_indicator_policy.h"
#include "custom_overlay.h"
#include "ipc_client.h"
#include "performance_metrics.h"
#include "system_metrics.h"

// Backend type enum
enum class OverlayBackendType { None, DX8, DX9, DX10, DX11, DX12, OpenGL, Vulkan };

class OverlayAdapter {
public:
    OverlayAdapter() noexcept;
    ~OverlayAdapter();

    // Initialize with graphics API-specific parameters
    bool InitDX8(void* device);   // IDirect3DDevice8*
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
    bool InitCustom(CustomOverlay::RendererBackend* customBackend, OverlayBackendType type = OverlayBackendType::DX12);

    void Shutdown();
    void SetShutdownMode(bool skipDeviceRelease);  // Call before Shutdown when
                                                   // device is being destroyed
    bool IsInitialized() const {
        return initialized.load(std::memory_order_acquire);
    }
    OverlayBackendType GetBackendType() const {
        std::lock_guard<std::mutex> lock(stateMutex);
        return backendType;
    }

    // Get the backend for Vulkan-specific operations (SetRenderContext, etc.)
    CustomOverlay::RendererBackend* GetBackend() {
        std::lock_guard<std::mutex> lock(stateMutex);
        return backend;
    }

    // Set external data sources
    void SetMetrics(PerformanceMetrics* m) {
        std::lock_guard<std::mutex> lock(stateMutex);
        metrics = m;
    }
    void SetIPCClient(IPCClient* ipc) {
        std::lock_guard<std::mutex> lock(stateMutex);
        this->ipc = ipc;
    }
    void SetHwnd(void* hwnd) {
        std::lock_guard<std::mutex> lock(stateMutex);
        this->hwnd = hwnd;
        // Share a valid game window across ALL overlay adapters so an adapter that never gets SetHwnd
        // (e.g. the descriptor-free DX12 backend) resolves the game's DPI instead of falling back to
        // GetForegroundWindow() — which during startup can be a 96-DPI launcher/splash (overlay rendered
        // at 100% instead of the Windows 150% scale; session 20260624_004915).
        RememberDpiReferenceHwnd(hwnd);
    }
    // Remember a known-valid game window for cross-adapter DPI resolution (file-static; see .cpp).
    static void RememberDpiReferenceHwnd(void* hwnd);
    void SetGraphicsAPI(const char* api, const char* evidenceSource = nullptr);
    void SetReserveInactiveFGSpace(bool reserve);
    void InvalidateCachedFrame();
    void SetDroppedFrames(uint32_t count) {
        std::lock_guard<std::mutex> lock(stateMutex);
        droppedFrames = count;
    }
    void SetHDR(bool enabled, int rtvFormat = 0) {
        std::lock_guard<std::mutex> lock(stateMutex);
        isHDR = enabled;
        renderTargetFormat = rtvFormat;
    }

    // DX12-specific: Set render target before RenderOverlay
    void SetDX12RenderTarget(void* cmdList, void* rtvHandle);
    void SetDX12UploadSlotFence(void* fence, uint64_t guardValue);
    void SetDX12NextUploadSlot(int slot);
    bool PrimeDX12Resources(void* cmdList);
    bool HasPendingDX12Resources() const;

    // Render the overlay (called after BeginFrame in hook's render path)
    void RenderOverlay(int viewportWidth, int viewportHeight);

private:
    struct FrameLayoutSnapshot {
        uint32_t rowMask = 0;
        uint32_t rowCount = 0;
        bool fgActive = false;
        bool reserveFGSpace = false;
        int fgMultiplier = 1;
        char fgLabel[16] = "";
        float fgBaseFPS = 0.0f;
        float fgOutputFPS = 0.0f;
        ce::recording_indicator::State recordingState = ce::recording_indicator::State::Idle;
        // Media armed a screen-grab capture pipeline that records the composited screen,
        // so this overlay's recording-start status must not be drawn into the game frames
        // that capture is about to read. Cleared when the recording goes live.
        bool recordingStatusDark = false;
        bool recordingActive = false;
        bool recordingAudioOnly = false;
        uint64_t recordingSeconds = 0;
        bool showOverloadWarning = false;
        uint32_t recordingWarningKind = 0;
        uint32_t recordingTargetFps = 0;
        uint32_t recordingSustainFpsX100 = 0;
        bool notificationVisible = false;
        uint32_t notificationType = 0;
    };

    bool InitializeBackendLocked(CustomOverlay::RendererBackend* newBackend, OverlayBackendType type,
                                 const char* backendName, float dpiScale);
    void ApplyShutdownModeLocked(bool skipRelease);
    void DestroyResourcesLocked(bool shutdownRenderer);
    void ResetStateLocked();
    void RenderContent(int viewportWidth, int viewportHeight, const OverlayConfig& cfg,
                       const FrameLayoutSnapshot& frameLayout, bool refreshLayout);
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
    void* hdrPaperWhiteMonitor = nullptr;
    float resolvedHdrPaperWhiteNits = 203.0f;
    std::atomic<bool> initialized{false};
    bool skipDeviceRelease = false;  // When true, Shutdown won't release device refs (app is closing)
    mutable std::mutex stateMutex;

    // Cached values for throttled updates
    DWORD lastUpdateTime = 0;
    float cachedFPS = 0.0f;
    float cachedAvgFPS = 0.0f;
    float cached1PercentLow = 0.0f;
    float cached01PercentLow = 0.0f;
    SystemMetrics cachedSystemMetrics;

    // Encoder overload warning tracking (5-second display with extension)
    uint64_t lastEncoderOverloadTick = 0;
    uint32_t lastRecordingWarningKind = 0;

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
    bool reserveInactiveFGSpace = false;
    bool hasLastFrameLayout = false;
    FrameLayoutSnapshot lastFrameLayout = {};
    OverlayConfig lastRenderedConfig = {};
};
extern OverlayAdapter g_OverlayAdapter;
