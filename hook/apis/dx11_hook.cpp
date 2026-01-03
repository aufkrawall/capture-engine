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

// Prerender Limit Fencing
static std::vector<ID3D11Query*> g_PrerenderQueries;
static uint64_t g_PrerenderFrameIndex = 0;
static int64_t g_LastSleepUs = 0;

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

#ifndef DXGI_PRESENT_ALLOW_TEARING
#define DXGI_PRESENT_ALLOW_TEARING 0x00000200UL
#endif
typedef HRESULT(STDMETHODCALLTYPE *CreateSamplerState_t)(ID3D11Device *pDevice,
                                                         const D3D11_SAMPLER_DESC *pSamplerDesc,
                                                         ID3D11SamplerState **ppSamplerState);
static CreateSamplerState_t oCreateSamplerState = NULL;
typedef HRESULT(WINAPI *D3D11CreateDeviceAndSwapChain_t)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
    UINT, UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **,
    ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

typedef HRESULT(STDMETHODCALLTYPE *CreateSwapChain_t)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
static CreateSwapChain_t oCreateSwapChain = NULL;

typedef HRESULT(STDMETHODCALLTYPE *CreateSwapChainForHwnd_t)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);
static CreateSwapChainForHwnd_t oCreateSwapChainForHwnd = NULL;

static ResizeBuffers_t oResizeBuffers = NULL; // Moved up

// Forward Declarations
static HRESULT STDMETHODCALLTYPE DetourDX11Present(IDXGISwapChain *pSwapChain, UINT SyncInterval, UINT Flags);
static HRESULT STDMETHODCALLTYPE DetourDX11Present1(IDXGISwapChain *pSwapChain, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS *pPresentParameters);
static HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain *pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
static HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain *pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
static void InstallVTableHooks(ID3D11Device *pDevice, ID3D11DeviceContext* pContext, IDXGISwapChain* pSwapChain);
static void HookDXGIFactory(ID3D11Device* pDevice);

static D3D11CreateDeviceAndSwapChain_t oD3D11CreateDeviceAndSwapChain = NULL;

static HRESULT WINAPI DetourD3D11CreateDeviceAndSwapChain(
    IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
    IDXGISwapChain **ppSwapChain, ID3D11Device **ppDevice,
    D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext) {
    
    if (pSwapChainDesc) {
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) {
             EarlyLog("DX11: D3D11CreateDeviceAndSwapChain called. Width=%u Height=%u Windowed=%d BufferCount=%u SwapEffect=%d Flags=0x%X",
                pSwapChainDesc->BufferDesc.Width, pSwapChainDesc->BufferDesc.Height, 
                pSwapChainDesc->Windowed, pSwapChainDesc->BufferCount, 
                pSwapChainDesc->SwapEffect, pSwapChainDesc->Flags);
        }
    } else {
        EarlyLog("DX11: D3D11CreateDeviceAndSwapChain called (pSwapChainDesc=NULL)");
    }
    
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
    
    HRESULT hr = oD3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, 
                                           pFeatureLevels, FeatureLevels, SDKVersion, 
                                           pFinalDesc, ppSwapChain, ppDevice, 
                                           pFeatureLevel, ppImmediateContext);

    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        IDXGISwapChain* sc = (ppSwapChain && *ppSwapChain) ? *ppSwapChain : nullptr;
        ID3D11DeviceContext* ctx = (ppImmediateContext && *ppImmediateContext) ? *ppImmediateContext : nullptr;
        // If immediate context not provided, get it from device
        if (!ctx) (*ppDevice)->GetImmediateContext(&ctx); // AddRef'd
        
        InstallVTableHooks(*ppDevice, ctx, sc);
        
        if (!sc) {
            // Swap chain not created yet, hook Factory to catch it later
            HookDXGIFactory(*ppDevice);
        }

        if (!ppImmediateContext && ctx) ctx->Release();
        
        // Explicitly set VRAM Total to prevent background thread crash
        if (ppDevice && *ppDevice) {
            IDXGIDevice* dxgiDevice = nullptr;
            if (SUCCEEDED((*ppDevice)->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice))) {
                 IDXGIAdapter* adapter = nullptr;
                 if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                     DXGI_ADAPTER_DESC desc;
                     if (SUCCEEDED(adapter->GetDesc(&desc))) {
                        SystemMetricsCollector::Get().SetVRAMTotal(desc.DedicatedVideoMemory);
                     }
                     adapter->Release();
                 }
                 dxgiDevice->Release();
            }
        }
    }
    
    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory *pFactory, IUnknown *pDevice, DXGI_SWAP_CHAIN_DESC *pDesc, IDXGISwapChain **ppSwapChain) {
    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) {
        if (pDesc) {
             EarlyLog("DX11: CreateSwapChain called. Width=%u Height=%u Windowed=%d BufferCount=%u SwapEffect=%d",
                pDesc->BufferDesc.Width, pDesc->BufferDesc.Height, 
                pDesc->Windowed, pDesc->BufferCount, pDesc->SwapEffect);
        }
    }

    HRESULT hr = oCreateSwapChain(pFactory, pDevice, pDesc, ppSwapChain);
    
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        ID3D11Device* pD3D11Device = nullptr;
        if (SUCCEEDED((*ppSwapChain)->GetDevice(__uuidof(ID3D11Device), (void**)&pD3D11Device))) {
             ID3D11DeviceContext* ctx = nullptr;
             pD3D11Device->GetImmediateContext(&ctx);
             InstallVTableHooks(pD3D11Device, ctx, *ppSwapChain);
             if (ctx) ctx->Release();
             pD3D11Device->Release();
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2 *pFactory, IUnknown *pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) {
    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) {
        if (pDesc) {
             EarlyLog("DX11: CreateSwapChainForHwnd called. Width=%u Height=%u BufferCount=%u SwapEffect=%d",
                pDesc->Width, pDesc->Height, pDesc->BufferCount, pDesc->SwapEffect);
        }
    }

    HRESULT hr = oCreateSwapChainForHwnd(pFactory, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        ID3D11Device* pD3D11Device = nullptr;
        if (SUCCEEDED((*ppSwapChain)->GetDevice(__uuidof(ID3D11Device), (void**)&pD3D11Device))) {
             ID3D11DeviceContext* ctx = nullptr;
             pD3D11Device->GetImmediateContext(&ctx);
             InstallVTableHooks(pD3D11Device, ctx, *ppSwapChain);
             if (ctx) ctx->Release();
             pD3D11Device->Release();
        }
    }
    return hr;
}

static void HookDXGIFactory(ID3D11Device* pDevice) {
    IDXGIDevice* dxgiDevice = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice))) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            IDXGIFactory* factory = nullptr;
            if (SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory))) {
                void** vtable = *(void***)factory;
                
                // Hook CreateSwapChain (Index 10)
                if (oCreateSwapChain == NULL) {
                    if (MH_CreateHook(vtable[10], (LPVOID)&DetourCreateSwapChain, (LPVOID*)&oCreateSwapChain) == MH_OK) {
                        MH_EnableHook(vtable[10]);
                        HookLog("DX11: Hooked IDXGIFactory::CreateSwapChain");
                    }
                }
                
                // Hook CreateSwapChainForHwnd (Index 15 on IDXGIFactory2)
                IDXGIFactory2* factory2 = nullptr;
                if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2), (void**)&factory2))) {
                    void** vtable2 = *(void***)factory2;
                    if (oCreateSwapChainForHwnd == NULL) {
                         if (MH_CreateHook(vtable2[15], (LPVOID)&DetourCreateSwapChainForHwnd, (LPVOID*)&oCreateSwapChainForHwnd) == MH_OK) {
                            MH_EnableHook(vtable2[15]);
                            HookLog("DX11: Hooked IDXGIFactory2::CreateSwapChainForHwnd");
                        }
                    }
                    factory2->Release();
                }
                
                factory->Release();
            }
            adapter->Release();
        }
        dxgiDevice->Release();
    }
}

static HRESULT STDMETHODCALLTYPE DetourCreateSamplerState(ID3D11Device *pDevice, const D3D11_SAMPLER_DESC *pSamplerDesc, ID3D11SamplerState **ppSamplerState);

// Helper to install vtable hooks
static void InstallVTableHooks(ID3D11Device *pDevice, ID3D11DeviceContext* pContext, IDXGISwapChain* pSwapChain) {
    // Hook Device methods
    if (pDevice) {
        void **pDeviceVTable = *(void ***)pDevice;
        if (oCreateSamplerState == NULL) {
        if (oCreateSamplerState == NULL) {
            // Index 23 is CreateSamplerState
            if (MH_CreateHook(pDeviceVTable[23], (LPVOID)&DetourCreateSamplerState,
                              (LPVOID *)&oCreateSamplerState) == MH_OK) {
                MH_EnableHook(pDeviceVTable[23]);
                HookLog("DX11: CreateSamplerState hook installed");
            }
        }
        }
    }

    // Hook SwapChain methods
    if (pSwapChain) {
        void **pSwapChainVTable = *(void ***)pSwapChain;
        
        // Present (Index 8)
        if (oPresent == NULL) {
            if (MH_CreateHook(pSwapChainVTable[8], (LPVOID)&DetourDX11Present,
                              (LPVOID *)&oPresent) == MH_OK) {
                MH_EnableHook(pSwapChainVTable[8]);
                HookLog("DX11: Present hook installed");
            }
        }

        // ResizeBuffers (Index 13)
        if (oResizeBuffers == NULL) {
            if (MH_CreateHook(pSwapChainVTable[13], (LPVOID)&DetourResizeBuffers,
                              (LPVOID *)&oResizeBuffers) == MH_OK) {
                MH_EnableHook(pSwapChainVTable[13]);
                HookLog("DX11: ResizeBuffers hook installed");
            }
        }

        // Present1 (Index 22) - For DXGI 1.2+
        if (oPresent1 == NULL) {
             // Only if vtable is large enough? DXGI 1.2 implies it is.
             // We can check device feature level or just try.
             // Safest is to try IO try/catch or just trust it's there on Win10+
             // But MinHook handles invalid pointers gracefully-ish? No. 
             // We assume DXGI 1.2 is present on target systems (Win 11).
             // To be safe, we can check IID_IDXGISwapChain1
             IDXGISwapChain1* sc1 = nullptr;
             if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc1)))) {
                 void **pSC1VTable = *(void ***)sc1;
                 if (MH_CreateHook(pSC1VTable[22], (LPVOID)&DetourDX11Present1,
                                   (LPVOID *)&oPresent1) == MH_OK) {
                    MH_EnableHook(pSC1VTable[22]);
                    HookLog("DX11: Present1 hook installed");
                 }
                 sc1->Release();
             }
        }
    }
}

// static ResizeBuffers_t oResizeBuffers = NULL; // Moved to top

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
            
            // Initialize SystemMetricsCollector with adapter LUID for GPU stats
            SystemMetricsCollector::Get().Initialize(luidLow, luidHigh);
            SystemMetricsCollector::Get().SetVRAMTotal(adapterDesc.DedicatedVideoMemory);
            
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
            
            // Initialize SystemMetricsCollector with adapter LUID for GPU stats
            SystemMetricsCollector::Get().Initialize(luidLow, luidHigh);
            
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
    
    // Cache these for reuse - some games have broken GetImmediateContext on subsequent calls
    g_pd3dDevice = device;
    g_pd3dDeviceContext = context;
    // Keep references (don't release here)

    // Initialize System Metrics (CPU/GPU/RAM)
    EarlyLog("%s: Initializing SystemMetrics...", g_DetectedAPI);
    IDXGIDevice* dxgiDev = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC adapterDesc;
            adapter->GetDesc(&adapterDesc);
            EarlyLog("%s: Calling SystemMetricsCollector::Initialize...", g_DetectedAPI);
            SystemMetricsCollector::Get().Initialize(adapterDesc.AdapterLuid.LowPart, adapterDesc.AdapterLuid.HighPart);
            EarlyLog("%s: SystemMetrics initialized OK", g_DetectedAPI);
            adapter->Release();
        }
        dxgiDev->Release();
    }

    EarlyLog("%s: First frame init complete", g_DetectedAPI);
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

  // Use cached device/context - some games have broken GetImmediateContext on re-acquire
  if (!g_pd3dDevice || !g_pd3dDeviceContext) {
      EarlyLog("%s: No cached device/context!", g_DetectedAPI);
      return;
  }

  if (!g_mainRenderTargetView) {
    EarlyLog("%s: Creating RTV...", g_DetectedAPI);
    ID3D11Texture2D *backbuffer = nullptr;
    HRESULT hr = pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    if (FAILED(hr) || !backbuffer) {
        EarlyLog("%s: GetBuffer FAILED hr=0x%08X", g_DetectedAPI, hr);
        return;
    }
    hr = g_pd3dDevice->CreateRenderTargetView(backbuffer, NULL, &g_mainRenderTargetView);
    backbuffer->Release();
    if (FAILED(hr)) {
        EarlyLog("%s: CreateRTV FAILED hr=0x%08X", g_DetectedAPI, hr);
        return;
    }
    EarlyLog("%s: RTV created OK", g_DetectedAPI);
  }

  g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
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

// --- Prerender Limit Support ---
static void ApplyPrerenderLimit(IDXGISwapChain* pSwapChain, float limit) {
    if (limit < 0.0f) return;

    ID3D11Device* dev = nullptr;
    if (FAILED(pSwapChain->GetDevice(IID_PPV_ARGS(&dev)))) return;

    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);

    if (g_PrerenderQueries.empty() || g_PrerenderQueries[0] == nullptr) {
        g_PrerenderQueries.clear();
        for (int i = 0; i < 16; i++) {
            D3D11_QUERY_DESC qd = {};
            qd.Query = D3D11_QUERY_EVENT;
            ID3D11Query* q = nullptr;
            if (SUCCEEDED(dev->CreateQuery(&qd, &q))) {
                g_PrerenderQueries.push_back(q);
            }
        }
        HookLog("DX11: Created manual prerender query ring buffer (size: %d)", (int)g_PrerenderQueries.size());
    }

    if (!g_PrerenderQueries.empty()) {
        bool isFractional = (limit > 0.01f && limit < 1.0f);
        
        if (limit == 0.0f) {
            // Strict Serial: Wait for current frame
            ID3D11Query* q = g_PrerenderQueries[g_PrerenderFrameIndex % g_PrerenderQueries.size()];
            ctx->End(q);
            while (ctx->GetData(q, nullptr, 0, 0) == S_FALSE) {
                SwitchToThread();
            }
        } else {
            // Buffered Limit: For fractional limits (e.g., 0.5), we use Buffered 1 (Lookback 2)
            // This allows GPU overlap while pacing provides the idle gap.
            int effectiveLimit = isFractional ? 1 : (int)limit;
            int lookback = effectiveLimit + 1;

            ID3D11Query* currentQ = g_PrerenderQueries[g_PrerenderFrameIndex % g_PrerenderQueries.size()];
            ctx->End(currentQ);

            if (g_PrerenderFrameIndex >= (uint64_t)lookback) {
                ID3D11Query* waitQ = g_PrerenderQueries[(g_PrerenderFrameIndex - lookback) % g_PrerenderQueries.size()];
                while (ctx->GetData(waitQ, nullptr, 0, 0) == S_FALSE) {
                    SwitchToThread();
                }
            }
        }
        g_PrerenderFrameIndex++;
        
        // Strict Serial + Fixed Idle Gap for fractional limits
        if (isFractional) {
            // effectiveLimit already set to 0 for Strict Serial above
            
            // After the wait completes, calculate and apply a fixed idle gap
            float fps = g_PerfMetrics.GetCurrentFPS();
            double targetFrameTimeUs = (fps > 1.0f) ? (1000000.0 / fps) : 16666.0;
            
            // Fixed Idle Gap = TargetFrameTime * (1.0 - limit) * 0.10
            int64_t idleGapUs = (int64_t)(targetFrameTimeUs * (1.0 - limit) * 0.10);
            if (idleGapUs > 0) {
                if (idleGapUs > 10000) idleGapUs = 10000; // Cap at 10ms
                PrecisionSleep(idleGapUs);
            }
        }
    }

    ctx->Release();
    dev->Release();
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
  
  // LOG ONCE: Actual SwapChain Desc
  static bool loggedSC = false;
  if (!loggedSC && g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) {
      DXGI_SWAP_CHAIN_DESC desc;
      if (SUCCEEDED(pSwapChain->GetDesc(&desc))) {
          EarlyLog("DX11: Present Hook - Actual SwapChain: Width=%u Height=%u Windowed=%d BufferCount=%u SwapEffect=%d Flags=0x%X",
              desc.BufferDesc.Width, desc.BufferDesc.Height, 
              desc.Windowed, desc.BufferCount, 
              desc.SwapEffect, desc.Flags);
           loggedSC = true;

           // Report LUID when logging swapchain first time
           ID3D11Device* dev = nullptr;
           if (SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&dev)))) {
               IDXGIDevice* dxgiDev = nullptr;
               if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) {
                   IDXGIAdapter* adapter = nullptr;
                   if (SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
                       DXGI_ADAPTER_DESC adesc;
                       adapter->GetDesc(&adesc);
                       ReportLUID(adesc.AdapterLuid.LowPart, adesc.AdapterLuid.HighPart);
                       adapter->Release();
                   }
                   dxgiDev->Release();
               }
               dev->Release();
           }
      }
  }

  // Initialize CSV logging once - only if debug logging is enabled
  static bool csvLoggingInitialized = false;
  IPCClient* ipc = g_IPC;
  SharedMemoryLayout* csvShm = (ipc) ? ipc->GetSharedMem() : nullptr;

  // Apply VSync Override
  VSyncOverride vsync = GetVSyncOverride();
  if (vsync.shouldOverride) {
      SyncInterval = (UINT)vsync.presentInterval;
      if (SyncInterval > 0) {
          Flags &= ~DXGI_PRESENT_ALLOW_TEARING;
      }
  }

  // Apply Prerender Limit
  float limit = GetActivePrerenderLimit();
  if (limit >= 0.0f) {
      static float lastLimit = -2.0f;
      if (fabs(limit - lastLimit) > 0.001f) {
          ID3D11Device* dev = nullptr;
          if (SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&dev)))) {
              IDXGIDevice1* dxgiDev = nullptr;
              if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) {
                   UINT effectiveLatency = (limit < 1.0f) ? 1 : (UINT)limit;
                   dxgiDev->SetMaximumFrameLatency(effectiveLatency);
                   dxgiDev->Release();
                   HookLog("DX11: Set maximum DXGI latency to %d (Active Limit: %.2f)", effectiveLatency, limit);
              }
              dev->Release();
          }
          lastLimit = limit;
      }
      ApplyPrerenderLimit(pSwapChain, limit);
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
         
         // PASS RAW QPC: MediaEngine converts to MS
         g_DX11Capture.SignalFrameReady(ipc, idx, qpc.QuadPart, g_DX11Capture.fenceValue);
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
    
          // PASS RAW QPC: MediaEngine converts to MS
          g_DX11Capture.SignalFrameReady(ipc, idx, qpc.QuadPart, 0);
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
      if (SyncInterval > 0) {
          PresentFlags &= ~DXGI_PRESENT_ALLOW_TEARING;
      }
  }

  // Apply Prerender Limit
  float limit = GetActivePrerenderLimit();
  if (limit >= 0.0f) {
      static float lastLimit = -2.0f;
      if (fabs(limit - lastLimit) > 0.001f) {
          ID3D11Device* dev = nullptr;
          if (SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&dev)))) {
              IDXGIDevice1* dxgiDev = nullptr;
              if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) {
                   UINT effectiveLatency = (limit < 1.0f) ? 1 : (UINT)limit;
                   dxgiDev->SetMaximumFrameLatency(effectiveLatency);
                   dxgiDev->Release();
                   HookLog("DX11: Set maximum DXGI latency to %d (Active Limit: %.2f)", effectiveLatency, limit);
              }
              dev->Release();
          }
          lastLimit = limit;
      }
      ApplyPrerenderLimit(pSwapChain, limit);
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
         // PASS RAW QPC
         g_DX11Capture.SignalFrameReady(g_IPC, idx, qpc.QuadPart, g_DX11Capture.fenceValue);
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
    if (!g_GraphicsOverridesActive.load(std::memory_order_acquire)) return oCreateSamplerState(pDevice, pSamplerDesc, ppSamplerState);

    D3D11_SAMPLER_DESC desc = *pSamplerDesc;
    bool modified = false;
    bool debug = false;
    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) {
        debug = true;
    }

    // Check if overrides should be applied
    // 1. MaxLOD == 0.0f implies mipmapping is disabled (clamped to base level)
    // 2. MinLOD == MaxLOD implies a single level is selected
    bool overridesAllowed = true;
    if (pSamplerDesc->MaxLOD == 0.0f) overridesAllowed = false;
    if (pSamplerDesc->MinLOD == pSamplerDesc->MaxLOD) overridesAllowed = false;

    if (overridesAllowed && g_IPC) {
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
                
                // CRITICAL: D3D11 forbids Anisotropic Filtering if any address mode is BORDER.
                if (desc.AddressU == D3D11_TEXTURE_ADDRESS_BORDER || 
                    desc.AddressV == D3D11_TEXTURE_ADDRESS_BORDER || 
                    desc.AddressW == D3D11_TEXTURE_ADDRESS_BORDER) {
                    // Skip AF override for Border address mode
                } else {
                    // Keep comparison flat if present
                    bool comparison = (desc.Filter >= D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT);
                    
                    desc.Filter = comparison ? D3D11_FILTER_COMPARISON_ANISOTROPIC : D3D11_FILTER_ANISOTROPIC;
                    desc.MaxAnisotropy = maxAniso;
                    modified = true;
                }
            }
        }

        // Mip Mapping (Filter Override)
        std::string mip = gfx.mipMapping;
        // Don't override filter for MipMapping if Anisotropy is already enabled (AF implies Trilinear)
        bool isAniso = (desc.Filter == D3D11_FILTER_ANISOTROPIC || desc.Filter == D3D11_FILTER_COMPARISON_ANISOTROPIC);
        
        if (mip != "default" && !isAniso) {
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

    HRESULT hr;
    if (modified) {
        hr = oCreateSamplerState(pDevice, &desc, ppSamplerState);
        if (FAILED(hr) && debug) {
            EarlyLog("DX11: CreateSamplerState FAILED with modified desc (hr=0x%08X). Filter=0x%X Bias=%.2f Aniso=%u", 
                     hr, desc.Filter, desc.MipLODBias, desc.MaxAnisotropy);
        }
    } else {
        hr = oCreateSamplerState(pDevice, pSamplerDesc, ppSamplerState);
    }
    return hr;
}

void DX11Hook::Init() {
  HookLog("DX11Hook::Init() - Lazy Hooking Mode");

  // We only hook D3D11CreateDeviceAndSwapChain directly here.
  // The VTable hooks (Present, etc.) are installed lazily when the game calls CreateDevice.
  
  HMODULE hD3D11 = LoadLibraryA("d3d11.dll");
  if (!hD3D11) {
    HookLog("DX11Hook: D3D11 DLL not found.");
    return;
  }

  typedef HRESULT (WINAPI *PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
  PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN pD3D11CreateDeviceAndSwapChain = (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)GetProcAddress(hD3D11, "D3D11CreateDeviceAndSwapChain");

  if (!pD3D11CreateDeviceAndSwapChain) {
      HookLog("DX11Hook: D3D11CreateDeviceAndSwapChain export not found.");
      return;
  }

  if (MH_CreateHook((LPVOID)pD3D11CreateDeviceAndSwapChain, (LPVOID)&DetourD3D11CreateDeviceAndSwapChain,
                    (LPVOID *)&oD3D11CreateDeviceAndSwapChain) == MH_OK) {
    MH_EnableHook((LPVOID)pD3D11CreateDeviceAndSwapChain);
    HookLog("DX11: D3D11CreateDeviceAndSwapChain hook installed.");
  } else {
      HookLog("DX11: Failed to hook D3D11CreateDeviceAndSwapChain.");
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
