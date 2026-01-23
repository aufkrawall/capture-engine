#include "dx12_hook.h"
#include "dx11_hook.h"
#include "graphics_hook.h"
#include "../common/hook_common.h"
#include "../../common/raii_helpers.h"
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
#include "../common/input_manager.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"
#include "imgui.h"
#include "../wrappers/d3d12_wrapper_interface.h"
#include "../wrappers/wrapper_hooks.h"
#include "../capture/shared_capture.h" // Added for shared capture

#include "dxgi_shared.h"
#include "../wrappers/vtable_hook.h"

// --- DX12 Overlay State Management ---
struct DX12OverlayState {
    static const int ALLOC_POOL_SIZE = 3;
    
    // Command List & Allocation
    std::vector<ID3D12CommandAllocator*> allocators;
    ID3D12GraphicsCommandList* cmdList = nullptr;
    int allocIndex = 0;
    
    // Synchronization
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    UINT64 currentFenceValue = 0;
    std::vector<UINT64> fenceValues;
    
    // Resources
    ID3D12DescriptorHeap* rtvDescHeap = nullptr;
    ID3D12DescriptorHeap* srvDescHeap = nullptr;
    UINT rtvDescriptorSize = 0;
    std::vector<ID3D12Resource*> backBuffers;
    
    // Status
    bool imGuiInit = false;
    bool syncInit = false;
    int cachedWidth = 0;
    int cachedHeight = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    
    // Added missing members for compatibility
    UINT bufferCount = 0;
    IDXGISwapChain* cachedSwapChain = nullptr;
    
    void Cleanup() {
        for (auto& bb : backBuffers) if (bb) bb->Release();
        backBuffers.clear();
        
        if (rtvDescHeap) { rtvDescHeap->Release(); rtvDescHeap = nullptr; }
        if (srvDescHeap) { srvDescHeap->Release(); srvDescHeap = nullptr; }
        
        for (auto* alloc : allocators) {
            if (alloc) alloc->Release();
        }
        allocators.clear();
        
        if (cmdList) { cmdList->Release(); cmdList = nullptr; }
        if (fence) { fence->Release(); fence = nullptr; }
        if (fenceEvent) { CloseHandle(fenceEvent); fenceEvent = nullptr; }
        
        imGuiInit = false;
        syncInit = false;
    }
};

static DX12OverlayState g_State;
static std::mutex g_ImGuiFrameMutex;

// Global SharedCapture instance for DX12
static SharedCaptureD3D12 g_SharedCaptureD3D12;


// --- Restored Forward Declarations and Globals ---
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

static ExecuteCommandListsPtr oExecuteCommandLists = nullptr;
static CreateSamplerPtr oCreateSampler = nullptr;
static CreateCommittedResourcePtr oCreateCommittedResource = nullptr;
PFN_D3D12_SERIALIZE_ROOT_SIGNATURE oSerializeRootSignature = nullptr;
PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE oSerializeVersionedRootSignature = nullptr;

typedef HRESULT(STDMETHODCALLTYPE *GetBufferPtr)(IDXGISwapChain *, UINT, REFIID, void **);
static GetBufferPtr oGetBuffer = nullptr;

static std::mutex g_DeviceQueuesMutex;
static std::map<ID3D12Device*, ID3D12CommandQueue*> g_DeviceQueues;

static std::atomic<int> g_SwapchainCreationCount{0};

static constexpr uint64_t OVERLAY_INIT_GRACE_FRAMES = 100;

// Global DX12Hook instance
DX12Hook g_dx12HookInstance;

// --- Missing Globals Restored ---
ID3D12Device *g_Device = nullptr;
ID3D12CommandQueue *g_CommandQueue = nullptr;
std::mutex g_CommandQueueMutex;
bool g_IPCReady = false;
static IDXGISwapChain *g_LastSwapChain = nullptr;
static ID3D12Resource *g_DummyBackBuffer = nullptr;
static std::mutex g_OverlayMutex;
static std::mutex g_InitImGuiMutex;
static std::mutex g_DX12CaptureMutex;
static std::atomic<bool> g_InSwapchainResizeCleanup{false};

static std::atomic<bool> g_FGQueueLocked{false};
static std::mutex g_FGQueueLockMutex;
static ID3D12CommandQueue* g_FGLockedQueue = nullptr;

static std::atomic<bool> g_IsQueueFromSwapchain{false};
static std::atomic<bool> g_FGNeedsDrain{false};
static std::atomic<uint64_t> g_FGNextDrainAttemptUs{0};

static std::atomic<int> g_CommandListsExecutedThisFrame{0};
static std::chrono::steady_clock::time_point g_LastResourceCleanup;
static std::atomic<bool> g_DeviceRemovedFatal{false};
static uint64_t g_FGDebugOverlayDraws = 0;

// Prerender Limit
static ID3D12Fence* g_PrerenderFence = nullptr;
static HANDLE g_PrerenderEvent = nullptr;
static UINT64 g_PrerenderValue = 0;
static UINT64 g_PrerenderFrameIndex = 0;
static std::vector<UINT64> g_PrerenderHistory;

// Helper to adjust wrapper resize depth
void DX12_AdjustWrapperResizeDepth(int delta) {
    if (delta > 0) DXGIShared::g_SharedState.wrapperResizeDepth.fetch_add(delta);
    else DXGIShared::g_SharedState.wrapperResizeDepth.fetch_sub(-delta);
}

// Forward Declarations
void STDMETHODCALLTYPE DetourExecuteCommandLists(ID3D12CommandQueue *pThis, UINT NumCommandLists, ID3D12CommandList *const *ppCommandLists);
void HookQueueVTable(ID3D12CommandQueue* queue);

static std::atomic<uint64_t> g_FGDebugFrameCount{0};

// Called by DX11 hook BEFORE FSR4SwapchainProvider creates new swapchain
void DX12_InvalidateSwapchain() {
    DXGIShared::g_SharedState.swapchainInvalid.store(true, std::memory_order_release);
    HookLog("DX12: Swapchain marked INVALID - overlay will abort on next Present check");
}

// Called by DX11 hook when FSR4SwapchainProvider creates new swapchain
void DX12_SignalFSR4SwapchainRecreated() {
    DXGIShared::g_SharedState.fsr4RecreationPending.store(true, std::memory_order_release);
    HookLog("DX12: FSR4 swapchain recreation signaled (pending cleanup on DX12 thread)");
}

// Track if we're using an FSR3 swapchain
static bool g_UsingFSR3Swapchain = false;

extern "C" __declspec(dllexport) void DX12_SetCommandQueue(ID3D12CommandQueue* pQueue) {
    if (!pQueue) return;
    std::lock_guard<std::mutex> lock(g_CommandQueueMutex);
    if (g_CommandQueue != pQueue) {
        if (g_CommandQueue) g_CommandQueue->Release();
        g_CommandQueue = pQueue;
        g_CommandQueue->AddRef();
        HookLog("DX12: CommandQueue manually set to %p", g_CommandQueue);
    }
}

// Forward declaration for DrawOverlay
void DrawOverlay(ID3D12GraphicsCommandList* list);

// Forward declaration for simple overlay
bool InitSimpleOverlay(ID3D12Device* device);
void RenderSimpleOverlay(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* backBuffer);

// Forward declaration for external access
void ProcessFrame(IDXGISwapChain *pSwapChain, bool processCapture);

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
bool InitImGui(ID3D12Device *device, int buffers, DXGI_FORMAT format,
               HWND hwnd) {
  // Guard against double initialization with mutex to prevent concurrent calls
  std::lock_guard<std::mutex> lock(g_InitImGuiMutex);
  if (g_State.imGuiInit) {
      return true;
  }

  EarlyLog("DX12: InitImGui ENTRY - device=%p, buffers=%d, format=%u, hwnd=%p",
           device, buffers, (unsigned)format, hwnd);

  g_State.format = format;
  g_SharedOverlay.InitImGui(hwnd);
  // Hook Input
  InputManager::Get().HookWindow(hwnd);

  // DIAGNOSTIC CACHE: Try creating a non-shader visible heap first (sanity check)
  {
      D3D12_DESCRIPTOR_HEAP_DESC testDesc = {};
      testDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
      testDesc.NumDescriptors = 1;
      testDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
      ID3D12DescriptorHeap* pTestHeap = nullptr;
      EarlyLog("DX12: Diagnostic - Creating RTV Heap (Non-Shader Visible)...");
      HRESULT testHr = device->CreateDescriptorHeap(&testDesc, IID_PPV_ARGS(&pTestHeap));
      if (SUCCEEDED(testHr)) {
          EarlyLog("DX12: Diagnostic - RTV Heap (Non-Shader Visible) creation SUCCESS (heap=%p).", pTestHeap);
          pTestHeap->Release();
      } else {
          EarlyLog("DX12: Diagnostic - RTV Heap (Non-Shader Visible) creation FAILED (hr=0x%08X).", testHr);
          if (testHr == DXGI_ERROR_DEVICE_REMOVED) {
              EarlyLog("DX12: Diagnostic - Device Removed Reason: 0x%08X", device->GetDeviceRemovedReason());
          }
      }
  }

  D3D12_DESCRIPTOR_HEAP_DESC desc = {};
  desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  desc.NumDescriptors = 64; // Increase size to avoid alignment issues
  desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

  // Handle Multi-Adapter/Linked-GPU nodes
  UINT nodeCount = device->GetNodeCount();
  EarlyLog("DX12: InitImGui - device->GetNodeCount() = %u", nodeCount);

  if (nodeCount > 1) {
      desc.NodeMask = 1; // Explicitly target Node 0
      EarlyLog("DX12: Multi-Node Device detected (Count=%u). Forcing NodeMask=1 for SRV Heap.", nodeCount);
  } else {
      desc.NodeMask = 0; // Default for single node (try 0)
  }

  EarlyLog("DX12: InitImGui - Creating SRV Heap (Type=%d, Num=%d, Flags=%d, NodeMask=%u)...",
           desc.Type, desc.NumDescriptors, desc.Flags, desc.NodeMask);

  HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_State.srvDescHeap));

  EarlyLog("DX12: InitImGui - CreateDescriptorHeap returned hr=0x%08X, heap=%p",
           hr, g_State.srvDescHeap);

  // FALLBACK: If NodeMask 0 failed on single node, try 1
  if (FAILED(hr) && nodeCount <= 1) {
       EarlyLog("DX12: SRV Heap creation failed with NodeMask=0. Retrying with NodeMask=1...");
       desc.NodeMask = 1;
       hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_State.srvDescHeap));
       EarlyLog("DX12: InitImGui - Retry with NodeMask=1 returned hr=0x%08X", hr);
  }

  if (FAILED(hr)) {
      EarlyLog("DX12: InitImGui - FAILED to create SRV heap (hr=0x%08X)", hr);

      if (hr == DXGI_ERROR_DEVICE_REMOVED) {
          HRESULT devRemovedReason = device->GetDeviceRemovedReason();
          EarlyLog("DX12: Device Removed Reason: 0x%08X", devRemovedReason);
          // Set fatal flag to stop trying - this device is in a bad state
          g_DeviceRemovedFatal.store(true);
          EarlyLog("DX12: Setting permanent failure flag - device removed, stopping overlay init");
      }

      // Try diagnostics
      D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
      if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))) {
         EarlyLog("DX12: ResourceBindingTier: %d", options.ResourceBindingTier);
      }

      return false;
  }
    
  bool initResult = ImGui_ImplDX12_Init(device, buffers, format, g_State.srvDescHeap,
                      g_State.srvDescHeap->GetCPUDescriptorHandleForHeapStart(),
                      g_State.srvDescHeap->GetGPUDescriptorHandleForHeapStart());

  if (!initResult) {
      EarlyLog("DX12: InitImGui - ImGui_ImplDX12_Init returned FALSE");
      // Cleanup the heap we just created so we can try again later
      g_State.srvDescHeap->Release();
      g_State.srvDescHeap = nullptr;
      return false;
  }
  
  if (g_CommandQueue) {
    ImGui_ImplDX12_SetCommandQueue(g_CommandQueue);
  }
  
  g_State.imGuiInit = true;
  EarlyLog("DX12: ImGui initialized (Queue=%p)", g_CommandQueue);
  return true;
}

void DrawOverlay(ID3D12GraphicsCommandList *cmdList) {
  if (!g_State.imGuiInit || !cmdList)
    return;

  ImGui_ImplDX12_NewFrame();
  g_SharedOverlay.BeginFrame();

  // Use shared overlay
  g_SharedOverlay.SetMetrics(DXGIShared::GetPerformanceMetrics());
  g_SharedOverlay.SetIPCClient(g_IPC);
  g_SharedOverlay.SetDroppedFrames(0); // Legacy dropped frames removed
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

// Replaces CheckCaptureInit
void CheckCaptureInit(IDXGISwapChain3 *pSwapChain) {
  if (g_SharedCaptureD3D12.IsActive())
    return;
    
  std::lock_guard<std::mutex> lock(g_DX12CaptureMutex);
  if (g_SharedCaptureD3D12.IsActive())
    return;

  IUnknown* pTempUnk = nullptr;
  if (FAILED(pSwapChain->GetDevice(IID_PPV_ARGS(&pTempUnk))))
    return;
  
  ID3D12Device* activeDevice = nullptr;
  ID3D12CommandQueue* activeQueue = nullptr;
  // Get Queue first as it is common in D3D12
  if (SUCCEEDED(pTempUnk->QueryInterface(IID_PPV_ARGS(&activeQueue)))) {
      if (SUCCEEDED(activeQueue->GetDevice(IID_PPV_ARGS(&activeDevice)))) {
          // Update global device if needed
          if (activeDevice != g_Device) {
             if (g_Device) g_Device->Release();
             g_Device = activeDevice;
             g_Device->AddRef();
          }
          activeDevice->Release();
      }
      activeQueue->Release();
  } else if (SUCCEEDED(pTempUnk->QueryInterface(IID_PPV_ARGS(&activeDevice)))) {
      if (g_Device) g_Device->Release();
      g_Device = activeDevice;
      g_Device->AddRef();
      activeDevice->Release();
  }
  pTempUnk->Release();
  if (!g_Device) return;

  // Initialize Shared Capture
  // Use g_CommandQueue (which should be set by now via ProcessFrame)
      if (g_CommandQueue) {
          g_SharedCaptureD3D12.Initialize(g_Device, pSwapChain);
          HookLog("SharedCaptureD3D12 Initialized.");
      }
}


void CreateRTVs(ID3D12Device *device, IDXGISwapChain3 *swapChain,
                int bufferCount) {
  if (g_State.rtvDescHeap)
    return; // Cleanup usually handles checks
  D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
  rtvHeapDesc.NumDescriptors = bufferCount;
  rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  HRESULT hr = device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_State.rtvDescHeap));
  if (FAILED(hr)) {
      HookLog("DX12: CreateRTVs - Failed to create RTV descriptor heap (hr=0x%08X)", hr);
      return;
  }
  HookLog("DX12: CreateRTVs - Created RTV heap %p for %d buffers", g_State.rtvDescHeap, bufferCount);
  
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

   static uint64_t lastLogTime = 0;
   uint64_t now = GetTickCount64();
   bool shouldLog = (now - lastLogTime > 2000); // Log every 2 seconds
   
   if (shouldLog) {
       static DWORD entryThreadId = 0;
       entryThreadId = GetCurrentThreadId();
       HookLog("DX12: [T:%04X] ========== DX12_ProcessFrameExternal ENTRY ==========", entryThreadId);
       lastLogTime = now;
   }

   static int64_t qpcFreq = 0;
   if (qpcFreq == 0) {
     LARGE_INTEGER f;
     QueryPerformanceFrequency(&f);
     qpcFreq = f.QuadPart;
   }

   LARGE_INTEGER qpc;
   QueryPerformanceCounter(&qpc);
   int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;
   if (auto* m = DXGIShared::GetPerformanceMetrics()) {
       m->Update(us);
       if (g_IPC) m->SetRecording(g_IPC->IsRecording());
   }

  IDXGISwapChain3* sc3 = nullptr;
  if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) {
    return;
  }

  // SELF-HEALING: If command queue is missing, try to capture it from the swapchain
  if (!g_CommandQueue) {
      ID3D12CommandQueue* queue = nullptr;
      if (SUCCEEDED(sc3->GetDevice(IID_PPV_ARGS(&queue)))) {
          HookLog("DX12: ProcessFrameExternal: Self-healing captured Command Queue %p", queue);
          DX12_SetCommandQueue(queue);
          queue->Release();
      }
  }

  // Record frame stats for FG logic (CRITICAL: This path is used when DX11 hook handles Present)
  int cmdListCount = g_CommandListsExecutedThisFrame.exchange(0);
  
  // Increment frame count to keep FG detection logic working if we use it later
  // Logging removed to prevent IO saturation crash during high-frequency Present calls
  g_FGDebugFrameCount++;
  
  g_FGCompat.RecordFrame(cmdListCount);

  ProcessFrame(sc3, cmdListCount > 0);
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
// FG-SAFE DEDICATED OVERLAY QUEUE - REMOVED
// ============================================================================

// Helper to drain the command queue
void DrainCommandQueue(ID3D12CommandQueue* queue, ID3D12Device* device) {
    if (!queue || !device) return;
    
    ID3D12Fence* fence = nullptr;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return;
    
    HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (fenceEvent) {
        if (SUCCEEDED(queue->Signal(fence, 1))) {
            if (fence->SetEventOnCompletion(1, fenceEvent) == S_OK) {
                WaitForSingleObject(fenceEvent, 2000);
            }
        }
        CloseHandle(fenceEvent);
    }
    fence->Release();
}

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
  
  g_State.currentFenceValue = 0;
  g_State.allocIndex = 0;
  g_State.syncInit = false;
  
  ShutdownImGui();
  
  // g_State.imGuiInitFrameCounter = 0; // Removed
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

  DXGIShared::g_SharedState.lastSwapchainCreation = std::chrono::steady_clock::now();
  DXGIShared::g_SharedState.fgSwapchainStabilized.store(false);

  std::lock_guard<std::mutex> lock(g_OverlayMutex);
  // Lock held until function exit
  // This ensures we wait for any active doOverlay() to finish before draining/cleanup.

  // CRITICAL: Drain command queue BEFORE releasing any resources
  // This ensures all pending GPU work is complete (like SpecialK's drain_queue)
  EarlyLog("DX12: DX12_OnSwapchainResizeBegin - draining command queue before cleanup");
  
  ID3D12CommandQueue* queueToDrain = nullptr;
  {
      std::lock_guard<std::mutex> qLock(g_CommandQueueMutex);
      if (g_CommandQueue) {
          queueToDrain = g_CommandQueue;
          queueToDrain->AddRef();
      }
  }
  
  if (queueToDrain && g_Device) {
      DrainCommandQueue(queueToDrain, g_Device);
      queueToDrain->Release();
  } else if (queueToDrain) {
      queueToDrain->Release();
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
    g_SharedCaptureD3D12.Reset();
  }

  if (g_State.imGuiInit) {
    ImGui_ImplDX12_InvalidateDeviceObjects();
  }

  g_InSwapchainResizeCleanup.store(false);
}

// AsyncCaptureThreadProc removed.

void ProcessFrame(IDXGISwapChain *pSwapChain, bool processCapture) {
  if (!pSwapChain) return;
  
  static uint64_t lastLogTime = 0;
  uint64_t now = GetTickCount64();
  bool shouldLog = (now - lastLogTime > 2000); // Log every 2 seconds

  // CRASH CONTEXT: Log entry with thread ID
  if (shouldLog) {
      static DWORD entryThreadId = 0;
      entryThreadId = GetCurrentThreadId();
      HookLog("DX12: [T:%04X] ========== ProcessFrame ENTRY ==========", entryThreadId);
  }

  // Vulkan coordination: Skip DX12 overlay if Vulkan Layer is active AND presenting.
  // This handles "Vulkan on DXGI" cases where Vulkan presents via DXGI on a different thread.
  if (g_pSharedMem) {
      uint64_t lastVulkan = g_pSharedMem->runtimeState.vulkanPresentTick.load(std::memory_order_acquire);
      if (g_pSharedMem->runtimeState.vulkanLayerActive && (GetTickCount64() - lastVulkan < 200)) {
          return;
      }
  }

  // LOCK HIERARCHY: g_OverlayMutex -> g_DX12CaptureMutex
  std::lock_guard<std::mutex> lock(g_OverlayMutex);

  if (shouldLog) {
      HookLog("DX12: ProcessFrame Entry (pSwapChain=%p, processCapture=%d)", pSwapChain, processCapture);
      lastLogTime = now;
  }

  // FG STATE detection
  bool fgActive = (g_FGCompat.DetectLoadedFGRuntime() != FGCompatibility::FGType::None);
  bool fgSuspended = g_FGCompat.IsSuspended();
  
  // FG SAFETY: When FG active + suspended, skip ALL operations entirely
  if (fgActive && fgSuspended) {
      return;
  }

  // 1. Determine active device and detect change
  IUnknown* pTempUnk = nullptr;
  if (!pSwapChain || FAILED(pSwapChain->GetDevice(IID_PPV_ARGS(&pTempUnk)))) return;

  ID3D12Device* activeDevice = nullptr;
  ID3D12CommandQueue* activeQueue = nullptr;
  
  if (SUCCEEDED(pTempUnk->QueryInterface(IID_PPV_ARGS(&activeQueue)))) {
      if (SUCCEEDED(activeQueue->GetDevice(IID_PPV_ARGS(&activeDevice)))) {
          HookQueueVTable(activeQueue);
      }
      activeQueue->Release();
  } else {
      pTempUnk->QueryInterface(IID_PPV_ARGS(&activeDevice));
  }
  pTempUnk->Release();

   if (!activeDevice) {
       if (shouldLog) HookLog("DX12: ProcessFrame - Failed to get activeDevice from swapchain");
       return;
   }
   
   // DEBUG: Log device and adapter info
   LUID luid = activeDevice->GetAdapterLuid();
   if (shouldLog) {
       HookLog("DX12: DEBUG - Device=%p, LUID=%08x-%08x, NodeCount=%u",
               activeDevice, luid.HighPart, luid.LowPart, activeDevice->GetNodeCount());
   }
   
   // Initialize System Metrics with correct GPU LUID
   SystemMetricsCollector::Get().Initialize(luid.LowPart, luid.HighPart);
   
   // Try to upgrade to SC3 internally
   IDXGISwapChain3* sc3 = nullptr;
   pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3));
   if (!sc3) {
       if (shouldLog) HookLog("DX12: ProcessFrame - Swapchain is not IDXGISwapChain3 - some features may be degraded");
   } else {
       if (shouldLog && !g_State.imGuiInit) HookLog("DX12: ProcessFrame - Valid IDXGISwapChain3 detected: %p", sc3);
   }
  
  bool isInitialSetup = (g_Device == nullptr);
  bool deviceChanged = (!isInitialSetup && (activeDevice != g_Device || pSwapChain != g_LastSwapChain));
  
  if (deviceChanged) {
      EarlyLog("DX12: ProcessFrame - Device or Swapchain change detected! (Old SC=%p, New SC=%p)", g_LastSwapChain, pSwapChain);
      g_FGCompat.OnDeviceChange(); 
      CleanupOverlay();
      CleanupRTVs();
      ShutdownImGui();
      {
          std::lock_guard<std::mutex> capLock(g_DX12CaptureMutex);
          if (g_SharedCaptureD3D12.IsActive()) {
              g_SharedCaptureD3D12.Reset();
          }
      }
      
      if (g_Device) g_Device->Release();
      g_Device = activeDevice;
      g_Device->AddRef();
      g_LastSwapChain = pSwapChain;
      g_State.imGuiInit = false;
      activeDevice->Release();

#ifdef ENABLE_D3D12_WRAPPER
      if (g_Device) {
          ID3D12Device* unwrapped = UnwrapDevice(g_Device);
          if (unwrapped != g_Device) {
              g_Device->Release();
              g_Device = unwrapped;
              g_Device->AddRef();
          }
      }
#endif
      return;
  }
  
  if (isInitialSetup) {
      g_Device = activeDevice;
      g_Device->AddRef();
      g_LastSwapChain = pSwapChain;

#ifdef ENABLE_D3D12_WRAPPER
      if (g_Device) {
          ID3D12Device* unwrapped = UnwrapDevice(g_Device);
          if (unwrapped != g_Device) {
              g_Device->Release();
              g_Device = unwrapped;
              g_Device->AddRef();
          }
      }
#endif
  }
  activeDevice->Release();

  if (g_DeviceRemovedFatal.load()) return;

   // 2. Get the correct queue
   ID3D12CommandQueue* targetQueue = nullptr;
   {
       std::lock_guard<std::mutex> devLock(g_DeviceQueuesMutex);
       if (g_DeviceQueues.count(g_Device)) {
           targetQueue = g_DeviceQueues[g_Device];
       }
   }
   // Fallback: read g_CommandQueue under lock
   if (!targetQueue) {
       std::lock_guard<std::mutex> qLock(g_CommandQueueMutex);
       if (g_CommandQueue) {
           targetQueue = g_CommandQueue;
       }
   }

   if (!targetQueue) {
       HookLog("DX12: ProcessFrame - No CommandQueue found for capture! (g_DeviceQueues size=%zu)", g_DeviceQueues.size());
       if (sc3) sc3->Release();
       return;
   }
   // SAFETY: Validate Queue Type (MUST be DIRECT)
   if (targetQueue) {
       D3D12_COMMAND_QUEUE_DESC qDesc = targetQueue->GetDesc();
       if (qDesc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
           HookLog("DX12: ProcessFrame - Skipping non-DIRECT queue type %d (Queue=%p, NodeMask=0x%X)",
                   qDesc.Type, targetQueue, qDesc.NodeMask);
           if (sc3) sc3->Release();
           return;
       }
   }

   // STABILITY FIX: Update g_CommandQueue under lock with proper AddRef/Release
   static int s_QueueChangeLog = 0;
   bool queueChanged = false;
   {
       std::lock_guard<std::mutex> qLock(g_CommandQueueMutex);
       if (g_CommandQueue != targetQueue) {
           D3D12_COMMAND_QUEUE_DESC oldDesc = {}, newDesc = targetQueue->GetDesc();
           UINT64 oldLuid = 0, newLuid = 0;
           if (g_CommandQueue) {
               oldDesc = g_CommandQueue->GetDesc();
               ID3D12Device* oldDev = nullptr;
               if (SUCCEEDED(g_CommandQueue->GetDevice(IID_PPV_ARGS(&oldDev)))) {
                   LUID oldluid = oldDev->GetAdapterLuid();
                   oldLuid = ((UINT64)oldluid.HighPart << 32) | oldluid.LowPart;
                   oldDev->Release();
               }
           }
           ID3D12Device* newDev = nullptr;
           if (SUCCEEDED(targetQueue->GetDevice(IID_PPV_ARGS(&newDev)))) {
               LUID newluid = newDev->GetAdapterLuid();
               newLuid = ((UINT64)newluid.HighPart << 32) | newluid.LowPart;
               newDev->Release();
           }
            HookLog("DX12: [T:%04X] Queue changing: Old=%p (LUID=0x%016llX, NodeMask=0x%X) -> New=%p (LUID=0x%016llX, NodeMask=0x%X)",
                    GetCurrentThreadId(), g_CommandQueue, oldLuid, oldDesc.NodeMask,
                    targetQueue, newLuid, newDesc.NodeMask);
            
            if (g_CommandQueue) g_CommandQueue->Release();
            g_CommandQueue = targetQueue;
            g_CommandQueue->AddRef();
            queueChanged = true;
            if (++s_QueueChangeLog < 5 || s_QueueChangeLog % 60 == 0) {
                HookLog("DX12: [T:%04X] ProcessFrame - Queue changed to %p (Log=%d)", GetCurrentThreadId(), g_CommandQueue, s_QueueChangeLog);
            }
        }
   }

    // Create a local AddRef'd snapshot for this frame's operations
    ID3D12CommandQueue* frameQueue = nullptr;
    {
        std::lock_guard<std::mutex> qLock(g_CommandQueueMutex);
        if (shouldLog) {
            HookLog("DX12: [T:%04X] Queue snapshot lock acquired (Thread=%04X)", 
                    GetCurrentThreadId(), GetCurrentThreadId());
        }
        if (g_CommandQueue) {
            frameQueue = g_CommandQueue;
            frameQueue->AddRef();
        }
    }

   if (!frameQueue) {
       if (shouldLog) HookLog("DX12: ProcessFrame - Queue disappeared during frame setup!");
       if (sc3) sc3->Release();
       return;
   }

   if (g_State.imGuiInit) {
       ImGui_ImplDX12_SetCommandQueue(frameQueue);
   }

   // 3. Initialize Shared Capture
   if (!g_SharedCaptureD3D12.IsActive()) {
       // If we haven't connected to IPC yet, we might want to wait or skip.
       // SharedCaptureD3D12::Initialize now has verbose logging to pinpoint failures.
       // Only initialize if queue changed recently (first few frames) or already stable
       if (queueChanged || s_QueueChangeLog > 60) {
           bool ok = g_SharedCaptureD3D12.Initialize(g_Device, pSwapChain);
           static int failCount = 0;
           if (!ok) {
               if (failCount++ % 60 == 0) {
                   HookLog("DX12: ProcessFrame - SharedCaptureD3D12::Initialize FAILED (Count: %d)", failCount);
               }
           } else {
               HookLog("DX12: ProcessFrame - SharedCaptureD3D12::Initialize SUCCESS");
           }
       }
   }

  // 4. Initialize Overlay
  if (!g_State.imGuiInit && sc3) {
      DXGI_SWAP_CHAIN_DESC desc;
      if (FAILED(pSwapChain->GetDesc(&desc))) {
          sc3->Release();
          return;
      }
      g_State.cachedWidth = desc.BufferDesc.Width;
      g_State.cachedHeight = desc.BufferDesc.Height;

      HookLog("DX12: ProcessFrame - Initializing Overlay (w=%u, h=%u)", g_State.cachedWidth, g_State.cachedHeight);
      if (!InitImGui(g_Device, DX12OverlayState::ALLOC_POOL_SIZE, desc.BufferDesc.Format, desc.OutputWindow)) {
          sc3->Release();
          return;
      }
      
      CreateRTVs(g_Device, sc3, desc.BufferCount);
      InitOverlaySync(g_Device, desc.BufferCount);
  }

  if (g_IPC && !g_IPCReady) {
    if (g_IPC->Connect()) g_IPCReady = true;
  }

  bool captureIncludeOverlay = false;
  bool shouldDrawOverlay = false;
  if (g_IPC) {
      SharedMemoryLayout* shm = g_IPC->GetSharedMem();
      captureIncludeOverlay = shm ? shm->overlayConfig.captureIncludeOverlay : true;
      shouldDrawOverlay = shm && shm->overlayConfig.showOverlay;
  }

   // No manual preparation needed, DrawOverlay handles it.

   // Define Lambdas - pass frameQueue to use stable snapshot
   auto doOverlay = [&](ID3D12CommandQueue* overlayQueue) {
      static uint64_t overlayCounter = 0;
      overlayCounter++;
      
      // DIAGNOSTIC LOGGING: Log first few frames and periodically
      // Throttled to avoid IO saturation
      static uint64_t lastLog = 0;
      uint64_t now = GetTickCount64();
      bool shouldLogCheckpoint = (now - lastLog > 500); 

      if (shouldLogCheckpoint) {
          HookLog("DX12: doOverlay Enter (Count=%llu)", overlayCounter);
          lastLog = now;
      }

      if (!shouldDrawOverlay || !g_Device || !g_State.cmdList || !g_State.imGuiInit || !g_State.syncInit) {
          if (shouldLogCheckpoint) HookLog("DX12: doOverlay SKIPPED (Condition Check Failed)");
          return;
      }
      
      // STABILITY FIX: Early device health check before any GPU operations
      // This prevents crashes from using a device that was removed mid-frame
      {
          HRESULT deviceReason = g_Device->GetDeviceRemovedReason();
          if (FAILED(deviceReason)) {
              static int logOnce = 0;
              if (logOnce++ < 3) {
                  HookLog("DX12: doOverlay - Device removed (0x%08X), aborting overlay", deviceReason);
              }
              g_DeviceRemovedFatal.store(true, std::memory_order_release);
              return;
          }
      }

      if (!g_State.rtvDescHeap) {
           static int logCount = 0;
           if (logCount++ < 5) HookLog("DX12: doOverlay - RTV Heap NULL despite InitImGui=true. Aborting overlay.");
           return;
      }
      
      // CRITICAL: We DO NOT lock g_OverlayMutex here because ProcessFrame ALREADY holds it.
      // Locking it again causes an immediate self-deadlock.
      // std::lock_guard<std::mutex> overlayLock(g_OverlayMutex);
      
      // Need a valid allocator
      
      // Need a valid allocator
      int allocIdx = g_State.allocIndex;
      g_State.allocIndex = (g_State.allocIndex + 1) % DX12OverlayState::ALLOC_POOL_SIZE;
      
      // Sync
      if (g_State.fence) {
          UINT64 completed = g_State.fence->GetCompletedValue();
          UINT64 target = g_State.fenceValues[allocIdx];
          if (completed < target) {
              g_State.fence->SetEventOnCompletion(target, g_State.fenceEvent);
              DWORD waitResult = WaitForSingleObject(g_State.fenceEvent, 50);
              if (waitResult != WAIT_OBJECT_0) {
                  HookLog("DX12: doOverlay - Fence Wait TIMEOUT (Target=%llu). Skipping overlay to avoid allocator corruption.", target);
                  return;
              }
          }
      }
      
      auto* list = g_State.cmdList;
      if (!list) return;

      // SAFETY: Check allocator bounds
      if (allocIdx >= g_State.allocators.size()) {
          HookLog("DX12: doOverlay - Allocator index %d out of bounds (Size=%zu)", allocIdx, g_State.allocators.size());
          return;
      }
      auto* alloc = g_State.allocators[allocIdx];
      if (!alloc) return;
      
      HRESULT hr = alloc->Reset();
      if (FAILED(hr)) {
          HookLog("DX12: doOverlay - FATAL: Allocator Reset failed (0x%08X). Aborting.", hr);
          return;
      }

      hr = list->Reset(alloc, nullptr);
      if (FAILED(hr)) {
          HookLog("DX12: doOverlay - FATAL: Command List Reset failed (0x%08X). Aborting.", hr);
          return;
      }
      
      // Get backbuffer
      
      UINT bufferIdx = 0;
      if (sc3) {
           bufferIdx = sc3->GetCurrentBackBufferIndex();
      }
      
      ID3D12Resource* backBuffer = nullptr;
      if (SUCCEEDED(pSwapChain->GetBuffer(bufferIdx, IID_PPV_ARGS(&backBuffer))) && backBuffer) {
           
           // Transition to RenderTarget
           D3D12_RESOURCE_BARRIER barrier = {};
           barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
           barrier.Transition.pResource = backBuffer;
           barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
           barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
           barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
           list->ResourceBarrier(1, &barrier);

           // RTV
           if (!g_State.rtvDescHeap) {
               // Should have been caught by early return, but double check
               if (static int logCount = 0; logCount++ < 10) HookLog("DX12: doOverlay - RTV Heap is NULL! Skipping overlay.");
               backBuffer->Release();
               return;
           }
           D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
           rtv.ptr += (SIZE_T)bufferIdx * g_State.rtvDescriptorSize;
           g_Device->CreateRenderTargetView(backBuffer, nullptr, rtv);
           list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
           
           // Viewport
           D3D12_VIEWPORT vp = {0, 0, (float)g_State.cachedWidth, (float)g_State.cachedHeight, 0, 1};
           list->RSSetViewports(1, &vp);
           D3D12_RECT scissor = {0, 0, (LONG)g_State.cachedWidth, (LONG)g_State.cachedHeight};
           list->RSSetScissorRects(1, &scissor);

           DrawOverlay(list);

           // Transition back
           barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
           barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
           list->ResourceBarrier(1, &barrier);
           
           hr = list->Close();
           if (FAILED(hr)) {
               HookLog("DX12: doOverlay - FATAL: Command List Close failed (0x%08X). Aborting Execute.", hr);
               backBuffer->Release();
               return;
           }
           
            ID3D12CommandList* lists[] = {list};

            // Use the passed overlayQueue (already AddRef'd)
            ID3D12CommandQueue* queueToUse = overlayQueue;
            if (queueToUse) {
                // SAFETY: Check Device Match
                ID3D12Device* qDev = nullptr;
                if (SUCCEEDED(queueToUse->GetDevice(IID_PPV_ARGS(&qDev)))) {
                    if (qDev != g_Device) {
                        HookLog("DX12: doOverlay - FATAL: Queue Device (%p) != Global Device (%p). Mismatch! Aborting Execute.", qDev, g_Device);
                        qDev->Release();
                        // Don't release queueToUse - caller owns the ref
                        backBuffer->Release();
                        return;
                    }
                    qDev->Release();
                }

                // STABILITY FIX: Check swapchain validity right before execute
                // This catches cases where swapchain was invalidated during draw
    if (DXGIShared::g_SharedState.swapchainInvalid.load(std::memory_order_acquire)) {
        HookLog("DX12: doOverlay - Swapchain invalidated mid-operation! Aborting.");
        backBuffer->Release();
        return;
    }

                queueToUse->ExecuteCommandLists(1, lists);

                // STABILITY FIX: Signal fence AND wait for completion before releasing backbuffer
                // This matches SpecialK's drain_queue pattern and prevents GPU race conditions
                if (g_State.fence) {
                     g_State.currentFenceValue++;
                     g_State.fenceValues[allocIdx] = g_State.currentFenceValue;
                     queueToUse->Signal(g_State.fence, g_State.currentFenceValue);

                     // NOTE: No post-execute wait needed - backbuffer is swapchain-owned.
                     // GPU sync for allocator reuse is handled by pre-use wait at doOverlay entry.
                }
            } else {
                HookLog("DX12: doOverlay - FATAL: CommandQueue is NULL before Execute!");
            }

            backBuffer->Release();
       }
   };

  auto doCapture = [&]() {
      static uint64_t lastCapLog = 0;
      bool capShouldLog = (now - lastCapLog > 5000); // Video log every 5s
      
      static uint64_t fgCaptureCounter = 0;
      bool shouldCapture = true;
      if (fgActive) {
          fgCaptureCounter++;
          shouldCapture = (fgCaptureCounter % 4 == 0);
      }
      
      bool isRecording = g_IPC && g_IPC->IsRecording();
      if (capShouldLog && isRecording) {
          HookLog("DX12: doCapture - isRecording=1, shouldCapture=%d, processCapture=%d, active=%d", 
                  shouldCapture, processCapture, g_SharedCaptureD3D12.IsActive());
          lastCapLog = now;
      }

      if (shouldCapture && processCapture && isRecording) {
          SharedMemoryLayout* shm = g_IPC->GetSharedMem();
          if (shm && g_SharedCaptureD3D12.IsActive()) {
              std::lock_guard<std::mutex> capLock(g_DX12CaptureMutex);
              UINT bbIdx = sc3 ? sc3->GetCurrentBackBufferIndex() : 0;
                if (g_SharedCaptureD3D12.CaptureFrame(frameQueue, bbIdx)) {
                  SharedFrameDescriptor desc;
                  if (g_SharedCaptureD3D12.GetCurrentFrame(&desc)) {
                      // 1. Sync handles to shm (Zero-copy handles)
                      shm->sharedHandles[0] = (uint64_t)g_SharedCaptureD3D12.GetSharedHandle(0);
                      shm->sharedHandles[1] = (uint64_t)g_SharedCaptureD3D12.GetSharedHandle(1);
                      shm->fenceShareHandle = (uint64_t)g_SharedCaptureD3D12.GetFenceShareHandle();
                      
                      // METADATA PROPAGATION
                      shm->width = desc.width;
                      shm->height = desc.height;
                      shm->format = desc.format;
                      
                      // Update LUID if not set (or changed)
                      if (shm->luidLowPart == 0 && shm->luidHighPart == 0 && g_Device) {
                          LUID luid = g_Device->GetAdapterLuid();
                          shm->luidLowPart = luid.LowPart;
                          shm->luidHighPart = luid.HighPart;
                          if (capShouldLog) HookLog("DX12: doCapture - Propagated LUID: %08x-%08x", luid.HighPart, luid.LowPart);
                      }
                      
                      // 2. Push to ring buffer (Lock-free SPSC)
                      // NOTE: writeIndex and readIndex are RAW monotonic counters (not wrapped)
                      // Slot access uses modulo, but index comparison uses difference
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
                          
                          if (capShouldLog) HookLog("DX12: doCapture - Frame Pushed: index=%u, tex=%d, wIdx=%u", 
                                                    desc.frameNumber, desc.textureIndex, wIdx);
                          shm->frameRing.writeIndex.store(wIdx + 1, std::memory_order_release);
                      } else {
                          // Buffer overflow - capture engine falling behind
                          if (capShouldLog) HookLog("DX12: doCapture - RING BUFFER FULL (rIdx=%u, wIdx=%u)", 
                                                    rIdx, wIdx);
                          shm->frameRing.droppedFrames.fetch_add(1, std::memory_order_relaxed);
                      }
                  } else {
                      if (capShouldLog) HookLog("DX12: doCapture - GetCurrentFrame FAILED");
                  }
              } else {
                  if (capShouldLog) {
                      UINT logBbIdx = sc3 ? sc3->GetCurrentBackBufferIndex() : 0;
                      HookLog("DX12: doCapture - CaptureFrame FAILED (idx=%u)", logBbIdx);
                  }
              }
          } else {
              if (capShouldLog && !g_SharedCaptureD3D12.IsActive()) HookLog("DX12: doCapture - SharedCaptureD3D12 NOT ACTIVE");
          }
      }
  };

   if (captureIncludeOverlay) {
       if (shouldLog) HookLog("DX12: ProcessFrame - doOverlay");
       doOverlay(frameQueue);
       if (shouldLog) HookLog("DX12: ProcessFrame - doCapture");
       doCapture();
   } else {
       if (shouldLog) HookLog("DX12: ProcessFrame - doCapture");
       doCapture();
       if (shouldLog) HookLog("DX12: ProcessFrame - doOverlay");
       doOverlay(frameQueue);
   }
   if (shouldLog) HookLog("DX12: ProcessFrame - Exit");

   // Release the frame queue snapshot
   if (frameQueue) frameQueue->Release();

   if (sc3) sc3->Release();
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
             IUnknown* pTempUnk = nullptr;
             if (SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&pTempUnk)))) {
                  ID3D12Device* device = nullptr;
                  ID3D12CommandQueue* activeQueue = nullptr;
                  if (SUCCEEDED(pTempUnk->QueryInterface(IID_PPV_ARGS(&activeQueue)))) {
                      activeQueue->GetDevice(IID_PPV_ARGS(&device));
                      activeQueue->Release();
                  } else {
                      pTempUnk->QueryInterface(IID_PPV_ARGS(&device));
                  }
                  pTempUnk->Release();
                  
                  if (device) {
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
    }
    
    if (g_DummyBackBuffer) {
             HookLog("DX12: GetBuffer: Serving Dummy Buffer for Index %u", Buffer);
             return g_DummyBackBuffer->QueryInterface(riid, ppSurface);
        }
    }
    
    return hr;
}

// --- Unified DXGI Handlers ---
namespace DXGIShared {

void HandleDX12ProcessFrame(IDXGISwapChain* pSwapChain, bool isRealFrame) {
    if (!pSwapChain) return;

    uint64_t presentCount = DXGIShared::g_SharedState.presentCallCount.load();

    // Grace period
    if (presentCount < OVERLAY_INIT_GRACE_FRAMES) {
        return;
    }

    // Record frame for FG metrics
    int cmdListCount = g_CommandListsExecutedThisFrame.exchange(0);
    g_FGCompat.RecordFrame(cmdListCount);

    // Process frame (Overlay/Capture)
    ProcessFrame(pSwapChain, isRealFrame);
}

void HandleDX12ResizeBegin() {
    DX12_OnSwapchainResizeBegin();
}

} // namespace DXGIShared

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
        delete[] modifiedSamplers1_1;
        delete[] modifiedSamplers;
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

// Helper to hook ExecuteCommandLists on a queue
void HookQueueVTable(ID3D12CommandQueue* queue) {
    // --- ORIGINAL CODE BELOW (RESTORED) ---
    if (!queue) return;
    
    HookLog("============ HookQueueVTable: Called for %p ============", queue);

    // CRITICAL FIX: Check if this is OUR wrapper queue (CWrapD3D12CommandQueue)
    // If it is, skip hooking because calling oExecuteCommandLists would loop back to wrapper
    {
        // Try to detect wrapper by checking our custom GUID
        void* unwrapped = nullptr;
        // IID_CWrapD3D12CommandQueue is defined in wrapper_base.h: {0xd4e5f678, 0x90ab, 0xcdef, ...}
        static const GUID IID_CWrapD3D12CommandQueue = 
        { 0xd4e5f678, 0x90ab, 0xcdef, { 0x12, 0x34, 0x56, 0x78, 0x90, 0x12, 0x34, 0x56 } };
        HRESULT hr = queue->QueryInterface(IID_CWrapD3D12CommandQueue, &unwrapped);
        if (SUCCEEDED(hr) && unwrapped) {
            // This IS a wrapper - the unwrapped pointer is the real queue
            HookLog("============ HookQueueVTable: SKIPPING (is wrapper, real=%p) ============", unwrapped);
            ((IUnknown*)unwrapped)->Release();
            return;
        }
    }

    static std::mutex s_HookMutex;
    std::lock_guard<std::mutex> lock(s_HookMutex);
    
    // Check if THIS SPECIFIC V-Table is already hooked (checking function pointer)
    void** qVtbl = *reinterpret_cast<void***>(queue);
    
    if (qVtbl[10] != DetourExecuteCommandLists) {
         HookLog("DX12: ExecuteCommandLists (VTable[10]) is %p, expected original %p. Hooking...", qVtbl[10], oExecuteCommandLists);
         if (VTableHook::Create(&qVtbl[10], (LPVOID)DetourExecuteCommandLists, (LPVOID*)&oExecuteCommandLists) == VTableHook::Success) {
            HookLog("============ HookQueueVTable: SUCCESS ============");
         } else {
            HookLog("============ HookQueueVTable: FAILED ============");
         }
    } else {
        HookLog("============ HookQueueVTable: ALREADY HOOKED ============");
    }
}

void STDMETHODCALLTYPE
DetourExecuteCommandLists(ID3D12CommandQueue *pThis, UINT NumCommandLists,
                          ID3D12CommandList *const *ppCommandLists) {
  // Trace logging for deadlock analysis
  // Trace logging for deadlock analysis (Disabled to prevent log spam/contention)
  static uint64_t execCount = 0;
  execCount++;
  if (execCount < 20) HookLog("============ DetourExecuteCommandLists (Num=%u, Queue=%p) entering... MATCH=%d ============", 
                             NumCommandLists, pThis, (pThis == g_CommandQueue));

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
      // No FG or Startup: Count from ANY direct queue if Num > 0
      // We check if it's a DIRECT queue by looking at our tracked map or using GetDesc (cached)
      bool isDirect = false;
      if (pThis == g_CommandQueue) {
          isDirect = true;
      } else {
          // Check cache/map
          std::lock_guard<std::mutex> lock(g_DeviceQueuesMutex);
          for (auto const& [dev, q] : g_DeviceQueues) {
              if (q == pThis) {
                  isDirect = true;
                  break;
              }
          }
      }

      if (isDirect) {
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
  
  // ----------------------------------------------------------------------------
  // ROBUST QUEUE DISCOVERY (Backbuffer Bitmap)
  // ----------------------------------------------------------------------------
  // Prevent thrashing between multiple Direct Queues (e.g. Async Compute vs Render)
  // by verifying the queue actually presents frames for all backbuffers.
  
  // FIX: Skip discovery if overlay is already initialized and working
  // The bitmap-based verification never completes for some games (like Strange Brigade)
  // that only use buffer 0. Once overlay is rendering successfully, we've proven the queue works.
  static bool s_DiscoveryComplete = false;
  if (g_State.imGuiInit && g_CommandQueue) {
      s_DiscoveryComplete = true;  // Overlay working = queue verified
  }
  
  if (isDirectQueue && !g_IsQueueFromSwapchain && !s_DiscoveryComplete) {
      bool queueVerified = false;
      
      // Use g_LastSwapChain to get current backbuffer index
      IDXGISwapChain3* pSC = static_cast<IDXGISwapChain3*>(g_LastSwapChain);
      
      if (pSC) {
          UINT currentBuffer = pSC->GetCurrentBackBufferIndex();
          DXGI_SWAP_CHAIN_DESC desc = {};
          if (SUCCEEDED(pSC->GetDesc(&desc))) {
              // Maintain a bitmap of specific backbuffers "touched" by this queue
              static std::map<ID3D12CommandQueue*, uint32_t> s_QueueBitmaps;
              static std::map<ID3D12CommandQueue*, uint64_t> s_QueueFirstFrame;
              static std::mutex s_DiscoveryMutex;
              
              std::lock_guard<std::mutex> discoveryLock(s_DiscoveryMutex);
              // EarlyLog("DX12: Queue Discovery: Locked mutex for %p", pThis);
              
              // Initialize tracking for new queue
              if (s_QueueBitmaps.find(pThis) == s_QueueBitmaps.end()) {
                  s_QueueBitmaps[pThis] = 0;
                  s_QueueFirstFrame[pThis] = g_FGDebugFrameCount.load();
                  EarlyLog("DX12: Queue Discovery: New Candidate %p (BufferIdx=%u, Frame=%llu)", 
                           pThis, currentBuffer, s_QueueFirstFrame[pThis]);
              }
              
              // EarlyLog("DX12: Queue Discovery: About to update bitmap for %p", pThis);
              // Update bitmap
              uint32_t& bitmap = s_QueueBitmaps[pThis];
              // EarlyLog("DX12: Queue Discovery: bitmap ref obtained, current=0x%X, bufferIdx=%u", bitmap, currentBuffer);
              bitmap |= (1 << currentBuffer);
              // EarlyLog("DX12: Queue Discovery: bitmap updated to 0x%X", bitmap);
              
              // Check if ALL buffers have been touched (0 to BufferCount-1)
              uint32_t bufCount = desc.BufferCount;
              // EarlyLog("DX12: Queue Discovery: BufferCount=%u", bufCount);
              if (bufCount > 16) bufCount = 16; // Safety cap
              uint32_t targetMask = (1 << bufCount) - 1;
              // EarlyLog("DX12: Queue Discovery: targetMask=0x%X", targetMask);
              
              if ((bitmap & targetMask) == targetMask) {
                  queueVerified = true;
                  
                  // If this is a DIFFERENT queue than current, switch carefully
                  bool differentQueue = false;
                  {
                      std::lock_guard<std::mutex> qLock(g_CommandQueueMutex);
                      differentQueue = (g_CommandQueue != pThis);
                  }

                  if (differentQueue) {
                      EarlyLog("DX12: Queue Discovery: VERIFIED CACHE HIT %p! (Bitmap=0x%X, Mask=0x%X). Switching from %p...", 
                               pThis, bitmap, targetMask, g_CommandQueue);
                      
                      std::lock_guard<std::mutex> qLock(g_CommandQueueMutex);
                      if (g_CommandQueue) g_CommandQueue->Release();
                      g_CommandQueue = pThis;
                      g_CommandQueue->AddRef();
                  }
              } else {
                  // Log progress occasionally
                  static int discLog = 0;
                  if (discLog++ % 300 == 0) {
                      EarlyLog("DX12: Queue Discovery: Tracking %p... Bitmap=0x%X (Target=0x%X)", 
                               pThis, bitmap, targetMask);
                  }
              }
              // EarlyLog("DX12: Queue Discovery: Section complete for %p", pThis);
              
              // Failsafe: If verification takes too long (>300 frames), assume first candidate is "good enough"
              // Use double-check lock pattern for g_CommandQueue check
              bool needsForcedSwitch = false;
              {
                  std::lock_guard<std::mutex> qLock(g_CommandQueueMutex);
                  if (!queueVerified && !g_CommandQueue && (g_FGDebugFrameCount - s_QueueFirstFrame[pThis] > 300)) {
                      needsForcedSwitch = true;
                  }
              }

              if (needsForcedSwitch) {
                  EarlyLog("DX12: Queue Discovery: Timeout waiting for full verification. Forcing %p.", pThis);
                  std::lock_guard<std::mutex> qLock(g_CommandQueueMutex);
                  if (!g_CommandQueue) { // Check again inside lock
                      g_CommandQueue = pThis;
                      g_CommandQueue->AddRef();
                  }
              }
          }
      } else {
         // No SwapChain yet - Can't verify. 
         // Fallback: If absolutely NO queue is set, take the first one timidly?
         // No, safer to wait for SwapChain so we don't crash on invalid RTVs
      }
  }
  
  static uint64_t oclCount = 0;
  oclCount++;
  if (oclCount < 10) {
      EarlyLog("DX12: DetourExecuteCommandLists: About to call oExecuteCommandLists (%p) for Queue %p, Num=%u", 
               oExecuteCommandLists, pThis, NumCommandLists);
  }
  oExecuteCommandLists(pThis, NumCommandLists, ppCommandLists);
  if (oclCount < 10) {
      EarlyLog("DX12: DetourExecuteCommandLists: oExecuteCommandLists returned OK");
  }
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


// Forward declaration for lazy hook helper
bool InstallDX12HooksOnSwapchain(IDXGISwapChain3* pSwapChain);

PFN_CreateSwapChain oCreateSwapChain = nullptr;
PFN_CreateSwapChainForHwnd oCreateSwapChainForHwnd = nullptr;
PFN_CreateSwapChainForCoreWindow oCreateSwapChainForCoreWindow = nullptr;
PFN_CreateSwapChainForComposition oCreateSwapChainForComposition = nullptr;

HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory *pThis,
                                                IUnknown *pDevice,
                                                DXGI_SWAP_CHAIN_DESC *pDesc,
                                                IDXGISwapChain **ppSwapChain) {
  // --- ORIGINAL CODE RESTORED ---
  // Check if pDevice is a D3D12 device or queue, and if it is wrapped.
  // If it is NOT wrapped, it implies it's a driver-internal device (e.g. Vulkan Promotion),
  // and we should NOT wrap the swapchain to avoid interference.
#ifdef ENABLE_D3D12_WRAPPER
  if (pDevice) {
      static bool isTestApp = false;
      static bool checkedTestApp = false;
      if (!checkedTestApp) {
          char moduleName[256] = {};
          if (GetModuleFileNameA(NULL, moduleName, sizeof(moduleName)) != 0) {
              const char* exeName = strrchr(moduleName, '\\');
              if (exeName) exeName++;
              else exeName = moduleName;
              if (strnicmp(exeName, "dx12_test", 9) == 0) {
                   isTestApp = true;
              }
          }
          checkedTestApp = true;
      }

      ID3D12Device* pD12Device = nullptr;
      if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12Device), (void**)&pD12Device))) {
          bool isWrapped = D3D12Wrapper_IsDeviceWrapped(pD12Device);
          pD12Device->Release();
          if (!isWrapped && !isTestApp) {
              HookLog("DX12: DetourCreateSwapChain: Enabling wrap for internal device/queue (Force Hook).");
              // HookLog("DX12: DetourCreateSwapChain: Skipping wrap for unwrapped (internal) device.");
              // return oCreateSwapChain(pThis, pDevice, pDesc, ppSwapChain);
          }
      } else {
          // If passed a Queue, get device from it
          ID3D12CommandQueue* pQueue = nullptr;
          if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&pQueue))) {
               if (SUCCEEDED(pQueue->GetDevice(IID_PPV_ARGS(&pD12Device)))) {
                   bool isWrapped = D3D12Wrapper_IsDeviceWrapped(pD12Device);
                   pD12Device->Release();
                   pQueue->Release();
                    if (!isWrapped && !isTestApp) {
                        HookLog("DX12: DetourCreateSwapChain: Enabling wrap for internal device/queue (Force Hook).");
                         // HookLog("DX12: DetourCreateSwapChain: Skipping wrap for unwrapped (internal) queue/device.");
                         // return oCreateSwapChain(pThis, pDevice, pDesc, ppSwapChain);
                    }
               } else {
                   pQueue->Release();
               }
          } else {
               // Not D3D12 Device AND Not D3D12 Queue. likely D3D11.
               HRESULT hr = oCreateSwapChain(pThis, pDevice, pDesc, ppSwapChain);
               if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
                   DX11Hook_OnSwapChainCreated(*ppSwapChain);
               }
               return hr;
          }
      }
  }
#endif

  EarlyLog("DX12: DetourCreateSwapChain called (pDevice=%p, pDesc=%p)", pDevice, pDesc);
  if (!pDesc) return DXGI_ERROR_INVALID_CALL;
  if (pDevice) {
    ID3D12CommandQueue *queue = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue),
                                          (void **)&queue))) {
      // Release old queue before capturing new one
      // Disable early capture to force Robust Queue Discovery
      // if (g_CommandQueue) g_CommandQueue->Release();
      // g_CommandQueue = queue; // Keep the reference from QueryInterface
      // g_IsQueueFromSwapchain = true;
      EarlyLog("DX12: DetourCreateSwapChain: Auth Queue %p detected (Skipping capture for Discovery)", queue);
      
      // Ensure ExecuteCommandLists is hooked on this queue
      HookQueueVTable(queue);
      
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
      
      // LAZY HOOKING: Install hooks on this specific swapchain instance if not already done globally
      IDXGISwapChain3* sc = nullptr;
      if (SUCCEEDED((*ppSwapChain)->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc))) {
          InstallDX12HooksOnSwapchain(sc);
          sc->Release();
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
  g_SwapchainCreationCount++;
  int swapCount = g_SwapchainCreationCount.load();
  DXGIShared::g_SharedState.lastSwapchainCreation = std::chrono::steady_clock::now();
  DXGIShared::g_SharedState.fgSwapchainStabilized.store(false);
  EarlyLog("DX12 FG: CreateSwapChainForHwnd #%d called (pDevice=%p, hWnd=%p, Size=%ux%u)", 
           swapCount, pDevice, hWnd, pDesc ? pDesc->Width : 0, pDesc ? pDesc->Height : 0);
  
  if (!pDesc) return DXGI_ERROR_INVALID_CALL;

  // --- ORIGINAL CODE RESTORED ---

  // Check if pDevice is a D3D12 device or queue, and if it is wrapped.
#ifdef ENABLE_D3D12_WRAPPER
  if (pDevice) {
      // Whitelist logic to allow internal tools to be wrapped even if they seem unwrapped initially
      static bool isTestApp = false;
      static bool checkedTestApp = false;
      if (!checkedTestApp) {
          char moduleName[256] = {};
          if (GetModuleFileNameA(NULL, moduleName, sizeof(moduleName)) != 0) {
              const char* exeName = strrchr(moduleName, '\\');
              if (exeName) exeName++;
              else exeName = moduleName;
              if (strnicmp(exeName, "dx12_test", 9) == 0) {
                  isTestApp = true;
              }
          }
          checkedTestApp = true;
      }
      
      ID3D12Device* pD12Device = nullptr;
      if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12Device), (void**)&pD12Device))) {
          bool isWrapped = D3D12Wrapper_IsDeviceWrapped(pD12Device);
          pD12Device->Release();
          if (!isWrapped && !isTestApp) {
              HookLog("DX12: DetourCreateSwapChainForHwnd: Enabling wrap for internal device/queue (Force Hook).");
              // HookLog("DX12: DetourCreateSwapChainForHwnd: Skipping wrap for unwrapped (internal) device.");
              // return oCreateSwapChainForHwnd(pThis, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
          }
      } else {
          ID3D12CommandQueue* pQueue = nullptr;
          if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&pQueue))) {
               if (SUCCEEDED(pQueue->GetDevice(IID_PPV_ARGS(&pD12Device)))) {
                   bool isWrapped = D3D12Wrapper_IsDeviceWrapped(pD12Device);
                   pD12Device->Release();
                   pQueue->Release();
                    if (!isWrapped && !isTestApp) {
                        HookLog("DX12: DetourCreateSwapChainForHwnd: Enabling wrap for internal device/queue (Force Hook).");
                         // HookLog("DX12: DetourCreateSwapChainForHwnd: Skipping wrap for unwrapped (internal) queue/device.");
                         // return oCreateSwapChainForHwnd(pThis, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
                    }
               } else {
                   pQueue->Release();
               }
          } else {
               // Not D3D12 Device AND Not D3D12 Queue. likely D3D11.
               HRESULT hr = oCreateSwapChainForHwnd(pThis, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
               if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
                   DX11Hook_OnSwapChainCreated(*ppSwapChain);
               }
               return hr;
          }
      }
  }
#endif

  if (pDevice) {
    // IGNORE dummy swapchain from proactive hooking (Queue Capture only)
    bool isDummy = (pDesc && pDesc->Width == 1 && pDesc->Height == 1);
    
    if (isDummy) {
        HookLog("DX12: DetourCreateSwapChainForHwnd: Ignoring 1x1 dummy swapchain queue capture (Hooks will still be installed).");
    } else {
        ID3D12CommandQueue *queue = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue),
                                              (void **)&queue))) {
            // Release old queue before capturing new one
            // Disable early capture to force Robust Queue Discovery
            // if (g_CommandQueue) g_CommandQueue->Release();
            // g_CommandQueue = queue; // Keep the reference
            // g_IsQueueFromSwapchain = true;
            EarlyLog("DX12: DetourCreateSwapChainForHwnd: Auth Queue %p detected (Skipping capture for Discovery)", queue);
            
            // Ensure ExecuteCommandLists is hooked on this queue
            HookQueueVTable(queue);
            
            ID3D12Device* dev = nullptr;
            if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&dev)))) {
                std::lock_guard<std::mutex> lock(g_DeviceQueuesMutex);
                // If we already have a queue for this device, release it
                if (g_DeviceQueues.count(dev)) g_DeviceQueues[dev]->Release();
                g_DeviceQueues[dev] = queue;
                queue->AddRef(); // One for the map
                
                if (g_Device) g_Device->Release();
                g_Device = dev; // Keep the reference from GetDevice
            }
            HookLog("CreateSwapChainForHwnd #%d: Captured Queue %p", swapCount, g_CommandQueue);
        }
        g_FGCompat.OnSwapchainRecreation(); // Notify FG detection (triggers suspend)
    }
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
           
           // LAZY HOOKING: Install hooks on this specific swapchain instance
           InstallDX12HooksOnSwapchain(swapChain3);
           
           swapChain3->Release();
           
           // Release local if we did not transfer ownership into g_LastSwapChain
           // (If assigned above, g_LastSwapChain holds the QI ref and we must not Release here)
           // In this function, we always transfer ownership, so null out local.
           swapChain3 = nullptr;
       }
  }
  
  return hr;
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForCoreWindow(
    IDXGIFactory2 *pThis, IUnknown *pDevice, IUnknown *pWindow,
    const DXGI_SWAP_CHAIN_DESC1 *pDesc, IDXGIOutput *pRestrictToOutput,
    IDXGISwapChain1 **ppSwapChain) {
    EarlyLog("DX12: DetourCreateSwapChainForCoreWindow called (pDevice=%p, pWindow=%p)", pDevice, pWindow);

#ifdef ENABLE_D3D12_WRAPPER
    if (pDevice) {
        static bool isTestApp = false;
        static bool checkedTestApp = false;
        if (!checkedTestApp) {
             char moduleName[256] = {};
             if (GetModuleFileNameA(NULL, moduleName, sizeof(moduleName)) != 0) {
                 const char* exeName = strrchr(moduleName, '\\');
                 if (exeName) exeName++;
                 else exeName = moduleName;
                 if (strnicmp(exeName, "dx12_test", 9) == 0) {
                     isTestApp = true;
                 }
             }
             checkedTestApp = true;
        }

        ID3D12Device* pD12Device = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12Device), (void**)&pD12Device))) {
            bool isWrapped = D3D12Wrapper_IsDeviceWrapped(pD12Device);
            pD12Device->Release();
            if (!isWrapped && !isTestApp) {
                 // return oCreateSwapChainForCoreWindow(pThis, pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
            }
        } else {
            ID3D12CommandQueue* pQueue = nullptr;
            if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&pQueue))) {
                 if (SUCCEEDED(pQueue->GetDevice(IID_PPV_ARGS(&pD12Device)))) {
                     bool isWrapped = D3D12Wrapper_IsDeviceWrapped(pD12Device);
                     pD12Device->Release();
                     pQueue->Release();
                     if (!isWrapped && !isTestApp) {
                          // return oCreateSwapChainForCoreWindow(pThis, pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
                     }
                 } else {
                     pQueue->Release();
                 }
            }
        }
    }
#endif

    if (pDevice) {
        ID3D12CommandQueue *queue = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue), (void **)&queue))) {
            // Disable early capture to force Robust Queue Discovery
            // if (g_CommandQueue) g_CommandQueue->Release();
            // g_CommandQueue = queue;
            // g_IsQueueFromSwapchain = true;
            EarlyLog("DX12: DetourCreateSwapChainForHwnd: Auth Queue %p detected (Skipping capture for Discovery)", queue);
            
            // Ensure ExecuteCommandLists is hooked
            HookQueueVTable(queue);
            
            ID3D12Device* dev = nullptr;
            if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&dev)))) {
                if (g_Device) g_Device->Release();
                g_Device = dev;
            }
        }
        g_FGCompat.OnSwapchainRecreation();
    }
    return oCreateSwapChainForCoreWindow(pThis, pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForComposition(
    IDXGIFactory2 *pThis, IUnknown *pDevice, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
    IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) {
    EarlyLog("DX12: DetourCreateSwapChainForComposition called (pDevice=%p)", pDevice);

#ifdef ENABLE_D3D12_WRAPPER
    if (pDevice) {
        static bool isTestApp = false;
        static bool checkedTestApp = false;
        if (!checkedTestApp) {
             char moduleName[256] = {};
             if (GetModuleFileNameA(NULL, moduleName, sizeof(moduleName)) != 0) {
                 const char* exeName = strrchr(moduleName, '\\');
                 if (exeName) exeName++;
                 else exeName = moduleName;
                 if (strnicmp(exeName, "dx12_test", 9) == 0) {
                     isTestApp = true;
                 }
             }
             checkedTestApp = true;
        }

        ID3D12Device* pD12Device = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12Device), (void**)&pD12Device))) {
            bool isWrapped = D3D12Wrapper_IsDeviceWrapped(pD12Device);
            pD12Device->Release();
            if (!isWrapped && !isTestApp) {
                 // return oCreateSwapChainForComposition(pThis, pDevice, pDesc, pRestrictToOutput, ppSwapChain);
            }
        } else {
            ID3D12CommandQueue* pQueue = nullptr;
            if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&pQueue))) {
                 if (SUCCEEDED(pQueue->GetDevice(IID_PPV_ARGS(&pD12Device)))) {
                     bool isWrapped = D3D12Wrapper_IsDeviceWrapped(pD12Device);
                     pD12Device->Release();
                     pQueue->Release();
                     if (!isWrapped && !isTestApp) {
                          // return oCreateSwapChainForComposition(pThis, pDevice, pDesc, pRestrictToOutput, ppSwapChain);
                     }
                 } else {
                     pQueue->Release();
                 }
            }
        }
    }
#endif

    if (pDevice) {
        ID3D12CommandQueue *queue = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue), (void **)&queue))) {
            // Disable early capture to force Robust Queue Discovery
            // if (g_CommandQueue) g_CommandQueue->Release();
            // g_CommandQueue = queue;
            // g_IsQueueFromSwapchain = true;
            EarlyLog("DX12: DetourCreateSwapChainForComposition: Auth Queue %p detected (Skipping capture for Discovery)", queue);
            
            // Ensure ExecuteCommandLists is hooked
            HookQueueVTable(queue);
            
            ID3D12Device* dev = nullptr;
            if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&dev)))) {
                if (g_Device) g_Device->Release();
                g_Device = dev;
            }
        }
        g_FGCompat.OnSwapchainRecreation();
    }
    return oCreateSwapChainForComposition(pThis, pDevice, pDesc, pRestrictToOutput, ppSwapChain);
}

// --- Redundant Detours Removed (Handled by DXGIShared) ---


#ifdef ENABLE_D3D12_WRAPPER
HRESULT WINAPI DetourD3D12CreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid, void** ppDevice) {
    HookLog("DX12: DetourD3D12CreateDevice called (pAdapter=%p)", pAdapter);
    if (pAdapter) {
        ICWrapDXGIAdapter* pWrapper = nullptr;
        if (SUCCEEDED(pAdapter->QueryInterface(IID_CWrapDXGIAdapter, (void**)&pWrapper)) && pWrapper) {
            HookLog("DX12: DetourD3D12CreateDevice - Unwrapping adapter %p -> %p", pAdapter, pWrapper->GetReal());
            pAdapter = pWrapper->GetReal();
            pWrapper->Release();
        }
    }
    HRESULT hr = oD3D12CreateDevice(pAdapter, MinimumFeatureLevel, riid, ppDevice);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        HookLog("DX12: Capturing Device via D3D12CreateDevice hook");
        ID3D12Device* dev = (ID3D12Device*)*ppDevice;
        LUID luid = dev->GetAdapterLuid();
        ReportLUID(luid.LowPart, luid.HighPart);

        void** vtbl = *reinterpret_cast<void***>(dev);
        static std::mutex s_HookMutex;
        std::lock_guard<std::mutex> lock(s_HookMutex);

        static bool hookedSampler = false;
        if (!hookedSampler) {
            VTableHook::Status s = VTableHook::Create(&vtbl[22], (LPVOID)DetourCreateSampler, (LPVOID*)&oCreateSampler);
            if (s == VTableHook::Success || s == VTableHook::ErrorAlreadyCreated) hookedSampler = true;
        }
        
        static bool hookedCreateResource = false;
        if (!hookedCreateResource) {
            VTableHook::Status s = VTableHook::Create(&vtbl[27], (LPVOID)DetourCreateCommittedResource, (LPVOID*)&oCreateCommittedResource);
            if (s == VTableHook::Success || s == VTableHook::ErrorAlreadyCreated) hookedCreateResource = true;
        }
        
        static bool hookedQueue = false;
        if (!hookedQueue) {
            D3D12_COMMAND_QUEUE_DESC queueDesc = {};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            ID3D12CommandQueue* dummyQueue = nullptr;
            if (SUCCEEDED(dev->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&dummyQueue))) && dummyQueue) {
                void** qVtbl = *reinterpret_cast<void***>(dummyQueue);
                VTableHook::Status s = VTableHook::Create(&qVtbl[10], (LPVOID)DetourExecuteCommandLists, (LPVOID*)&oExecuteCommandLists);
                if (s == VTableHook::Success) HookLog("DX12: Hooked ExecuteCommandLists (via lazy queue creation)");
                dummyQueue->Release();
            }
            hookedQueue = true;
        }
    }
    return hr;
}
#endif

void DX12Hook::Init() {
  static std::mutex s_InitMutex;
  static bool s_InitDone = false;
  
  std::lock_guard<std::mutex> lock(s_InitMutex);
  if (s_InitDone) return;
  s_InitDone = true;

  EarlyLog("DX12Hook::Init() called - installing SAFE hooks (Lazy Mode)");
  HMODULE hD3D12 = LoadLibraryA("d3d12.dll");
  HMODULE hDXGI = LoadLibraryA("dxgi.dll");
  if (!hD3D12 || !hDXGI) {
    HookLog("DX12: D3D12 or DXGI DLLs not found. Skipping DX12 hook.");
    return;
  }

#ifdef ENABLE_D3D12_WRAPPER
  HookLog("DX12: Skipping MinHook for D3D12CreateDevice (using IAT wrapper instead)");
#endif

  typedef HRESULT (WINAPI *PFN_CREATE_DXGI_FACTORY1)(REFIID, void**);
  PFN_CREATE_DXGI_FACTORY1 pCreateDXGIFactory1 = nullptr;
  
  // Use wrapper pointer if available to avoid recursion
  if (oCreateDXGIFactory1) {
      pCreateDXGIFactory1 = (PFN_CREATE_DXGI_FACTORY1)oCreateDXGIFactory1;
      HookLog("DX12: Using existing oCreateDXGIFactory1 from wrapper");
  } else {
      pCreateDXGIFactory1 = (PFN_CREATE_DXGI_FACTORY1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
  }

  if (!pCreateDXGIFactory1) {
    HookLog("DX12: Failed to find CreateDXGIFactory1 entry point.");
    return;
  }

  IDXGIFactory4 *factory = nullptr;
  HRESULT hr = pCreateDXGIFactory1(IID_PPV_ARGS(&factory));

  if (factory && SUCCEEDED(hr)) {
      void **facVTable = *reinterpret_cast<void ***>(factory);
      VTableHook::Create(&facVTable[10], (LPVOID)DetourCreateSwapChain, (LPVOID *)&oCreateSwapChain);
      VTableHook::Create(&facVTable[15], (LPVOID)DetourCreateSwapChainForHwnd, (LPVOID *)&oCreateSwapChainForHwnd);
      VTableHook::Create(&facVTable[16], (LPVOID)DetourCreateSwapChainForCoreWindow, (LPVOID *)&oCreateSwapChainForCoreWindow);
      VTableHook::Create(&facVTable[24], (LPVOID)DetourCreateSwapChainForComposition, (LPVOID *)&oCreateSwapChainForComposition);
      
      HookLog("DX12: Attempting proactive swapchain VTable hooking...");
      
      // RESTORED: Proactive swapchain creation for VTable discovery
      if (hD3D12) {
          typedef HRESULT (WINAPI *PFN_D3D12_CREATE_DEVICE)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
          PFN_D3D12_CREATE_DEVICE pD3D12CreateDevice = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(hD3D12, "D3D12CreateDevice");

          ID3D12Device* dummyDevice = nullptr;
          if (pD3D12CreateDevice && SUCCEEDED(pD3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dummyDevice)))) {
              D3D12_COMMAND_QUEUE_DESC queueDesc = {};
              queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
              ID3D12CommandQueue* dummyQueue = nullptr;
              if (SUCCEEDED(dummyDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&dummyQueue)))) {
                  DXGI_SWAP_CHAIN_DESC1 scDesc = {};
                  scDesc.Width = 1; scDesc.Height = 1; scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                  scDesc.SampleDesc.Count = 1; scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                  scDesc.BufferCount = 2; scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

                  HWND hwnd = CreateWindowA("STATIC", "Dummy", WS_OVERLAPPEDWINDOW, 0, 0, 1, 1, NULL, NULL, NULL, NULL);
                  if (hwnd) {
                      IDXGISwapChain1* sc1 = nullptr;
                      if (SUCCEEDED(factory->CreateSwapChainForHwnd(dummyQueue, hwnd, &scDesc, nullptr, nullptr, &sc1))) {
                          IDXGISwapChain3* sc3 = nullptr;
                          if (SUCCEEDED(sc1->QueryInterface(IID_PPV_ARGS(&sc3)))) {
                              DXGIShared::InstallHooks(sc3);
                              sc3->Release();
                          }
                          sc1->Release();
                      }
                      DestroyWindow(hwnd);
                  }
                  dummyQueue->Release();
              }
              dummyDevice->Release();
          }
      }
      factory->Release();
  }
  HookLog("DX12Hook: VTableHook initialized (Lazy Init Complete).");
}

// DX11 Present hooks are declared in dx11_hook.h (included via dx11_hook.h in this file)
// They are used here to detect if DX11 has ALREADY hooked a swapchain's vtable

bool InstallDX12HooksOnSwapchain(IDXGISwapChain3* pSwapChain) {
    return DXGIShared::InstallHooks(pSwapChain);
}


void DX12Hook::Shutdown() {
  HookLog("DX12Hook::Shutdown()");
  
  // Cleanup any tracked internal resources first
  CleanupResources();
  
  // First disable hooks to stop new frames from coming in
  // No disable needed for VTable hooks as we don't unpatch on shutdown currently
  // MH_DisableHook(MH_ALL_HOOKS);
  
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
  
  // Release capture resources (SharedCaptureD3D12)
  if (g_SharedCaptureD3D12.IsActive()) {
      g_SharedCaptureD3D12.Reset();
  }

  g_IPCReady = false;

  HookLog("DX12Hook shutdown complete");
}

void DX12Hook::OnHostDisconnect() {
  HookLog("DX12Hook::OnHostDisconnect() - reset for reconnection");
  
  // SharedCapture doesn't have a separate thread to stop manually,
  // but we can reset it if needed. For now, keep it alive or let next init handle it.
  
  // Reset IPC ready flag so we re-establish connection
  g_IPCReady = false;
  
  HookLog("DX12Hook::OnHostDisconnect() complete - ready for reconnection");
}

void DX12Hook::TrackResource(IUnknown* res) {
    if (!res) return;
    std::lock_guard<std::mutex> lock(resourceMutex);
    res->AddRef();
    trackedResources.push_back(res);
}

void DX12Hook::CleanupResources() {
    std::lock_guard<std::mutex> lock(resourceMutex);
    for (auto* res : trackedResources) {
        if (res) res->Release();
    }
    trackedResources.clear();
}

DWORD WINAPI UnloadThread(LPVOID lpParam) {
  Sleep(200);
  g_dx12HookInstance.Shutdown(); 
  return 0;
}
