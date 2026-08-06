#include "overlay_adapter_internal.h"

// Global adapter instance
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
OverlayAdapter g_OverlayAdapter;

void OverlayAdapter::RememberDpiReferenceHwnd(void* hwnd) {
    HWND h = reinterpret_cast<HWND>(hwnd);
    if (IsWindow(h)) {
        overlay_adapter_g_SharedOverlayDpiHwnd.store(h, std::memory_order_release);
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
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            return dpi / 96.0f;
        }
    }

    // Fallback to GetDeviceCaps
    HDC hdc = GetDC(hwnd);
    if (hdc) {
        int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(hwnd, hdc);
        if (dpiX > 0) {
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            return dpiX / 96.0f;
        }
    }

    // Default to 1.0 if detection fails
    return 1.0f;
}

OverlayAdapter::OverlayAdapter() noexcept {
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
