#include <combaseapi.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <unknwn.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../../common/frame_timing.h"
#include "../../common/raii_helpers.h"
#include "../capture/shared_capture.h"
#include "../common/capture_base.h"
#include "../common/fg_detection.h"
#include "../common/hook_common.h"
#include "../common/input_manager.h"
#include "../common/overlay_adapter.h"
#include "../common/performance_metrics.h"
#include "../common/streamline_compat.h"

#include "../common/fps_limiter.h"
#include "../common/freeze_watchdog.h"
#include "../common/perf_logger.h"
#include "../common/swapchain_wrapper.h"
#include "../common/system_metrics.h"
#include "../wrappers/d3d12_wrapper_interface.h"
#include "../wrappers/dxgi_swapchain_wrap.h"
#include "../wrappers/root_signature_parser.h"
#include "../wrappers/wrapper_hooks.h"
#include "dx11_hook.h"
#include "dx12_hook.h"
#include "ffx_hook.h"
#include "graphics_hook.h"
#include "lod_helper.h"

#include "../wrappers/vtable_hook.h"
#include "../wrappers/wrapper_base.h"
#include "dxgi_shared.h"

// ============================================================================
// SpecialK-style Streamline Handling
// ============================================================================

// ============================================================================
// Typedefs for D3D12 functions
typedef void(STDMETHODCALLTYPE* ExecuteCommandListsPtr)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
typedef void(STDMETHODCALLTYPE* CreateSamplerPtr)(ID3D12Device*, const D3D12_SAMPLER_DESC*,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE);
typedef HRESULT(STDMETHODCALLTYPE* CreateCommittedResourcePtr)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*,
                                                               D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*,
                                                               D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID,
                                                               void**);
typedef HRESULT(WINAPI* PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(const D3D12_ROOT_SIGNATURE_DESC*,
                                                            D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI* PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*,
                                                                      ID3DBlob**, ID3DBlob**);

// Global Function Pointers for detours (Visible to other modules)
ExecuteCommandListsPtr oExecuteCommandLists = nullptr;
CreateSamplerPtr oCreateSampler = nullptr;
CreateCommittedResourcePtr oCreateCommittedResource = nullptr;
PFN_D3D12_SERIALIZE_ROOT_SIGNATURE oSerializeRootSignature = nullptr;
PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE oSerializeVersionedRootSignature = nullptr;
static std::recursive_mutex g_ExecuteCommandListsHookStateMutex;
static std::map<void**, ExecuteCommandListsPtr> g_ExecuteCommandListsOriginalByVTable;

// SwapChain Detour Pointers
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                        IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND,
                                                               const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                               IDXGISwapChain1**);

static PFN_CreateSwapChain oCreateSwapChain = nullptr;
static PFN_CreateSwapChainForHwnd oCreateSwapChainForHwnd = nullptr;

// --- DX12 Overlay State Management ---
struct DX12OverlayState {
    // Large pool size ensures we never need to wait for GPU.
    // Even at 60fps with 100ms GPU latency, only 6 allocators are in flight.
    // 16 provides 2.5x headroom - allocator is always ready, zero waiting.
    static const int ALLOC_POOL_SIZE = 16;
    std::vector<ID3D12CommandAllocator*> allocators;
    ID3D12GraphicsCommandList* cmdList = nullptr;
    int allocIndex = 0;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    UINT64 currentFenceValue = 0;
    std::vector<UINT64> fenceValues;
    ID3D12DescriptorHeap* rtvDescHeap = nullptr;
    ID3D12DescriptorHeap* srvDescHeap = nullptr;
    UINT rtvDescriptorSize = 0;
    std::vector<ID3D12Resource*> backBuffers;
    bool overlayInit = false;
    bool syncInit = false;
    int cachedWidth = 0;
    int cachedHeight = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT bufferCount = 0;
    IDXGISwapChain* cachedSwapChain = nullptr;

    // Dedicated overlay command queue for FG-safe rendering.
    // When FG is active, overlay commands execute on this queue with CPU-side
    // fence synchronization to avoid interfering with Streamline's game queue
    // management.  When FG is not active, overlay commands go on the game queue
    // directly (zero CPU waits).
    ID3D12CommandQueue* overlayQueue = nullptr;

    // Cross-queue fence: game queue signals to mark work completion, then
    // CPU-side wait before submitting overlay work on the overlay queue.
    // GPU-side CommandQueue::Wait was removed (NVIDIA WaitImpl Alt+Tab hang).
    ID3D12Fence* crossQueueFence = nullptr;
    UINT64 crossQueueFenceValue = 0;
    HANDLE crossQueueFenceEvent = nullptr;

    void Cleanup() {
        // FG-SAFE: backBuffers no longer holds references
        backBuffers.clear();
        if (rtvDescHeap) {
            rtvDescHeap->Release();
            rtvDescHeap = nullptr;
        }
        if (srvDescHeap) {
            srvDescHeap->Release();
            srvDescHeap = nullptr;
        }
        for (auto* alloc : allocators)
            if (alloc)
                alloc->Release();
        allocators.clear();
        if (cmdList) {
            cmdList->Release();
            cmdList = nullptr;
        }
        if (fence) {
            fence->Release();
            fence = nullptr;
        }
        if (fenceEvent) {
            CloseHandle(fenceEvent);
            fenceEvent = nullptr;
        }
        // CRITICAL: Release dedicated overlay queue
        if (overlayQueue) {
            overlayQueue->Release();
            overlayQueue = nullptr;
        }
        // Release cross-queue synchronization fence and event
        if (crossQueueFenceEvent) {
            CloseHandle(crossQueueFenceEvent);
            crossQueueFenceEvent = nullptr;
        }
        if (crossQueueFence) {
            crossQueueFence->Release();
            crossQueueFence = nullptr;
        }
        overlayInit = false;
        syncInit = false;
        crossQueueFenceValue = 0;
    }
};

static DX12OverlayState g_State;
static SharedCaptureD3D12 g_SharedCaptureD3D12;

// CRITICAL FIX: Use atomic pointers for thread-safe access
// These are read/written from multiple threads (hook thread, present thread, etc.)
std::atomic<ID3D12Device*> g_Device{nullptr};
std::atomic<ID3D12CommandQueue*> g_CommandQueue{nullptr};
std::recursive_mutex g_CommandQueueMutex;

// CRITICAL FIX: Thread-safe accessors for g_Device and g_CommandQueue
// These functions acquire the mutex and return a reference-counted pointer
// to prevent use-after-free when the queue/device is destroyed on another
// thread
struct DX12Context {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;

    DX12Context() = default;

    DX12Context(ID3D12Device* d, ID3D12CommandQueue* q) : device(d), queue(q) {
        if (device)
            device->AddRef();
        if (queue)
            queue->AddRef();
    }

    ~DX12Context() {
        if (device) {
            device->Release();
            device = nullptr;
        }
        if (queue) {
            queue->Release();
            queue = nullptr;
        }
    }

    // Disable copy to prevent accidental double-release
    DX12Context(const DX12Context&) = delete;
    DX12Context& operator=(const DX12Context&) = delete;

    // Enable move
    DX12Context(DX12Context&& other) noexcept : device(other.device), queue(other.queue) {
        other.device = nullptr;
        other.queue = nullptr;
    }

    DX12Context& operator=(DX12Context&& other) noexcept {
        if (this != &other) {
            if (device)
                device->Release();
            if (queue)
                queue->Release();
            device = other.device;
            queue = other.queue;
            other.device = nullptr;
            other.queue = nullptr;
        }
        return *this;
    }

    bool IsValid() const {
        return device != nullptr && queue != nullptr;
    }
};

// Thread-safe accessor - ALWAYS use this instead of direct
// g_Device/g_CommandQueue access
static DX12Context GetDX12Context() {
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    return DX12Context(g_Device.load(), g_CommandQueue.load());
}

static std::atomic<uint64_t> g_FrameIndex{0};
static std::atomic<int> g_CommandListsExecutedThisFrame{0};
static std::atomic<uint64_t> g_FGDebugFrameCount{0};

// Last swapchain reference for device change detection
static IDXGISwapChain* g_LastSwapChain = nullptr;
// Pending swapchain cleanup - released after ResizeBuffers completes
static IDXGISwapChain* g_PendingSwapChainCleanup = nullptr;

// IPC ready flag
static bool g_IPCReady = false;

ID3D12Resource* g_DummyBackBuffer = nullptr;

// Swapchain queue - captured at swapchain creation time, preferred for overlay
// rendering to ensure barriers execute on the queue DXGI synchronises with.
static ID3D12CommandQueue* g_SwapchainQueue = nullptr;

// Guard flag: skip queue capture during temp swapchain creation
static std::atomic<bool> g_CreatingTempSwapchain{false};

// LOCK HIERARCHY (MUST be acquired in this order to prevent deadlocks):
// 1. g_OverlayMutex (outermost - protects overlay state)
// 2. g_CommandQueueMutex (protects command queue pointer)
// 3. g_DX12CaptureMutex (innermost - protects capture state)
//
// Rule: When acquiring multiple locks, always acquire in order above.
//       Use std::lock_guard with std::adopt_lock when using try_lock().
static std::recursive_mutex g_OverlayMutex;
static std::recursive_mutex g_DX12CaptureMutex;
static std::atomic<bool> g_InSwapchainResizeCleanup{false};

// Frame counter for post-ImGui-init delay (skip first frame to let GPU
// stabilize)
static std::atomic<int> s_framesSinceInit{0};

// Use pointer to prevent static destructor execution in non-game processes
// (Explorer fix)
DX12Hook* g_dx12HookInstance = nullptr;

std::recursive_mutex g_DeviceQueuesMutex;
std::map<ID3D12Device*, ID3D12CommandQueue*> g_DeviceQueues;

// CPU Prerender Limit State (DX12)
static std::vector<ID3D12Fence*> g_PrerenderFences;
static std::vector<HANDLE> g_PrerenderEvents;
static uint64_t g_PrerenderFrameIndex = 0;
static std::mutex g_PrerenderMutex;

// Re-entrancy guard: set when the current thread is inside DetourECL.
// During Alt+Tab, D3D12's internal WaitImpl inside ECL can pump window messages
// (DefWindowProc), which may trigger Present → ProcessFrame.  If ProcessFrame
// submits an overlay ECL while the outer ECL is still inside WaitImpl, a second
// WaitImpl cascades and the render thread hangs.  ProcessFrame checks this flag
// and skips overlay rendering when it's set.
static thread_local bool s_insideECL = false;

// Forward Declarations
void STDMETHODCALLTYPE DetourExecuteCommandLists(ID3D12CommandQueue* pThis, UINT NumCommandLists,
                                                 ID3D12CommandList* const* ppCommandLists);
void DX12_HookQueueVTable(ID3D12CommandQueue* queue);
void DX12_HookDeviceVTable(ID3D12Device* device);

static ExecuteCommandListsPtr GetOriginalExecuteCommandLists(ID3D12CommandQueue* queue) {
    if (!queue)
        return oExecuteCommandLists;

    void** vtbl = *reinterpret_cast<void***>(queue);
    if (!vtbl)
        return oExecuteCommandLists;

    std::lock_guard<std::recursive_mutex> lock(g_ExecuteCommandListsHookStateMutex);
    auto it = g_ExecuteCommandListsOriginalByVTable.find(vtbl);
    if (it != g_ExecuteCommandListsOriginalByVTable.end())
        return it->second;

    return oExecuteCommandLists;
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory* pThis, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                                IDXGISwapChain** ppSwapChain);
HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd,
                                                       const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc, IDXGIOutput* pOut,
                                                       IDXGISwapChain1** ppSC);
void STDMETHODCALLTYPE DetourCreateSampler(ID3D12Device* pDevice, const D3D12_SAMPLER_DESC* pDesc,
                                           D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);

extern "C" BOOL WINAPI ApplyDX12SamplerOverridesCallback(D3D12_SAMPLER_DESC* pDesc);

// REQUIRED EXPORTS
void DX12_AdjustWrapperResizeDepth(int delta) {
    if (delta > 0)
        DXGIShared::g_SharedState.wrapperResizeDepth.fetch_add(delta);
    else
        DXGIShared::g_SharedState.wrapperResizeDepth.fetch_sub(-delta);
}

// Forward declaration for CleanupRTVs
void CleanupRTVs();

void DX12_InvalidateSwapchain() {
    DXGIShared::g_SharedState.swapchainInvalid.store(true, std::memory_order_release);
    HookLog("DX12: Swapchain marked INVALID (FSR/FG transition detected)");
    // Log current state for debugging
    HookLog("DX12: Invalidating - overlayInit=%d, syncInit=%d, device=%p, queue=%p", g_State.overlayInit,
            g_State.syncInit, g_Device.load(), g_CommandQueue.load());

    // Only invalidate swapchain-level state, not device-level sync resources
    // This allows swapchain changes without full reinitialization
    if (g_State.overlayInit) {
        HookLog(
            "DX12: Invalidating swapchain resources (device-level resources "
            "preserved)");
        g_State.overlayInit = false;
        CleanupRTVs();
    }
}

void DX12_SignalFSR4SwapchainRecreated() {
    DXGIShared::g_SharedState.fsr4RecreationPending.store(true, std::memory_order_release);
    HookLog("DX12: FSR4 swapchain recreation signaled");
}

// C Linkage Exports for cross-module calls (e.g. from C clients or
// GetProcAddress)
extern "C" {
void DX12_SetCommandQueue(ID3D12CommandQueue* pQueue) {
    if (!pQueue)
        return;

    // CRITICAL FIX: Only allow DIRECT queues for overlay rendering.
    // Strange Brigade and other DX12 games use Async Compute queues.
    // Submitting overlay (Direct) commands to a Compute queue causes a device
    // lost/crash.
    D3D12_COMMAND_QUEUE_DESC desc = pQueue->GetDesc();
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        // HookLog("DX12: Ignoring non-direct queue (Type=%d)", desc.Type);
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    if (g_CommandQueue.load() != pQueue) {
        if (g_CommandQueue.load())
            g_CommandQueue.load()->Release();
        g_CommandQueue.store(pQueue);
        pQueue->AddRef();
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(pQueue->GetDevice(IID_PPV_ARGS(&dev)))) {
            if (g_Device.load() != dev) {
                if (g_Device.load())
                    g_Device.load()->Release();
                g_Device.store(dev);
            } else
                dev->Release();
        }
    }

    // CRITICAL FIX: Hook queue vtable lazily here instead of during swapchain
    // creation This prevents hangs during DXGI internal operations
    DX12_HookQueueVTable(pQueue);
}
// Capture the queue that was passed to CreateSwapChain* so we can prefer it
// for overlay submission.  Only accepts DIRECT queues (same rule as
// DX12_SetCommandQueue).  Also hooks the queue vtable for ECL interception.
static void DX12_SetSwapchainQueue(ID3D12CommandQueue* pQueue) {
    if (!pQueue)
        return;

    D3D12_COMMAND_QUEUE_DESC desc = pQueue->GetDesc();
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return;

    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    if (g_SwapchainQueue != pQueue) {
        if (g_SwapchainQueue)
            g_SwapchainQueue->Release();
        g_SwapchainQueue = pQueue;
        g_SwapchainQueue->AddRef();
        HookLog("DX12: Swapchain queue captured (queue=%p)", pQueue);
    }

    // Also hook the vtable so ECL fires for this queue
    DX12_HookQueueVTable(pQueue);
}

void DX12_AdjustWrapperResizeDepth_C(int delta) {
    DX12_AdjustWrapperResizeDepth(delta);
}

// Export for D3D12 wrapper to notify command list execution (frame
// classification)
void DX12_NotifyCommandLists(UINT numCommandLists) {
    g_CommandListsExecutedThisFrame.fetch_add(numCommandLists, std::memory_order_relaxed);
}
}

void DX12_OnSwapchainResizeEnd();
void CleanupOverlay();
void CleanupRTVs();
void DX12_InvalidateSwapchain();

// Helper to ensure global hook instance exists
void EnsureDX12Hook() {
    if (!g_dx12HookInstance) {
        g_dx12HookInstance = new DX12Hook();
    }
}

// Forward declarations
static void InstallGlobalVTableHooks();
static void HookSwapchainVTableViaTempSwapchain(bool presentOnly = false);
static void FindAndWrapPreExistingSwapchains();

void DX12Hook::Init() {
    EnsureDX12Hook();  // Self-init check
    static std::recursive_mutex s_InitMutex;
    static bool s_InitDone = false;
    std::lock_guard<std::recursive_mutex> lock(s_InitMutex);
    if (s_InitDone)
        return;
    s_InitDone = true;

    // CRITICAL FIX: Check if Vulkan is active before installing ANY DXGI hooks
    // Vulkan games using WSI-to-DXGI mapping can freeze if we hook DXGI
    HMODULE hVulkan = GetModuleHandleW(L"vulkan-1.dll");
    if (hVulkan) {
        HookLog(
            "DX12: Vulkan detected (vulkan-1.dll), SKIPPING ALL DXGI hook "
            "installation");
        return;
    }

    // Note: Crash handler is installed in DllMain (hook/main.cpp)

    // Start freeze detection watchdog with dynamic timeout based on game engine
    // The watchdog auto-detects UE5, DLSS FG and uses extended timeouts
    double timeout = g_RenderWatchdog.GetRecommendedTimeout();
    g_RenderWatchdog.SetMonitoredThread(GetCurrentThreadId());
    g_RenderWatchdog.Start(timeout);
    HookLog("DX12: Freeze watchdog started (%.0f second timeout)", timeout);

    // CRITICAL FIX: Install global swapchain vtable hooks by getting the vtable
    // directly from the DXGI module. This avoids creating a temp swapchain which
    // causes deadlocks with Steam overlay + Streamline.
    InstallGlobalVTableHooks();

    // NOTE: HookSwapchainVTableViaTempSwapchain() is NOT called here.
    // It is deferred to EnsurePresentHooks(), which is called from
    // Wrapped_D3D12CreateDevice only after the game has confirmed D3D12 usage.
    // This prevents creating a temp D3D12 device in DX11-only apps (which load
    // d3d12.dll via D3D11On12), which would corrupt shared DXGI internal state
    // and crash the DX11 swap chain.

    HookLog("DX12Hook: Initialized (factory hooks installed; Present hooks deferred)");

    FindAndWrapPreExistingSwapchains();
}

void DX12Hook::EnsurePresentHooks() {
    static std::atomic<bool> s_done{false};
    bool expected = false;
    if (!s_done.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;  // Already installed
    }
    HookLog("DX12: Installing Present inline hooks (D3D12 device created by game)");
    HookSwapchainVTableViaTempSwapchain();
    HookLog("DX12: Present inline hooks installed");
}

static void FindAndWrapPreExistingSwapchains() {
    // Pre-existing swapchains (created before injection) are now handled via
    // inline hooks on Present/Present1. The inline hook approach:
    // 1. Patches the function code in memory (not the vtable)
    // 2. Creates a trampoline that executes the original instructions
    // 3. Calling the trampoline GUARANTEED bypasses the hook - no re-entry
    //
    // This works for both pre-existing swapchains AND wrapped swapchains.
    // For wrapped swapchains, DetourPresent detects the wrapper and passes
    // through. For pre-existing swapchains, DetourPresent processes the frame
    // normally.
    HookLog("DX12: Pre-existing swapchain support enabled via inline Present hooks");
}

// Function pointers for global factory vtable hooks
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                        IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND,
                                                               const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                               IDXGISwapChain1**);

static PFN_CreateSwapChain oCreateSwapChainGlobal = nullptr;
static PFN_CreateSwapChainForHwnd oCreateSwapChainForHwndGlobal = nullptr;

// Detour for global CreateSwapChain hook
static HRESULT STDMETHODCALLTYPE DetourCreateSwapChainGlobal(IDXGIFactory* pThis, IUnknown* pDevice,
                                                             DXGI_SWAP_CHAIN_DESC* pDesc,
                                                             IDXGISwapChain** ppSwapChain) {
    // CRITICAL: Pass through during shutdown
    if (HookIsShuttingDown()) {
        if (oCreateSwapChainGlobal)
            return oCreateSwapChainGlobal(pThis, pDevice, pDesc, ppSwapChain);
        return E_FAIL;
    }

    HookLog("DetourCreateSwapChainGlobal: CALLED (factory=%p, device=%p)", pThis, pDevice);

    // Call original first
    HRESULT hr = oCreateSwapChainGlobal(pThis, pDevice, pDesc, ppSwapChain);

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        // Log swapchain details
        if (pDesc) {
            HookLog("DetourCreateSwapChainGlobal: Creating swapchain %ux%u", pDesc->BufferDesc.Width,
                    pDesc->BufferDesc.Height);
        }

        // NOTE: We don't install global Present vtable hooks for DX12.
        // The wrapper (CWrapDXGISwapChain) handles all Present interception.
        // This avoids conflicts between vtable hooks and wrapper interception
        // that caused stack overflow crashes.

        // CRITICAL: Check if this swapchain is already wrapped
        // This prevents double-wrapping which causes infinite Present recursion
        void* pExistingWrapper = nullptr;
        if (SUCCEEDED((*ppSwapChain)->QueryInterface(IID_CWrapDXGISwapChain, &pExistingWrapper))) {
            ((IUnknown*)pExistingWrapper)->Release();
            HookLog(
                "DetourCreateSwapChainGlobal: Swapchain already wrapped, "
                "skipping double-wrap");
            return hr;
        }

        // Wrap the swapchain with CWrapDXGISwapChain
        HookLog("DetourCreateSwapChainGlobal: Wrapping swapchain %p", *ppSwapChain);
        auto* wrapper = new CWrapDXGISwapChain(*ppSwapChain, pDevice);
        *ppSwapChain = wrapper;
        HookLog("DetourCreateSwapChainGlobal: Swapchain wrapped successfully");

        // For DX12, capture the swapchain queue so overlay uses the correct queue
        ID3D12CommandQueue* pQueue = nullptr;
        if (pDevice && SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&pQueue)))) {
            DX12_SetSwapchainQueue(pQueue);
            pQueue->Release();
        }
    }

    return hr;
}

// Detour for global CreateSwapChainForHwnd hook
static HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwndGlobal(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd,
                                                                    const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc,
                                                                    IDXGIOutput* pOut, IDXGISwapChain1** ppSC) {
    // CRITICAL: Pass through during shutdown
    if (HookIsShuttingDown()) {
        if (oCreateSwapChainForHwndGlobal)
            return oCreateSwapChainForHwndGlobal(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
        return E_FAIL;
    }

    HookLog(
        "DetourCreateSwapChainForHwndGlobal: CALLED (factory=%p, device=%p, "
        "hwnd=%p)",
        pThis, pDevice, hWnd);

    HRESULT hr = oCreateSwapChainForHwndGlobal(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);

    if (SUCCEEDED(hr) && ppSC && *ppSC) {
        // Log swapchain details
        if (pDesc) {
            HookLog("DetourCreateSwapChainForHwndGlobal: Creating swapchain %ux%u", pDesc->Width, pDesc->Height);
        }

        // NOTE: We don't install global Present vtable hooks for DX12.
        // The wrapper (CWrapDXGISwapChain) handles all Present interception.
        // This avoids conflicts between vtable hooks and wrapper interception
        // that caused stack overflow crashes.

        // CRITICAL: Check if this swapchain is already wrapped
        // This prevents double-wrapping which causes infinite Present recursion
        void* pExistingWrapper = nullptr;
        if (SUCCEEDED((*ppSC)->QueryInterface(IID_CWrapDXGISwapChain, &pExistingWrapper))) {
            ((IUnknown*)pExistingWrapper)->Release();
            HookLog(
                "DetourCreateSwapChainForHwndGlobal: Swapchain already wrapped, "
                "skipping double-wrap");
            return hr;
        }

        HookLog("DetourCreateSwapChainForHwndGlobal: Wrapping swapchain %p", *ppSC);
        auto* wrapper = new CWrapDXGISwapChain(*ppSC, pDevice);
        *ppSC = (IDXGISwapChain1*)wrapper;
        HookLog("DetourCreateSwapChainForHwndGlobal: Swapchain wrapped successfully");

        ID3D12CommandQueue* pQueue = nullptr;
        if (pDevice && SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&pQueue)))) {
            DX12_SetSwapchainQueue(pQueue);
            pQueue->Release();
        }
    }

    return hr;
}

// Install global hooks on the DXGI factory to catch ALL swapchain creation
// This hooks the factory vtable directly in the DXGI module
static void InstallGlobalVTableHooks() {
    HookLog("DX12: InstallGlobalVTableHooks called");

    // CRITICAL: Install global factory vtable hooks to catch swapchain creation
    // even for factories created before our IAT hooks were installed.
    // This ensures ALL swapchains get wrapped regardless of timing.

    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    if (!hDXGI) {
        HookLog("DX12: DXGI module not loaded, skipping factory vtable hooks");
        return;
    }

    // Get CreateDXGIFactory1 export to create a temp factory
    typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID, void**);
    PFN_CreateDXGIFactory1 pCreateFactory = (PFN_CreateDXGIFactory1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
    if (!pCreateFactory) {
        HookLog("DX12: CreateDXGIFactory1 not found");
        return;
    }

    // Create a temp factory to get its vtable
    IDXGIFactory2* pFactory = nullptr;
    HRESULT hr = pCreateFactory(IID_PPV_ARGS(&pFactory));
    if (FAILED(hr) || !pFactory) {
        HookLog("DX12: Failed to create temp factory for vtable extraction");
        return;
    }

    // Get the vtable - ALL IDXGIFactory instances share this vtable
    void** vtable = *(void***)pFactory;
    HookLog("DX12: Factory vtable at %p", vtable);

    // Hook CreateSwapChain (vtable[10] for IDXGIFactory)
    // Hook CreateSwapChainForHwnd (vtable[15] for IDXGIFactory2)
    if (VTableHook::Create(&vtable[10], (LPVOID)DetourCreateSwapChainGlobal, (LPVOID*)&oCreateSwapChainGlobal)) {
        HookLog("DX12: Hooked global CreateSwapChain at vtable[10]");
    }

    if (VTableHook::Create(&vtable[15], (LPVOID)DetourCreateSwapChainForHwndGlobal,
                           (LPVOID*)&oCreateSwapChainForHwndGlobal)) {
        HookLog("DX12: Hooked global CreateSwapChainForHwnd at vtable[15]");
    }

    pFactory->Release();

    HookLog("DX12: Global factory vtable hooks installed");
}

void RemoveGlobalVTableHooks() {
    if (!oCreateSwapChainGlobal && !oCreateSwapChainForHwndGlobal) {
        return;
    }

    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    if (!hDXGI) {
        HookLog("DX12: DXGI module not loaded, skipping vtable hook removal");
        return;
    }

    typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID, void**);
    PFN_CreateDXGIFactory1 pCreateFactory = (PFN_CreateDXGIFactory1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
    if (!pCreateFactory) {
        HookLog("DX12: CreateDXGIFactory1 not found for vtable hook removal");
        return;
    }

    IDXGIFactory2* pFactory = nullptr;
    HRESULT hr = pCreateFactory(IID_PPV_ARGS(&pFactory));
    if (FAILED(hr) || !pFactory) {
        HookLog("DX12: Failed to create factory for vtable hook removal");
        return;
    }

    void** vtable = *(void***)pFactory;

    if (oCreateSwapChainGlobal) {
        VTableHook::Remove(&vtable[10], (void*)oCreateSwapChainGlobal);
        HookLog("DX12: Removed CreateSwapChain vtable hook");
        oCreateSwapChainGlobal = nullptr;
    }

    if (oCreateSwapChainForHwndGlobal) {
        VTableHook::Remove(&vtable[15], (void*)oCreateSwapChainForHwndGlobal);
        HookLog("DX12: Removed CreateSwapChainForHwnd vtable hook");
        oCreateSwapChainForHwndGlobal = nullptr;
    }

    pFactory->Release();
    HookLog("DX12: Global factory vtable hooks removed");
}

// Install Present vtable hooks for pre-existing swapchains (late injection)
// DISABLED: Global Present vtable hooks cause shutdown crashes
// Factory wrapping is now the primary mechanism for intercepting swapchains
void DX12_InstallPresentHooksForSwapchain(IDXGISwapChain* pSwapChain) {
    // DISABLED: Present vtable hooks are disabled to prevent crashes
    // Pre-existing swapchains (created before injection) won't have overlay
    (void)pSwapChain;
}

// Install inline hooks on Present/Present1 via temp swapchain creation.
// Inline hooks patch the function code in memory, creating a trampoline that
// bypasses the hook entirely. This solves the re-entry problem with vtable
// hooks. presentOnly: if true, only install Present hooks (defer ResizeBuffers
// for Strange Brigade)
static void HookSwapchainVTableViaTempSwapchain(bool presentOnly) {
    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    HMODULE hD3D12 = GetModuleHandleA("d3d12.dll");
    if (!hDXGI || !hD3D12)
        return;

    typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID, void**);
    typedef HRESULT(WINAPI * PFN_D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

    PFN_CreateDXGIFactory1 pCreateFactory = (PFN_CreateDXGIFactory1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
    PFN_D3D12CreateDevice pD3D12CreateDevice = (PFN_D3D12CreateDevice)GetProcAddress(hD3D12, "D3D12CreateDevice");
    if (!pCreateFactory || !pD3D12CreateDevice)
        return;

    IDXGIFactory2* pFactory = nullptr;
    if (FAILED(pCreateFactory(IID_PPV_ARGS(&pFactory))) || !pFactory)
        return;

    ID3D12Device* pDevice = nullptr;
    if (FAILED(pD3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&pDevice))) || !pDevice) {
        pFactory->Release();
        return;
    }

    // Hook CreateSampler on the device vtable
    // All D3D12 devices share the same vtable, so this hooks ALL devices
    DX12_HookDeviceVTable(pDevice);

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* pQueue = nullptr;
    if (FAILED(pDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&pQueue))) || !pQueue) {
        pDevice->Release();
        pFactory->Release();
        return;
    }

    // Create a minimal hidden window
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CE_Temp";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"CE_Temp", L"", WS_POPUP, 0, 0, 2, 2, nullptr, nullptr, wc.hInstance, nullptr);

    // Create temp swapchain
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = 2;
    scd.Height = 2;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain1* pSwapChain = nullptr;
    HRESULT hr = E_FAIL;

    // CRITICAL: Call the ORIGINAL CreateSwapChainForHwnd to get an unwrapped
    // swapchain We must use oCreateSwapChainForHwndGlobal directly to bypass our
    // wrapper If the original is not available, skip vtable hook installation
    if (oCreateSwapChainForHwndGlobal) {
        // Call original directly - bypasses our wrapper
        hr = oCreateSwapChainForHwndGlobal(pFactory, pQueue, hwnd, &scd, nullptr, nullptr, &pSwapChain);
        if (SUCCEEDED(hr) && pSwapChain) {
            HookLog(
                "DX12: Created temp swapchain via original "
                "CreateSwapChainForHwnd (unwrapped)");
        }
    } else {
        HookLog(
            "DX12: oCreateSwapChainForHwndGlobal not available, skipping "
            "Present vtable hooks");
    }

    if (SUCCEEDED(hr) && pSwapChain) {
        HookLog("DX12: Installing Present inline hooks via temp swapchain");
        if (DXGIShared::InstallPresentInlineHooks(pSwapChain)) {
            HookLog("DX12: Present inline hooks installed successfully");
        } else {
            HookLog("DX12: Failed to install Present inline hooks");
        }
        pSwapChain->Release();
    } else {
        HookLog("DX12: Failed to create temp swapchain (hr=0x%08X)", hr);
    }

    // Hook ExecuteCommandLists on the temp queue's vtable.
    // All DX12 command queues share the same vtable, so this hooks ALL queues
    // (including the game's pre-existing queue). When ECL fires, it calls
    // DX12_SetCommandQueue which captures the game's actual queue pointer.
    DX12_HookQueueVTable(pQueue);

    // Cleanup
    if (hwnd)
        DestroyWindow(hwnd);
    UnregisterClassW(L"CE_Temp", wc.hInstance);
    pQueue->Release();
    pDevice->Release();
    pFactory->Release();
}

void ShutdownImGui() {
    if (!g_State.overlayInit)
        return;

    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    if (g_State.srvDescHeap) {
        g_State.srvDescHeap->Release();
        g_State.srvDescHeap = nullptr;
    }
    g_State.overlayInit = false;
}

bool InitImGui(ID3D12Device* device, int buffers, DXGI_FORMAT format, HWND hwnd) {
    std::lock_guard<std::recursive_mutex> lock(g_OverlayMutex);

    // OverlayAdapter re-init check
    if (g_OverlayAdapter.IsInitialized()) {
        HookLog(
            "InitImGui: OverlayAdapter already initialized, shutting down for "
            "re-init");
        g_OverlayAdapter.Shutdown();
    }

    if (g_State.overlayInit) {
        HookLog("InitImGui: Already initialized, returning early");
        return true;
    }

    HookLog(
        "InitImGui: Proceeding with initialization - buffers=%d, format=%d, "
        "hwnd=%p",
        buffers, format, hwnd);

    g_State.format = format;

    // Use OverlayAdapter instead of ImGui
    // Rendering always goes through the game queue since GPU drivers require
    // swapchain writes from the owning queue. The overlay queue handles fence
    // management independently.
    ID3D12CommandQueue* queueForBackend = nullptr;
    {
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        queueForBackend = g_CommandQueue.load();
    }
    g_OverlayAdapter.SetHwnd(hwnd);
    if (!g_OverlayAdapter.InitDX12(device, queueForBackend, format)) {
        HookLog("[Overlay] DX12: OverlayAdapter::InitDX12 FAILED (device=%p, queue=%p, fmt=%d)", device,
                queueForBackend, format);
        return false;
    }

    // OverlayAdapter handles its own initialization
    HookLog("[Overlay] DX12: OverlayAdapter::InitDX12 succeeded (hwnd=%p)", hwnd);

    InputManager::Get().HookWindow(hwnd);

    // We don't need SRV heap for ImGui anymore, OverlayAdapter manages its own
    // resources. But we might need it if we keep ImGui for menus? For now
    // assuming full replacement for overlay.

    g_State.overlayInit = true;

    // Reset frame delay counter on reinitialization
    extern void DX12_ResetOverlayFrameDelay();
    DX12_ResetOverlayFrameDelay();

    return true;
}

void DrawOverlay(ID3D12GraphicsCommandList* cmdList, bool isRealFrame, UINT bufferIdx) {
    // CRITICAL FIX: Lock mutex to prevent concurrent access during overlay
    // shutdown/reinit
    std::lock_guard<std::recursive_mutex> lock(g_OverlayMutex);

    if (!g_State.overlayInit || !cmdList)
        return;

    // CRITICAL FIX: Always set IPC client regardless of frame type.
    // RenderOverlay() guards on ipc being non-null, so if this was only set
    // on real frames, overlay would never render when isRealFrame is false.
    g_OverlayAdapter.SetIPCClient(g_IPC);

    if (isRealFrame) {
        g_OverlayAdapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
        const char* api = "DX12";
        if (GetModuleHandleA("d3d12core.dll") && (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll")))
            api = "DX12 (VKD3D)";
        g_OverlayAdapter.SetGraphicsAPI(api);
        bool isHDR =
            (g_State.format == DXGI_FORMAT_R16G16B16A16_FLOAT || g_State.format == DXGI_FORMAT_R10G10B10A2_UNORM);
        g_OverlayAdapter.SetHDR(isHDR, (int)g_State.format);
    }

    // Set Render Target for Custom Overlay
    // CRITICAL FIX: Add null check for rtvDescHeap to prevent crash
    if (!g_State.rtvDescHeap) {
        HookLog("DrawOverlay: rtvDescHeap is null, skipping overlay");
        return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += bufferIdx * g_State.rtvDescriptorSize;

    g_OverlayAdapter.SetDX12RenderTarget(cmdList, (void*)rtvHandle.ptr);

    // Render overlay content
    g_OverlayAdapter.RenderOverlay(g_State.cachedWidth, g_State.cachedHeight);
}

void CreateRTVs(ID3D12Device* device, IDXGISwapChain3* swapChain, int bufferCount) {
    if (g_State.rtvDescHeap)
        return;

    // DLSS FG FIX: Validate buffer count before creating RTVs
    if (bufferCount <= 0 || bufferCount > 8) {
        HookLog("CreateRTVs: Invalid buffer count %d, limiting to 3", bufferCount);
        bufferCount = 3;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, (UINT)bufferCount,
                                              D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_State.rtvDescHeap))))
        return;
    g_State.bufferCount = bufferCount;
    g_State.cachedSwapChain = swapChain;
    g_State.rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    // FG-SAFE: Do NOT hold persistent references on backbuffers.
    // FSR FG monitors backbuffer reference counts and crashes if extra refs are held.
    // Create RTVs and release immediately — re-acquire per-frame in render path.
    g_State.backBuffers.clear();
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
    for (int i = 0; i < bufferCount; i++) {
        ID3D12Resource* bb = nullptr;
        if (SUCCEEDED(swapChain->GetBuffer(i, IID_PPV_ARGS(&bb))) && bb) {
            device->CreateRenderTargetView(bb, nullptr, rtvHandle);
            bb->Release();  // Release immediately - RTV descriptor remains valid
        }
        rtvHandle.ptr += g_State.rtvDescriptorSize;
    }
    HookLog("CreateRTVs: Created %d RTVs (no held refs)", bufferCount);
}

void InitOverlaySync(ID3D12Device* device, int bufferCount, ID3D12CommandQueue* gameQueue) {
    HookLogImportant("InitOverlaySync: ENTER (syncInit=%d)", g_State.syncInit);

    if (g_State.syncInit) {
        HookLogImportant("InitOverlaySync: Already initialized, returning early");
        return;
    }

    // Release any previously allocated sync resources to prevent leaks when
    // syncInit was cleared by a resize or error path without calling Shutdown.
    if (g_State.fence) {
        if (g_State.fenceEvent) {
            CloseHandle(g_State.fenceEvent);
            g_State.fenceEvent = nullptr;
        }
        g_State.fence->Release();
        g_State.fence = nullptr;
    }
    if (g_State.cmdList) {
        g_State.cmdList->Release();
        g_State.cmdList = nullptr;
    }
    for (auto* a : g_State.allocators)
        if (a)
            a->Release();
    g_State.allocators.clear();
    g_State.fenceValues.clear();

    // Release previous overlay queue if any
    if (g_State.overlayQueue) {
        g_State.overlayQueue->Release();
        g_State.overlayQueue = nullptr;
    }
    // Release previous cross-queue fence and event if any
    if (g_State.crossQueueFenceEvent) {
        CloseHandle(g_State.crossQueueFenceEvent);
        g_State.crossQueueFenceEvent = nullptr;
    }
    if (g_State.crossQueueFence) {
        g_State.crossQueueFence->Release();
        g_State.crossQueueFence = nullptr;
    };

    // Dedicated overlay queue: when FG is active, overlay commands execute on
    // this queue instead of the game queue.  This avoids interfering with
    // Streamline's game queue management.  CPU-side fence waits provide
    // cross-queue synchronization (GPU-side Wait was removed due to NVIDIA
    // WaitImpl Alt+Tab hangs).
    HookLog("InitOverlaySync: Creating dedicated overlay command queue");

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;

    // Get the node mask from the game queue if available
    if (gameQueue) {
        D3D12_COMMAND_QUEUE_DESC gameQueueDesc;
        gameQueue->GetDesc(&gameQueueDesc);
        queueDesc.NodeMask = gameQueueDesc.NodeMask;
        HookLog("InitOverlaySync: Using node mask 0x%X from game queue", queueDesc.NodeMask);
    }

    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_State.overlayQueue)))) {
        HookLog("InitOverlaySync: FAILED to create overlay queue, falling back to single-queue");
        // Continue without overlay queue - will fall back to game queue
    } else {
        HookLogImportant("InitOverlaySync: Dedicated overlay queue created (ptr=%p, gameQueue=%p)",
                         g_State.overlayQueue, gameQueue);
    }

    // Cross-queue fence: game queue signals to mark work completion before
    // overlay queue starts.  CPU-side WaitForSingleObject is used instead of
    // GPU-side CommandQueue::Wait to avoid NVIDIA WaitImpl Alt+Tab hangs.
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_State.crossQueueFence)))) {
        HookLog("InitOverlaySync: FAILED to create cross-queue fence");
    }
    g_State.crossQueueFenceValue = 0;
    g_State.crossQueueFenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_State.fence))))
        return;

    g_State.allocators.resize(DX12OverlayState::ALLOC_POOL_SIZE);
    g_State.fenceValues.resize(DX12OverlayState::ALLOC_POOL_SIZE);

    // CRITICAL FIX: Reset all fence values to 0 for fresh start
    // After resize/reinit, old fence values could be stale and cause infinite
    // waits
    std::fill(g_State.fenceValues.begin(), g_State.fenceValues.end(), 0);
    g_State.currentFenceValue = 0;
    g_State.allocIndex = 0;

    HookLog("InitOverlaySync: Fence values reset to 0, currentFenceValue=0");

    bool success = true;
    for (int i = 0; i < DX12OverlayState::ALLOC_POOL_SIZE; i++) {
        if (FAILED(
                device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_State.allocators[i])))) {
            success = false;
            break;
        }
    }

    if (success) {
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_State.allocators[0], nullptr,
                                             IID_PPV_ARGS(&g_State.cmdList)))) {
            success = false;
        }
    }

    if (success) {
        g_State.cmdList->Close();
        g_State.fenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

        g_State.syncInit = true;
    } else {
        // Cleanup partial initialization
        for (auto* alloc : g_State.allocators)
            if (alloc)
                alloc->Release();
        g_State.allocators.clear();
        g_State.fenceValues.clear();
        if (g_State.cmdList) {
            g_State.cmdList->Release();
            g_State.cmdList = nullptr;
        }
        if (g_State.fence) {
            g_State.fence->Release();
            g_State.fence = nullptr;
        }
    }
}

static bool DrainCommandQueue(ID3D12CommandQueue* queue, ID3D12Device* device) {
    if (!queue || !device)
        return false;

    // NON-BLOCKING DRAIN: Use a flush approach instead of waiting
    // to avoid deadlocking when called from the submit thread.
    ID3D12Fence* fence = nullptr;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return false;

    // Signal the fence but don't wait - if we're on the submit thread,
    // waiting would deadlock. The fence will be processed when the
    // game next submits work.
    queue->Signal(fence, 1);

    // Quick check if already completed (GPU was idle)
    if (fence->GetCompletedValue() >= 1) {
        fence->Release();
        return true;
    }

    // Optional: very short wait for already-in-flight work (1ms)
    // This helps if the GPU is just finishing up, without blocking
    // the submit thread for long.
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (event) {
        if (fence->SetEventOnCompletion(1, event) == S_OK) {
            WaitForSingleObject(event, 1);  // 1ms non-blocking wait
        }
        CloseHandle(event);
    }
    fence->Release();
    return true;
}

void CleanupOverlay() {
    if (!g_State.syncInit)
        return;

    // Dedicated overlay queue: flush overlay queue instead of game queue
    ID3D12CommandQueue* queueToFlush = g_State.overlayQueue;
    if (!queueToFlush) {
        // Fallback to game queue if no dedicated overlay queue
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        queueToFlush = g_CommandQueue;
    }

    if (g_State.fence && queueToFlush) {
        UINT64 waitValue = g_State.currentFenceValue + 1;
        if (SUCCEEDED(queueToFlush->Signal(g_State.fence, waitValue))) {
            if (g_State.fence->GetCompletedValue() < waitValue) {
                g_State.fence->SetEventOnCompletion(waitValue, g_State.fenceEvent);
                WaitForSingleObject(g_State.fenceEvent, 200);
            }
        }
    }
    if (g_State.fenceEvent) {
        CloseHandle(g_State.fenceEvent);
        g_State.fenceEvent = NULL;
    }
    for (auto alloc : g_State.allocators)
        if (alloc)
            alloc->Release();
    g_State.allocators.clear();
    g_State.fenceValues.clear();
    if (g_State.cmdList) {
        g_State.cmdList->Release();
        g_State.cmdList = nullptr;
    }
    if (g_State.fence) {
        g_State.fence->Release();
        g_State.fence = nullptr;
    }
    // Release dedicated overlay queue
    if (g_State.overlayQueue) {
        g_State.overlayQueue->Release();
        g_State.overlayQueue = nullptr;
    }
    // Release cross-queue synchronization fence
    if (g_State.crossQueueFence) {
        g_State.crossQueueFence->Release();
        g_State.crossQueueFence = nullptr;
    }
    g_State.currentFenceValue = 0;
    g_State.crossQueueFenceValue = 0;
    g_State.allocIndex = 0;
    g_State.syncInit = false;
}

void CleanupRTVs() {
    // FG-SAFE: backBuffers no longer holds references (released at create time)
    g_State.backBuffers.clear();
    if (g_DummyBackBuffer) {
        g_DummyBackBuffer->Release();
        g_DummyBackBuffer = nullptr;
    }
    if (g_State.rtvDescHeap) {
        g_State.rtvDescHeap->Release();
        g_State.rtvDescHeap = nullptr;
    }
    if (g_State.srvDescHeap) {
        g_State.srvDescHeap->Release();
        g_State.srvDescHeap = nullptr;
    }
    g_State.bufferCount = 0;
    g_State.cachedSwapChain = nullptr;
}

void DX12_OnSwapchainResizeBegin() {
    bool wasAlreadySet = g_InSwapchainResizeCleanup.exchange(true);
    HookLog("DX12: DX12_OnSwapchainResizeBegin called, wasAlreadySet=%d", wasAlreadySet);

    // Prevent recursion - if already in resize, return immediately
    if (wasAlreadySet) {
        HookLog(
            "DX12: DX12_OnSwapchainResizeBegin - already in resize, returning "
            "early");
        return;
    }

    DXGIShared::g_SharedState.lastSwapchainCreation = std::chrono::steady_clock::now();
    HookLog("DX12: DX12_OnSwapchainResizeBegin - step 1: timestamp updated");

    // ResizeBuffers must see a fully invalidated overlay state; skipping cleanup
    // here leaves stale RTV descriptors/backbuffer state that can hang drivers.
    std::lock_guard<std::recursive_mutex> lock(g_OverlayMutex);
    HookLog("DX12: DX12_OnSwapchainResizeBegin - step 2: got mutex");

    // Invalidate and release swapchain-bound resources before ResizeBuffers.
    g_State.overlayInit = false;
    g_State.syncInit = false;
    CleanupRTVs();
    HookLog("DX12: DX12_OnSwapchainResizeBegin - step 3: marked state invalid");

    // g_LastSwapChain is stored as a raw (non-AddRef'd) pointer to avoid
    // interfering with FSR FG's reference count management.  Do NOT Release it.
    g_PendingSwapChainCleanup = nullptr;
    g_LastSwapChain = nullptr;
    HookLog("DX12: DX12_OnSwapchainResizeBegin - complete");
}

void DX12_OnSwapchainResizeEnd() {
    HookLog("DX12: DX12_OnSwapchainResizeEnd called");
    // Only clear if it was set - prevents unbalanced calls from clearing
    // prematurely
    if (g_InSwapchainResizeCleanup.load(std::memory_order_acquire)) {
        g_InSwapchainResizeCleanup.store(false, std::memory_order_release);
    }
    // g_PendingSwapChainCleanup is no longer used (swapchain stored without
    // AddRef), so nothing to release here.
    if (g_PendingSwapChainCleanup) {
        g_PendingSwapChainCleanup = nullptr;
    }
}

// --- CPU Prerender Limit Support (DX12) ---
static void ApplyPrerenderLimitDX12(float limit) {
    if (limit < 0.0f)
        return;
    // CRITICAL FIX: Use thread-safe accessor to prevent race conditions
    DX12Context ctx = GetDX12Context();
    if (!ctx.IsValid())
        return;

    std::lock_guard<std::mutex> lock(g_PrerenderMutex);

    // Initialize fence ring buffer if needed
    if (g_PrerenderFences.empty()) {
        for (int i = 0; i < 16; i++) {
            ID3D12Fence* fence = nullptr;
            HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (SUCCEEDED(ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
                g_PrerenderFences.push_back(fence);
                g_PrerenderEvents.push_back(event);
            } else if (event) {
                CloseHandle(event);
            }
        }
        HookLog("DX12: Created prerender limit fence ring buffer (size: %d)", (int)g_PrerenderFences.size());
    }

    if (g_PrerenderFences.empty())
        return;

    constexpr DWORD kPrerenderWaitTimeoutMs = 8;
    static std::atomic<int> s_prerenderWarnLogs{0};
    auto waitFenceBounded = [&](ID3D12Fence* waitFence, HANDLE waitEvent, uint64_t waitValue) -> bool {
        if (!waitFence || !waitEvent)
            return false;
        if (waitFence->GetCompletedValue() >= waitValue)
            return true;

        HRESULT setHr = waitFence->SetEventOnCompletion(waitValue, waitEvent);
        if (FAILED(setHr)) {
            if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLog("DX12: Prerender SetEventOnCompletion failed hr=0x%08X value=%llu", setHr, waitValue);
            }
            return false;
        }

        DWORD waitResult = WaitForSingleObject(waitEvent, kPrerenderWaitTimeoutMs);
        if (waitResult == WAIT_OBJECT_0)
            return true;

        if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
            if (waitResult == WAIT_TIMEOUT) {
                HookLog("DX12: Prerender wait timed out (%lums) value=%llu", kPrerenderWaitTimeoutMs, waitValue);
            } else {
                HookLog("DX12: Prerender wait failed result=%lu value=%llu", waitResult, waitValue);
            }
        }
        return false;
    };

    size_t idx = g_PrerenderFrameIndex % g_PrerenderFences.size();
    ID3D12Fence* fence = g_PrerenderFences[idx];
    HANDLE event = g_PrerenderEvents[idx];

    if (limit == 0.0f) {
        // Strict Serial: Signal and immediately wait
        uint64_t value = g_PrerenderFrameIndex + 1;
        HRESULT signalHr = ctx.queue->Signal(fence, value);
        if (SUCCEEDED(signalHr)) {
            waitFenceBounded(fence, event, value);
        } else if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
            HookLog("DX12: Prerender signal failed hr=0x%08X value=%llu", signalHr, value);
        }
    } else {
        // Buffered Limit: For fractional limits (e.g., 0.5), we use Buffered 1
        // (Lookback 1) combined with an idle gap to approximate sub-frame latency.
        bool isFractional = (limit > 0.01f && limit < 1.0f);
        int effectiveLimit = isFractional ? 1 : (int)limit;
        int lookback = effectiveLimit;

        // Signal current frame
        uint64_t signalValue = g_PrerenderFrameIndex + 1;
        HRESULT signalHr = ctx.queue->Signal(fence, signalValue);
        if (FAILED(signalHr)) {
            if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLog("DX12: Prerender signal failed hr=0x%08X value=%llu", signalHr, signalValue);
            }
            g_PrerenderFrameIndex++;
            return;
        }

        // Wait on N frames ago
        if (g_PrerenderFrameIndex >= (uint64_t)lookback) {
            size_t waitIdx = (g_PrerenderFrameIndex - lookback) % g_PrerenderFences.size();
            ID3D12Fence* waitFence = g_PrerenderFences[waitIdx];
            HANDLE waitEvent = g_PrerenderEvents[waitIdx];
            uint64_t waitValue = (g_PrerenderFrameIndex - lookback) + 1;

            if (waitFence->GetCompletedValue() < waitValue) {
                waitFenceBounded(waitFence, waitEvent, waitValue);
            }
        }
    }

    g_PrerenderFrameIndex++;
}

void ProcessFrame(IDXGISwapChain* pSwapChain, bool processCapture) {
    // Re-entrancy guard: NVIDIA driver can pump window messages during
    // ExecuteCommandLists (via WaitImpl → DefWindowProc), which can re-enter
    // our overlay code.  Detect and skip the re-entrant call.
    static thread_local bool s_inProcessFrame = false;
    if (s_inProcessFrame) {
        return;
    }
    s_inProcessFrame = true;
    auto reentryGuard = ce::make_scope_guard([&]() { s_inProcessFrame = false; });

    static bool s_firstFrame = true;
    if (s_firstFrame) {
        s_firstFrame = false;
        HookLogImportant("DX12: ProcessFrame FIRST CALL (swapchain=%p)", (void*)pSwapChain);
    }

    // Performance metrics for this frame
    FrameMetrics perfMetrics;
    perfMetrics.qpcUs = PerfLogger::GetQpcUs();
    strncpy(perfMetrics.api, "DX12", sizeof(perfMetrics.api) - 1);
    perfMetrics.api[sizeof(perfMetrics.api) - 1] = '\0';
    static uint64_t s_perfFrameNum = 0;
    perfMetrics.frameNum = ++s_perfFrameNum;

    // Scope guard to log metrics on any exit path
    auto perfGuard = ce::make_scope_guard([&]() {
        if (PerfLogger::Get().IsEnabled()) {
            perfMetrics.totalUs = static_cast<int32_t>((PerfLogger::GetQpcUs() - perfMetrics.qpcUs));
            perfMetrics.fpsLimitWaitUs = static_cast<int32_t>(g_SharedFpsLimiter.GetLastWaitUs());
            PerfLogger::Get().LogFrame(perfMetrics);
        }
    });

    // Skip performance logging if disabled
    if (!PerfLogger::Get().IsEnabled()) {
        perfGuard.dismiss();
    }

    // CRITICAL: Skip all rendering during shutdown to prevent crashes
    if (HookIsShuttingDown()) {
        return;
    }

    bool inResize = g_InSwapchainResizeCleanup.load(std::memory_order_acquire);
    if (!pSwapChain || inResize) {
        HookLog("DX12: ProcessFrame - early return (null=%d, inResize=%d)", !pSwapChain, inResize);
        return;
    }

    DXGI_SWAP_CHAIN_DESC frameDesc = {};
    if (FAILED(pSwapChain->GetDesc(&frameDesc))) {
        HookLog("DX12: ProcessFrame - failed to get swapchain desc (precheck)");
        return;
    }

    const bool zeroSizedSwapchain = (frameDesc.BufferDesc.Width == 0 || frameDesc.BufferDesc.Height == 0);
    const bool iconicWindow = frameDesc.OutputWindow && IsIconic(frameDesc.OutputWindow);
    const bool suspendOverlayWork = zeroSizedSwapchain || iconicWindow;
    static bool s_swapchainSuspended = false;
    if (suspendOverlayWork) {
        if (!s_swapchainSuspended) {
            s_swapchainSuspended = true;
            g_State.overlayInit = false;
            CleanupRTVs();
            HookLog("DX12: ProcessFrame - suspending overlay (w=%u h=%u iconic=%d)", frameDesc.BufferDesc.Width,
                    frameDesc.BufferDesc.Height, iconicWindow ? 1 : 0);
        }
    } else if (s_swapchainSuspended) {
        s_swapchainSuspended = false;
        HookLog("DX12: ProcessFrame - resuming overlay after drawable swapchain restored");
    }

    // CPU Prerender Limit - Apply before any rendering
    float prerenderLimit = GetActivePrerenderLimit();
    if (prerenderLimit >= 0.0f) {
        int64_t prerenderStartUs = PerfLogger::GetQpcUs();
        ApplyPrerenderLimitDX12(prerenderLimit);
        perfMetrics.prerenderWaitUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - prerenderStartUs);
    }

    // PERFORMANCE FIX: Use try_lock instead of blocking lock_guard
    // This prevents stalling the render thread if another thread holds the lock
    if (!g_OverlayMutex.try_lock()) {
        // Another thread is processing, skip this frame
        HookLog("DX12: ProcessFrame - mutex busy, skipping frame");
        return;
    }
    // RAII unlock when we exit
    std::lock_guard<std::recursive_mutex> lock(g_OverlayMutex, std::adopt_lock);

    // SAFETY: Check device state after acquiring lock
    if (g_InSwapchainResizeCleanup.load(std::memory_order_acquire)) {
        HookLog("DX12: ProcessFrame - in resize cleanup, returning");
        return;
    }

    // Dedicated overlay queue architecture: when FG is active, overlay commands
    // execute on the overlay queue with CPU-side fence sync to avoid interfering
    // with Streamline.  When FG is not active, commands go on the game queue.

    // FG-SAFE device resolution: avoid COM calls through swapchain when FG is
    // active.  FSR FG wraps the swapchain, and QueryInterface/GetDevice through
    // the wrapper can corrupt FG state (causing a delayed null-deref crash in
    // game code ~2 seconds later when FG activates).
    //
    // Instead, we rely on device/queue already captured by our hook infrastructure:
    //   - g_Device is set from CreateSwapChainForHwnd detour or ECL detour
    //   - g_CommandQueue / g_SwapchainQueue are set from ECL/CreateSwapChain hooks
    //
    // The only thing we need from ProcessFrame is swapchain change tracking.
    // FG-SAFE swapchain change tracking: track pointer value only, no AddRef/Release.
    // FSR FG wraps the swapchain and monitors reference counts. Holding an extra
    // reference prevents FSR FG from properly transitioning, causing a delayed
    // null-deref crash in game code ~2s later when FG activates.
    if (pSwapChain != g_LastSwapChain) {
        if (g_LastSwapChain) {
            CleanupRTVs();
            g_SharedCaptureD3D12.Reset();
            g_State.overlayInit = false;
        }
        // Store raw pointer for change detection only - no AddRef to avoid
        // interfering with FSR FG's swapchain lifecycle management
        g_LastSwapChain = pSwapChain;

        if (!g_Device.load()) {
            return;
        }
        HookLog("DX12: ProcessFrame - new swapchain tracked (device=%p)", g_Device.load());
    }
    // Prefer the swapchain queue(captured at creation time) so that our
    // RENDER_TARGET -> PRESENT barrier executes on the queue DXGI syncs with.
    // Fall back to the last observed direct queue if it was not captured yet.
    ID3D12CommandQueue* gameQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        gameQueue = g_SwapchainQueue ? g_SwapchainQueue : g_CommandQueue.load();
    }
    if (!gameQueue) {
        HookLog("DX12: ProcessFrame - no game queue, skipping overlay");
        return;
    }

    // Queue-change-based FG detection: FSR FG creates its own command queue
    // and reroutes all ECL calls through it.  Detecting a queue pointer change
    // after the first few frames is a strong signal that FG has activated.
    {
        static ID3D12CommandQueue* s_initialQueue = nullptr;
        static bool s_queueChangeDetected = false;
        static int s_queueFrameCount = 0;
        ++s_queueFrameCount;
        if (s_queueFrameCount <= 5) {
            // Capture initial queue during first 5 frames (before FG activates)
            s_initialQueue = gameQueue;
        } else if (!s_queueChangeDetected && s_initialQueue && gameQueue != s_initialQueue) {
            s_queueChangeDetected = true;
            HookLogImportant("DX12: FG detected via queue change (initial=%p, current=%p, frame=%d)", s_initialQueue,
                             gameQueue, s_queueFrameCount);
            g_FGCompat.SetFSRFGActive(true);
        }
    }

    // Remove delay - install overlay immediately(Strange Brigade compatibility)
    static std::atomic<int> s_framesBeforeInit{0};
    if (!suspendOverlayWork && !g_State.overlayInit) {
        int frames = ++s_framesBeforeInit;
        if (frames < 1) {
            return;
        } else if (frames == 1) {
            HookLog("DX12: ProcessFrame - Proceeding with overlay init");
        }

        // CRITICAL FIX: Don't initialize ImGui during FG suspension, FSR
        // stabilization, or native FSR FG This prevents initialization with
        // potentially unstable frame generation state and avoids initializing
        // overlay resources we'll never use (native FSR FG skips rendering)
        // CRITICAL FIX: Clean up any existing overlay context from previous
        // swapchain This happens when FSR FG recreates the swapchain and we
        // deferred cleanup MUST hold mutex to prevent race with DrawOverlay
        if (g_OverlayAdapter.IsInitialized()) {
            std::lock_guard<std::recursive_mutex> cleanupLock(g_OverlayMutex);
            HookLog("DX12: ProcessFrame - cleaning up stale OverlayAdapter (mutex held)");
            g_OverlayAdapter.Shutdown();
            CleanupOverlay();
            CleanupRTVs();
            HookLog("DX12: ProcessFrame - cleanup complete, proceeding with init");
        }

        DXGI_SWAP_CHAIN_DESC desc;
        if (SUCCEEDED(pSwapChain->GetDesc(&desc))) {
            g_State.cachedWidth = desc.BufferDesc.Width;
            g_State.cachedHeight = desc.BufferDesc.Height;

            // Use actual swapchain buffer count for ImGui initialization
            // The separate overlay queue (Change 1) eliminates the need for buffer
            // limiting
            int imguiBufferCount = desc.BufferCount;

            HookLog("DX12: ProcessFrame - initializing ImGui (%dx%d, buffers=%d)", g_State.cachedWidth,
                    g_State.cachedHeight, imguiBufferCount);

            // Validate swapchain buffers are accessible before initializing
            IDXGISwapChain3* sc3 = nullptr;
            if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
                int validBuffers = 0;
                for (int i = 0; i < imguiBufferCount; i++) {
                    ID3D12Resource* bb = nullptr;
                    if (SUCCEEDED(sc3->GetBuffer(i, IID_PPV_ARGS(&bb)))) {
                        if (bb) {
                            bb->Release();
                            validBuffers++;
                        }
                    } else {
                        HookLog(
                            "DX12: ProcessFrame - buffer %d not accessible, stopping "
                            "validation",
                            i);
                        break;
                    }
                }

                if (validBuffers < imguiBufferCount) {
                    HookLog(
                        "DX12: ProcessFrame - only %d/%d buffers valid, skipping "
                        "ImGui init this frame",
                        validBuffers, imguiBufferCount);
                    sc3->Release();
                    return;
                }

                if (InitImGui(g_Device.load(), imguiBufferCount, desc.BufferDesc.Format, desc.OutputWindow)) {
                    int actualBufferCount = desc.BufferCount;
                    if (actualBufferCount > 8) {
                        HookLog("DX12: Swapchain has %d buffers, limiting RTVs to 8", actualBufferCount);
                        actualBufferCount = 8;
                    }
                    CreateRTVs(g_Device.load(), sc3, actualBufferCount);
                    InitOverlaySync(g_Device.load(), imguiBufferCount, gameQueue);
                    HookLog(
                        "DX12: ProcessFrame - ImGui initialized with %d RTVs, "
                        "syncInit=%d",
                        actualBufferCount, g_State.syncInit);
                } else {
                    HookLog("DX12: ProcessFrame - ImGui initialization FAILED");
                }
                // SAFETY: Check sc3 is still valid before releasing
                if (sc3) {
                    sc3->Release();
                }
            } else {
                HookLog("DX12: ProcessFrame - failed to get IDXGISwapChain3 interface");
            }
        } else {
            HookLog("DX12: ProcessFrame - failed to get swapchain desc");
        }
    }

    // Single log on first frame to verify overlay system is entering
    static int s_firstFrameLogged = 0;
    if (s_firstFrameLogged == 0) {
        s_firstFrameLogged = 1;
        HookLog(
            "DX12: ProcessFrame first call - overlayInit=%d, syncInit=%d, "
            "gameQueue=%p",
            g_State.overlayInit, g_State.syncInit, gameQueue);
    }

    UINT currentBackBufferIdx = 0;
    bool hasCurrentBackBufferIdx = false;

    if (!suspendOverlayWork && !s_insideECL && g_State.overlayInit && g_State.syncInit) {
        // Single log on first successful overlay render
        static int s_firstOverlayLogged = 0;
        if (s_firstOverlayLogged == 0) {
            s_firstOverlayLogged = 1;
            HookLog(
                "DX12: ProcessFrame - first successful overlay render (fence=%p, "
                "cmdList=%p, fgActive=%d, fgType=%s)",
                g_State.fence, g_State.cmdList, g_FGCompat.IsFGActive() ? 1 : 0,
                g_FGCompat.GetFGTypeName(g_FGCompat.GetActiveFGType()));
        }

        // Log FG state transitions for debugging
        static bool s_lastFGActive = false;
        bool currentFGActive = g_FGCompat.IsFGActive();
        if (currentFGActive != s_lastFGActive) {
            HookLogImportant("DX12: FG state changed: %s -> %s (type=%s)", s_lastFGActive ? "active" : "inactive",
                             currentFGActive ? "active" : "inactive",
                             g_FGCompat.GetFGTypeName(g_FGCompat.GetActiveFGType()));
            s_lastFGActive = currentFGActive;
        }

        // Periodic health log every 500 frames for debugging stability
        static std::atomic<uint64_t> s_overlayFrameCount{0};
        uint64_t frameNum = s_overlayFrameCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (frameNum == 1 || frameNum == 10 || frameNum == 50 || frameNum == 100 || (frameNum % 500) == 0) {
            ID3D12Device* dev = g_Device.load();
            HRESULT devRemovedHr = dev ? dev->GetDeviceRemovedReason() : E_FAIL;
            HookLogImportant(
                "DX12: Overlay frame #%llu (deviceRemoved=0x%08X, fgActive=%d, "
                "queue=%p, allocIdx=%d)",
                (unsigned long long)frameNum, (unsigned)devRemovedHr, currentFGActive ? 1 : 0, gameQueue,
                g_State.allocIndex);
        }

        // Check device removed BEFORE rendering - skip if GPU is already in error
        {
            ID3D12Device* devCheck = g_Device.load();
            if (devCheck && FAILED(devCheck->GetDeviceRemovedReason())) {
                HookLogImportant("DX12: GPU device removed (0x%08X), skipping overlay",
                                 (unsigned)devCheck->GetDeviceRemovedReason());
                goto overlay_done;
            }
        }

        {
            int idx = g_State.allocIndex;
            g_State.allocIndex = (idx + 1) % DX12OverlayState::ALLOC_POOL_SIZE;

            // With 16 allocators, we never need to wait under normal conditions.
            // However, during Alt+Tab / GPU throttle, the GPU may stall and the
            // fence value for this allocator slot won't advance.  We must check
            // before Reset() to avoid undefined behaviour (driver hang / crash).
            auto* list = g_State.cmdList;
            auto* alloc = (idx < (int)g_State.allocators.size()) ? g_State.allocators[idx] : nullptr;
            if (list && alloc) {
                // Guard: skip overlay render if this allocator is still in flight.
                if (g_State.fence && idx < (int)g_State.fenceValues.size() && g_State.fenceValues[idx] > 0) {
                    UINT64 completed = g_State.fence->GetCompletedValue();
                    if (completed < g_State.fenceValues[idx]) {
                        static std::atomic<int> s_allocSkipLogs{0};
                        if (s_allocSkipLogs.fetch_add(1, std::memory_order_relaxed) < 30) {
                            HookLog(
                                "DX12: Allocator[%d] still in-flight (completed=%llu, "
                                "needed=%llu), skipping overlay this frame",
                                idx, completed, g_State.fenceValues[idx]);
                        }
                        goto overlay_done;
                    }
                }
                HRESULT allocResetHr = alloc->Reset();
                if (SUCCEEDED(allocResetHr)) {
                    HRESULT listResetHr = list->Reset(alloc, nullptr);
                    if (SUCCEEDED(listResetHr)) {
                        IDXGISwapChain3* sc3 = nullptr;
                        if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
                            UINT swapchainBufferIdx = sc3->GetCurrentBackBufferIndex();
                            currentBackBufferIdx = swapchainBufferIdx;
                            hasCurrentBackBufferIdx = true;
                            // CRITICAL FIX: Use actual swapchain buffer index directly
                            // CreateRTVs now creates RTVs for all swapchain buffers (up to 8)
                            // so no need to wrap the index - this prevents sync issues
                            UINT bufferIdx = swapchainBufferIdx;
                            // Validate buffer index is within our allocated range
                            if (bufferIdx >= (UINT)g_State.bufferCount) {
                                HookLog(
                                    "DX12: Buffer index %u exceeds allocated count %d, "
                                    "clamping",
                                    bufferIdx, g_State.bufferCount);
                                bufferIdx = g_State.bufferCount - 1;
                            }
                            // FG-SAFE: Acquire backbuffer per-frame via GetBuffer.
                            // We do NOT cache backbuffer pointers because FSR FG
                            // monitors reference counts and crashes if extra refs
                            // are held persistently.
                            ID3D12Resource* bb = nullptr;
                            bool bbNeedsRelease = false;
                            if (SUCCEEDED(sc3->GetBuffer(swapchainBufferIdx, IID_PPV_ARGS(&bb))) && bb) {
                                bbNeedsRelease = true;
                                // Recreate RTV for this buffer index (cheap CPU-side op).
                                // Ensures RTV matches current buffer even after FSR FG
                                // swapchain transitions.
                                D3D12_CPU_DESCRIPTOR_HANDLE rtvRecreate =
                                    g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
                                rtvRecreate.ptr += (SIZE_T)bufferIdx * g_State.rtvDescriptorSize;
                                g_Device.load()->CreateRenderTargetView(bb, nullptr, rtvRecreate);
                            }
                            if (bb) {
                                bool cmdRecordOk = false;

                                // Barrier: PRESENT -> RENDER_TARGET
                                D3D12_RESOURCE_BARRIER barrier = {};
                                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                barrier.Transition.pResource = bb;
                                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                list->ResourceBarrier(1, &barrier);

                                // Set render target
                                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
                                    g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
                                UINT rtvSize =
                                    g_Device.load()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                                rtvHandle.ptr += (SIZE_T)bufferIdx * rtvSize;
                                list->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

                                // Draw overlay
                                bool isRealFrame = g_FGCompat.IsCurrentFrameReal();
                                DrawOverlay(list, isRealFrame, bufferIdx);

                                // Barrier: RENDER_TARGET -> PRESENT
                                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                                list->ResourceBarrier(1, &barrier);

                                HRESULT closeHr = list->Close();
                                if (FAILED(closeHr)) {
                                    HookLog("DX12: list->Close failed hr=0x%08X, forcing reinit", closeHr);
                                    g_State.syncInit = false;
                                } else {
                                    // Submit on game queue
                                    ID3D12CommandList* lists[] = {list};
                                    ExecuteCommandListsPtr origECL = GetOriginalExecuteCommandLists(gameQueue);
                                    if (origECL) {
                                        origECL(gameQueue, 1, lists);
                                    } else {
                                        gameQueue->ExecuteCommandLists(1, lists);
                                    }
                                    cmdRecordOk = true;
                                }

                                // Fence signaling for allocator pool management
                                if (cmdRecordOk && g_State.fence) {
                                    UINT64 next = g_State.currentFenceValue + 1;
                                    HRESULT shr = gameQueue->Signal(g_State.fence, next);
                                    if (SUCCEEDED(shr)) {
                                        g_State.currentFenceValue = next;
                                        g_State.fenceValues[idx] = next;
                                    } else {
                                        HookLog("DX12: Overlay fence signal failed hr=0x%08X", shr);
                                    }
                                }
                                // FG-SAFE: Release per-frame backbuffer reference
                                if (bbNeedsRelease)
                                    bb->Release();
                            } else {
                                HookLog("DX12: GetBuffer(%u) failed, forcing RTV reinit", swapchainBufferIdx);
                                CleanupRTVs();
                                g_State.overlayInit = false;
                            }
                            sc3->Release();
                        } else {
                            HookLog("DX12: ProcessFrame - failed to get SwapChain3 interface");
                        }
                    } else {
                        HookLog("DX12: ProcessFrame - list->Reset failed hr=0x%08X, forcing reinit", listResetHr);
                        g_State.syncInit = false;
                    }
                } else {
                    HookLog("DX12: ProcessFrame - alloc->Reset failed hr=0x%08X, forcing reinit", allocResetHr);
                    g_State.syncInit = false;
                }
            } else {
                HookLog("DX12: ProcessFrame - null list or alloc");
            }
        }  // end device-removed-check scope
    overlay_done:;
    }

    // Change 6: Remove verbose debug logging - keep only error logging
    if (processCapture && g_IPC && g_IPC->IsRecording()) {
        int64_t captureStartUs = PerfLogger::GetQpcUs();
        SharedMemoryLayout* shm = g_IPC->GetSharedMem();
        if (shm) {
            if (shm->throttleCapture.load(std::memory_order_acquire)) {
                // Skip capture to let encoder catch up
            } else {
                if (!g_SharedCaptureD3D12.IsActive())
                    g_SharedCaptureD3D12.Initialize(g_Device.load(), pSwapChain);
                if (g_SharedCaptureD3D12.IsActive()) {
                    std::lock_guard<std::recursive_mutex> capLock(g_DX12CaptureMutex);
                    // Keep capture submission on the same queue selection as overlay/present
                    // work to avoid cross-queue sync jitter.
                    ID3D12CommandQueue* captureQueue = gameQueue;
                    UINT bbIdx = 0;
                    if (hasCurrentBackBufferIdx) {
                        bbIdx = currentBackBufferIdx;
                    } else {
                        IDXGISwapChain3* sc3 = nullptr;
                        pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3));
                        bbIdx = sc3 ? sc3->GetCurrentBackBufferIndex() : 0;
                        if (sc3)
                            sc3->Release();
                    }
                    if (captureQueue && g_SharedCaptureD3D12.CaptureFrame(captureQueue, bbIdx)) {
                        SharedFrameDescriptor desc;
                        if (g_SharedCaptureD3D12.GetCurrentFrame(&desc)) {
                            shm->SetSharedHandle(0, (uint64_t)g_SharedCaptureD3D12.GetSharedHandle(0));
                            shm->SetSharedHandle(1, (uint64_t)g_SharedCaptureD3D12.GetSharedHandle(1));
                            shm->SetFenceShareHandle((uint64_t)g_SharedCaptureD3D12.GetFenceShareHandle());
                            shm->SetWidth(desc.width);
                            shm->SetHeight(desc.height);
                            shm->SetFormat(desc.format);
                            // CRITICAL FIX: Use acquire ordering to see consumer's readIndex
                            // updates
                            uint32_t wIdx = shm->frameRing.writeIndex.load(std::memory_order_acquire);
                            uint32_t rIdx = shm->frameRing.readIndex.load(std::memory_order_acquire);
                            if ((uint32_t)(wIdx - rIdx) < (uint32_t)FRAME_RING_SIZE) {
                                FrameSlot& slot = shm->frameRing.slots[wIdx % FRAME_RING_SIZE];
                                slot.fenceValue = desc.fenceValue;
                                slot.timestamp = desc.presentTime;
                                slot.frameIndex = desc.frameNumber;
                                slot.textureIndex = desc.textureIndex;
                                slot.sourcePid = GetCurrentProcessId();
                                // CRITICAL FIX: Add release fence before setting valid flag
                                // Ensures all slot fields are visible to consumer before valid=1
                                std::atomic_thread_fence(std::memory_order_release);
                                slot.valid.store(1, std::memory_order_release);
                                shm->frameRing.writeIndex.store(wIdx + 1, std::memory_order_release);
                            } else
                                shm->frameRing.droppedFrames.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            }
            perfMetrics.captureUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - captureStartUs);
        }
    }
}

// Delay overlay rendering for first frames after ImGui init
// This prevents GPU crashes when frame generation tech (DLSS FG/FSR FG) is
// initializing
static std::atomic<bool> s_initDelayComplete{false};
static std::atomic<int> s_framesBeforeInit{0};  // Defined earlier in ProcessFrame

void DX12_ResetImGuiFrameCounter() {
    s_framesBeforeInit = 0;
    // Also reset the post-init frame counter
    s_framesSinceInit = 0;
    HookLog("DX12: Reset ImGui frame counter");
}

void DX12_ResetOverlayFrameDelay() {
    s_framesSinceInit = 0;
    s_initDelayComplete = false;
    HookLog("DX12: Reset overlay frame delay counter");
}

void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain) {
    // CRITICAL: Skip all rendering during shutdown to prevent crashes
    if (HookIsShuttingDown()) {
        return;
    }

    // CRITICAL: Heartbeat FIRST - before ANY checks that might early-return
    // This ensures the freeze watchdog gets heartbeats even with FSR/DLSS FG
    // active
    g_RenderWatchdog.Heartbeat();

    // Retry FFX hook initialization periodically for late-loading FSR FG modules.
    // UE5 games often load amd_fidelityfx_framegeneration_dx12.dll after initial
    // hook setup completes, so we must retry until the module is found.
    static int s_ffxRetryCounter = 0;
    static bool s_ffxRetryLogged = false;
    if (!FFXHook::IsInitialized()) {
        // Retry FFX hook every 60 frames (UE5 games may load FFX modules late)
        if (++s_ffxRetryCounter % 60 == 0) {
            FFXHook::Init();
            if (FFXHook::IsInitialized()) {
                HookLogImportant("DX12: FFX Hook installed on render-frame retry #%d", s_ffxRetryCounter / 60);
            } else if (!s_ffxRetryLogged && s_ffxRetryCounter >= 600) {
                s_ffxRetryLogged = true;
                HookLogImportant(
                    "DX12: FFX Hook not found after %d render-frame retries (FG may use native integration)",
                    s_ffxRetryCounter / 60);
            }
        }
    }

    // CRITICAL FIX: Reset delay flag when ImGui is not initialized
    // This ensures we wait again after each init
    if (!g_State.overlayInit) {
        s_initDelayComplete = false;
        s_framesSinceInit = 0;
    }

    // Minimal delay after ImGui init before rendering overlay (for stability)
    if (g_State.overlayInit && !s_initDelayComplete.load()) {
        int frames = ++s_framesSinceInit;
        if (frames < 1) {
            // Skip - proceed immediately
            return;
        } else {
            s_initDelayComplete = true;
            HookLog(
                "DX12: ProcessFrameExternal - Overlay rendering enabled (frame "
                "%d after init)",
                frames);
        }
    }

    // CRITICAL FIX: Dynamically detect Vulkan WSI swapchains
    // When NVIDIA's Vulkan WSI-to-DXGI mapping is active, the swapchain is
    // presented through DXGI but the device is not a real D3D12 device we can
    // render to. Check this dynamically because games can switch between Vulkan
    // WSI (focused) and DXGI (unfocused) modes.
    static bool s_checkedForVulkan = false;
    static bool s_vulkanLayerActive = false;
    if (!s_checkedForVulkan) {
        HMODULE hVulkanLayer = GetModuleHandleW(L"VK_LAYER_CE_overlay.dll");
        if (!hVulkanLayer) {
            hVulkanLayer = GetModuleHandleW(L"VK_LAYER_CE_overlay_x86.dll");
        }
        s_vulkanLayerActive = (hVulkanLayer != nullptr);
        if (s_vulkanLayerActive) {
            HookLog(
                "DX12: Vulkan layer detected, will skip DXGI overlay for Vulkan "
                "WSI swapchains");
        }
        s_checkedForVulkan = true;
    }

    // If Vulkan layer is active, check if this is a Vulkan WSI swapchain
    // by attempting to get the D3D12 device - Vulkan WSI swapchains will fail
    // or return a device we can't use for rendering
    if (s_vulkanLayerActive && pSwapChain) {
        ID3D12Device* pDevice = nullptr;
        HRESULT hr = pSwapChain->GetDevice(IID_PPV_ARGS(&pDevice));
        if (FAILED(hr) || !pDevice) {
            // This is likely a Vulkan WSI swapchain - skip DX12 overlay
            // The Vulkan layer will handle overlay rendering
            return;
        }
        // Check if we can actually use this device (Vulkan WSI devices may fail
        // here)
        D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevels = {};
        hr = pDevice->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &featureLevels, sizeof(featureLevels));
        pDevice->Release();
        if (FAILED(hr)) {
            // Vulkan WSI device that doesn't support full D3D12 features
            return;
        }
    }

    if (!pSwapChain) {
        HookLog("DX12: ProcessFrameExternal - null swapchain");
        return;
    }
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) {
        HookLog("DX12: ProcessFrameExternal - failed to get SwapChain3");
        return;
    }
    int count = g_CommandListsExecutedThisFrame.exchange(0);
    ++g_FGDebugFrameCount;
    g_FGCompat.RecordFrame(count);
    // Interpolated (FG) frame detection: the game submits zero command lists
    // between consecutive Present calls for frames generated by the FG engine.
    bool isInterpolatedFrame = (count == 0);

    // ECL-count-based FG activation: detect frame generation via the pattern
    // of alternating real (ECL>0) and interpolated (ECL=0) frames.  This works
    // for UE5 native FSR FG and other implementations that don't use hookable
    // DLLs (e.g., statically linked into engine plugins).
    {
        static int s_eclRealFrames = 0;
        static int s_eclInterpFrames = 0;
        static bool s_eclFGDetected = false;
        if (isInterpolatedFrame)
            ++s_eclInterpFrames;
        else
            ++s_eclRealFrames;
        if (!s_eclFGDetected && s_eclInterpFrames >= 10 && s_eclRealFrames >= 5) {
            s_eclFGDetected = true;
            HookLogImportant("DX12: FG detected via ECL count pattern (real=%d, interp=%d)", s_eclRealFrames,
                             s_eclInterpFrames);
            g_FGCompat.SetFSRFGActive(true);
        }
    }

    // With the dedicated overlay queue, overlay commands execute on a separate
    // GPU queue with CPU-side fence synchronization, so it is safe to render
    // on both real and interpolated FG frames.  Without an overlay queue, we
    // must skip interpolated frames to avoid submitting work on the game queue
    // during Streamline's Present pipeline.
    bool hasDedicatedQueue = (g_State.overlayQueue != nullptr && g_State.crossQueueFence != nullptr &&
                              g_State.crossQueueFenceEvent != nullptr);
    if (isInterpolatedFrame && !hasDedicatedQueue) {
        sc3->Release();
        return;
    }
    // For interpolated frames, only render overlay (no capture processing) since
    // the backbuffer content is from the FG engine, not a real game frame.
    ProcessFrame(sc3, /*processCapture=*/!isInterpolatedFrame);
    sc3->Release();
}

namespace DXGIShared {
void HandleDX12ProcessFrame(IDXGISwapChain* pSwapChain, bool isRealFrame) {
    DX12_ProcessFrameExternal(pSwapChain);
}
void HandleDX12ResizeBegin() {
    HookLog("DX12: HandleDX12ResizeBegin CALLED from DetourResizeBuffers");
    DX12_OnSwapchainResizeBegin();
}
void HandleDX12ResizeEnd() {
    HookLog("DX12: HandleDX12ResizeEnd CALLED");
    DX12_OnSwapchainResizeEnd();
}
}  // namespace DXGIShared

// External function for swapchain wrapper to wait for overlay completion before
// Present
extern "C" __declspec(dllexport) void DX12_WaitForOverlayCompletion(ID3D12CommandQueue* pGameQueue) {
    (void)pGameQueue;
    if (!g_State.fence)
        return;

    UINT64 fenceValueToWait = g_State.currentFenceValue;
    if (fenceValueToWait == 0)
        return;

    // CRITICAL FIX: When window is not foreground, do NOT attempt any fence waiting.
    // Even WaitForSingleObject(fenceEvent, 0) can hang in the kernel driver
    // (NtGdiDdDDISubmitWaitForSyncObjectsToHwQueue) when GPU is throttled during Alt+Tab.
    // Just check if fence is already signaled and return immediately.
    // The overlay will be visible on the next frame when GPU resumes.
    if (g_State.fence->GetCompletedValue() >= fenceValueToWait)
        return;

    // Fence not ready - don't wait, just return. This is safe because:
    // 1. The overlay is still rendered, just might not be synchronized with this frame
    // 2. On next frame when GPU resumes, everything will be back in sync
    // 3. Prevents deadlock in driver when GPU is throttled
}

static const GUID SKID_D3D12SwapChainBufferBitmap = {
    0xbc53df3b, 0x956f, 0x47db, {0xa6, 0x53, 0x5, 0xd7, 0xb8, 0x71, 0x53, 0x38}};
static std::atomic<int> g_ECLCallCount{0};

void STDMETHODCALLTYPE DetourExecuteCommandLists(ID3D12CommandQueue* pThis, UINT NumCommandLists,
                                                 ID3D12CommandList* const* ppCommandLists) {
    // Heartbeat for freeze watchdog - ExecuteCommandLists is called frequently
    g_RenderWatchdog.Heartbeat();

    // CRITICAL: Recursion depth guard.  If an FG engine (FSR FG, DLSS FG)
    // hooks ECL and its "original" pointer loops back to us, we'd recurse
    // infinitely.  Detect and break the cycle by calling the real ECL directly.
    static thread_local int s_eclRecursionDepth = 0;
    if (s_eclRecursionDepth > 0) {
        // We're being called recursively — an FG hook is looping back to us.
        // Call the original (real D3D12) ECL directly to break the cycle.
        ExecuteCommandListsPtr original = oExecuteCommandLists;
        if (original)
            original(pThis, NumCommandLists, ppCommandLists);
        return;
    }
    ++s_eclRecursionDepth;
    auto depthGuard = ce::make_scope_guard([&]() { --s_eclRecursionDepth; });

    // Track that this thread is inside an ECL call.  During Alt+Tab, D3D12's
    // internal WaitImpl can pump window messages which may trigger Present →
    // ProcessFrame.  ProcessFrame checks s_insideECL and skips overlay rendering
    // to prevent a cascading second WaitImpl that hangs the render thread.
    bool wasInsideECL = s_insideECL;
    s_insideECL = true;
    auto eclGuard = ce::make_scope_guard([&]() { s_insideECL = wasInsideECL; });

    // Skip our own overlay queue - don't count overlay submissions as game
    // command lists and don't re-register the overlay queue as the game queue.
    if (pThis == g_State.overlayQueue) {
        ExecuteCommandListsPtr original = GetOriginalExecuteCommandLists(pThis);
        if (original)
            original(pThis, NumCommandLists, ppCommandLists);
        return;
    }

    // Debug: Log first few calls to verify hook is working
    int count = ++g_ECLCallCount;
    if (count <= 5) {
        HookLog("DX12: ExecuteCommandLists called #%d (queue=%p)", count, pThis);
    }

    // Count command lists from game queues to detect real frames
    g_CommandListsExecutedThisFrame.fetch_add(NumCommandLists, std::memory_order_relaxed);

    // Register game's queue for overlay execution
    DX12_SetCommandQueue(pThis);

    ExecuteCommandListsPtr original = GetOriginalExecuteCommandLists(pThis);
    if (original)
        original(pThis, NumCommandLists, ppCommandLists);
}

void DX12_HookQueueVTable(ID3D12CommandQueue* queue) {
    if (!queue)
        return;

    // Never hook our own overlay queue to avoid re-entry in ECL
    if (queue == g_State.overlayQueue)
        return;

    // We ALWAYS hook the queue for freeze detection heartbeat
    // The overlay rendering is skipped separately in ProcessFrameExternal if
    // needed This ensures freeze watchdog works even with DLSS/FSR FG

    void* unwrapped = nullptr;
    static const GUID IID_CWrapD3D12CommandQueue = {
        0xd4e5f678, 0x90ab, 0xcdef, {0x12, 0x34, 0x56, 0x78, 0x90, 0x12, 0x34, 0x56}};
    if (SUCCEEDED(queue->QueryInterface(IID_CWrapD3D12CommandQueue, &unwrapped))) {
        ((IUnknown*)unwrapped)->Release();
        return;
    }
    static std::recursive_mutex s_HookMutex;
    std::lock_guard<std::recursive_mutex> lock(s_HookMutex);
    void** vtbl = *reinterpret_cast<void***>(queue);

    // CRITICAL FIX: Check if we've already hooked this vtable BEFORE checking
    // the current vtable entry.  FG engines (FSR FG, DLSS FG) may overwrite
    // our vtable entry with their own hook.  If we detect the change and
    // re-hook, we create a circular hook chain:
    //   DetourECL → FG_ECL → DetourECL → FG_ECL → ... (stack overflow)
    // because FG's saved "original" points to our detour, and our new
    // "original" points to FG's hook.  The correct behavior is to leave
    // FG's hook in place — our detour is still in the chain via FG's
    // saved original pointer.
    {
        std::lock_guard<std::recursive_mutex> stateLock(g_ExecuteCommandListsHookStateMutex);
        bool alreadyHooked =
            g_ExecuteCommandListsOriginalByVTable.find(vtbl) != g_ExecuteCommandListsOriginalByVTable.end();
        if (alreadyHooked) {
            // Another hook (FSR FG, DLSS FG, etc.) may have replaced our
            // vtable entry, but the chain is intact:
            //   FG_ECL → DetourECL → realECL
            // Do NOT re-hook — that would create infinite recursion.
            if (vtbl[10] != (void*)DetourExecuteCommandLists) {
                static std::atomic<int> s_chainNotifyCount{0};
                if (s_chainNotifyCount.fetch_add(1, std::memory_order_relaxed) < 3) {
                    HookLogImportant(
                        "DX12: ECL vtable[%p] modified by FG engine (was our "
                        "detour, now %p) - chain intact, NOT re-hooking",
                        vtbl, vtbl[10]);
                }
            }
            return;
        }
    }

    if (vtbl[10] != (void*)DetourExecuteCommandLists) {
        HookLog("DX12: Hooking ExecuteCommandLists vtable for queue %p", queue);
        ExecuteCommandListsPtr original = nullptr;
        VTableHook::Status hookStatus =
            VTableHook::Create(&vtbl[10], (LPVOID)DetourExecuteCommandLists, (LPVOID*)&original);
        if (hookStatus == VTableHook::Success && original) {
            std::lock_guard<std::recursive_mutex> stateLock(g_ExecuteCommandListsHookStateMutex);
            g_ExecuteCommandListsOriginalByVTable[vtbl] = original;
            if (!oExecuteCommandLists)
                oExecuteCommandLists = original;
        }
    } else {
        std::lock_guard<std::recursive_mutex> stateLock(g_ExecuteCommandListsHookStateMutex);
        if (g_ExecuteCommandListsOriginalByVTable.find(vtbl) == g_ExecuteCommandListsOriginalByVTable.end() &&
            oExecuteCommandLists) {
            g_ExecuteCommandListsOriginalByVTable[vtbl] = oExecuteCommandLists;
        }
    }
}

// Hook device vtable for CreateSampler interception
void DX12_HookDeviceVTable(ID3D12Device* device) {
    if (!device)
        return;

    // Don't hook wrapped devices
    void* unwrapped = nullptr;
    static const GUID IID_CWrapD3D12Device = {
        0xc3d4e5f6, 0x7890, 0xabcd, {0xef, 0x12, 0x34, 0x56, 0x78, 0x90, 0x12, 0x34}};
    if (SUCCEEDED(device->QueryInterface(IID_CWrapD3D12Device, &unwrapped))) {
        ((IUnknown*)unwrapped)->Release();
        return;  // Already wrapped, skip vtable hook
    }

    static std::recursive_mutex s_DeviceHookMutex;
    std::lock_guard<std::recursive_mutex> lock(s_DeviceHookMutex);

    void** vtbl = *reinterpret_cast<void***>(device);

    // CreateSampler is at vtable index 20 in ID3D12Device
    // ID3D12Object: QueryInterface=0, AddRef=1, Release=2, GetPrivateData=3,
    // SetPrivateData=4, SetPrivateDataInterface=5, SetName=6 ID3D12Device:
    // GetNodeCount=7, CreateCommandQueue=8, CreateCommandAllocator=9,
    // CreateGraphicsPipelineState=10, CreateComputePipelineState=11,
    // CreateCommandList=12, CheckFeatureSupport=13, CreateDescriptorHeap=14,
    // GetDescriptorHandleIncrementSize=15, CreateRootSignature=16,
    // CreateConstantBufferView=17, CreateShaderResourceView=18,
    // CreateUnorderedAccessView=19, CreateRenderTargetView=20,
    // CreateDepthStencilView=21, CreateSampler=22 Let's use 22 for CreateSampler

    if (vtbl[22] != (void*)DetourCreateSampler) {
        HookLog("DX12: Hooking CreateSampler vtable for device %p", device);
        VTableHook::Create(&vtbl[22], (LPVOID)DetourCreateSampler, (LPVOID*)&oCreateSampler);
    }
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory* pThis, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                                IDXGISwapChain** ppSwapChain) {
    // CRITICAL: ALWAYS hook the command queue vtable for frame detection
    if (pDevice) {
        ID3D12CommandQueue* q = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&q)))) {
            DX12_HookQueueVTable(q);
            q->Release();
        }
    }

    HRESULT hr = oCreateSwapChain(pThis, pDevice, pDesc, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED((*ppSwapChain)->QueryInterface(IID_PPV_ARGS(&sc3)))) {
            sc3->Release();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd,
                                                       const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc, IDXGIOutput* pOut,
                                                       IDXGISwapChain1** ppSC) {
    // CRITICAL: ALWAYS hook the command queue vtable for frame detection
    if (pDevice) {
        ID3D12CommandQueue* q = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&q)))) {
            DX12_HookQueueVTable(q);
            q->Release();
        }
    }

    HRESULT hr = oCreateSwapChainForHwnd(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);

    if (SUCCEEDED(hr) && ppSC && *ppSC) {
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED((*ppSC)->QueryInterface(IID_PPV_ARGS(&sc3)))) {
            sc3->Release();
        }
    }
}

void STDMETHODCALLTYPE DetourCreateSampler(ID3D12Device* pDevice, const D3D12_SAMPLER_DESC* pDesc,
                                           D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (!pDesc || !oCreateSampler) {
        if (oCreateSampler)
            oCreateSampler(pDevice, pDesc, DestDescriptor);
        return;
    }

    D3D12_SAMPLER_DESC modifiedDesc = *pDesc;
    ApplyDX12SamplerOverridesCallback(&modifiedDesc);

    oCreateSampler(pDevice, &modifiedDesc, DestDescriptor);
}

HRESULT WINAPI DetourSerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* pRootSignature,
                                            D3D_ROOT_SIGNATURE_VERSION Version, ID3DBlob** ppBlob,
                                            ID3DBlob** ppErrorBlob) {
    if (!pRootSignature || !ppBlob) {
        if (oSerializeRootSignature)
            return oSerializeRootSignature(pRootSignature, Version, ppBlob, ppErrorBlob);
        return E_INVALIDARG;
    }

    HookLog("DetourSerializeRootSignature: CALLED, NumStaticSamplers=%u", pRootSignature->NumStaticSamplers);

    if (pRootSignature->NumStaticSamplers > 0 && pRootSignature->pStaticSamplers) {
        // Clone the descriptor with modified samplers
        D3D12_ROOT_SIGNATURE_DESC modified = *pRootSignature;
        std::vector<D3D12_STATIC_SAMPLER_DESC> modifiedSamplers(
            pRootSignature->pStaticSamplers, pRootSignature->pStaticSamplers + pRootSignature->NumStaticSamplers);

        bool anyModified = false;
        for (auto& sampler : modifiedSamplers) {
            if (RootSignatureParser::ApplyStaticSamplerOverrides(sampler)) {
                anyModified = true;
            }
        }

        if (anyModified) {
            HookLog(
                "DetourSerializeRootSignature: Modified %zu static samplers for "
                "AF/mip bias",
                modifiedSamplers.size());
            modified.pStaticSamplers = modifiedSamplers.data();
            if (oSerializeRootSignature)
                return oSerializeRootSignature(&modified, Version, ppBlob, ppErrorBlob);
        }
    }

    if (oSerializeRootSignature)
        return oSerializeRootSignature(pRootSignature, Version, ppBlob, ppErrorBlob);
    return E_FAIL;
}

HRESULT WINAPI DetourSerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pRootSignature,
                                                     ID3DBlob** ppBlob, ID3DBlob** ppErrorBlob) {
    if (!pRootSignature || !ppBlob) {
        if (oSerializeVersionedRootSignature)
            return oSerializeVersionedRootSignature(pRootSignature, ppBlob, ppErrorBlob);
        return E_INVALIDARG;
    }

    uint32_t numStaticSamplers = 0;
    if (pRootSignature->Version == D3D_ROOT_SIGNATURE_VERSION_1)
        numStaticSamplers = pRootSignature->Desc_1_0.NumStaticSamplers;
    HookLog(
        "DetourSerializeVersionedRootSignature: CALLED, Version=%u, "
        "NumStaticSamplers=%u",
        pRootSignature->Version, numStaticSamplers);

    if (pRootSignature->Version == D3D_ROOT_SIGNATURE_VERSION_1) {
        const D3D12_ROOT_SIGNATURE_DESC* pDesc = &pRootSignature->Desc_1_0;

        if (pDesc->NumStaticSamplers > 0 && pDesc->pStaticSamplers) {
            D3D12_VERSIONED_ROOT_SIGNATURE_DESC modified = *pRootSignature;
            D3D12_ROOT_SIGNATURE_DESC modifiedDesc = *pDesc;

            std::vector<D3D12_STATIC_SAMPLER_DESC> modifiedSamplers(pDesc->pStaticSamplers,
                                                                    pDesc->pStaticSamplers + pDesc->NumStaticSamplers);

            bool anyModified = false;
            for (auto& sampler : modifiedSamplers) {
                if (RootSignatureParser::ApplyStaticSamplerOverrides(sampler)) {
                    anyModified = true;
                }
            }

            if (anyModified) {
                HookLog(
                    "DetourSerializeVersionedRootSignature: Modified %zu static "
                    "samplers (v1.0)",
                    modifiedSamplers.size());
                modifiedDesc.pStaticSamplers = modifiedSamplers.data();
                modified.Desc_1_0 = modifiedDesc;
                if (oSerializeVersionedRootSignature)
                    return oSerializeVersionedRootSignature(&modified, ppBlob, ppErrorBlob);
            }
        }
    }

    if (oSerializeVersionedRootSignature)
        return oSerializeVersionedRootSignature(pRootSignature, ppBlob, ppErrorBlob);
    return E_FAIL;
}

HRESULT STDMETHODCALLTYPE DetourCreateCommittedResource(ID3D12Device* device,
                                                        const D3D12_HEAP_PROPERTIES* pHeapProperties,
                                                        D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc,
                                                        D3D12_RESOURCE_STATES InitialResourceState,
                                                        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
                                                        REFIID riidResource, void** ppvResource) {
    if (oCreateCommittedResource)
        return oCreateCommittedResource(device, pHeapProperties, HeapFlags, pDesc, InitialResourceState,
                                        pOptimizedClearValue, riidResource, ppvResource);
    return E_FAIL;
}

void DX12Hook::Shutdown() {
    CleanupResources();
    CleanupOverlay();
    CleanupRTVs();
    {
        std::lock_guard<std::recursive_mutex> lock(g_DeviceQueuesMutex);
        for (auto& pair : g_DeviceQueues)
            if (pair.second)
                pair.second->Release();
        g_DeviceQueues.clear();
    }
    if (g_SwapchainQueue) {
        g_SwapchainQueue->Release();
        g_SwapchainQueue = nullptr;
    }
    if (g_CommandQueue.load()) {
        g_CommandQueue.load()->Release();
        g_CommandQueue.store(nullptr);
    }
    {
        std::lock_guard<std::recursive_mutex> lock(g_ExecuteCommandListsHookStateMutex);
        g_ExecuteCommandListsOriginalByVTable.clear();
        oExecuteCommandLists = nullptr;
    }
    if (g_Device.load()) {
        g_Device.load()->Release();
        g_Device.store(nullptr);
    }
    // g_LastSwapChain is a raw (non-AddRef'd) pointer — do NOT Release
    if (g_LastSwapChain) {
        g_LastSwapChain = nullptr;
    }
    if (g_SharedCaptureD3D12.IsActive())
        g_SharedCaptureD3D12.Reset();
    g_IPCReady = false;
}

void DX12Hook::OnHostDisconnect() {
    g_IPCReady = false;
}
void DX12Hook::TrackResource(IUnknown* res) {
    if (!res)
        return;
    std::lock_guard<std::recursive_mutex> lock(resourceMutex);
    res->AddRef();
    trackedResources.push_back(res);
}
void DX12Hook::CleanupResources() {
    std::lock_guard<std::recursive_mutex> lock(resourceMutex);
    for (auto* res : trackedResources)
        if (res)
            res->Release();
    trackedResources.clear();
}

bool DX12Hook::IsRealFrame() const {
    return g_FGCompat.IsCurrentFrameReal();
}

void DX12Hook::ClassifyFrame(int commandListCount) {
    g_FGCompat.RecordFrame(commandListCount);
}

// FIXED: Clean up the global hook instance if allocated
DWORD WINAPI UnloadThread(LPVOID lpParam) {
    Sleep(200);
    if (g_dx12HookInstance) {
        g_dx12HookInstance->Shutdown();
        delete g_dx12HookInstance;
        g_dx12HookInstance = nullptr;
    }
    return 0;
}
