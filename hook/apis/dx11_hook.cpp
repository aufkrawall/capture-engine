#include "dx11_hook.h"
#include "lod_helper.h"
#include "../common/capture_base.h"
#include "../common/fps_limiter.h"
#include "../common/overlay.h"
#include "hook_common.h"
#include "../../common/frame_timing.h"
#include "performance_metrics.h"
#include <MinHook.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <cstdint>
#include <cstdio>
#include <d3d10.h>    // For DX10 detection
#include <d3d10_1.h>  // For DX10.1 detection
#include <d3d11.h>
#include <d3d11_1.h> // For ID3D11DeviceContext1
#include <d3d11_4.h> // For ID3D11Fence and ID3D11Device5
#include <dxgi1_2.h> // For LUID
#include <imgui.h>

// Globals
static ID3D11Device *g_pd3dDevice = NULL;
static ID3D11DeviceContext *g_pd3dDeviceContext = NULL;
static IDXGISwapChain *g_pSwapChain = NULL;
static ID3D11RenderTargetView *g_mainRenderTargetView = NULL;

// DX10 vs DX11 detection
static bool g_IsDX10Device = false;
static const char* g_DetectedAPI = "DX11"; // Will be set to "DX10" if DX10 device detected

typedef HRESULT(STDMETHODCALLTYPE *Present_t)(IDXGISwapChain *pSwapChain,
                                              UINT SyncInterval, UINT Flags);
typedef HRESULT(STDMETHODCALLTYPE *ResizeBuffers_t)(IDXGISwapChain *pSwapChain,
                                                    UINT BufferCount, UINT Width,
                                                    UINT Height, DXGI_FORMAT NewFormat,
                                                    UINT SwapChainFlags);
static Present_t oPresent = NULL;
typedef HRESULT(STDMETHODCALLTYPE *Present1_t)(IDXGISwapChain *pSwapChain,
                                               UINT SyncInterval, UINT PresentFlags,
                                               const DXGI_PRESENT_PARAMETERS *pPresentParameters);
static Present1_t oPresent1 = NULL;
typedef HRESULT(STDMETHODCALLTYPE *CreateSamplerState_t)(ID3D11Device *pDevice,
                                                         const D3D11_SAMPLER_DESC *pSamplerDesc,
                                                         ID3D11SamplerState **ppSamplerState);
static CreateSamplerState_t oCreateSamplerState = NULL;
typedef HRESULT(WINAPI *D3D11CreateDeviceAndSwapChain_t)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
    UINT, UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **,
    ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

static D3D11CreateDeviceAndSwapChain_t oD3D11CreateDeviceAndSwapChain = NULL;

static HRESULT WINAPI DetourD3D11CreateDeviceAndSwapChain(
    IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
    IDXGISwapChain **ppSwapChain, ID3D11Device **ppDevice,
    D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext) {
    
    EarlyLog("DX11: D3D11CreateDeviceAndSwapChain called");
    
    DXGI_SWAP_CHAIN_DESC desc;
    const DXGI_SWAP_CHAIN_DESC *pFinalDesc = pSwapChainDesc;
    
    if (pSwapChainDesc) {
        const GraphicsConfig& gfx = GetActiveGraphicsConfig();
        desc = *pSwapChainDesc;
        bool modified = false;
        
        // Backbuffer Count
        int count = gfx.backbufferCount;
        if (count >= 2 && count <= 6) {
            desc.BufferCount = (UINT)count;
            modified = true;
            HookLog("DX11: CreateDeviceAndSwapChain: Overriding BufferCount to %d", count);
        }
        
        // MSAA Override
        const char* msaa = gfx.msaaSamples.c_str();
        if (msaa[0] != 'd') {
            if (strcmp(msaa, "off") == 0) {
                desc.SampleDesc.Count = 1;
                desc.SampleDesc.Quality = 0;
                modified = true;
                HookLog("DX11: CreateDeviceAndSwapChain: Forcing MSAA OFF");
            } else {
                UINT samples = 1;
                if (strcmp(msaa, "2x") == 0) samples = 2;
                else if (strcmp(msaa, "4x") == 0) samples = 4;
                else if (strcmp(msaa, "8x") == 0) samples = 8;
                
                if (samples > 1) {
                    desc.SampleDesc.Count = samples;
                    desc.SampleDesc.Quality = 0;
                    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; // MSAA requires DISCARD in D3D11
                    modified = true;
                    HookLog("DX11: CreateDeviceAndSwapChain: Forcing MSAA %dx", samples);
                }
            }
        }
        
        if (modified) pFinalDesc = &desc;
    }
    
    return oD3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, 
                                           pFeatureLevels, FeatureLevels, SDKVersion, 
                                           pFinalDesc, ppSwapChain, ppDevice, 
                                           pFeatureLevel, ppImmediateContext);
}

static HRESULT STDMETHODCALLTYPE DetourCreateSamplerState(ID3D11Device *pDevice, const D3D11_SAMPLER_DESC *pSamplerDesc, ID3D11SamplerState **ppSamplerState);

// Helper to install vtable hooks
static void InstallVTableHooks(ID3D11Device *pDevice, ID3D11DeviceContext* pContext, IDXGISwapChain* pSwapChain) {
    // Get device vtable
    void **pDeviceVTable = *(void ***)pDevice;
    
    // Hook CreateSamplerState (index 43)
    if (oCreateSamplerState == NULL) { // Only hook once
        if (MH_CreateHook(pDeviceVTable[43], (LPVOID)&DetourCreateSamplerState,
                          (LPVOID *)&oCreateSamplerState) == MH_OK) {
            MH_EnableHook(pDeviceVTable[43]);
            HookLog("DX11: CreateSamplerState hook installed");
    }
    }
}

static ResizeBuffers_t oResizeBuffers = NULL;

static PerformanceMetrics g_PerfMetrics;
static bool g_ImGuiInitialized = false;
static HWND g_CachedHwnd = NULL;

// DX11 Capture Implementation extending HookCaptureBase
// Supports both DX11 and DX10 games - DX10 games require creating a D3D11 device
class DX11Capture : public HookCaptureBase {
public:
  ID3D11Texture2D *sharedTextures[CAPTURE_TEXTURE_COUNT]{};
  ID3D11Query *copyQueries[CAPTURE_TEXTURE_COUNT]{};  // GPU sync queries
  ID3D11Device *cachedDevice = nullptr;
  ID3D11DeviceContext *cachedContext = nullptr;
  
  // For DX10 games: we own a D3D11 device for creating shared textures
  ID3D11Device *ownedDevice = nullptr;
  ID3D11DeviceContext *ownedContext = nullptr;
  bool isDX10Mode = false;
  
  // DX11.3 Fence Support
  ID3D11Fence *fence = nullptr;
  ID3D11DeviceContext4 *context4 = nullptr; // Needed for Signal
  bool useFences = false;
  UINT64 fenceValue = 0;
  
  // sharedTextureHandles are in base class

  void Cleanup() override {
    StopCaptureThread();
    // Close shared handles first to prevent leaks
    CleanupSharedHandles();
    
    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
        if (sharedTextures[i])
            sharedTextures[i]->Release();
        sharedTextures[i] = nullptr;
        if (copyQueries[i])
            copyQueries[i]->Release();
        copyQueries[i] = nullptr;
    }

    if (fence) { fence->Release(); fence = nullptr; }
    if (context4) { context4->Release(); context4 = nullptr; }
    
    // Release owned D3D11 device (used for DX10 mode)
    if (ownedContext) ownedContext->Release();
    ownedContext = nullptr;
    if (ownedDevice) ownedDevice->Release();
    ownedDevice = nullptr;
    
    cachedDevice = nullptr;
    cachedContext = nullptr;
    initialized = false;
    useFences = false;
    isDX10Mode = false;
    fenceValue = 0;  // Reset fence value for next session
  }


  void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override {
    // This virtual method is called by CheckCaptureInit or manually
    // We need the device to create resources, so we'll store it in Init
  }

  // Create a D3D11 device matching the same GPU adapter (for DX10 interop)
  bool CreateD3D11DeviceForAdapter(IDXGIAdapter *adapter) {
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL featureLevel;
    
    HMODULE hD3D11 = LoadLibraryA("d3d11.dll");
    if (!hD3D11) {
        HookLog("DX10: D3D11 DLL not found");
        return false;
    }

    typedef HRESULT (WINAPI *PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
    if (!pD3D11CreateDevice) return false;

    HRESULT hr = pD3D11CreateDevice(
        adapter,
        D3D_DRIVER_TYPE_UNKNOWN,  // Must use UNKNOWN when adapter is specified
        NULL,
        0,
        featureLevels,
        3,
        D3D11_SDK_VERSION,
        &ownedDevice,
        &featureLevel,
        &ownedContext
    );
    
    if (FAILED(hr)) {
        HookLog("DX10: Failed to create D3D11 device for capture (hr=0x%08x)", hr);
        return false;
    }
    
    HookLog("DX10: Created D3D11 device for capture (feature level %d)", featureLevel);
    return true;
  }

  // Initialize for DX10 games - creates D3D11 device on same adapter
  bool InitDX10(IDXGISwapChain *swapChain) {
    // Get adapter from swapchain
    IDXGIDevice *dxgiDevice = nullptr;
    IDXGIAdapter *adapter = nullptr;
    
    // Get the device from swapchain (could be DX10 or DX10.1)
    IUnknown *pDevice = nullptr;
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D10Device), (void**)&pDevice))) {
        if (FAILED(swapChain->GetDevice(__uuidof(ID3D10Device1), (void**)&pDevice))) {
            HookLog("DX10: Failed to get device from swapchain");
            return false;
        }
    }
    
    if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC adapterDesc;
            adapter->GetDesc(&adapterDesc);
            luidLow = adapterDesc.AdapterLuid.LowPart;
            luidHigh = adapterDesc.AdapterLuid.HighPart;
            
            // Create D3D11 device on same adapter
            if (!CreateD3D11DeviceForAdapter(adapter)) {
                adapter->Release();
                dxgiDevice->Release();
                pDevice->Release();
                return false;
            }
            adapter->Release();
        }
        dxgiDevice->Release();
    }
    pDevice->Release();
    
    isDX10Mode = true;
    return true;
  }

  // Specialized Init that takes the device (DX11 path)
  void Init(ID3D11Device *device, IDXGISwapChain *swapChain) {
    if (initialized)
      return;

    DXGI_SWAP_CHAIN_DESC desc;
    swapChain->GetDesc(&desc);

    width = desc.BufferDesc.Width;
    height = desc.BufferDesc.Height;
    format = desc.BufferDesc.Format;

    // Determine which device to use for creating textures
    ID3D11Device *captureDevice = device;
    
    if (isDX10Mode) {
        // Use our owned D3D11 device
        captureDevice = ownedDevice;
        cachedDevice = nullptr;  // We don't cache the DX10 device
        cachedContext = ownedContext;
    } else {
        // Get LUID from DX11 device
        IDXGIDevice *dxgiDevice = nullptr;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
          IDXGIAdapter *adapter = nullptr;
          if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC adapterDesc;
            adapter->GetDesc(&adapterDesc);
            luidLow = adapterDesc.AdapterLuid.LowPart;
            luidHigh = adapterDesc.AdapterLuid.HighPart;
            adapter->Release();
          }
          dxgiDevice->Release();
        }
        cachedDevice = device;
        device->GetImmediateContext(&cachedContext);
    }

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = (DXGI_FORMAT)format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    // Try to enable Fences (DX11.3) - only for DX11 native mode
    if (!isDX10Mode) {
        ID3D11Device5* device5 = nullptr;
        if (SUCCEEDED(captureDevice->QueryInterface(IID_PPV_ARGS(&device5)))) {
            if (SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)))) {
                ID3D11DeviceContext* ctx = nullptr;
                captureDevice->GetImmediateContext(&ctx);
                if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&context4)))) {
                    IDXGIResource* res = nullptr;
                    if (SUCCEEDED(fence->QueryInterface(IID_PPV_ARGS(&res)))) {
                        res->GetSharedHandle(&sharedFenceHandle);
                        res->Release();
                        useFences = true;
                        HookLog("DX11: ID3D11Fence support enabled");
                    }
                }
                ctx->Release();
            }
            device5->Release();
        }
    }
    
    if (!useFences) {
        HookLog("%s: Fence not available, using query-based sync", isDX10Mode ? "DX10" : "DX11");
    }

    bool success = true;
    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
      HRESULT hr = captureDevice->CreateTexture2D(&texDesc, NULL, &sharedTextures[i]);
      if (SUCCEEDED(hr)) {
        IDXGIResource *pResource = NULL;
        sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&pResource));
        pResource->GetSharedHandle(&sharedTextureHandles[i]);
        pResource->Release();
        
        // Create query for GPU sync only if fences are NOT used
        if (!useFences) {
            D3D11_QUERY_DESC queryDesc = {};
            queryDesc.Query = D3D11_QUERY_EVENT;
            captureDevice->CreateQuery(&queryDesc, &copyQueries[i]);
        }
      } else {
        success = false;
        HookLog("%s: Failed to create texture %d (hr=0x%08x)", isDX10Mode ? "DX10" : "DX11", i, hr);
      }
    }

    if (success) {
      if (g_IPC) {
        PublishToSharedMemory(g_IPC);
      }
      initialized = true;
      HookLog("%s Capture Initialized: %dx%d (LUID: %08x)", isDX10Mode ? "DX10" : "DX11", 
              width, height, luidLow);
    }
  }
  
  // Wait for a specific query to complete (with timeout)
  bool WaitForCopy(ID3D11DeviceContext *context, int idx, DWORD timeoutMs = 10) {
    if (!copyQueries[idx]) return true;  // No query = assume complete
    DWORD start = GetTickCount();
    BOOL data = FALSE;
    while (context->GetData(copyQueries[idx], &data, sizeof(data), 0) == S_FALSE) {
      if (GetTickCount() - start > timeoutMs) {
        return false;  // Timeout
      }
      SwitchToThread();  // Yield CPU
    }
    return true;
  }
  
  // Get the context to use for capture operations
  ID3D11DeviceContext* GetCaptureContext() {
    return isDX10Mode ? ownedContext : cachedContext;
  }
};

static DX11Capture g_DX11Capture;

void DrawDX11Overlay(IDXGISwapChain *pSwapChain) {
  // For DX10 games, we need to get a D3D11 device for ImGui
  // Try DX11 first, fallback to creating one for DX10
  if (!g_ImGuiInitialized) {
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    
    // Try to get DX11 device directly
    if (FAILED(pSwapChain->GetDevice(IID_PPV_ARGS(&device)))) {
        // DX10 game - need to create a D3D11 device for ImGui
        // Use the capture class's owned device if available
        if (g_DX11Capture.ownedDevice) {
            device = g_DX11Capture.ownedDevice;
            device->AddRef(); // Balance the Release below
            context = g_DX11Capture.ownedContext;
            context->AddRef();
        } else {
            HookLog("DrawDX11Overlay: No D3D11 device available for DX10 game");
            return;
        }
    } else {
        device->GetImmediateContext(&context);
    }

    DXGI_SWAP_CHAIN_DESC desc;
    pSwapChain->GetDesc(&desc);
    g_CachedHwnd = desc.OutputWindow;

    g_SharedOverlay.InitImGui(desc.OutputWindow);
    ImGui_ImplDX11_Init(device, context);
    g_ImGuiInitialized = true;
    EarlyLog("%s: ImGui initialized", g_DetectedAPI);

    device->Release();
    context->Release();
  }

  ImGui_ImplDX11_NewFrame();
  g_SharedOverlay.BeginFrame();

  // Determine if HDR is active
  DXGI_SWAP_CHAIN_DESC desc;
  pSwapChain->GetDesc(&desc);
  bool isHDR = (desc.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || 
               desc.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
  g_SharedOverlay.SetHDR(isHDR);

  g_SharedOverlay.SetMetrics(&g_PerfMetrics);
  g_SharedOverlay.SetIPCClient(g_IPC);
  g_SharedOverlay.SetDroppedFrames(g_DX11Capture.droppedFrames.load(std::memory_order_relaxed));
  g_SharedOverlay.SetGraphicsAPI(g_DetectedAPI);
  g_SharedOverlay.RenderUI();

  g_SharedOverlay.EndFrame();

  // Set RTV
  ID3D11DeviceContext *context = NULL;
  ID3D11Device *device = NULL;
  pSwapChain->GetDevice(IID_PPV_ARGS(&device));
  device->GetImmediateContext(&context);

  if (!g_mainRenderTargetView) {
    ID3D11Texture2D *backbuffer = nullptr;
    pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    device->CreateRenderTargetView(backbuffer, NULL, &g_mainRenderTargetView);
    backbuffer->Release();
  }

  context->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

  device->Release();
  context->Release();
}

// Handle SwapChain resize - must release RTV and reinitialize ImGui
HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain *pSwapChain,
                                               UINT BufferCount, UINT Width,
                                               UINT Height, DXGI_FORMAT NewFormat,
                                               UINT SwapChainFlags) {
  HookLog("DX11: ResizeBuffers called (%dx%d)", Width, Height);
  
  // Release render target view before resize
  if (g_mainRenderTargetView) {
    g_mainRenderTargetView->Release();
    g_mainRenderTargetView = nullptr;
  }
  
  // Invalidate ImGui device objects (they reference old backbuffer)
  if (g_ImGuiInitialized) {
    ImGui_ImplDX11_InvalidateDeviceObjects();
  }
  
  // Apply backbuffer count override
  int count = GetActiveGraphicsConfig().backbufferCount;
  if (count >= 2 && count <= 6) {
      BufferCount = (UINT)count;
      HookLog("DX11: ResizeBuffers: Overriding BufferCount to %d", count);
  }

  // Call original ResizeBuffers
  HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
  
  // Recreate ImGui device objects after resize
  if (g_ImGuiInitialized && SUCCEEDED(hr)) {
    ImGui_ImplDX11_CreateDeviceObjects();
  }
  
  return hr;
}

HRESULT STDMETHODCALLTYPE DetourDX11Present(IDXGISwapChain *pSwapChain,
                                            UINT SyncInterval, UINT Flags) {
  static int64_t qpcFreq = 0;
  if (qpcFreq == 0) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpcFreq = f.QuadPart;
  }
  if (g_ShuttingDown) return oPresent(pSwapChain, SyncInterval, Flags);

  LARGE_INTEGER qpc;
  QueryPerformanceCounter(&qpc);
  int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;

  // Initialize CSV logging once - only if debug logging is enabled
  static bool csvLoggingInitialized = false;
  IPCClient* ipc = g_IPC;
  SharedMemoryLayout* csvShm = (ipc) ? ipc->GetSharedMem() : nullptr;

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
      
      // Update Latency (Queue Size) if changed
      if (fabs(limit - lastLimit) > 0.001f) {
          ID3D11Device* dev = nullptr;
          if (SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&dev)))) {
              IDXGIDevice1* dxgiDev = nullptr;
              if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) {
                   // Map limit to integer latency
                   // 0 -> 1 (Strict)
                   // 0.5 -> 1 (Buffered + Sleep)
                   // 1 -> 1
                   // 2 -> 2
                   // If limit < 1, we set latency 1 to allow strictly 1 frame (or overlap if < 1).
                   // Wait, "MaximumFrameLatency" of 1 means 1 frame queued? Or 1 frame in flight?
                   // Docs say: "The number of frames that can be queued".
                   // 1 = Serial? No, 1 = 1 buffered.
                   // 0 is invalid/driver default?
                   // Actually DXGI default is 3.
                   
                   UINT effectiveLatency = (limit < 1.0f) ? 1 : (UINT)limit;
                   if (effectiveLatency < 1) effectiveLatency = 1;

                   dxgiDev->SetMaximumFrameLatency(effectiveLatency);
                   dxgiDev->Release();
                   HookLog("DX11: Set maximum frame latency to %d (Active Limit: %.2f)", effectiveLatency, limit);
              }
              dev->Release();
          }
          lastLimit = limit;
      }
      
      // Strict Serial (Limit 0) Logic (DX11)
      // Since Latency=1 allows 1 frame, we need to Wait to ensure queue is empty.
      // But we can only wait on queries/fences.
      // We don't have a reliable "Frame Fence" here unless we inject one?
      // For now, Limit 0 with Latency 1 is "Low Latency" but not "Strict Serial".
      
      // Hybrid Pacing (Limit 0.x)
      if (limit > 0.01f && limit < 1.0f) {
           float fps = g_PerfMetrics.GetCurrentFPS();
           double avgFrameTimeUs = (fps > 1.0f) ? (1000000.0 / fps) : 16666.0;
           int64_t sleepUs = (int64_t)(avgFrameTimeUs * (1.0 - limit) * 0.70); // 0.70 Safety Factor
           if (sleepUs > 0) PrecisionSleep(sleepUs);
      }
  }

  if (!csvLoggingInitialized && csvShm && csvShm->debugLogging) {
      char modulePath[MAX_PATH];
      HMODULE hModule = NULL;
      GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         (LPCSTR)&DetourDX11Present, &hModule);
      if (hModule) {
          GetModuleFileNameA(hModule, modulePath, MAX_PATH);
          char *lastSlash = strrchr(modulePath, '\\');
          if (lastSlash) {
              *lastSlash = '\0';
              strcat(modulePath, "\\logs");
              CreateDirectoryA(modulePath, NULL);
              strcat(modulePath, "\\frame_times.csv");
              g_PerfMetrics.EnableCSVLogging(modulePath);
              HookLog("DX11: Frame time CSV logging enabled (%s)", modulePath);
          }
      }
      csvLoggingInitialized = true;
  }

  g_PerfMetrics.Update(us);
  
  // Update recording state for CSV logging
  bool isRecording = ipc && ipc->IsRecording();
  g_PerfMetrics.SetRecording(isRecording);

  // Capture Logic
  if (ipc && isRecording) {
    // Try to get D3D11 device first; if fails, this is a DX10 game
    ID3D11Device *device = NULL;
    bool isDX10 = false;
    
    if (FAILED(pSwapChain->GetDevice(IID_PPV_ARGS(&device)))) {
        // DX10 game detected
        isDX10 = true;
        g_IsDX10Device = true;
        g_DetectedAPI = "DX10";
    } else {
        g_IsDX10Device = false;
        g_DetectedAPI = "DX11";
    }

    // Lazy init
    if (!g_DX11Capture.initialized) {
      if (isDX10) {
        // DX10 path: create D3D11 device on same adapter first
        if (!g_DX11Capture.InitDX10(pSwapChain)) {
            HookLog("DX10: Failed to initialize capture");
            goto skip_capture;
        }
        g_DX11Capture.Init(nullptr, pSwapChain);
      } else {
        g_DX11Capture.Init(device, pSwapChain);
      }
    }

    // Get backbuffer as D3D11 texture (works for both DX10 and DX11 via DXGI)
    ID3D11Texture2D *backbuffer = nullptr;
    pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));

    if (backbuffer && g_DX11Capture.initialized) {
      int idx = g_DX11Capture.writeIndex;
      ID3D11DeviceContext *captureContext = g_DX11Capture.GetCaptureContext();
      
      if (g_DX11Capture.useFences && g_DX11Capture.fence && g_DX11Capture.context4) {
         // Async Path using Fences (Non-blocking) - DX11 only
         captureContext->CopyResource(g_DX11Capture.sharedTextures[idx], backbuffer);
         
         g_DX11Capture.fenceValue++;
         g_DX11Capture.context4->Signal(g_DX11Capture.fence, g_DX11Capture.fenceValue);
         
         g_DX11Capture.SignalFrameReady(ipc, idx, us / 1000, g_DX11Capture.fenceValue);
         g_DX11Capture.AdvanceWriteIndex();
      } else if (captureContext) {
          // Fallback Path using Queries (Potentially Blocking)
          if (g_DX11Capture.copyQueries[idx]) {
            captureContext->Begin(g_DX11Capture.copyQueries[idx]);
          }
          
          captureContext->CopyResource(g_DX11Capture.sharedTextures[idx], backbuffer);
          
          if (g_DX11Capture.copyQueries[idx]) {
            captureContext->End(g_DX11Capture.copyQueries[idx]);
          }
          
          // Wait for copy to complete 
          g_DX11Capture.WaitForCopy(captureContext, idx, 5);
    
          g_DX11Capture.SignalFrameReady(ipc, idx, us / 1000, 0);
          g_DX11Capture.AdvanceWriteIndex();
      }
    }

    if (backbuffer)
      backbuffer->Release();
    if (device)
      device->Release();
  } else if (g_DX11Capture.initialized) {
    g_DX11Capture.Cleanup();
  }

skip_capture:
  // Draw Overlay - add null check
  SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
  if (shm && shm->overlayConfig.showOverlay) {
    DrawDX11Overlay(pSwapChain);
  }

  // Apply shared FPS limiter
  g_SharedFpsLimiter.SetIPCClient(g_IPC);
  g_SharedFpsLimiter.Apply();

  return oPresent(pSwapChain, SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE DetourDX11Present1(IDXGISwapChain *pSwapChain,
                                             UINT SyncInterval, UINT PresentFlags,
                                             const DXGI_PRESENT_PARAMETERS *pPresentParameters) {
  if (g_ShuttingDown) return oPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
  
  // Apply VSync Override
  VSyncOverride vsync = GetVSyncOverride();
  if (vsync.shouldOverride) {
      SyncInterval = (UINT)vsync.presentInterval;
  }
  
  // Reuse the same logic as Present, just call Present1 at the end
  // We can pass Flags directly as Present logic handles capture/overlay common parts
  // Note: We might miss some Present1-specific features but for overlay/capture 
  // the core logic is the same (getting backbuffer, drawing ImGui)
  
  // Call the main capture/overlay logic using the Present detour
  // We can't easily share the code without refactoring, so we'll just duplicate the essential entry point logic 
  // or (better) just call the DetourDX11Present logic? 
  // No, DetourDX11Present calls oPresent at the end. We can't do that.
  
  // Let's copy-paste the logic for now or refactor. 
  // Given the constraint, copy-paste with slight modifications is safer to avoid breaking existing Present hook
  
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
  
  // Note: CSV logging is handled in DetourDX11Present, which covers initial setup.
  // We just need to ensure the recording state is updated here too if Present1 is the primary path.
  IPCClient* ipc = g_IPC;
  bool isRecording = ipc && ipc->IsRecording();
  g_PerfMetrics.SetRecording(isRecording);

  // Capture Logic
  if (g_IPC && g_IPC->IsRecording()) {
    // Try to get D3D11 device first; if fails, this is a DX10 game
    ID3D11Device *device = NULL;
    bool isDX10 = false;
    
    if (FAILED(pSwapChain->GetDevice(IID_PPV_ARGS(&device)))) {
        // DX10 game detected
        isDX10 = true;
        g_IsDX10Device = true;
        g_DetectedAPI = "DX10";
    } else {
        // DX11
        g_IsDX10Device = false;
        g_DetectedAPI = "DX11";
    }

    // Lazy init
    if (!g_DX11Capture.initialized) {
      if (isDX10) {
        if (!g_DX11Capture.InitDX10(pSwapChain)) {
            HookLog("DX10: Failed to initialize capture");
            goto skip_capture;
        }
        g_DX11Capture.Init(nullptr, pSwapChain);
      } else {
        g_DX11Capture.Init(device, pSwapChain);
      }
    }

    // Get backbuffer
    ID3D11Texture2D *backbuffer = nullptr;
    pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));

    if (backbuffer && g_DX11Capture.initialized) {
      int idx = g_DX11Capture.writeIndex;
      ID3D11DeviceContext *captureContext = g_DX11Capture.GetCaptureContext();
      
      if (g_DX11Capture.useFences && g_DX11Capture.fence && g_DX11Capture.context4) {
         captureContext->CopyResource(g_DX11Capture.sharedTextures[idx], backbuffer);
         g_DX11Capture.fenceValue++;
         g_DX11Capture.context4->Signal(g_DX11Capture.fence, g_DX11Capture.fenceValue);
         g_DX11Capture.SignalFrameReady(g_IPC, idx, us / 1000, g_DX11Capture.fenceValue);
         g_DX11Capture.AdvanceWriteIndex();
      } else if (captureContext) {
          if (g_DX11Capture.copyQueries[idx]) {
            captureContext->Begin(g_DX11Capture.copyQueries[idx]);
          }
          captureContext->CopyResource(g_DX11Capture.sharedTextures[idx], backbuffer);
          if (g_DX11Capture.copyQueries[idx]) {
            captureContext->End(g_DX11Capture.copyQueries[idx]);
          }
          g_DX11Capture.WaitForCopy(captureContext, idx, 5);
          g_DX11Capture.SignalFrameReady(g_IPC, idx, us / 1000, 0);
          g_DX11Capture.AdvanceWriteIndex();
      }
    }

    if (backbuffer) backbuffer->Release();
    if (device) device->Release();
  } else if (g_DX11Capture.initialized) {
    g_DX11Capture.Cleanup();
  }

skip_capture:
  // Draw Overlay
  SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
  if (shm && shm->overlayConfig.showOverlay) {
    DrawDX11Overlay(pSwapChain);
  }

  // Apply shared FPS limiter
  g_SharedFpsLimiter.SetIPCClient(g_IPC);
  g_SharedFpsLimiter.Apply();

  return oPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
}

// Hook: CreateSamplerState
HRESULT STDMETHODCALLTYPE DetourCreateSamplerState(ID3D11Device *pDevice,
                                                   const D3D11_SAMPLER_DESC *pSamplerDesc,
                                                   ID3D11SamplerState **ppSamplerState) {
    if (!pSamplerDesc) return oCreateSamplerState(pDevice, pSamplerDesc, ppSamplerState);

    D3D11_SAMPLER_DESC desc = *pSamplerDesc;
    bool modified = false;

    if (g_IPC) {
        const auto& gfx = GetActiveGraphicsConfig();
        // Anisotropic Filtering
        std::string af = gfx.anisotropicFiltering;
        if (af != "default") {
            if (af == "off") {
                // Remove anisotropic filter if set
                if ((desc.Filter & D3D11_FILTER_ANISOTROPIC) || (desc.Filter & D3D11_FILTER_COMPARISON_ANISOTROPIC)) {
                    // Fallback to Trilinear
                    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                    desc.MaxAnisotropy = 1;
                    modified = true;
                }
            } else {
                // Force AF
                int maxAniso = 16;
                if (af == "2x") maxAniso = 2;
                else if (af == "4x") maxAniso = 4;
                else if (af == "8x") maxAniso = 8;
                
                // Keep comparison flat if present
                bool comparison = (desc.Filter >= D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT);
                
                desc.Filter = comparison ? D3D11_FILTER_COMPARISON_ANISOTROPIC : D3D11_FILTER_ANISOTROPIC;
                desc.MaxAnisotropy = maxAniso;
                modified = true;
            }
        }

        // Mip Mapping (Filter Override)
        std::string mip = gfx.mipMapping;
        if (mip != "default") {
            // This is complex because we need to preserve Min/Mag filters if possible, or just force standard
            // We will just force standard Trilinear/Bilinear for simplicity
             if (mip == "trilinear") {
                 desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                 modified = true;
             } else if (mip == "bilinear") {
                 desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
                 modified = true;
             }
        }

        // Mip Bias
        std::string bias = gfx.mipBias;
        if (bias != "default") {
             try {
                desc.MipLODBias = std::stof(bias);
                modified = true;
             } catch (...) {}
        }

        // SGSSAA Auto-Bias
        if (gfx.sgssaa && !gfx.disableAutoMipBias) {
            float sgBias = 0.0f;
            if (GetSGSSAABias(gfx.sgssaa, gfx.msaaSamples.c_str(), sgBias)) {
                desc.MipLODBias += sgBias;
                modified = true;
            }
        }
    }

    if (modified) {
        return oCreateSamplerState(pDevice, &desc, ppSamplerState);
    }
    return oCreateSamplerState(pDevice, pSamplerDesc, ppSamplerState);
}

void DX11Hook::Init() {
  HookLog("DX11Hook::Init()");

  // Create a dummy device to get the SwapChain vtable
  D3D_FEATURE_LEVEL featureLevel;
  const D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0,
                                             D3D_FEATURE_LEVEL_10_1};

  DXGI_SWAP_CHAIN_DESC scd;
  ZeroMemory(&scd, sizeof(scd));
  scd.BufferCount = 1;
  scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  scd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
  scd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
  scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
  scd.OutputWindow = GetForegroundWindow(); // Just need a window
  scd.SampleDesc.Count = 1;
  scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  scd.Windowed = TRUE;

  if (scd.OutputWindow == NULL) {
    scd.OutputWindow = CreateWindowA("STATIC", "Dummy", 0, 0, 0, 100, 100, NULL,
                                     NULL, NULL, NULL);
  }

  ID3D11Device *pDevice = NULL;
  ID3D11DeviceContext *pContext = NULL;
  IDXGISwapChain *pSwapChain = NULL;

  HMODULE hD3D11 = LoadLibraryA("d3d11.dll");
  if (!hD3D11) {
    HookLog("DX11Hook: D3D11 DLL not found. Skipping DX11 hook.");
    return;
  }

  typedef HRESULT (WINAPI *PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
  PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN pD3D11CreateDeviceAndSwapChain = (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)GetProcAddress(hD3D11, "D3D11CreateDeviceAndSwapChain");

  if (!pD3D11CreateDeviceAndSwapChain) {
      HookLog("DX11Hook: D3D11CreateDeviceAndSwapChain not found.");
      return;
  }

  HRESULT hr = pD3D11CreateDeviceAndSwapChain(
      NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, featureLevels, 2,
      D3D11_SDK_VERSION, &scd, &pSwapChain, &pDevice, &featureLevel, &pContext);

  if (FAILED(hr)) {
    HookLog("DX11Hook: Failed to create dummy device.");
    return;
  }

  void **vtable = *(void ***)pSwapChain;
  // index 8 is Present
  void *pPresent = vtable[8];

  if (MH_CreateHook(pPresent, (LPVOID)&DetourDX11Present,
                    (LPVOID *)&oPresent) != MH_OK) {
    HookLog("DX11Hook: Failed to create Present hook.");
  } else {
    if (MH_EnableHook(pPresent) != MH_OK) {
      HookLog("DX11Hook: Failed to enable Present hook.");
    } else {
      HookLog("DX11Hook: Present hook enabled.");
    }
  }

  if (MH_CreateHook((LPVOID)pD3D11CreateDeviceAndSwapChain, (LPVOID)&DetourD3D11CreateDeviceAndSwapChain,
                    (LPVOID *)&oD3D11CreateDeviceAndSwapChain) == MH_OK) {
    MH_EnableHook((LPVOID)pD3D11CreateDeviceAndSwapChain);
    HookLog("DX11: D3D11CreateDeviceAndSwapChain hook enabled.");
  }

  // Also hook ResizeBuffers (index 13)
  void *pResizeBuffers = vtable[13];
  if (MH_CreateHook(pResizeBuffers, (LPVOID)&DetourResizeBuffers,
                    (LPVOID *)&oResizeBuffers) != MH_OK) {
    HookLog("DX11Hook: Failed to create ResizeBuffers hook.");
  } else {
    if (MH_EnableHook(pResizeBuffers) != MH_OK) {
      HookLog("DX11Hook: Failed to enable ResizeBuffers hook.");
    } else {
      HookLog("DX11Hook: ResizeBuffers hook enabled.");
    }
  }

  // Hook CreateSamplerState (index 23)
  // ID3D11Device VTable: 3 (IUnknown) + 20 = 23 (for CreateSamplerState)
  void **deviceVTable = *(void***)pDevice;
  void *pCreateSamplerState = deviceVTable[23];
  
  if (MH_CreateHook(pCreateSamplerState, (LPVOID)&DetourCreateSamplerState,
                   (LPVOID *)&oCreateSamplerState) != MH_OK) {
      HookLog("DX11Hook: Failed to create CreateSamplerState hook.");
  } else {
      if (MH_EnableHook(pCreateSamplerState) != MH_OK) {
        HookLog("DX11Hook: Failed to enable CreateSamplerState hook.");
      } else {
        HookLog("DX11Hook: CreateSamplerState hook enabled.");
      }
  }



  // Hook Present1 (index 22) for DXGI 1.2+
  void *pPresent1 = vtable[22];
  if (MH_CreateHook(pPresent1, (LPVOID)&DetourDX11Present1,
                    (LPVOID *)&oPresent1) != MH_OK) {
    HookLog("DX11Hook: Failed to create Present1 hook.");
  } else {
      if (MH_EnableHook(pPresent1) != MH_OK) {
        HookLog("DX11Hook: Failed to enable Present1 hook.");
      } else {
        HookLog("DX11Hook: Present1 hook enabled.");
      }
  }

  pSwapChain->Release();
  pDevice->Release();
  pContext->Release();
  if (scd.OutputWindow != GetForegroundWindow()) {
    DestroyWindow(scd.OutputWindow);
  }
}

void DX11Hook::Shutdown() {
  HookLog("DX11Hook::Shutdown()");
  
  // Shutdown ImGui if initialized
  if (g_ImGuiInitialized) {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_ImGuiInitialized = false;
  }
  
  g_DX11Capture.Cleanup();
  if (g_mainRenderTargetView) {
    g_mainRenderTargetView->Release();
    g_mainRenderTargetView = nullptr;
  }
}

void DX11Hook::OnHostDisconnect() {
  HookLog("DX11Hook::OnHostDisconnect() - ready for reconnection");
  // DX11 capture is synchronous, nothing to stop
  // Just cleanup for potential new session
  g_DX11Capture.Cleanup();
}
