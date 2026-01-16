#include "dx12_hook.h"
#include "hook_common.h"
#include "../../common/frame_timing.h"
#include "lod_helper.h"
#include "../common/capture_base.h"
#include "../common/fps_limiter.h"
#include "../common/overlay.h"
#include "../common/performance_metrics.h"
#include "../common/fg_detection.h"
#include "../common/system_metrics.h"
#include "../common/streamline_compat.h"
#include "../common/swapchain_wrapper.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"
#include "imgui.h"
#include <../wrappers/minhook_shim.h>
#include <atomic>
#include <avrt.h>
#include <cmath>
#include <condition_variable>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <fstream>
#include <intrin.h>
#include <iostream>
#include <mutex>
#include <processthreadsapi.h>
#include <string>
#include <thread>
#include <vector>
#include <map>

#pragma comment(lib, "avrt.lib")

// Check if Vulkan is primary API (to avoid double FPS limiting)
extern void* g_VulkanHook;
static bool IsVulkanPrimary() { return g_VulkanHook != nullptr; }

 static bool IsUnityProcess() {
     static LONG s_init = 0;
     static bool s_isUnity = false;
     if (InterlockedCompareExchange(&s_init, 1, 0) == 0) {
         s_isUnity = (GetModuleHandleA("UnityPlayer.dll") != nullptr);
         InterlockedExchange(&s_init, 2);
     }
     while (s_init < 2) {
         Sleep(0);
     }
     return s_isUnity;
 }

// --- Forward Declarations ---
typedef HRESULT(STDMETHODCALLTYPE *PresentPtr)(IDXGISwapChain3 *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *Present1Ptr)(
    IDXGISwapChain3 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
typedef HRESULT(STDMETHODCALLTYPE *ResizeBuffersPtr)(IDXGISwapChain3 *, UINT,
                                                     UINT, UINT, DXGI_FORMAT,
                                                     UINT);
typedef void(STDMETHODCALLTYPE *ExecuteCommandListsPtr)(
    ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
typedef void(STDMETHODCALLTYPE *CreateSamplerPtr)(ID3D12Device *, const D3D12_SAMPLER_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef HRESULT(STDMETHODCALLTYPE *CreateCommittedResourcePtr)(
    ID3D12Device *, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES,
    const D3D12_CLEAR_VALUE *, REFIID, void **);

// Root Signature Serialization hooks (for Static Sampler override)
typedef HRESULT(WINAPI *PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(
    const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI *PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);

static PresentPtr oPresent = nullptr;
static Present1Ptr oPresent1 = nullptr;
static ResizeBuffersPtr oResizeBuffers = nullptr;
static ExecuteCommandListsPtr oExecuteCommandLists = nullptr;
static CreateSamplerPtr oCreateSampler = nullptr;
static CreateCommittedResourcePtr oCreateCommittedResource = nullptr;
static PFN_D3D12_SERIALIZE_ROOT_SIGNATURE oSerializeRootSignature = nullptr;
static PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE oSerializeVersionedRootSignature = nullptr;

typedef HRESULT(STDMETHODCALLTYPE *GetBufferPtr)(IDXGISwapChain *, UINT, REFIID, void **);
static GetBufferPtr oGetBuffer = nullptr;

extern DX12Hook g_DX12Hook;

ID3D12Device *g_Device = nullptr;
ID3D12CommandQueue *g_CommandQueue = nullptr;
bool g_IPCReady = false;
static IDXGISwapChain3 *g_LastSwapChain = nullptr;
static ID3D12Resource *g_DummyBackBuffer = nullptr;
static std::mutex g_OverlayMutex;
static std::atomic<bool> g_InSwapchainResizeCleanup{false};

static std::atomic<bool> g_FGQueueLocked{false};
static std::mutex        g_FGQueueLockMutex;
static ID3D12CommandQueue* g_FGLockedQueue = nullptr;

static std::atomic<bool> g_IsQueueFromSwapchain{false};
static std::atomic<bool> g_FGNeedsDrain{false};
static std::atomic<uint64_t> g_FGNextDrainAttemptUs{0};

// static bool g_ImGuiInit = false;  // Moved to DX12OverlayState
// static int g_ImGuiInitFrameCounter = 0; // Moved to DX12OverlayState

// FG Real Frame Detection
static std::atomic<int> g_CommandListsExecutedThisFrame{0};
static std::chrono::steady_clock::time_point g_LastResourceCleanup;
static constexpr int INIT_COOLDOWN_MS = 200; // Wait 200ms after cleanup before re-init

// FG Passthrough Mode - when FG is detected at runtime via LoadLibrary, Present hooks bypass
bool g_FGPassthroughMode = false;

// FSR4 swapchain recreation signaling (set by DX11 hook, checked by DX12 Present)
// This avoids cross-thread cleanup calls which cause deadlocks
static std::atomic<bool> g_FSR4SwapchainRecreatedPending{false};

// Swapchain invalidation flag - set BEFORE new swapchain created to prevent overlay crash
// This flag is checked at multiple points in DetourPresent to abort safely
static std::atomic<bool> g_SwapchainInvalid{false};

// Called by DX11 hook BEFORE FSR4SwapchainProvider creates new swapchain
void DX12_InvalidateSwapchain() {
    g_SwapchainInvalid.store(true, std::memory_order_release);
    EarlyLog("DX12: Swapchain marked INVALID - overlay will abort on next Present check");
}

// Called by DX11 hook when FSR4SwapchainProvider creates new swapchain
void DX12_SignalFSR4SwapchainRecreated() {
    g_FSR4SwapchainRecreatedPending.store(true, std::memory_order_release);
    EarlyLog("DX12: FSR4 swapchain recreation signaled (pending cleanup on DX12 thread)");
}

// ============================================================================
// FSR3 FRAME INTERPOLATION SWAPCHAIN DETECTION
// From AMD FidelityFX SDK: FrameInterpolationSwapchainDX12.h line 176
// ============================================================================
// {BEED74B2-282E-4AA3-BBF7-534560507A45}
static const GUID IID_IFfxFrameInterpolationSwapChain = 
    {0xbeed74b2, 0x282e, 0x4aa3, {0xbb, 0xf7, 0x53, 0x45, 0x60, 0x50, 0x7a, 0x45}};

// {5f5fa2f5-3bc5-48d8-a63b-e60318e38000}
static const GUID IID_IFrameInterpolationSwapChainDX12 = 
    {0x5f5fa2f5, 0x3bc5, 0x48d8, {0xa6, 0x3b, 0xe6, 0x03, 0x18, 0xe3, 0x80, 0x00}};

// Track if we're using an FSR3 swapchain
static bool g_UsingFSR3Swapchain = false;

// Check if swapchain is FSR3's FrameInterpolationSwapChainDX12
bool IsFSR3SwapChain(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) return false;
    IUnknown* pTest = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_IFfxFrameInterpolationSwapChain, (void**)&pTest))) {
        pTest->Release();
        return true;
    }
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_IFrameInterpolationSwapChainDX12, (void**)&pTest))) {
        pTest->Release();
        return true;
    }
    return false;
}

// ============================================================================
// COMMAND QUEUE DRAIN - Like SpecialK's _d3d12_rbk->drain_queue()
// Wait for all pending GPU work to complete before releasing resources
// ============================================================================
static ID3D12Fence* g_DrainFence = nullptr;
static HANDLE g_DrainEvent = NULL;
static UINT64 g_DrainFenceValue = 0;

void DrainCommandQueue(ID3D12CommandQueue* queue, ID3D12Device* device) {
    if (!queue || !device) return;
    
    // Create drain fence if needed
    if (!g_DrainFence) {
        HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_DrainFence));
        if (FAILED(hr)) {
            EarlyLog("DX12: Failed to create drain fence: 0x%08X", hr);
            return;
        }
    }
    if (!g_DrainEvent) {
        g_DrainEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!g_DrainEvent) {
            EarlyLog("DX12: Failed to create drain event");
            return;
        }
    }
    
    // Signal and wait
    UINT64 fenceValue = ++g_DrainFenceValue;
    HRESULT hr = queue->Signal(g_DrainFence, fenceValue);
    if (FAILED(hr)) {
        EarlyLog("DX12: DrainCommandQueue Signal failed: 0x%08X", hr);
        return;
    }
    
    if (g_DrainFence->GetCompletedValue() < fenceValue) {
        hr = g_DrainFence->SetEventOnCompletion(fenceValue, g_DrainEvent);
        if (SUCCEEDED(hr)) {
            DWORD result = WaitForSingleObject(g_DrainEvent, 5000); // 5s timeout
            if (result == WAIT_TIMEOUT) {
                EarlyLog("DX12: DrainCommandQueue timeout waiting for GPU");
            }
        }
    }
    
    EarlyLog("DX12: Command queue drained successfully (fence=%llu)", fenceValue);
}

// ============================================================================
// NATIVE INTERFACE STORAGE (for FG overlay compatibility)
// Per SpecialK: When Streamline FG is active, we MUST use native (non-proxy) 
// interfaces for overlay rendering. Streamline wraps device/queue/swapchain
// and using proxies for overlay operations causes crashes.
// ============================================================================
static ID3D12Device* g_NativeDevice = nullptr;
static ID3D12CommandQueue* g_NativeCommandQueue = nullptr;
static IDXGISwapChain3* g_NativeSwapChain = nullptr;
static bool g_UsingNativeInterfaces = false;
static bool g_NativeInterfacesQueried = false;


// Swapchain wrapper for FG overlay support
OverlayDrawCallback g_OverlayDrawCallback = nullptr;
static OverlaySwapChainWrapper* g_SwapChainWrapper = nullptr;
static bool g_UsingSwapChainWrapper = false;

static std::map<ID3D12Device*, ID3D12CommandQueue*> g_DeviceQueues;
static std::mutex g_DeviceQueuesMutex;

// Prerender Limit Fencing
static ID3D12Fence* g_PrerenderFence = nullptr;
static HANDLE g_PrerenderEvent = NULL;
static UINT64 g_PrerenderValue = 0;
static std::vector<UINT64> g_PrerenderHistory;
static uint64_t g_PrerenderFrameIndex = 0;
static int64_t g_LastSleepUs = 0;

// ============================================================================
// FG OVERLAY INFRASTRUCTURE
// Per Streamline SDK: overlays must intercept CreateSwapChainXXX and not make 
// assumptions about swapchain/queue when FG is active. We use dedicated queue.
// ============================================================================

// Swapchain creation tracking for FG stabilization
static std::atomic<int> g_SwapchainCreationCount{0};
static std::chrono::steady_clock::time_point g_LastSwapchainCreation;
static std::atomic<bool> g_FGSwapchainStabilized{false};
static constexpr int FG_STABILIZATION_MS = 500; // Short delay for swapchain to stabilize after FG init

// Dedicated overlay queue (isolated from game/FG queue conflicts)
static ID3D12CommandQueue* g_OverlayQueue = nullptr;
static ID3D12CommandAllocator* g_OverlayAllocators[3] = {nullptr};
static ID3D12GraphicsCommandList* g_OverlayCmdList = nullptr;
static ID3D12Fence* g_OverlayFence = nullptr;
static HANDLE g_OverlayFenceEvent = nullptr;
static UINT64 g_OverlayFenceValue = 0;
static UINT64 g_OverlayFenceValues[3] = {0};
static int g_OverlayAllocIndex = 0;
static bool g_OverlayQueueInitialized = false;

// FG overlay debug counters
static std::atomic<uint64_t> g_FGDebugFrameCount{0};
static std::atomic<uint64_t> g_FGDebugOverlayDraws{0};
static std::atomic<uint64_t> g_FGDebugOverlaySkips{0};
static std::atomic<uint64_t> g_FGDebugResourceFails{0};

// Forward declaration for FG overlay callback - implemented after DX12OverlayState is defined
void FGOverlayDrawCallback(IDXGISwapChain3* pSwapChain, ID3D12CommandQueue* pQueue);

// --- Capture Resources ---
class DX12Capture : public HookCaptureBase {
public:
  ID3D12Resource *sharedTextures[CAPTURE_TEXTURE_COUNT] = {};
  ID3D12Resource *backBufferCache[16] = {nullptr};
  UINT backBufferCount = 0;
  ID3D12Fence *fence = nullptr;
  ID3D12CommandQueue *captureQueue = nullptr;
  ID3D12Fence *gameSyncFence = nullptr;
  HANDLE gameSyncEvent = nullptr;  // CPU-side wait event for FG compatibility
  UINT64 gameSyncValue = 0;
  ID3D12CommandAllocator *cmdAlloc[CAPTURE_TEXTURE_COUNT] = {};
  ID3D12GraphicsCommandList *cmdList = nullptr;

  void CreateSharedResources(uint32_t, uint32_t, uint32_t) override {
    // DX12 creates resources lazily in CheckCaptureInit() when swapchain is available
    // This override intentionally empty - DX12 needs swapchain context for DXGI feature level
  }

  void Cleanup() override {
    StopCaptureThread();
    CleanupSharedHandles();
    
    // Release D3D12 resources
    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
      if (sharedTextures[i])
        sharedTextures[i]->Release();
      sharedTextures[i] = nullptr;
      if (cmdAlloc[i])
        cmdAlloc[i]->Release();
      cmdAlloc[i] = nullptr;
    }
    if (fence)
      fence->Release();
    fence = nullptr;
    if (gameSyncFence)
      gameSyncFence->Release();
    gameSyncFence = nullptr;
    if (gameSyncEvent) {
      CloseHandle(gameSyncEvent);
      gameSyncEvent = nullptr;
    }
    if (cmdList)
      cmdList->Release();
    cmdList = nullptr;
    if (captureQueue)
      captureQueue->Release();
    captureQueue = nullptr;
    
    // Release cached backbuffer references (prevents GPU memory leak on resize)
    for (UINT i = 0; i < backBufferCount; i++) {
      if (backBufferCache[i]) {
        backBufferCache[i]->Release();
        backBufferCache[i] = nullptr;
      }
    }
    backBufferCount = 0;
    
    initialized = false;
  }
};
static DX12Capture g_DX12Capture;
static std::mutex g_DX12CaptureMutex;
static ID3D12Fence *g_DX12CaptureCompletionFence = nullptr;

// --- ImGui & Overlay State ---
struct DX12OverlayState {
  // ImGui Resources
  ID3D12DescriptorHeap *srvDescHeap = nullptr;
  ID3D12DescriptorHeap *rtvDescHeap = nullptr;
  std::vector<ID3D12Resource *> backBuffers;
  UINT rtvDescriptorSize = 0;
  UINT bufferCount = 0; // Number of swapchain buffers (for RTV access bounds check)
  IDXGISwapChain3* cachedSwapChain = nullptr; // Track swapchain for reinit on change (FSR4 swaps it)
  UINT cachedWidth = 0;
  UINT cachedHeight = 0;
  bool imGuiInit = false;
  int imGuiInitFrameCounter = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

  // Overlay Command/Sync Resources
  ID3D12CommandQueue *overlayQueue = nullptr;
  std::vector<ID3D12CommandAllocator *> allocators;
  ID3D12GraphicsCommandList *cmdList = nullptr;
  ID3D12Fence *fence = nullptr;
  ID3D12Fence *overlaySyncFence = nullptr;
  UINT64 overlaySyncValue = 0;
  std::vector<UINT64> fenceValues;
  UINT64 currentFenceValue = 0;
  HANDLE fenceEvent = NULL;
  int allocIndex = 0;  // Rotating index
  static const int ALLOC_POOL_SIZE = 8;
  bool syncInit = false;
};

static DX12OverlayState g_State;
static PerformanceMetrics g_PerfMetrics;

// Forward declaration for DrawOverlay
void DrawOverlay(ID3D12GraphicsCommandList* list);

// Forward declaration for external access
void ProcessFrame(IDXGISwapChain3 *pSwapChain, bool processCapture);

// ============================================================================
// FG OVERLAY CALLBACK - Called from swapchain wrapper's Present BEFORE FG
// This is the key to overlay support with Frame Generation
// ============================================================================
void FGOverlayDrawCallback(IDXGISwapChain3* pSwapChain, ID3D12CommandQueue* pQueue) {
    if (!pSwapChain || !pQueue || !g_Device) return;
    
    static bool firstCall = true;
    if (firstCall) {
        EarlyLog("DX12 FG: Wrapper overlay callback first invocation (SC=%p, Q=%p)", pSwapChain, pQueue);
        firstCall = false;
    }
    
    // Check if overlay resources are initialized
    if (!g_State.imGuiInit || !g_State.syncInit || !g_State.fence) {
        static int initLog = 0;
        if (initLog++ % 500 == 0) {
            HookLog("DX12 FG Wrapper: Waiting for overlay init (imGui=%d, sync=%d)", 
                    g_State.imGuiInit, g_State.syncInit);
        }
        return;
    }
    
    // Get current backbuffer
    int bufferIdx = pSwapChain->GetCurrentBackBufferIndex();
    ID3D12Resource* backBuffer = nullptr;
    HRESULT hr = pSwapChain->GetBuffer(bufferIdx, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer) return;
    
    // Get allocator and command list
    int allocIdx = g_State.allocIndex;
    g_State.allocIndex = (g_State.allocIndex + 1) % DX12OverlayState::ALLOC_POOL_SIZE;
    
    // Wait for previous use of this allocator
    UINT64 completed = g_State.fence->GetCompletedValue();
    UINT64 target = g_State.fenceValues[allocIdx];
    if (completed < target) {
        g_State.fence->SetEventOnCompletion(target, g_State.fenceEvent);
        WaitForSingleObject(g_State.fenceEvent, 50);
    }
    
    auto* alloc = g_State.allocators[allocIdx];
    auto* list = g_State.cmdList;
    if (!alloc || !list) {
        backBuffer->Release();
        return;
    }
    
    alloc->Reset();
    list->Reset(alloc, nullptr);
    
    // Create RTV for this backbuffer
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)bufferIdx * g_State.rtvDescriptorSize;
    g_Device->CreateRenderTargetView(backBuffer, nullptr, rtv);
    
    // Transition to render target - we're BEFORE FG so this is safe
    D3D12_RESOURCE_BARRIER preBarrier = {};
    preBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    preBarrier.Transition.pResource = backBuffer;
    preBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    preBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    preBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &preBarrier);
    
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    
    D3D12_VIEWPORT vp = {0, 0, (float)g_State.cachedWidth, (float)g_State.cachedHeight, 0, 1};
    list->RSSetViewports(1, &vp);
    D3D12_RECT scissor = {0, 0, (LONG)g_State.cachedWidth, (LONG)g_State.cachedHeight};
    list->RSSetScissorRects(1, &scissor);
    
    // Draw overlay
    DrawOverlay(list);
    
    // Transition back to present
    D3D12_RESOURCE_BARRIER postBarrier = {};
    postBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    postBarrier.Transition.pResource = backBuffer;
    postBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    postBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    postBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &postBarrier);
    
    list->Close();
    
    // Execute on the provided queue
    ID3D12CommandList* lists[] = {list};
    pQueue->ExecuteCommandLists(1, lists);
    
    // Signal fence
    g_State.currentFenceValue++;
    g_State.fenceValues[allocIdx] = g_State.currentFenceValue;
    pQueue->Signal(g_State.fence, g_State.currentFenceValue);
    
    // Wait for completion before returning (FG needs finished state)
    g_State.fence->SetEventOnCompletion(g_State.currentFenceValue, g_State.fenceEvent);
    WaitForSingleObject(g_State.fenceEvent, 50);
    
    g_FGDebugOverlayDraws++;
    backBuffer->Release();
}

// --- Helper Functions ---

void ShutdownImGui() {
  if (!g_State.imGuiInit) return;
  HookLog("DX12: Shutting down ImGui...");
  ImGui_ImplDX12_Shutdown();
  g_SharedOverlay.ShutdownImGui();

  // FG: when overlay is torn down, release any locked queue.
  {
    std::lock_guard<std::mutex> lock(g_FGQueueLockMutex);
    if (g_FGLockedQueue) {
      g_FGLockedQueue->Release();
      g_FGLockedQueue = nullptr;
    }
    g_FGQueueLocked.store(false, std::memory_order_relaxed);
  }
  g_FGNeedsDrain.store(true, std::memory_order_relaxed);
  g_FGNextDrainAttemptUs.store(0, std::memory_order_relaxed);
  if (g_State.srvDescHeap) {
    g_State.srvDescHeap->Release();
    g_State.srvDescHeap = nullptr;
  }
  g_State.imGuiInit = false;
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd,
                                                             UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);
void InitImGui(ID3D12Device *device, int buffers, DXGI_FORMAT format,
               HWND hwnd) {
  // Guard against double initialization
  if (g_State.imGuiInit) {
      return;
  }
  
  g_State.format = format;
  g_SharedOverlay.InitImGui(hwnd);
  
  D3D12_DESCRIPTOR_HEAP_DESC desc = {};
  desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  desc.NumDescriptors = 1;
  desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_State.srvDescHeap))))
    return;
    
  ImGui_ImplDX12_Init(device, buffers, format, g_State.srvDescHeap,
                      g_State.srvDescHeap->GetCPUDescriptorHandleForHeapStart(),
                      g_State.srvDescHeap->GetGPUDescriptorHandleForHeapStart());
  
  if (g_CommandQueue) {
    ImGui_ImplDX12_SetCommandQueue(g_CommandQueue);
  }
  
  g_State.imGuiInit = true;
  EarlyLog("DX12: ImGui initialized (Queue=%p)", g_CommandQueue);
}

void DrawOverlay(ID3D12GraphicsCommandList *cmdList) {
  if (!g_State.imGuiInit || !cmdList)
    return;

  ImGui_ImplDX12_NewFrame();
  g_SharedOverlay.BeginFrame();

  // Use shared overlay
  g_SharedOverlay.SetMetrics(&g_PerfMetrics);
  g_SharedOverlay.SetIPCClient(g_IPC);
  g_SharedOverlay.SetDroppedFrames(g_DX12Capture.droppedFrames.load(std::memory_order_relaxed));
  const char* finalApi = "DX12";
  // Check for VKD3D-Proton - specifically check for vkd3d DLLs, not just vulkan-1.dll
  // vulkan-1.dll alone can be loaded by NVIDIA drivers even in native DX12 games
  if (GetModuleHandleA("d3d12core.dll") && (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll"))) {
      finalApi = "DX12 (VKD3D)";
  }
  g_SharedOverlay.SetGraphicsAPI(finalApi);
  // Detect HDR
  bool isHDR = (g_State.format == DXGI_FORMAT_R16G16B16A16_FLOAT || 
               g_State.format == DXGI_FORMAT_R10G10B10A2_UNORM);
  g_SharedOverlay.SetHDR(isHDR);

  g_SharedOverlay.RenderUI();
  g_SharedOverlay.EndFrame();
  
  cmdList->SetDescriptorHeaps(1, &g_State.srvDescHeap);
  ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList); 
}

void CheckCaptureInit(IDXGISwapChain3 *pSwapChain) {
  if (g_DX12Capture.initialized)
    return;
  std::lock_guard<std::mutex> lock(g_DX12CaptureMutex);
  if (g_DX12Capture.initialized)
    return;

  if (FAILED(pSwapChain->GetDevice(IID_PPV_ARGS(&g_Device))))
    return;

  DXGI_SWAP_CHAIN_DESC1 desc;
  pSwapChain->GetDesc1(&desc);
  g_DX12Capture.width = desc.Width;
  g_DX12Capture.height = desc.Height;

  if (!g_DX12Capture.sharedTextures[0]) {
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = desc.Width;
    texDesc.Height = desc.Height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = desc.Format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                    D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    D3D12_HEAP_PROPERTIES heapProps = {D3D12_HEAP_TYPE_DEFAULT,
                                       D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                       D3D12_MEMORY_POOL_UNKNOWN, 1, 1};

    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
      g_Device->CreateCommittedResource(
          &heapProps, D3D12_HEAP_FLAG_SHARED, &texDesc,
          D3D12_RESOURCE_STATE_COMMON, NULL,
          IID_PPV_ARGS(&g_DX12Capture.sharedTextures[i]));
      g_Device->CreateSharedHandle(g_DX12Capture.sharedTextures[i], NULL,
                                   GENERIC_ALL, NULL,
                                   &g_DX12Capture.sharedTextureHandles[i]);
    }
  }

  if (pSwapChain && g_DX12Capture.backBufferCount == 0) {
    DXGI_SWAP_CHAIN_DESC scDesc;
    pSwapChain->GetDesc(&scDesc);
    g_DX12Capture.backBufferCount = scDesc.BufferCount;
    if (g_DX12Capture.backBufferCount > 16)
      g_DX12Capture.backBufferCount = 16;
    for (UINT i = 0; i < g_DX12Capture.backBufferCount; i++) {
      pSwapChain->GetBuffer(i, IID_PPV_ARGS(&g_DX12Capture.backBufferCache[i]));
    }
  }

  if (!g_DX12Capture.captureQueue) {
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    int priority = (g_IPC && g_IPC->GetSharedMem())
                       ? g_IPC->GetSharedMem()->copyQueuePriority
                       : 1;
    if (priority == 0)
      queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    else if (priority == 2)
      queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    else
      queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;

    g_Device->CreateCommandQueue(&queueDesc,
                                 IID_PPV_ARGS(&g_DX12Capture.captureQueue));
  }

  if (!g_DX12Capture.gameSyncFence) {
    g_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                          IID_PPV_ARGS(&g_DX12Capture.gameSyncFence));
    g_DX12Capture.gameSyncEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
  }
  if (!g_DX12Capture.fence) {
    g_Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                          IID_PPV_ARGS(&g_DX12Capture.fence));
    g_Device->CreateSharedHandle(g_DX12Capture.fence, NULL, GENERIC_ALL, NULL,
                                 &g_DX12Capture.sharedFenceHandle);
    g_DX12Capture.fenceValue = 1;
  }
  if (!g_DX12CaptureCompletionFence)
    g_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                          IID_PPV_ARGS(&g_DX12CaptureCompletionFence));

  if (!g_DX12Capture.cmdAlloc[0]) {
    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++)
      g_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       IID_PPV_ARGS(&g_DX12Capture.cmdAlloc[i]));
    g_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                g_DX12Capture.cmdAlloc[0], nullptr,
                                IID_PPV_ARGS(&g_DX12Capture.cmdList));
    g_DX12Capture.cmdList->Close();
  }

  // Use shared method to publish handles to IPC
  g_DX12Capture.width = desc.Width;
  g_DX12Capture.height = desc.Height;
  g_DX12Capture.format = desc.Format;
  g_DX12Capture.PublishToSharedMemory(g_IPC);

  // Initialize SystemMetricsCollector with adapter LUID for GPU stats
  LUID adapterLuid = g_Device->GetAdapterLuid();
  SystemMetricsCollector::Get().Initialize(adapterLuid.LowPart, adapterLuid.HighPart);

  // Set VRAM Total explicitly to prevent background thread crash
  IDXGIFactory4* factory = nullptr;
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&factory))) {
      IDXGIAdapter* adapter = nullptr;
      for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
          DXGI_ADAPTER_DESC desc;
          adapter->GetDesc(&desc);
          if (desc.AdapterLuid.LowPart == adapterLuid.LowPart && desc.AdapterLuid.HighPart == adapterLuid.HighPart) {
              SystemMetricsCollector::Get().SetVRAMTotal(desc.DedicatedVideoMemory);
              adapter->Release();
              break;
          }
          adapter->Release();
      }
      factory->Release();
  }

  g_DX12Capture.initialized = true;
  HookLog("Capture Resources Initialized");
}

void CreateRTVs(ID3D12Device *device, IDXGISwapChain3 *swapChain,
                int bufferCount) {
  if (g_State.rtvDescHeap)
    return; // Cleanup usually handles checks
  D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
  rtvHeapDesc.NumDescriptors = bufferCount;
  rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc,
                                          IID_PPV_ARGS(&g_State.rtvDescHeap))))
    return;
  
  // Store buffer count for bounds checking in overlay draw
  g_State.bufferCount = bufferCount;
  
  // Track which swapchain these RTVs belong to (for FSR4 swapchain change detection)
  g_State.cachedSwapChain = swapChain;

  g_State.rtvDescriptorSize =
      device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
      g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();

  // IMPORTANT (FG / FSR4SwapchainProvider safety): do not hold strong refs to backbuffers.
  // Keeping backbuffer refs can keep the native swapchain refcount elevated during
  // Streamline -> FSR swapchain provider transitions and may destabilize startup.
  g_State.backBuffers.clear();
  for (int i = 0; i < bufferCount; i++) {
    ID3D12Resource* bb = nullptr;
    if (SUCCEEDED(swapChain->GetBuffer(i, IID_PPV_ARGS(&bb))) && bb) {
      device->CreateRenderTargetView(bb, nullptr, rtvHandle);
      bb->Release();
    }
    rtvHandle.ptr += g_State.rtvDescriptorSize;
  }
}

void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain) {
  if (!pSwapChain) return;

  static int64_t qpcFreq = 0;
  if (qpcFreq == 0) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpcFreq = f.QuadPart;
  }

  LARGE_INTEGER qpc;
  QueryPerformanceCounter(&qpc);
  int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;
  g_PerfMetrics.Update(us);
  if (g_IPC) {
    g_PerfMetrics.SetRecording(g_IPC->IsRecording());
  }

  IDXGISwapChain3* sc3 = nullptr;
  if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) {
    return;
  }

  // Record frame stats for FG logic (CRITICAL: This path is used when DX11 hook handles Present)
  int cmdListCount = g_CommandListsExecutedThisFrame.exchange(0);
  if (g_FGDebugFrameCount < 12000) { 
      EarlyLog("DX12: ProcessFrameExternal - RecordFrame cmdListCount=%d", cmdListCount);
  }
  g_FGCompat.RecordFrame(cmdListCount);

  ProcessFrame(sc3, true);
  sc3->Release();
}

// ============================================================================
// DRAIN QUEUE - SpecialK approach for FG compatibility
// IMPORTANT: Do NOT run this every frame; it can stall the render/present thread.
// Instead, only drain on transitions (swapchain recreation / init) and with backoff.
// ============================================================================
static bool WaitFenceWithTimeout(ID3D12Fence* fence, UINT64 value, HANDLE eventHandle, DWORD timeoutMs) {
    if (!fence || !eventHandle) return false;
    if (fence->GetCompletedValue() >= value) return true;
    HRESULT hr = fence->SetEventOnCompletion(value, eventHandle);
    if (FAILED(hr)) return false;
    DWORD result = WaitForSingleObject(eventHandle, timeoutMs);
    return result == WAIT_OBJECT_0;
}

static bool DrainCommandQueue(ID3D12CommandQueue* queue, DWORD timeoutMs) {
    if (!g_State.syncInit || !g_State.fence || !queue) {
        return false;
    }
    
    // Signal a new fence value and wait for it
    // This ensures ALL previously submitted work has completed
    g_State.currentFenceValue++;
    UINT64 drainValue = g_State.currentFenceValue;
    
    HRESULT hr = queue->Signal(g_State.fence, drainValue);
    if (FAILED(hr)) {
        EarlyLog("DX12 FG: DrainCommandQueue Signal failed: 0x%08X", hr);
        return false;
    }

    if (!WaitFenceWithTimeout(g_State.fence, drainValue, g_State.fenceEvent, timeoutMs)) {
        EarlyLog("DX12 FG: DrainCommandQueue timeout waiting for GPU");
        return false;
    }
    
    // Update all allocator fence values to current
    for (int i = 0; i < DX12OverlayState::ALLOC_POOL_SIZE; i++) {
        if (g_State.fenceValues[i] < drainValue) {
            g_State.fenceValues[i] = drainValue;
        }
    }
    
    static bool drainLogOnce = false;
    if (!drainLogOnce) {
        EarlyLog("DX12 FG: DrainCommandQueue completed (fence=%llu)", drainValue);
        drainLogOnce = true;
    }
    
    return true;
}

void InitOverlaySync(ID3D12Device *device, int bufferCount) {
  if (g_State.syncInit)
    return;
    
  HRESULT hr;
  
  // Create fence for tracking allocator usage
  hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_State.fence));
  if (FAILED(hr) || !g_State.fence) {
    HookLog("DX12: Failed to create overlay fence: 0x%08X", hr);
    return;
  }

  // Use a larger allocator pool (8) with rotating index to prevent GPU starvation
  // Increase pool size if FG is active to handle higher frame inflight count
  const int poolSize = g_FGCompat.IsFGActive() ? 16 : DX12OverlayState::ALLOC_POOL_SIZE;
  g_State.allocators.resize(poolSize);
  g_State.fenceValues.resize(poolSize, 0);
  for (int i = 0; i < poolSize; i++) {
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                   IID_PPV_ARGS(&g_State.allocators[i]));
    if (FAILED(hr)) {
      HookLog("DX12: Failed to create overlay allocator %d: 0x%08X", i, hr);
      // Cleanup already created allocators
      for (int j = 0; j < i; j++) {
        if (g_State.allocators[j]) g_State.allocators[j]->Release();
      }
      g_State.allocators.clear();
      g_State.fence->Release();
      g_State.fence = nullptr;
      return;
    }
  }

  hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                            g_State.allocators[0], nullptr,
                            IID_PPV_ARGS(&g_State.cmdList));
  if (FAILED(hr) || !g_State.cmdList) {
    HookLog("DX12: Failed to create overlay command list: 0x%08X", hr);
    for (auto alloc : g_State.allocators) {
      if (alloc) alloc->Release();
    }
    g_State.allocators.clear();
    g_State.fence->Release();
    g_State.fence = nullptr;
    return;
  }
  g_State.cmdList->Close();

  g_State.fenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
  g_State.syncInit = true;
  HookLog("DX12: Overlay sync initialized");
}

// ============================================================================
// FG-SAFE DEDICATED OVERLAY QUEUE
// Per Streamline SDK guidance, we use a completely separate command queue
// for overlay rendering to avoid conflicts with FG runtime's queue management.
// ============================================================================

bool InitDedicatedOverlayQueue(ID3D12Device* device) {
    if (g_OverlayQueueInitialized) return true;
    if (!device) {
        EarlyLog("DX12 FG: InitDedicatedOverlayQueue - device is NULL!");
        return false;
    }
    
    EarlyLog("DX12 FG: Initializing dedicated overlay queue (device=%p)...", device);
    
    // Per Streamline SDK: Third party libraries SHOULD NOT use SL proxies.
    // Try to get native device using Streamline's special GUID.
    // GUID: {ADEC44E2-61F0-45C3-AD9F-1B37379284FF}
    ID3D12Device* nativeDevice = nullptr;
    IID slNativeGuid;
    if (SUCCEEDED(IIDFromString(L"{ADEC44E2-61F0-45C3-AD9F-1B37379284FF}", &slNativeGuid))) {
        HRESULT hrQuery = device->QueryInterface(slNativeGuid, (void**)&nativeDevice);
        if (SUCCEEDED(hrQuery) && nativeDevice) {
            EarlyLog("DX12 FG: Got native device from SL proxy: %p -> %p", device, nativeDevice);
            device = nativeDevice; // Use native device instead
        } else {
            EarlyLog("DX12 FG: No SL proxy detected (hr=0x%08X), using device as-is", hrQuery);
            // Not a SL proxy, use original device
        }
    }
    
    // Validate device is usable - simple check without SEH (clang doesn't support __try)
    LUID luid = device->GetAdapterLuid();
    EarlyLog("DX12 FG: Device LUID check passed: %08X-%08X", luid.HighPart, luid.LowPart);
    
    // Create dedicated command queue for overlay
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;
    
    EarlyLog("DX12 FG: Calling CreateCommandQueue...");
    HRESULT hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_OverlayQueue));
    if (FAILED(hr)) {
        EarlyLog("DX12 FG: Failed to create overlay queue: 0x%08X", hr);
        return false;
    }
    EarlyLog("DX12 FG: Created dedicated overlay queue: %p", g_OverlayQueue);
    
    // Create command allocators (one per buffer in flight)
    for (int i = 0; i < 3; i++) {
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, 
                                            IID_PPV_ARGS(&g_OverlayAllocators[i]));
        if (FAILED(hr)) {
            EarlyLog("DX12 FG: Failed to create overlay allocator %d: 0x%08X", i, hr);
            return false;
        }
    }
    
    // Create command list
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, 
                                   g_OverlayAllocators[0], nullptr, 
                                   IID_PPV_ARGS(&g_OverlayCmdList));
    if (FAILED(hr)) {
        EarlyLog("DX12 FG: Failed to create overlay command list: 0x%08X", hr);
        return false;
    }
    g_OverlayCmdList->Close(); // Start in closed state
    
    // Create fence for synchronization
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_OverlayFence));
    if (FAILED(hr)) {
        EarlyLog("DX12 FG: Failed to create overlay fence: 0x%08X", hr);
        return false;
    }
    
    g_OverlayFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!g_OverlayFenceEvent) {
        EarlyLog("DX12 FG: Failed to create overlay fence event");
        return false;
    }
    
    g_OverlayQueueInitialized = true;
    EarlyLog("DX12 FG: Dedicated overlay queue initialized successfully");
    return true;
}

void CleanupDedicatedOverlayQueue() {
    if (!g_OverlayQueueInitialized) return;
    
    EarlyLog("DX12 FG: Cleaning up dedicated overlay queue...");
    
    // Wait for GPU to complete
    if (g_OverlayQueue && g_OverlayFence) {
        UINT64 waitValue = g_OverlayFenceValue + 1;
        g_OverlayQueue->Signal(g_OverlayFence, waitValue);
        if (g_OverlayFence->GetCompletedValue() < waitValue) {
            g_OverlayFence->SetEventOnCompletion(waitValue, g_OverlayFenceEvent);
            WaitForSingleObject(g_OverlayFenceEvent, 100);
        }
    }
    
    if (g_OverlayFenceEvent) { CloseHandle(g_OverlayFenceEvent); g_OverlayFenceEvent = nullptr; }
    if (g_OverlayFence) { g_OverlayFence->Release(); g_OverlayFence = nullptr; }
    if (g_OverlayCmdList) { g_OverlayCmdList->Release(); g_OverlayCmdList = nullptr; }
    for (int i = 0; i < 3; i++) {
        if (g_OverlayAllocators[i]) { g_OverlayAllocators[i]->Release(); g_OverlayAllocators[i] = nullptr; }
    }
    if (g_OverlayQueue) { g_OverlayQueue->Release(); g_OverlayQueue = nullptr; }
    
    g_OverlayQueueInitialized = false;
    g_OverlayFenceValue = 0;
    g_OverlayAllocIndex = 0;
    EarlyLog("DX12 FG: Dedicated overlay queue cleaned up");
}

// Forward declaration (defined after CleanupRTVs)
void CleanupNativeInterfaces();

void CleanupOverlay() {
  if (!g_State.syncInit) return;
  
  // Wait for GPU to finish any pending overlay work
  if (g_State.fence && g_CommandQueue) {
    HookLog("DX12: CleanupOverlay: Waiting for GPU...");
    UINT64 waitValue = g_State.currentFenceValue + 1;
    HRESULT hr_sig = g_CommandQueue->Signal(g_State.fence, waitValue);
    if (FAILED(hr_sig)) {
        HookLog("DX12: CleanupOverlay: Signal failed: 0x%08X", hr_sig);
    } else {
        if (g_State.fence->GetCompletedValue() < waitValue) {
          g_State.fence->SetEventOnCompletion(waitValue, g_State.fenceEvent);
          WaitForSingleObject(g_State.fenceEvent, 100);  // 100ms max
        }
    }
  }
  HookLog("DX12: CleanupOverlay: Releasing resources...");
  
  // Release event handle
  if (g_State.fenceEvent) {
    CloseHandle(g_State.fenceEvent);
    g_State.fenceEvent = NULL;
  }
  
  // Release allocators
  for (auto alloc : g_State.allocators) {
    if (alloc) alloc->Release();
  }
  g_State.allocators.clear();
  g_State.fenceValues.clear();
  
  // Release command list
  if (g_State.cmdList) {
    g_State.cmdList->Release();
    g_State.cmdList = nullptr;
  }
  
  // Release fences
  if (g_State.fence) {
    g_State.fence->Release();
    g_State.fence = nullptr;
  }
  if (g_State.overlaySyncFence) {
    g_State.overlaySyncFence->Release();
    g_State.overlaySyncFence = nullptr;
  }
  
  // Release command queue
  if (g_State.overlayQueue) {
    g_State.overlayQueue->Release();
    g_State.overlayQueue = nullptr;
  }
  
  g_State.currentFenceValue = 0;
  g_State.allocIndex = 0;
  g_State.syncInit = false;
  
  // CRITICAL: Properly shutdown ImGui before clearing flag
  // This calls ImGui_ImplDX12_Shutdown() to reset ImGui's internal backend state
  // Without this, reinit fails with "Already initialized" assertion
  ShutdownImGui();
  
  // FG: Cleanup native interfaces when overlay is cleaned up
  CleanupNativeInterfaces();
  
  g_State.imGuiInitFrameCounter = 0;
  HookLog("DX12: CleanupOverlay complete");
}

void CleanupRTVs() {
  for (auto *r : g_State.backBuffers)
    if (r)
      r->Release();
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
void DX12_OnSwapchainResizeBegin() {
  bool expected = false;
  if (!g_InSwapchainResizeCleanup.compare_exchange_strong(expected, true)) {
    return; // already cleaning up
  }

  g_LastSwapchainCreation = std::chrono::steady_clock::now();
  g_FGSwapchainStabilized = false;

  std::unique_lock<std::mutex> lock(g_OverlayMutex, std::defer_lock);
  for (int i = 0; i < 200; i++) {
    if (lock.try_lock()) break;
    Sleep(1);
  }
  if (!lock.owns_lock()) {
    g_InSwapchainResizeCleanup.store(false);
    return;
  }

  // CRITICAL: Drain command queue BEFORE releasing any resources
  // This ensures all pending GPU work is complete (like SpecialK's drain_queue)
  EarlyLog("DX12: DX12_OnSwapchainResizeBegin - draining command queue before cleanup");
  if (g_CommandQueue && g_Device) {
      DrainCommandQueue(g_CommandQueue, g_Device);
  }

  if (g_DummyBackBuffer) {
    g_DummyBackBuffer->Release();
    g_DummyBackBuffer = nullptr;
  }

  g_FGCompat.OnSwapchainRecreation();

  // FG: swapchain recreation means any previously selected queue may be invalid
  // and we may need a one-time drain again after resources are rebuilt.
  {
    std::lock_guard<std::mutex> lock(g_FGQueueLockMutex);
    if (g_FGLockedQueue) {
      g_FGLockedQueue->Release();
      g_FGLockedQueue = nullptr;
    }
    g_FGQueueLocked.store(false, std::memory_order_relaxed);
  }
  g_FGNeedsDrain.store(true, std::memory_order_relaxed);
  g_FGNextDrainAttemptUs.store(0, std::memory_order_relaxed);
  CleanupOverlay();
  CleanupRTVs();

  {
    std::lock_guard<std::mutex> capLock(g_DX12CaptureMutex);
    g_DX12Capture.Cleanup();
  }

  if (g_State.imGuiInit) {
    ImGui_ImplDX12_InvalidateDeviceObjects();
  }

  g_InSwapchainResizeCleanup.store(false);
}

// ============================================================================
// NATIVE INTERFACE QUERY (for FG overlay compatibility)
// Per SpecialK: When Streamline FG is active, we must use native (non-proxy)
// interfaces for overlay rendering. Query using the Streamline GUID.
// ============================================================================
bool QueryNativeInterfaces(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain3* swapchain) {
    if (g_NativeInterfacesQueried) return g_UsingNativeInterfaces;
    g_NativeInterfacesQueried = true;
    
    EarlyLog("DX12 FG: Querying native interfaces from Streamline proxies...");
    
    bool isProxy = false;
    
    // Query native device
    if (device) {
        ID3D12Device* nativeDevice = GetNativeDevice(device);
        if (nativeDevice && nativeDevice != device) {
            EarlyLog("DX12 FG: Got native device: proxy=%p -> native=%p", device, nativeDevice);
            g_NativeDevice = nativeDevice;
            isProxy = true;
        } else {
            EarlyLog("DX12 FG: Device is not a Streamline proxy");
            if (nativeDevice) nativeDevice->Release();
        }
    }
    
    // Query native command queue
    if (queue) {
        ID3D12CommandQueue* nativeQueue = GetNativeCommandQueue(queue);
        if (nativeQueue && nativeQueue != queue) {
            EarlyLog("DX12 FG: Got native queue: proxy=%p -> native=%p", queue, nativeQueue);
            g_NativeCommandQueue = nativeQueue;
            isProxy = true;
        } else {
            EarlyLog("DX12 FG: Queue is not a Streamline proxy");
            if (nativeQueue) nativeQueue->Release();
        }
    }
    
    // Query native swapchain
    if (swapchain) {
        IDXGISwapChain3* nativeSwapchain = GetNativeSwapChain3(swapchain);
        if (nativeSwapchain && nativeSwapchain != swapchain) {
            EarlyLog("DX12 FG: Got native swapchain: proxy=%p -> native=%p", swapchain, nativeSwapchain);
            g_NativeSwapChain = nativeSwapchain;
            isProxy = true;
        } else {
            EarlyLog("DX12 FG: Swapchain is not a Streamline proxy");
            if (nativeSwapchain) nativeSwapchain->Release();
        }
    }
    
    g_UsingNativeInterfaces = isProxy;
    EarlyLog("DX12 FG: Native interface query complete. Using native interfaces: %s", 
             isProxy ? "YES" : "NO");
    return isProxy;
}

void CleanupNativeInterfaces() {
    if (g_NativeDevice) { g_NativeDevice->Release(); g_NativeDevice = nullptr; }
    if (g_NativeCommandQueue) { g_NativeCommandQueue->Release(); g_NativeCommandQueue = nullptr; }
    if (g_NativeSwapChain) { g_NativeSwapChain->Release(); g_NativeSwapChain = nullptr; }
    g_UsingNativeInterfaces = false;
    g_NativeInterfacesQueried = false;
    EarlyLog("DX12 FG: Native interfaces cleaned up");
}

void AsyncCaptureThreadProc() {
  HookLog("AsyncCaptureThread Started");
  g_DX12Capture.captureThreadRunning = true;
  static uint64_t captureFrameCount = 0;
  static uint64_t lastLogFrame = 0;
  
  while (!g_DX12Capture.captureThreadShutdown) {
    // Wait for frame signal with timeout to allow checking shutdown flag
    DWORD waitResult = WaitForSingleObject(g_DX12Capture.captureEvent, 100);
    if (waitResult == WAIT_TIMEOUT)
      continue;
    if (waitResult != WAIT_OBJECT_0)
      continue;

    // Use loop to drain queue if multiple frames updated
    while (true) {
      uint32_t wIdx = g_DX12Capture.pendingWriteIdx.load(std::memory_order_acquire);
      uint32_t rIdx = g_DX12Capture.pendingReadIdx.load(std::memory_order_acquire);
      if (rIdx >= wIdx)
        break;

      PendingCaptureFrame &frame =
          g_DX12Capture.pendingRing[rIdx % CAPTURE_RING_SIZE];

      // Snapshot data under mutex, then release before GPU work
      ID3D12CommandQueue *activeQueue = nullptr;
      ID3D12Resource *pBackBuffer = nullptr;
      ID3D12Resource *writeTexture = nullptr;
      ID3D12CommandAllocator *cmdAlloc = nullptr;
      ID3D12GraphicsCommandList *cmdList = nullptr;
      ID3D12Fence *captureFence = nullptr;
      int writeIdx = 0;
      UINT64 fenceVal = 0;
      int64_t timestampQPC = frame.timestampQPC;
      UINT64 frameFenceValue = frame.fenceValue;
      UINT64 completionFenceValue = frame.completionFenceValue;
      
      {
        std::lock_guard<std::mutex> lock(g_DX12CaptureMutex);
        if (!g_DX12Capture.initialized) {
          g_DX12Capture.pendingReadIdx.store(rIdx + 1, std::memory_order_release);
          continue;
        }
        
        activeQueue = g_DX12Capture.captureQueue;
        if (!activeQueue) activeQueue = g_CommandQueue;
        
        if (frame.backBufferIndex < g_DX12Capture.backBufferCount)
          pBackBuffer = g_DX12Capture.backBufferCache[frame.backBufferIndex];
        
        writeIdx = g_DX12Capture.writeIndex;
        writeTexture = g_DX12Capture.sharedTextures[writeIdx];
        cmdAlloc = g_DX12Capture.cmdAlloc[writeIdx];
        cmdList = g_DX12Capture.cmdList;
        captureFence = g_DX12Capture.fence;
        
        // Update writeIndex while holding lock
        g_DX12Capture.writeIndex = (writeIdx + 1) % CAPTURE_TEXTURE_COUNT;
        g_DX12Capture.fenceValue++;
        fenceVal = g_DX12Capture.fenceValue;
      }
      // Mutex released - now do GPU work without holding it
      
      captureFrameCount++;
      
      if (!activeQueue || !pBackBuffer || !writeTexture || !cmdAlloc || !cmdList) {
        if (captureFrameCount % 100 == 1) {
          HookLog("AsyncCapture[%llu]: Missing resources", captureFrameCount);
        }
        g_DX12Capture.pendingReadIdx.store(rIdx + 1, std::memory_order_release);
        continue;
      }

      // GPU-side wait for game to finish rendering before our copy commands
      // This is critical - ensures backbuffer is in correct state when we copy
      if (activeQueue && g_DX12Capture.gameSyncFence && frameFenceValue > 0) {
        activeQueue->Wait(g_DX12Capture.gameSyncFence, frameFenceValue);
      }

      // Build and execute command list - OUTSIDE mutex
      cmdAlloc->Reset();
      cmdList->Reset(cmdAlloc, nullptr);

      // Barriers for backbuffer and shared texture
      // Use COMMON state - backbuffer decays to COMMON after Present
      D3D12_RESOURCE_BARRIER barriers[2] = {};
      
      barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barriers[0].Transition.pResource = pBackBuffer;
      barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
      barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      
      barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barriers[1].Transition.pResource = writeTexture;
      barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
      barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
      barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

      cmdList->ResourceBarrier(2, barriers);
      cmdList->CopyResource(writeTexture, pBackBuffer);

      // Transition back to COMMON
      std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
      std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
      cmdList->ResourceBarrier(2, barriers);
      
      HRESULT closeHr = cmdList->Close();
      if (FAILED(closeHr)) {
        HookLog("AsyncCapture[%llu]: Close failed: 0x%08X", captureFrameCount, closeHr);
        g_DX12Capture.pendingReadIdx.store(rIdx + 1, std::memory_order_release);
        continue;
      }

      if (captureFrameCount - lastLogFrame >= 100) {
        HookLog("AsyncCapture: Processed %llu frames", captureFrameCount);
        lastLogFrame = captureFrameCount;
      }

      ID3D12CommandList *lists[] = {cmdList};
      activeQueue->ExecuteCommandLists(1, lists);
      activeQueue->Signal(captureFence, fenceVal);

      if (g_DX12CaptureCompletionFence && completionFenceValue > 0) {
        activeQueue->Signal(g_DX12CaptureCompletionFence, completionFenceValue);
      }

      g_DX12Capture.SignalFrameReady(g_IPC, writeIdx, timestampQPC, fenceVal);
      g_DX12Capture.pendingReadIdx.store(rIdx + 1, std::memory_order_release);
    }
  }
  g_DX12Capture.captureThreadRunning = false;
  HookLog("AsyncCaptureThread Exiting");
}

void ProcessFrame(IDXGISwapChain3 *pSwapChain, bool processCapture) {
  // LOCK HIERARCHY: g_OverlayMutex -> g_DX12CaptureMutex
  // This function acquires g_OverlayMutex first, then g_DX12CaptureMutex (line 639).
  // All code paths MUST follow this order to prevent deadlocks.
  // Never acquire g_DX12CaptureMutex before g_OverlayMutex elsewhere in the codebase.
  std::lock_guard<std::mutex> lock(g_OverlayMutex);

  // FG STATE: Determine FG active and stabilization status for overlay rendering
  bool fgActive = (g_FGCompat.DetectLoadedFGRuntime() != FGCompatibility::FGType::None);
  int64_t fgSinceSwapchainMs = -1;
  if (fgActive && !g_FGSwapchainStabilized.load()) {
      if (g_LastSwapchainCreation.time_since_epoch().count() == 0) {
          g_LastSwapchainCreation = std::chrono::steady_clock::now();
      }
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - g_LastSwapchainCreation).count();
      fgSinceSwapchainMs = elapsed;
      if (elapsed >= FG_STABILIZATION_MS) {
          g_FGSwapchainStabilized = true;

          static bool s_loggedStabilized = false;
          if (!s_loggedStabilized) {
              HookLog("DX12 FG: Swapchain stabilized after %lld ms - overlay may draw now", (long long)elapsed);
              s_loggedStabilized = true;
          }
      }
  }
  if (fgActive && fgSinceSwapchainMs < 0 && g_LastSwapchainCreation.time_since_epoch().count() != 0) {
      fgSinceSwapchainMs = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - g_LastSwapchainCreation).count();
  }
  bool fgStabilized = fgActive && g_FGSwapchainStabilized.load();
  bool fgSuspended = g_FGCompat.IsSuspended();

  // Talos/UE may resize multiple times during startup with Streamline swapchain provider.
  // Waiting for full stabilization can mean the overlay never appears. Use a shorter
  // ready delay so we can draw once things are minimally settled.
  const bool fgReadyForOverlay = (!fgActive) || (fgSinceSwapchainMs >= 100);
  
  // FG SAFETY: When FG active + suspended, skip ALL operations entirely
  // No device access during suspension to prevent GPU crashes during FG toggle
  if (fgActive && fgSuspended) {
      static int suspendSkipLog = 0;
      if (suspendSkipLog++ % 300 == 0) {
          EarlyLog("DX12 FG: ProcessFrame fully skipped (fgSuspended)");
      }
      return;
  }
  
  // FG SAFETY: When FG active + ImGui not initialized (but not suspended), 
  // only do minimal device operations for overlay init
  if (fgActive && !g_State.imGuiInit) {
      // Minimal initialization path for ImGui only
      ID3D12Device* activeDevice = nullptr;
      if (pSwapChain && SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&activeDevice)))) {
          if (!g_Device) {
              g_Device = activeDevice;
              g_Device->AddRef();
              // NOTE: g_LastSwapChain is treated as a weak pointer (no AddRef/Release)
              // to avoid interfering with FG swapchain provider transitions.
              g_LastSwapChain = pSwapChain;
          }
          activeDevice->Release();
          
          // Try to initialize overlay resources if not done
          if (g_Device && !fgSuspended) {
              DXGI_SWAP_CHAIN_DESC desc;
              if (SUCCEEDED(pSwapChain->GetDesc(&desc))) {
                  g_State.cachedWidth = desc.BufferDesc.Width;
                  g_State.cachedHeight = desc.BufferDesc.Height;
                  
                  static int initAttemptLog = 0;
                  if (initAttemptLog++ % 100 == 0) {
                      EarlyLog("DX12 FG: Attempting overlay init (imGui=%d, rtv=%p, sync=%d)", 
                               g_State.imGuiInit ? 1 : 0, g_State.rtvDescHeap, g_State.syncInit ? 1 : 0);
                  }
                  
                  // Initialize all overlay resources needed for FG overlay
                  if (!g_State.imGuiInit) {
                      InitImGui(g_Device, DX12OverlayState::ALLOC_POOL_SIZE, desc.BufferDesc.Format, desc.OutputWindow);
                  }
                  if (!g_State.rtvDescHeap) {
                      IDXGISwapChain3* sc3 = nullptr;
                      if (SUCCEEDED(pSwapChain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3))) {
                          CreateRTVs(g_Device, sc3, desc.BufferCount);
                          sc3->Release();
                      }
                  }
                  if (!g_State.syncInit) {
                      InitOverlaySync(g_Device, desc.BufferCount);
                  }
              }
          }
      }
      // Skip ALL other operations when FG active + not ready
      return;
  }

  // 1. Determine active device and detect change (FSR-FG Proxying)
  ID3D12Device* activeDevice = nullptr;
  if (!pSwapChain || FAILED(pSwapChain->GetDevice(IID_PPV_ARGS(&activeDevice)))) {
      return;
  }
  
  // Distinguish between initial setup and device change
  bool isInitialSetup = (g_Device == nullptr);
  bool deviceChanged = (!isInitialSetup && (activeDevice != g_Device || pSwapChain != g_LastSwapChain));
  
  if (deviceChanged) {
      // Device/swapchain CHANGE - cleanup old resources and exit
      HookLog("DX12: Device or Swapchain change detected (Device: %p -> %p, SC: %p -> %p). Resetting resources...", 
              g_Device, activeDevice, g_LastSwapChain, pSwapChain);
      g_FGCompat.OnDeviceChange(); // Notify FG detection
      CleanupOverlay();
      CleanupRTVs();
      ShutdownImGui();
      {
          // Acquire g_DX12CaptureMutex AFTER g_OverlayMutex (respects lock hierarchy)
          std::lock_guard<std::mutex> capLock(g_DX12CaptureMutex);
          g_DX12Capture.Cleanup();
      }
      g_LastResourceCleanup = std::chrono::steady_clock::now(); // Mark cleanup time
      
      // Update global device with ref counting
      if (g_Device) g_Device->Release();
      g_Device = activeDevice;
      g_Device->AddRef(); 

      g_LastSwapChain = pSwapChain;

      g_State.imGuiInit = false;
      g_State.imGuiInitFrameCounter = 0;
      
      // Release the local ref from GetDevice
      activeDevice->Release();
      HookLog("DX12: ProcessFrame: Device/SC Updated. g_Device=%p, g_LastSwapChain=%p (Refs Added)", g_Device, g_LastSwapChain);
      
      // ENSURE EARLY EXIT: Resources (Command List, Allocators) have been released.
      return;
  }
  
  if (isInitialSetup) {
      // INITIAL SETUP - capture device/swapchain and CONTINUE (no cleanup needed, no early exit)
      EarlyLog("DX12: Initial device capture (Device: %p, SC: %p)", activeDevice, pSwapChain);
      g_Device = activeDevice;
      g_Device->AddRef();
      g_LastSwapChain = pSwapChain;
  }
  activeDevice->Release();

  // Initialize overlay as early as possible (before queue check)
  // CRITICAL: Do NOT initialize ImGui if FG is active/suspended (e.g. at startup)
  // Note: fgActive and fgSuspended are defined at start of ProcessFrame
  if (!g_State.imGuiInit && !fgActive && !fgSuspended && g_Device) {
      // Check if another renderer backend (e.g. Vulkan) already initialized ImGui
      ImGuiContext* existingCtx = ImGui::GetCurrentContext();
      if (existingCtx) {
          ImGuiIO& io = ImGui::GetIO();
          if (io.BackendRendererUserData != nullptr) {
              EarlyLog("DX12: Skipping ImGui init - another backend already active");
              g_State.imGuiInit = true;  // Mark as handled to avoid repeated checks
              // Continue normal processing; just don't initialize DX12 ImGui backend.
          }
      }

      {
          DXGI_SWAP_CHAIN_DESC desc;
          pSwapChain->GetDesc(&desc);
          g_State.cachedWidth = desc.BufferDesc.Width;
          g_State.cachedHeight = desc.BufferDesc.Height;
          
          EarlyLog("DX12: Initializing ImGui (device=%p, size=%ux%u)", g_Device, desc.BufferDesc.Width, desc.BufferDesc.Height);
          InitImGui(g_Device, DX12OverlayState::ALLOC_POOL_SIZE, desc.BufferDesc.Format,
                    desc.OutputWindow);
          CreateRTVs(g_Device, pSwapChain, desc.BufferCount);
          InitOverlaySync(g_Device, desc.BufferCount);
          g_State.imGuiInit = true;
          EarlyLog("DX12: ImGui initialized successfully");
      }
  }

  // 2. Get the correct queue for this active device
  ID3D12CommandQueue* targetQueue = nullptr;
  {
      std::lock_guard<std::mutex> devLock(g_DeviceQueuesMutex);
      if (g_DeviceQueues.count(g_Device)) {
          targetQueue = g_DeviceQueues[g_Device];
      }
  }
  
  // Fallback: use g_CommandQueue directly if device-mapped queue not found
  // This handles the case where ExecuteCommandLists captured a different device pointer
  if (!targetQueue && g_CommandQueue) {
      targetQueue = g_CommandQueue;
  }
  
  if (!targetQueue) {
      // If we haven't intercepted any queue for this device yet, we can't draw the overlay.
      return;
  }
  
  // Sync the global queue and ImGui backend with the correct target
  g_CommandQueue = targetQueue;
  if (g_State.imGuiInit) {
      ImGui_ImplDX12_SetCommandQueue(g_CommandQueue);
  }

  if (g_IPC && !g_IPCReady) {
    if (g_IPC->Connect())
      g_IPCReady = true;
  }

  if (g_IPC && g_IPCReady) {
    if (!g_DX12Capture.initialized) {
        // Rate limiting for re-initialization
        auto elapsed = std::chrono::steady_clock::now() - g_LastResourceCleanup;
        if (elapsed > std::chrono::milliseconds(INIT_COOLDOWN_MS)) {
            CheckCaptureInit(pSwapChain);
        }
    }

    SharedMemoryLayout* shm = g_IPC->GetSharedMem();
    bool captureIncludeOverlay = shm ? shm->overlayConfig.captureIncludeOverlay : true;
    bool shouldDrawOverlay = shm && shm->overlayConfig.showOverlay;

    static int s_overlayGateLog = 0;
    if (s_overlayGateLog++ % 300 == 0) {
        HookLog("DX12 FG: Gate state show=%d include=%d imGui=%d rtv=%p active=%d stabilized=%d ready=%d suspended=%d msSinceSC=%lld", 
                shouldDrawOverlay ? 1 : 0,
                captureIncludeOverlay ? 1 : 0,
                g_State.imGuiInit ? 1 : 0,
                g_State.rtvDescHeap,
                fgActive ? 1 : 0,
                fgStabilized ? 1 : 0,
                fgReadyForOverlay ? 1 : 0,
                fgSuspended ? 1 : 0,
                (long long)fgSinceSwapchainMs);
    }
    bool overlayDrawn = false;

    // Lambda for overlay drawing - FG-SAFE implementation using NATIVE INTERFACES
    // Per SpecialK research: When Streamline FG is active, we MUST use native interfaces
    // (not proxies) for overlay rendering. Query native interfaces once when FG stabilizes.
    auto doOverlay = [&]() {
      // FG SAFETY: Only draw if stabilized and not currently suspended
      if (fgSuspended || (fgActive && !fgReadyForOverlay)) {
          static int fgSuspendLog = 0;
          if (fgSuspendLog++ % 120 == 0) {
              HookLog("DX12 FG: Skipping overlay (active=%d, stabilized=%d, ready=%d, suspended=%d)", 
                      fgActive ? 1 : 0, fgStabilized ? 1 : 0, fgReadyForOverlay ? 1 : 0, fgSuspended ? 1 : 0);
          }
          g_FGDebugOverlaySkips++;
          return;
      }
      
      // Check prerequisites
      if (!shouldDrawOverlay || !g_Device) return;
      
      // Determine which interfaces to use for overlay
      // When FG active: use game's command queue (not separate overlay queue)
      // This matches SpecialK's approach to avoid queue conflicts with DLSS FG
      IDXGISwapChain3* overlaySwapChain = pSwapChain;
      ID3D12CommandQueue* overlayQueue = g_CommandQueue;
      ID3D12Device* overlayDevice = g_Device;

      // FG MODE: lock to a stable queue once we start drawing, to prevent queue flapping
      // (Talos uses multiple DIRECT queues; picking the wrong one can stall fence waits)
      if (fgActive) {
          if (g_FGQueueLocked.load(std::memory_order_relaxed)) {
              overlayQueue = g_FGLockedQueue;
          } else if (overlayQueue) {
              std::lock_guard<std::mutex> lock(g_FGQueueLockMutex);
              if (!g_FGQueueLocked.load(std::memory_order_relaxed) && overlayQueue) {
                  g_FGLockedQueue = overlayQueue;
                  g_FGLockedQueue->AddRef();
                  g_FGQueueLocked.store(true, std::memory_order_relaxed);
                  EarlyLog("DX12 FG: Locked overlay command queue to %p", g_FGLockedQueue);
              }
          }
      }
      
      // FG MODE: Query native interfaces once when FG is active
      // Per SpecialK research: overlay must use native (non-proxy) interfaces
      // to avoid crashes when Streamline FG is processing frames
      if (fgActive && fgReadyForOverlay && !g_NativeInterfacesQueried) {
          QueryNativeInterfaces(g_Device, g_CommandQueue, pSwapChain);
          static bool fgNativeLog = false;
          if (!fgNativeLog) {
              EarlyLog("DX12 FG: Native interface query complete. Using natives: %s", 
                       g_UsingNativeInterfaces ? "YES" : "NO (not Streamline proxies)");
              fgNativeLog = true;
          }
      }
      
      // FG MODE: Use native interfaces for overlay when available
      // This is critical - overlay operations on proxy interfaces crash with FG
      if (fgActive && g_UsingNativeInterfaces) {
          if (g_NativeSwapChain) overlaySwapChain = g_NativeSwapChain;
          if (g_NativeDevice) overlayDevice = g_NativeDevice;
          // Keep game's command queue - don't use native queue (per SpecialK)
          
          static bool fgNativeModeLog = false;
          if (!fgNativeModeLog) {
              EarlyLog("DX12 FG: Using native SC/Dev for overlay (SC=%p, Queue=%p, Dev=%p)",
                       overlaySwapChain, overlayQueue, overlayDevice);
              fgNativeModeLog = true;
          }
      }
      
      // FG MODE: When using swapchain wrapper, overlay is drawn from wrapper's Present
      // so skip the normal overlay drawing here
      if (fgActive && g_UsingSwapChainWrapper) {
          // Overlay is handled by the wrapper - don't draw here
          return;
      }
      
      // Check ImGui is initialized - required for DrawOverlay
      if (!g_State.imGuiInit) {
          static int imguiSkipLog = 0;
          if (imguiSkipLog++ % 300 == 0) {
              HookLog("DX12: Skipping overlay - ImGui not initialized yet");
          }
          return;
      }
      
      // Use g_State resources with selected queue/swapchain (native when FG active)
      // NOTE: Overlay must not depend on processCapture/isRealFrame (intro videos, UI frames, etc.)
      if (!g_State.syncInit || !g_State.fence || !overlayQueue) return;
      
      // FG MODE: Drain ONLY on transitions (and with backoff), never every frame.
      if (fgActive && g_FGNeedsDrain.load(std::memory_order_relaxed)) {
          const uint64_t nowUs = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count();
          const uint64_t nextUs = g_FGNextDrainAttemptUs.load(std::memory_order_relaxed);
          if (nowUs >= nextUs) {
              static bool fgDrainLog = false;
              if (!fgDrainLog) {
                  EarlyLog("DX12 FG: Draining queue before overlay draw (one-time / transition)");
                  fgDrainLog = true;
              }

              // Very short timeout to avoid freezing the render thread.
              if (DrainCommandQueue(overlayQueue, 2)) {
                  g_FGNeedsDrain.store(false, std::memory_order_relaxed);
              } else {
                  // Backoff: avoid hammering the queue every frame
                  g_FGNextDrainAttemptUs.store(nowUs + 250000 /* 250ms */, std::memory_order_relaxed);
                  static int drainFailLog = 0;
                  if (drainFailLog++ % 20 == 0) {
                      EarlyLog("DX12 FG: DrainCommandQueue failed; backing off 250ms and skipping overlay");
                  }
                  return;
              }
          } else {
              // Not time to retry yet; skip overlay to avoid stalls
              return;
          }
      }
      
      // FG MODE: Detect swapchain change (FSR4SwapchainProvider replaces swapchain mid-session)
      // If swapchain pointer or buffer count changed, invalidate RTVs for recreation
      if (overlaySwapChain != g_State.cachedSwapChain && g_State.cachedSwapChain != nullptr) {
          static bool swapchainChangeLog = false;
          if (!swapchainChangeLog) {
              EarlyLog("DX12 FG: Swapchain changed (old=%p new=%p) - reinit RTVs required",
                       g_State.cachedSwapChain, overlaySwapChain);
              swapchainChangeLog = true;
          }
          
          // Invalidate RTVs - they're bound to the old swapchain
          CleanupRTVs();
          g_State.cachedSwapChain = nullptr;
          g_State.bufferCount = 0;
          
          // Get new swapchain desc and recreate RTVs
          DXGI_SWAP_CHAIN_DESC desc;
          if (SUCCEEDED(overlaySwapChain->GetDesc(&desc))) {
              CreateRTVs(overlayDevice, overlaySwapChain, desc.BufferCount);
              EarlyLog("DX12 FG: RTVs recreated for new swapchain (bufCount=%u)", desc.BufferCount);
          }
      }
      
      // FG MODE: Use native swapchain for buffer index and GetBuffer
      int bufferIdx = overlaySwapChain->GetCurrentBackBufferIndex();
      
      static int logCounter = 0;
      if (logCounter++ % 1000 == 0) {
          HookLog("DX12: bufferIdx=%d, bufferCount=%u, fgActive=%d", 
                  bufferIdx, g_State.bufferCount, fgActive ? 1 : 0);
      }
      
      // CRITICAL: Validate bufferIdx against RTV heap size
      // FSR4SwapchainProvider may report different buffer count than our cached RTVs
      if (g_State.bufferCount == 0 || (UINT)bufferIdx >= g_State.bufferCount) {
          static int boundsLog = 0;
          if (boundsLog++ % 50 == 0) {
              EarlyLog("DX12 FG: bufferIdx %d out of bounds (heap has %u RTVs) - reinit needed",
                       bufferIdx, g_State.bufferCount);
          }
          // Trigger RTV recreation on next frame
          return;
      }
      
      // Get backbuffer from appropriate swapchain (native when FG active)
      ID3D12Resource* currentBackBuffer = nullptr;
      HRESULT hrGet = overlaySwapChain->GetBuffer(bufferIdx, IID_PPV_ARGS(&currentBackBuffer));
      if (FAILED(hrGet) || !currentBackBuffer) {
          static int getFailLog = 0;
          if (getFailLog++ % 100 == 0) {
              HookLog("DX12: GetBuffer failed bufferIdx=%d hr=0x%08X", 
                      bufferIdx, hrGet);
          }
          return;
      }
      
      int allocIdx = g_State.allocIndex;
      g_State.allocIndex = (g_State.allocIndex + 1) % DX12OverlayState::ALLOC_POOL_SIZE;
      
      UINT64 completed = g_State.fence->GetCompletedValue();
      UINT64 target = g_State.fenceValues[allocIdx];
      if (completed < target) {
          if (fgActive) {
              // Never block indefinitely on the render/present thread in FG mode.
              // If fence doesn't complete quickly, skip overlay this frame.
              if (!WaitFenceWithTimeout(g_State.fence, target, g_State.fenceEvent, 5)) {
                  static int fgAllocWaitLog = 0;
                  if (fgAllocWaitLog++ % 200 == 0) {
                      EarlyLog("DX12 FG: Allocator fence wait timeout (target=%llu completed=%llu) - skipping overlay", target, completed);
                  }
                  currentBackBuffer->Release();
                  return;
              }
          } else {
              g_State.fence->SetEventOnCompletion(target, g_State.fenceEvent);
              WaitForSingleObject(g_State.fenceEvent, INFINITE);
          }
      }
      
      auto *alloc = g_State.allocators[allocIdx];
      auto *list = g_State.cmdList;
      alloc->Reset();
      list->Reset(alloc, nullptr);
      
      D3D12_CPU_DESCRIPTOR_HANDLE rtv =
          g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
      rtv.ptr += (SIZE_T)bufferIdx * g_State.rtvDescriptorSize;
      
      // Use appropriate device for RTV creation (native when FG active)
      overlayDevice->CreateRenderTargetView(currentBackBuffer, nullptr, rtv);
      
      D3D12_RESOURCE_BARRIER preBarrier = {};
      preBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      preBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
      preBarrier.Transition.pResource = currentBackBuffer;
      preBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      preBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
      preBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
      // SpecialK approach: Always perform state transitions, even with FG active
      list->ResourceBarrier(1, &preBarrier);
      
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      
      D3D12_VIEWPORT vp = {0, 0, (float)g_State.cachedWidth, (float)g_State.cachedHeight, 0, 1};
      list->RSSetViewports(1, &vp);
      D3D12_RECT scissor = {0, 0, (LONG)g_State.cachedWidth, (LONG)g_State.cachedHeight};
      list->RSSetScissorRects(1, &scissor);
      
      DrawOverlay(list);
      
      D3D12_RESOURCE_BARRIER postBarrier = {};
      postBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      postBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
      postBarrier.Transition.pResource = currentBackBuffer;
      postBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      postBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
      postBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
      // SpecialK approach: Always perform state transitions, even with FG active
      list->ResourceBarrier(1, &postBarrier);
      
      list->Close();
      
      // Execute on appropriate queue (native when FG active)
      ID3D12CommandList *lists[] = {list};
      overlayQueue->ExecuteCommandLists(1, lists);
      
      g_State.currentFenceValue++;
      g_State.fenceValues[allocIdx] = g_State.currentFenceValue;
      overlayQueue->Signal(g_State.fence, g_State.currentFenceValue);
      
      // FG MODE: Wait for overlay draw to complete before returning
      // This ensures FG runtime sees finished backbuffer state
      if (fgActive) {
          g_State.fence->SetEventOnCompletion(g_State.currentFenceValue, g_State.fenceEvent);
          WaitForSingleObject(g_State.fenceEvent, 50); // 50ms max wait
          g_FGDebugOverlayDraws++;
      }
      
      overlayDrawn = true;
      currentBackBuffer->Release();
    };

    // Lambda for capture operation
    auto doCapture = [&]() {
      // With FG: capture every 4th frame to minimize GPU contention
      // FG is sensitive to queue operations - reduce capture frequency significantly
      static uint64_t fgCaptureCounter = 0;
      bool shouldCapture = true;
      if (fgActive) {
          fgCaptureCounter++;
          shouldCapture = (fgCaptureCounter % 4 == 0);  // Capture every 4th frame only
      }
      
      if (shouldCapture && processCapture && g_IPC->IsRecording() && g_DX12Capture.initialized) {
        if (!g_DX12Capture.captureThreadRunning) {
          g_DX12Capture.StartCaptureThread(AsyncCaptureThreadProc);
        }

        if (g_DX12Capture.gameSyncFence) {
          g_DX12Capture.gameSyncValue++;
          g_CommandQueue->Signal(g_DX12Capture.gameSyncFence,
                                 g_DX12Capture.gameSyncValue);
        }

        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);

        g_DX12Capture.EnqueueFrame(qpc.QuadPart, g_DX12Capture.gameSyncValue,
                               pSwapChain->GetCurrentBackBufferIndex(),
                               pSwapChain);
      }
    };

    // Order capture/overlay based on config
    if (captureIncludeOverlay) {
      doOverlay();   // Draw overlay first
      doCapture();   // Then capture (includes overlay)
    } else {
      doCapture();   // Capture first (clean frame)
      doOverlay();   // Then draw overlay (visible but not recorded)
    }
  }
}


// --- Prerender Limit Support ---
static void ApplyPrerenderLimit(float limit) {
    if (limit < 0.0f || g_CommandQueue == nullptr || g_Device == nullptr) return;

    if (g_PrerenderFence == nullptr) {
        if (FAILED(g_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_PrerenderFence)))) return;
        g_PrerenderEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        g_PrerenderValue = 0;
        g_PrerenderHistory.clear();
        g_PrerenderFrameIndex = 0;
        HookLog("DX12: Created manual prerender fence and event");
    }

    // Manual Fencing (Hard Limit)
    bool isFractional = (limit > 0.01f && limit < 1.0f);
    
    if (limit == 0.0f) {
        // Strict Serial (Wait for current frame)
        g_PrerenderValue++;
        g_CommandQueue->Signal(g_PrerenderFence, g_PrerenderValue);
        if (g_PrerenderFence->GetCompletedValue() < g_PrerenderValue) {
            g_PrerenderFence->SetEventOnCompletion(g_PrerenderValue, g_PrerenderEvent);
            WaitForSingleObject(g_PrerenderEvent, INFINITE);
        }
    } else if (isFractional) {
        // Fractional limits: Sleep is now handled AFTER Present returns
        // See post-Present sleep in DetourPresent
        // No pre-Present processing needed for fractional limits
    } else {
        // Buffered Limit (integer limits > 1)
        int effectiveLimit = (int)limit;
        int lookback = effectiveLimit + 1;
        
        g_PrerenderValue++;
        g_CommandQueue->Signal(g_PrerenderFence, g_PrerenderValue);
        g_PrerenderHistory.push_back(g_PrerenderValue);
        
        if (g_PrerenderHistory.size() >= (size_t)lookback) {
            UINT64 waitValue = g_PrerenderHistory[g_PrerenderHistory.size() - lookback];
            if (waitValue > 0 && g_PrerenderFence->GetCompletedValue() < waitValue) {
                g_PrerenderFence->SetEventOnCompletion(waitValue, g_PrerenderEvent);
                WaitForSingleObject(g_PrerenderEvent, INFINITE);
            }
            // Keep history manageable
            if (g_PrerenderHistory.size() > 32) {
                g_PrerenderHistory.erase(g_PrerenderHistory.begin(), g_PrerenderHistory.end() - 17);
            }
        }
    }
}

// --- Hooks ---

HRESULT STDMETHODCALLTYPE DetourGetBuffer(IDXGISwapChain *pSwapChain, UINT Buffer, REFIID riid, void **ppSurface) {
    HRESULT hr = oGetBuffer(pSwapChain, Buffer, riid, ppSurface);
    
    // If GetBuffer failed (likely due to OOB index from override), return a dummy buffer to prevent crash
    if (FAILED(hr) && Buffer >= 2) {
        // Only trigger this if we suspect an override is active (or just failsafe)
        if (!g_DummyBackBuffer) {
             ID3D12Device* device = nullptr;
             if (SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&device)))) {
                 DXGI_SWAP_CHAIN_DESC desc;
                 if (SUCCEEDED(pSwapChain->GetDesc(&desc))) {
                      // Create a dummy texture matching swapchain props
                      D3D12_HEAP_PROPERTIES heapProps = {};
                      heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
                      
                      D3D12_RESOURCE_DESC resDesc = {};
                      resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                      resDesc.Width = desc.BufferDesc.Width;
                      resDesc.Height = desc.BufferDesc.Height;
                      resDesc.DepthOrArraySize = 1;
                      resDesc.MipLevels = 1;
                      resDesc.Format = desc.BufferDesc.Format;
                      resDesc.SampleDesc = desc.SampleDesc;
                      resDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                      resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

                      HRESULT resHr = device->CreateCommittedResource(
                          &heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
                          D3D12_RESOURCE_STATE_PRESENT, nullptr,
                          IID_PPV_ARGS(&g_DummyBackBuffer));
                      
                      if (SUCCEEDED(resHr)) {
                          HookLog("DX12: GetBuffer: Created Dummy BackBuffer for Index %u", Buffer);
                      } else {
                          HookLog("DX12: GetBuffer: Failed to create Dummy Buffer: 0x%X", resHr);
                      }
                 }
                 device->Release();
             }
        }
        
        if (g_DummyBackBuffer) {
             HookLog("DX12: GetBuffer: Serving Dummy Buffer for Index %u", Buffer);
             return g_DummyBackBuffer->QueryInterface(riid, ppSurface);
        }
    }
    
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain3 *pSwapChain,
                                        UINT SyncInterval, UINT Flags) {
  // Debug: Log that hook is being called (once)
  static bool loggedOnce = false;
  static int callCount = 0;
  callCount++;
  if (!loggedOnce) {
      HookLog("DX12: >>> DetourPresent CALLED! (first call, oPresent=%p)", oPresent);
      loggedOnce = true;
  }
  if (callCount % 600 == 0) { // Log every ~10 seconds at 60fps
      HookLog("DX12: DetourPresent still active (call #%d)", callCount);
  }
  
  // ============================================================================
  // FG OVERLAY SUPPORT - Per Streamline SDK research
  // Phase 1: FG Runtime Detection
  // Phase 2: Swapchain Stabilization (3 seconds after last swapchain creation)
  // Phase 3: Dedicated Queue Overlay Rendering
  // ============================================================================
  
  // CRITICAL: Check if swapchain is being invalidated by FSR4 swapchain recreation
  // If so, skip ALL overlay operations and just passthrough Present
  if (g_SwapchainInvalid.load(std::memory_order_acquire)) {
      // Swapchain is being replaced - abort overlay, just passthrough
      EarlyLog("DX12: Swapchain INVALID - skipping ALL overlay, passthrough only");
      g_FGDebugOverlaySkips++;
      return oPresent(pSwapChain, SyncInterval, Flags);
  }
  
  // FSR4 Cleanup Check: If DX11 hook signaled pending cleanup, do it now on DX12 thread
  // This avoids deadlocks that occur when calling directly from DX11 hook thread
  if (g_FSR4SwapchainRecreatedPending.load(std::memory_order_acquire)) {
      EarlyLog("DX12: Processing pending FSR4 swapchain recreation cleanup on DX12 thread");
      DX12_OnSwapchainResizeBegin();
      g_FSR4SwapchainRecreatedPending.store(false, std::memory_order_release);
      // Clear invalidation flag after cleanup is done
      g_SwapchainInvalid.store(false, std::memory_order_release);
      EarlyLog("DX12: Swapchain invalidation cleared - overlay may resume");
  }
  
  static FGCompatibility::FGType fgType = FGCompatibility::FGType::None;
  static FGCompatibility::FGType lastFgType = FGCompatibility::FGType::None;
  uint64_t frameNum = ++g_FGDebugFrameCount;
  
  // FG Runtime Detection - CONTINUOUS to handle runtime switching
  // Check every 100 frames to detect FG enable/disable/switch
  static uint64_t lastTypeCheck = 0;
  if (frameNum - lastTypeCheck >= 100 || fgType == FGCompatibility::FGType::None) {
      lastTypeCheck = frameNum;
      FGCompatibility::FGType newFgType = g_FGCompat.DetectLoadedFGRuntime();
      
      if (newFgType != lastFgType) {
          const char* fgName = "None";
          switch (newFgType) {
              case FGCompatibility::FGType::DLSS_FG: fgName = "DLSS FG"; break;
              case FGCompatibility::FGType::FSR_FG: fgName = "FSR FG"; break;
              case FGCompatibility::FGType::DLSS_MSFG: fgName = "DLSS Multi-FG"; break;
              default: break;
          }
          EarlyLog("DX12 FG: Frame %llu - FG runtime changed to: %s", frameNum, fgName);
          lastFgType = newFgType;
          
          // If switching TO FSR FG, log warning
          if (newFgType == FGCompatibility::FGType::FSR_FG) {
              EarlyLog("DX12 FG: FSR FG detected - overlay DISABLED (temporary workaround)");
          }
      }
      fgType = newFgType;
  }
  
  // FSR3 Swapchain Detection - check continuously for FSR3 wrapper
  if (pSwapChain) {
      bool isFSR3 = IsFSR3SwapChain(pSwapChain);
      if (isFSR3 != g_UsingFSR3Swapchain) {
          g_UsingFSR3Swapchain = isFSR3;
          if (isFSR3) {
              EarlyLog("DX12: Detected FSR3 FrameInterpolationSwapChain - disabling overlay");
          } else {
              EarlyLog("DX12: FSR3 swapchain wrapper no longer active");
          }
      }
  }
  
  // FSR FG SKIP: Disable overlay entirely when FSR FG is detected (temporary workaround)
  bool fsrFGActive = (fgType == FGCompatibility::FGType::FSR_FG) || g_UsingFSR3Swapchain;
  if (fsrFGActive) {
      g_FGDebugOverlaySkips++;
      return oPresent(pSwapChain, SyncInterval, Flags);
  }
  
  // Record frame for FG metrics
  int cmdListCount = g_CommandListsExecutedThisFrame.exchange(0);
  if (g_FGDebugFrameCount < 12000) { // Log first ~3 minutes
      EarlyLog("DX12: Present - RecordFrame cmdListCount=%d", cmdListCount);
  }
  g_FGCompat.RecordFrame(cmdListCount);
  
  // FG Swapchain Stabilization Check (DLSS FG only now - reduced delay for constant overlay)
  // DLSS FG uses much shorter stabilization (100ms instead of 500ms) for minimal overlay gap
  bool dlssFGActive = (fgType == FGCompatibility::FGType::DLSS_FG || 
                       fgType == FGCompatibility::FGType::DLSS_MSFG);
  bool fgActive = dlssFGActive;
  bool fgStabilized = false;
  
  // Very short stabilization for DLSS FG - just 100ms to minimize overlay gap
  constexpr int DLSS_STABILIZATION_MS = 100;
  
  if (fgActive) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - g_LastSwapchainCreation).count();
      
      if (elapsed >= DLSS_STABILIZATION_MS) {
          if (!g_FGSwapchainStabilized) {
              g_FGSwapchainStabilized = true;
              EarlyLog("DX12 FG: Frame %llu - DLSS swapchain stabilized after %lld ms. Overlay enabled.",
                       frameNum, elapsed);
          }
          fgStabilized = true;
      } else {
          // Still stabilizing - brief passthrough
          if (frameNum % 50 == 0) {
              EarlyLog("DX12 FG: Frame %llu - DLSS stabilizing... %lld/%d ms", frameNum, elapsed, DLSS_STABILIZATION_MS);
          }
          g_FGDebugOverlaySkips++;
          return oPresent(pSwapChain, SyncInterval, Flags);
      }
  }
  
  // Normal path continues below (both FG stabilized and non-FG)
  static int64_t qpcFreq = 0;
  if (qpcFreq == 0) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpcFreq = f.QuadPart;
  }
  LARGE_INTEGER qpc;
  QueryPerformanceCounter(&qpc);

  // Note: cmdListCount already defined and RecordFrame called at start of DetourPresent
  bool isRealFrame = (cmdListCount > 0);

  // Convert to microseconds
  int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;

  g_PerfMetrics.Update(us);

  // Update recording state for CSV logging
  bool isRecording = g_IPC && g_IPC->IsRecording();
  g_PerfMetrics.SetRecording(isRecording);

  UINT oldInterval = SyncInterval;
  UINT oldFlags = Flags;

  // NOTE: VSync override re-enabled if configured
  // Debug: Trace IPC state
  static bool ipcStateLogged = false;
  if (!ipcStateLogged) {
      HookLog("DX12: VSync Debug: g_IPC=%p", (void*)g_IPC);
      if (g_IPC) {
          auto* shm = g_IPC->GetSharedMem();
          HookLog("DX12: VSync Debug: shm=%p", (void*)shm);
      }
      ipcStateLogged = true;
  }
  
  // Apply VSync Override
  VSyncOverride vsync = GetVSyncOverride();
  if (vsync.shouldOverride) {
      SyncInterval = (UINT)vsync.presentInterval;
  }

  // Apply Prerender Limit
  float limit = GetActivePrerenderLimit();
  if (limit >= 0.0f) {
      static float lastLimit = -2.0f;
      if (fabs(limit - lastLimit) > 0.001f) {
           UINT effectiveLatency = (limit < 1.0f) ? 1 : (UINT)limit;
           pSwapChain->SetMaximumFrameLatency(effectiveLatency);
           HookLog("DX12: Set maximum DXGI latency to %d (Active Limit: %.2f)", effectiveLatency, limit);
           lastLimit = limit;
      }
      ApplyPrerenderLimit(limit);
  }

  // Sanitize Flags for VSync
  if (SyncInterval > 0) {
      // DXGI_PRESENT_ALLOW_TEARING = 512
      Flags &= ~512; // DXGI_PRESENT_ALLOW_TEARING
  }

  if (oldInterval != SyncInterval || oldFlags != Flags) {
      // Rate limit logging to avoid overhead every frame
      static int logCount = 0;
      if (logCount < 5) {
          HookLog("DX12: Present: Override VSync (Interval: %u -> %u, Flags: 0x%X -> 0x%X)", 
                  oldInterval, SyncInterval, oldFlags, Flags);
          logCount++;
      }
  }

  // SpecialK approach: Draw overlay BEFORE calling original Present
  // This allows the overlay to be composited with the frame before FG processes it
  // CRITICAL: Second invalidation check - catches mid-Present invalidation
  if (!g_SwapchainInvalid.load(std::memory_order_acquire)) {
      ProcessFrame(pSwapChain, isRealFrame);
  }
  
  // Apply shared FPS limiter BEFORE Present (when not FG active)
  // DISABLED when FG active - FPS limiter interferes with FG timing and causes FG to disable itself
  // DISABLED when Vulkan is primary - NVIDIA promotes Vulkan to DXGI, causing double-limiting
  if (!fgActive && !IsVulkanPrimary()) {
      g_SharedFpsLimiter.SetIPCClient(g_IPC);
      g_SharedFpsLimiter.Apply();
  }

  // CRITICAL: Third invalidation check before calling oPresent
  // If swapchain became invalid, the pSwapChain pointer may be stale
  if (g_SwapchainInvalid.load(std::memory_order_acquire)) {
      EarlyLog("DX12: Swapchain INVALID detected before oPresent - aborting frame");
      g_FSR4SwapchainRecreatedPending.store(false, std::memory_order_release);
      g_SwapchainInvalid.store(false, std::memory_order_release);
      return S_OK;  // Return success to avoid game thinking Present failed
  }

  // Now call the original Present
  HRESULT hr = oPresent(pSwapChain, SyncInterval, Flags);
  
  // Post-Present sleep for fractional limits
  // Sleep AFTER Present returns - GPU has just finished its frame and is idle
  float postLimit = GetActivePrerenderLimit();
  bool isPostFractional = (postLimit > 0.01f && postLimit < 1.0f);
  if (isPostFractional) {
      float fps = g_PerfMetrics.GetCurrentFPS();
      double targetFrameTimeUs = (fps > 1.0f) ? (1000000.0 / fps) : 16666.0;
      
      // Fixed Idle Gap = TargetFrameTime * (1.0 - limit) * 0.10
      // For limit=0.5 at 60fps: 16666 * 0.5 * 0.10 = ~833us
      int64_t idleGapUs = (int64_t)(targetFrameTimeUs * (1.0 - postLimit) * 0.10);
      if (idleGapUs > 0) {
          if (idleGapUs > 10000) idleGapUs = 10000; // Cap at 10ms
          PrecisionSleep(idleGapUs);
      }
  }
  
  return hr;
}

HRESULT STDMETHODCALLTYPE
DetourPresent1(IDXGISwapChain3 *pSwapChain, UINT SyncInterval, UINT Flags,
               const DXGI_PRESENT_PARAMETERS *pPresentParameters) {
  // FG-SAFE: No more passthrough mode - overlay draws on all frames
  // Device change detection is handled in DetourPresent
  
  // FG: Record frame for behavioral detection
  int cmdListCount = g_CommandListsExecutedThisFrame.exchange(0);
  bool isRealFrame = (cmdListCount > 0);
  g_FGCompat.RecordFrame(cmdListCount);

  ProcessFrame(pSwapChain, isRealFrame);

  // Apply shared FPS limiter
  // DISABLED when FG active - FPS limiter interferes with FG timing
  // DISABLED when Vulkan is primary - NVIDIA promotes Vulkan to DXGI, causing double-limiting
  bool fgActive = g_FGCompat.IsFGActive();
  if (!fgActive && !IsVulkanPrimary()) {
      g_SharedFpsLimiter.SetIPCClient(g_IPC);
      g_SharedFpsLimiter.Apply();
  }

  UINT oldInterval = SyncInterval;
  UINT oldFlags = Flags;

  // Apply VSync Override
  VSyncOverride vsync = GetVSyncOverride();
  if (vsync.shouldOverride) {
      SyncInterval = (UINT)vsync.presentInterval;
  }

  // Apply Prerender Limit
  float limit = GetActivePrerenderLimit();
  if (limit >= 0.0f) {
      static float lastLimit = -2.0f;
      if (fabs(limit - lastLimit) > 0.001f) {
           UINT effectiveLatency = (limit < 1.0f) ? 1 : (UINT)limit;
           pSwapChain->SetMaximumFrameLatency(effectiveLatency);
           HookLog("DX12: Set maximum DXGI latency to %d (Active Limit: %.2f)", effectiveLatency, limit);
           lastLimit = limit;
      }
      ApplyPrerenderLimit(limit);
  }

  // Sanitize Flags for VSync
  if (SyncInterval > 0) {
      Flags &= ~512; // DXGI_PRESENT_ALLOW_TEARING
  }

  if (oldInterval != SyncInterval || oldFlags != Flags) {
      static int logCount = 0;
      if (logCount < 5) {
          HookLog("DX12: Present1: Override VSync (Interval: %u -> %u, Flags: 0x%X -> 0x%X)", 
                  oldInterval, SyncInterval, oldFlags, Flags);
          logCount++;
      }
  }

  HRESULT hr = oPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
  return hr;
}

static void STDMETHODCALLTYPE DetourCreateSampler(
    ID3D12Device *pDevice, const D3D12_SAMPLER_DESC *pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (!pDesc) {
        oCreateSampler(pDevice, pDesc, DestDescriptor);
        return;
    }
    
    if (!g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        oCreateSampler(pDevice, pDesc, DestDescriptor);
        return;
    }

    // Debug Log for Sampler Creation
    static int samplerLogCount = 0;
    if (samplerLogCount < 5) {
        HookLog("DX12: CreateSampler called");
        samplerLogCount++;
    }

    D3D12_SAMPLER_DESC desc = *pDesc;
    bool modified = false;

    // Check availability of mipmaps
    bool overridesAllowed = true;
    if (pDesc->MaxLOD == 0.0f) overridesAllowed = false;
    if (pDesc->MinLOD == pDesc->MaxLOD) overridesAllowed = false;

    bool userBiasActive = false;
    float userBiasVal = 0.0f;

    if (overridesAllowed && g_IPC) {
        const auto& gfx = GetActiveGraphicsConfig();

        // Anisotropic Filtering
        const char* af = gfx.anisotropicFiltering.c_str();
        if (af[0] != 'd') { // default
            if (af[0] == 'o' && strncmp(af, "off", 3) == 0) {
                 if ((desc.Filter & D3D12_FILTER_ANISOTROPIC) || (desc.Filter & D3D12_FILTER_COMPARISON_ANISOTROPIC)) {
                    desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                    desc.MaxAnisotropy = 1;
                    modified = true;
                    HookLog("DX12: CreateSampler: Forced AF OFF");
                }
            } else {
                int maxAniso = 16;
                bool isAF = false;
                if (af[0] == '2' && af[1] == 'x') { maxAniso = 2; isAF = true; }
                else if (af[0] == '4' && af[1] == 'x') { maxAniso = 4; isAF = true; }
                else if (af[0] == '8' && af[1] == 'x') { maxAniso = 8; isAF = true; }
                else if (af[0] == '1' && af[1] == '6') { maxAniso = 16; isAF = true; }
                
                if (isAF) {
                    bool comparison = (desc.Filter >= D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT);
                    desc.Filter = comparison ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC;
                    desc.MaxAnisotropy = maxAniso;
                    modified = true;
                    HookLog("DX12: CreateSampler: Forced AF %dx", maxAniso);
                }
            }
        }
        
        // Mip Mapping
        const char* mip = gfx.mipMapping.c_str();
        if (mip[0] != 'd') {
             if (mip[0] == 't' && strncmp(mip, "trilinear", 9) == 0) {
                 if (!(desc.Filter & D3D12_FILTER_ANISOTROPIC)) { // Don't break AF
                     desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                     modified = true;
                     HookLog("DX12: CreateSampler: Forced Trilinear");
                 }
             } else if (mip[0] == 'b' && strncmp(mip, "bilinear", 8) == 0) {
                 if (!(desc.Filter & D3D12_FILTER_ANISOTROPIC)) {
                     desc.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
                     modified = true;
                     HookLog("DX12: CreateSampler: Forced Bilinear");
                 }
             }
        }
        
        // Mip Bias
        const char* biasStr = gfx.mipBias.c_str();
        if (biasStr[0] != 'd') {
             char* end;
             float val = strtof(biasStr, &end);
             if (end != biasStr) {
                userBiasActive = true;
                userBiasVal = val;
                float originalBias = pDesc->MipLODBias;
                std::string mode = gfx.mipBiasMode;
                
                if (mode == "offset") {
                    desc.MipLODBias = originalBias + val;
                } else if (mode == "base") {
                    if (originalBias < 0.0f) {
                        desc.MipLODBias = originalBias + val;
                    } else {
                        desc.MipLODBias = originalBias;
                    }
                } else {
                    // Strict
                    desc.MipLODBias = val;
                }
                modified = true;
                HookLog("DX12: CreateSampler: Forced MipBias %.2f (Mode: %s, Orig: %.2f)", desc.MipLODBias, mode.c_str(), originalBias);
             }
        }

        // SGSSAA Bias
        float sgssaaBias = 0.0f;
        if (GetSGSSAABias(gfx.sgssaa, gfx.msaaSamples.c_str(), sgssaaBias)) {
             desc.MipLODBias += sgssaaBias;
             modified = true;
        }

        if (userBiasActive && userBiasVal < 0.0f && !gfx.sgssaa && IsUnityProcess()) {
            if (desc.MipLODBias < -0.5f) {
                desc.MipLODBias = -0.5f;
                modified = true;
            }
        }
    }
    
    oCreateSampler(pDevice, modified ? &desc : pDesc, DestDescriptor);
}

// --- Static Sampler Override via Root Signature Serialization ---
// This is the key to forcing AF in DX12 games that use static samplers in root signatures

// Helper to modify static sampler for SGSSAA
static void ModifyStaticSamplerSGSSAA(D3D12_STATIC_SAMPLER_DESC& sampler) {
     const auto& gfx = GetActiveGraphicsConfig();
     if (gfx.sgssaa) {
         float bias = 0.0f;
         if (GetSGSSAABias(gfx.sgssaa, gfx.msaaSamples.c_str(), bias)) {
             sampler.MipLODBias += bias;
         }
     }
}

// Helper to modify static samplers based on config
static void ModifyStaticSampler(D3D12_STATIC_SAMPLER_DESC& sampler) {
    if (!g_IPC) return;
    
    auto* shm = g_IPC->GetSharedMem();
    if (!shm) return;

    const auto& gfx = GetActiveGraphicsConfig();
    
    // 1. Anisotropic Filtering
    std::string af = gfx.anisotropicFiltering;
    if (af != "default") {
        // Handle AF off
        if (af == "off") {
            if (sampler.Filter == D3D12_FILTER_ANISOTROPIC || 
                sampler.Filter == D3D12_FILTER_COMPARISON_ANISOTROPIC) {
                sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                sampler.MaxAnisotropy = 1;
                HookLog("DX12: Static Sampler: Forced AF OFF");
            }
        } else {
            // Handle AF levels (2x, 4x, 8x, 16x)
            int maxAniso = 16;
            bool isAF = false;
            if (af[0] == '2' && af[1] == 'x') { maxAniso = 2; isAF = true; }
            else if (af[0] == '4' && af[1] == 'x') { maxAniso = 4; isAF = true; }
            else if (af[0] == '8' && af[1] == 'x') { maxAniso = 8; isAF = true; }
            else if (af[0] == '1' && af[1] == '6') { maxAniso = 16; isAF = true; }
            
            if (isAF) {
                D3D12_FILTER newFilter = sampler.Filter;
                bool canForceAF = false;

                switch (sampler.Filter) {
                    case D3D12_FILTER_MIN_MAG_MIP_LINEAR:
                    case D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT:
                        newFilter = D3D12_FILTER_ANISOTROPIC;
                        canForceAF = true;
                        break;
                    case D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT:
                        newFilter = D3D12_FILTER_COMPARISON_ANISOTROPIC;
                        canForceAF = true;
                        break;
                    default:
                        // Don't force AF on other filter types (e.g. Point, or unknown)
                        break;
                }

                if (canForceAF) {
                    sampler.Filter = newFilter;
                    sampler.MaxAnisotropy = maxAniso;
                    // Move logging to caller or rate limit if strictly needed
                    // HookLog("DX12: Static Sampler: Forced AF %dx", maxAniso); 
                }
            }
        }
    }

    // 2. Mip Mapping

    std::string mip = gfx.mipMapping;
    if (mip != "default") {
        if (mip == "trilinear") {
             // Force Linear Mip Filter
             if (sampler.Filter == D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT) sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
             // ... other cases could be expanded logic
        } else if (mip == "bilinear") {
             // Force Point Mip Filter
             if (sampler.Filter == D3D12_FILTER_MIN_MAG_MIP_LINEAR) sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        }
    }

    // 3. Mip Bias
    std::string biasStr = gfx.mipBias;
    bool userBiasActive = false;
    float userBiasVal = 0.0f;
    if (biasStr != "default") {
        try {
            float val = std::stof(biasStr);
            userBiasActive = true;
            userBiasVal = val;
            float originalBias = sampler.MipLODBias;
            std::string mode = gfx.mipBiasMode;

            if (mode == "offset") {
                sampler.MipLODBias = originalBias + val;
            } else if (mode == "base") {
                if (originalBias < 0.0f) {
                    sampler.MipLODBias = originalBias + val;
                }
            } else {
                // Strict
                sampler.MipLODBias = val;
            }
            HookLog("DX12: Static Sampler: Forced MipBias %.2f (Mode: %s, Orig: %.2f)", sampler.MipLODBias, mode.c_str(), originalBias);
        } catch(...) {}
    }

    if (userBiasActive && userBiasVal < 0.0f && !gfx.sgssaa && IsUnityProcess()) {
        if (sampler.MipLODBias < -0.5f) {
            sampler.MipLODBias = -0.5f;
        }
    }
}

HRESULT WINAPI DetourSerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC* pRootSignature,
    D3D_ROOT_SIGNATURE_VERSION Version,
    ID3DBlob** ppBlob,
    ID3DBlob** ppErrorBlob)
{
    if (!pRootSignature || pRootSignature->NumStaticSamplers == 0) {
        return oSerializeRootSignature(pRootSignature, Version, ppBlob, ppErrorBlob);
    }
    
    static int logCount = 0;
    if (logCount < 5) {
        HookLog("DX12: SerializeRootSignature intercepted (%d static samplers)", pRootSignature->NumStaticSamplers);
        logCount++;
    }
    
    // Create a copy of the root signature desc with modified static samplers
    D3D12_ROOT_SIGNATURE_DESC modifiedDesc = *pRootSignature;
    
    // Allocate and copy static samplers
    D3D12_STATIC_SAMPLER_DESC* modifiedSamplers = new D3D12_STATIC_SAMPLER_DESC[pRootSignature->NumStaticSamplers];
    for (UINT i = 0; i < pRootSignature->NumStaticSamplers; i++) {
        modifiedSamplers[i] = pRootSignature->pStaticSamplers[i];
        ModifyStaticSampler(modifiedSamplers[i]);
    }
    modifiedDesc.pStaticSamplers = modifiedSamplers;
    
    HRESULT hr = oSerializeRootSignature(&modifiedDesc, Version, ppBlob, ppErrorBlob);
    
    delete[] modifiedSamplers;
    return hr;
}

HRESULT WINAPI DetourSerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pRootSignature,
    ID3DBlob** ppBlob,
    ID3DBlob** ppErrorBlob)
{
    if (!pRootSignature) {
        return oSerializeVersionedRootSignature(pRootSignature, ppBlob, ppErrorBlob);
    }
    
    UINT numSamplers = 0;
    switch (pRootSignature->Version) {
        case D3D_ROOT_SIGNATURE_VERSION_1_0:
            numSamplers = pRootSignature->Desc_1_0.NumStaticSamplers;
            break;
        case D3D_ROOT_SIGNATURE_VERSION_1_1:
            numSamplers = pRootSignature->Desc_1_1.NumStaticSamplers;
            break;
        default:
            // Unknown version, pass through
            return oSerializeVersionedRootSignature(pRootSignature, ppBlob, ppErrorBlob);
    }
    
    if (numSamplers == 0) {
        return oSerializeVersionedRootSignature(pRootSignature, ppBlob, ppErrorBlob);
    }
    
    static int logCount = 0;
    if (logCount < 5) {
        HookLog("DX12: SerializeVersionedRootSignature intercepted (Version=%d, %d static samplers)", 
                pRootSignature->Version, numSamplers);
        logCount++;
    }
    
    // Create a copy with modified static samplers
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC modifiedDesc = *pRootSignature;
    D3D12_STATIC_SAMPLER_DESC* modifiedSamplers = new D3D12_STATIC_SAMPLER_DESC[numSamplers];
    
    if (pRootSignature->Version == D3D_ROOT_SIGNATURE_VERSION_1_0) {
        for (UINT i = 0; i < numSamplers; i++) {
            modifiedSamplers[i] = pRootSignature->Desc_1_0.pStaticSamplers[i];
            ModifyStaticSampler(modifiedSamplers[i]);
        }
        modifiedDesc.Desc_1_0.pStaticSamplers = modifiedSamplers;
    } else if (pRootSignature->Version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
        // 1.1 uses D3D12_STATIC_SAMPLER_DESC1 which is compatible layout-wise
        for (UINT i = 0; i < numSamplers; i++) {
            // Copy to D3D12_STATIC_SAMPLER_DESC (base fields are compatible)
            memcpy(&modifiedSamplers[i], &pRootSignature->Desc_1_1.pStaticSamplers[i], 
                   sizeof(D3D12_STATIC_SAMPLER_DESC));
            ModifyStaticSampler(modifiedSamplers[i]);
        }
        // For 1.1, we need to allocate proper 1.1 samplers
        D3D12_STATIC_SAMPLER_DESC* modifiedSamplers1_1 = new D3D12_STATIC_SAMPLER_DESC[numSamplers];
        for (UINT i = 0; i < numSamplers; i++) {
            memcpy(&modifiedSamplers1_1[i], &pRootSignature->Desc_1_1.pStaticSamplers[i], sizeof(D3D12_STATIC_SAMPLER_DESC));
            modifiedSamplers1_1[i].Filter = modifiedSamplers[i].Filter;
            modifiedSamplers1_1[i].MaxAnisotropy = modifiedSamplers[i].MaxAnisotropy;
        }
        modifiedDesc.Desc_1_1.pStaticSamplers = (const D3D12_STATIC_SAMPLER_DESC*)modifiedSamplers1_1;
        
        HRESULT hr = oSerializeVersionedRootSignature(&modifiedDesc, ppBlob, ppErrorBlob);
        
        delete[] modifiedSamplers;
        delete[] (D3D12_STATIC_SAMPLER_DESC*)modifiedSamplers1_1;
        return hr;
    }
    
    HRESULT hr = oSerializeVersionedRootSignature(&modifiedDesc, ppBlob, ppErrorBlob);
    
    delete[] modifiedSamplers;
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourCreateCommittedResource(
    ID3D12Device *device, const D3D12_HEAP_PROPERTIES *pHeapProperties,
    D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc,
    D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE *pOptimizedClearValue, REFIID riidResource,
    void **ppvResource) {
    
    if (pDesc) {
        const auto& gfx = GetActiveGraphicsConfig();
        const char* msaa = gfx.msaaSamples.c_str();
        if (msaa[0] != 'd') {
            UINT count = 1;
            if (strcmp(msaa, "2x") == 0) count = 2;
            else if (strcmp(msaa, "4x") == 0) count = 4;
            else if (strcmp(msaa, "8x") == 0) count = 8;
            
            if (count > 1 || strcmp(msaa, "off") == 0) {
                if (strcmp(msaa, "off") == 0) count = 1;
                
                // Only upgrade for RT or DS that look like screen-sized resources
                if (pDesc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) {
                    D3D12_RESOURCE_DESC modifiedDesc = *pDesc;
                    modifiedDesc.SampleDesc.Count = count;
                    modifiedDesc.SampleDesc.Quality = 0;
                    
                    // HookLog("DX12: CreateCommittedResource: Forcing MSAA %dx", count);
                    return oCreateCommittedResource(device, pHeapProperties, HeapFlags, &modifiedDesc, InitialResourceState, pOptimizedClearValue, riidResource, ppvResource);
                }
            }
        }
    }
    
    return oCreateCommittedResource(device, pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, riidResource, ppvResource);
}

void STDMETHODCALLTYPE
DetourExecuteCommandLists(ID3D12CommandQueue *pThis, UINT NumCommandLists,
                          ID3D12CommandList *const *ppCommandLists) {
  // FG: Track command list execution for real frame detection
  // 
  // Key insight for MFG detection with DLSS FG:
  // - Real frames: Render queue executes multiple command lists in a batch (Num >= 2)
  // - Interpolated frames: Only have single command list executions (Num = 1) for compose/present
  // 
  // With DLSS FG, there are 2 queues:
  // 1. Presentation queue (g_CommandQueue from swapchain) - always Num=1 per present
  // 2. Render queue (different queue) - Num=2-3+ for scene rendering, only on real frames
  // 
  // Strategy: Count command lists from ANY DIRECT queue, but only if Num > 1.
  // This filters out the presentation work (always Num=1) and only captures real render work.
  // With 4x MFG: 1 frame per 4 presents has render work (ratio ~4.0)
  
  // Check if FG runtime DLL is loaded
  const auto fgType = g_FGCompat.GetDllDetectedType();
  const bool fgDllLoaded = (fgType == FGCompatibility::FGType::DLSS_FG || 
                            fgType == FGCompatibility::FGType::DLSS_MSFG ||
                            fgType == FGCompatibility::FGType::FSR_FG);
  
  // Count command lists - strategy depends on FG state
  if (fgDllLoaded) {
      // FG active: Only count batches with Num > 2 (real render work, not presentation or overhead)
      // This captures work from ANY DIRECT queue. Log analysis shows overhead/interpolated frames 
      // can have Num=2, while real frames have Num=3+.
      if (NumCommandLists > 2) {
          g_CommandListsExecutedThisFrame.fetch_add(NumCommandLists, std::memory_order_relaxed);
      }
  } else {
      // No FG: Count from g_CommandQueue only (original behavior)
      if (pThis == g_CommandQueue) {
          g_CommandListsExecutedThisFrame.fetch_add(NumCommandLists, std::memory_order_relaxed);
      }
  }
  
  // Debug logging (first 50 only)
  // Debug logging disabled to prevent contention
  /*
  static std::atomic<int> s_logCmd{0};
  int count = s_logCmd.fetch_add(1);
  if (count < 50) { 
     EarlyLog("DX12: ExecuteCommandLists: Num=%u, Queue=%p (Active=%p, Match=%d, FG_DLL=%d)", 
              NumCommandLists, pThis, g_CommandQueue, (pThis == g_CommandQueue), fgDllLoaded);
  }
  if (count > 2000) s_logCmd.store(0);
  */

  // Capture queue if not yet available (enables first-frame overlay)
  // Cache queue type by tracking in g_DeviceQueues map - avoid repeated GetDesc() calls
  bool isDirectQueue = false;
  if (pThis) {
    ID3D12Device* dev = nullptr;
    if (SUCCEEDED(pThis->GetDevice(IID_PPV_ARGS(&dev)))) {
        std::lock_guard<std::mutex> lock(g_DeviceQueuesMutex);
        // Check if this queue is in our tracked map (implies it's DIRECT)
        if (g_DeviceQueues.count(dev) && g_DeviceQueues[dev] == pThis) {
            isDirectQueue = true;
        } else if (pThis->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            // New DIRECT queue - track it
            if (g_DeviceQueues.count(dev)) g_DeviceQueues[dev]->Release();
            g_DeviceQueues[dev] = pThis;
            pThis->AddRef();
            isDirectQueue = true;
        }
        dev->Release();
    }
  }
  
  // Queue capture heuristic - only when FG DLL is NOT loaded (to prevent flapping during FG)
  // Once FG DLL is loaded, we lock in the currently captured queue as the presentation queue
  if (isDirectQueue && !fgDllLoaded && !g_IsQueueFromSwapchain && g_CommandQueue != pThis) {
      if (g_CommandQueue) g_CommandQueue->Release();
      g_CommandQueue = pThis;
      g_CommandQueue->AddRef();
      EarlyLog("DX12: ExecuteCommandLists: Captured queue %p (Heuristic)", g_CommandQueue);
  }
  
  oExecuteCommandLists(pThis, NumCommandLists, ppCommandLists);
}

typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChain)(IDXGIFactory *, IUnknown *,
                                              DXGI_SWAP_CHAIN_DESC *,
                                              IDXGISwapChain **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChainForHwnd)(
    IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChainForCoreWindow)(
    IDXGIFactory2 *, IUnknown *, IUnknown *, const DXGI_SWAP_CHAIN_DESC1 *,
    IDXGIOutput *, IDXGISwapChain1 **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChainForComposition)(
    IDXGIFactory2 *, IUnknown *, const DXGI_SWAP_CHAIN_DESC1 *,
    IDXGIOutput *, IDXGISwapChain1 **);

PFN_CreateSwapChain oCreateSwapChain = nullptr;
PFN_CreateSwapChainForHwnd oCreateSwapChainForHwnd = nullptr;
PFN_CreateSwapChainForCoreWindow oCreateSwapChainForCoreWindow = nullptr;
PFN_CreateSwapChainForComposition oCreateSwapChainForComposition = nullptr;

HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory *pThis,
                                                IUnknown *pDevice,
                                                DXGI_SWAP_CHAIN_DESC *pDesc,
                                                IDXGISwapChain **ppSwapChain) {
  EarlyLog("DX12: DetourCreateSwapChain called (pDevice=%p, pDesc=%p)", pDevice, pDesc);
  if (!pDesc) return DXGI_ERROR_INVALID_CALL;
  if (pDevice) {
    ID3D12CommandQueue *queue = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue),
                                          (void **)&queue))) {
      // Release old queue before capturing new one
      if (g_CommandQueue) g_CommandQueue->Release();
      g_CommandQueue = queue; // Keep the reference from QueryInterface
      g_IsQueueFromSwapchain = true;
      EarlyLog("DX12: DetourCreateSwapChain: Auth Queue %p captured from Swapchain", g_CommandQueue);
      
      ID3D12Device* dev = nullptr;
      if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&dev)))) {
          std::lock_guard<std::mutex> lock(g_DeviceQueuesMutex);
          // If we already have a queue for this device, release it
          if (g_DeviceQueues.count(dev)) g_DeviceQueues[dev]->Release();
          g_DeviceQueues[dev] = queue; 
          queue->AddRef(); // One for the map
          
          if (g_Device) g_Device->Release();
          g_Device = dev; // Keep the reference from GetDevice

          LUID luid = dev->GetAdapterLuid();
          ReportLUID(luid.LowPart, luid.HighPart);
      }
      HookLog("CreateSwapChain: Captured Queue %p", g_CommandQueue);
    }
    g_FGCompat.OnSwapchainRecreation(); // Notify FG detection (triggers suspend)
  }


  DXGI_SWAP_CHAIN_DESC modifiedDesc = *pDesc;
  {
      const GraphicsConfig& gfx = GetActiveGraphicsConfig();
      // Backbuffer Count
      int count = gfx.backbufferCount;
      if (count >= 2 && count <= 6) {
          modifiedDesc.BufferCount = (UINT)count;
          HookLog("DX12/DXGI: CreateSwapChain: Overriding BufferCount to %d", count);
      }
      
      // MSAA Override (ONLY for D3D11/10/9 devices, D3D12 MUST be 1x)
      ID3D12Device* d12Dev = nullptr;
      bool isD12 = (pDevice && SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12Device), (void**)&d12Dev)));
      if (d12Dev) d12Dev->Release();

      if (!isD12) {
          const char* msaa = gfx.msaaSamples.c_str();
          if (msaa[0] != 'd') {
              if (strcmp(msaa, "off") == 0) {
                  modifiedDesc.SampleDesc.Count = 1;
                  modifiedDesc.SampleDesc.Quality = 0;
                  HookLog("DX11/DXGI: CreateSwapChain: Forcing MSAA OFF");
              } else {
                  UINT samples = 1;
                  if (strcmp(msaa, "2x") == 0) samples = 2;
                  else if (strcmp(msaa, "4x") == 0) samples = 4;
                  else if (strcmp(msaa, "8x") == 0) samples = 8;
                  
                  if (samples > 1) {
                      modifiedDesc.SampleDesc.Count = samples;
                      modifiedDesc.SampleDesc.Quality = 0;
                      modifiedDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; // Required for MSAA
                      HookLog("DX11/DXGI: CreateSwapChain: Forcing MSAA %dx", samples);
                  }
              }
          }
      } else {
          // For D3D12, we must ensure Count=1 if the user tried to force MSAA on swapchain
          if (modifiedDesc.SampleDesc.Count > 1) {
              modifiedDesc.SampleDesc.Count = 1;
              modifiedDesc.SampleDesc.Quality = 0;
              HookLog("DX12/DXGI: CreateSwapChain: Clamping MSAA to 1x (D3D12 requirement)");
          }
      }
  }



  HRESULT hr = oCreateSwapChain(pThis, pDevice, &modifiedDesc, ppSwapChain);
  
  if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
       // Capture g_LastSwapChain immediately to prevent false-positive "Device Changed" detection
       // in ProcessFrame (which triggers 3s suspend for FG safety)
       IDXGISwapChain3 *swapChain3 = nullptr;
       if (SUCCEEDED((*ppSwapChain)->QueryInterface(__uuidof(IDXGISwapChain3), (void **)&swapChain3))) {
           // Weak pointer only (do not affect refcounts)
           g_LastSwapChain = swapChain3;
           swapChain3->Release();
       }

      if (g_IPC) {
           void **vtbl = *reinterpret_cast<void ***>(*ppSwapChain);
           EarlyLog("DX12: SwapChain created (Present=%p, ResizeBuffers=%p)", vtbl[8], vtbl[13]);
      }
  }

  return hr;
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(
    IDXGIFactory2 *pThis, IUnknown *pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1 *pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
    IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) {
  
  // FG SWAPCHAIN TRACKING: Record creation for stabilization timing
  int swapCount = ++g_SwapchainCreationCount;
  g_LastSwapchainCreation = std::chrono::steady_clock::now();
  g_FGSwapchainStabilized = false; // Reset stabilization on any swapchain creation
  EarlyLog("DX12 FG: CreateSwapChainForHwnd #%d called (pDevice=%p, hWnd=%p, Size=%ux%u)", 
           swapCount, pDevice, hWnd, pDesc ? pDesc->Width : 0, pDesc ? pDesc->Height : 0);
  
  if (!pDesc) return DXGI_ERROR_INVALID_CALL;
  if (pDevice) {
    ID3D12CommandQueue *queue = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue),
                                          (void **)&queue))) {
      // Release old queue before capturing new one
      if (g_CommandQueue) g_CommandQueue->Release();
      g_CommandQueue = queue; // Keep the reference from QueryInterface
      g_IsQueueFromSwapchain = true;
      EarlyLog("DX12: DetourCreateSwapChainForHwnd: Auth Queue %p captured from Swapchain", g_CommandQueue);
      
      ID3D12Device* dev = nullptr;
      if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&dev)))) {
          std::lock_guard<std::mutex> lock(g_DeviceQueuesMutex);
          // If we already have a queue for this device, release it
          if (g_DeviceQueues.count(dev)) g_DeviceQueues[dev]->Release();
          g_DeviceQueues[dev] = queue;
          queue->AddRef(); // One for the map
          
          if (g_Device) g_Device->Release();
          g_Device = dev; // Keep the reference from GetDevice
          
          // FG: Cleanup dedicated overlay queue if device changed
          if (g_OverlayQueueInitialized) {
              EarlyLog("DX12 FG: Device changed during swapchain creation, cleaning up overlay queue");
              CleanupDedicatedOverlayQueue();
          }
      }
      HookLog("CreateSwapChainForHwnd #%d: Captured Queue %p", swapCount, g_CommandQueue);
    }
    g_FGCompat.OnSwapchainRecreation(); // Notify FG detection (triggers suspend)
  }

  DXGI_SWAP_CHAIN_DESC1 modifiedDesc = *pDesc;
  {
      const GraphicsConfig& gfx = GetActiveGraphicsConfig();
      // Backbuffer Count
      int count = gfx.backbufferCount;
      if (count >= 2 && count <= 6) {
          modifiedDesc.BufferCount = (UINT)count;
          HookLog("DX12/DXGI: CreateSwapChainForHwnd: Overriding BufferCount to %d", count);
      }
      
      // MSAA Override (ONLY for D3D11/10/9 devices, D3D12 MUST be 1x)
      ID3D12Device* d12Dev = nullptr;
      bool isD12 = (pDevice && SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12Device), (void**)&d12Dev)));
      if (d12Dev) d12Dev->Release();

      if (!isD12) {
          const char* msaa = gfx.msaaSamples.c_str();
          if (msaa[0] != 'd') {
              if (strcmp(msaa, "off") == 0) {
                  modifiedDesc.SampleDesc.Count = 1;
                  modifiedDesc.SampleDesc.Quality = 0;
                  HookLog("DX11/DXGI: CreateSwapChainForHwnd: Forcing MSAA OFF");
              } else {
                  UINT samples = 1;
                  if (strcmp(msaa, "2x") == 0) samples = 2;
                  else if (strcmp(msaa, "4x") == 0) samples = 4;
                  else if (strcmp(msaa, "8x") == 0) samples = 8;
                  
                  if (samples > 1) {
                      modifiedDesc.SampleDesc.Count = samples;
                      modifiedDesc.SampleDesc.Quality = 0;
                      modifiedDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; // Required for MSAA
                      HookLog("DX11/DXGI: CreateSwapChainForHwnd: Forcing MSAA %dx", samples);
                  }
              }
          }
      } 
  }

  HRESULT hr = oCreateSwapChainForHwnd(pThis, pDevice, hWnd, &modifiedDesc, pFullscreenDesc,
                                 pRestrictToOutput, ppSwapChain);
                                 
  if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
       // Capture g_LastSwapChain immediately to prevent false-positive "Device Changed" detection
       IDXGISwapChain3 *swapChain3 = nullptr;
       if (SUCCEEDED((*ppSwapChain)->QueryInterface(__uuidof(IDXGISwapChain3), (void **)&swapChain3))) {
           // Weak pointer only (do not affect refcounts)
           g_LastSwapChain = swapChain3;
           swapChain3->Release();
           
           // Release local if we did not transfer ownership into g_LastSwapChain
           // (If assigned above, g_LastSwapChain holds the QI ref and we must not Release here)
           // In this function, we always transfer ownership, so null out local.
           swapChain3 = nullptr;

           bool useWrapper = false; 

           // IMPORTANT: If we didn't wrap, we MUST hook the vtable of the returned swapchain
           // because it might be a Streamline Proxy (different vtable than the one we hooked in Init).
           if (!useWrapper && *ppSwapChain) {
               void** vtbl = *(void***)(*ppSwapChain);
               
                // Attempt to hook Present (Index 8)
                if (vtbl[8] != (void*)DetourPresent) {
                    void* tempO = nullptr;
                    MH_STATUS s = MH_CreateHook(&vtbl[8], (LPVOID)DetourPresent, (LPVOID*)&tempO);
                    if (s == MH_OK) {
                        EarlyLog("DX12: Hooked specific SwapChain Present (vtbl[8]=%p)", vtbl[8]);
                    }
                }
               
                // Attempt to hook Present1 (Index 22)
                if (vtbl[22] != (void*)DetourPresent1) {
                    void* tempO1 = nullptr;
                    MH_STATUS s = MH_CreateHook(&vtbl[22], (LPVOID)DetourPresent1, (LPVOID*)&tempO1);
                    if (s == MH_OK) {
                        EarlyLog("DX12: Hooked specific SwapChain Present1 (vtbl[22]=%p)", vtbl[22]);
                    }
                }
           }
       }
  }
  
  return hr;
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForCoreWindow(
    IDXGIFactory2 *pThis, IUnknown *pDevice, IUnknown *pWindow,
    const DXGI_SWAP_CHAIN_DESC1 *pDesc, IDXGIOutput *pRestrictToOutput,
    IDXGISwapChain1 **ppSwapChain) {
    EarlyLog("DX12: DetourCreateSwapChainForCoreWindow called (pDevice=%p, pWindow=%p)", pDevice, pWindow);
    if (pDevice) {
        ID3D12CommandQueue *queue = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue), (void **)&queue))) {
            if (g_CommandQueue) g_CommandQueue->Release();
            g_CommandQueue = queue;
            g_IsQueueFromSwapchain = true;
            EarlyLog("DX12: DetourCreateSwapChainForCoreWindow: Auth Queue %p captured", g_CommandQueue);
        }
        g_FGCompat.OnSwapchainRecreation();
    }
    return oCreateSwapChainForCoreWindow(pThis, pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForComposition(
    IDXGIFactory2 *pThis, IUnknown *pDevice, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
    IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) {
    EarlyLog("DX12: DetourCreateSwapChainForComposition called (pDevice=%p)", pDevice);
    if (pDevice) {
        ID3D12CommandQueue *queue = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue), (void **)&queue))) {
            if (g_CommandQueue) g_CommandQueue->Release();
            g_CommandQueue = queue;
            g_IsQueueFromSwapchain = true;
            EarlyLog("DX12: DetourCreateSwapChainForComposition: Auth Queue %p captured", g_CommandQueue);
        }
        g_FGCompat.OnSwapchainRecreation();
    }
    return oCreateSwapChainForComposition(pThis, pDevice, pDesc, pRestrictToOutput, ppSwapChain);
}

HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain3 *pSwapChain,
                                              UINT BufferCount, UINT Width,
                                              UINT Height,
                                              DXGI_FORMAT NewFormat,
                                              UINT SwapChainFlags) {
  if (!pSwapChain) return DXGI_ERROR_INVALID_CALL;

  HookLog("DX12: DetourResizeBuffers entering...");
  std::lock_guard<std::mutex> lock(g_OverlayMutex);
  HookLog("DX12: DetourResizeBuffers lock acquired");
  g_FGCompat.OnSwapchainRecreation(); // Notify FG detection
  CleanupOverlay();
  HookLog("DX12: DetourResizeBuffers CleanupOverlay done");
  CleanupRTVs();
  HookLog("DX12: DetourResizeBuffers CleanupRTVs done");
  
  // Release capture resources (backbuffer references)
  {
      std::lock_guard<std::mutex> lock(g_DX12CaptureMutex);
      g_DX12Capture.Cleanup();
  }
  HookLog("DX12: DetourResizeBuffers Capture Cleanup done");

  if (g_State.imGuiInit)
    ImGui_ImplDX12_InvalidateDeviceObjects();

  int count = GetActiveGraphicsConfig().backbufferCount;
  if (count >= 2 && count <= 6) {
      BufferCount = (UINT)count;
      HookLog("DX12: ResizeBuffers: Overriding BufferCount to %d", count);
  }

  HookLog("DX12: Calling original ResizeBuffers...");
  HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat,
                               SwapChainFlags);
  HookLog("DX12: original ResizeBuffers returned 0x%08X", hr);

  return hr;
}

DWORD WINAPI UnloadThread(LPVOID lpParam) {
  // Basic unload logic
  g_DX12Capture.StopCaptureThread();
  Sleep(200);
  g_DX12Hook.Shutdown(); // triggers MH_Disable
  return 0;
}

typedef HRESULT (WINAPI *PFN_D3D12_CREATE_DEVICE)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
static PFN_D3D12_CREATE_DEVICE oD3D12CreateDevice = nullptr;

HRESULT WINAPI DetourD3D12CreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid, void** ppDevice) {
    HRESULT hr = oD3D12CreateDevice(pAdapter, MinimumFeatureLevel, riid, ppDevice);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        HookLog("DX12: Capturing Device via D3D12CreateDevice hook");
        ID3D12Device* dev = (ID3D12Device*)*ppDevice;
        LUID luid = dev->GetAdapterLuid();
        ReportLUID(luid.LowPart, luid.HighPart);

        void** vtbl = *reinterpret_cast<void***>(dev);
        
        static bool hookedSampler = false;
        if (!hookedSampler) {
            // Index 22 is CreateSampler in ID3D12Device
            MH_STATUS s = MH_CreateHook(&vtbl[22], (LPVOID)DetourCreateSampler, (LPVOID*)&oCreateSampler);
            if (s == MH_OK) {
                HookLog("DX12: Hooked CreateSampler (early export)");
                hookedSampler = true;
            } else if (s == MH_ERROR_ALREADY_CREATED) {
                 hookedSampler = true;
            }
        }
        
        static bool hookedCreateResource = false;
        if (!hookedCreateResource) {
            // Index 27 is CreateCommittedResource
            MH_STATUS s = MH_CreateHook(&vtbl[27], (LPVOID)DetourCreateCommittedResource, (LPVOID*)&oCreateCommittedResource);
            if (s == MH_OK) {
                HookLog("DX12: Hooked CreateCommittedResource");
                hookedCreateResource = true;
            } else if (s == MH_ERROR_ALREADY_CREATED) {
                 hookedCreateResource = true;
            }
        }
    }
    return hr;
}

// DX12Hook Implementation
void DX12Hook::Init() {
  EarlyLog("DX12Hook::Init() called - installing hooks");

  WNDCLASSEX wc = {sizeof(WNDCLASSEX),    CS_CLASSDC, DefWindowProc, 0L,   0L,
                   GetModuleHandle(NULL), NULL,       NULL,          NULL, NULL,
                   "DX12Dummy",           NULL};
  RegisterClassEx(&wc);
  HWND hWnd =
      CreateWindow(wc.lpszClassName, "DX12 Capture Hook", WS_OVERLAPPEDWINDOW,
                   100, 100, 1280, 720, NULL, NULL, wc.hInstance, NULL);
  if (!hWnd)
    return;

  D3D12_COMMAND_QUEUE_DESC queueDesc = {};
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

  ID3D12Device *d3d12Device = nullptr;
  ID3D12CommandQueue *commandQueue = nullptr;
  IDXGISwapChain3 *swapChain = nullptr;

  HMODULE hD3D12 = LoadLibraryA("d3d12.dll");
  HMODULE hDXGI = LoadLibraryA("dxgi.dll");
  if (!hD3D12 || !hDXGI) {
    HookLog("DX12: D3D12 or DXGI DLLs not found. Skipping DX12 hook.");
    return;
  }
  
  // Hook D3D12CreateDevice export to catch early device creation
  MH_STATUS s_dev = MH_CreateHookApi(L"d3d12.dll", "D3D12CreateDevice", (LPVOID)DetourD3D12CreateDevice, (LPVOID*)&oD3D12CreateDevice);
  if (s_dev == MH_OK) {
      MH_EnableHook((LPVOID)oD3D12CreateDevice);
      HookLog("DX12: Hooked D3D12CreateDevice export.");
  } else {
      HookLog("DX12: Failed to hook D3D12CreateDevice export: %s", MH_StatusToString(s_dev));
  }

  typedef HRESULT (WINAPI *PFN_D3D12_CREATE_DEVICE)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
  typedef HRESULT (WINAPI *PFN_CREATE_DXGI_FACTORY1)(REFIID, void**);

  PFN_D3D12_CREATE_DEVICE pD3D12CreateDevice = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(hD3D12, "D3D12CreateDevice");
  PFN_CREATE_DXGI_FACTORY1 pCreateDXGIFactory1 = (PFN_CREATE_DXGI_FACTORY1)GetProcAddress(hDXGI, "CreateDXGIFactory1");

  if (!pD3D12CreateDevice || !pCreateDXGIFactory1) {
    HookLog("DX12: Failed to find creation entry points.");
    return;
  }

  pD3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12Device));
  if (!d3d12Device) {
      HookLog("DX12: Failed to create dummy device.");
      return;
  }
  d3d12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue));

  IDXGIFactory4 *factory = nullptr;
  pCreateDXGIFactory1(IID_PPV_ARGS(&factory));

  DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
  swapChainDesc.BufferCount = 2;
  swapChainDesc.Width = 1280;
  swapChainDesc.Height = 720;
  swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  swapChainDesc.SampleDesc.Count = 1;

  IDXGISwapChain1 *tempSwapChain = nullptr;
  factory->CreateSwapChainForHwnd(commandQueue, hWnd, &swapChainDesc, NULL,
                                  NULL, &tempSwapChain);
  swapChain = (IDXGISwapChain3 *)tempSwapChain;

  void **scVTable = *reinterpret_cast<void ***>(swapChain);
  void **cqVTable = *reinterpret_cast<void ***>(commandQueue);
  void **facVTable = *reinterpret_cast<void ***>(factory);
  void **devVTable = *reinterpret_cast<void ***>(d3d12Device);
  
  HookLog("DX12: Dummy SwapChain VTable: %p", scVTable);
  HookLog("DX12: Dummy Present Address: %p", scVTable[8]);
  HookLog("DX12: Dummy ResizeBuffers Address: %p", scVTable[13]);

  MH_STATUS s;
  s = MH_CreateHook(&facVTable[10], (LPVOID)DetourCreateSwapChain,
                (LPVOID *)&oCreateSwapChain);
  if (s != MH_OK) HookLog("Failed to hook CreateSwapChain: %s", MH_StatusToString(s));

  s = MH_CreateHook(&facVTable[15], (LPVOID)DetourCreateSwapChainForHwnd,
                (LPVOID *)&oCreateSwapChainForHwnd);
  if (s != MH_OK) HookLog("Failed to hook CreateSwapChainForHwnd: %s", MH_StatusToString(s));

  s = MH_CreateHook(&facVTable[16], (LPVOID)DetourCreateSwapChainForCoreWindow,
                (LPVOID *)&oCreateSwapChainForCoreWindow);
  if (s != MH_OK) HookLog("Failed to hook CreateSwapChainForCoreWindow: %s", MH_StatusToString(s));

  s = MH_CreateHook(&facVTable[24], (LPVOID)DetourCreateSwapChainForComposition,
                (LPVOID *)&oCreateSwapChainForComposition);
  if (s != MH_OK) HookLog("Failed to hook CreateSwapChainForComposition: %s", MH_StatusToString(s));
  
  s = MH_CreateHook(&scVTable[9], (LPVOID)DetourGetBuffer, (LPVOID *)&oGetBuffer);
  if (s != MH_OK) HookLog("Failed to hook GetBuffer: %s", MH_StatusToString(s));

  // Check for FG runtime BEFORE installing Present hooks
  // FG runtimes (DLSS-G, FSR-FG) create proxy swapchains - hooking Present on dummy causes crashes
  auto fgType = g_FGCompat.DetectLoadedFGRuntime();
  bool skipPresentHooks = (fgType != FGCompatibility::FGType::None);
  
  if (skipPresentHooks) {
      HookLog("DX12Hook: FG Runtime detected (%d) - SKIPPING Present/ResizeBuffers hooks (use CreateSwapChainForHwnd hook instead)", (int)fgType);
      g_FGCompat.SuspendFor(100); // Minimal suspend - swapchain wrapper handles FG safely
  } else {
      s = MH_CreateHook(&scVTable[8], (LPVOID)DetourPresent, (LPVOID *)&oPresent);
      if (s == MH_OK) HookLog("DX12: Present hook installed (vtable[8]=%p)", scVTable[8]);
      else HookLog("DX12: Failed to hook Present: %s", MH_StatusToString(s));
      
      s = MH_CreateHook(&scVTable[22], (LPVOID)DetourPresent1, (LPVOID *)&oPresent1);
      if (s == MH_OK) HookLog("DX12: Present1 hook installed (vtable[22]=%p)", scVTable[22]);
      else HookLog("DX12: Failed to hook Present1: %s", MH_StatusToString(s));
      
      s = MH_CreateHook(&scVTable[13], (LPVOID)DetourResizeBuffers,
                    (LPVOID *)&oResizeBuffers);
      if (s != MH_OK) HookLog("Failed to hook ResizeBuffers: %s", MH_StatusToString(s));
  }
  
  // ExecuteCommandLists is safe to hook even with FG
  s = MH_CreateHook(&cqVTable[10], (LPVOID)DetourExecuteCommandLists,
                (LPVOID *)&oExecuteCommandLists);
  if (s != MH_OK) HookLog("Failed to hook ExecuteCommandLists: %s", MH_StatusToString(s));
  
  // Hook CreateSampler (Index 22)
  MH_CreateHook(&devVTable[22], (LPVOID)DetourCreateSampler, (LPVOID *)&oCreateSampler);

  // Hook Root Signature Serialization (export hooks for Static Sampler override)
  // These are the key to forcing AF in DX12 games that use static samplers
  void* pSerializeRootSig = (void*)GetProcAddress(hD3D12, "D3D12SerializeRootSignature");
  void* pSerializeVersionedRootSig = (void*)GetProcAddress(hD3D12, "D3D12SerializeVersionedRootSignature");
  
  if (pSerializeRootSig) {
      s = MH_CreateHook(pSerializeRootSig, (LPVOID)DetourSerializeRootSignature, (LPVOID*)&oSerializeRootSignature);
      if (s == MH_OK) {
          MH_EnableHook(pSerializeRootSig);
          HookLog("DX12: Hooked D3D12SerializeRootSignature");
      } else {
          HookLog("DX12: Failed to hook D3D12SerializeRootSignature: %s", MH_StatusToString(s));
      }
  }
  
  if (pSerializeVersionedRootSig) {
      s = MH_CreateHook(pSerializeVersionedRootSig, (LPVOID)DetourSerializeVersionedRootSignature, (LPVOID*)&oSerializeVersionedRootSignature);
      if (s == MH_OK) {
          MH_EnableHook(pSerializeVersionedRootSig);
          HookLog("DX12: Hooked D3D12SerializeVersionedRootSignature");
      } else {
          HookLog("DX12: Failed to hook D3D12SerializeVersionedRootSignature: %s", MH_StatusToString(s));
      }
  }

  // FG detection now happens earlier - before Present hooks are installed
  MH_EnableHook(MH_ALL_HOOKS);
  HookLog("DX12Hook: MinHook enabled.");

  if (d3d12Device)
    d3d12Device->Release();
  if (commandQueue)
    commandQueue->Release();
  if (swapChain)
    swapChain->Release();
  if (factory)
    factory->Release();
  DestroyWindow(hWnd);
}

// Helper function callable from dx11_hook.cpp to install DX12 hooks on actual game swapchain
// This is needed because UE5/DLSS games create swapchains via DXGI which triggers DX11 hooks first
bool InstallDX12HooksOnSwapchain(IDXGISwapChain3* pSwapChain) {
    if (!pSwapChain) return false;
    
    void** scVTable = *(void***)pSwapChain;
    bool anyInstalled = false;
    
    if (oPresent == nullptr) {
        MH_STATUS s = MH_CreateHook(&scVTable[8], (LPVOID)DetourPresent, (LPVOID*)&oPresent);
        if (s == MH_OK) {
            HookLog("DX12: Present hook installed on game swapchain (vtable[8]=%p)", scVTable[8]);
            anyInstalled = true;
        } else {
            HookLog("DX12: Failed to hook Present on game swapchain: %s", MH_StatusToString(s));
        }
    }
    
    if (oPresent1 == nullptr) {
        MH_STATUS s = MH_CreateHook(&scVTable[22], (LPVOID)DetourPresent1, (LPVOID*)&oPresent1);
        if (s == MH_OK) {
            HookLog("DX12: Present1 hook installed on game swapchain (vtable[22]=%p)", scVTable[22]);
            anyInstalled = true;
        } else {
            HookLog("DX12: Failed to hook Present1 on game swapchain: %s", MH_StatusToString(s));
        }
    }
    
    if (oResizeBuffers == nullptr) {
        MH_STATUS s = MH_CreateHook(&scVTable[13], (LPVOID)DetourResizeBuffers, (LPVOID*)&oResizeBuffers);
        if (s == MH_OK) {
            HookLog("DX12: ResizeBuffers hook installed on game swapchain");
            anyInstalled = true;
        }
    }
    
    return anyInstalled;
}

void DX12Hook::Shutdown() {
  HookLog("DX12Hook::Shutdown()");
  
  // First disable hooks to stop new frames from coming in
  MH_DisableHook(MH_ALL_HOOKS);
  
  // Stop capture thread and wait for it to finish
  if (g_DX12Capture.captureThreadRunning) {
    // Signal shutdown - this is what the thread actually checks
    g_DX12Capture.captureThreadShutdown = true;
    
    // Wake up the capture thread if it's waiting on the event
    if (g_DX12Capture.captureEvent) {
      SetEvent(g_DX12Capture.captureEvent);
    }
    
    // Give thread time to exit
    Sleep(100);
  }
  
  // Wait for GPU to finish all pending work
  if (g_CommandQueue && g_DX12Capture.fence) {
    UINT64 waitValue = g_DX12Capture.fenceValue + 1;
    g_CommandQueue->Signal(g_DX12Capture.fence, waitValue);
    if (g_DX12Capture.fence->GetCompletedValue() < waitValue) {
      HANDLE waitEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (waitEvent) {
        g_DX12Capture.fence->SetEventOnCompletion(waitValue, waitEvent);
        WaitForSingleObject(waitEvent, 1000); // Max 1 second wait
        CloseHandle(waitEvent);
      }
    }
  }
  
  // Shutdown ImGui before releasing D3D12 resources
  ShutdownImGui();
  
  // Release overlay resources
  CleanupOverlay();
  
  // Release descriptor heaps and back buffers
  CleanupRTVs();

  // Release captured queues map
  {
      std::lock_guard<std::mutex> lock(g_DeviceQueuesMutex);
      for (auto& pair : g_DeviceQueues) {
          if (pair.second) pair.second->Release();
      }
      g_DeviceQueues.clear();
  }

  // Release global queue and device
  if (g_CommandQueue) { g_CommandQueue->Release(); g_CommandQueue = nullptr; }
  if (g_Device) { g_Device->Release(); g_Device = nullptr; }
  
  if (g_LastSwapChain) { g_LastSwapChain = nullptr; }
  
  // Release capture resources
  g_DX12Capture.Cleanup();

  g_IPCReady = false;

  HookLog("DX12Hook shutdown complete");
}

void DX12Hook::OnHostDisconnect() {
  HookLog("DX12Hook::OnHostDisconnect() - stopping capture for reconnection");
  
  // Stop capture thread if running (host died mid-recording)
  if (g_DX12Capture.captureThreadRunning) {
    // Signal shutdown - this is what the thread actually checks
    g_DX12Capture.captureThreadShutdown = true;
    
    // Wake up the capture thread if it's waiting on the event
    if (g_DX12Capture.captureEvent) {
      SetEvent(g_DX12Capture.captureEvent);
    }
    
    // Give thread time to exit
    Sleep(100);
    
    // Reset flags for future recordings
    g_DX12Capture.captureThreadShutdown = false;
    g_DX12Capture.captureThreadRunning = false;
  }
  
  // Reset IPC ready flag so we re-establish connection
  g_IPCReady = false;
  
  HookLog("DX12Hook::OnHostDisconnect() complete - ready for reconnection");
}
