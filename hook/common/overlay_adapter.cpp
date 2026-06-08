/**
 * Overlay Adapter Implementation
 *
 * Creates the appropriate backend and renders overlay using CustomOverlay.
 */

#include "overlay_adapter.h"
#include "custom_font.h"
#include "custom_overlay.h"
#include "fg_detection.h"
#include "hook_common.h"
#include "perf_logger.h"
#include "../../common/capture_pipeline_policy.h"

#include <cfloat>  // FLT_MAX

// Include backends based on build context
// VK_LAYER_CE_OVERLAY is defined when building the Vulkan layer
#ifndef VK_LAYER_CE_OVERLAY
// Full backends for hook DLL
#include "custom_overlay_dx10.h"
#include "custom_overlay_dx11.h"
#include "custom_overlay_dx12.h"
#include "custom_overlay_dx8.h"
#include "custom_overlay_dx9.h"
#include "custom_overlay_gl.h"
#endif
#include "custom_overlay_vk.h"

#include <cstdio>
#include <cstring>
#include <memory>

// Global adapter instance
OverlayAdapter g_OverlayAdapter;

namespace {
bool OverlayConfigEquals(const OverlayConfig& a, const OverlayConfig& b) {
    return a.showOverlay == b.showOverlay && a.captureIncludeOverlay == b.captureIncludeOverlay &&
           a.screenshotIncludeOverlay == b.screenshotIncludeOverlay && a.showFPS == b.showFPS &&
           a.showFrameTime == b.showFrameTime && a.showCPU == b.showCPU && a.showGPU == b.showGPU &&
           a.showRAM == b.showRAM && a.showVRAM == b.showVRAM && a.showRecording == b.showRecording &&
           a.showFG == b.showFG && a.position == b.position && a.padding == b.padding &&
           a.compactMode == b.compactMode && a.horizontalMode == b.horizontalMode && a.fontSize == b.fontSize &&
           a.roundedCorners == b.roundedCorners && a.bgColor == b.bgColor && a.bgAlpha == b.bgAlpha &&
           a.fpsColor == b.fpsColor && a.cpuColor == b.cpuColor && a.gpuColor == b.gpuColor &&
           a.ramColor == b.ramColor && a.vramColor == b.vramColor && a.frametimeColor == b.frametimeColor &&
           a.textColor == b.textColor && a.textOutline == b.textOutline && a.textOutlineColor == b.textOutlineColor &&
           a.textOutlineThickness == b.textOutlineThickness && a.loadColorLow == b.loadColorLow &&
           a.loadColorMed == b.loadColorMed && a.loadColorHigh == b.loadColorHigh &&
           a.textUpdateInterval == b.textUpdateInterval && a.hdrPaperWhite == b.hdrPaperWhite;
}

class ScopedThreadDpiAwareness {
public:
    ScopedThreadDpiAwareness() {
        HMODULE user32 = GetModuleHandleA("user32.dll");
        if (!user32) {
            user32 = LoadLibraryA("user32.dll");
        }

        setThreadDpiAwarenessContext_ =
            reinterpret_cast<SetThreadDpiAwarenessContextFn>(GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
        if (setThreadDpiAwarenessContext_) {
            oldContext_ = setThreadDpiAwarenessContext_(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    ~ScopedThreadDpiAwareness() {
        if (setThreadDpiAwarenessContext_ && oldContext_) {
            setThreadDpiAwarenessContext_(oldContext_);
        }
    }

private:
    using SetThreadDpiAwarenessContextFn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT value);

    SetThreadDpiAwarenessContextFn setThreadDpiAwarenessContext_ = nullptr;
    DPI_AWARENESS_CONTEXT oldContext_ = nullptr;
};

std::string FormatEncoderOverloadLabel(uint32_t sustainFpsX100, uint32_t targetFps) {
    const double sustainFps = static_cast<double>(sustainFpsX100) / 100.0;
    if (targetFps == 0 || sustainFpsX100 == 0) {
        return "!ENCODER OVERLOAD!";
    }

    char buffer[64];
    const double ratio = sustainFps / static_cast<double>(targetFps);
    if (ratio >= 0.95) {
        std::snprintf(buffer, sizeof(buffer), "!ENC LIMIT %.1f/%u!", sustainFps, targetFps);
    } else if (ratio >= 0.80) {
        std::snprintf(buffer, sizeof(buffer), "!ENC OVER %.1f/%u!", sustainFps, targetFps);
    } else {
        std::snprintf(buffer, sizeof(buffer), "!ENC SEVERE %.1f/%u!", sustainFps, targetFps);
    }
    return buffer;
}

}  // namespace

// Helper to detect Windows DPI scaling
static float GetWindowsDpiScale(HWND targetHwnd) {
    // Prefer the target game window if available.
    HWND hwnd = targetHwnd;
    if (!IsWindow(hwnd)) {
        hwnd = GetForegroundWindow();
    }
    if (!IsWindow(hwnd)) {
        hwnd = GetDesktopWindow();
    }

    ScopedThreadDpiAwareness dpiAwarenessScope;

    // Try GetDpiForWindow first (Windows 10 1607+)
    typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
    static GetDpiForWindowFn getDpiForWindow =
        (GetDpiForWindowFn)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow");

    if (getDpiForWindow) {
        UINT dpi = getDpiForWindow(hwnd);
        if (dpi > 0) {
            return dpi / 96.0f;
        }
    }

    // Fallback to GetDeviceCaps
    HDC hdc = GetDC(hwnd);
    if (hdc) {
        int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(hwnd, hdc);
        if (dpiX > 0) {
            return dpiX / 96.0f;
        }
    }

    // Default to 1.0 if detection fails
    return 1.0f;
}

OverlayAdapter::OverlayAdapter() {
    memset(graphicsAPI, 0, sizeof(graphicsAPI));
}

OverlayAdapter::~OverlayAdapter() {
    // During process termination, skip all GPU resource cleanup to avoid
    // driver access violations (D3D12/NVIDIA may be partially torn down).
#ifndef VK_LAYER_CE_OVERLAY
    if (IsProcessTerminating())
        return;
#endif
    std::lock_guard<std::mutex> lock(stateMutex);
    if (skipDeviceRelease) {
        DestroyResourcesLocked(false);
        ResetStateLocked();
        return;
    }
    DestroyResourcesLocked(true);
    ResetStateLocked();
}

void OverlayAdapter::SetGraphicsAPI(const char* api) {
    std::lock_guard<std::mutex> lock(stateMutex);
    strncpy(graphicsAPI, api, sizeof(graphicsAPI) - 1);
    graphicsAPI[sizeof(graphicsAPI) - 1] = '\0';
    layoutDirty = true;
}

void OverlayAdapter::SetReserveInactiveFGSpace(bool reserve) {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (reserveInactiveFGSpace != reserve) {
        reserveInactiveFGSpace = reserve;
        layoutDirty = true;
        hasCachedFrame = false;
    }
}

void OverlayAdapter::InvalidateCachedFrame() {
    std::lock_guard<std::mutex> lock(stateMutex);
    hasCachedFrame = false;
    layoutDirty = true;
}

bool OverlayAdapter::InitializeBackendLocked(CustomOverlay::RendererBackend* newBackend, OverlayBackendType type,
                                             const char* backendName, float dpiScale) {
    if (!newBackend) {
        return false;
    }

    if (initialized.load(std::memory_order_acquire)) {
        delete newBackend;
        return true;
    }

    auto newRenderer = std::make_unique<CustomOverlay::Renderer>();
    if (!newRenderer->Initialize(newBackend, dpiScale)) {
        HookLogImportant("[Overlay] Init%s: Renderer::Initialize FAILED (dpiScale=%.2f)", backendName, dpiScale);
        delete newBackend;
        return false;
    }

    renderer = newRenderer.release();
    backend = newBackend;
    backendType = type;
    initialized.store(true, std::memory_order_release);
    HookLogImportant("[Overlay] %s backend initialized successfully (dpiScale=%.2f)", backendName, dpiScale);
    return true;
}

void OverlayAdapter::ApplyShutdownModeLocked(bool skipRelease) {
    skipDeviceRelease = skipRelease;
    if (renderer) {
        renderer->SetSkipDeviceRelease(skipRelease);
    }
#ifndef VK_LAYER_CE_OVERLAY
    if (backend && backendType == OverlayBackendType::DX11) {
        auto dx11Backend = static_cast<CustomOverlay::DX11Backend*>(backend);
        dx11Backend->SetSkipRelease(skipRelease);
    }
#endif
}

void OverlayAdapter::DestroyResourcesLocked(bool shutdownRenderer) {
    ApplyShutdownModeLocked(skipDeviceRelease);

    if (renderer) {
        if (shutdownRenderer) {
            renderer->Shutdown();
        }
        delete renderer;
        renderer = nullptr;
    }

    if (backend) {
        delete backend;
        backend = nullptr;
    }
}

void OverlayAdapter::ResetStateLocked() {
    backendType = OverlayBackendType::None;
    initialized.store(false, std::memory_order_release);
    hasCachedFrame = false;
    hasRenderedConfig = false;
    lastViewportWidth = 0;
    lastViewportHeight = 0;
    lastUpdateTime = 0;
    lastFGActive = false;
    reserveInactiveFGSpace = false;
    lastReserveInactiveFGSpace = false;
    lastRecordingActive = false;
    lastShowOverloadWarning = false;
    lastRecordingSeconds = 0;
    lastEncoderOverloadTick = 0;
    lastRecordingWarningKind = ce::capture_policy::kOverlayWarningNone;
    lastRenderedRecordingWarningKind = ce::capture_policy::kOverlayWarningNone;
    layoutDirty = true;
    memset(&lastRenderedConfig, 0, sizeof(lastRenderedConfig));
}

bool OverlayAdapter::InitDX9(void* device) {
#ifndef VK_LAYER_CE_OVERLAY
    std::lock_guard<std::mutex> lock(stateMutex);
    if (initialized.load(std::memory_order_acquire))
        return true;
    if (!device) {
        HookLogImportant("[Overlay] InitDX9 failed: null device pointer");
        return false;
    }

    HookLogImportant("[Overlay] Initializing DX9 backend (device=%p)", device);
    float dpiScale = GetWindowsDpiScale(reinterpret_cast<HWND>(hwnd));
    return InitializeBackendLocked(new CustomOverlay::DX9Backend((IDirect3DDevice9*)device), OverlayBackendType::DX9,
                                   "DX9", dpiScale);
#endif
    return true;
}

bool OverlayAdapter::InitDX8(void* device) {
#ifndef VK_LAYER_CE_OVERLAY
    std::lock_guard<std::mutex> lock(stateMutex);
    if (initialized.load(std::memory_order_acquire))
        return true;
    if (!device) {
        HookLogImportant("[Overlay] InitDX8 failed: null device pointer");
        return false;
    }

    HookLogImportant("[Overlay] Initializing DX8 backend (device=%p)", device);
    float dpiScale = GetWindowsDpiScale(reinterpret_cast<HWND>(hwnd));
    return InitializeBackendLocked(new CustomOverlay::DX8Backend((IDirect3DDevice8*)device), OverlayBackendType::DX8,
                                   "DX8", dpiScale);
#endif
    return true;
}

bool OverlayAdapter::InitDX10(void* device) {
#ifndef VK_LAYER_CE_OVERLAY
    std::lock_guard<std::mutex> lock(stateMutex);
    if (initialized.load(std::memory_order_acquire))
        return true;
    if (!device) {
        HookLogImportant("[Overlay] InitDX10 failed: null device pointer");
        return false;
    }

    HookLogImportant("[Overlay] Initializing DX10 backend (device=%p)", device);
    float dpiScale = GetWindowsDpiScale(reinterpret_cast<HWND>(hwnd));
    return InitializeBackendLocked(new CustomOverlay::DX10Backend((ID3D10Device*)device), OverlayBackendType::DX10,
                                   "DX10", dpiScale);
#endif
    return true;
}

bool OverlayAdapter::InitDX11(void* device, void* context) {
#ifndef VK_LAYER_CE_OVERLAY
    std::lock_guard<std::mutex> lock(stateMutex);
    if (initialized.load(std::memory_order_acquire))
        return true;
    if (!device || !context) {
        HookLogImportant("[Overlay] InitDX11 failed: null device=%p context=%p", device, context);
        return false;
    }

    HookLogImportant("[Overlay] Initializing DX11 backend (device=%p, context=%p)", device, context);
    float dpiScale = GetWindowsDpiScale(reinterpret_cast<HWND>(hwnd));
    return InitializeBackendLocked(new CustomOverlay::DX11Backend((ID3D11Device*)device, (ID3D11DeviceContext*)context),
                                   OverlayBackendType::DX11, "DX11", dpiScale);
#endif
    return true;
}

bool OverlayAdapter::InitDX12(void* device, void* queue, int rtvFormat) {
#ifndef VK_LAYER_CE_OVERLAY
    std::lock_guard<std::mutex> lock(stateMutex);
    if (initialized.load(std::memory_order_acquire))
        return true;
    if (!device || !queue) {
        HookLogImportant("[Overlay] InitDX12 failed: null device=%p queue=%p", device, queue);
        return false;
    }

    HookLogImportant("[Overlay] Initializing DX12 backend (device=%p, queue=%p, fmt=%d)", device, queue, rtvFormat);
    float dpiScale = GetWindowsDpiScale(reinterpret_cast<HWND>(hwnd));
    return InitializeBackendLocked(
        new CustomOverlay::DX12Backend((ID3D12Device*)device, (ID3D12CommandQueue*)queue, (DXGI_FORMAT)rtvFormat),
        OverlayBackendType::DX12, "DX12", dpiScale);
#endif
    return true;
}

bool OverlayAdapter::InitOpenGL() {
#ifndef VK_LAYER_CE_OVERLAY
    std::lock_guard<std::mutex> lock(stateMutex);
    if (initialized.load(std::memory_order_acquire))
        return true;

    HookLogImportant("[Overlay] Initializing OpenGL backend");
    float dpiScale = GetWindowsDpiScale(reinterpret_cast<HWND>(hwnd));
    return InitializeBackendLocked(new CustomOverlay::OpenGLBackend(), OverlayBackendType::OpenGL, "OpenGL", dpiScale);
#endif
    return true;
}

bool OverlayAdapter::InitVulkan(void* device, void* physDevice, void* queue, uint32_t queueFamily, void* deviceDispatch,
                                void* instanceDispatch) {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (initialized.load(std::memory_order_acquire))
        return true;
    if (!device) {
        HookLogImportant("[Overlay] InitVulkan failed: null device pointer");
        return false;
    }

    HookLogImportant("[Overlay] Initializing Vulkan backend (device=%p, queue=%p, family=%u)", device, queue,
                     queueFamily);

    auto* vkBackend =
        new CustomOverlay::VulkanBackend((VkDevice)device, (VkPhysicalDevice)physDevice, (VkQueue)queue, queueFamily);

    // Set dispatch tables before initialization
    if (deviceDispatch && instanceDispatch) {
        vkBackend->SetDispatchTable(deviceDispatch, instanceDispatch);
    }

    float dpiScale = GetWindowsDpiScale(reinterpret_cast<HWND>(hwnd));
    return InitializeBackendLocked(vkBackend, OverlayBackendType::Vulkan, "Vulkan", dpiScale);
}

bool OverlayAdapter::InitCustom(CustomOverlay::RendererBackend* customBackend, OverlayBackendType type) {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (initialized.load(std::memory_order_acquire))
        return true;
    if (!customBackend) {
        HookLogImportant("[Overlay] InitCustom failed: null backend");
        return false;
    }

    HookLogImportant("[Overlay] Initializing custom backend (type=%d, ptr=%p)", (int)type, customBackend);
    float dpiScale = GetWindowsDpiScale(reinterpret_cast<HWND>(hwnd));
    return InitializeBackendLocked(customBackend, type, "Custom", dpiScale);
}

void OverlayAdapter::SetShutdownMode(bool skipRelease) {
    std::lock_guard<std::mutex> lock(stateMutex);
    ApplyShutdownModeLocked(skipRelease);
}

void OverlayAdapter::Shutdown() {
    std::lock_guard<std::mutex> lock(stateMutex);
    DestroyResourcesLocked(true);
    ResetStateLocked();
}

void OverlayAdapter::SetDX12RenderTarget(void* cmdList, void* rtvHandle) {
#ifndef VK_LAYER_CE_OVERLAY
    std::lock_guard<std::mutex> lock(stateMutex);
    if (backendType == OverlayBackendType::DX12 && backend) {
        auto dx12Backend = static_cast<CustomOverlay::DX12Backend*>(backend);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv;
        rtv.ptr = (SIZE_T)rtvHandle;
        dx12Backend->SetRenderTarget((ID3D12GraphicsCommandList*)cmdList, rtv);
    }
#endif
}

void OverlayAdapter::SetDX12UploadSlotFence(void* fence, uint64_t guardValue) {
#ifndef VK_LAYER_CE_OVERLAY
    std::lock_guard<std::mutex> lock(stateMutex);
    if (backendType == OverlayBackendType::DX12 && backend) {
        auto dx12Backend = static_cast<CustomOverlay::DX12Backend*>(backend);
        dx12Backend->SetUploadSlotFence(static_cast<ID3D12Fence*>(fence), guardValue);
    }
#endif
}

bool OverlayAdapter::PrimeDX12Resources(void* cmdList) {
#ifndef VK_LAYER_CE_OVERLAY
    std::lock_guard<std::mutex> lock(stateMutex);
    if (backendType == OverlayBackendType::DX12 && backend) {
        auto dx12Backend = static_cast<CustomOverlay::DX12Backend*>(backend);
        return dx12Backend->PrimeResources(static_cast<ID3D12GraphicsCommandList*>(cmdList));
    }
#endif
    return false;
}

bool OverlayAdapter::HasPendingDX12Resources() const {
#ifndef VK_LAYER_CE_OVERLAY
    std::lock_guard<std::mutex> lock(stateMutex);
    if (backendType == OverlayBackendType::DX12 && backend) {
        auto dx12Backend = static_cast<CustomOverlay::DX12Backend*>(backend);
        return dx12Backend->HasPendingResources();
    }
#endif
    return false;
}

uint32_t OverlayAdapter::GetLoadColor(float load) {
    using namespace CustomOverlay::Colors;
    if (load < 50.0f)
        return LoadLow;
    if (load < 85.0f)
        return LoadMed;
    return LoadHigh;
}

void OverlayAdapter::RenderOverlay(int viewportWidth, int viewportHeight) {
    std::lock_guard<std::mutex> lock(stateMutex);
    static int renderLogCount = 0;
    if (renderLogCount < 5) {
        HookLog("[Overlay] RenderOverlay#%d: init=%d renderer=%p ipc=%p shm=%p showOverlay=%d vp=%dx%d", renderLogCount,
                initialized ? 1 : 0, (void*)renderer, (void*)ipc, ipc ? (void*)ipc->GetSharedMem() : nullptr,
                (ipc && ipc->GetSharedMem()) ? ipc->GetSharedMem()->ReadOverlayConfig().showOverlay : -1, viewportWidth,
                viewportHeight);
        renderLogCount++;
    }

    if (!initialized || !renderer) {
        if (renderLogCount < 5)
            HookLog("[Overlay] RenderOverlay: early return - not initialized or no renderer");
        return;
    }

    if (!ipc || !ipc->GetSharedMem()) {
        if (renderLogCount < 5)
            HookLog("[Overlay] RenderOverlay: early return - no IPC or shared memory");
        return;
    }
    auto* sharedMem = ipc->GetSharedMem();
    auto cfg = sharedMem->ReadOverlayConfig();
    if (!cfg.showOverlay) {
        if (renderLogCount < 5)
            HookLog("[Overlay] RenderOverlay: early return - showOverlay is false");
        return;
    }

    // Update throttling
    DWORD now = GetTickCount();
    bool shouldUpdate = (now - lastUpdateTime) >= cfg.textUpdateInterval;
    if (shouldUpdate) {
        lastUpdateTime = now;
        if (metrics) {
            cachedFPS = metrics->GetCurrentFPS();
            cachedAvgFPS = metrics->GetAverageFPS();
            cached1PercentLow = metrics->Get1PercentLowFPS();
            cached01PercentLow = metrics->Get01PercentLowFPS();
        }
        if (cfg.showCPU || cfg.showRAM || cfg.showGPU || cfg.showVRAM) {
            SystemMetricsCollector::Get().Update();
            cachedSystemMetrics = SystemMetricsCollector::Get().GetMetrics();
        }
    }

    bool fgVisible = cfg.showFG && metrics && metrics->IsFGActive();
    bool isRecording = sharedMem->runtimeState.isRecording.load(std::memory_order_acquire);
    uint64_t recordingSeconds = 0;
    uint64_t nowTick64 = GetTickCount64();
    if (cfg.showRecording && isRecording) {
        int64_t startTime = sharedMem->runtimeState.recordingStartTime.load(std::memory_order_acquire);
        if (startTime > 0) {
            recordingSeconds = (nowTick64 - startTime) / 1000;
        }
        uint32_t overloadFlags = sharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
        const uint32_t captureHealthFlags =
            sharedMem->runtimeState.wgcCaptureHealthFlags.load(std::memory_order_relaxed);
        const uint32_t warningKind = ce::capture_policy::SelectWgcOverlayWarningKind(overloadFlags, captureHealthFlags);
        if (ce::capture_policy::IsWgcCaptureLimitedForOverlay(captureHealthFlags)) {
            lastEncoderOverloadTick = 0;
            lastRecordingWarningKind = ce::capture_policy::kOverlayWarningNone;
        } else if (warningKind != ce::capture_policy::kOverlayWarningNone) {
            lastEncoderOverloadTick = nowTick64;
            lastRecordingWarningKind = warningKind;
        }
    } else {
        lastEncoderOverloadTick = 0;
        lastRecordingWarningKind = ce::capture_policy::kOverlayWarningNone;
    }
    bool showOverloadWarning = (lastEncoderOverloadTick != 0) && ((nowTick64 - lastEncoderOverloadTick) <= 5000);
    const uint32_t recordingWarningKind =
        showOverloadWarning ? lastRecordingWarningKind : ce::capture_policy::kOverlayWarningNone;
    PresentDebugSample* activeDebugSample = PerfLogger::Get().GetActiveDebugSample();
    bool showGraph = cfg.showFrameTime && metrics;
    bool shouldRefreshGraph = showGraph;
    bool viewportChanged = (viewportWidth != lastViewportWidth) || (viewportHeight != lastViewportHeight);
    bool configChanged = !hasRenderedConfig || !OverlayConfigEquals(cfg, lastRenderedConfig);
    bool dynamicStateChanged = (fgVisible != lastFGActive) || (reserveInactiveFGSpace != lastReserveInactiveFGSpace) ||
                               (isRecording != lastRecordingActive) || (recordingSeconds != lastRecordingSeconds) ||
                               (showOverloadWarning != lastShowOverloadWarning) ||
                               (recordingWarningKind != lastRenderedRecordingWarningKind);
    bool needRebuild = !hasCachedFrame || shouldUpdate || shouldRefreshGraph || viewportChanged || configChanged ||
                       dynamicStateChanged || layoutDirty;
    static int renderPathLogCount = 0;
    if (renderPathLogCount < 10) {
        HookLogImportant(
            "[Overlay] RenderOverlay path: rebuild=%d cache=%d shouldUpdate=%d viewportChanged=%d cfgChanged=%d",
            needRebuild ? 1 : 0, hasCachedFrame ? 1 : 0, shouldUpdate ? 1 : 0, viewportChanged ? 1 : 0,
            configChanged ? 1 : 0);
        renderPathLogCount++;
    }

    // Pass HDR params to backend for shader constants
    if (backend) {
        float paperWhite = cfg.hdrPaperWhite;
        if (paperWhite <= 0.0f)
            paperWhite = 200.0f;  // Default paper white nits
        int mode = 0;             // SDR
        if (isHDR) {
            // Detect HDR10/PQ (R10G10B10A2) vs scRGB (FP16) from render target format
            // DXGI_FORMAT_R10G10B10A2_UNORM = 24
            mode = (renderTargetFormat == 24) ? 2 : 1;
        }
        backend->SetHDRParams(mode, paperWhite);
    }

    if (!needRebuild) {
        const int64_t cachedRenderStartUs = activeDebugSample ? PerfLogger::GetQpcUs() : 0;
        if (renderer->RenderCachedFrame(viewportWidth, viewportHeight)) {
            if (activeDebugSample) {
                activeDebugSample->flags |= kPresentSampleFlagOverlayCacheHit;
                activeDebugSample->overlayRenderUs +=
                    static_cast<int32_t>(PerfLogger::GetQpcUs() - cachedRenderStartUs);
            }
            return;
        }
        hasCachedFrame = false;
    }

    const int64_t overlayBuildStartUs = activeDebugSample ? PerfLogger::GetQpcUs() : 0;
    if (renderPathLogCount < 10) {
        HookLogImportant("[Overlay] RenderOverlay: BeginFrame %dx%d", viewportWidth, viewportHeight);
    }
    renderer->BeginFrame(viewportWidth, viewportHeight);
    RenderContent(viewportWidth, viewportHeight, cfg, shouldUpdate);
    if (activeDebugSample) {
        activeDebugSample->flags |= kPresentSampleFlagOverlayRebuilt;
        activeDebugSample->overlayBuildUs += static_cast<int32_t>(PerfLogger::GetQpcUs() - overlayBuildStartUs);
    }
    const int64_t overlayRenderStartUs = activeDebugSample ? PerfLogger::GetQpcUs() : 0;
    if (renderPathLogCount < 10) {
        HookLogImportant("[Overlay] RenderOverlay: EndFrame");
    }
    renderer->EndFrame();
    if (activeDebugSample) {
        activeDebugSample->overlayRenderUs += static_cast<int32_t>(PerfLogger::GetQpcUs() - overlayRenderStartUs);
    }

    hasCachedFrame = true;
    hasRenderedConfig = true;
    lastRenderedConfig = cfg;
    lastViewportWidth = viewportWidth;
    lastViewportHeight = viewportHeight;
    lastFGActive = fgVisible;
    lastReserveInactiveFGSpace = reserveInactiveFGSpace;
    lastRecordingActive = isRecording;
    lastRecordingSeconds = recordingSeconds;
    lastShowOverloadWarning = showOverloadWarning;
    lastRenderedRecordingWarningKind = recordingWarningKind;
}

void OverlayAdapter::RenderContent(int viewportWidth, int viewportHeight, const OverlayConfig& cfg, bool shouldUpdate) {
    using namespace CustomOverlay;

    if (!ipc || !ipc->GetSharedMem())
        return;
    auto& mem = *ipc->GetSharedMem();

    // Get DPI scale for consistent sizing
    float dpiScale = renderer->GetDpiScale();

    // Calculate position (DPI-aware padding)
    float padding = (float)cfg.padding * dpiScale;
    float x = padding, y = padding;

    // Log position info on first few renders
    static int posLogCount = 0;
    if (posLogCount < 3) {
        HookLog("[Overlay] RenderContent#%d: dpiScale=%.2f, vp=%dx%d, x=%.1f, y=%.1f, padding=%.1f", posLogCount,
                dpiScale, viewportWidth, viewportHeight, x, y, padding);
        posLogCount++;
    }

    float lineHeight = (float)renderer->GetLineHeight();

    // Check dynamic states used by both sizing and rendering
    bool fgActive = metrics && metrics->IsFGActive();
    bool showFGDetails = cfg.showFG && fgActive;
    const bool reserveFGDetailsSpace = cfg.showFG && reserveInactiveFGSpace;
    bool isRecording = mem.runtimeState.isRecording.load(std::memory_order_acquire);

    // Adaptive overlay width: measure visible labels/values and size to content.
    const float kShadowPad = 1.0f;
    const float kBgLeftPad = 4.0f * dpiScale;
    const float kBgRightPad = 4.0f * dpiScale + kShadowPad;
    float kBgTopPad = 2.0f * dpiScale;
    if (cfg.showVRAM || cfg.showRAM) {
        kBgTopPad = (std::max)(kBgTopPad, lineHeight * 0.20f + kShadowPad);
    }
    const float kBgBottomPad = 2.0f * dpiScale + kShadowPad;
    const float kColumnGap = 10.0f * dpiScale;
    const float kMinContentWidth = 210.0f * dpiScale;
    const float kMemorySuffixScale = 0.75f;
    const float kMemoryGap = 2.0f * dpiScale;

    auto MeasureTextWidth = [&](const char* text) -> float {
        float w = 0, h = 0;
        renderer->CalcTextSize(text ? text : "", &w, &h);
        return w;
    };
    auto MeasureTextWidthScaled = [&](const char* text, float scale) -> float {
        float w = 0, h = 0;
        renderer->CalcTextSizeScaled(text ? text : "", &w, &h, scale);
        return w;
    };

    // Expensive layout measurement (snprintf + CalcTextSize) – cached between
    // updates to avoid per-frame overhead at high refresh rates.
    if (shouldUpdate || layoutDirty) {
        float maxLabelWidth = 0.0f;
        float maxValueWidth = 0.0f;
        char measureBuf[96];

        if (cfg.showGPU) {
            snprintf(measureBuf, sizeof(measureBuf), "%.0f%%", cachedSystemMetrics.gpuUsage);
            maxLabelWidth = (std::max)(maxLabelWidth, MeasureTextWidth(SystemMetricsCollector::Get().GetGPUName()));
            maxValueWidth = (std::max)(maxValueWidth, MeasureTextWidth(measureBuf) + kShadowPad);
        }
        if (cfg.showCPU) {
            snprintf(measureBuf, sizeof(measureBuf), "%.0f%% (%.0f%%)", cachedSystemMetrics.cpuUsage,
                     cachedSystemMetrics.cpuMaxCoreUsage);
            maxLabelWidth = (std::max)(maxLabelWidth, MeasureTextWidth(SystemMetricsCollector::Get().GetCPUName()));
            maxValueWidth = (std::max)(maxValueWidth, MeasureTextWidth(measureBuf) + kShadowPad);
        }
        if (cfg.showVRAM) {
            float gbUsed = (float)cachedSystemMetrics.vramUsed / (1024.0f * 1024.0f * 1024.0f);
            float gbTotal = (float)cachedSystemMetrics.vramTotal / (1024.0f * 1024.0f * 1024.0f);
            if (gbTotal < 0.1f)
                gbTotal = 11.66f;
            char usedBuf[32], totalBuf[32];
            snprintf(usedBuf, sizeof(usedBuf), "%.2f GB", gbUsed);
            snprintf(totalBuf, sizeof(totalBuf), "of %.2f GB", gbTotal);
            float valueWidth = MeasureTextWidth(usedBuf) + kMemoryGap +
                               MeasureTextWidthScaled(totalBuf, kMemorySuffixScale) + kShadowPad;
            maxLabelWidth = (std::max)(maxLabelWidth, MeasureTextWidth("VRAM"));
            maxValueWidth = (std::max)(maxValueWidth, valueWidth);
        }
        if (cfg.showRAM) {
            float gbUsed = (float)cachedSystemMetrics.ramUsed / (1024.0f * 1024.0f * 1024.0f);
            float gbTotal = (float)cachedSystemMetrics.ramTotal / (1024.0f * 1024.0f * 1024.0f);
            if (gbTotal < 0.1f)
                gbTotal = 31.93f;
            char usedBuf[32], totalBuf[32];
            snprintf(usedBuf, sizeof(usedBuf), "%.2f GB", gbUsed);
            snprintf(totalBuf, sizeof(totalBuf), "of %.2f GB", gbTotal);
            float valueWidth = MeasureTextWidth(usedBuf) + kMemoryGap +
                               MeasureTextWidthScaled(totalBuf, kMemorySuffixScale) + kShadowPad;
            maxLabelWidth = (std::max)(maxLabelWidth, MeasureTextWidth("RAM"));
            maxValueWidth = (std::max)(maxValueWidth, valueWidth);
        }
        if (cfg.showFPS) {
            const char* apiLabel = graphicsAPI[0] ? graphicsAPI : "FPS";
            snprintf(measureBuf, sizeof(measureBuf), "%.0f FPS", cachedFPS);
            maxLabelWidth = (std::max)(maxLabelWidth, MeasureTextWidth(apiLabel));
            maxValueWidth = (std::max)(maxValueWidth, MeasureTextWidth(measureBuf) + kShadowPad);

            if ((showFGDetails || reserveFGDetailsSpace) && metrics) {
                float baseFPS = metrics->GetFGBaseFPS();
                float outputFPS = metrics->GetFGOutputFPS();
                int fgMult = metrics->GetFGMultiplier();
                if (outputFPS < 1.0f)
                    outputFPS = cachedFPS;
                if (baseFPS < 1.0f) {
                    if (fgMult >= 2)
                        baseFPS = outputFPS / fgMult;
                    else
                        baseFPS = cachedFPS;
                }
                snprintf(measureBuf, sizeof(measureBuf), "%.0f / %.0f FPS", baseFPS, outputFPS);
                maxLabelWidth = (std::max)(maxLabelWidth, MeasureTextWidth("Base/Display"));
                maxValueWidth = (std::max)(maxValueWidth, MeasureTextWidth(measureBuf) + kShadowPad);
            }

            if (cachedAvgFPS > 0 && cached1PercentLow > 0) {
                snprintf(measureBuf, sizeof(measureBuf), "%.0f / %.0f / %.0f", cachedAvgFPS, cached1PercentLow,
                         cached01PercentLow);
                maxLabelWidth = (std::max)(maxLabelWidth, MeasureTextWidth("Avg/1%/0.1%"));
                maxValueWidth = (std::max)(maxValueWidth, MeasureTextWidth(measureBuf) + kShadowPad);
            }
        }
        if ((showFGDetails || reserveFGDetailsSpace) && metrics) {
            int multiplier = metrics->GetFGMultiplier();
            const char* fgLabel = metrics->GetFGTypeLabel();
            if (reserveInactiveFGSpace && !showFGDetails) {
                fgLabel = "DLSS FG";
                multiplier = 4;
            }
            snprintf(measureBuf, sizeof(measureBuf), "%s %dx", fgLabel, multiplier);
            maxLabelWidth = (std::max)(maxLabelWidth, MeasureTextWidth(fgLabel));
            maxValueWidth = (std::max)(maxValueWidth, MeasureTextWidth(measureBuf) + kShadowPad);
        }

        float measuredWidth = kMinContentWidth;
        if (maxLabelWidth > 0.0f || maxValueWidth > 0.0f) {
            measuredWidth = (std::max)(measuredWidth, maxLabelWidth + kColumnGap + maxValueWidth);
        }

        // Recording row uses a fixed-width digit format; measure a canonical string
        // so the result is stable regardless of elapsed time (digits are tabular).
        if (cfg.showRecording && isRecording) {
            char recBuf[96];
            snprintf(recBuf, sizeof(recBuf), "REC 00:00:00");
            measuredWidth = (std::max)(measuredWidth, MeasureTextWidth(recBuf) + kShadowPad);
            snprintf(recBuf, sizeof(recBuf), "REC 00:00:00 !ENCODER OVERLOAD!");
            measuredWidth = (std::max)(measuredWidth, MeasureTextWidth(recBuf) + kShadowPad);
            snprintf(recBuf, sizeof(recBuf), "AUDIO 00:00:00");
            measuredWidth = (std::max)(measuredWidth, MeasureTextWidth(recBuf) + kShadowPad);
        }

        cachedContentWidth = measuredWidth;
        layoutDirty = false;
    }

    float contentWidth = cachedContentWidth;
    float bgWidth = contentWidth + kBgLeftPad + kBgRightPad;
    float maxBgWidth = (float)viewportWidth - 2.0f * padding;
    maxBgWidth = (std::max)(maxBgWidth, 100.0f * dpiScale);
    if (bgWidth > maxBgWidth) {
        bgWidth = maxBgWidth;
        contentWidth = (std::max)(80.0f * dpiScale, bgWidth - kBgLeftPad - kBgRightPad);
    }

    // Calculate required height upfront so BottomLeft/BottomRight use the real
    // overlay height instead of a hardcoded estimate.
    float requiredHeight = lineHeight;
    if (cfg.showGPU)
        requiredHeight += lineHeight;
    if (cfg.showCPU)
        requiredHeight += lineHeight;
    if (cfg.showVRAM)
        requiredHeight += lineHeight;
    if (cfg.showRAM)
        requiredHeight += lineHeight;
    if (cfg.showFPS) {
        requiredHeight += lineHeight;
        if (cachedAvgFPS > 0)
            requiredHeight += lineHeight;
        // Base/Display line when FG is active
        if (showFGDetails || reserveFGDetailsSpace)
            requiredHeight += lineHeight;
    }
    if (showFGDetails || reserveFGDetailsSpace)
        requiredHeight += lineHeight;
    if (cfg.showRecording && isRecording)
        requiredHeight += lineHeight;

    // Extend by one line when screenshot notification is active
    uint64_t notifExpiry = mem.runtimeState.notificationExpiry.load(std::memory_order_acquire);
    uint32_t notifType = mem.runtimeState.notificationType.load(std::memory_order_relaxed);
    bool showNotification = (notifExpiry > 0 && GetTickCount64() < notifExpiry && notifType != 0);
    if (showNotification)
        requiredHeight += lineHeight;

    constexpr int GRAPH_SAMPLES = 180;
    bool showGraph = cfg.showFrameTime && metrics;
    if (showGraph)
        requiredHeight += 4 * dpiScale + 50.0f * dpiScale;
    float bgHeight = requiredHeight + kBgTopPad + kBgBottomPad;

    switch (cfg.position) {
        case OverlayPosition::TopLeft:
            x = padding;
            y = padding;
            break;
        case OverlayPosition::TopRight:
            x = viewportWidth - padding - bgWidth;
            y = padding;
            break;
        case OverlayPosition::BottomLeft:
            x = padding;
            y = viewportHeight - padding - bgHeight;
            break;
        case OverlayPosition::BottomRight:
            x = viewportWidth - padding - bgWidth;
            y = viewportHeight - padding - bgHeight;
            break;
    }

    float valueRightEdge = x + contentWidth;

    float graphData[GRAPH_SAMPLES] = {};
    float graphY = 0, graphHeight = 0, graphWidth = 0, graphX = 0;
    float graphMinVal = 0, graphMaxVal = 0;
    if (showGraph)
        metrics->GetLastHistory(graphData, GRAPH_SAMPLES);

    // --- PASS 1: All solid geometry (background + graph) ---
    uint8_t bgAlpha = (uint8_t)(cfg.bgAlpha * 255);
    uint32_t bgColor = (bgAlpha << 24) | (cfg.bgColor & 0x00FFFFFF);
    renderer->DrawRectFilled(x - kBgLeftPad, y - kBgTopPad, bgWidth, bgHeight, bgColor);

    // Calculate cursorY for graph position (same layout as text pass)
    float graphCursorY = y;
    if (cfg.showGPU)
        graphCursorY += lineHeight;
    if (cfg.showCPU)
        graphCursorY += lineHeight;
    if (cfg.showVRAM)
        graphCursorY += lineHeight;
    if (cfg.showRAM)
        graphCursorY += lineHeight;
    if (cfg.showFPS) {
        graphCursorY += lineHeight;
        if (cachedAvgFPS > 0 && cached1PercentLow > 0)
            graphCursorY += lineHeight;
        if (showFGDetails || reserveFGDetailsSpace)
            graphCursorY += lineHeight;
    }
    if (showFGDetails || reserveFGDetailsSpace)
        graphCursorY += lineHeight;
    if (cfg.showRecording && isRecording)
        graphCursorY += lineHeight;
    if (showNotification)
        graphCursorY += lineHeight;

    // Max frame time display values – recomputed at most once every 2 seconds so
    // the label doesn't flicker on every frame.
    float recentMaxFrameTime = cachedMaxFrameTime;
    float recentAvgFrameTime = cachedAvgFrameTimeForColor;
    if (showGraph) {
        DWORD ftNow = GetTickCount();
        if ((ftNow - lastMaxFrameTimeUpdateTime) >= 2000) {
            lastMaxFrameTimeUpdateTime = ftNow;
            int samplesPerSecond = (cachedFPS > 0) ? (int)cachedFPS : 60;
            int samplesFor2Seconds = (std::min)(GRAPH_SAMPLES, samplesPerSecond * 2);
            int startIdx = GRAPH_SAMPLES - samplesFor2Seconds;

            float newMax = 0.0f, sum = 0.0f;
            int count = 0;
            for (int i = startIdx; i < GRAPH_SAMPLES; i++) {
                float val = graphData[i];
                if (val > newMax)
                    newMax = val;
                sum += val;
                count++;
            }
            cachedMaxFrameTime = newMax;
            cachedAvgFrameTimeForColor = (count > 0) ? sum / count : 0.0f;
            recentMaxFrameTime = cachedMaxFrameTime;
            recentAvgFrameTime = cachedAvgFrameTimeForColor;
        }

        // Calculate graph scaling - use exact peak with dynamic minimum for
        // better vertical centering. Instead of always starting at 0ms,
        // calculate a minimum that provides ~15% padding below the lowest point.
        float peakVal = 0.0f;
        float minVal = FLT_MAX;
        for (int i = 0; i < GRAPH_SAMPLES; i++) {
            if (graphData[i] > peakVal)
                peakVal = graphData[i];
            if (graphData[i] > 0.001f && graphData[i] < minVal)
                minVal = graphData[i];
        }
        if (minVal == FLT_MAX)
            minVal = 0.0f;

        // Smarter scaling to avoid over-dramatizing frame time variations.
        // Instead of just 10% above peak, use a scale that provides meaningful
        // context (e.g., 30fps vs 60fps threshold should be visible).
        float avgVal = 0;
        int avgCount = 0;
        for (int i = 0; i < GRAPH_SAMPLES; i++) {
            if (graphData[i] > 0.001f) {
                avgVal += graphData[i];
                avgCount++;
            }
        }
        if (avgCount > 0)
            avgVal /= (float)avgCount;

        // Scale based on average with generous headroom:
        // - At least 50% above average (not peak) to avoid zooming in on noise
        // - At least 2x the minimum (so 8ms avg shows 0-16ms, not 0-12ms)
        // - Minimum 33ms range to show 30fps threshold
        float scaleBase = (std::max)(avgVal, minVal * 2.0f);
        float rawMax = (std::max)(scaleBase * 1.5f, 33.0f);

        // Dynamic minimum: leave ~15% padding below lowest point, but never go negative
        // This makes the graph line appear more vertically centered
        float range = rawMax - minVal;
        float dynamicMin = (std::max)(0.0f, minVal - range * 0.15f);

        graphMinVal = dynamicMin;
        graphMaxVal = rawMax;

        graphWidth = bgWidth;
        graphHeight = 50.0f * dpiScale;
        graphX = x - 4 * dpiScale;
        graphY = graphCursorY + 4 * dpiScale;  // Small gap below stats

        // Debug: log graph dimensions on first render
        static bool s_loggedGraphDims = false;
        if (!s_loggedGraphDims) {
            s_loggedGraphDims = true;
            HookLogImportant("[Overlay] Graph dims: graphX=%.1f graphWidth=%.1f bgWidth=%.1f x=%.1f kBgLeftPad=%.1f",
                             graphX, graphWidth, bgWidth, x, kBgLeftPad);
        }

        // Top padding keeps the graph line below the scale marker and frame time
        // labels that are drawn at graphY + ~6px.
        float graphTopPad = 14.0f * dpiScale;

        uint32_t graphColor = cfg.frametimeColor ? cfg.frametimeColor : Colors::Yellow;
        renderer->DrawFrameTimeGraph(graphX, graphY + graphTopPad, graphWidth, graphHeight - graphTopPad, graphData,
                                     GRAPH_SAMPLES, graphMinVal, graphMaxVal, graphColor);
    }

    // --- PASS 2: All text (single textured batch) ---
    float cursorY = y;
    char buf[64];

    uint32_t textColor = cfg.textColor ? cfg.textColor : Colors::White;
    uint32_t shadowColor = cfg.textOutlineColor ? cfg.textOutlineColor : Colors::Black;

    // Column positions for alignment (DPI-aware)
    float labelCol = x;

    // Transient notification inside the overlay panel (background extends to fit)
    if (notifExpiry > 0 && GetTickCount64() < notifExpiry && notifType != 0) {
        const char* notifText = nullptr;
        uint32_t notifColor = Colors::Green;
        switch (notifType) {
            case 1:
                notifText = "Screenshot saved!";
                break;
            default:
                break;
        }
        if (notifText) {
            renderer->DrawTextWithShadow(labelCol, cursorY, notifText, notifColor, shadowColor);
            cursorY += lineHeight;
        }
    }

    // GPU - Name in green, % in yellow (based on load)
    if (cfg.showGPU) {
        snprintf(buf, 64, "%.0f%%", cachedSystemMetrics.gpuUsage);
        renderer->DrawTextWithShadow(labelCol, cursorY, SystemMetricsCollector::Get().GetGPUName(), Colors::LabelGreen,
                                     shadowColor);
        renderer->DrawTextRightAligned(valueRightEdge, cursorY, buf, GetLoadColor(cachedSystemMetrics.gpuUsage),
                                       shadowColor);
        cursorY += lineHeight;
    }

    // CPU - Name in green, % (maxCore%) in cyan
    if (cfg.showCPU) {
        snprintf(buf, 64, "%.0f%% (%.0f%%)", cachedSystemMetrics.cpuUsage, cachedSystemMetrics.cpuMaxCoreUsage);
        renderer->DrawTextWithShadow(labelCol, cursorY, SystemMetricsCollector::Get().GetCPUName(), Colors::LabelGreen,
                                     shadowColor);
        renderer->DrawTextRightAligned(valueRightEdge, cursorY, buf, Colors::ValueCyan, shadowColor);
        cursorY += lineHeight;
    }

    // VRAM - Label in orange, "X.XX GB" in white, "of Y.YY GB" in smaller raised
    // text
    if (cfg.showVRAM) {
        float gbUsed = (float)cachedSystemMetrics.vramUsed / (1024.0f * 1024.0f * 1024.0f);
        float gbTotal = (float)cachedSystemMetrics.vramTotal / (1024.0f * 1024.0f * 1024.0f);
        if (gbTotal < 0.1f)
            gbTotal = 11.66f;  // Fallback if not detected

        renderer->DrawTextWithShadow(labelCol, cursorY, "VRAM", Colors::LabelOrange, shadowColor);

        char usedBuf[32];
        snprintf(usedBuf, 32, "%.2f GB", gbUsed);
        char totalBuf[32];
        snprintf(totalBuf, 32, "of %.2f GB", gbTotal);

        // Right-align the whole "used + of total" composite so it always fits.
        float usedWidth = 0, usedHeight = 0;
        float totalWidth = 0;
        renderer->CalcTextSize(usedBuf, &usedWidth, &usedHeight);
        float smallScale = 0.75f;     // Smaller scale for superscript
        float gap = 2.0f * dpiScale;  // Minimal gap between segments
        renderer->CalcTextSizeScaled(totalBuf, &totalWidth, nullptr, smallScale);
        float usedX = valueRightEdge - (usedWidth + gap + totalWidth);
        renderer->DrawTextWithShadow(usedX, cursorY, usedBuf, Colors::LabelOrange, shadowColor);

        float raisedY = cursorY - usedHeight * 0.20f;  // Raised 20% of line height
        renderer->DrawTextScaledWithShadow(usedX + usedWidth + gap, raisedY, totalBuf, textColor, shadowColor,
                                           smallScale);

        cursorY += lineHeight;
    }

    // RAM - Label in pink, "X.XX GB" in white, "of Y.YY GB" in smaller raised
    // text
    if (cfg.showRAM) {
        float gbUsed = (float)cachedSystemMetrics.ramUsed / (1024.0f * 1024.0f * 1024.0f);
        float gbTotal = (float)cachedSystemMetrics.ramTotal / (1024.0f * 1024.0f * 1024.0f);
        if (gbTotal < 0.1f)
            gbTotal = 31.93f;  // Fallback if not detected

        renderer->DrawTextWithShadow(labelCol, cursorY, "RAM", Colors::LabelPink, shadowColor);

        char usedBuf[32];
        snprintf(usedBuf, 32, "%.2f GB", gbUsed);
        char totalBuf[32];
        snprintf(totalBuf, 32, "of %.2f GB", gbTotal);

        // Right-align the whole "used + of total" composite so it always fits.
        float usedWidth = 0, usedHeight = 0;
        float totalWidth = 0;
        renderer->CalcTextSize(usedBuf, &usedWidth, &usedHeight);
        float smallScale = 0.75f;     // Smaller scale for superscript
        float gap = 2.0f * dpiScale;  // Minimal gap between segments
        renderer->CalcTextSizeScaled(totalBuf, &totalWidth, nullptr, smallScale);
        float usedX = valueRightEdge - (usedWidth + gap + totalWidth);
        renderer->DrawTextWithShadow(usedX, cursorY, usedBuf, Colors::LabelPink, shadowColor);

        float raisedY = cursorY - usedHeight * 0.20f;  // Raised 20% of line height
        renderer->DrawTextScaledWithShadow(usedX + usedWidth + gap, raisedY, totalBuf, textColor, shadowColor,
                                           smallScale);

        cursorY += lineHeight;
    }

    // FPS Section
    if (cfg.showFPS) {
        // Graphics API label (if set) with current FPS
        const char* apiLabel = graphicsAPI[0] ? graphicsAPI : "FPS";
        snprintf(buf, 64, "%.0f FPS", cachedFPS);
        renderer->DrawTextWithShadow(labelCol, cursorY, apiLabel, textColor, shadowColor);
        renderer->DrawTextRightAligned(valueRightEdge, cursorY, buf, cfg.fpsColor, shadowColor);
        cursorY += lineHeight;

        // Base/Display FPS when FG is active (shown first as in reference)
        if (showFGDetails) {
            float baseFPS = metrics->GetFGBaseFPS();
            float outputFPS = metrics->GetFGOutputFPS();
            int fgMult = metrics->GetFGMultiplier();

            // When metrics haven't been computed (dormant mode or early startup),
            // derive values from the live per-frame FPS and the known multiplier.
            if (outputFPS < 1.0f)
                outputFPS = cachedFPS;
            if (baseFPS < 1.0f) {
                if (fgMult >= 2)
                    baseFPS = outputFPS / fgMult;
                else
                    baseFPS = cachedFPS;
            }

            snprintf(buf, 64, "%.0f / %.0f FPS", baseFPS, outputFPS);
            renderer->DrawTextWithShadow(labelCol, cursorY, "Base/Display", Colors::LabelYellow, shadowColor);
            renderer->DrawTextRightAligned(valueRightEdge, cursorY, buf, Colors::ValueYellow, shadowColor);
            cursorY += lineHeight;
        } else if (reserveFGDetailsSpace) {
            cursorY += lineHeight;
        }

        // Avg/1%/0.1%
        if (cachedAvgFPS > 0 && cached1PercentLow > 0) {
            snprintf(buf, 64, "%.0f / %.0f / %.0f", cachedAvgFPS, cached1PercentLow, cached01PercentLow);
            renderer->DrawTextWithShadow(labelCol, cursorY, "Avg/1%/0.1%", textColor, shadowColor);
            renderer->DrawTextRightAligned(valueRightEdge, cursorY, buf, Colors::ValueYellow, shadowColor);
            cursorY += lineHeight;
        }
    }

    // FG Status line
    if (showFGDetails) {
        int multiplier = metrics->GetFGMultiplier();
        const char* fgLabel = metrics->GetFGTypeLabel();
        snprintf(buf, 64, "%s %dx", fgLabel, multiplier);
        renderer->DrawTextWithShadow(labelCol, cursorY, fgLabel, Colors::LabelCyan, shadowColor);
        renderer->DrawTextRightAligned(valueRightEdge, cursorY, buf, Colors::LabelCyan, shadowColor);
        cursorY += lineHeight;
    } else if (reserveFGDetailsSpace) {
        cursorY += lineHeight;
    }

    // Recording status line
    if (cfg.showRecording && isRecording) {
        bool isAudioOnly = mem.runtimeState.audioOnly.load(std::memory_order_acquire);
        const char* recLabel = isAudioOnly ? "AUDIO" : "REC";

        int64_t startTime = mem.runtimeState.recordingStartTime.load(std::memory_order_acquire);
        int64_t elapsed = 0;
        if (startTime > 0) {
            elapsed = (GetTickCount64() - startTime) / 1000;
        }
        int hours = (int)(elapsed / 3600);
        int minutes = (int)((elapsed % 3600) / 60);
        int seconds = (int)(elapsed % 60);

        // Suppress encoder warnings while WGC is source/scheduler limited so
        // variable-FPS games do not look like encoder failures.
        uint32_t overloadFlags = mem.runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
        const uint32_t captureHealthFlags =
            mem.runtimeState.wgcCaptureHealthFlags.load(std::memory_order_relaxed);
        uint64_t nowTick = GetTickCount64();
        const uint32_t warningKind = ce::capture_policy::SelectWgcOverlayWarningKind(overloadFlags, captureHealthFlags);
        if (ce::capture_policy::IsWgcCaptureLimitedForOverlay(captureHealthFlags)) {
            lastEncoderOverloadTick = 0;
            lastRecordingWarningKind = ce::capture_policy::kOverlayWarningNone;
        } else if (warningKind != ce::capture_policy::kOverlayWarningNone) {
            lastEncoderOverloadTick = nowTick;
            lastRecordingWarningKind = warningKind;
        }

        // Show overload warning if within 5 seconds of last detection
        bool showOverloadWarning = (lastEncoderOverloadTick != 0) && ((nowTick - lastEncoderOverloadTick) <= 5000);

        if (showOverloadWarning) {
            const uint32_t targetFps = mem.runtimeState.wgcTargetFps.load(std::memory_order_relaxed);
            const uint32_t sustainFpsX100 = mem.runtimeState.encoderSustainFpsX100.load(std::memory_order_relaxed);
            const std::string overloadLabel = FormatEncoderOverloadLabel(sustainFpsX100, targetFps);
            std::snprintf(buf, sizeof(buf), "%s %02d:%02d:%02d %s", recLabel, hours, minutes, seconds, overloadLabel.c_str());
            renderer->DrawTextWithShadow(labelCol, cursorY, buf, Colors::Red, shadowColor);
        } else {
            // Normal recording display
            snprintf(buf, 64, "%s %02d:%02d:%02d", recLabel, hours, minutes, seconds);
            renderer->DrawTextWithShadow(labelCol, cursorY, buf, Colors::Red, shadowColor);
        }
        cursorY += lineHeight;
    } else {
        // Reset overload tracking when not recording
        lastEncoderOverloadTick = 0;
        lastRecordingWarningKind = ce::capture_policy::kOverlayWarningNone;
    }

    // Frame time graph labels and markers
    if (showGraph) {
        uint32_t graphLabelColor = Colors::Green;
        uint32_t grayColor = 0xFFB0B0B0;  // Light gray for scale marker
        float smallFontScale = 0.75f;     // Smaller font for graph labels

        // Scale marker: small gray line at top left with ceiling value (with ms
        // unit)
        float scaleLineLength = 15.0f * dpiScale;
        float scaleLineY = graphY + 1.0f * dpiScale;
        renderer->DrawLine(graphX + 4 * dpiScale, scaleLineY, graphX + 4 * dpiScale + scaleLineLength, scaleLineY,
                           grayColor, 1.0f * dpiScale);

        // Scale marker text (ceiling value with ms unit in small gray font)
        snprintf(buf, 64, "%.0f ms", graphMaxVal);
        float scaleTextWidth = 0, scaleTextHeight = 0;
        renderer->CalcTextSizeScaled(buf, &scaleTextWidth, &scaleTextHeight, smallFontScale);
        renderer->DrawTextScaledWithShadow(graphX + 6 * dpiScale + scaleLineLength, scaleLineY - scaleTextHeight * 0.5f,
                                           buf, grayColor, shadowColor, smallFontScale);

        // Current frame time display at top right of graph
        // Color based on comparison with average: green (close), yellow (spike),
        // red (stutter)
        uint32_t frameTimeColor = Colors::Green;
        if (recentAvgFrameTime > 0.001f) {
            float ratio = recentMaxFrameTime / recentAvgFrameTime;
            if (ratio > 2.0f) {
                frameTimeColor = Colors::Red;  // Bad stutter (2x average)
            } else if (ratio > 1.5f) {
                frameTimeColor = Colors::Yellow;  // Moderate spike (1.5x average)
            }
        }

        snprintf(buf, 64, "%.1f ms", recentMaxFrameTime);
        float ftTextWidth = 0, ftTextHeight = 0;
        renderer->CalcTextSizeScaled(buf, &ftTextWidth, &ftTextHeight, smallFontScale);
        renderer->DrawTextScaledWithShadow(graphX + graphWidth - ftTextWidth - 4 * dpiScale,
                                           scaleLineY - ftTextHeight * 0.5f, buf, frameTimeColor, shadowColor,
                                           smallFontScale);
    }

    (void)viewportWidth;
    (void)viewportHeight;
}
