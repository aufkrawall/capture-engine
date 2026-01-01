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
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"
#include "imgui.h"
#include <MinHook.h>
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

// static bool g_ImGuiInit = false;  // Moved to DX12OverlayState
// static int g_ImGuiInitFrameCounter = 0; // Moved to DX12OverlayState

// FG Real Frame Detection
static std::atomic<int> g_CommandListsExecutedThisFrame{0};
static std::chrono::steady_clock::time_point g_LastResourceCleanup;
static constexpr int INIT_COOLDOWN_MS = 200; // Wait 200ms after cleanup before re-init

static std::map<ID3D12Device*, ID3D12CommandQueue*> g_DeviceQueues;
static std::mutex g_DeviceQueuesMutex;

// --- Capture Resources ---
class DX12Capture : public HookCaptureBase {
public:
  ID3D12Resource *sharedTextures[CAPTURE_TEXTURE_COUNT] = {};
  ID3D12Resource *backBufferCache[16] = {nullptr};
  UINT backBufferCount = 0;
  ID3D12Fence *fence = nullptr;
  ID3D12CommandQueue *captureQueue = nullptr;
  ID3D12Fence *gameSyncFence = nullptr;
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
  UINT cachedWidth = 0;
  UINT cachedHeight = 0;
  bool imGuiInit = false;
  int imGuiInitFrameCounter = 0;

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

// --- Helper Functions ---

void ShutdownImGui() {
  if (!g_State.imGuiInit) return;
  HookLog("DX12: Shutting down ImGui...");
  ImGui_ImplDX12_Shutdown();
  g_SharedOverlay.ShutdownImGui();
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
  HookLog("ImGui Initialized with Command Queue: %p", g_CommandQueue);
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
  g_SharedOverlay.SetGraphicsAPI("DX12");
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

  if (!g_DX12Capture.gameSyncFence)
    g_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                          IID_PPV_ARGS(&g_DX12Capture.gameSyncFence));
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

  g_State.rtvDescriptorSize =
      device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
      g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();

  g_State.backBuffers.resize(bufferCount);
  for (int i = 0; i < bufferCount; i++) {
    swapChain->GetBuffer(i, IID_PPV_ARGS(&g_State.backBuffers[i]));
    device->CreateRenderTargetView(g_State.backBuffers[i], nullptr, rtvHandle);
    rtvHandle.ptr += g_State.rtvDescriptorSize;
  }
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
  const int poolSize = g_FGCompat.IsFGLikelyActive() ? 16 : DX12OverlayState::ALLOC_POOL_SIZE;
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
  
  // CRITICAL: Force ProcessFrame to re-evaluate initialization safety (FG check)
  // by resetting this flag. Prevents ResizeBuffers from blindly re-initializing.
  g_State.imGuiInit = false;
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

void AsyncCaptureThreadProc() {
  HookLog("AsyncCaptureThread Started");
  g_DX12Capture.captureThreadRunning = true;
  while (!g_DX12Capture.captureThreadShutdown) {
    // Wait indefinitely for frame signal - more responsive than 16ms timeout
    // At 120+ FPS, frames arrive every 8ms, so 16ms timeout could miss frames
    DWORD waitResult = WaitForSingleObject(g_DX12Capture.captureEvent, INFINITE);
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

      std::lock_guard<std::mutex> lock(g_DX12CaptureMutex);
      if (!g_DX12Capture.initialized) {
        g_DX12Capture.pendingReadIdx.store(rIdx + 1, std::memory_order_release);
        continue;
      }

      // Core processing...
      ID3D12CommandQueue *activeQueue = g_DX12Capture.captureQueue;
      if (!activeQueue && g_CommandQueue)
        activeQueue = g_CommandQueue; // Fallback
      if (!activeQueue) {
        g_DX12Capture.pendingReadIdx.store(rIdx + 1, std::memory_order_release);
        continue;
      }

      if (activeQueue && g_DX12Capture.gameSyncFence && frame.fenceValue > 0) {
        activeQueue->Wait(g_DX12Capture.gameSyncFence, frame.fenceValue);
      }

      ID3D12Resource *pBackBuffer = nullptr;
      if (frame.backBufferIndex < g_DX12Capture.backBufferCount)
        pBackBuffer = g_DX12Capture.backBufferCache[frame.backBufferIndex];
      if (!pBackBuffer) {
        g_DX12Capture.pendingReadIdx.store(rIdx + 1, std::memory_order_release);
        continue;
      }

      // Copy logic
      int writeIdx = g_DX12Capture.writeIndex;
      
      ID3D12Resource *writeTexture = g_DX12Capture.sharedTextures[writeIdx];

      g_DX12Capture.cmdAlloc[writeIdx]->Reset();
      g_DX12Capture.cmdList->Reset(g_DX12Capture.cmdAlloc[writeIdx], nullptr);

      D3D12_RESOURCE_BARRIER barriers[2] = {};
      barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barriers[0].Transition.pResource = pBackBuffer;
      // Use COMMON state - more resilient than assuming PRESENT state
      // COMMON is implicitly promotable for copy operations
      barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
      barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

      barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barriers[1].Transition.pResource = writeTexture;
      barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
      barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

      g_DX12Capture.cmdList->ResourceBarrier(2, barriers);
      g_DX12Capture.cmdList->CopyResource(writeTexture, pBackBuffer);

      std::swap(barriers[0].Transition.StateBefore,
                barriers[0].Transition.StateAfter);
      std::swap(barriers[1].Transition.StateBefore,
                barriers[1].Transition.StateAfter);
      g_DX12Capture.cmdList->ResourceBarrier(2, barriers);
      g_DX12Capture.cmdList->Close();

      ID3D12CommandList *lists[] = {g_DX12Capture.cmdList};
      activeQueue->ExecuteCommandLists(1, lists);

      g_DX12Capture.fenceValue++;
      activeQueue->Signal(g_DX12Capture.fence, g_DX12Capture.fenceValue);

      if (g_DX12CaptureCompletionFence && frame.completionFenceValue > 0) {
        activeQueue->Signal(g_DX12CaptureCompletionFence,
                            frame.completionFenceValue);
      }

      g_DX12Capture.writeIndex = (g_DX12Capture.writeIndex + 1) % CAPTURE_TEXTURE_COUNT;

      // Signal frame ready in IPC ring buffer
      g_DX12Capture.SignalFrameReady(g_IPC, writeIdx, frame.timestampQPC,
                                 g_DX12Capture.fenceValue);

      // Done processing
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


  // 1. Determine active device and detect change (FSR-FG Proxying)
  ID3D12Device* activeDevice = nullptr;
  if (!pSwapChain || FAILED(pSwapChain->GetDevice(IID_PPV_ARGS(&activeDevice)))) {
      return;
  }
  
  if (activeDevice != g_Device || pSwapChain != g_LastSwapChain) {
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

      if (g_LastSwapChain) g_LastSwapChain->Release();
      g_LastSwapChain = pSwapChain;
      g_LastSwapChain->AddRef();

      g_State.imGuiInit = false;
      g_State.imGuiInitFrameCounter = 0;
      
      // Release the local ref from GetDevice
      activeDevice->Release();
      HookLog("DX12: ProcessFrame: Device/SC Updated. g_Device=%p, g_LastSwapChain=%p (Refs Added)", g_Device, g_LastSwapChain);
      
      // ENSURE EARLY EXIT: Resources (Command List, Allocators) have been released.
      return;
  }
  activeDevice->Release();

  // Initialize overlay as early as possible (before queue check)
  // CRITICAL: Do NOT initialize ImGui if FG is active or suspended (e.g. at startup)
  bool fgActive = g_FGCompat.IsFGLikelyActive();
  if (!g_State.imGuiInit && !fgActive && g_Device) {
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

  // 2. Get the correct queue for this active device
  ID3D12CommandQueue* targetQueue = nullptr;
  {
      std::lock_guard<std::mutex> devLock(g_DeviceQueuesMutex);
      if (g_DeviceQueues.count(g_Device)) {
          targetQueue = g_DeviceQueues[g_Device];
      }
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

    // Overlay - only render if IPC is valid and overlay enabled
    // CRITICAL: Skip overlay when FG is active - FG runtimes also call ExecuteCommandLists
    // so we cannot reliably detect "real" frames. Our overlay commands cause GPU crashes
    // because FG hijacks the swapchain and backbuffers in ways we don't understand.
    SharedMemoryLayout* shm = g_IPC->GetSharedMem();
    if (!fgActive && processCapture && shm && shm->overlayConfig.showOverlay && g_State.syncInit && 
        g_State.fence && g_CommandQueue) {
      int bufferIdx = pSwapChain->GetCurrentBackBufferIndex();
      
      static int logCounter = 0;
      if (logCounter++ % 1000 == 0) {
        HookLog("DX12: bufferIdx=%d, bufferCount=%zu", bufferIdx, g_State.backBuffers.size());
      }
      
      if (bufferIdx >= (int)g_State.backBuffers.size()) {
        return; 
      }
      
      // Use rotating allocator index (8 allocators) to ensure GPU has finished
      // by the time we come back to the same allocator
      int allocIdx = g_State.allocIndex;
      g_State.allocIndex = (g_State.allocIndex + 1) % DX12OverlayState::ALLOC_POOL_SIZE;
      
      // Wait for this allocator's previous work to complete (will be instant with 8 allocators)
      UINT64 completed = g_State.fence->GetCompletedValue();
      UINT64 target = g_State.fenceValues[allocIdx];
      if (completed < target) {
        // Should rarely happen with 8 allocators - use event wait if needed
        g_State.fence->SetEventOnCompletion(target, g_State.fenceEvent);
        WaitForSingleObject(g_State.fenceEvent, INFINITE);
      }

      auto *alloc = g_State.allocators[allocIdx];
      auto *list = g_State.cmdList;
      alloc->Reset();
      list->Reset(alloc, nullptr);

      D3D12_CPU_DESCRIPTOR_HANDLE rtv =
          g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
      rtv.ptr += (SIZE_T)bufferIdx * g_State.rtvDescriptorSize;
      
      // BARRIER 1: Transition to RENDER_TARGET
      D3D12_RESOURCE_BARRIER preBarrier = {};
      preBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      preBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
      preBarrier.Transition.pResource = g_State.backBuffers[bufferIdx];
      preBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      preBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
      preBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
      list->ResourceBarrier(1, &preBarrier);

      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

      D3D12_VIEWPORT vp = {0, 0, (float)g_State.cachedWidth, (float)g_State.cachedHeight,
                           0, 1};
      list->RSSetViewports(1, &vp);
      D3D12_RECT scissor = {0, 0, (LONG)g_State.cachedWidth, (LONG)g_State.cachedHeight};
      list->RSSetScissorRects(1, &scissor);
      
      DrawOverlay(list);

      // BARRIER 2: Transition back to PRESENT
      D3D12_RESOURCE_BARRIER postBarrier = {};
      postBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      postBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
      postBarrier.Transition.pResource = g_State.backBuffers[bufferIdx];
      postBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      postBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
      postBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
      list->ResourceBarrier(1, &postBarrier);

      list->Close();

      ID3D12CommandList *lists[] = {list};
      
      // Execute on game's queue directly (simpler, more compatible)
      g_CommandQueue->ExecuteCommandLists(1, lists);
      
      // Signal fence for this allocator
      g_State.currentFenceValue++;
      g_State.fenceValues[allocIdx] = g_State.currentFenceValue;
      g_CommandQueue->Signal(g_State.fence, g_State.currentFenceValue);
    }

    // Recording
    // CRITICAL: Skip capture when FG is active - same reason as overlay skip above
    if (!fgActive && processCapture && g_IPC->IsRecording() && g_DX12Capture.initialized) {
      if (!g_DX12Capture.captureThreadRunning) {
        g_DX12Capture.StartCaptureThread(AsyncCaptureThreadProc);
      }

      if (g_DX12Capture.gameSyncFence) {
        g_DX12Capture.gameSyncValue++;
        g_CommandQueue->Signal(g_DX12Capture.gameSyncFence,
                               g_DX12Capture.gameSyncValue);
      }

      // Enqueue
      LARGE_INTEGER qpc;
      QueryPerformanceCounter(&qpc);

      // Use existing EnqueueFrame helper
      g_DX12Capture.EnqueueFrame(qpc.QuadPart, g_DX12Capture.gameSyncValue,
                             pSwapChain->GetCurrentBackBufferIndex(),
                             pSwapChain);
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
  HookLog("DX12: DetourPresent called (SyncInterval=%u, Flags=%u)", SyncInterval, Flags);
  static int64_t qpcFreq = 0;
  if (qpcFreq == 0) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpcFreq = f.QuadPart;
  }
  LARGE_INTEGER qpc;
  QueryPerformanceCounter(&qpc);

  // FG: Record every present call to track output FPS
  g_FGCompat.RecordPresentCall();

  // FG: Check if this present call had associated command lists executed
  // If not, it's likely an interpolated frame
  int cmdListCount = g_CommandListsExecutedThisFrame.exchange(0);
  bool isRealFrame = (cmdListCount > 0);
  
  if (isRealFrame) {
      g_FGCompat.RecordRealFrame();
  }

  // Convert to microseconds
  int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;

  // Initialize CSV logging once - only if debug logging is enabled
  static bool csvLoggingInitialized = false;
  SharedMemoryLayout* csvShm = (g_IPC) ? g_IPC->GetSharedMem() : nullptr;
  if (!csvLoggingInitialized && csvShm && csvShm->debugLogging) {
    char modulePath[MAX_PATH];
    HMODULE hModule = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&DetourPresent, &hModule);
    if (hModule) {
      GetModuleFileNameA(hModule, modulePath, MAX_PATH);
      char *lastSlash = strrchr(modulePath, '\\');
      if (lastSlash) {
        *lastSlash = '\0';
        strcat(modulePath, "\\logs");
        CreateDirectoryA(modulePath, NULL);
        strcat(modulePath, "\\frame_times.csv");
        g_PerfMetrics.EnableCSVLogging(modulePath);
        HookLog("DX12: Frame time CSV logging enabled (%s)", modulePath);
      }
    }
    csvLoggingInitialized = true;
  }

  g_PerfMetrics.Update(us);

  // Update recording state for CSV logging
  bool isRecording = g_IPC && g_IPC->IsRecording();
  g_PerfMetrics.SetRecording(isRecording);

  ProcessFrame(pSwapChain, isRealFrame);

  // Apply shared FPS limiter
  g_SharedFpsLimiter.SetIPCClient(g_IPC);
  g_SharedFpsLimiter.Apply();

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

  // Apply Prerender Limit (Hybrid Pacing)
  float limit = GetActivePrerenderLimit();
  if (limit >= 0.0f) {
      static bool prerenderLimitSet = false;
      static float lastLimit = -2.0f;
      
      if (fabs(limit - lastLimit) > 0.001f) {
           UINT effectiveLatency = (limit < 1.0f) ? 1 : (UINT)limit;
           if (effectiveLatency < 1) effectiveLatency = 1;

           pSwapChain->SetMaximumFrameLatency(effectiveLatency);
           prerenderLimitSet = true;
           HookLog("DX12: Set maximum frame latency to %d (Active Limit: %.2f)", effectiveLatency, limit);
           lastLimit = limit;
      }
      
      // Hybrid Pacing (Limit 0.x)
      if (limit > 0.01f && limit < 1.0f) {
           float fps = g_PerfMetrics.GetCurrentFPS();
           double avgFrameTimeUs = (fps > 1.0f) ? (1000000.0 / fps) : 16666.0;
           int64_t sleepUs = (int64_t)(avgFrameTimeUs * (1.0 - limit) * 0.70); // 0.70 Safety Factor
           if (sleepUs > 0) PrecisionSleep(sleepUs);
      }
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

  HRESULT hr = oPresent(pSwapChain, SyncInterval, Flags);
  return hr;
}

HRESULT STDMETHODCALLTYPE
DetourPresent1(IDXGISwapChain3 *pSwapChain, UINT SyncInterval, UINT Flags,
               const DXGI_PRESENT_PARAMETERS *pPresentParameters) {
  HookLog("DX12: DetourPresent1 called (SyncInterval=%u, Flags=%u)", SyncInterval, Flags);
  // FG: Record Present call
  g_FGCompat.RecordPresentCall();
  
  // FG: Real frame detection
  int cmdListCount = g_CommandListsExecutedThisFrame.exchange(0);
  bool isRealFrame = (cmdListCount > 0);
  if (isRealFrame) g_FGCompat.RecordRealFrame();

  ProcessFrame(pSwapChain, isRealFrame);

  // Apply shared FPS limiter
  g_SharedFpsLimiter.SetIPCClient(g_IPC);
  g_SharedFpsLimiter.Apply();

  UINT oldInterval = SyncInterval;
  UINT oldFlags = Flags;

  // Override VSync for Present1 too
  if (g_IPC) {
      auto* shm = g_IPC->GetSharedMem();
      if (shm) {
          const char* mode = shm->graphicsConfig.vsyncMode;
          if (mode[0] != 'd') { // not default
              if (mode[0] == 'o' && strncmp(mode, "off", 3) == 0) SyncInterval = 0;
              else if (mode[0] == 'f' && strncmp(mode, "fifo", 4) == 0) SyncInterval = 1;
              else if (mode[0] == 'a' && strncmp(mode, "adaptive", 8) == 0) SyncInterval = 0;
              else if (mode[0] == 'm' && strncmp(mode, "mailbox", 7) == 0) SyncInterval = 0;
          }
      }
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
    
    // Debug Log for Sampler Creation
    static int samplerLogCount = 0;
    if (samplerLogCount < 5) {
        HookLog("DX12: CreateSampler called");
        samplerLogCount++;
    }

    D3D12_SAMPLER_DESC desc = *pDesc;
    bool modified = false;

    if (g_IPC) {
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
        const char* bias = gfx.mipBias.c_str();
        if (bias[0] != 'd') {
             char* end;
             float val = strtof(bias, &end);
             if (end != bias) {
                desc.MipLODBias = val;
                modified = true;
                HookLog("DX12: CreateSampler: Forced MipBias %.2f", val);
             }
        }

        // SGSSAA Bias
        float sgssaaBias = 0.0f;
        if (GetSGSSAABias(gfx.sgssaa, gfx.msaaSamples.c_str(), sgssaaBias)) {
             desc.MipLODBias += sgssaaBias;
             modified = true;
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
                switch (sampler.Filter) {
                    case D3D12_FILTER_MIN_MAG_MIP_LINEAR:
                    case D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT:
                        newFilter = D3D12_FILTER_ANISOTROPIC;
                        break;
                    case D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR:
                    case D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT:
                        newFilter = D3D12_FILTER_COMPARISON_ANISOTROPIC;
                        break;
                }
                sampler.Filter = newFilter;
                sampler.MaxAnisotropy = maxAniso;
                HookLog("DX12: Static Sampler: Forced AF %dx", maxAniso);
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
    std::string bias = gfx.mipBias;
    if (bias != "default") {
        try {
            sampler.MipLODBias += std::stof(bias);
            HookLog("DX12: Static Sampler: Forced MipBias %.2f", sampler.MipLODBias);
        } catch(...) {}
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
    
    HookLog("DX12: SerializeRootSignature intercepted (%d static samplers)", pRootSignature->NumStaticSamplers);
    
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
    
    HookLog("DX12: SerializeVersionedRootSignature intercepted (Version=%d, %d static samplers)", 
            pRootSignature->Version, numSamplers);
    
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
  g_CommandListsExecutedThisFrame.fetch_add(1, std::memory_order_relaxed);

  /*
  if (pThis && pThis->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
    ID3D12Device* dev = nullptr;
    if (SUCCEEDED(pThis->GetDevice(IID_PPV_ARGS(&dev)))) {
        std::lock_guard<std::mutex> lock(g_DeviceQueuesMutex);
        if (g_DeviceQueues.count(dev) == 0 || g_DeviceQueues[dev] != pThis) {
             if (g_DeviceQueues.count(dev)) g_DeviceQueues[dev]->Release();
             g_DeviceQueues[dev] = pThis;
             pThis->AddRef();
        }
        dev->Release();
    }
    
    if (g_CommandQueue != pThis) {
        if (g_CommandQueue) g_CommandQueue->Release();
        g_CommandQueue = pThis;
        g_CommandQueue->AddRef();
        HookLog("DX12: ExecuteCommandLists: Updated g_CommandQueue to %p", g_CommandQueue);
    }
  }
  */
  HookLog("DX12: Calling original ExecuteCommandLists...");
  oExecuteCommandLists(pThis, NumCommandLists, ppCommandLists);
}

typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChain)(IDXGIFactory *, IUnknown *,
                                              DXGI_SWAP_CHAIN_DESC *,
                                              IDXGISwapChain **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChainForHwnd)(
    IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);

PFN_CreateSwapChain oCreateSwapChain = nullptr;
PFN_CreateSwapChainForHwnd oCreateSwapChainForHwnd = nullptr;

HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory *pThis,
                                                IUnknown *pDevice,
                                                DXGI_SWAP_CHAIN_DESC *pDesc,
                                                IDXGISwapChain **ppSwapChain) {
  if (!pDesc) return DXGI_ERROR_INVALID_CALL;
  if (pDevice) {
    ID3D12CommandQueue *queue = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue),
                                          (void **)&queue))) {
      // Release old queue before capturing new one
      if (g_CommandQueue) g_CommandQueue->Release();
      g_CommandQueue = queue; // Keep the reference from QueryInterface
      
      ID3D12Device* dev = nullptr;
      if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&dev)))) {
          std::lock_guard<std::mutex> lock(g_DeviceQueuesMutex);
          // If we already have a queue for this device, release it
          if (g_DeviceQueues.count(dev)) g_DeviceQueues[dev]->Release();
          g_DeviceQueues[dev] = queue; 
          queue->AddRef(); // One for the map
          
          if (g_Device) g_Device->Release();
          g_Device = dev; // Keep the reference from GetDevice
          // HookLog("CreateSwapChain: Captured Device %p", g_Device);
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
  
  if (g_IPC) {
      // Debug: Check Present Address
      if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
           void **vtbl = *reinterpret_cast<void ***>(*ppSwapChain);
           HookLog("DX12: Game SwapChain Present Address: %p", vtbl[8]);
           HookLog("DX12: Game SwapChain ResizeBuffers Address: %p", vtbl[13]);
      }
  }

  return hr;
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(
    IDXGIFactory2 *pThis, IUnknown *pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1 *pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
    IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) {
  if (!pDesc) return DXGI_ERROR_INVALID_CALL;
  if (pDevice) {
    ID3D12CommandQueue *queue = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue),
                                          (void **)&queue))) {
      // Release old queue before capturing new one
      if (g_CommandQueue) g_CommandQueue->Release();
      g_CommandQueue = queue; // Keep the reference from QueryInterface
      
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
      HookLog("CreateSwapChainForHwnd: Captured Queue %p", g_CommandQueue);
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

  return oCreateSwapChainForHwnd(pThis, pDevice, hWnd, &modifiedDesc, pFullscreenDesc,
                                 pRestrictToOutput, ppSwapChain);
}

HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain3 *pSwapChain,
                                              UINT BufferCount, UINT Width,
                                              UINT Height,
                                              DXGI_FORMAT NewFormat,
                                              UINT SwapChainFlags) {
  if (!pSwapChain) return DXGI_ERROR_INVALID_CALL;

  HookLog("DX12: DetourResizeBuffers entering...");
  
  if (g_DummyBackBuffer) {
      g_DummyBackBuffer->Release();
      g_DummyBackBuffer = nullptr;
  }

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
PFN_D3D12_CREATE_DEVICE oD3D12CreateDevice = nullptr;

HRESULT WINAPI DetourD3D12CreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid, void** ppDevice) {
    HRESULT hr = oD3D12CreateDevice(pAdapter, MinimumFeatureLevel, riid, ppDevice);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        HookLog("DX12: Capturing Device via D3D12CreateDevice hook");
        ID3D12Device* dev = (ID3D12Device*)*ppDevice;
        void** vtbl = *reinterpret_cast<void***>(dev);
        
        static bool hookedSampler = false;
        if (!hookedSampler) {
            // Index 22 is CreateSampler in ID3D12Device
            MH_STATUS s = MH_CreateHook(vtbl[22], (LPVOID)DetourCreateSampler, (LPVOID*)&oCreateSampler);
            if (s == MH_OK) {
                MH_EnableHook(vtbl[22]);
                HookLog("DX12: Hooked CreateSampler (early export)");
                hookedSampler = true;
            } else if (s == MH_ERROR_ALREADY_CREATED) {
                 hookedSampler = true;
            }
        }
        
        static bool hookedCreateResource = false;
        if (!hookedCreateResource) {
            // Index 27 is CreateCommittedResource
            MH_STATUS s = MH_CreateHook(vtbl[27], (LPVOID)DetourCreateCommittedResource, (LPVOID*)&oCreateCommittedResource);
            if (s == MH_OK) {
                MH_EnableHook(vtbl[27]);
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
  HookLog("DX12Hook::Init() called.");

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
  s = MH_CreateHook(facVTable[10], (LPVOID)DetourCreateSwapChain,
                (LPVOID *)&oCreateSwapChain);
  if (s != MH_OK) HookLog("Failed to hook CreateSwapChain: %s", MH_StatusToString(s));

  s = MH_CreateHook(facVTable[15], (LPVOID)DetourCreateSwapChainForHwnd,
                (LPVOID *)&oCreateSwapChainForHwnd);
  if (s != MH_OK) HookLog("Failed to hook CreateSwapChainForHwnd: %s", MH_StatusToString(s));
  
  s = MH_CreateHook(scVTable[9], (LPVOID)DetourGetBuffer, (LPVOID *)&oGetBuffer);
  if (s != MH_OK) HookLog("Failed to hook GetBuffer: %s", MH_StatusToString(s));

  s = MH_CreateHook(scVTable[8], (LPVOID)DetourPresent, (LPVOID *)&oPresent);
  if (s != MH_OK) HookLog("Failed to hook Present: %s", MH_StatusToString(s));
  
  s = MH_CreateHook(scVTable[22], (LPVOID)DetourPresent1, (LPVOID *)&oPresent1);
  if (s != MH_OK) HookLog("Failed to hook Present1: %s", MH_StatusToString(s));
  
  s = MH_CreateHook(scVTable[13], (LPVOID)DetourResizeBuffers,
                (LPVOID *)&oResizeBuffers);
  if (s != MH_OK) HookLog("Failed to hook ResizeBuffers: %s", MH_StatusToString(s));
  
  s = MH_CreateHook(cqVTable[10], (LPVOID)DetourExecuteCommandLists,
                (LPVOID *)&oExecuteCommandLists);
  if (s != MH_OK) HookLog("Failed to hook ExecuteCommandLists: %s", MH_StatusToString(s));
  
  // Hook CreateSampler (Index 22)
  MH_CreateHook(devVTable[22], (LPVOID)DetourCreateSampler, (LPVOID *)&oCreateSampler);

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

  // Early FG Detection
  if (g_FGCompat.DetectLoadedFGRuntime() != FGCompatibility::FGType::None) {
      HookLog("DX12Hook: FG Runtime detected at init - triggering safety suspend");
      g_FGCompat.SuspendFor(5000); // 5 seconds suspend on startup if FG is present
  }

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
  
  if (g_LastSwapChain) { g_LastSwapChain->Release(); g_LastSwapChain = nullptr; }
  
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
