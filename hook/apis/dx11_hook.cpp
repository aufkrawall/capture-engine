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
#include <backends/imgui_impl_dx10.h>
#include <backends/imgui_impl_win32.h>
#include <cstdint>
#include <cstdio>
#include <d3d10.h>    // For DX10 detection
#include <d3d10_1.h>  // For DX10.1 detection
#include <d3d11.h>
#include <d3d11_1.h> // For ID3D11DeviceContext1
#include <d3d11_4.h> // For ID3D11Fence and ID3D11Device5
#include <dxgi1_2.h> // Required for IDXGIResource1 and CreateSharedHandle
#include <d3dcompiler.h>
#include <dxgi1_2.h> // For LUID
#include <imgui.h>

// Globals
static ID3D11Device *g_pd3dDevice = NULL;
static ID3D11DeviceContext *g_pd3dDeviceContext = NULL;
static ID3D11RenderTargetView *g_mainRenderTargetView = NULL;

static ID3D10Device *g_pd3d10Device = NULL;
static ID3D10RenderTargetView *g_mainRenderTargetView10 = NULL;

static IDXGISwapChain *g_pSwapChain = NULL;

static bool g_IsDX10Device = false;
static const char* g_DetectedAPI = "DX11"; 

static bool g_IsDX11Active = false;
static bool g_IsDX10Active = false;

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

// D3D10 CreateSamplerState hook
typedef HRESULT(STDMETHODCALLTYPE *CreateSamplerState10_t)(ID3D10Device *pDevice,
                                                            const D3D10_SAMPLER_DESC *pSamplerDesc,
                                                            ID3D10SamplerState **ppSamplerState);
static CreateSamplerState10_t oCreateSamplerState10 = NULL;

// Forward declaration
static void InstallRuntimeD3D10Hooks(ID3D10Device* pDevice);

// Typedefs for D3D10 SetSamplers (used by hooks) for runtime sampler replacement
typedef void(STDMETHODCALLTYPE *PSSetSamplers10_t)(ID3D10Device *pDevice, UINT StartSlot, UINT NumSamplers, ID3D10SamplerState *const *ppSamplers);
typedef void(STDMETHODCALLTYPE *VSSetSamplers10_t)(ID3D10Device *pDevice, UINT StartSlot, UINT NumSamplers, ID3D10SamplerState *const *ppSamplers);
typedef void(STDMETHODCALLTYPE *GSSetSamplers10_t)(ID3D10Device *pDevice, UINT StartSlot, UINT NumSamplers, ID3D10SamplerState *const *ppSamplers);
static PSSetSamplers10_t oPSSetSamplers10 = NULL;
static VSSetSamplers10_t oVSSetSamplers10 = NULL;
static GSSetSamplers10_t oGSSetSamplers10 = NULL;

// Lock-free sampler replacement cache for D3D10
// Maps original sampler -> our modified replacement sampler
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>
#include <atomic>
static std::unordered_map<ID3D10SamplerState*, ID3D10SamplerState*> g_SamplerCache10;
static std::shared_mutex g_SamplerCacheMutex10;
static ID3D10Device* g_CachedD3D10Device = nullptr;
static std::unordered_set<ID3D10SamplerState*> g_ReplacementSamplers10; // Prevent recursive replacements

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
static void InstallVTableHooks(ID3D11Device *pDevice, ID3D11DeviceContext* pContext, IDXGISwapChain* pSwapChain);
static void HookDXGIFactory(IUnknown* pDevice);

static D3D11CreateDeviceAndSwapChain_t oD3D11CreateDeviceAndSwapChain = NULL;

typedef HRESULT(WINAPI *D3D10CreateDeviceAndSwapChain_t)(
    IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT, DXGI_SWAP_CHAIN_DESC*,
    IDXGISwapChain**, ID3D10Device**);
static D3D10CreateDeviceAndSwapChain_t oD3D10CreateDeviceAndSwapChain = NULL;

typedef HRESULT(WINAPI *D3D10CreateDeviceAndSwapChain1_t)(
    IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, D3D10_FEATURE_LEVEL1, UINT,
    DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D10Device1**);
static D3D10CreateDeviceAndSwapChain1_t oD3D10CreateDeviceAndSwapChain1 = NULL;

typedef HRESULT(WINAPI *D3D10CreateDevice_t)(IDXGIAdapter *, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT, ID3D10Device **);
static D3D10CreateDevice_t oD3D10CreateDevice = NULL;

typedef HRESULT(WINAPI *D3D10CreateDevice1_t)(IDXGIAdapter *, D3D10_DRIVER_TYPE, HMODULE, UINT, D3D10_FEATURE_LEVEL1, UINT, ID3D10Device1 **);
static D3D10CreateDevice1_t oD3D10CreateDevice1 = NULL;

typedef HRESULT(WINAPI *CreateDXGIFactory_t)(REFIID, void **);
static CreateDXGIFactory_t oCreateDXGIFactory = NULL;

typedef HRESULT(WINAPI *CreateDXGIFactory1_t)(REFIID, void **);
static CreateDXGIFactory1_t oCreateDXGIFactory1 = NULL;

typedef HRESULT(WINAPI *CreateDXGIFactory2_t)(UINT, REFIID, void **);
static CreateDXGIFactory2_t oCreateDXGIFactory2 = NULL;

static void HookDXGIFactoryInstance(IUnknown* factory);

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

static HRESULT WINAPI DetourD3D10CreateDeviceAndSwapChain(
    IDXGIAdapter *pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, UINT SDKVersion, DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
    IDXGISwapChain **ppSwapChain, ID3D10Device **ppDevice) {
    
    EarlyLog("DX10: D3D10CreateDeviceAndSwapChain called");
    
    DXGI_SWAP_CHAIN_DESC desc;
    DXGI_SWAP_CHAIN_DESC *pFinalDesc = pSwapChainDesc;
    
    if (pSwapChainDesc) {
        const GraphicsConfig& gfx = GetActiveGraphicsConfig();
        desc = *pSwapChainDesc;
        bool modified = false;
        
        int count = gfx.backbufferCount;
        if (count >= 2 && count <= 6) {
            desc.BufferCount = (UINT)count;
            modified = true;
        }
        
        if (modified) pFinalDesc = &desc;
    }
    
    HRESULT hr = oD3D10CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, SDKVersion, pFinalDesc, ppSwapChain, ppDevice);
    
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        InstallVTableHooks(NULL, NULL, *ppSwapChain);
    }
    return hr;
}

static HRESULT WINAPI DetourD3D10CreateDeviceAndSwapChain1(
    IDXGIAdapter *pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, D3D10_FEATURE_LEVEL1 HardwareLevel, UINT SDKVersion,
    DXGI_SWAP_CHAIN_DESC *pSwapChainDesc, IDXGISwapChain **ppSwapChain,
    ID3D10Device1 **ppDevice) {
    
    EarlyLog("DX10.1: D3D10CreateDeviceAndSwapChain1 called");
    
    DXGI_SWAP_CHAIN_DESC desc;
    DXGI_SWAP_CHAIN_DESC *pFinalDesc = pSwapChainDesc;
    
    if (pSwapChainDesc) {
        const GraphicsConfig& gfx = GetActiveGraphicsConfig();
        desc = *pSwapChainDesc;
        bool modified = false;
        
        int count = gfx.backbufferCount;
        if (count >= 2 && count <= 6) {
            desc.BufferCount = (UINT)count;
            modified = true;
        }
        
        if (modified) pFinalDesc = &desc;
    }
    
    HRESULT hr = oD3D10CreateDeviceAndSwapChain1(pAdapter, DriverType, Software, Flags, HardwareLevel, SDKVersion, pFinalDesc, ppSwapChain, ppDevice);
    
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        InstallVTableHooks(NULL, NULL, *ppSwapChain);
    }
    return hr;
}

static HRESULT WINAPI DetourD3D10CreateDevice(
    IDXGIAdapter *pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, UINT SDKVersion, ID3D10Device **ppDevice) {
    
    HRESULT hr = oD3D10CreateDevice(pAdapter, DriverType, Software, Flags, SDKVersion, ppDevice);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        HookDXGIFactory(*ppDevice);
    }
    return hr;
}

static HRESULT WINAPI DetourD3D10CreateDevice1(
    IDXGIAdapter *pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, D3D10_FEATURE_LEVEL1 HardwareLevel, UINT SDKVersion,
    ID3D10Device1 **ppDevice) {
    
    HRESULT hr = oD3D10CreateDevice1(pAdapter, DriverType, Software, Flags, HardwareLevel, SDKVersion, ppDevice);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        HookDXGIFactory(*ppDevice);
    }
    return hr;
}

static HRESULT WINAPI DetourCreateDXGIFactory(REFIID riid, void **ppFactory) {
    HRESULT hr = oCreateDXGIFactory(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
        HookDXGIFactoryInstance((IUnknown*)*ppFactory);
    }
    return hr;
}

static HRESULT WINAPI DetourCreateDXGIFactory1(REFIID riid, void **ppFactory) {
    HRESULT hr = oCreateDXGIFactory1(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
        HookDXGIFactoryInstance((IUnknown*)*ppFactory);
    }
    return hr;
}

static HRESULT WINAPI DetourCreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory) {
    HRESULT hr = oCreateDXGIFactory2(Flags, riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
        HookDXGIFactoryInstance((IUnknown*)*ppFactory);
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
        } else {
             // Fallback for D3D10/10.1 or other versions
             InstallVTableHooks(NULL, NULL, *ppSwapChain);
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
        } else {
             // Fallback for D3D10/10.1 or other versions
             InstallVTableHooks(NULL, NULL, *ppSwapChain);
        }
    }
    return hr;
}

static void HookDXGIFactoryInstance(IUnknown* factory) {
    if (!factory) return;
    
    IDXGIFactory* pFactory = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory), (void**)&pFactory))) {
        void** vtable = *(void***)pFactory;
        
        // Hook CreateSwapChain (Index 10)
        if (oCreateSwapChain == NULL) {
            if (MH_CreateHook(vtable[10], (LPVOID)&DetourCreateSwapChain, (LPVOID*)&oCreateSwapChain) == MH_OK) {
                MH_EnableHook(vtable[10]);
                HookLog("DX11: Hooked IDXGIFactory::CreateSwapChain");
            }
        }
        
        // Hook CreateSwapChainForHwnd (Index 15 on IDXGIFactory2)
        IDXGIFactory2* factory2 = nullptr;
        if (SUCCEEDED(pFactory->QueryInterface(__uuidof(IDXGIFactory2), (void**)&factory2))) {
            void** vtable2 = *(void***)factory2;
            if (oCreateSwapChainForHwnd == NULL) {
                 if (MH_CreateHook(vtable2[15], (LPVOID)&DetourCreateSwapChainForHwnd, (LPVOID*)&oCreateSwapChainForHwnd) == MH_OK) {
                    MH_EnableHook(vtable2[15]);
                    HookLog("DX11: Hooked IDXGIFactory2::CreateSwapChainForHwnd");
                }
            }
            factory2->Release();
        }
        pFactory->Release();
    }
}

static void HookDXGIFactory(IUnknown* pDevice) {
    if (!pDevice) return;
    IDXGIDevice* dxgiDevice = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice))) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            IDXGIFactory* factory = nullptr;
            if (SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory))) {
                HookDXGIFactoryInstance(factory);
                factory->Release();
            }
            adapter->Release();
        }
        dxgiDevice->Release();
    }
}

static HRESULT STDMETHODCALLTYPE DetourCreateSamplerState(ID3D11Device *pDevice, const D3D11_SAMPLER_DESC *pSamplerDesc, ID3D11SamplerState **ppSamplerState);
static HRESULT STDMETHODCALLTYPE DetourCreateSamplerState10(ID3D10Device *pDevice, const D3D10_SAMPLER_DESC *pSamplerDesc, ID3D10SamplerState **ppSamplerState);
static void STDMETHODCALLTYPE DetourPSSetSamplers10(ID3D10Device *pDevice, UINT StartSlot, UINT NumSamplers, ID3D10SamplerState *const *ppSamplers);
static void STDMETHODCALLTYPE DetourVSSetSamplers10(ID3D10Device *pDevice, UINT StartSlot, UINT NumSamplers, ID3D10SamplerState *const *ppSamplers);
static void STDMETHODCALLTYPE DetourGSSetSamplers10(ID3D10Device *pDevice, UINT StartSlot, UINT NumSamplers, ID3D10SamplerState *const *ppSamplers);
static void STDMETHODCALLTYPE DetourDraw10(ID3D10Device* pDevice, UINT VertexCount, UINT StartVertexLocation);
static void STDMETHODCALLTYPE DetourDrawIndexed10(ID3D10Device* pDevice, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation);
static void STDMETHODCALLTYPE DetourDrawInstanced10(ID3D10Device* pDevice, UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation);
static void STDMETHODCALLTYPE DetourDrawIndexedInstanced10(ID3D10Device* pDevice, UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation);

// Helper to install vtable hooks
static void InstallVTableHooks(ID3D11Device *pDevice, ID3D11DeviceContext* pContext, IDXGISwapChain* pSwapChain) {
    // Hook D3D11 Device methods
    if (pDevice) {
        void **pDeviceVTable = *(void ***)pDevice;
        if (oCreateSamplerState == NULL) {
            // Index 23 is CreateSamplerState for D3D11
            if (MH_CreateHook(pDeviceVTable[23], (LPVOID)&DetourCreateSamplerState,
                              (LPVOID *)&oCreateSamplerState) == MH_OK) {
                MH_EnableHook(pDeviceVTable[23]);
                HookLog("DX11: CreateSamplerState hook installed");
            }
        }
    }
    
    // ALSO try to hook D3D10 device from swapchain (Windows D3D10/D3D11 interop means both will succeed)
    // We need to hook D3D10 CreateSamplerState and SetSamplers for D3D10 games
    if (pSwapChain) {
        ID3D10Device *pDevice10 = nullptr;
        HRESULT hr = pSwapChain->GetDevice(__uuidof(ID3D10Device), (void**)&pDevice10);
        if (SUCCEEDED(hr) && pDevice10) {
            void **pDeviceVTable = *(void ***)pDevice10;
            
            // CreateSamplerState (Index 87)
            if (oCreateSamplerState10 == NULL) {
                HookLog("DX10: Got D3D10 device from swapchain, hooking CreateSamplerState...");
                MH_STATUS status = MH_CreateHook(pDeviceVTable[87], (LPVOID)&DetourCreateSamplerState10,
                                  (LPVOID *)&oCreateSamplerState10);
                if (status == MH_OK) {
                    MH_EnableHook(pDeviceVTable[87]);
                    HookLog("DX10: CreateSamplerState hook installed");
                } else {
                    HookLog("DX10: MH_CreateHook failed for CreateSamplerState, status=%d", status);
                }
            }
            
            // PSSetSamplers (Index 6)
            if (oPSSetSamplers10 == NULL) {
                MH_STATUS status = MH_CreateHook(pDeviceVTable[6], (LPVOID)&DetourPSSetSamplers10,
                                  (LPVOID *)&oPSSetSamplers10);
                if (status == MH_OK) {
                    MH_EnableHook(pDeviceVTable[6]);
                    HookLog("DX10: PSSetSamplers hook installed");
                }
            }
            
            // VSSetSamplers (Index 20)
            if (oVSSetSamplers10 == NULL) {
                MH_STATUS status = MH_CreateHook(pDeviceVTable[20], (LPVOID)&DetourVSSetSamplers10,
                                  (LPVOID *)&oVSSetSamplers10);
                if (status == MH_OK) {
                    MH_EnableHook(pDeviceVTable[20]);
                    HookLog("DX10: VSSetSamplers hook installed");
                }
            }
            
            // GSSetSamplers (Index 23)
            if (oGSSetSamplers10 == NULL) {
                MH_STATUS status = MH_CreateHook(pDeviceVTable[23], (LPVOID)&DetourGSSetSamplers10,
                                  (LPVOID *)&oGSSetSamplers10);
                if (status == MH_OK) {
                    MH_EnableHook(pDeviceVTable[23]);
                    HookLog("DX10: GSSetSamplers hook installed");
                }
            }
            pDevice10->Release();
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
  
  // Keyed Mutex Support (Proper Fix)
  IDXGIKeyedMutex *keyedMutexes[CAPTURE_TEXTURE_COUNT]{};
  bool useKeyedMutex = false;
  
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
    
    if (ownedContext) ownedContext->Release();
    ownedContext = nullptr;
    if (ownedDevice) ownedDevice->Release();
    ownedDevice = nullptr;
    
    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
        if (keyedMutexes[i]) {
            keyedMutexes[i]->Release();
            keyedMutexes[i] = nullptr;
        }
    }
    
    cachedDevice = nullptr;
    cachedContext = nullptr;
    initialized = false;
    useFences = false;
    useKeyedMutex = false;
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
    
    // Use plain shared textures with NT Handles for cross-process sharing
    // Synchronization is handled via D3D11 Fence (DX11.3+) or no sync fallback
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    useKeyedMutex = false; // Disabled - using Fence instead

    // Try to create D3D11 Fence for async GPU synchronization (DX11.3+)
    ID3D11Device5 *device5 = nullptr;
    if (SUCCEEDED(captureDevice->QueryInterface(IID_PPV_ARGS(&device5)))) {
        HRESULT hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence));
        if (SUCCEEDED(hr)) {
            // Get shared fence handle for cross-process
            hr = fence->CreateSharedHandle(NULL, GENERIC_ALL, NULL, &sharedFenceHandle);
            if (SUCCEEDED(hr)) {
                // Get Context4 for Signal()
                ID3D11DeviceContext *immCtx = nullptr;
                captureDevice->GetImmediateContext(&immCtx);
                if (SUCCEEDED(immCtx->QueryInterface(IID_PPV_ARGS(&context4)))) {
                    useFences = true;
                    EarlyLog("DX11: D3D11 Fence created (async GPU sync enabled)");
                } else {
                    EarlyLog("DX11: Warning - ID3D11DeviceContext4 not available");
                }
                immCtx->Release();
            } else {
                EarlyLog("DX11: Warning - Fence shared handle creation failed");
                fence->Release();
                fence = nullptr;
            }
        } else {
            EarlyLog("DX11: Warning - Fence creation failed (hr=0x%08x)", hr);
        }
        device5->Release();
    } else {
        EarlyLog("DX11: ID3D11Device5 not available (DX11.3 required for Fences)");
    }


    bool success = true;
    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
      HRESULT hr = captureDevice->CreateTexture2D(&texDesc, NULL, &sharedTextures[i]);
      if (SUCCEEDED(hr)) {
        IDXGIResource1 *pResource1 = NULL;
        // Use IDXGIResource1 for NT Handles
        if (SUCCEEDED(sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&pResource1)))) {
            hr = pResource1->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &sharedTextureHandles[i]);
            pResource1->Release();
        } else {
            // Fallback to legacy KMT if Resource1 not valid (should not happen with NTHANDLE flag)
            // But if we requested NTHANDLE, GetSharedHandle (KMT) will fail on some drivers.
            // We should log this specific failure path.
            IDXGIResource *pResource = NULL;
            if (SUCCEEDED(sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&pResource))) && (texDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED)) {
                pResource->GetSharedHandle(&sharedTextureHandles[i]);
                pResource->Release();
                EarlyLog("DX11: Warning - Fallback to KMT handle for KeyedMutex (NT Handle QI failed)");
            } else {
                EarlyLog("DX11: Error - Failed to get any shared handle interface for texture %d", i);
            }
        }
        
        if (sharedTextureHandles[i] == NULL) {
            EarlyLog("DX11: Critical - Shared Handle is NULL for texture %d", i);
            success = false;
        } else {
            EarlyLog("DX11: Created Texture %d Handle %p", i, sharedTextureHandles[i]);
        }
        
      } else {
        // Fallback to legacy shared if NT Handle not supported
         if (hr == E_INVALIDARG && (texDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
             EarlyLog("DX11: NT Handle not supported, falling back to legacy shared textures");
             texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
             i--; // Retry this index
             continue;
         }
      
        success = false;
        HookLog("%s: Failed to create texture %d (hr=0x%08x)", isDX10Mode ? "DX10" : "DX11", i, hr);
      }
    }

    if (success) {
      if (g_IPC) {
        PublishToSharedMemory(g_IPC);
      }
      initialized = true;
      EarlyLog("%s Capture Initialized: %dx%d (Fence: %s)", isDX10Mode ? "DX10" : "DX11", 
              width, height, useFences ? "ON" : "OFF");
    } else {
      EarlyLog("%s Capture Init FAILED (success=false)", isDX10Mode ? "DX10" : "DX11");
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

// Helper to force rebind of all samplers (triggering our DetourSetSamplers)
// Returns true if any samplers were actually found and rebound
static bool RebindSamplers10(ID3D10Device* pDevice) {
    if (!pDevice) return false;
    
    bool foundAny = false;
    ID3D10SamplerState* samplers[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT] = {0};
    
    // Pixel Shader
    pDevice->PSGetSamplers(0, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
    int psCount = 0;
    for (UINT i = 0; i < D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT; i++) {
        if (samplers[i]) psCount++;
    }
    
    if (psCount > 0) {
        pDevice->PSSetSamplers(0, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
        foundAny = true;
    }
    
    for (UINT i = 0; i < D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT; i++) {
        if (samplers[i]) samplers[i]->Release();
        samplers[i] = nullptr; // Reset for next stage
    }
    
    // Vertex Shader
    pDevice->VSGetSamplers(0, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
    int vsCount = 0;
    for (UINT i = 0; i < D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT; i++) {
        if (samplers[i]) vsCount++;
    }
    
    if (vsCount > 0) {
        pDevice->VSSetSamplers(0, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
        foundAny = true;
    }
    
    for (UINT i = 0; i < D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT; i++) {
        if (samplers[i]) samplers[i]->Release();
        samplers[i] = nullptr;
    }
    
    // Geometry Shader
    pDevice->GSGetSamplers(0, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
    int gsCount = 0;
    for (UINT i = 0; i < D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT; i++) {
        if (samplers[i]) gsCount++;
    }
    
    if (gsCount > 0) {
        pDevice->GSSetSamplers(0, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
        foundAny = true;
    }
    
    for (UINT i = 0; i < D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT; i++) {
        if (samplers[i]) samplers[i]->Release();
        samplers[i] = nullptr;
    }
    
    if (foundAny) {
        HookLog("DX10: Forced sampler rebind (Device=%p, PS=%d, VS=%d, GS=%d)", pDevice, psCount, vsCount, gsCount);
    }
    
    return foundAny;
}

static void DrawDX10Overlay(IDXGISwapChain *pSwapChain, HWND currentHwnd, int frameCount) {
    ID3D10Device* device = nullptr;
    if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D10Device), (void**)&device))) {
        if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D10Device1), (void**)&device))) {
            return;
        }
    }

    // Capture/Hook on the real device seen in Present
    static ID3D10Device* s_HookedDevice = nullptr;
    static bool s_DidRebind = false;
    if (s_HookedDevice != device) {
        // Install runtime hooks on this device vtable if needed
        InstallRuntimeD3D10Hooks(device);
        s_HookedDevice = device;
        s_DidRebind = false;
    }
    
    // One-time rebind for this device to ensure late initialization is caught
    if (!s_DidRebind && g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        if (RebindSamplers10(device)) {
            s_DidRebind = true;
        }
    }
    
    if (g_ImGuiInitialized && currentHwnd != g_CachedHwnd) {
        EarlyLog("DX10: HWND changed from %p to %p. Re-initializing ImGui.", g_CachedHwnd, currentHwnd);
        ImGui_ImplDX10_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_ImGuiInitialized = false;
        g_IsDX10Active = false;
    }

    if (!g_ImGuiInitialized) {
        g_CachedHwnd = currentHwnd;
        g_SharedOverlay.InitImGui(currentHwnd);
        ImGui_ImplDX10_Init(device);
        g_ImGuiInitialized = true;
        g_IsDX10Active = true;
        g_pd3d10Device = device;
        EarlyLog("DX10: ImGui initialized for HWND %p", currentHwnd);
    }

    static IDXGISwapChain* lastSC = nullptr;
    if (!g_mainRenderTargetView10 || pSwapChain != lastSC) {
        if (g_mainRenderTargetView10) {
            g_mainRenderTargetView10->Release();
            g_mainRenderTargetView10 = nullptr;
        }
        
        ID3D10Texture2D *backbuffer = nullptr;
        if (SUCCEEDED(pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
            device->CreateRenderTargetView(backbuffer, NULL, &g_mainRenderTargetView10);
            backbuffer->Release();
        }
        lastSC = pSwapChain;
    }

    // Determine if HDR is active (rare in DX10, but possible via DXGI)
    DXGI_SWAP_CHAIN_DESC dsc;
    pSwapChain->GetDesc(&dsc);
    bool isHDR = (dsc.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || 
                 dsc.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
    g_SharedOverlay.SetHDR(isHDR);

    g_SharedOverlay.SetMetrics(&g_PerfMetrics);
    g_SharedOverlay.SetIPCClient(g_IPC);
    g_SharedOverlay.SetDroppedFrames(g_DX11Capture.droppedFrames.load(std::memory_order_relaxed));
    const char* api = "DX10";
    if (GetModuleHandleA("vulkan-1.dll") || GetModuleHandleA("winevulkan.dll")) {
        api = "DX10 (DXVK)";
    }
    g_SharedOverlay.SetGraphicsAPI(api);
    
    ImGui_ImplDX10_NewFrame();
    g_SharedOverlay.BeginFrame();
    g_SharedOverlay.RenderUI();
    g_SharedOverlay.EndFrame();

    device->OMSetRenderTargets(1, &g_mainRenderTargetView10, NULL);
    ImGui_ImplDX10_RenderDrawData(ImGui::GetDrawData());
    
    device->Release();
}

void DrawDX11Overlay(IDXGISwapChain *pSwapChain) {
  static IDXGISwapChain* lastSwapChain = nullptr;
  static HWND lastHwnd = NULL;
  static int frameCount = 0;
  
  frameCount++;
  
  DXGI_SWAP_CHAIN_DESC desc;
  pSwapChain->GetDesc(&desc);
  HWND currentHwnd = desc.OutputWindow;

  if (frameCount % 60 == 0) {
      EarlyLog("DX: DrawOverlay frame %d on SC %p (HWND %p, %ux%u)", 
               frameCount, pSwapChain, currentHwnd, desc.BufferDesc.Width, desc.BufferDesc.Height);
  }

  // Check for D3D10 FIRST (D3D11 interop means D3D11 QI succeeds even for D3D10 devices!)
  ID3D10Device *device10 = NULL;
  if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D10Device), (void**)&device10))) {
      device10->Release();
      // It's a D3D10 swapchain - use native D3D10 rendering
      DrawDX10Overlay(pSwapChain, currentHwnd, frameCount);
      return;
  }
  
  // Try D3D10.1
  ID3D10Device1 *device10_1 = NULL;
  if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D10Device1), (void**)&device10_1))) {
      device10_1->Release();
      DrawDX10Overlay(pSwapChain, currentHwnd, frameCount);
      return;
  }

  // If we reach here, it's a true DX11 swapchain
  ID3D11Device *device11 = NULL;
  if (FAILED(pSwapChain->GetDevice(IID_PPV_ARGS(&device11)))) {
      return; // Unknown device type
  }
  ID3D11Device *device = device11;
  ID3D11DeviceContext *context = NULL;
  device->GetImmediateContext(&context);

  if (g_ImGuiInitialized && currentHwnd != g_CachedHwnd) {
      EarlyLog("DX11: HWND changed from %p to %p. Re-initializing ImGui.", g_CachedHwnd, currentHwnd);
      ImGui_ImplDX11_Shutdown();
      ImGui_ImplWin32_Shutdown();
      ImGui::DestroyContext();
      g_ImGuiInitialized = false;
      g_IsDX11Active = false;
  }

  if (!g_ImGuiInitialized) {
      g_CachedHwnd = currentHwnd;
      lastHwnd = currentHwnd;

      g_SharedOverlay.InitImGui(currentHwnd);
      ImGui_ImplDX11_Init(device, context);
      g_ImGuiInitialized = true;
      g_IsDX11Active = true;
      EarlyLog("DX11: ImGui initialized for HWND %p", currentHwnd);
      
      g_pd3dDevice = device;
      g_pd3dDeviceContext = context;
  }

  // Detect HWND change (multi-window apps)
  if (currentHwnd != lastHwnd) {
      EarlyLog("DX11: HWND changed from %p to %p. Overlay might not be visible on new window without re-init.", lastHwnd, currentHwnd);
      // We don't re-init ImGui fully here yet as it's dangerous, but we log it.
      // However, we should at least update some internal state if needed.
      lastHwnd = currentHwnd;
  }

  // Determine if HDR is active
  // Re-use desc from above if needed
  bool isHDR = (desc.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || 
               desc.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
  g_SharedOverlay.SetHDR(isHDR);

  g_SharedOverlay.SetMetrics(&g_PerfMetrics);
  g_SharedOverlay.SetIPCClient(g_IPC);
  g_SharedOverlay.SetDroppedFrames(g_DX11Capture.droppedFrames.load(std::memory_order_relaxed));
  const char* finalApi = g_DetectedAPI;
  if (GetModuleHandleA("vulkan-1.dll") || GetModuleHandleA("winevulkan.dll")) {
      if (strcmp(g_DetectedAPI, "DX11") == 0) finalApi = "DX11 (DXVK)";
  }
  g_SharedOverlay.SetGraphicsAPI(finalApi);
  
  ImGui_ImplDX11_NewFrame();
  g_SharedOverlay.BeginFrame();
  g_SharedOverlay.RenderUI();
  g_SharedOverlay.EndFrame();

  // Use cached device/context - some games have broken GetImmediateContext on re-acquire
  if (!g_mainRenderTargetView || pSwapChain != lastSwapChain) {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
    
    EarlyLog("%s: Creating RTV for SwapChain %p (%ux%u)...", g_DetectedAPI, pSwapChain, desc.BufferDesc.Width, desc.BufferDesc.Height);
    
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
    lastSwapChain = pSwapChain;
    EarlyLog("%s: RTV created OK", g_DetectedAPI);
  }

  // DEBUG: Clear to pink every 120 frames to see if we are drawing on the right window
  if (frameCount % 120 == 0) {
      float pink[4] = { 1.0f, 0.0f, 1.0f, 1.0f }; // Magenta/Pink
      g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, pink);
      EarlyLog("DX11: Cleared screen to PINK for frame %d on SC %p", frameCount, pSwapChain);
  }

  g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
  
  // Explicitly set viewport
  D3D11_VIEWPORT vp;
  vp.Width = (float)desc.BufferDesc.Width;
  vp.Height = (float)desc.BufferDesc.Height;
  vp.MinDepth = 0.0f;
  vp.MaxDepth = 1.0f;
  vp.TopLeftX = 0;
  vp.TopLeftY = 0;
  g_pd3dDeviceContext->RSSetViewports(1, &vp);

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
  if (g_mainRenderTargetView10) {
    g_mainRenderTargetView10->Release();
    g_mainRenderTargetView10 = nullptr;
  }
  
  // Invalidate ImGui device objects (they reference old backbuffer)
  if (g_ImGuiInitialized) {
    if (g_IsDX11Active) ImGui_ImplDX11_InvalidateDeviceObjects();
    if (g_IsDX10Active) ImGui_ImplDX10_InvalidateDeviceObjects();
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
    if (g_IsDX11Active) ImGui_ImplDX11_CreateDeviceObjects();
    if (g_IsDX10Active) ImGui_ImplDX10_CreateDeviceObjects();
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

  // Track whether overlay was already drawn (for skip_capture path)
  bool overlayDrawn = false;
  
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

    // Get shared memory for overlay config
    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    bool captureIncludeOverlay = shm ? shm->overlayConfig.captureIncludeOverlay : false;
    bool shouldDrawOverlay = shm && shm->overlayConfig.showOverlay;

    // Lambda for capture operation
    auto doCapture = [&]() {
      if (backbuffer && g_DX11Capture.initialized) {
        int idx = g_DX11Capture.writeIndex;
        ID3D11DeviceContext *captureContext = g_DX11Capture.GetCaptureContext();
        
        if (captureContext) {
            captureContext->CopyResource(g_DX11Capture.sharedTextures[idx], backbuffer);
            
            uint64_t signalValue = 0;
            if (g_DX11Capture.useFences && g_DX11Capture.context4 && g_DX11Capture.fence) {
                g_DX11Capture.fenceValue++;
                signalValue = g_DX11Capture.fenceValue;
                g_DX11Capture.context4->Signal(g_DX11Capture.fence, signalValue);
            }
            
            g_DX11Capture.SignalFrameReady(ipc, idx, qpc.QuadPart, signalValue);
            g_DX11Capture.AdvanceWriteIndex();
        }
      }
    };

    // Lambda for overlay drawing
    auto doOverlay = [&]() {
      if (shouldDrawOverlay) {
        DrawDX11Overlay(pSwapChain);
        overlayDrawn = true;
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

    if (backbuffer)
      backbuffer->Release();
    if (device)
      device->Release();
  } else if (g_DX11Capture.initialized) {
    g_DX11Capture.Cleanup();
  }

skip_capture:
  // Draw overlay if we skipped the capture path or overlay wasn't already drawn
  if (!overlayDrawn) {
    SharedMemoryLayout* shmSkip = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    if (shmSkip && shmSkip->overlayConfig.showOverlay) {
      DrawDX11Overlay(pSwapChain);
    }
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
      
      if (g_DX11Capture.useKeyedMutex && g_DX11Capture.keyedMutexes[idx]) {
          // KEYED MUTEX PATH
          if (g_DX11Capture.keyedMutexes[idx]->AcquireSync(0, 0) == S_OK) {
              captureContext->CopyResource(g_DX11Capture.sharedTextures[idx], backbuffer);
              g_DX11Capture.keyedMutexes[idx]->ReleaseSync(1);
              g_DX11Capture.SignalFrameReady(g_IPC, idx, qpc.QuadPart, 0);
              g_DX11Capture.AdvanceWriteIndex();
          }
      } else if (captureContext) {
          // Legacy Fallback
          captureContext->CopyResource(g_DX11Capture.sharedTextures[idx], backbuffer);
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

// Hook: D3D10 CreateSamplerState - Same logic as D3D11 version
HRESULT STDMETHODCALLTYPE DetourCreateSamplerState10(ID3D10Device *pDevice,
                                                     const D3D10_SAMPLER_DESC *pSamplerDesc,
                                                     ID3D10SamplerState **ppSamplerState) {
    static int callCount = 0;
    callCount++;
    if (callCount <= 10) {
        EarlyLog("DX10: DetourCreateSamplerState10 called (count=%d)", callCount);
    }
    
    if (!pSamplerDesc) return oCreateSamplerState10(pDevice, pSamplerDesc, ppSamplerState);
    if (!g_GraphicsOverridesActive.load(std::memory_order_acquire)) return oCreateSamplerState10(pDevice, pSamplerDesc, ppSamplerState);

    D3D10_SAMPLER_DESC desc = *pSamplerDesc;
    bool modified = false;
    bool debug = false;
    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) {
        debug = true;
    }

    // Check if overrides should be applied
    bool overridesAllowed = true;
    if (pSamplerDesc->MaxLOD == 0.0f) overridesAllowed = false;
    if (pSamplerDesc->MinLOD == pSamplerDesc->MaxLOD) overridesAllowed = false;

    if (overridesAllowed && g_IPC) {
        const auto& gfx = GetActiveGraphicsConfig();
        // Anisotropic Filtering
        std::string af = gfx.anisotropicFiltering;
        if (af != "default") {
            if (af == "off") {
                if ((desc.Filter & D3D10_FILTER_ANISOTROPIC) || (desc.Filter == D3D10_FILTER_ANISOTROPIC)) {
                    desc.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
                    desc.MaxAnisotropy = 1;
                    modified = true;
                }
            } else {
                int maxAniso = 16;
                if (af == "2x") maxAniso = 2;
                else if (af == "4x") maxAniso = 4;
                else if (af == "8x") maxAniso = 8;
                
                if (desc.AddressU == D3D10_TEXTURE_ADDRESS_BORDER || 
                    desc.AddressV == D3D10_TEXTURE_ADDRESS_BORDER || 
                    desc.AddressW == D3D10_TEXTURE_ADDRESS_BORDER) {
                    // Skip AF override for Border address mode
                } else {
                    desc.Filter = D3D10_FILTER_ANISOTROPIC;
                    desc.MaxAnisotropy = maxAniso;
                    modified = true;
                }
            }
        }

        // Mip Mapping
        std::string mip = gfx.mipMapping;
        bool isAniso = (desc.Filter == D3D10_FILTER_ANISOTROPIC);
        
        if (mip != "default" && !isAniso) {
            if (mip == "trilinear") {
                desc.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
                modified = true;
            } else if (mip == "bilinear") {
                desc.Filter = D3D10_FILTER_MIN_MAG_LINEAR_MIP_POINT;
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
        hr = oCreateSamplerState10(pDevice, &desc, ppSamplerState);
        if (FAILED(hr) && debug) {
            EarlyLog("DX10: CreateSamplerState FAILED with modified desc (hr=0x%08X). Filter=0x%X Bias=%.2f Aniso=%u", 
                     hr, desc.Filter, desc.MipLODBias, desc.MaxAnisotropy);
        } else if (debug) {
            static int logCount = 0;
            if (logCount++ < 5) {
                EarlyLog("DX10: CreateSamplerState overridden. Filter=0x%X Bias=%.2f Aniso=%u", 
                         desc.Filter, desc.MipLODBias, desc.MaxAnisotropy);
            }
        }
    } else {
        hr = oCreateSamplerState10(pDevice, pSamplerDesc, ppSamplerState);
    }
    return hr;
}

// Helper: Get or create a replacement sampler with our overrides applied
static ID3D10SamplerState* GetOrCreateReplacementSampler10(ID3D10Device* pDevice, ID3D10SamplerState* pOriginal) {
    if (!pOriginal) return nullptr;

    std::unique_lock<std::shared_mutex> lock(g_SamplerCacheMutex10);

    // 1. If it's already a replacement sampler, don't try to replace it again
    if (g_ReplacementSamplers10.count(pOriginal)) {
        return pOriginal;
    }

    // 2. Check the cache
    auto it = g_SamplerCache10.find(pOriginal);
    if (it != g_SamplerCache10.end()) {
        return it->second;
    }
    
    // Get original sampler description
    D3D10_SAMPLER_DESC originalDesc;
    pOriginal->GetDesc(&originalDesc);
    
    // Check if overrides are applicable
    bool overridesAllowed = true;
    if (originalDesc.MaxLOD == 0.0f) overridesAllowed = false;
    if (originalDesc.MinLOD == originalDesc.MaxLOD) overridesAllowed = false;
    
    if (!overridesAllowed || !g_IPC) {
        g_SamplerCache10[pOriginal] = pOriginal; // Cache as no-op
        return pOriginal;
    }
    
    D3D10_SAMPLER_DESC desc = originalDesc;
    bool modified = false;
    
    const auto& gfx = GetActiveGraphicsConfig();
    
    // Anisotropic Filtering
    std::string af = gfx.anisotropicFiltering;
    if (af != "default") {
        if (af == "off") {
            if (desc.Filter == D3D10_FILTER_ANISOTROPIC) {
                desc.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
                desc.MaxAnisotropy = 1;
                modified = true;
            }
        } else {
            int maxAniso = 16;
            if (af == "2x") maxAniso = 2;
            else if (af == "4x") maxAniso = 4;
            else if (af == "8x") maxAniso = 8;
            
            if (desc.AddressU != D3D10_TEXTURE_ADDRESS_BORDER && 
                desc.AddressV != D3D10_TEXTURE_ADDRESS_BORDER && 
                desc.AddressW != D3D10_TEXTURE_ADDRESS_BORDER) {
                desc.Filter = D3D10_FILTER_ANISOTROPIC;
                desc.MaxAnisotropy = maxAniso;
                modified = true;
            }
        }
    }

    // Mip Mapping
    std::string mip = gfx.mipMapping;
    bool isAniso = (desc.Filter == D3D10_FILTER_ANISOTROPIC);
    
    if (mip != "default" && !isAniso) {
        if (mip == "trilinear") {
            desc.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
            modified = true;
        } else if (mip == "bilinear") {
            desc.Filter = D3D10_FILTER_MIN_MAG_LINEAR_MIP_POINT;
            modified = true;
        }
    }

    // Mip Bias
    std::string bias = gfx.mipBias;
    if (bias != "default") {
        try {
            float biasVal = std::stof(bias);
            std::string mode = gfx.mipBiasMode;
            
            if (mode == "offset") {
                desc.MipLODBias += biasVal;
                modified = true;
            } else if (mode == "base") {
                // Apply only if original has negative bias
                if (desc.MipLODBias < 0.0f) {
                    desc.MipLODBias += biasVal;
                    modified = true;
                }
            } else {
                // Strict (default) - Absolute override
                desc.MipLODBias = biasVal;
                modified = true;
            }
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
    
    if (!modified) {
        g_SamplerCache10[pOriginal] = pOriginal; // Cache as no-op
        return pOriginal;
    }
    
    // Create the replacement sampler
    ID3D10SamplerState* pReplacement = nullptr;
    HRESULT hr = pDevice->CreateSamplerState(&desc, &pReplacement);
    if (SUCCEEDED(hr)) {
        g_SamplerCache10[pOriginal] = pReplacement;
        g_ReplacementSamplers10.insert(pReplacement);
        static int logCount = 0;
        if (logCount++ < 10) {
            EarlyLog("DX10: Created replacement sampler %p -> %p (AF=%d, Bias=%.2f)", 
                     pOriginal, pReplacement, desc.MaxAnisotropy, desc.MipLODBias);
        }
        return pReplacement;
    } else {
        g_SamplerCache10[pOriginal] = pOriginal; // Failed, use original
        return pOriginal;
    }
}

// D3D10 PSSetSamplers detour
static void STDMETHODCALLTYPE DetourPSSetSamplers10(ID3D10Device *pDevice, UINT StartSlot, UINT NumSamplers, ID3D10SamplerState *const *ppSamplers) {
    static int callCount = 0;
    callCount++;
    if (callCount <= 5) {
        EarlyLog("DX10: DetourPSSetSamplers10 called (StartSlot=%u, NumSamplers=%u)", StartSlot, NumSamplers);
    }
    
    if (!ppSamplers || NumSamplers == 0 || !g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        oPSSetSamplers10(pDevice, StartSlot, NumSamplers, ppSamplers);
        return;
    }
    
    // Cache device for later sampler creation
    if (!g_CachedD3D10Device) g_CachedD3D10Device = pDevice;
    
    // Clamp NumSamplers to avoid stack overflow
    UINT actualNum = (NumSamplers > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT) ? D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT : NumSamplers;
    
    ID3D10SamplerState* replacedSamplers[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT];
    for (UINT i = 0; i < actualNum; i++) {
        replacedSamplers[i] = GetOrCreateReplacementSampler10(pDevice, ppSamplers[i]);
    }
    
    // For anything beyond actualNum, we don't care about replacedSamplers
    
    oPSSetSamplers10(pDevice, StartSlot, NumSamplers, replacedSamplers);
}

// D3D10 VSSetSamplers detour
static void STDMETHODCALLTYPE DetourVSSetSamplers10(ID3D10Device *pDevice, UINT StartSlot, UINT NumSamplers, ID3D10SamplerState *const *ppSamplers) {
    if (!ppSamplers || NumSamplers == 0 || !g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        oVSSetSamplers10(pDevice, StartSlot, NumSamplers, ppSamplers);
        return;
    }
    
    if (!g_CachedD3D10Device) g_CachedD3D10Device = pDevice;
    
    // Clamp NumSamplers to avoid stack overflow
    UINT actualNum = (NumSamplers > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT) ? D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT : NumSamplers;

    ID3D10SamplerState* replacedSamplers[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT];
    for (UINT i = 0; i < actualNum; i++) {
        replacedSamplers[i] = GetOrCreateReplacementSampler10(pDevice, ppSamplers[i]);
    }
    
    oVSSetSamplers10(pDevice, StartSlot, NumSamplers, replacedSamplers);
}

// D3D10 GSSetSamplers detour
static void STDMETHODCALLTYPE DetourGSSetSamplers10(ID3D10Device *pDevice, UINT StartSlot, UINT NumSamplers, ID3D10SamplerState *const *ppSamplers) {
    if (!ppSamplers || NumSamplers == 0 || !g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        oGSSetSamplers10(pDevice, StartSlot, NumSamplers, ppSamplers);
        return;
    }
    
    if (!g_CachedD3D10Device) g_CachedD3D10Device = pDevice;
    
    // Clamp NumSamplers to avoid stack overflow
    UINT actualNum = (NumSamplers > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT) ? D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT : NumSamplers;

    ID3D10SamplerState* replacedSamplers[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT];
    for (UINT i = 0; i < actualNum; i++) {
        replacedSamplers[i] = GetOrCreateReplacementSampler10(pDevice, ppSamplers[i]);
    }
    
    oGSSetSamplers10(pDevice, StartSlot, NumSamplers, replacedSamplers);
}

// Helper: Install hooks on a specific D3D10 device VTable at runtime
// This ensures we catch the correct VTable even if it differs from our temp device
static void InstallRuntimeD3D10Hooks(ID3D10Device* pDevice) {
    if (!pDevice) return;
    
    void **pVTable = *(void ***)pDevice;
    MH_STATUS status;

    // PSSetSamplers (Index 6)
    status = MH_CreateHook(pVTable[6], (LPVOID)&DetourPSSetSamplers10, (LPVOID *)&oPSSetSamplers10);
    if (status == MH_OK || status == MH_ERROR_ALREADY_CREATED) {
        MH_EnableHook(pVTable[6]);
        if (status == MH_OK) HookLog("DX10: Runtime PSSetSamplers hook installed");
    }

    // VSSetSamplers (Index 20)
    status = MH_CreateHook(pVTable[20], (LPVOID)&DetourVSSetSamplers10, (LPVOID *)&oVSSetSamplers10);
    if (status == MH_OK || status == MH_ERROR_ALREADY_CREATED) {
        MH_EnableHook(pVTable[20]);
        if (status == MH_OK) HookLog("DX10: Runtime VSSetSamplers hook installed");
    }

    // GSSetSamplers (Index 23)
    status = MH_CreateHook(pVTable[23], (LPVOID)&DetourGSSetSamplers10, (LPVOID *)&oGSSetSamplers10);
    if (status == MH_OK || status == MH_ERROR_ALREADY_CREATED) {
        MH_EnableHook(pVTable[23]);
        if (status == MH_OK) HookLog("DX10: Runtime GSSetSamplers hook installed");
    }
}


void DX11Hook::Init() {
  HookLog("DX11Hook::Init()");

  // 1. Hook D3D11 primary entry point
  HMODULE hD3D11 = GetModuleHandleA("d3d11.dll");
  if (hD3D11) {
    oD3D11CreateDeviceAndSwapChain = (D3D11CreateDeviceAndSwapChain_t)GetProcAddress(hD3D11, "D3D11CreateDeviceAndSwapChain");
    if (oD3D11CreateDeviceAndSwapChain) {
      if (MH_CreateHook((LPVOID)oD3D11CreateDeviceAndSwapChain, (LPVOID)&DetourD3D11CreateDeviceAndSwapChain,
                        (LPVOID *)&oD3D11CreateDeviceAndSwapChain) == MH_OK) {
        MH_EnableHook((LPVOID)oD3D11CreateDeviceAndSwapChain);
        HookLog("DX11: D3D11CreateDeviceAndSwapChain hook installed.");
      }
    }
  }

  // 2. Hook D3D10 entry points
  HMODULE hD3D10 = GetModuleHandleA("d3d10.dll");
  if (hD3D10) {
    oD3D10CreateDeviceAndSwapChain = (D3D10CreateDeviceAndSwapChain_t)GetProcAddress(hD3D10, "D3D10CreateDeviceAndSwapChain");
    if (oD3D10CreateDeviceAndSwapChain) {
      if (MH_CreateHook((LPVOID)oD3D10CreateDeviceAndSwapChain, (LPVOID)&DetourD3D10CreateDeviceAndSwapChain,
                        (LPVOID *)&oD3D10CreateDeviceAndSwapChain) == MH_OK) {
        MH_EnableHook((LPVOID)oD3D10CreateDeviceAndSwapChain);
        HookLog("DX11: D3D10CreateDeviceAndSwapChain hook installed.");
      }
    }
    
    oD3D10CreateDevice = (D3D10CreateDevice_t)GetProcAddress(hD3D10, "D3D10CreateDevice");
    if (oD3D10CreateDevice) {
      if (MH_CreateHook((LPVOID)oD3D10CreateDevice, (LPVOID)&DetourD3D10CreateDevice,
                        (LPVOID *)&oD3D10CreateDevice) == MH_OK) {
        MH_EnableHook((LPVOID)oD3D10CreateDevice);
        HookLog("DX11: D3D10CreateDevice hook installed.");
      }
    }
  }

  HMODULE hD3D10_1 = GetModuleHandleA("d3d10_1.dll");
  if (hD3D10_1) {
    oD3D10CreateDeviceAndSwapChain1 = (D3D10CreateDeviceAndSwapChain1_t)GetProcAddress(hD3D10_1, "D3D10CreateDeviceAndSwapChain1");
    if (oD3D10CreateDeviceAndSwapChain1) {
      if (MH_CreateHook((LPVOID)oD3D10CreateDeviceAndSwapChain1, (LPVOID)&DetourD3D10CreateDeviceAndSwapChain1,
                        (LPVOID *)&oD3D10CreateDeviceAndSwapChain1) == MH_OK) {
        MH_EnableHook((LPVOID)oD3D10CreateDeviceAndSwapChain1);
        HookLog("DX11: D3D10CreateDeviceAndSwapChain1 hook installed.");
      }
    }
    
    oD3D10CreateDevice1 = (D3D10CreateDevice1_t)GetProcAddress(hD3D10_1, "D3D10CreateDevice1");
    if (oD3D10CreateDevice1) {
      if (MH_CreateHook((LPVOID)oD3D10CreateDevice1, (LPVOID)&DetourD3D10CreateDevice1,
                        (LPVOID *)&oD3D10CreateDevice1) == MH_OK) {
        MH_EnableHook((LPVOID)oD3D10CreateDevice1);
        HookLog("DX11: D3D10CreateDevice1 hook installed.");
      }
    }
  } else {
      HookLog("DX11: d3d10_1.dll not loaded, skipping hooks.");
  }

  // 3. Hook DXGI Factory entry points
  HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
  if (hDXGI) {
      oCreateDXGIFactory = (CreateDXGIFactory_t)GetProcAddress(hDXGI, "CreateDXGIFactory");
      if (oCreateDXGIFactory) {
          MH_CreateHook((LPVOID)oCreateDXGIFactory, (LPVOID)&DetourCreateDXGIFactory, (LPVOID*)&oCreateDXGIFactory);
          MH_EnableHook((LPVOID)oCreateDXGIFactory);
      }
      oCreateDXGIFactory1 = (CreateDXGIFactory1_t)GetProcAddress(hDXGI, "CreateDXGIFactory1");
      if (oCreateDXGIFactory1) {
          MH_CreateHook((LPVOID)oCreateDXGIFactory1, (LPVOID)&DetourCreateDXGIFactory1, (LPVOID*)&oCreateDXGIFactory1);
          MH_EnableHook((LPVOID)oCreateDXGIFactory1);
      }
      oCreateDXGIFactory2 = (CreateDXGIFactory2_t)GetProcAddress(hDXGI, "CreateDXGIFactory2");
      if (oCreateDXGIFactory2) {
          MH_CreateHook((LPVOID)oCreateDXGIFactory2, (LPVOID)&DetourCreateDXGIFactory2, (LPVOID*)&oCreateDXGIFactory2);
          MH_EnableHook((LPVOID)oCreateDXGIFactory2);
      }
      HookLog("DX11: DXGI Factory creation hooks installed.");
  }

  // 4. Early DXGI Factory Hooking (via dummy device)
  // We create a dummy device to get a factory and hook it early. 
  // This helps when games create devices via D3D11CreateDevice and then create swapchains via factory.
  if (hD3D11) {
      ID3D11Device* dummyDevice = nullptr;
      D3D_FEATURE_LEVEL fl;
      if (SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dummyDevice, &fl, NULL))) {
          HookDXGIFactory(dummyDevice);
          dummyDevice->Release();
      }
  }

  // 5. Scan for EXISTING swapchains (late injection scenario)
  // If the game already created the device/swapchain before we injected,
  // we need to hook the vtable of an EXISTING swapchain.
  // We do this by creating a temporary swapchain using the hooked factory,
  // which will also trigger our InstallVTableHooks.
  HookLog("DX11: Scanning for pre-existing swapchains...");
  
  // First, try D3D10 route (the game is D3D10)
  if (hD3D10) {
      typedef HRESULT(WINAPI *PFN_D3D10CreateDevice)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT, ID3D10Device**);
      PFN_D3D10CreateDevice pD3D10CD = (PFN_D3D10CreateDevice)GetProcAddress(hD3D10, "D3D10CreateDevice");
      if (pD3D10CD) {
          ID3D10Device* tempDevice = nullptr;
          // Use the REAL function, not our detour, to create a temp device
          HRESULT hr = pD3D10CD(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_SDK_VERSION, &tempDevice);
          if (SUCCEEDED(hr) && tempDevice) {
              // Get DXGI factory from temp device
              IDXGIDevice* dxgiDev = nullptr;
              if (SUCCEEDED(tempDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) {
                  IDXGIAdapter* adapter = nullptr;
                  if (SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
                      IDXGIFactory* factory = nullptr;
                      if (SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory))) {
                          // Create a temp hidden window for temp swapchain
                          HWND tempHwnd = CreateWindowExA(0, "STATIC", "TempDXGI", WS_OVERLAPPEDWINDOW, 
                                                           0, 0, 100, 100, NULL, NULL, GetModuleHandle(NULL), NULL);
                          if (tempHwnd) {
                              DXGI_SWAP_CHAIN_DESC scd = {};
                              scd.BufferCount = 1;
                              scd.BufferDesc.Width = 100;
                              scd.BufferDesc.Height = 100;
                              scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                              scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                              scd.OutputWindow = tempHwnd;
                              scd.SampleDesc.Count = 1;
                              scd.Windowed = TRUE;
                              scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
                              
                              IDXGISwapChain* tempSC = nullptr;
                              // This call goes through our detour and will install vtable hooks!
                              hr = factory->CreateSwapChain(tempDevice, &scd, &tempSC);
                              if (SUCCEEDED(hr) && tempSC) {
                                  HookLog("DX11: Temp D3D10 swapchain created to install vtable hooks");
                                  tempSC->Release();
                              }
                              DestroyWindow(tempHwnd);
                          }
                          factory->Release();
                      }
                      adapter->Release();
                  }
                  dxgiDev->Release();
              }
              tempDevice->Release();
          }
      }
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
  if (g_mainRenderTargetView10) {
    g_mainRenderTargetView10->Release();
    g_mainRenderTargetView10 = nullptr;
  }
}

void DX11Hook::OnHostDisconnect() {
  HookLog("DX11Hook::OnHostDisconnect() - ready for reconnection");
  // DX11 capture is synchronous, nothing to stop
  // Just cleanup for potential new session
  g_DX11Capture.Cleanup();
}
