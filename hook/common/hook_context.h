#pragma once

// Centralized Hook Context for CaptureEngine
// Consolidates all global state into a single, well-defined structure.
// Provides proper initialization ordering and thread-safe shutdown.

#include <atomic>
#include <memory>
#include <mutex>

#include "../../common/config.h"
#include "../../common/invariants.h"
#include "../../common/shared_defs.h"
#include "../../common/validation.h"
#include "ipc_client.h"
#include "lifecycle.h"

namespace ce {

// Forward declarations
class PerformanceMetrics;
class Overlay;
class FGDetector;

// Forward declaration for unique_ptr - we use void* instead to avoid incomplete
// type issues The actual PerformanceMetrics is not owned by HookContext during
// this migration phase

// Which graphics API is currently active (only one per process)
enum class ActiveGraphicsAPI : uint8_t { None = 0, DX9, DX10, DX11, DX12, Vulkan, OpenGL };

inline const char* GraphicsAPIName(ActiveGraphicsAPI api) {
    static const char* names[] = {"None", "DX9", "DX10", "DX11", "DX12", "Vulkan", "OpenGL"};
    auto idx = static_cast<size_t>(api);
    return idx <= 6 ? names[idx] : "?";
}

// API-specific context data (stored in union-like manner to save memory)
struct DX11Context {
    void* device = nullptr;         // ID3D11Device*
    void* deviceContext = nullptr;  // ID3D11DeviceContext*
    void* swapchain = nullptr;      // IDXGISwapChain*
};

struct DX12Context {
    void* device = nullptr;        // ID3D12Device*
    void* commandQueue = nullptr;  // ID3D12CommandQueue*
    void* swapchain = nullptr;     // IDXGISwapChain*
    uint32_t backBufferCount = 0;
};

struct VulkanContext {
    void* instance = nullptr;        // VkInstance
    void* physicalDevice = nullptr;  // VkPhysicalDevice
    void* device = nullptr;          // VkDevice
    void* queue = nullptr;           // VkQueue
    uint32_t queueFamilyIndex = 0;
};

struct OpenGLContext {
    void* hdc = nullptr;    // HDC
    void* hglrc = nullptr;  // HGLRC
};

struct DX9Context {
    void* device = nullptr;  // IDirect3DDevice9*
};

// Union of all API-specific contexts
union GraphicsContextData {
    DX9Context dx9;
    DX11Context dx11;
    DX12Context dx12;
    VulkanContext vulkan;
    OpenGLContext opengl;

    GraphicsContextData() {
        memset(this, 0, sizeof(*this));
    }
};

// Main hook context - owns all subsystem state
//
// MIGRATION STATUS (v1.1.0):
// This structure is gradually replacing scattered global variables.
// During migration, some state is duplicated between here and legacy globals.
// Always call SyncWithLegacyGlobals() after modifying either system's state.
//
// Legacy globals still in use:
//   - g_IPC: IPCClient pointer (HookContext references but doesn't own yet)
//   - g_pLocalConfig: AppConfig pointer (synced but not owned)
//   - g_DX11Hook/g_DX12Hook/etc: API-specific state (not yet migrated)
//   - g_SharedMem: Global shared memory pointer (use ctx->sharedMem instead)
//
// Future work:
//   - Move all device pointers into GraphicsContextData
//   - Have HookContext own all subsystems
//   - Remove legacy globals entirely
struct HookContext {
    // ========================================================================
    // Lifecycle state machines
    // ========================================================================
    HookLifecycle hookLifecycle;
    CaptureLifecycle captureLifecycle;
    OverlayLifecycle overlayLifecycle;

    // Note: EncoderLifecycle is owned by CaptureEngine (host process), not hook

    // ========================================================================
    // IPC and shared memory
    // ========================================================================
    std::unique_ptr<IPCClient> ipc;
    SharedMemoryLayout* sharedMem = nullptr;  // Mapped view (not owned)

    // ========================================================================
    // Configuration
    // ========================================================================
    std::unique_ptr<AppConfig> localConfig;
    bool debugLoggingEnabled = false;

    // ========================================================================
    // Graphics API state
    // ========================================================================
    ActiveGraphicsAPI activeAPI = ActiveGraphicsAPI::None;
    GraphicsContextData graphicsData;

    // ========================================================================
    // Subsystem pointers (lazy-initialized)
    // During migration phase, these are raw pointers not owned by HookContext
    // ========================================================================
    PerformanceMetrics* metrics = nullptr;  // Not owned during migration
    // Overlay and FGDetector are typically API-specific, stored elsewhere

    // ========================================================================
    // Thread safety
    // ========================================================================
    std::atomic<bool> shuttingDown{false};
    std::atomic<bool> graphicsOverridesActive{false};
    std::mutex initMutex;  // Protects initialization/shutdown

    // ========================================================================
    // Process info
    // ========================================================================
    char processName[260] = {0};
    DWORD processId = 0;
    HMODULE hookModule = nullptr;  // Our DLL handle

    // ========================================================================
    // Frame tracking
    // ========================================================================
    std::atomic<uint64_t> frameCount{0};

    // ========================================================================
    // Methods
    // ========================================================================

    // Initialize the hook context - call from deferred init, NOT DllMain
    bool Initialize(HMODULE hModule);

    // Shutdown in reverse order - safe to call multiple times
    void Shutdown();

    // Check if we're in a valid state for capture operations
    bool CanCapture() const {
        return hookLifecycle.IsActive() && captureLifecycle.CanCapture() &&
               !shuttingDown.load(std::memory_order_acquire);
    }

    // Check if we're in a valid state for overlay rendering
    bool CanRender() const {
        return hookLifecycle.IsActive() && overlayLifecycle.CanRender() &&
               !shuttingDown.load(std::memory_order_acquire);
    }

    // Set the active graphics API (only once per hook lifetime)
    bool SetActiveAPI(ActiveGraphicsAPI api);

    // Get active graphics config (local > IPC)
    GraphicsConfig GetActiveGraphicsConfig() const;

    // Increment frame counter and run periodic health checks
    void OnFrame();

private:
    bool initialized_ = false;
};

// Singleton accessor - returns null before Initialize() is called
HookContext* GetHookContext();

// Helper macros for common patterns
#define HOOK_CTX ::ce::GetHookContext()
#define HOOK_SHUTTING_DOWN (HOOK_CTX && HOOK_CTX->shuttingDown.load(std::memory_order_acquire))
#define HOOK_CAN_CAPTURE (HOOK_CTX && HOOK_CTX->CanCapture())
#define HOOK_CAN_RENDER (HOOK_CTX && HOOK_CTX->CanRender())

// ============================================================================
// Implementation
// ============================================================================

namespace detail {
inline HookContext* g_HookContext = nullptr;
inline std::atomic<bool> g_HookContextCreated{false};
}  // namespace detail

inline HookContext* GetHookContext() {
    return detail::g_HookContext;
}

// Create the singleton instance (call once from deferred init)
// Uses atomic flag instead of std::call_once to avoid linker issues with x86
inline HookContext* CreateHookContext() {
    bool expected = false;
    if (detail::g_HookContextCreated.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                             std::memory_order_acquire)) {
        static HookContext instance;
        detail::g_HookContext = &instance;
    }
    return detail::g_HookContext;
}

// Sync HookContext with legacy globals (for gradual migration)
// Call after creating HookContext to link it with existing global state
void SyncWithLegacyGlobals();

inline bool HookContext::Initialize(HMODULE hModule) {
    std::lock_guard<std::mutex> lock(initMutex);

    // Reject if shutdown is already in progress
    if (shuttingDown.load(std::memory_order_acquire)) {
        CE_LOG_WARN("HookCtx", "Initialize called during shutdown — ignoring");
        return false;
    }

    if (initialized_) {
        CE_LOG_WARN("HookCtx", "already initialized");
        return true;
    }

    hookModule = hModule;
    processId = GetCurrentProcessId();

    // Get process name
    if (GetModuleFileNameA(nullptr, processName, sizeof(processName)) == 0) {
        strncpy_s(processName, "unknown.exe", _TRUNCATE);
    } else {
        // Extract just the filename
        const char* lastSlash = strrchr(processName, '\\');
        if (lastSlash) {
            memmove(processName, lastSlash + 1, strlen(lastSlash + 1) + 1);
        }
    }

    CE_LOG_INFO("HookCtx", "init pid=%lu exe=%s", processId, processName);

    // Transition to Connecting state
    if (!hookLifecycle.TransitionTo(HookState::Connecting)) {
        CE_LOG_ERROR("HookCtx", "failed to transition to Connecting");
        return false;
    }

    // Create IPC client
    ipc = std::make_unique<IPCClient>();
    if (!ipc->Connect()) {
        CE_LOG_ERROR("HookCtx", "IPC connect failed");
        hookLifecycle.TransitionTo(HookState::Disconnecting);
        hookLifecycle.TransitionTo(HookState::Detached);
        return false;
    }

    // Get shared memory pointer from IPC
    sharedMem = ipc->GetSharedMem();
    if (!sharedMem) {
        CE_LOG_ERROR("HookCtx", "shared memory null");
        ipc->Disconnect();
        hookLifecycle.TransitionTo(HookState::Disconnecting);
        hookLifecycle.TransitionTo(HookState::Detached);
        return false;
    }

    // Validate shared memory
    if (!ValidateSharedMemory(sharedMem)) {
        CE_LOG_ERROR("HookCtx", "shared memory validation failed");
        ipc->Disconnect();
        hookLifecycle.TransitionTo(HookState::Disconnecting);
        hookLifecycle.TransitionTo(HookState::Detached);
        return false;
    }

    // Load local config (optional - doesn't fail init)
    localConfig = std::make_unique<AppConfig>();
    // Config loading happens elsewhere based on process name

    // Enable debug logging from config if set
    if (sharedMem && sharedMem->GetDebugLogging()) {
        debugLoggingEnabled = true;
        g_DebugLoggingEnabled = true;
    }

    // Transition to Connected
    if (!hookLifecycle.TransitionTo(HookState::Connected)) {
        CE_LOG_ERROR("HookCtx", "failed to transition to Connected");
        Shutdown();
        return false;
    }

    initialized_ = true;
    CE_LOG_INFO("HookCtx", "init complete");
    return true;
}

inline void HookContext::Shutdown() {
    // Set shutdown flag first (acquire-release for visibility)
    bool expected = false;
    if (!shuttingDown.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        // Already shutting down
        return;
    }

    std::lock_guard<std::mutex> lock(initMutex);

    CE_LOG_INFO("HookCtx", "shutdown begin");

    // Transition through shutdown states
    auto state = hookLifecycle.GetState();

    if (state == HookState::Active) {
        hookLifecycle.TransitionTo(HookState::Detaching);
    }

    // Clean up capture lifecycle
    if (captureLifecycle.GetState() != CaptureState::Uninitialized &&
        captureLifecycle.GetState() != CaptureState::CleaningUp) {
        // Force to cleaning up state
        auto capState = captureLifecycle.GetState();
        if (capState == CaptureState::Error) {
            captureLifecycle.TransitionTo(CaptureState::CleaningUp);
        } else if (capState != CaptureState::CleaningUp) {
            // Try valid transitions
            if (capState == CaptureState::Capturing) {
                captureLifecycle.TransitionTo(CaptureState::Ready);
            }
            captureLifecycle.TransitionTo(CaptureState::CleaningUp);
        }
    }

    // Clean up overlay lifecycle
    if (overlayLifecycle.GetState() != OverlayState::Uninitialized &&
        overlayLifecycle.GetState() != OverlayState::CleaningUp) {
        auto ovlState = overlayLifecycle.GetState();
        if (ovlState == OverlayState::Rendering) {
            overlayLifecycle.TransitionTo(OverlayState::Ready);
        }
        if (overlayLifecycle.GetState() != OverlayState::CleaningUp) {
            overlayLifecycle.TransitionTo(OverlayState::CleaningUp);
        }
    }

    // Release resources in reverse order
    // During migration, metrics is not owned so we just nullify the reference
    metrics = nullptr;
    localConfig.reset();

    // Disconnect IPC
    if (ipc) {
        ipc->Disconnect();
        ipc.reset();
    }
    sharedMem = nullptr;

    // Final state transitions
    state = hookLifecycle.GetState();
    if (state == HookState::Detaching) {
        hookLifecycle.TransitionTo(HookState::Disconnecting);
    }
    if (hookLifecycle.GetState() == HookState::Disconnecting) {
        hookLifecycle.TransitionTo(HookState::Detached);
    }

    // Reset lifecycle states
    if (captureLifecycle.GetState() == CaptureState::CleaningUp) {
        captureLifecycle.TransitionTo(CaptureState::Uninitialized);
    }
    if (overlayLifecycle.GetState() == OverlayState::CleaningUp) {
        overlayLifecycle.TransitionTo(OverlayState::Uninitialized);
    }

    // Clear graphics data
    activeAPI = ActiveGraphicsAPI::None;
    memset(&graphicsData, 0, sizeof(graphicsData));

    initialized_ = false;
    CE_LOG_INFO("HookCtx", "shutdown complete");
}

inline bool HookContext::SetActiveAPI(ActiveGraphicsAPI api) {
    std::lock_guard<std::mutex> lock(initMutex);
    if (activeAPI != ActiveGraphicsAPI::None && activeAPI != api) {
        CE_LOG_ERROR("HookCtx", "API already set to %s, cannot change to %s", GraphicsAPIName(activeAPI),
                     GraphicsAPIName(api));
        return false;
    }

    if (activeAPI == api) {
        return true;  // Already set
    }

    activeAPI = api;
    CE_LOG_INFO("HookCtx", "active API set to %s", GraphicsAPIName(api));

    // Transition to Attaching if we're Connected
    if (hookLifecycle.IsInState(HookState::Connected)) {
        hookLifecycle.TransitionTo(HookState::Attaching);
    }

    return true;
}

inline GraphicsConfig HookContext::GetActiveGraphicsConfig() const {
    // Local config takes priority - check if any overrides are set
    if (localConfig && !localConfig->graphics.vsyncMode.empty()) {
        return localConfig->graphics;
    }
    // Return from shared mem graphics config - convert SharedGraphicsConfig to
    // GraphicsConfig
    if (sharedMem) {
        GraphicsConfig result;
        result.vsyncMode = sharedMem->graphicsConfig.vsyncMode;
        result.anisotropicFiltering = sharedMem->graphicsConfig.anisotropicFiltering;
        result.samplerOverrideMode =
            sharedMem->graphicsConfig.samplerOverrideMode[0] ? sharedMem->graphicsConfig.samplerOverrideMode : "safe";
        result.mipMapping = sharedMem->graphicsConfig.mipMapping;
        result.mipBias = sharedMem->graphicsConfig.mipBias;
        result.mipBiasMode = sharedMem->graphicsConfig.mipBiasMode;
        result.forceMipBiasClamp = sharedMem->graphicsConfig.forceMipBiasClamp;
        result.msaaSamples = sharedMem->graphicsConfig.msaaSamples;
        result.nvLodSpreadFix = sharedMem->graphicsConfig.nvLodSpreadFix;
        result.forceRayReconstruction = sharedMem->graphicsConfig.forceRayReconstruction;
        result.cpuPrerenderLimit = sharedMem->graphicsConfig.prerenderLimit;
        result.backbufferCount = sharedMem->graphicsConfig.backbufferCount;
        result.frameLatency = sharedMem->graphicsConfig.frameLatency;
        result.sgssaa = sharedMem->graphicsConfig.sgssaa;
        return result;
    }
    return GraphicsConfig{};
}

inline void HookContext::OnFrame() {
    [[maybe_unused]] uint64_t frame = frameCount.fetch_add(1, std::memory_order_relaxed);

    // Periodic health check in debug builds
#ifdef _DEBUG
    if (debugLoggingEnabled && (frame % 1000) == 0) {
        PeriodicHealthCheck(sharedMem, frame);
    }
#endif
}

}  // namespace ce
