#include <combaseapi.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <unknwn.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../../common/frame_timing.h"
#include "../../common/raii_helpers.h"
#include "../capture/shared_capture.h"
#include "../common/capture_base.h"
#include "../common/fg_detection.h"
#include "../common/fps_limiter.h"
#include "../common/hook_common.h"
#include "../common/input_manager.h"
#include "../common/overlay.h"
#include "../common/performance_metrics.h"
#include "../common/streamline_compat.h"
#include "../common/swapchain_wrapper.h"
#include "../common/system_metrics.h"
#include "../wrappers/d3d12_wrapper_interface.h"
#include "../wrappers/wrapper_hooks.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"
#include "dx11_hook.h"
#include "dx12_hook.h"
#include "graphics_hook.h"
#include "imgui.h"
#include "lod_helper.h"

#include "../wrappers/vtable_hook.h"
#include "../wrappers/wrapper_base.h"
#include "dxgi_shared.h"

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
    static const int ALLOC_POOL_SIZE = 3;
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
    bool imGuiInit = false;
    bool syncInit = false;
    int cachedWidth = 0;
    int cachedHeight = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT bufferCount = 0;
    IDXGISwapChain* cachedSwapChain = nullptr;
    void Cleanup()
    {
        for (auto& bb : backBuffers)
            if (bb) bb->Release();
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
            if (alloc) alloc->Release();
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
        imGuiInit = false;
        syncInit = false;
    }
};

static DX12OverlayState g_State;
static std::recursive_mutex g_ImGuiFrameMutex;
static SharedCaptureD3D12 g_SharedCaptureD3D12;

ID3D12Device* g_Device = nullptr;
ID3D12CommandQueue* g_CommandQueue = nullptr;
std::recursive_mutex g_CommandQueueMutex;
bool g_IPCReady = false;
static IDXGISwapChain* g_LastSwapChain = nullptr;
static ID3D12Resource* g_DummyBackBuffer = nullptr;
// LOCK HIERARCHY (MUST be acquired in this order to prevent deadlocks):
// 1. g_OverlayMutex (outermost - protects overlay state)
// 2. g_CommandQueueMutex (protects command queue pointer)
// 3. g_DX12CaptureMutex (innermost - protects capture state)
//
// Rule: When acquiring multiple locks, always acquire in order above.
//       Use std::lock_guard with std::adopt_lock when using try_lock().
static std::recursive_mutex g_OverlayMutex;
static std::recursive_mutex g_InitImGuiMutex;
static std::recursive_mutex g_DX12CaptureMutex;
static std::atomic<bool> g_InSwapchainResizeCleanup{false};
static std::atomic<int> g_CommandListsExecutedThisFrame{0};
static std::atomic<uint64_t> g_FGDebugFrameCount{0};

// Use pointer to prevent static destructor execution in non-game processes (Explorer fix)
DX12Hook* g_dx12HookInstance = nullptr;

// Cached overlay renderer for zero-overhead interpolated frame rendering
overlay::CachedOverlayRenderer* g_CachedOverlayRenderer = nullptr;
bool g_UseCachedRenderer = false;  // DISABLED: Cached renderer needs proper D3D12 secondary command list implementation

std::recursive_mutex g_DeviceQueuesMutex;
std::map<ID3D12Device*, ID3D12CommandQueue*> g_DeviceQueues;

// CPU Prerender Limit State (DX12)
static std::vector<ID3D12Fence*> g_PrerenderFences;
static std::vector<HANDLE> g_PrerenderEvents;
static uint64_t g_PrerenderFrameIndex = 0;
static std::mutex g_PrerenderMutex;

// Forward Declarations
void STDMETHODCALLTYPE DetourExecuteCommandLists(ID3D12CommandQueue* pThis, UINT NumCommandLists,
                                                 ID3D12CommandList* const* ppCommandLists);
void DX12_HookQueueVTable(ID3D12CommandQueue* queue);
HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory* pThis, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                                IDXGISwapChain** ppSwapChain);
HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd,
                                                       const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc, IDXGIOutput* pOut,
                                                       IDXGISwapChain1** ppSC);

// REQUIRED EXPORTS
__declspec(dllexport) void DX12_AdjustWrapperResizeDepth(int delta)
{
    if (delta > 0)
        DXGIShared::g_SharedState.wrapperResizeDepth.fetch_add(delta);
    else
        DXGIShared::g_SharedState.wrapperResizeDepth.fetch_sub(-delta);
}

__declspec(dllexport) void DX12_InvalidateSwapchain()
{
    DXGIShared::g_SharedState.swapchainInvalid.store(true, std::memory_order_release);
    HookLog("DX12: Swapchain marked INVALID");
}

__declspec(dllexport) void DX12_SignalFSR4SwapchainRecreated()
{
    DXGIShared::g_SharedState.fsr4RecreationPending.store(true, std::memory_order_release);
    HookLog("DX12: FSR4 swapchain recreation signaled");
}

// C Linkage Exports for cross-module calls (e.g. from C clients or GetProcAddress)
extern "C" {
__declspec(dllexport) void DX12_SetCommandQueue(ID3D12CommandQueue* pQueue)
{
    if (!pQueue) return;

    // CRITICAL FIX: Only allow DIRECT queues for overlay rendering.
    // Strange Brigade and other DX12 games use Async Compute queues.
    // Submitting overlay (Direct) commands to a Compute queue causes a device lost/crash.
    D3D12_COMMAND_QUEUE_DESC desc = pQueue->GetDesc();
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        // HookLog("DX12: Ignoring non-direct queue (Type=%d)", desc.Type);
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    if (g_CommandQueue != pQueue) {
        if (g_CommandQueue) g_CommandQueue->Release();
        g_CommandQueue = pQueue;
        g_CommandQueue->AddRef();
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(g_CommandQueue->GetDevice(IID_PPV_ARGS(&dev)))) {
            if (g_Device != dev) {
                if (g_Device) g_Device->Release();
                g_Device = dev;
            } else
                dev->Release();
        }
    }
}
__declspec(dllexport) void DX12_AdjustWrapperResizeDepth_C(int delta) { DX12_AdjustWrapperResizeDepth(delta); }

// Export for D3D12 wrapper to notify command list execution (frame classification)
__declspec(dllexport) void DX12_NotifyCommandLists(UINT numCommandLists)
{
    g_CommandListsExecutedThisFrame.fetch_add(numCommandLists, std::memory_order_relaxed);
}
}

void DX12_OnSwapchainResizeEnd();
void DX12_OnSwapchainResizeBegin();

// Helper to ensure global hook instance exists
void EnsureDX12Hook()
{
    if (!g_dx12HookInstance) {
        g_dx12HookInstance = new DX12Hook();
    }
}

void DX12Hook::Init()
{
    EnsureDX12Hook();  // Self-init check
    static std::recursive_mutex s_InitMutex;
    static bool s_InitDone = false;
    std::lock_guard<std::recursive_mutex> lock(s_InitMutex);
    if (s_InitDone) return;
    s_InitDone = true;

    // WRAPPER-ONLY ARCHITECTURE: DXGI vtable hooks are removed.
    // Present/ResizeBuffers/CreateSwapChain are handled by CWrapDXGISwapChain/CWrapDXGIFactory2.
    // We only need to hook ExecuteCommandLists for frame detection, which is done
    // dynamically when a command queue is first seen.
    HookLog("DX12Hook: Initialized (wrapper-only mode, no DXGI vtable hooks)");
}

void ShutdownImGui()
{
    if (!g_State.imGuiInit) return;
    
    // Shutdown cached overlay renderer
    if (g_CachedOverlayRenderer) {
        g_CachedOverlayRenderer->Shutdown();
        delete g_CachedOverlayRenderer;
        g_CachedOverlayRenderer = nullptr;
    }
    
    ImGui_ImplDX12_Shutdown();
    g_SharedOverlay.ShutdownImGui();
    if (g_State.srvDescHeap) {
        g_State.srvDescHeap->Release();
        g_State.srvDescHeap = nullptr;
    }
    g_State.imGuiInit = false;
}

bool InitImGui(ID3D12Device* device, int buffers, DXGI_FORMAT format, HWND hwnd)
{
    std::lock_guard<std::recursive_mutex> lock(g_InitImGuiMutex);
    if (g_State.imGuiInit) return true;
    g_State.format = format;
    g_SharedOverlay.InitImGui(hwnd);
    InputManager::Get().HookWindow(hwnd);
    D3D12_DESCRIPTOR_HEAP_DESC desc = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    UINT nodeCount = device->GetNodeCount();
    if (nodeCount > 1) desc.NodeMask = 1;
    if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_State.srvDescHeap)))) {
        desc.NodeMask = 0;
        if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_State.srvDescHeap)))) return false;
    }
    if (!ImGui_ImplDX12_Init(device, buffers, format, g_State.srvDescHeap,
                             g_State.srvDescHeap->GetCPUDescriptorHandleForHeapStart(),
                             g_State.srvDescHeap->GetGPUDescriptorHandleForHeapStart())) {
        g_State.srvDescHeap->Release();
        g_State.srvDescHeap = nullptr;
        return false;
    }
    if (g_CommandQueue) ImGui_ImplDX12_SetCommandQueue(g_CommandQueue);
    g_State.imGuiInit = true;
    
    // Initialize cached overlay renderer for Frame Generation support
    if (g_UseCachedRenderer && !g_CachedOverlayRenderer) {
        g_CachedOverlayRenderer = new overlay::CachedOverlayRenderer();
        if (!g_CachedOverlayRenderer->Initialize(device, g_CommandQueue, buffers)) {
            HookLog("DX12: Failed to initialize cached overlay renderer, falling back to standard");
            delete g_CachedOverlayRenderer;
            g_CachedOverlayRenderer = nullptr;
            g_UseCachedRenderer = false;
        } else {
            HookLog("DX12: Cached overlay renderer initialized for %d buffers", buffers);
        }
    }
    
    return true;
}

void DrawOverlay(ID3D12GraphicsCommandList* cmdList, bool isRealFrame, UINT bufferIdx)
{
    if (!g_State.imGuiInit || !cmdList) return;
    
    // Use cached renderer for Frame Generation support when available
    if (g_UseCachedRenderer && g_CachedOverlayRenderer) {
        // Update content on real frames OR if content has never been updated (first frame)
        // This ensures we have content to render even if all frames appear as "interpolated"
        bool needsContentUpdate = isRealFrame || g_CachedOverlayRenderer->ShouldUpdateContent();
        
        if (needsContentUpdate) {
            // Build overlay content using shared overlay system
            ImGui_ImplDX12_NewFrame();
            g_SharedOverlay.BeginFrame();
            g_SharedOverlay.SetMetrics(DXGIShared::GetPerformanceMetrics());
            g_SharedOverlay.SetIPCClient(g_IPC);
            const char* api = "DX12";
            if (GetModuleHandleA("d3d12core.dll") && 
                (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll"))) {
                api = "DX12 (VKD3D)";
            }
            g_SharedOverlay.SetGraphicsAPI(api);
            bool isHDR = (g_State.format == DXGI_FORMAT_R16G16B16A16_FLOAT || 
                         g_State.format == DXGI_FORMAT_R10G10B10A2_UNORM);
            g_SharedOverlay.SetHDR(isHDR);
            g_SharedOverlay.RenderUI();
            g_SharedOverlay.EndFrame();
            
            // Update cached renderer content
            auto* metrics = DXGIShared::GetPerformanceMetrics();
            g_CachedOverlayRenderer->UpdateContent(metrics, 
                                                   (float)g_State.cachedWidth, 
                                                   (float)g_State.cachedHeight,
                                                   0.0f); // Delta time not critical for overlay
        }
        
        // Render using cached renderer (fast path for both real and interpolated frames)
        ImVec2 displaySize((float)g_State.cachedWidth, (float)g_State.cachedHeight);
        g_CachedOverlayRenderer->Render(cmdList, bufferIdx, isRealFrame, displaySize);
        return;
    }
    
    // Standard overlay rendering (fallback path when cached renderer not available)
    ImGui_ImplDX12_NewFrame();
    g_SharedOverlay.BeginFrame();
    g_SharedOverlay.SetMetrics(DXGIShared::GetPerformanceMetrics());
    g_SharedOverlay.SetIPCClient(g_IPC);
    const char* api = "DX12";
    if (GetModuleHandleA("d3d12core.dll") && (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll")))
        api = "DX12 (VKD3D)";
    g_SharedOverlay.SetGraphicsAPI(api);
    bool isHDR = (g_State.format == DXGI_FORMAT_R16G16B16A16_FLOAT || g_State.format == DXGI_FORMAT_R10G10B10A2_UNORM);
    g_SharedOverlay.SetHDR(isHDR);
    g_SharedOverlay.RenderUI();
    g_SharedOverlay.EndFrame();
    cmdList->SetDescriptorHeaps(1, &g_State.srvDescHeap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList);
}

void CreateRTVs(ID3D12Device* device, IDXGISwapChain3* swapChain, int bufferCount)
{
    if (g_State.rtvDescHeap) return;
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, (UINT)bufferCount,
                                              D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_State.rtvDescHeap)))) return;
    g_State.bufferCount = bufferCount;
    g_State.cachedSwapChain = swapChain;
    g_State.rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
    for (int i = 0; i < bufferCount; i++) {
        ID3D12Resource* bb = nullptr;
        if (SUCCEEDED(swapChain->GetBuffer(i, IID_PPV_ARGS(&bb))) && bb) {
            device->CreateRenderTargetView(bb, nullptr, rtvHandle);
            bb->Release();
        }
        rtvHandle.ptr += g_State.rtvDescriptorSize;
    }
}

void InitOverlaySync(ID3D12Device* device, int bufferCount)
{
    if (g_State.syncInit) return;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_State.fence)))) return;

    g_State.allocators.resize(DX12OverlayState::ALLOC_POOL_SIZE);
    g_State.fenceValues.resize(DX12OverlayState::ALLOC_POOL_SIZE, 0);

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
            if (alloc) alloc->Release();
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

static bool DrainCommandQueue(ID3D12CommandQueue* queue, ID3D12Device* device)
{
    if (!queue || !device) return false;
    
    // NON-BLOCKING DRAIN: Use a flush approach instead of waiting
    // to avoid deadlocking when called from the submit thread.
    ID3D12Fence* fence = nullptr;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    
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

void CleanupOverlay()
{
    if (!g_State.syncInit) return;
    if (g_State.fence && g_CommandQueue) {
        UINT64 waitValue = g_State.currentFenceValue + 1;
        if (SUCCEEDED(g_CommandQueue->Signal(g_State.fence, waitValue))) {
            if (g_State.fence->GetCompletedValue() < waitValue) {
                g_State.fence->SetEventOnCompletion(waitValue, g_State.fenceEvent);
                WaitForSingleObject(g_State.fenceEvent, 100);
            }
        }
    }
    if (g_State.fenceEvent) {
        CloseHandle(g_State.fenceEvent);
        g_State.fenceEvent = NULL;
    }
    for (auto alloc : g_State.allocators)
        if (alloc) alloc->Release();
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
    g_State.currentFenceValue = 0;
    g_State.allocIndex = 0;
    g_State.syncInit = false;
    ShutdownImGui();
}

void CleanupRTVs()
{
    for (auto* r : g_State.backBuffers)
        if (r) r->Release();
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
}

void DX12_OnSwapchainResizeBegin()
{
    bool wasAlreadySet = g_InSwapchainResizeCleanup.exchange(true);
    HookLog("DX12: DX12_OnSwapchainResizeBegin called, wasAlreadySet=%d", wasAlreadySet);
    
    // Prevent recursion - if already in resize, return immediately
    if (wasAlreadySet) {
        HookLog("DX12: DX12_OnSwapchainResizeBegin - already in resize, returning early");
        return;
    }
    
    DXGIShared::g_SharedState.lastSwapchainCreation = std::chrono::steady_clock::now();
    HookLog("DX12: DX12_OnSwapchainResizeBegin - step 1: timestamp updated");

    // Use try_lock to avoid blocking the render thread
    if (!g_OverlayMutex.try_lock()) {
        HookLog("DX12: DX12_OnSwapchainResizeBegin - mutex busy, returning early");
        return;
    }
    HookLog("DX12: DX12_OnSwapchainResizeBegin - step 2: got mutex");
    
    // RAII unlock when we exit
    std::lock_guard<std::recursive_mutex> lock(g_OverlayMutex, std::adopt_lock);

    // DO NOT release D3D12 resources here - just mark them invalid.
    // The real ResizeBuffers will handle synchronization internally.
    // Releasing resources now can cause the GPU to hang waiting for them.
    
    // Just mark ImGui as not initialized - resources will be cleaned up after resize
    g_State.imGuiInit = false;
    g_State.syncInit = false;
    HookLog("DX12: DX12_OnSwapchainResizeBegin - step 3: marked state invalid");
    
    // Release swapchain reference only (this is safe)
    if (g_LastSwapChain) {
        g_LastSwapChain->Release();
        g_LastSwapChain = nullptr;
    }
    HookLog("DX12: DX12_OnSwapchainResizeBegin - complete");
}

void DX12_OnSwapchainResizeEnd() { 
    HookLog("DX12: DX12_OnSwapchainResizeEnd called");
    // Only clear if it was set - prevents unbalanced calls from clearing prematurely
    if (g_InSwapchainResizeCleanup.load(std::memory_order_acquire)) {
        g_InSwapchainResizeCleanup.store(false, std::memory_order_release); 
    }
}

// --- CPU Prerender Limit Support (DX12) ---
static void ApplyPrerenderLimitDX12(float limit)
{
    if (limit < 0.0f) return;
    if (!g_Device || !g_CommandQueue) return;
    
    std::lock_guard<std::mutex> lock(g_PrerenderMutex);
    
    // Initialize fence ring buffer if needed
    if (g_PrerenderFences.empty()) {
        for (int i = 0; i < 16; i++) {
            ID3D12Fence* fence = nullptr;
            HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (SUCCEEDED(g_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
                g_PrerenderFences.push_back(fence);
                g_PrerenderEvents.push_back(event);
            } else if (event) {
                CloseHandle(event);
            }
        }
        HookLog("DX12: Created prerender limit fence ring buffer (size: %d)", (int)g_PrerenderFences.size());
    }
    
    if (g_PrerenderFences.empty()) return;
    
    size_t idx = g_PrerenderFrameIndex % g_PrerenderFences.size();
    ID3D12Fence* fence = g_PrerenderFences[idx];
    HANDLE event = g_PrerenderEvents[idx];
    
    if (limit == 0.0f) {
        // Strict Serial: Signal and immediately wait
        uint64_t value = g_PrerenderFrameIndex + 1;
        g_CommandQueue->Signal(fence, value);
        if (SUCCEEDED(fence->SetEventOnCompletion(value, event))) {
            WaitForSingleObject(event, INFINITE);
        }
    } else {
        // Buffered Limit
        bool isFractional = (limit > 0.01f && limit < 1.0f);
        int effectiveLimit = isFractional ? 1 : (int)limit;
        int lookback = effectiveLimit + 1;
        
        // Signal current frame
        uint64_t signalValue = g_PrerenderFrameIndex + 1;
        g_CommandQueue->Signal(fence, signalValue);
        
        // Wait on N frames ago
        if (g_PrerenderFrameIndex >= (uint64_t)lookback) {
            size_t waitIdx = (g_PrerenderFrameIndex - lookback) % g_PrerenderFences.size();
            ID3D12Fence* waitFence = g_PrerenderFences[waitIdx];
            HANDLE waitEvent = g_PrerenderEvents[waitIdx];
            uint64_t waitValue = (g_PrerenderFrameIndex - lookback) + 1;
            
            if (waitFence->GetCompletedValue() < waitValue) {
                if (SUCCEEDED(waitFence->SetEventOnCompletion(waitValue, waitEvent))) {
                    WaitForSingleObject(waitEvent, INFINITE);
                }
            }
        }
    }
    
    g_PrerenderFrameIndex++;
}

void ProcessFrame(IDXGISwapChain* pSwapChain, bool processCapture)
{
    bool inResize = g_InSwapchainResizeCleanup.load(std::memory_order_acquire);
    if (!pSwapChain || inResize) {
        HookLog("DX12: ProcessFrame - early return (null=%d, inResize=%d)", !pSwapChain, inResize);
        return;
    }
    
    // CPU Prerender Limit - Apply before any rendering
    float prerenderLimit = GetActivePrerenderLimit();
    if (prerenderLimit >= 0.0f) {
        ApplyPrerenderLimitDX12(prerenderLimit);
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

    // OPTIMIZATION: Only resolve device if swapchain changed or device not yet known
    if (!g_Device || pSwapChain != g_LastSwapChain) {
        IUnknown* pUnk = nullptr;
        if (FAILED(pSwapChain->GetDevice(IID_PPV_ARGS(&pUnk)))) {
            HookLog("DX12: ProcessFrame - failed to get device from swapchain");
            return;
        }
        ID3D12Device* activeDevice = nullptr;
        ID3D12CommandQueue* activeQueue = nullptr;
        if (SUCCEEDED(pUnk->QueryInterface(IID_PPV_ARGS(&activeQueue)))) {
            activeQueue->GetDevice(IID_PPV_ARGS(&activeDevice));
            DX12_HookQueueVTable(activeQueue);
            activeQueue->Release();
            HookLog("DX12: ProcessFrame - got device via command queue");
        } else {
            pUnk->QueryInterface(IID_PPV_ARGS(&activeDevice));
            HookLog("DX12: ProcessFrame - got device directly (no queue)");
        }
        pUnk->Release();

        if (!activeDevice) {
            HookLog("DX12: ProcessFrame - no active device");
            return;
        }

        // Initialize SystemMetricsCollector with adapter LUID when device is first obtained
        // This MUST happen before any device/swapchain change detection
        // NOTE: The D3D12 device and swap chain are wrapped, so we can't use QueryInterface
        // or GetParent directly. Instead, we get the output from the swap chain desc,
        // then get the adapter from the output.
        static bool s_metricsInitialized = false;
        if (!s_metricsInitialized && pSwapChain) {
            DXGI_SWAP_CHAIN_DESC swapDesc;
            if (SUCCEEDED(pSwapChain->GetDesc(&swapDesc))) {
                // Get the output from the swap chain, then get adapter from output
                IDXGIOutput* output = swapDesc.OutputWindow ? nullptr : nullptr;
                // Actually, swapDesc.OutputWindow is HWND, not IDXGIOutput
                // Let's enumerate outputs from the swap chain
                
                // Alternative: Use EnumAdapters to find the adapter that matches the device's node
                // Or: Create a temporary DXGI factory and find the adapter
                
                // Best approach: Use the fact that we can get IDXGIDevice from the swap chain's device
                // even if the D3D12 device is wrapped
                IUnknown* pDeviceUnk = nullptr;
                if (SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&pDeviceUnk)))) {
                    // Try to query IDXGIDevice directly from the device
                    IDXGIDevice* dxgiDevice = nullptr;
                    if (SUCCEEDED(pDeviceUnk->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
                        IDXGIAdapter* adapter = nullptr;
                        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                            DXGI_ADAPTER_DESC desc;
                            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                                SystemMetricsCollector::Get().Initialize(
                                    (int32_t)desc.AdapterLuid.LowPart,
                                    (int32_t)desc.AdapterLuid.HighPart);
                                SystemMetricsCollector::Get().SetVRAMTotal(desc.DedicatedVideoMemory);
                                HookLog("DX12: SystemMetricsCollector initialized with LUID %08X:%08X, VRAM: %llu MB",
                                        desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart,
                                        desc.DedicatedVideoMemory / (1024 * 1024));
                                s_metricsInitialized = true;
                            }
                            adapter->Release();
                        }
                        dxgiDevice->Release();
                    } else {
                        HookLog("DX12: Failed to query IDXGIDevice from swap chain's device");
                        
                        // Fallback: Create DXGI factory and use first adapter
                        // This is better than nothing - most systems have one GPU
                        IDXGIFactory1* factory = nullptr;
                        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
                            IDXGIAdapter* adapter = nullptr;
                            if (SUCCEEDED(factory->EnumAdapters(0, &adapter))) {
                                DXGI_ADAPTER_DESC desc;
                                if (SUCCEEDED(adapter->GetDesc(&desc))) {
                                    SystemMetricsCollector::Get().Initialize(
                                        (int32_t)desc.AdapterLuid.LowPart,
                                        (int32_t)desc.AdapterLuid.HighPart);
                                    SystemMetricsCollector::Get().SetVRAMTotal(desc.DedicatedVideoMemory);
                                    HookLog("DX12: SystemMetricsCollector FALLBACK init with LUID %08X:%08X, VRAM: %llu MB",
                                            desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart,
                                            desc.DedicatedVideoMemory / (1024 * 1024));
                                    s_metricsInitialized = true;
                                }
                                adapter->Release();
                            }
                            factory->Release();
                        }
                    }
                    pDeviceUnk->Release();
                } else {
                    HookLog("DX12: Failed to get device from swap chain");
                }
            } else {
                HookLog("DX12: Failed to get swap chain desc");
            }
        }

        // DIAGNOSTIC: Check if SystemMetricsCollector has been initialized
        static bool s_metricsInitLogged = false;
        if (!s_metricsInitLogged) {
            auto metrics = SystemMetricsCollector::Get().GetMetrics();
            HookLog("DX12: DIAGNOSTIC - SystemMetricsCollector GPU usage: %.1f%%, VRAM: %.1f MB",
                    metrics.gpuUsage, metrics.vramUsed / (1024.0f * 1024.0f));
            s_metricsInitLogged = true;
        }

        if (g_Device == nullptr || activeDevice != g_Device || pSwapChain != g_LastSwapChain) {
            if (g_Device) {
                CleanupOverlay();
                CleanupRTVs();
                ShutdownImGui();
                g_SharedCaptureD3D12.Reset();
                g_Device->Release();
            }
            g_Device = activeDevice;
            g_Device->AddRef();
            if (g_LastSwapChain) g_LastSwapChain->Release();
            g_LastSwapChain = pSwapChain;
            g_LastSwapChain->AddRef();
            g_State.imGuiInit = false;
            HookLog("DX12: ProcessFrame - new device/swapchain, ImGui reset");
        }
        activeDevice->Release();
    }
    ID3D12CommandQueue* q = nullptr;
    {
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        if (g_CommandQueue) {
            q = g_CommandQueue;
            q->AddRef();
        }
    }
    if (!q) {
        HookLog("DX12: ProcessFrame - no command queue, skipping overlay");
        return;
    }
    HookLog("DX12: ProcessFrame - got queue, imGuiInit=%d", g_State.imGuiInit);
    if (g_State.imGuiInit) ImGui_ImplDX12_SetCommandQueue(q);
    if (!g_State.imGuiInit) {
        DXGI_SWAP_CHAIN_DESC desc;
        if (SUCCEEDED(pSwapChain->GetDesc(&desc))) {
            g_State.cachedWidth = desc.BufferDesc.Width;
            g_State.cachedHeight = desc.BufferDesc.Height;
            HookLog("DX12: ProcessFrame - initializing ImGui (%dx%d, buffers=%d)", 
                    g_State.cachedWidth, g_State.cachedHeight, desc.BufferCount);
            if (InitImGui(g_Device, desc.BufferCount, desc.BufferDesc.Format, desc.OutputWindow)) {
                IDXGISwapChain3* sc3 = nullptr;
                if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
                    CreateRTVs(g_Device, sc3, desc.BufferCount);
                    InitOverlaySync(g_Device, desc.BufferCount);
                    sc3->Release();
                }
                HookLog("DX12: ProcessFrame - ImGui initialized, syncInit=%d", g_State.syncInit);
            } else {
                HookLog("DX12: ProcessFrame - ImGui initialization FAILED");
            }
        } else {
            HookLog("DX12: ProcessFrame - failed to get swapchain desc");
        }
    }
    if (g_State.imGuiInit && g_State.syncInit) {
        HookLog("DX12: ProcessFrame - drawing overlay (cachedRenderer=%p, useCached=%d)", 
                g_CachedOverlayRenderer, g_UseCachedRenderer);
        int idx = g_State.allocIndex;
        g_State.allocIndex = (idx + 1) % DX12OverlayState::ALLOC_POOL_SIZE;

        bool safeToProceed = true;
        if (g_State.fence) {
            UINT64 comp = g_State.fence->GetCompletedValue();
            UINT64 target = g_State.fenceValues[idx];
            if (comp < target) {
                HRESULT hr = g_State.fence->SetEventOnCompletion(target, g_State.fenceEvent);
                if (SUCCEEDED(hr)) {
                    // Wait strictly. If wait fails, we assume state is bad.
                    if (WaitForSingleObject(g_State.fenceEvent, INFINITE) != WAIT_OBJECT_0) {
                        safeToProceed = false;
                    }
                } else {
                    // Device removed or other error
                    safeToProceed = false;
                }
            }
        } else {
            safeToProceed = false;
        }

        if (safeToProceed) {
            auto* list = g_State.cmdList;
            auto* alloc = g_State.allocators[idx];
            if (list && alloc) {
                if (SUCCEEDED(alloc->Reset())) {
                    if (SUCCEEDED(list->Reset(alloc, nullptr))) {
                        IDXGISwapChain3* sc3 = nullptr;
                        if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
                            UINT bufferIdx = sc3->GetCurrentBackBufferIndex();
                            ID3D12Resource* bb = nullptr;
                            if (SUCCEEDED(pSwapChain->GetBuffer(bufferIdx, IID_PPV_ARGS(&bb)))) {
                                D3D12_RESOURCE_BARRIER b = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                                                            D3D12_RESOURCE_BARRIER_FLAG_NONE,
                                                            {bb, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                                             D3D12_RESOURCE_STATE_PRESENT,
                                                             D3D12_RESOURCE_STATE_RENDER_TARGET}};
                                list->ResourceBarrier(1, &b);
                                D3D12_CPU_DESCRIPTOR_HANDLE rtv =
                                    g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
                                rtv.ptr += (SIZE_T)bufferIdx * g_State.rtvDescriptorSize;
                                g_Device->CreateRenderTargetView(bb, nullptr, rtv);
                                list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                                D3D12_VIEWPORT vp = {0, 0, (float)g_State.cachedWidth, (float)g_State.cachedHeight,
                                                     0, 1};
                                list->RSSetViewports(1, &vp);
                                D3D12_RECT scissor = {0, 0, (LONG)g_State.cachedWidth, (LONG)g_State.cachedHeight};
                                list->RSSetScissorRects(1, &scissor);
                                DrawOverlay(list, processCapture, bufferIdx);
                                b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                                b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                                list->ResourceBarrier(1, &b);
                                list->Close();

                                ID3D12CommandList* lists[] = {list};
                                q->ExecuteCommandLists(1, lists);

                                g_State.currentFenceValue++;
                                g_State.fenceValues[idx] = g_State.currentFenceValue;
                                q->Signal(g_State.fence, g_State.currentFenceValue);

                                bb->Release();
                            }
                            sc3->Release();
                        }
                    }
                }
            }
        }
    }
    // DEBUG: Check capture conditions
    static int s_captureCheckCount = 0;
    if (++s_captureCheckCount <= 10) {
        HookLog("DX12: Capture check %d - processCapture=%d, g_IPC=%p, IsRecording=%d", 
                s_captureCheckCount, processCapture, g_IPC, g_IPC ? g_IPC->IsRecording() : -1);
    }
    
    if (processCapture && g_IPC && g_IPC->IsRecording()) {
        SharedMemoryLayout* shm = g_IPC->GetSharedMem();
        if (shm) {
            HookLog("DX12: Capture conditions met, capturing frame...");
            if (!g_SharedCaptureD3D12.IsActive()) g_SharedCaptureD3D12.Initialize(g_Device, pSwapChain);
            if (g_SharedCaptureD3D12.IsActive()) {
                std::lock_guard<std::recursive_mutex> capLock(g_DX12CaptureMutex);
                IDXGISwapChain3* sc3 = nullptr;
                pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3));
                UINT bbIdx = sc3 ? sc3->GetCurrentBackBufferIndex() : 0;
                if (sc3) sc3->Release();
                if (g_SharedCaptureD3D12.CaptureFrame(q, bbIdx)) {
                    HookLog("DX12: CaptureFrame succeeded");
                    SharedFrameDescriptor desc;
                    if (g_SharedCaptureD3D12.GetCurrentFrame(&desc)) {
                        shm->sharedHandles[0] = (uint64_t)g_SharedCaptureD3D12.GetSharedHandle(0);
                        shm->sharedHandles[1] = (uint64_t)g_SharedCaptureD3D12.GetSharedHandle(1);
                        shm->fenceShareHandle = (uint64_t)g_SharedCaptureD3D12.GetFenceShareHandle();
                        shm->width = desc.width;
                        shm->height = desc.height;
                        shm->format = desc.format;
                        uint32_t wIdx = shm->frameRing.writeIndex.load(std::memory_order_relaxed);
                        uint32_t rIdx = shm->frameRing.readIndex.load(std::memory_order_acquire);
                        if ((uint32_t)(wIdx - rIdx) < (uint32_t)FRAME_RING_SIZE) {
                            FrameSlot& slot = shm->frameRing.slots[wIdx % FRAME_RING_SIZE];
                            slot.fenceValue = desc.fenceValue;
                            slot.timestamp = desc.presentTime;
                            slot.frameIndex = desc.frameNumber;
                            slot.textureIndex = desc.textureIndex;
                            slot.sourcePid = GetCurrentProcessId();
                            slot.valid.store(1, std::memory_order_release);
                            shm->frameRing.writeIndex.store(wIdx + 1, std::memory_order_release);
                        } else
                            shm->frameRing.droppedFrames.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
    }
    q->Release();
}

void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain)
{
    // CRITICAL DEBUG: This log MUST appear if the function is called
    static int s_callCount = 0;
    if (++s_callCount <= 10) {
        HookLog("DX12: ProcessFrameExternal ENTERED call #%d", s_callCount);
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
    uint64_t frameNum = ++g_FGDebugFrameCount;
    g_FGCompat.RecordFrame(count);
    HookLog("DX12: ProcessFrameExternal - Frame %llu, cmdLists=%d", frameNum, count);
    ProcessFrame(sc3, count > 0);
    sc3->Release();
}

namespace DXGIShared {
void HandleDX12ProcessFrame(IDXGISwapChain* pSwapChain, bool isRealFrame) { DX12_ProcessFrameExternal(pSwapChain); }
void HandleDX12ResizeBegin() { DX12_OnSwapchainResizeBegin(); }
void HandleDX12ResizeEnd() { DX12_OnSwapchainResizeEnd(); }
}  // namespace DXGIShared

static const GUID SKID_D3D12SwapChainBufferBitmap = {
    0xbc53df3b, 0x956f, 0x47db, {0xa6, 0x53, 0x5, 0xd7, 0xb8, 0x71, 0x53, 0x38}};
void STDMETHODCALLTYPE DetourExecuteCommandLists(ID3D12CommandQueue* pThis, UINT NumCommandLists,
                                                 ID3D12CommandList* const* ppCommandLists)
{
    g_CommandListsExecutedThisFrame.fetch_add(NumCommandLists, std::memory_order_relaxed);

    // OPTIMIZATION: Only run detection if we haven't found the main queue yet
    // We use a relaxed atomic load of the pointer (safe enough for a heuristic)
    // If g_CommandQueue is NULL, we assume we need to search.
    // If g_CommandQueue is SET, we only check if pThis matches if we suspect a switch,
    // but for performance we assume the game uses one main queue for the swapchain.
    bool runDetection = (g_CommandQueue == nullptr);

    if (runDetection && pThis && !g_InSwapchainResizeCleanup.load(std::memory_order_acquire)) {
        IDXGISwapChain3* sc = nullptr;
        {
            // Check g_LastSwapChain with lock
            std::lock_guard<std::recursive_mutex> lock(g_OverlayMutex);
            if (g_LastSwapChain) {
                g_LastSwapChain->QueryInterface(IID_PPV_ARGS(&sc));
            }
        }

        if (sc) {
            UINT idx = sc->GetCurrentBackBufferIndex();
            DXGI_SWAP_CHAIN_DESC d;
            if (SUCCEEDED(sc->GetDesc(&d))) {
                UINT size = sizeof(uint16_t);
                uint16_t bitmap = 0;
                pThis->GetPrivateData(SKID_D3D12SwapChainBufferBitmap, &size, &bitmap);
                bitmap |= (1 << idx);
                pThis->SetPrivateData(SKID_D3D12SwapChainBufferBitmap, sizeof(uint16_t), &bitmap);

                auto CountBits = [](uint16_t n) {
                    int c = 0;
                    while (n > 0) {
                        n &= (n - 1);
                        c++;
                    }
                    return c;
                };
                if (CountBits(bitmap) == (int)d.BufferCount) {
                    bool diff = false;
                    {
                        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                        diff = (g_CommandQueue != pThis);
                    }
                    if (diff) DX12_SetCommandQueue(pThis);
                }
            }
            sc->Release();
        }
    }
    if (oExecuteCommandLists) oExecuteCommandLists(pThis, NumCommandLists, ppCommandLists);
}

void DX12_HookQueueVTable(ID3D12CommandQueue* queue)
{
    if (!queue) return;
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
    if (vtbl[10] != (void*)DetourExecuteCommandLists)
        VTableHook::Create(&vtbl[10], (LPVOID)DetourExecuteCommandLists, (LPVOID*)&oExecuteCommandLists);
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory* pThis, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                                IDXGISwapChain** ppSwapChain)
{
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
            DXGIShared::InstallHooks(sc3);
            sc3->Release();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd,
                                                       const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc, IDXGIOutput* pOut,
                                                       IDXGISwapChain1** ppSC)
{
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
            DXGIShared::InstallHooks(sc3);
            sc3->Release();
        }
    }
    return hr;
}

void STDMETHODCALLTYPE DetourCreateSampler(ID3D12Device* pDevice, const D3D12_SAMPLER_DESC* pDesc,
                                           D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    if (oCreateSampler) oCreateSampler(pDevice, pDesc, DestDescriptor);
}
HRESULT WINAPI DetourSerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* pRootSignature,
                                            D3D_ROOT_SIGNATURE_VERSION Version, ID3DBlob** ppBlob,
                                            ID3DBlob** ppErrorBlob)
{
    if (oSerializeRootSignature) return oSerializeRootSignature(pRootSignature, Version, ppBlob, ppErrorBlob);
    return E_FAIL;
}
HRESULT WINAPI DetourSerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pRootSignature,
                                                     ID3DBlob** ppBlob, ID3DBlob** ppErrorBlob)
{
    if (oSerializeVersionedRootSignature) return oSerializeVersionedRootSignature(pRootSignature, ppBlob, ppErrorBlob);
    return E_FAIL;
}
HRESULT STDMETHODCALLTYPE DetourCreateCommittedResource(ID3D12Device* device,
                                                        const D3D12_HEAP_PROPERTIES* pHeapProperties,
                                                        D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc,
                                                        D3D12_RESOURCE_STATES InitialResourceState,
                                                        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
                                                        REFIID riidResource, void** ppvResource)
{
    if (oCreateCommittedResource)
        return oCreateCommittedResource(device, pHeapProperties, HeapFlags, pDesc, InitialResourceState,
                                        pOptimizedClearValue, riidResource, ppvResource);
    return E_FAIL;
}

void DX12Hook::Shutdown()
{
    CleanupResources();
    ShutdownImGui();
    CleanupOverlay();
    CleanupRTVs();
    {
        std::lock_guard<std::recursive_mutex> lock(g_DeviceQueuesMutex);
        for (auto& pair : g_DeviceQueues)
            if (pair.second) pair.second->Release();
        g_DeviceQueues.clear();
    }
    if (g_CommandQueue) {
        g_CommandQueue->Release();
        g_CommandQueue = nullptr;
    }
    if (g_Device) {
        g_Device->Release();
        g_Device = nullptr;
    }
    if (g_LastSwapChain) {
        g_LastSwapChain->Release();
        g_LastSwapChain = nullptr;
    }
    if (g_SharedCaptureD3D12.IsActive()) g_SharedCaptureD3D12.Reset();
    g_IPCReady = false;
}

void DX12Hook::OnHostDisconnect() { g_IPCReady = false; }
void DX12Hook::TrackResource(IUnknown* res)
{
    if (!res) return;
    std::lock_guard<std::recursive_mutex> lock(resourceMutex);
    res->AddRef();
    trackedResources.push_back(res);
}
void DX12Hook::CleanupResources()
{
    std::lock_guard<std::recursive_mutex> lock(resourceMutex);
    for (auto* res : trackedResources)
        if (res) res->Release();
    trackedResources.clear();
}

// FIXED: Clean up the global hook instance if allocated
DWORD WINAPI UnloadThread(LPVOID lpParam)
{
    Sleep(200);
    if (g_dx12HookInstance) {
        g_dx12HookInstance->Shutdown();
        delete g_dx12HookInstance;
        g_dx12HookInstance = nullptr;
    }
    return 0;
}
