#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <unknwn.h>
#include <combaseapi.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>
#include <map>
#include <chrono>
#include <string>

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
#include "../capture/shared_capture.h"

#include "dxgi_shared.h"
#include "../wrappers/vtable_hook.h"
#include "../wrappers/wrapper_base.h"

// Typedefs for D3D12 functions
typedef void(STDMETHODCALLTYPE *ExecuteCommandListsPtr)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
typedef void(STDMETHODCALLTYPE *CreateSamplerPtr)(ID3D12Device *, const D3D12_SAMPLER_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef HRESULT(STDMETHODCALLTYPE *CreateCommittedResourcePtr)(ID3D12Device *, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **);
typedef HRESULT(WINAPI *PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI *PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);

// Global Function Pointers for detours (Visible to other modules)
ExecuteCommandListsPtr oExecuteCommandLists = nullptr;
CreateSamplerPtr oCreateSampler = nullptr;
CreateCommittedResourcePtr oCreateCommittedResource = nullptr;
PFN_D3D12_SERIALIZE_ROOT_SIGNATURE oSerializeRootSignature = nullptr;
PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE oSerializeVersionedRootSignature = nullptr;

// SwapChain Detour Pointers
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChain)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChainForHwnd)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);

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
    void Cleanup() {
        for (auto& bb : backBuffers) if (bb) bb->Release();
        backBuffers.clear();
        if (rtvDescHeap) { rtvDescHeap->Release(); rtvDescHeap = nullptr; }
        if (srvDescHeap) { srvDescHeap->Release(); srvDescHeap = nullptr; }
        for (auto* alloc : allocators) if (alloc) alloc->Release();
        allocators.clear();
        if (cmdList) { cmdList->Release(); cmdList = nullptr; }
        if (fence) { fence->Release(); fence = nullptr; }
        if (fenceEvent) { CloseHandle(fenceEvent); fenceEvent = nullptr; }
        imGuiInit = false; syncInit = false;
    }
};

static DX12OverlayState g_State;
static std::recursive_mutex g_ImGuiFrameMutex;
static SharedCaptureD3D12 g_SharedCaptureD3D12;

ID3D12Device *g_Device = nullptr;
ID3D12CommandQueue *g_CommandQueue = nullptr;
std::recursive_mutex g_CommandQueueMutex;
bool g_IPCReady = false;
static IDXGISwapChain *g_LastSwapChain = nullptr;
static ID3D12Resource *g_DummyBackBuffer = nullptr;
static std::recursive_mutex g_OverlayMutex;
static std::recursive_mutex g_InitImGuiMutex;
static std::recursive_mutex g_DX12CaptureMutex;
static std::atomic<bool> g_InSwapchainResizeCleanup{false};
static std::atomic<int> g_CommandListsExecutedThisFrame{0};
static std::atomic<uint64_t> g_FGDebugFrameCount{0};

// Use pointer to prevent static destructor execution in non-game processes (Explorer fix)
DX12Hook* g_dx12HookInstance = nullptr;

std::recursive_mutex g_DeviceQueuesMutex;
std::map<ID3D12Device*, ID3D12CommandQueue*> g_DeviceQueues;

// Forward Declarations
void STDMETHODCALLTYPE DetourExecuteCommandLists(ID3D12CommandQueue *pThis, UINT NumCommandLists, ID3D12CommandList *const *ppCommandLists);
void HookQueueVTable(ID3D12CommandQueue* queue);
HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory *pThis, IUnknown *pDevice, DXGI_SWAP_CHAIN_DESC *pDesc, IDXGISwapChain **ppSwapChain);
HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2 *pThis, IUnknown *pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFDesc, IDXGIOutput *pOut, IDXGISwapChain1 **ppSC);

// REQUIRED EXPORTS
__declspec(dllexport) void DX12_AdjustWrapperResizeDepth(int delta) {
    if (delta > 0) DXGIShared::g_SharedState.wrapperResizeDepth.fetch_add(delta);
    else DXGIShared::g_SharedState.wrapperResizeDepth.fetch_sub(-delta);
}

__declspec(dllexport) void DX12_InvalidateSwapchain() {
    DXGIShared::g_SharedState.swapchainInvalid.store(true, std::memory_order_release);
    HookLog("DX12: Swapchain marked INVALID");
}

__declspec(dllexport) void DX12_SignalFSR4SwapchainRecreated() {
    DXGIShared::g_SharedState.fsr4RecreationPending.store(true, std::memory_order_release);
    HookLog("DX12: FSR4 swapchain recreation signaled");
}

// C Linkage Exports for cross-module calls (e.g. from C clients or GetProcAddress)
extern "C" {
    __declspec(dllexport) void DX12_SetCommandQueue(ID3D12CommandQueue* pQueue) {
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
            g_CommandQueue = pQueue; g_CommandQueue->AddRef();
            ID3D12Device* dev = nullptr;
            if (SUCCEEDED(g_CommandQueue->GetDevice(IID_PPV_ARGS(&dev)))) {
                if (g_Device != dev) { if (g_Device) g_Device->Release(); g_Device = dev; } else dev->Release();
            }
        }
    }
    __declspec(dllexport) void DX12_AdjustWrapperResizeDepth_C(int delta) {
        DX12_AdjustWrapperResizeDepth(delta);
    }
}

void DX12_OnSwapchainResizeEnd();
void DX12_OnSwapchainResizeBegin();

// Helper to ensure global hook instance exists
void EnsureDX12Hook() {
    if (!g_dx12HookInstance) {
        g_dx12HookInstance = new DX12Hook();
    }
}

void DX12Hook::Init() {
  EnsureDX12Hook(); // Self-init check
  static std::recursive_mutex s_InitMutex; static bool s_InitDone = false;
  std::lock_guard<std::recursive_mutex> lock(s_InitMutex); if (s_InitDone) return; s_InitDone = true;

  if (AreWrappersActive()) {
      HookLog("DX12Hook: Wrappers are active, skipping Factory VTable hooks.");
      return;
  }
  
  HMODULE hDXGI = LoadLibraryA("dxgi.dll"); if (!hDXGI) return;
  typedef HRESULT (WINAPI *PFN_CREATE_DXGI_FACTORY1)(REFIID, void**);
  PFN_CREATE_DXGI_FACTORY1 pCreate = (PFN_CREATE_DXGI_FACTORY1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
  if (!pCreate) return;
  IDXGIFactory4 *factory = nullptr;
  if (SUCCEEDED(pCreate(IID_PPV_ARGS(&factory)))) {
      void **vtbl = *reinterpret_cast<void ***>(factory);
      VTableHook::Create(&vtbl[10], (LPVOID)DetourCreateSwapChain, (LPVOID *)&oCreateSwapChain);
      VTableHook::Create(&vtbl[15], (LPVOID)DetourCreateSwapChainForHwnd, (LPVOID *)&oCreateSwapChainForHwnd);
      factory->Release();
  }
}

void ShutdownImGui() {
  if (!g_State.imGuiInit) return;
  ImGui_ImplDX12_Shutdown(); g_SharedOverlay.ShutdownImGui();
  if (g_State.srvDescHeap) { g_State.srvDescHeap->Release(); g_State.srvDescHeap = nullptr; }
  g_State.imGuiInit = false;
}

bool InitImGui(ID3D12Device *device, int buffers, DXGI_FORMAT format, HWND hwnd) {
  std::lock_guard<std::recursive_mutex> lock(g_InitImGuiMutex);
  if (g_State.imGuiInit) return true;
  g_State.format = format; g_SharedOverlay.InitImGui(hwnd);
  InputManager::Get().HookWindow(hwnd);
  D3D12_DESCRIPTOR_HEAP_DESC desc = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
  UINT nodeCount = device->GetNodeCount();
  if (nodeCount > 1) desc.NodeMask = 1;
  if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_State.srvDescHeap)))) {
      desc.NodeMask = 0; if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_State.srvDescHeap)))) return false;
  }
  if (!ImGui_ImplDX12_Init(device, buffers, format, g_State.srvDescHeap, g_State.srvDescHeap->GetCPUDescriptorHandleForHeapStart(), g_State.srvDescHeap->GetGPUDescriptorHandleForHeapStart())) {
      g_State.srvDescHeap->Release(); g_State.srvDescHeap = nullptr; return false;
  }
  if (g_CommandQueue) ImGui_ImplDX12_SetCommandQueue(g_CommandQueue);
  g_State.imGuiInit = true; return true;
}

void DrawOverlay(ID3D12GraphicsCommandList *cmdList) {
  if (!g_State.imGuiInit || !cmdList) return;
  ImGui_ImplDX12_NewFrame(); g_SharedOverlay.BeginFrame();
  g_SharedOverlay.SetMetrics(DXGIShared::GetPerformanceMetrics());
  g_SharedOverlay.SetIPCClient(g_IPC);
  const char* api = "DX12";
  if (GetModuleHandleA("d3d12core.dll") && (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll"))) api = "DX12 (VKD3D)";
  g_SharedOverlay.SetGraphicsAPI(api);
  bool isHDR = (g_State.format == DXGI_FORMAT_R16G16B16A16_FLOAT || g_State.format == DXGI_FORMAT_R10G10B10A2_UNORM);
  g_SharedOverlay.SetHDR(isHDR);
  g_SharedOverlay.RenderUI(); g_SharedOverlay.EndFrame();
  cmdList->SetDescriptorHeaps(1, &g_State.srvDescHeap); ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList); 
}

void CreateRTVs(ID3D12Device *device, IDXGISwapChain3 *swapChain, int bufferCount) {
  if (g_State.rtvDescHeap) return;
  D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, (UINT)bufferCount, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
  if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_State.rtvDescHeap)))) return;
  g_State.bufferCount = bufferCount; g_State.cachedSwapChain = swapChain;
  g_State.rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
  for (int i = 0; i < bufferCount; i++) {
    ID3D12Resource* bb = nullptr;
    if (SUCCEEDED(swapChain->GetBuffer(i, IID_PPV_ARGS(&bb))) && bb) { device->CreateRenderTargetView(bb, nullptr, rtvHandle); bb->Release(); }
    rtvHandle.ptr += g_State.rtvDescriptorSize;
  }
}

void InitOverlaySync(ID3D12Device *device, int bufferCount) {
  if (g_State.syncInit) return;
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_State.fence)))) return;
  
  g_State.allocators.resize(DX12OverlayState::ALLOC_POOL_SIZE);
  g_State.fenceValues.resize(DX12OverlayState::ALLOC_POOL_SIZE, 0);
  
  bool success = true;
  for (int i = 0; i < DX12OverlayState::ALLOC_POOL_SIZE; i++) {
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_State.allocators[i])))) {
        success = false;
        break;
    }
  }
  
  if (success) {
      if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_State.allocators[0], nullptr, IID_PPV_ARGS(&g_State.cmdList)))) {
          success = false;
      }
  }

  if (success) {
      g_State.cmdList->Close(); 
      g_State.fenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL); 
      g_State.syncInit = true;
  } else {
      // Cleanup partial initialization
      for (auto* alloc : g_State.allocators) if (alloc) alloc->Release();
      g_State.allocators.clear();
      g_State.fenceValues.clear();
      if (g_State.cmdList) { g_State.cmdList->Release(); g_State.cmdList = nullptr; }
      if (g_State.fence) { g_State.fence->Release(); g_State.fence = nullptr; }
  }
}

static bool DrainCommandQueue(ID3D12CommandQueue* queue, ID3D12Device* device) {
    if (!queue || !device) return false;
    ID3D12Fence* fence = nullptr;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (event) {
        if (SUCCEEDED(queue->Signal(fence, 1))) { if (fence->SetEventOnCompletion(1, event) == S_OK) WaitForSingleObject(event, 2000); }
        CloseHandle(event);
    }
    fence->Release(); return true;
}

void CleanupOverlay() {
  if (!g_State.syncInit) return;
  if (g_State.fence && g_CommandQueue) {
    UINT64 waitValue = g_State.currentFenceValue + 1;
    if (SUCCEEDED(g_CommandQueue->Signal(g_State.fence, waitValue))) {
        if (g_State.fence->GetCompletedValue() < waitValue) { g_State.fence->SetEventOnCompletion(waitValue, g_State.fenceEvent); WaitForSingleObject(g_State.fenceEvent, 100); }
    }
  }
  if (g_State.fenceEvent) { CloseHandle(g_State.fenceEvent); g_State.fenceEvent = NULL; }
  for (auto alloc : g_State.allocators) if (alloc) alloc->Release();
  g_State.allocators.clear(); g_State.fenceValues.clear();
  if (g_State.cmdList) { g_State.cmdList->Release(); g_State.cmdList = nullptr; }
  if (g_State.fence) { g_State.fence->Release(); g_State.fence = nullptr; }
  g_State.currentFenceValue = 0; g_State.allocIndex = 0; g_State.syncInit = false; ShutdownImGui();
}

void CleanupRTVs() {
  for (auto *r : g_State.backBuffers) if (r) r->Release();
  g_State.backBuffers.clear();
  if (g_DummyBackBuffer) { g_DummyBackBuffer->Release(); g_DummyBackBuffer = nullptr; }
  if (g_State.rtvDescHeap) { g_State.rtvDescHeap->Release(); g_State.rtvDescHeap = nullptr; }
  if (g_State.srvDescHeap) { g_State.srvDescHeap->Release(); g_State.srvDescHeap = nullptr; }
}

void DX12_OnSwapchainResizeBegin() {
  if (g_InSwapchainResizeCleanup.exchange(true)) return;
  DXGIShared::g_SharedState.lastSwapchainCreation = std::chrono::steady_clock::now();
  
  // 1. Capture Queue Reference (Thread-safe)
  ID3D12CommandQueue* q = nullptr;
  { 
      std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex); 
      if (g_CommandQueue) { 
          q = g_CommandQueue; 
          q->AddRef(); 
      } 
  }
  
  // 2. Drain Queue (NO LOCK HELD)
  // Prevents stalling other threads if the GPU is slow to drain
  if (q) { 
      ID3D12Device* d = nullptr; 
      if (SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&d)))) { 
          DrainCommandQueue(q, d); 
          d->Release(); 
      } 
      q->Release(); 
  }
  
  // 3. Cleanup Resources (Hold Overlay Lock)
  std::lock_guard<std::recursive_mutex> lock(g_OverlayMutex);
  
  CleanupOverlay(); 
  CleanupRTVs();
  { std::lock_guard<std::recursive_mutex> cl(g_DX12CaptureMutex); g_SharedCaptureD3D12.Reset(); }
  if (g_State.imGuiInit) ImGui_ImplDX12_InvalidateDeviceObjects();
  if (g_LastSwapChain) { g_LastSwapChain->Release(); g_LastSwapChain = nullptr; }
}

void DX12_OnSwapchainResizeEnd() { g_InSwapchainResizeCleanup.store(false, std::memory_order_release); }

void ProcessFrame(IDXGISwapChain *pSwapChain, bool processCapture) {
  if (!pSwapChain || g_InSwapchainResizeCleanup.load(std::memory_order_acquire)) return;
  std::lock_guard<std::recursive_mutex> lock(g_OverlayMutex);

  // OPTIMIZATION: Only resolve device if swapchain changed or device not yet known
  if (!g_Device || pSwapChain != g_LastSwapChain) {
      IUnknown* pUnk = nullptr; if (FAILED(pSwapChain->GetDevice(IID_PPV_ARGS(&pUnk)))) return;
      ID3D12Device* activeDevice = nullptr; ID3D12CommandQueue* activeQueue = nullptr;
      if (SUCCEEDED(pUnk->QueryInterface(IID_PPV_ARGS(&activeQueue)))) { 
          activeQueue->GetDevice(IID_PPV_ARGS(&activeDevice)); 
          HookQueueVTable(activeQueue); 
          activeQueue->Release(); 
      }
      else {
          pUnk->QueryInterface(IID_PPV_ARGS(&activeDevice));
      }
      pUnk->Release(); 
      
      if (!activeDevice) return;
      
      if (g_Device == nullptr || activeDevice != g_Device || pSwapChain != g_LastSwapChain) {
          if (g_Device) { CleanupOverlay(); CleanupRTVs(); ShutdownImGui(); g_SharedCaptureD3D12.Reset(); g_Device->Release(); }
          g_Device = activeDevice; g_Device->AddRef();
          if (g_LastSwapChain) g_LastSwapChain->Release();
          g_LastSwapChain = pSwapChain; g_LastSwapChain->AddRef();
          g_State.imGuiInit = false;
      }
      activeDevice->Release();
  }
  ID3D12CommandQueue* q = nullptr;
  { std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex); if (g_CommandQueue) { q = g_CommandQueue; q->AddRef(); } }
  if (!q) return;
  if (g_State.imGuiInit) ImGui_ImplDX12_SetCommandQueue(q);
  if (!g_State.imGuiInit) {
      DXGI_SWAP_CHAIN_DESC desc;
      if (SUCCEEDED(pSwapChain->GetDesc(&desc))) {
          g_State.cachedWidth = desc.BufferDesc.Width; g_State.cachedHeight = desc.BufferDesc.Height;
          if (InitImGui(g_Device, desc.BufferCount, desc.BufferDesc.Format, desc.OutputWindow)) {
              IDXGISwapChain3* sc3 = nullptr;
              if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) { CreateRTVs(g_Device, sc3, desc.BufferCount); InitOverlaySync(g_Device, desc.BufferCount); sc3->Release(); }
          }
      }
  }
  if (g_State.imGuiInit && g_State.syncInit) {
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
          auto* list = g_State.cmdList; auto* alloc = g_State.allocators[idx];
          if (list && alloc) {
              if (SUCCEEDED(alloc->Reset())) {
                  if (SUCCEEDED(list->Reset(alloc, nullptr))) {
                      IDXGISwapChain3* sc3 = nullptr;
                      if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
                          UINT bufferIdx = sc3->GetCurrentBackBufferIndex(); 
                          ID3D12Resource* bb = nullptr;
                          if (SUCCEEDED(pSwapChain->GetBuffer(bufferIdx, IID_PPV_ARGS(&bb)))) {
                              D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE, { bb, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET } };
                              list->ResourceBarrier(1, &b);
                              D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
                              rtv.ptr += (SIZE_T)bufferIdx * g_State.rtvDescriptorSize;
                              g_Device->CreateRenderTargetView(bb, nullptr, rtv);
                              list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                              D3D12_VIEWPORT vp = {0, 0, (float)g_State.cachedWidth, (float)g_State.cachedHeight, 0, 1}; list->RSSetViewports(1, &vp);
                              D3D12_RECT scissor = {0, 0, (LONG)g_State.cachedWidth, (LONG)g_State.cachedHeight}; list->RSSetScissorRects(1, &scissor);
                              DrawOverlay(list);
                              b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
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
  if (processCapture && g_IPC && g_IPC->IsRecording()) {
      SharedMemoryLayout* shm = g_IPC->GetSharedMem();
      if (shm) {
          if (!g_SharedCaptureD3D12.IsActive()) g_SharedCaptureD3D12.Initialize(g_Device, pSwapChain);
          if (g_SharedCaptureD3D12.IsActive()) {
              std::lock_guard<std::recursive_mutex> capLock(g_DX12CaptureMutex);
              IDXGISwapChain3* sc3 = nullptr; pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3));
              UINT bbIdx = sc3 ? sc3->GetCurrentBackBufferIndex() : 0; if (sc3) sc3->Release();
              if (g_SharedCaptureD3D12.CaptureFrame(q, bbIdx)) {
                  SharedFrameDescriptor desc;
                  if (g_SharedCaptureD3D12.GetCurrentFrame(&desc)) {
                      shm->sharedHandles[0] = (uint64_t)g_SharedCaptureD3D12.GetSharedHandle(0);
                      shm->sharedHandles[1] = (uint64_t)g_SharedCaptureD3D12.GetSharedHandle(1);
                      shm->fenceShareHandle = (uint64_t)g_SharedCaptureD3D12.GetFenceShareHandle();
                      shm->width = desc.width; shm->height = desc.height; shm->format = desc.format;
                      uint32_t wIdx = shm->frameRing.writeIndex.load(std::memory_order_relaxed);
                      uint32_t rIdx = shm->frameRing.readIndex.load(std::memory_order_acquire);
                      if ((uint32_t)(wIdx - rIdx) < (uint32_t)FRAME_RING_SIZE) {
                          FrameSlot& slot = shm->frameRing.slots[wIdx % FRAME_RING_SIZE];
                          slot.fenceValue = desc.fenceValue; slot.timestamp = desc.presentTime;
                          slot.frameIndex = desc.frameNumber; slot.textureIndex = desc.textureIndex;
                          slot.sourcePid = GetCurrentProcessId(); slot.valid.store(1, std::memory_order_release);
                          shm->frameRing.writeIndex.store(wIdx + 1, std::memory_order_release);
                      } else shm->frameRing.droppedFrames.fetch_add(1, std::memory_order_relaxed);
                  }
              }
          }
      }
  }
  q->Release();
}

void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain) {
  if (!pSwapChain) return;
  IDXGISwapChain3* sc3 = nullptr;
  if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) return;
  int count = g_CommandListsExecutedThisFrame.exchange(0);
  g_FGDebugFrameCount++; g_FGCompat.RecordFrame(count);
  ProcessFrame(sc3, count > 0); sc3->Release();
}

namespace DXGIShared {
void HandleDX12ProcessFrame(IDXGISwapChain* pSwapChain, bool isRealFrame) { DX12_ProcessFrameExternal(pSwapChain); }
void HandleDX12ResizeBegin() { DX12_OnSwapchainResizeBegin(); }
} 

static const GUID SKID_D3D12SwapChainBufferBitmap = { 0xbc53df3b, 0x956f, 0x47db, { 0xa6, 0x53, 0x5, 0xd7, 0xb8, 0x71, 0x53, 0x38 } };
void STDMETHODCALLTYPE DetourExecuteCommandLists(ID3D12CommandQueue *pThis, UINT NumCommandLists, ID3D12CommandList *const *ppCommandLists) {
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
              UINT size = sizeof(uint16_t); uint16_t bitmap = 0;
              pThis->GetPrivateData(SKID_D3D12SwapChainBufferBitmap, &size, &bitmap);
              bitmap |= (1 << idx);
              pThis->SetPrivateData(SKID_D3D12SwapChainBufferBitmap, sizeof(uint16_t), &bitmap);
              
              auto CountBits = [](uint16_t n) { int c = 0; while (n > 0) { n &= (n - 1); c++; } return c; };
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

void HookQueueVTable(ID3D12CommandQueue* queue) {
    if (!queue) return;
    void* unwrapped = nullptr;
    static const GUID IID_CWrapD3D12CommandQueue = { 0xd4e5f678, 0x90ab, 0xcdef, { 0x12, 0x34, 0x56, 0x78, 0x90, 0x12, 0x34, 0x56 } };
    if (SUCCEEDED(queue->QueryInterface(IID_CWrapD3D12CommandQueue, &unwrapped))) { ((IUnknown*)unwrapped)->Release(); return; }
    static std::recursive_mutex s_HookMutex; std::lock_guard<std::recursive_mutex> lock(s_HookMutex);
    void** vtbl = *reinterpret_cast<void***>(queue);
    if (vtbl[10] != (void*)DetourExecuteCommandLists) VTableHook::Create(&vtbl[10], (LPVOID)DetourExecuteCommandLists, (LPVOID*)&oExecuteCommandLists);
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory *pThis, IUnknown *pDevice, DXGI_SWAP_CHAIN_DESC *pDesc, IDXGISwapChain **ppSwapChain) {
  if (pDevice) { ID3D12CommandQueue* q = nullptr; if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&q)))) { HookQueueVTable(q); q->Release(); } }
  HRESULT hr = oCreateSwapChain(pThis, pDevice, pDesc, ppSwapChain);
  if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) { IDXGISwapChain3* sc3 = nullptr; if (SUCCEEDED((*ppSwapChain)->QueryInterface(IID_PPV_ARGS(&sc3)))) { DXGIShared::InstallHooks(sc3); sc3->Release(); } }
  return hr;
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2 *pThis, IUnknown *pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFDesc, IDXGIOutput *pOut, IDXGISwapChain1 **ppSC) {
    if (pDevice) { ID3D12CommandQueue* q = nullptr; if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&q)))) { HookQueueVTable(q); q->Release(); } }
    HRESULT hr = oCreateSwapChainForHwnd(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
    if (SUCCEEDED(hr) && ppSC && *ppSC) { IDXGISwapChain3* sc3 = nullptr; if (SUCCEEDED((*ppSC)->QueryInterface(IID_PPV_ARGS(&sc3)))) { DXGIShared::InstallHooks(sc3); sc3->Release(); } }
    return hr;
}

void STDMETHODCALLTYPE DetourCreateSampler(ID3D12Device *pDevice, const D3D12_SAMPLER_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) { if (oCreateSampler) oCreateSampler(pDevice, pDesc, DestDescriptor); }
HRESULT WINAPI DetourSerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* pRootSignature, D3D_ROOT_SIGNATURE_VERSION Version, ID3DBlob** ppBlob, ID3DBlob** ppErrorBlob) { if (oSerializeRootSignature) return oSerializeRootSignature(pRootSignature, Version, ppBlob, ppErrorBlob); return E_FAIL; }
HRESULT WINAPI DetourSerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pRootSignature, ID3DBlob** ppBlob, ID3DBlob** ppErrorBlob) { if (oSerializeVersionedRootSignature) return oSerializeVersionedRootSignature(pRootSignature, ppBlob, ppErrorBlob); return E_FAIL; }
HRESULT STDMETHODCALLTYPE DetourCreateCommittedResource(ID3D12Device *device, const D3D12_HEAP_PROPERTIES *pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE *pOptimizedClearValue, REFIID riidResource, void **ppvResource) { if (oCreateCommittedResource) return oCreateCommittedResource(device, pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, riidResource, ppvResource); return E_FAIL; }

void DX12Hook::Shutdown() {
  CleanupResources(); ShutdownImGui(); CleanupOverlay(); CleanupRTVs();
  { std::lock_guard<std::recursive_mutex> lock(g_DeviceQueuesMutex); for (auto& pair : g_DeviceQueues) if (pair.second) pair.second->Release(); g_DeviceQueues.clear(); }
  if (g_CommandQueue) { g_CommandQueue->Release(); g_CommandQueue = nullptr; }
  if (g_Device) { g_Device->Release(); g_Device = nullptr; }
  if (g_LastSwapChain) { g_LastSwapChain->Release(); g_LastSwapChain = nullptr; }
  if (g_SharedCaptureD3D12.IsActive()) g_SharedCaptureD3D12.Reset();
  g_IPCReady = false;
}

void DX12Hook::OnHostDisconnect() { g_IPCReady = false; }
void DX12Hook::TrackResource(IUnknown* res) { if (!res) return; std::lock_guard<std::recursive_mutex> lock(resourceMutex); res->AddRef(); trackedResources.push_back(res); }
void DX12Hook::CleanupResources() { std::lock_guard<std::recursive_mutex> lock(resourceMutex); for (auto* res : trackedResources) if (res) res->Release(); trackedResources.clear(); }

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
