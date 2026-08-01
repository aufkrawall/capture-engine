/**
 * Overlay Adapter Implementation
 *
 * Creates the appropriate backend and renders overlay using CustomOverlay.
 */

#include "overlay_adapter.h"
#include "../../common/capture_pipeline_policy.h"
#include "custom_font.h"
#include "custom_overlay.h"
#include "fg_detection.h"
#include "graphics_api_identity.h"
#include "hook_common.h"
#include "overlay_layout_policy.h"
#include "perf_logger.h"
#include "../../common/secure_dll_loading.h"

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

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

// Global adapter instance
OverlayAdapter g_OverlayAdapter;

namespace {
using namespace ce::overlay_layout;

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
            user32 = ce::security::LoadSystemLibrary(L"user32.dll");
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

std::string FormatRecordingHealthLabel(uint32_t warningKind, uint32_t sustainFpsX100, uint32_t targetFps) {
    if (warningKind == ce::capture_policy::kOverlayWarningRecordingDegraded) {
        return "!VIDEO DEGRADED!";
    }
    if (warningKind == ce::capture_policy::kOverlayWarningRecordingRecovering) {
        return "!RECOVERING!";
    }

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
// A known-valid game window remembered from any adapter's SetHwnd, used as the DPI fallback before
// GetForegroundWindow(). During game startup the foreground window can be a 96-DPI launcher/splash, which
// made adapters that init without their own hwnd (the descriptor-free DX12 backend) render at 100% instead
// of the Windows scale.
static std::atomic<HWND> g_SharedOverlayDpiHwnd{nullptr};

static HWND ResolveOverlayReferenceHwnd(HWND targetHwnd) {
    HWND resolved = targetHwnd;
    if (!IsWindow(resolved))
        resolved = g_SharedOverlayDpiHwnd.load(std::memory_order_acquire);
    if (!IsWindow(resolved))
        resolved = GetForegroundWindow();
    if (!IsWindow(resolved))
        resolved = GetDesktopWindow();
    return resolved;
}

static bool QueryWindowsSdrWhiteNits(HMONITOR monitor, float& nits, ULONG& rawLevel) {
    if (!monitor)
        return false;
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo))
        return false;

    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        LONG result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (result != ERROR_SUCCESS || pathCount == 0)
            return false;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER)
            continue;
        if (result != ERROR_SUCCESS)
            return false;
        paths.resize(pathCount);
        for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
            sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sourceName.header.size = sizeof(sourceName);
            sourceName.header.adapterId = path.sourceInfo.adapterId;
            sourceName.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS ||
                lstrcmpiW(sourceName.viewGdiDeviceName, monitorInfo.szDevice) != 0) {
                continue;
            }
            DISPLAYCONFIG_SDR_WHITE_LEVEL whiteLevel{};
            whiteLevel.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
            whiteLevel.header.size = sizeof(whiteLevel);
            whiteLevel.header.adapterId = path.targetInfo.adapterId;
            whiteLevel.header.id = path.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&whiteLevel.header) != ERROR_SUCCESS || whiteLevel.SDRWhiteLevel == 0)
                return false;
            rawLevel = whiteLevel.SDRWhiteLevel;
            nits = std::clamp(static_cast<float>(rawLevel) * (80.0f / 1000.0f), 80.0f, 1000.0f);
            return true;
        }
        return false;
    }
    return false;
}

void OverlayAdapter::RememberDpiReferenceHwnd(void* hwnd) {
    HWND h = reinterpret_cast<HWND>(hwnd);
    if (IsWindow(h)) {
        g_SharedOverlayDpiHwnd.store(h, std::memory_order_release);
    }
}

static float GetWindowsDpiScale(HWND targetHwnd) {
    // Prefer the target game window. Adapters without one reuse a known game
    // window before falling back to foreground/desktop state.
    HWND hwnd = ResolveOverlayReferenceHwnd(targetHwnd);

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

void OverlayAdapter::SetGraphicsAPI(const char* api, const char* evidenceSource) {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (!api)
        api = "";
    if (!ce::graphics_api_identity::LabelsDiffer(graphicsAPI, api))
        return;

    char previousAPI[sizeof(graphicsAPI)] = {};
    strncpy(previousAPI, graphicsAPI, sizeof(previousAPI) - 1);
    strncpy(graphicsAPI, api, sizeof(graphicsAPI) - 1);
    graphicsAPI[sizeof(graphicsAPI) - 1] = '\0';
    layoutDirty = true;
    hasCachedFrame = false;
    HookLogImportant("[GraphicsAPI] label transition '%s' -> '%s' evidence=%s", previousAPI[0] ? previousAPI : "unset",
                     graphicsAPI[0] ? graphicsAPI : "unset", evidenceSource ? evidenceSource : "unspecified");
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
    reserveInactiveFGSpace = false;
    hasLastFrameLayout = false;
    lastFrameLayout = {};
    lastEncoderOverloadTick = 0;
    lastRecordingWarningKind = ce::capture_policy::kOverlayWarningNone;
    layoutDirty = true;
    hdrPaperWhiteMonitor = nullptr;
    resolvedHdrPaperWhiteNits = 203.0f;
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

void OverlayAdapter::SetDX12NextUploadSlot(int slot) {
#ifndef VK_LAYER_CE_OVERLAY
    std::lock_guard<std::mutex> lock(stateMutex);
    if (backendType == OverlayBackendType::DX12 && backend) {
        auto* dx12Backend = static_cast<CustomOverlay::DX12Backend*>(backend);
        dx12Backend->SetNextUploadSlot(slot);
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

    FrameLayoutSnapshot frameLayout = {};
    frameLayout.fgActive = cfg.showFG && metrics && metrics->IsFGActive();
    frameLayout.reserveFGSpace = cfg.showFG && reserveInactiveFGSpace;
    if (frameLayout.fgActive && metrics) {
        frameLayout.fgMultiplier = metrics->GetFGMultiplier();
        std::snprintf(frameLayout.fgLabel, sizeof(frameLayout.fgLabel), "%s", metrics->GetFGTypeLabel());
        frameLayout.fgBaseFPS = metrics->GetFGBaseFPS();
        frameLayout.fgOutputFPS = metrics->GetFGOutputFPS();
    } else if (frameLayout.reserveFGSpace) {
        frameLayout.fgMultiplier = 4;
        std::snprintf(frameLayout.fgLabel, sizeof(frameLayout.fgLabel), "DLSS FG");
    }
    if (frameLayout.fgOutputFPS < 1.0f)
        frameLayout.fgOutputFPS = cachedFPS;
    if (frameLayout.fgBaseFPS < 1.0f) {
        frameLayout.fgBaseFPS =
            frameLayout.fgMultiplier >= 2 ? frameLayout.fgOutputFPS / frameLayout.fgMultiplier : cachedFPS;
    }

    frameLayout.recordingActive = sharedMem->runtimeState.isRecording.load(std::memory_order_acquire);
    frameLayout.recordingAudioOnly = sharedMem->runtimeState.audioOnly.load(std::memory_order_acquire);
    const RecordingStartIntent recordingStartIntent = sharedMem->runtimeState.GetRecordingStartIntent();
    frameLayout.recordingState = ce::recording_indicator::SelectState(
        frameLayout.recordingActive, frameLayout.recordingAudioOnly, recordingStartIntent);
    uint64_t nowTick64 = GetTickCount64();
    if (cfg.showRecording && frameLayout.recordingActive) {
        int64_t startTime = sharedMem->runtimeState.recordingStartTime.load(std::memory_order_acquire);
        if (startTime > 0) {
            frameLayout.recordingSeconds = (nowTick64 - startTime) / 1000;
        }
        uint32_t overloadFlags = sharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
        const uint32_t captureHealthFlags =
            sharedMem->runtimeState.wgcCaptureHealthFlags.load(std::memory_order_relaxed);
        const uint32_t recordingHealthFlags =
            sharedMem->runtimeState.recordingHealthFlags.load(std::memory_order_relaxed);
        const uint32_t warningKind = ce::capture_policy::SelectWgcOverlayWarningKind(
            overloadFlags, captureHealthFlags, recordingHealthFlags);
        if (warningKind == ce::capture_policy::kOverlayWarningNone &&
            ce::capture_policy::IsWgcCaptureLimitedForOverlay(captureHealthFlags)) {
            lastEncoderOverloadTick = 0;
            lastRecordingWarningKind = ce::capture_policy::kOverlayWarningNone;
        } else if (warningKind != ce::capture_policy::kOverlayWarningNone) {
            lastEncoderOverloadTick = nowTick64;
            lastRecordingWarningKind = warningKind;
        }
    } else {
        lastEncoderOverloadTick = 0;
        lastRecordingWarningKind = ce::capture_policy::kOverlayWarningNone;
