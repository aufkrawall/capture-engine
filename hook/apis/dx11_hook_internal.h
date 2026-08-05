#pragma once

class D3D11InternalIdentityProbeScope;

struct D3D11ContextVTableOriginals;

struct D3D11SamplerCacheEntry;

struct D3D11StageState;

struct D3D11PerContextState;

class DX11Capture;

#include <atomic>

#include <cmath>

#include <cstdint>

#include <cstdio>

#include <cstring>

#include <limits>

#include <mutex>

#include <shared_mutex>

#include <string>

#include <unordered_map>

#include <unordered_set>

#include <vector>

#include "../../common/frame_timing.h"

#include "../../common/raii_helpers.h"

#include "../common/capture_base.h"

#include "../common/capture_pacing.h"

#include "../common/deferred_release.h"

#include "../common/dll_utils.h"

#include "../common/fg_detection.h"

#include "../common/fps_limiter.h"

#include "../common/graphics_api_identity.h"

#include "../common/hook_common.h"

#include "../common/overlay_adapter.h"

#include "../common/overlay_metrics_publisher.h"

#include "../common/perf_logger.h"

#include "../common/sampler_override_utils.h"

#include "../common/screenshot_hook.h"

#include "../common/system_metrics.h"

#include "../wrappers/dxgi_swapchain_wrap.h"

#include "../wrappers/iat_hook.h"

#include "../wrappers/wrapper_base.h"

#include "dx11_hook.h"

#include "dx12_hook.h"

#include "dxgi_shared.h"

#include "graphics_hook.h"

#include "lod_helper.h"

#include "performance_metrics.h"

// Global Deferred Release Queue for D3D11 resources
// Prevents render thread stalls during resource destruction
extern ce::DeferredReleaseQueue g_DeferredRelease;

#include <d3d10.h>    // For DX10 detection

#include <d3d10_1.h>  // For DX10.1 detection

#include <d3d11.h>

#include <d3d11_1.h>  // For ID3D11DeviceContext1

#include <d3d11_4.h>  // For ID3D11Fence and ID3D11Device5

#include <d3d12.h>    // For ID3D12CommandQueue detection

#include <d3dcompiler.h>

#include <dxgi1_2.h>  // Required for IDXGIResource1 and CreateSharedHandle

#include <dxgi1_4.h>  // For IDXGISwapChain3

#include "../common/input_manager.h"

#include "../wrappers/custom_hook.h"

#include "../wrappers/d3d11_devicecontext_wrap.h"

#include "../wrappers/iat_hook.h"

#include "../wrappers/vtable_hook.h"

#include "../wrappers/wrapper_base.h"

extern thread_local unsigned g_D3D11InternalIdentityProbeDepth;

typedef HRESULT(STDMETHODCALLTYPE* D3D11QueryInterface_t)(IUnknown*, REFIID, void**);

// Re-entrancy guard: prevents mutual recursion with other Present hooks (e.g. Steam overlay)
extern thread_local bool g_InPresentHook;

typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);

typedef HRESULT(STDMETHODCALLTYPE* ResizeBuffers_t)(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width,
                                                    UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);

typedef HRESULT(STDMETHODCALLTYPE* Present1_t)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT PresentFlags,
                                               const DXGI_PRESENT_PARAMETERS* pPresentParameters);

#ifndef DXGI_PRESENT_ALLOW_TEARING
#define DXGI_PRESENT_ALLOW_TEARING 0x00000200UL
#endif

typedef HRESULT(STDMETHODCALLTYPE* CreateSamplerState_t)(ID3D11Device* pDevice, const D3D11_SAMPLER_DESC* pSamplerDesc,
                                                         ID3D11SamplerState** ppSamplerState);

typedef HRESULT(STDMETHODCALLTYPE* CreatePixelShader11_t)(ID3D11Device* pDevice, const void* pShaderBytecode,
                                                          SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage,
                                                          ID3D11PixelShader** ppPixelShader);

typedef HRESULT(STDMETHODCALLTYPE* CreateDeferredContext11_t)(ID3D11Device* pDevice, UINT ContextFlags,
                                                              ID3D11DeviceContext** ppDeferredContext);

typedef void(STDMETHODCALLTYPE* SetShaderResources11_t)(ID3D11DeviceContext* pContext, UINT StartSlot, UINT NumViews,
                                                        ID3D11ShaderResourceView* const* ppShaderResourceViews);

typedef void(STDMETHODCALLTYPE* SetSamplers11_t)(ID3D11DeviceContext* pContext, UINT StartSlot, UINT NumSamplers,
                                                 ID3D11SamplerState* const* ppSamplers);

typedef void(STDMETHODCALLTYPE* PSSetShader11_t)(ID3D11DeviceContext* pContext, ID3D11PixelShader* pPixelShader,
                                                 ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances);

typedef void(STDMETHODCALLTYPE* DrawIndexed11_t)(ID3D11DeviceContext* pContext, UINT IndexCount,
                                                 UINT StartIndexLocation, INT BaseVertexLocation);

typedef void(STDMETHODCALLTYPE* Draw11_t)(ID3D11DeviceContext* pContext, UINT VertexCount, UINT StartVertexLocation);

typedef void(STDMETHODCALLTYPE* DrawIndexedInstanced11_t)(ID3D11DeviceContext* pContext, UINT IndexCountPerInstance,
                                                          UINT InstanceCount, UINT StartIndexLocation,
                                                          INT BaseVertexLocation, UINT StartInstanceLocation);

typedef void(STDMETHODCALLTYPE* DrawInstanced11_t)(ID3D11DeviceContext* pContext, UINT VertexCountPerInstance,
                                                   UINT InstanceCount, UINT StartVertexLocation,
                                                   UINT StartInstanceLocation);

typedef void(STDMETHODCALLTYPE* DrawAuto11_t)(ID3D11DeviceContext* pContext);

typedef void(STDMETHODCALLTYPE* DrawIndexedInstancedIndirect11_t)(ID3D11DeviceContext* pContext,
                                                                  ID3D11Buffer* pBufferForArgs,
                                                                  UINT AlignedByteOffsetForArgs);

typedef void(STDMETHODCALLTYPE* DrawInstancedIndirect11_t)(ID3D11DeviceContext* pContext, ID3D11Buffer* pBufferForArgs,
                                                           UINT AlignedByteOffsetForArgs);

typedef void(STDMETHODCALLTYPE* ExecuteCommandList11_t)(ID3D11DeviceContext* pContext, ID3D11CommandList* pCommandList,
                                                        BOOL RestoreContextState);

// D3D10 CreateSamplerState hook
typedef HRESULT(STDMETHODCALLTYPE* CreateSamplerState10_t)(ID3D10Device* pDevice,
                                                           const D3D10_SAMPLER_DESC* pSamplerDesc,
                                                           ID3D10SamplerState** ppSamplerState);

#include <algorithm>

#include <atomic>

#include <shared_mutex>

#include <unordered_map>

#include <unordered_set>

#include <vector>

enum class D3D11ShaderStage : uint32_t {
    Pixel,
    Vertex,
    Geometry,
    Hull,
    Domain,
    Compute,
};

// Use typedef from dx11_hook.h
// typedef HRESULT(WINAPI *D3D11CreateDeviceAndSwapChain_t)(...);
// Global original function pointer - set by IAT patching
extern PFN_D3D11CreateDeviceAndSwapChain oD3D11CreateDeviceAndSwapChain;

typedef HRESULT(WINAPI* PFN_D3D10CreateDeviceAndSwapChain)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT,
                                                           DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D10Device**);

typedef HRESULT(WINAPI* PFN_D3D10CreateDeviceAndSwapChain1)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT,
                                                            D3D10_FEATURE_LEVEL1, UINT, DXGI_SWAP_CHAIN_DESC*,
                                                            IDXGISwapChain**, ID3D10Device1**);

typedef HRESULT(WINAPI* PFN_D3D10CreateDevice)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT, ID3D10Device**);

typedef HRESULT(WINAPI* PFN_D3D10CreateDevice1)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, D3D10_FEATURE_LEVEL1,
                                                UINT, ID3D10Device1**);

typedef HRESULT(WINAPI* PFN_CreateDXGIFactory)(REFIID, void**);

typedef HRESULT(WINAPI* PFN_CreateDXGIFactory1)(REFIID, void**);

typedef HRESULT(WINAPI* PFN_CreateDXGIFactory2)(UINT, REFIID, void**);

typedef HRESULT(STDMETHODCALLTYPE* CreateSwapChain_t)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                      IDXGISwapChain**);

typedef HRESULT(STDMETHODCALLTYPE* CreateSwapChainForHwnd_t)(IDXGIFactory2*, IUnknown*, HWND,
                                                             const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                             IDXGISwapChain1**);

extern void DX12_SignalFSR4SwapchainRecreated();

extern void DX12_InvalidateSwapchain();

void DX11Hook_BeginInternalIdentityProbe();

void DX11Hook_EndInternalIdentityProbe();

void DX10Hook_RegisterDeviceIdentity(ID3D10Device* device, bool is10_1, const char* evidence);

void DX10Hook_RegisterSwapChainIdentity(IDXGISwapChain* swapChain, bool is10_1, const char* evidence);

void DX11Hook_RegisterDeviceIdentity(ID3D11Device* device, const char* evidence, bool newDevice);

void DX11Hook_ReportApiUse(ID3D11Device* device, unsigned minorVersion, const char* evidence);

void DX11Hook_BeginWrapperContextForwarding();

void DX11Hook_EndWrapperContextForwarding();

bool DX11Hook_IsWrapperContextForwarding();

void DX11Hook_BeginWrapperSamplerForwarding();

void DX11Hook_EndWrapperSamplerForwarding();

bool DX11Hook_IsWrapperSamplerForwarding();

bool DX11Hook_ApplySamplerOverrides(D3D11_SAMPLER_DESC& desc, const GraphicsConfig& gfx, bool allowAnisotropicOverride);

void CleanupDX11Resources(bool releaseDeviceContext = true);

void HandleDX11ProcessFrame(IDXGISwapChain* pSwapChain, bool isRealFrame);

void DrawDX11Overlay(IDXGISwapChain* pSwapChain);

void ApplyDeferredSamplerOverrides11(IDXGISwapChain* pSwapChain);

HRESULT STDMETHODCALLTYPE DetourDX11Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);

HRESULT STDMETHODCALLTYPE DetourDX11Present1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters);

HRESULT WINAPI DX11_DetourCreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc, IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext);

void DX11Hook_OnSwapChainCreated(IDXGISwapChain* pSwapChain);

void DX11_UpdatePerformanceMetrics(int64_t qpcUs);

void DX11Hook_OnSwapChainCreated(IDXGISwapChain* pSwapChain);

void DX11Hook_InstallDeviceAndContextHooks(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, IDXGISwapChain* pSwapChain);

void DX11_UpdatePerformanceMetrics(int64_t qpcUs);

void CleanupDX11Resources(bool releaseDeviceContext);

extern void DrawDX11Overlay(IDXGISwapChain* pSwapChain);

void HandleDX11ProcessFrame(IDXGISwapChain* pSwapChain, bool isRealFrame);

void HandleDX11ResizeBegin();

void DX11_ProcessFrameExternal(IDXGISwapChain* pSwapChain);

void DrawDX11Overlay(IDXGISwapChain* pSwapChain);

HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);

void ApplyPrerenderLimit(IDXGISwapChain* pSwapChain, float limit);

namespace DXGIShared {
void HandleDX11ProcessFrame(IDXGISwapChain* pSwapChain, bool isRealFrame);
}

namespace DXGIShared {
void HandleDX11ResizeBegin();
}

HRESULT STDMETHODCALLTYPE DetourCreateSamplerState(ID3D11Device* pDevice, const D3D11_SAMPLER_DESC* pSamplerDesc, ID3D11SamplerState** ppSamplerState);

bool DX10Hook_ApplySamplerOverrides(D3D10_SAMPLER_DESC& desc, const GraphicsConfig& gfx);

HRESULT STDMETHODCALLTYPE DetourCreateSamplerState10(ID3D10Device* pDevice, const D3D10_SAMPLER_DESC* pSamplerDesc, ID3D10SamplerState** ppSamplerState);

// Globals
// Cached device/context with AddRef — avoids calling GetDevice() every frame
// (which crashes during shutdown when the swapchain's internal device ref is
// freed)
inline ID3D11Device* dx11_hook_g_pd3dDevice = NULL;

inline ID3D11DeviceContext* dx11_hook_g_pd3dDeviceContext = NULL;

inline ID3D11RenderTargetView* dx11_hook_g_mainRenderTargetView = NULL;

inline IDXGISwapChain* dx11_hook_g_D3D11IdentitySwapChain = NULL;

inline ID3D11Device* dx11_hook_g_D3D11IdentityDevice = NULL;

inline ID3D10RenderTargetView* dx11_hook_g_mainRenderTargetView10 = NULL;

inline const char* dx11_hook_g_DetectedAPI = "DX11";

inline std::mutex dx11_hook_g_GraphicsApiIdentityMutex;

inline std::unordered_map<ID3D10Device*, bool> dx11_hook_g_D3D10DeviceIdentities;

inline std::unordered_map<IDXGISwapChain*, bool> dx11_hook_g_D3D10SwapChainIdentities;

inline ce::graphics_api_identity::ScopedIdentityRegistry<unsigned> dx11_hook_g_D3D11MinorUse;

class D3D11InternalIdentityProbeScope {
public:
    D3D11InternalIdentityProbeScope() {
        DX11Hook_BeginInternalIdentityProbe();
    }
    ~D3D11InternalIdentityProbeScope() {
        DX11Hook_EndInternalIdentityProbe();
    }
};

inline bool ResolveD3D10Is10_1(ID3D10Device* device, IDXGISwapChain* swapChain) {
    std::lock_guard<std::mutex> lock(dx11_hook_g_GraphicsApiIdentityMutex);
    if (device) {
        const auto deviceIt = dx11_hook_g_D3D10DeviceIdentities.find(device);
        if (deviceIt != dx11_hook_g_D3D10DeviceIdentities.end())
            return deviceIt->second;
    }
    const auto swapChainIt = dx11_hook_g_D3D10SwapChainIdentities.find(swapChain);
    return swapChainIt != dx11_hook_g_D3D10SwapChainIdentities.end() && swapChainIt->second;
}

inline unsigned ResolveD3D11MinorUse(ID3D11Device* device) {
    std::lock_guard<std::mutex> lock(dx11_hook_g_GraphicsApiIdentityMutex);
    unsigned identity = 0;
    dx11_hook_g_D3D11MinorUse.TryGet(device, &identity);
    return identity;
}

inline std::unordered_map<void**, D3D11QueryInterface_t> dx11_hook_g_D3D11QueryInterfaceOriginals;

inline unsigned D3D11DeviceMinorFromIID(REFIID iid) {
    if (iid == IID_ID3D11Device1)
        return 1;
    if (iid == IID_ID3D11Device2)
        return 2;
    if (iid == IID_ID3D11Device3)
        return 3;
    if (iid == IID_ID3D11Device4 || iid == IID_ID3D11Device5)
        return 4;
    return 0;
}

inline unsigned D3D11ContextMinorFromIID(REFIID iid) {
    if (iid == IID_ID3D11DeviceContext1)
        return 1;
    if (iid == IID_ID3D11DeviceContext2)
        return 2;
    if (iid == IID_ID3D11DeviceContext3)
        return 3;
    if (iid == IID_ID3D11DeviceContext4)
        return 4;
    return 0;
}

inline HRESULT STDMETHODCALLTYPE DetourD3D11QueryInterface(IUnknown* object, REFIID iid, void** result) {
    D3D11QueryInterface_t original = nullptr;
    void** vtable = object ? *(void***)object : nullptr;
    {
        std::lock_guard<std::mutex> lock(dx11_hook_g_GraphicsApiIdentityMutex);
        const auto it = dx11_hook_g_D3D11QueryInterfaceOriginals.find(vtable);
        if (it != dx11_hook_g_D3D11QueryInterfaceOriginals.end())
            original = it->second;
    }
    if (!original)
        return E_NOINTERFACE;

    const HRESULT hr = original(object, iid, result);
    if (FAILED(hr) || !result || !*result || g_D3D11InternalIdentityProbeDepth != 0)
        return hr;

    const unsigned deviceMinor = D3D11DeviceMinorFromIID(iid);
    if (deviceMinor != 0) {
        DX11Hook_ReportApiUse(reinterpret_cast<ID3D11Device*>(object), deviceMinor,
                              "external D3D11 device QueryInterface");
        return hr;
    }

    const unsigned contextMinor = D3D11ContextMinorFromIID(iid);
    if (contextMinor != 0) {
        ID3D11Device* device = nullptr;
        reinterpret_cast<ID3D11DeviceContext*>(object)->GetDevice(&device);
        DX11Hook_ReportApiUse(device, contextMinor, "external D3D11 context QueryInterface");
        if (device)
            device->Release();
    }
    return hr;
}

inline void InstallD3D11IdentityQueryHook(IUnknown* object, const char* source) {
    if (!object)
        return;
    void** vtable = *(void***)object;
    std::lock_guard<std::mutex> lock(dx11_hook_g_GraphicsApiIdentityMutex);
    if (dx11_hook_g_D3D11QueryInterfaceOriginals.find(vtable) != dx11_hook_g_D3D11QueryInterfaceOriginals.end())
        return;

    D3D11QueryInterface_t original = nullptr;
    if (VTableHook::Create(reinterpret_cast<void*>(&vtable[0]), (LPVOID)&DetourD3D11QueryInterface, (LPVOID*)&original) != VTableHook::Success ||
        !original) {
        HookLog("[GraphicsAPI] D3D11 QueryInterface hook failed object=%p source=%s", object,
                source ? source : "unknown");
        return;
    }
    dx11_hook_g_D3D11QueryInterfaceOriginals.emplace(vtable, original);
    HookLog("[GraphicsAPI] D3D11 QueryInterface evidence hook installed object=%p source=%s", object,
            source ? source : "unknown");
}

// Prerender Limit Fencing
inline std::vector<ID3D11Query*> dx11_hook_g_PrerenderQueries;

inline uint64_t dx11_hook_g_PrerenderFrameIndex = 0;

inline ID3D11Device* dx11_hook_g_PrerenderQueryDevice = nullptr;

inline ID3D10Device* dx11_hook_g_PrerenderQueryDevice10 = nullptr;

inline ID3D10Query* dx11_hook_g_PrerenderSerialQuery10 = nullptr;

inline std::mutex dx11_hook_g_PrerenderMutex;

// Deferred state bootstrap: used after Present hook installation so games that
// bound samplers before our vtable hooks still get a tracked logical state.
// This must be per-context: UE3 titles can create several short-lived D3D11
// devices before the real game swapchain, and a process-wide latch would make
// the final context keep blurry original samplers forever.
inline std::mutex dx11_hook_g_DeferredAFBootstrapMutex;

inline std::unordered_set<uintptr_t> dx11_hook_g_DeferredAFBootstrappedContexts;

// Sampler override diagnostic counters (rate-limited logging)
inline std::atomic<int> dx11_hook_g_DiagSamplerAllowsAF{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerSkipNoMips{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerSkipBorder{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerSkipReduction{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerSkipComparison{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerSkipStage{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerSkipNoSRV{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerSkipFormat{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerSkipSingleMip{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerSkipNonColorResource{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerSkipUnsafeResource{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerSkipNoShader{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerSkipNoShaderMetadata{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerSkipShaderUnused{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerSkipExplicitSample{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerAllowLodSample{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerReplacementCreated{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerRebound{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerDrawReconcileCalls{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerReconcileSlots{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerBindDeferred{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerEffectiveBindCalls{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerEffectiveBinds{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerEffectiveBindSkips{0};

inline std::atomic<int> dx11_hook_g_DiagDeferredAFBootstrapComplete{0};

inline std::atomic<int> dx11_hook_g_DiagDeferredAFBootstrapRetry{0};

inline std::atomic<int> dx11_hook_g_DiagDeferredAFBootstrapDisabled{0};

inline std::atomic<int> dx11_hook_g_DiagSamplerRuntimeHookInstalled{0};

inline std::atomic<int> dx11_hook_g_DiagD3D11ContextVTablesHooked{0};

inline std::atomic<int> dx11_hook_g_DiagD3D11ContextHookSkips{0};

inline std::atomic<int> dx11_hook_g_DiagCreateDeferredContext11{0};

inline std::atomic<int> dx11_hook_g_DiagExecuteCommandList11{0};

inline std::atomic<int> dx11_hook_g_DiagPrerenderFrames{0};

inline std::atomic<int> dx11_hook_g_DiagPrerenderWaits{0};

// Cross-function tracking for FSR4/FG swapchain recreation detection
// Shared between DetourCreateSwapChain and DetourCreateSwapChainForHwnd
inline bool dx11_hook_g_FirstGameSwapchainCreated = false;

// Forces overlay and capture to use the same backbuffer index within one frame.
// This eliminates index races on FLIP swapchains.
inline thread_local int dx11_hook_g_ForcedCaptureBackBufferIndex = -1;

// Capture can optionally reuse the RTV that overlay rendered to on this frame.
// Leave this off when capture must happen before overlay.
inline thread_local bool dx11_hook_g_CaptureUsesOverlayRTV = false;

// DX11 runtimes can expose D3D10 compatibility interfaces on the same swapchain.
// Prefer the highest actual device API so DX11 swapchains do not fall into the
// DX10 overlay/capture paths just because ID3D10 queries happen to succeed.
inline DXGIShared::APIType DetectSwapChainAPITypeForDX11Hook(IDXGISwapChain* swapChain) {
    if (!swapChain) {
        return DXGIShared::APIType::Unknown;
    }

    bool hasD3D12Device = false;
    bool hasD3D11Device = false;
    bool hasD3D10Device = false;

    ID3D12Device* d3d12Device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D12Device), (void**)&d3d12Device)) && d3d12Device) {
        d3d12Device->Release();
        hasD3D12Device = true;
    }

    // Always try all three — do NOT short-circuit when D3D11 succeeds.
    // On Windows 10+ the D3D10 runtime is implemented on D3D11
    // (D3D10-on-D3D11).  A D3D10 device will QI for BOTH ID3D11Device
    // and ID3D10Device, so checking D3D11 first and skipping D3D10
    // would wrongly classify the swapchain as D3D11.  Let
    // SelectPrimarySwapChainAPIType make the final decision.
    ID3D11Device* d3d11Device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&d3d11Device)) && d3d11Device) {
        d3d11Device->Release();
        hasD3D11Device = true;
    }

    ID3D10Device* d3d10Device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D10Device), (void**)&d3d10Device)) && d3d10Device) {
        d3d10Device->Release();
        hasD3D10Device = true;
    } else {
        ID3D10Device1* d3d10Device1 = nullptr;
        if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D10Device1), (void**)&d3d10Device1)) && d3d10Device1) {
            d3d10Device1->Release();
            hasD3D10Device = true;
        }
    }

    return DXGIShared::SelectPrimarySwapChainAPIType(hasD3D12Device, hasD3D11Device, hasD3D10Device);
}

inline const char* GetDX11HookBaseAPIName(DXGIShared::APIType api) {
    switch (api) {
        case DXGIShared::APIType::D3D10:
            return "DX10";
        case DXGIShared::APIType::D3D11:
            return "DX11";
        case DXGIShared::APIType::D3D12:
            return "DX12";
        default:
            return "Unknown";
    }
}

inline bool IsDeferredAFBootstrapped11(ID3D11DeviceContext* context) {
    if (!context) {
        return false;
    }
    std::lock_guard<std::mutex> lock(dx11_hook_g_DeferredAFBootstrapMutex);
    return dx11_hook_g_DeferredAFBootstrappedContexts.find(reinterpret_cast<uintptr_t>(context)) !=
           dx11_hook_g_DeferredAFBootstrappedContexts.end();
}

inline void MarkDeferredAFBootstrapped11(ID3D11DeviceContext* context) {
    if (!context) {
        return;
    }
    std::lock_guard<std::mutex> lock(dx11_hook_g_DeferredAFBootstrapMutex);
    dx11_hook_g_DeferredAFBootstrappedContexts.insert(reinterpret_cast<uintptr_t>(context));
}

inline void ApplyDX11WaitableFlag(DXGI_SWAP_CHAIN_DESC& desc) {
    desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
}

inline void ApplyDX11WaitableFlag(DXGI_SWAP_CHAIN_DESC1& desc) {
    desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
}

inline bool ApplyDX11BackbufferCountOverride(DXGI_SWAP_CHAIN_DESC& desc, const char* source) {
    const GraphicsConfig& gfx = GetActiveGraphicsConfig();
    if (!HasBackbufferCountOverride(gfx.backbufferCount)) {
        return false;
    }

    const UINT requested = static_cast<UINT>(gfx.backbufferCount);
    const bool isFlip =
        (desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
    if (isFlip)
        ApplyDX11WaitableFlag(desc);
    if (isFlip && requested < desc.BufferCount) {
        ApplyDX11WaitableFlag(desc);
        HookLogImportant("DX11: %s BufferCount override skipped requested=%u game=%u swapEffect=%d (flip model)",
                         source ? source : "CreateSwapChain", requested, desc.BufferCount, desc.SwapEffect);
        return false;
    }
    if (desc.BufferCount != requested) {
        HookLogImportant("DX11: %s BufferCount override %u -> %u swapEffect=%d", source ? source : "CreateSwapChain",
                         desc.BufferCount, requested, desc.SwapEffect);
        desc.BufferCount = requested;
        return true;
    }

    HookLogImportant("DX11: %s BufferCount already matches requested=%u swapEffect=%d",
                     source ? source : "CreateSwapChain", requested, desc.SwapEffect);
    return false;
}

inline bool ApplyDX11BackbufferCountOverride(DXGI_SWAP_CHAIN_DESC1& desc, const char* source) {
    const GraphicsConfig& gfx = GetActiveGraphicsConfig();
    if (!HasBackbufferCountOverride(gfx.backbufferCount)) {
        return false;
    }

    const UINT requested = static_cast<UINT>(gfx.backbufferCount);
    const bool isFlip =
        (desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
    if (isFlip)
        ApplyDX11WaitableFlag(desc);
    if (isFlip && requested < desc.BufferCount) {
        ApplyDX11WaitableFlag(desc);
        HookLogImportant("DX11: %s BufferCount override skipped requested=%u game=%u swapEffect=%d (flip model)",
                         source ? source : "CreateSwapChainForHwnd", requested, desc.BufferCount, desc.SwapEffect);
        return false;
    }
    if (desc.BufferCount != requested) {
        HookLogImportant("DX11: %s BufferCount override %u -> %u swapEffect=%d",
                         source ? source : "CreateSwapChainForHwnd", desc.BufferCount, requested, desc.SwapEffect);
        desc.BufferCount = requested;
        return true;
    }

    HookLogImportant("DX11: %s BufferCount already matches requested=%u swapEffect=%d",
                     source ? source : "CreateSwapChainForHwnd", requested, desc.SwapEffect);
    return false;
}

inline bool IsDXVKD3D10OrD3D11Loaded() {
    return IsDllFromProject("d3d11.dll", "dxvk") || IsDllFromProject("d3d10.dll", "dxvk") ||
           IsDllFromProject("d3d10_1.dll", "dxvk");
}

inline const char* GetDX11HookOverlayAPIName(DXGIShared::APIType api) {
    const bool isDxvk = IsDXVKD3D10OrD3D11Loaded();
    switch (api) {
        case DXGIShared::APIType::D3D10:
            return isDxvk ? "DX10 (DXVK)" : "DX10";
        case DXGIShared::APIType::D3D11:
            return isDxvk ? "DX11 (DXVK)" : "DX11";
        case DXGIShared::APIType::D3D12:
            return "DX12";
        default:
            return "Unknown";
    }
}

inline UINT ResolveDX11BackBufferIndex(IDXGISwapChain* swapChain, const DXGI_SWAP_CHAIN_DESC* swapChainDesc = nullptr) {
    if (!swapChain)
        return 0;

    if (dx11_hook_g_ForcedCaptureBackBufferIndex >= 0)
        return static_cast<UINT>(dx11_hook_g_ForcedCaptureBackBufferIndex);

    DXGI_SWAP_CHAIN_DESC localDesc = {};
    const DXGI_SWAP_CHAIN_DESC* desc = swapChainDesc;
    if (!desc) {
        if (FAILED(swapChain->GetDesc(&localDesc)))
            return 0;
        desc = &localDesc;
    }

    bool isFlipSwapchain =
        (desc->SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD || desc->SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
    if (!isFlipSwapchain)
        return 0;

    IDXGISwapChain3* swapChain3 = nullptr;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))) || !swapChain3) {
        return 0;
    }

    UINT bufferIndex = swapChain3->GetCurrentBackBufferIndex();
    swapChain3->Release();
    return bufferIndex;
}

inline bool IsUnityProcess() {
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

inline Present_t dx11_hook_oPresent = NULL;

inline Present1_t dx11_hook_oPresent1 = NULL;

inline CreateSamplerState_t dx11_hook_oCreateSamplerState = NULL;

inline CreatePixelShader11_t dx11_hook_oCreatePixelShader11 = NULL;

inline CreateDeferredContext11_t dx11_hook_oCreateDeferredContext11 = NULL;

inline PSSetShader11_t dx11_hook_oPSSetShader11 = NULL;

inline DrawIndexed11_t dx11_hook_oDrawIndexed11 = NULL;

inline Draw11_t dx11_hook_oDraw11 = NULL;

inline DrawIndexedInstanced11_t dx11_hook_oDrawIndexedInstanced11 = NULL;

inline DrawInstanced11_t dx11_hook_oDrawInstanced11 = NULL;

inline DrawAuto11_t dx11_hook_oDrawAuto11 = NULL;

inline DrawIndexedInstancedIndirect11_t dx11_hook_oDrawIndexedInstancedIndirect11 = NULL;

inline DrawInstancedIndirect11_t dx11_hook_oDrawInstancedIndirect11 = NULL;

inline ExecuteCommandList11_t dx11_hook_oExecuteCommandList11 = NULL;

inline SetShaderResources11_t dx11_hook_oPSSetShaderResources11 = NULL;

inline SetShaderResources11_t dx11_hook_oVSSetShaderResources11 = NULL;

inline SetShaderResources11_t dx11_hook_oGSSetShaderResources11 = NULL;

inline SetShaderResources11_t dx11_hook_oHSSetShaderResources11 = NULL;

inline SetShaderResources11_t dx11_hook_oDSSetShaderResources11 = NULL;

inline SetShaderResources11_t dx11_hook_oCSSetShaderResources11 = NULL;

inline SetSamplers11_t dx11_hook_oPSSetSamplers11 = NULL;

inline SetSamplers11_t dx11_hook_oVSSetSamplers11 = NULL;

inline SetSamplers11_t dx11_hook_oGSSetSamplers11 = NULL;

inline SetSamplers11_t dx11_hook_oHSSetSamplers11 = NULL;

inline SetSamplers11_t dx11_hook_oDSSetSamplers11 = NULL;

inline SetSamplers11_t dx11_hook_oCSSetSamplers11 = NULL;

struct D3D11ContextVTableOriginals {
    SetShaderResources11_t psSetShaderResources = nullptr;
    PSSetShader11_t psSetShader = nullptr;
    SetSamplers11_t psSetSamplers = nullptr;
    DrawIndexed11_t drawIndexed = nullptr;
    Draw11_t draw = nullptr;
    DrawIndexedInstanced11_t drawIndexedInstanced = nullptr;
    DrawInstanced11_t drawInstanced = nullptr;
    SetShaderResources11_t vsSetShaderResources = nullptr;
    SetSamplers11_t vsSetSamplers = nullptr;
    SetShaderResources11_t gsSetShaderResources = nullptr;
    SetSamplers11_t gsSetSamplers = nullptr;
    DrawAuto11_t drawAuto = nullptr;
    DrawIndexedInstancedIndirect11_t drawIndexedInstancedIndirect = nullptr;
    DrawInstancedIndirect11_t drawInstancedIndirect = nullptr;
    ExecuteCommandList11_t executeCommandList = nullptr;
    SetShaderResources11_t hsSetShaderResources = nullptr;
    SetSamplers11_t hsSetSamplers = nullptr;
    SetShaderResources11_t dsSetShaderResources = nullptr;
    SetSamplers11_t dsSetSamplers = nullptr;
    SetShaderResources11_t csSetShaderResources = nullptr;
    SetSamplers11_t csSetSamplers = nullptr;
};

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
inline std::shared_mutex dx11_hook_g_D3D11ContextVTableOriginalsMutex;

inline std::unordered_map<void*, D3D11ContextVTableOriginals> dx11_hook_g_D3D11ContextVTableOriginals;

inline std::atomic<void*> dx11_hook_g_PrimaryD3D11ContextVTable{nullptr};

inline std::atomic<uint32_t> dx11_hook_g_D3D11ContextVTableOriginalsGeneration{1};

template <typename Fn>

static Fn ResolveContextOriginal11(ID3D11DeviceContext* context, UINT slot, Fn D3D11ContextVTableOriginals::* member,
                                   Fn fallback) {
    if (!context) {
        return fallback;
    }

    void** vtable = *(void***)context;
    void* vtableKey = reinterpret_cast<void*>(vtable);
    if (!vtableKey || vtableKey == dx11_hook_g_PrimaryD3D11ContextVTable.load(std::memory_order_acquire)) {
        return fallback;
    }

    static thread_local void* s_cachedVTable = nullptr;
    static thread_local UINT s_cachedSlot = UINT_MAX;
    static thread_local Fn s_cachedOriginal = nullptr;
    static thread_local uint32_t s_cachedGeneration = 0;
    const uint32_t generation = dx11_hook_g_D3D11ContextVTableOriginalsGeneration.load(std::memory_order_acquire);
    if (s_cachedGeneration == generation && s_cachedVTable == vtableKey && s_cachedSlot == slot && s_cachedOriginal) {
        return s_cachedOriginal;
    }

    std::shared_lock<std::shared_mutex> lock(dx11_hook_g_D3D11ContextVTableOriginalsMutex);
    auto it = dx11_hook_g_D3D11ContextVTableOriginals.find(vtableKey);
    if (it == dx11_hook_g_D3D11ContextVTableOriginals.end()) {
        return fallback;
    }

    Fn original = it->second.*member;
    if (!original) {
        return fallback;
    }

    s_cachedVTable = vtableKey;
    s_cachedSlot = slot;
    s_cachedOriginal = original;
    s_cachedGeneration = generation;
    return original;
}

inline CreateSamplerState10_t dx11_hook_oCreateSamplerState10 = NULL;

struct D3D11SamplerCacheEntry {
    ID3D11Device* device;
    D3D11_SAMPLER_DESC desc;
    ID3D11SamplerState* sampler;
};

inline std::vector<D3D11SamplerCacheEntry> dx11_hook_g_SamplerCache11;

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
inline std::shared_mutex dx11_hook_g_SamplerCacheMutex11;

inline std::vector<ID3D11SamplerState*> dx11_hook_g_ReplacementSamplers11;

inline uint64_t dx11_hook_g_SamplerConfigHash11 = 0;

inline std::atomic<uint64_t> dx11_hook_g_SamplerConfigHash11Fast{0};

inline thread_local bool dx11_hook_g_InOverlayRender = false;

inline bool SameSamplerDesc11(const D3D11_SAMPLER_DESC& a, const D3D11_SAMPLER_DESC& b) {
    return a.Filter == b.Filter && a.AddressU == b.AddressU && a.AddressV == b.AddressV && a.AddressW == b.AddressW &&
           a.MipLODBias == b.MipLODBias && a.MaxAnisotropy == b.MaxAnisotropy && a.ComparisonFunc == b.ComparisonFunc &&
           // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison) - exact bitwise cache identity is intended
           std::memcmp(a.BorderColor, b.BorderColor, sizeof(a.BorderColor)) == 0 && a.MinLOD == b.MinLOD &&
           a.MaxLOD == b.MaxLOD;
}

inline ID3D11SamplerState* FindReplacementSampler11(ID3D11Device* device, const D3D11_SAMPLER_DESC& desc) {
    std::shared_lock<std::shared_mutex> lock(dx11_hook_g_SamplerCacheMutex11);
    for (const auto& entry : dx11_hook_g_SamplerCache11) {
        if (entry.device == device && SameSamplerDesc11(entry.desc, desc)) {
            return entry.sampler;
        }
    }
    return nullptr;
}

inline void AddReplacementSampler11(ID3D11Device* device, const D3D11_SAMPLER_DESC& desc,
                                    ID3D11SamplerState* replacement) {
    std::unique_lock<std::shared_mutex> lock(dx11_hook_g_SamplerCacheMutex11);
    for (auto& entry : dx11_hook_g_SamplerCache11) {
        if (entry.device == device && SameSamplerDesc11(entry.desc, desc)) {
            entry.sampler = replacement;
            return;
        }
    }
    dx11_hook_g_SamplerCache11.push_back({device, desc, replacement});
}

inline bool IsReplacementSampler11(ID3D11SamplerState* sampler) {
    std::shared_lock<std::shared_mutex> lock(dx11_hook_g_SamplerCacheMutex11);
    for (auto* replacement : dx11_hook_g_ReplacementSamplers11) {
        if (replacement == sampler) {
            return true;
        }
    }
    return false;
}

inline void AddToReplacementSet11(ID3D11SamplerState* sampler) {
    std::unique_lock<std::shared_mutex> lock(dx11_hook_g_SamplerCacheMutex11);
    dx11_hook_g_ReplacementSamplers11.push_back(sampler);
}

inline void ClearReplacementSamplerCache11Unlocked() {
    for (auto* sampler : dx11_hook_g_ReplacementSamplers11) {
        if (sampler) {
            sampler->Release();
        }
    }
    dx11_hook_g_ReplacementSamplers11.clear();
    dx11_hook_g_SamplerCache11.clear();
    dx11_hook_g_SamplerConfigHash11 = 0;
    dx11_hook_g_SamplerConfigHash11Fast.store(0, std::memory_order_release);
}

inline void EnsureSamplerCacheFresh11(const GraphicsConfig& gfx) {
    const uint64_t configHash = ce::sampler_override::HashSamplerOverrideConfig(gfx);
    if (dx11_hook_g_SamplerConfigHash11Fast.load(std::memory_order_acquire) == configHash) {
        return;
    }
    std::unique_lock<std::shared_mutex> lock(dx11_hook_g_SamplerCacheMutex11);
    if (dx11_hook_g_SamplerConfigHash11 == configHash) {
        dx11_hook_g_SamplerConfigHash11Fast.store(configHash, std::memory_order_release);
        return;
    }
    ClearReplacementSamplerCache11Unlocked();
    dx11_hook_g_SamplerConfigHash11 = configHash;
    dx11_hook_g_SamplerConfigHash11Fast.store(configHash, std::memory_order_release);
}

struct D3D11StageState {
    ID3D11ShaderResourceView* srvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    ID3D11SamplerState* realSamplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
};

struct D3D11PerContextState {
    D3D11StageState stages[6];
    ID3D11PixelShader* pixelShader = nullptr;
    WrapperPixelShaderAFMetadata pixelShaderMetadata = {};
    bool hasPixelShaderMetadata = false;
    uint32_t pixelSamplerDirtyMask = 0;
};

inline std::mutex dx11_hook_g_D3D11ContextStateMutex;

inline std::unordered_map<ID3D11DeviceContext*, D3D11PerContextState> dx11_hook_g_D3D11ContextStates;

inline std::atomic<uint32_t> dx11_hook_g_D3D11DirtyContextCount{0};

// The raw-vtable fallback is normally bypassed by the context wrapper. For
// callers that retain a real context pointer, let clean draws test one atomic
// and return without taking the process-global state mutex.
inline void MarkPixelSamplersDirty11Locked(D3D11PerContextState& state, uint32_t mask) {
    if (mask == 0) {
        return;
    }
    const uint32_t previous = state.pixelSamplerDirtyMask;
    state.pixelSamplerDirtyMask |= mask;
    if (previous == 0) {
        dx11_hook_g_D3D11DirtyContextCount.fetch_add(1, std::memory_order_release);
    }
}

inline size_t GetStageIndex(D3D11ShaderStage stage) {
    switch (stage) {
        case D3D11ShaderStage::Pixel:
            return 0;
        case D3D11ShaderStage::Vertex:
            return 1;
        case D3D11ShaderStage::Geometry:
            return 2;
        case D3D11ShaderStage::Hull:
            return 3;
        case D3D11ShaderStage::Domain:
            return 4;
        case D3D11ShaderStage::Compute:
        default:
            return 5;
    }
}

inline const char* GetStageName11(D3D11ShaderStage stage) {
    switch (stage) {
        case D3D11ShaderStage::Pixel:
            return "PS";
        case D3D11ShaderStage::Vertex:
            return "VS";
        case D3D11ShaderStage::Geometry:
            return "GS";
        case D3D11ShaderStage::Hull:
            return "HS";
        case D3D11ShaderStage::Domain:
            return "DS";
        case D3D11ShaderStage::Compute:
        default:
            return "CS";
    }
}

inline uint32_t SamplerRangeMask11(UINT startSlot, UINT numSamplers) {
    if (startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT || numSamplers == 0) {
        return 0;
    }
    const UINT maxSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const UINT actualSamplers = (numSamplers < maxSamplers) ? numSamplers : maxSamplers;
    uint32_t mask = 0;
    for (UINT i = 0; i < actualSamplers; ++i) {
        mask |= (1u << (startSlot + i));
    }
    return mask;
}

inline uint32_t TrackedPixelSamplerMask11Locked(const D3D11PerContextState& state) {
    uint32_t mask = 0;
    const D3D11StageState& pixelStage = state.stages[GetStageIndex(D3D11ShaderStage::Pixel)];
    for (UINT slot = 0; slot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++slot) {
        if (pixelStage.samplers[slot]) {
            mask |= (1u << slot);
        }
    }
    return mask;
}

inline uint32_t PixelSamplerDirtyMaskForResourceRange11Locked(const D3D11PerContextState& state, UINT startSlot,
                                                              UINT numViews) {
    if (startSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT || numViews == 0) {
        return 0;
    }

    if (!state.hasPixelShaderMetadata || !state.pixelShaderMetadata.available) {
        return TrackedPixelSamplerMask11Locked(state);
    }

    const UINT maxViews = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - startSlot;
    const UINT actualViews = (numViews < maxViews) ? numViews : maxViews;
    uint32_t mask = 0;
    for (UINT i = 0; i < actualViews; ++i) {
        mask |=
            ce::sampler_override::D3D11ShaderSamplerMaskForTextureSlot(state.pixelShaderMetadata.usage, startSlot + i);
    }
    return mask & TrackedPixelSamplerMask11Locked(state);
}

inline void GetStageShaderResources11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot,
                                      UINT numViews, ID3D11ShaderResourceView** views) {
    if (!context || !views || numViews == 0) {
        return;
    }
    switch (stage) {
        case D3D11ShaderStage::Pixel:
            context->PSGetShaderResources(startSlot, numViews, views);
            break;
        case D3D11ShaderStage::Vertex:
            context->VSGetShaderResources(startSlot, numViews, views);
            break;
        case D3D11ShaderStage::Geometry:
            context->GSGetShaderResources(startSlot, numViews, views);
            break;
        case D3D11ShaderStage::Hull:
            context->HSGetShaderResources(startSlot, numViews, views);
            break;
        case D3D11ShaderStage::Domain:
            context->DSGetShaderResources(startSlot, numViews, views);
            break;
        case D3D11ShaderStage::Compute:
            context->CSGetShaderResources(startSlot, numViews, views);
            break;
    }
}

inline void GetStageSamplers11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot, UINT numSamplers,
                               ID3D11SamplerState** samplers) {
    if (!context || !samplers || numSamplers == 0) {
        return;
    }
    switch (stage) {
        case D3D11ShaderStage::Pixel:
            context->PSGetSamplers(startSlot, numSamplers, samplers);
            break;
        case D3D11ShaderStage::Vertex:
            context->VSGetSamplers(startSlot, numSamplers, samplers);
            break;
        case D3D11ShaderStage::Geometry:
            context->GSGetSamplers(startSlot, numSamplers, samplers);
            break;
        case D3D11ShaderStage::Hull:
            context->HSGetSamplers(startSlot, numSamplers, samplers);
            break;
        case D3D11ShaderStage::Domain:
            context->DSGetSamplers(startSlot, numSamplers, samplers);
            break;
        case D3D11ShaderStage::Compute:
            context->CSGetSamplers(startSlot, numSamplers, samplers);
            break;
    }
}

inline void ReleaseTrackedContextState11(D3D11PerContextState& state) {
    for (D3D11StageState& stageState : state.stages) {
        for (ID3D11ShaderResourceView*& view : stageState.srvs) {
            if (view) {
                view->Release();
                view = nullptr;
            }
        }
        for (ID3D11SamplerState*& sampler : stageState.samplers) {
            if (sampler) {
                sampler->Release();
                sampler = nullptr;
            }
        }
        for (ID3D11SamplerState*& sampler : stageState.realSamplers) {
            if (sampler) {
                sampler->Release();
                sampler = nullptr;
            }
        }
    }
    if (state.pixelShader) {
        state.pixelShader->Release();
        state.pixelShader = nullptr;
    }
}

inline void ReleaseTrackedShaderResources11Unlocked() {
    // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order) - resource release order is irrelevant
    for (auto& [context, state] : dx11_hook_g_D3D11ContextStates) {
        (void)context;
        ReleaseTrackedContextState11(state);
    }
    dx11_hook_g_D3D11ContextStates.clear();
    dx11_hook_g_D3D11DirtyContextCount.store(0, std::memory_order_release);
}

inline void ClearTrackedContextState11(ID3D11DeviceContext* context) {
    if (!context) {
        return;
    }
    D3D11PerContextState retiredState = {};
    {
        std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
        auto it = dx11_hook_g_D3D11ContextStates.find(context);
        if (it == dx11_hook_g_D3D11ContextStates.end()) {
            return;
        }
        if (it->second.pixelSamplerDirtyMask != 0) {
            dx11_hook_g_D3D11DirtyContextCount.fetch_sub(1, std::memory_order_release);
        }
        retiredState = it->second;
        dx11_hook_g_D3D11ContextStates.erase(it);
    }
    // Driver-owned COM destruction must not run under the tracking mutex; a
    // release can execute arbitrary runtime code and must be safe to re-enter.
    ReleaseTrackedContextState11(retiredState);
}

inline void ReleaseTrackedShaderResources11() {
    {
        std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
        ReleaseTrackedShaderResources11Unlocked();
    }
}

inline void UpdateStageShaderResources(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot,
                                       UINT numViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    if (!context) {
        return;
    }
    if (startSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        return;
    }

    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    D3D11PerContextState& contextState = dx11_hook_g_D3D11ContextStates[context];
    D3D11StageState& state = contextState.stages[GetStageIndex(stage)];
    const UINT maxViews = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - startSlot;
    const UINT actualViews = (numViews < maxViews) ? numViews : maxViews;
    bool changed = false;

    for (UINT i = 0; i < actualViews; ++i) {
        const UINT slot = startSlot + i;
        ID3D11ShaderResourceView* view = ppShaderResourceViews ? ppShaderResourceViews[i] : nullptr;
        if (state.srvs[slot] == view) {
            continue;
        }
        changed = true;

        if (state.srvs[slot]) {
            state.srvs[slot]->Release();
            state.srvs[slot] = nullptr;
        }

        if (view) {
            view->AddRef();
        }
        state.srvs[slot] = view;
    }
    if (stage == D3D11ShaderStage::Pixel && changed) {
        MarkPixelSamplersDirty11Locked(
            contextState, PixelSamplerDirtyMaskForResourceRange11Locked(contextState, startSlot, actualViews));
    }
}

inline uint32_t UpdateStageSamplers(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot,
                                    UINT numSamplers, ID3D11SamplerState* const* ppSamplers) {
    if (!context) {
        return 0;
    }
    if (startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    D3D11PerContextState& contextState = dx11_hook_g_D3D11ContextStates[context];
    D3D11StageState& state = contextState.stages[GetStageIndex(stage)];
    const UINT maxSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const UINT actualSamplers = (numSamplers < maxSamplers) ? numSamplers : maxSamplers;
    uint32_t dirtyMask = 0;

    for (UINT i = 0; i < actualSamplers; ++i) {
        const UINT slot = startSlot + i;
        ID3D11SamplerState* sampler = ppSamplers ? ppSamplers[i] : nullptr;
        if (state.samplers[slot] == sampler) {
            continue;
        }
        if (stage == D3D11ShaderStage::Pixel) {
            dirtyMask |= (1u << slot);
        }

        if (state.samplers[slot]) {
            state.samplers[slot]->Release();
            state.samplers[slot] = nullptr;
        }

        if (sampler) {
            sampler->AddRef();
        }
        state.samplers[slot] = sampler;
    }
    MarkPixelSamplersDirty11Locked(contextState, dirtyMask);
    return dirtyMask;
}

inline void RememberRealSampler11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT slot,
                                  ID3D11SamplerState* sampler) {
    if (!context || slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return;
    }

    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    D3D11StageState& stageState = dx11_hook_g_D3D11ContextStates[context].stages[GetStageIndex(stage)];
    ID3D11SamplerState*& tracked = stageState.realSamplers[slot];
    if (tracked == sampler) {
        return;
    }
    if (sampler) {
        sampler->AddRef();
    }
    if (tracked) {
        tracked->Release();
    }
    tracked = sampler;
}

inline void RememberRealSamplerRange11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot,
                                       UINT numSamplers, ID3D11SamplerState* const* ppSamplers) {
    if (!context || startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return;
    }
    const UINT maxSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const UINT actualSamplers = (numSamplers < maxSamplers) ? numSamplers : maxSamplers;
    for (UINT i = 0; i < actualSamplers; ++i) {
        RememberRealSampler11(context, stage, startSlot + i, ppSamplers ? ppSamplers[i] : nullptr);
    }
}

inline ID3D11SamplerState* GetRememberedRealSampler11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT slot) {
    if (!context || slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    auto it = dx11_hook_g_D3D11ContextStates.find(context);
    if (it == dx11_hook_g_D3D11ContextStates.end()) {
        return nullptr;
    }
    ID3D11SamplerState* sampler = it->second.stages[GetStageIndex(stage)].realSamplers[slot];
    if (sampler) {
        sampler->AddRef();
    }
    return sampler;
}

inline uint32_t PeekPixelSamplerDirtyMask11(ID3D11DeviceContext* context, uint32_t slotMask) {
    if (!context || slotMask == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    auto it = dx11_hook_g_D3D11ContextStates.find(context);
    if (it == dx11_hook_g_D3D11ContextStates.end()) {
        return 0;
    }
    return it->second.pixelSamplerDirtyMask & slotMask;
}

inline void ClearPixelSamplerDirtyMask11(ID3D11DeviceContext* context, uint32_t slotMask) {
    if (!context || slotMask == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    auto it = dx11_hook_g_D3D11ContextStates.find(context);
    if (it != dx11_hook_g_D3D11ContextStates.end()) {
        const uint32_t previous = it->second.pixelSamplerDirtyMask;
        it->second.pixelSamplerDirtyMask &= ~slotMask;
        if (previous != 0 && it->second.pixelSamplerDirtyMask == 0) {
            dx11_hook_g_D3D11DirtyContextCount.fetch_sub(1, std::memory_order_release);
        }
    }
}

inline ID3D11ShaderResourceView* GetTrackedShaderResourceView11(ID3D11DeviceContext* context, D3D11ShaderStage stage,
                                                                UINT slot) {
    if (!context || slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    auto it = dx11_hook_g_D3D11ContextStates.find(context);
    if (it == dx11_hook_g_D3D11ContextStates.end()) {
        return nullptr;
    }

    ID3D11ShaderResourceView* view = it->second.stages[GetStageIndex(stage)].srvs[slot];
    if (view) {
        view->AddRef();
    }
    return view;
}

inline ID3D11SamplerState* GetTrackedSampler11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT slot) {
    if (!context || slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    auto it = dx11_hook_g_D3D11ContextStates.find(context);
    if (it == dx11_hook_g_D3D11ContextStates.end()) {
        return nullptr;
    }

    ID3D11SamplerState* sampler = it->second.stages[GetStageIndex(stage)].samplers[slot];
    if (sampler) {
        sampler->AddRef();
    }
    return sampler;
}

inline void UpdateTrackedPixelShader11(ID3D11DeviceContext* context, ID3D11PixelShader* shader) {
    if (!context) {
        return;
    }

    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    D3D11PerContextState& contextState = dx11_hook_g_D3D11ContextStates[context];
    if (contextState.pixelShader == shader) {
        return;
    }
    MarkPixelSamplersDirty11Locked(contextState, TrackedPixelSamplerMask11Locked(contextState));
    if (shader) {
        shader->AddRef();
    }
    if (contextState.pixelShader) {
        contextState.pixelShader->Release();
    }
    contextState.pixelShader = shader;
    contextState.pixelShaderMetadata = {};
    contextState.hasPixelShaderMetadata =
        GetWrapperPixelShaderAFMetadata(contextState.pixelShader, &contextState.pixelShaderMetadata);
}

inline uint32_t ConsumePixelSamplerDirtyMask11(ID3D11DeviceContext* context) {
    if (!context) {
        return 0;
    }
    if (dx11_hook_g_D3D11DirtyContextCount.load(std::memory_order_acquire) == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    auto it = dx11_hook_g_D3D11ContextStates.find(context);
    if (it == dx11_hook_g_D3D11ContextStates.end()) {
        return 0;
    }
    const uint32_t mask = it->second.pixelSamplerDirtyMask;
    it->second.pixelSamplerDirtyMask = 0;
    if (mask != 0) {
        dx11_hook_g_D3D11DirtyContextCount.fetch_sub(1, std::memory_order_release);
    }
    return mask;
}

inline bool GetTrackedPixelShaderMetadata11(ID3D11DeviceContext* context, bool* hasShader,
                                            WrapperPixelShaderAFMetadata* metadata) {
    if (hasShader) {

        *hasShader = false;
    }
    if (!context || !metadata) {
        return false;
    }
    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    auto it = dx11_hook_g_D3D11ContextStates.find(context);
    if (it == dx11_hook_g_D3D11ContextStates.end()) {
        return false;
    }
    if (hasShader) {
        *hasShader = it->second.pixelShader != nullptr;
    }
    if (!it->second.hasPixelShaderMetadata) {
        return false;
    }
    *metadata = it->second.pixelShaderMetadata;
    return true;
}

inline void RefreshPixelShaderFromContext11(ID3D11DeviceContext* context) {
    if (!context) {
        return;
    }

    ID3D11PixelShader* shader = nullptr;
    UINT classInstanceCount = 0;
    context->PSGetShader(&shader, nullptr, &classInstanceCount);
    UpdateTrackedPixelShader11(context, shader);
    if (shader) {
        shader->Release();
    }
}

inline void RefreshStageShaderResourcesFromContext11(ID3D11DeviceContext* context, D3D11ShaderStage stage,
                                                     UINT startSlot, UINT numViews) {
    if (!context || startSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT || numViews == 0) {
        return;
    }
    const UINT maxViews = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - startSlot;
    const UINT actualViews = (numViews < maxViews) ? numViews : maxViews;
    ID3D11ShaderResourceView* views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    GetStageShaderResources11(context, stage, startSlot, actualViews, views);
    UpdateStageShaderResources(context, stage, startSlot, actualViews, views);
    for (UINT i = 0; i < actualViews; ++i) {
        if (views[i]) {
            views[i]->Release();
        }
    }
}

inline void RefreshStageSamplersFromContext11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot,
                                              UINT numSamplers) {
    if (!context || startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT || numSamplers == 0) {
        return;
    }
    const UINT maxSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const UINT actualSamplers = (numSamplers < maxSamplers) ? numSamplers : maxSamplers;
    ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    ID3D11SamplerState* logicalSamplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    GetStageSamplers11(context, stage, startSlot, actualSamplers, samplers);
    for (UINT i = 0; i < actualSamplers; ++i) {
        logicalSamplers[i] = samplers[i];
        if (samplers[i] && IsReplacementSampler11(samplers[i])) {
            ID3D11SamplerState* trackedOriginal = GetTrackedSampler11(context, stage, startSlot + i);
            if (trackedOriginal) {
                logicalSamplers[i] = trackedOriginal;
            }
        }
    }
    UpdateStageSamplers(context, stage, startSlot, actualSamplers, logicalSamplers);
    RememberRealSamplerRange11(context, stage, startSlot, actualSamplers, samplers);
    for (UINT i = 0; i < actualSamplers; ++i) {
        if (logicalSamplers[i] && logicalSamplers[i] != samplers[i]) {
            logicalSamplers[i]->Release();
        }
        if (samplers[i]) {
            samplers[i]->Release();
        }
    }
}

inline ce::sampler_override::D3D11ForcedAFResourceDecision ClassifyViewForForcedAF11(
    ID3D11Device* device, ID3D11ShaderResourceView* view,
    ce::sampler_override::D3D11Texture2DForcedAFInfo* outInfo = nullptr) {
    (void)device;
    return GetWrapperForcedAFViewMetadata(view, outInfo);
}

inline bool SamplerAllowsForcedAF(const D3D11_SAMPLER_DESC& desc, const GraphicsConfig& gfx) {
    using ce::sampler_override::D3D11ForcedAFSamplerDecision;
    const D3D11ForcedAFSamplerDecision decision = ce::sampler_override::ClassifyD3D11SamplerForForcedAF(desc, gfx);
    switch (decision) {
        case D3D11ForcedAFSamplerDecision::Allow:
            return true;
        case D3D11ForcedAFSamplerDecision::OverrideDisabled:
            return false;
        case D3D11ForcedAFSamplerDecision::FixedLOD: {
            int idx = dx11_hook_g_DiagSamplerSkipNoMips.fetch_add(1, std::memory_order_relaxed);
            if (idx < 12) {
                HookLogImportant("DX11: AF skip sampler (fixed/no mips) Filter=0x%X MaxLOD=%.1f MinLOD=%.1f",
                                 desc.Filter, desc.MaxLOD, desc.MinLOD);
            }
            return false;
        }
        case D3D11ForcedAFSamplerDecision::BorderAddress: {
            int idx = dx11_hook_g_DiagSamplerSkipBorder.fetch_add(1, std::memory_order_relaxed);
            if (idx < 12) {
                HookLogImportant("DX11: AF skip sampler (border address) Filter=0x%X U=%d V=%d W=%d", desc.Filter,
                                 desc.AddressU, desc.AddressV, desc.AddressW);
            }
            return false;
        }
        case D3D11ForcedAFSamplerDecision::ReductionFilter: {
            int idx = dx11_hook_g_DiagSamplerSkipReduction.fetch_add(1, std::memory_order_relaxed);
            if (idx < 6) {
                HookLogImportant("DX11: AF skip sampler (reduction filter) Filter=0x%X", desc.Filter);
            }
            return false;
        }
        case D3D11ForcedAFSamplerDecision::ComparisonFilter: {
            int idx = dx11_hook_g_DiagSamplerSkipComparison.fetch_add(1, std::memory_order_relaxed);
            if (idx < 12) {
                HookLogImportant("DX11: AF skip sampler (comparison filter) Filter=0x%X Func=%d Addr=%d/%d/%d",
                                 desc.Filter, desc.ComparisonFunc, desc.AddressU, desc.AddressV, desc.AddressW);
            }
            return false;
        }
        case D3D11ForcedAFSamplerDecision::PointMinMag:
            return false;
    }
    return false;
}

inline bool ShouldForceAnisotropyForStageSlot(ID3D11Device* device, ID3D11DeviceContext* context,
                                              D3D11ShaderStage stage, UINT slot, const D3D11_SAMPLER_DESC& desc,
                                              const GraphicsConfig& gfx) {
    if (!SamplerAllowsForcedAF(desc, gfx)) {
        return false;
    }
    if (stage != D3D11ShaderStage::Pixel) {
        int idx = dx11_hook_g_DiagSamplerSkipStage.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            HookLogImportant("DX11: AF skip sampler (non-pixel stage=%s slot=%u)", GetStageName11(stage), slot);
        }
        return false;
    }
    if (slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return false;
    }

    WrapperPixelShaderAFMetadata metadata = {};
    bool hasShader = false;
    bool hasMetadata = GetTrackedPixelShaderMetadata11(context, &hasShader, &metadata);
    if (!hasShader) {
        RefreshPixelShaderFromContext11(context);
        hasMetadata = GetTrackedPixelShaderMetadata11(context, &hasShader, &metadata);
    }
    if (!hasShader) {
        int idx = dx11_hook_g_DiagSamplerSkipNoShader.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            HookLogImportant("DX11: AF skip sampler (no active pixel shader, slot=%u)", slot);
        }
        return false;
    }

    if (!hasMetadata || !metadata.available) {
        int idx = dx11_hook_g_DiagSamplerSkipNoShaderMetadata.fetch_add(1, std::memory_order_relaxed);
        if (idx < 24) {
            HookLogImportant("DX11: AF skip sampler (no pixel-shader sample metadata, slot=%u has=%d failed=%d)", slot,
                             hasMetadata ? 1 : 0, metadata.disassembleFailed ? 1 : 0);
        }
        return false;
    }

    if (!ce::sampler_override::D3D11ShaderSamplerUsesAnyTexture(metadata.usage, slot)) {
        int idx = dx11_hook_g_DiagSamplerSkipShaderUnused.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            HookLogImportant("DX11: AF skip sampler (pixel shader does not sample with s%u)", slot);
        }
        return false;
    }
    if (!ce::sampler_override::D3D11ShaderSamplerUsesAFSafeSample(metadata.usage, slot)) {
        int idx = dx11_hook_g_DiagSamplerSkipExplicitSample.fetch_add(1, std::memory_order_relaxed);
        if (idx < 24) {
            HookLogImportant(
                "DX11: AF skip sampler (pixel shader uses non-implicit sample opcode with s%u implicit=%d bias=%d "
                "lod=%d grad=%d comp=%d other=%d explicit=%d)",
                slot, metadata.usage.samplerUsesImplicitSample[slot] ? 1 : 0,
                ce::sampler_override::D3D11ShaderSamplerUsesBiasSample(metadata.usage, slot) ? 1 : 0,
                ce::sampler_override::D3D11ShaderSamplerUsesLodSample(metadata.usage, slot) ? 1 : 0,
                ce::sampler_override::D3D11ShaderSamplerUsesGradientSample(metadata.usage, slot) ? 1 : 0,
                ce::sampler_override::D3D11ShaderSamplerUsesComparisonSample(metadata.usage, slot) ? 1 : 0,
                ce::sampler_override::D3D11ShaderSamplerUsesOtherExplicitSample(metadata.usage, slot) ? 1 : 0,
                ce::sampler_override::D3D11ShaderSamplerUsesExplicitSample(metadata.usage, slot) ? 1 : 0);
        }
        return false;
    }

    UINT firstTextureSlot = UINT_MAX;
    UINT lastTextureSlot = UINT_MAX;
    const UINT textureCount = ce::sampler_override::CountD3D11ShaderSamplerTextureUses(
        metadata.usage, slot, &firstTextureSlot, &lastTextureSlot);
    D3D11_SHADER_RESOURCE_VIEW_DESC firstSrvDesc = {};
    ce::sampler_override::D3D11Texture2DForcedAFInfo firstResourceInfo = {};
    bool capturedFirstResource = false;

    for (UINT textureSlot = 0; textureSlot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++textureSlot) {
        if (!ce::sampler_override::D3D11ShaderSamplerUsesTexture(metadata.usage, slot, textureSlot)) {
            continue;
        }

        ID3D11ShaderResourceView* view = GetTrackedShaderResourceView11(context, stage, textureSlot);
        if (!view) {
            RefreshStageShaderResourcesFromContext11(context, stage, textureSlot, 1);
            view = GetTrackedShaderResourceView11(context, stage, textureSlot);
        }
        if (!view) {
            int idx = dx11_hook_g_DiagSamplerSkipNoSRV.fetch_add(1, std::memory_order_relaxed);
            if (idx < 24) {
                HookLogImportant("DX11: AF skip sampler (shader samples missing SRV s%u->t%u, sampledTextures=%u)",
                                 slot, textureSlot, textureCount);
            }
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        view->GetDesc(&srvDesc);
        ce::sampler_override::D3D11Texture2DForcedAFInfo resourceInfo = {};
        const auto resourceDecision = ClassifyViewForForcedAF11(device, view, &resourceInfo);
        if (resourceDecision != ce::sampler_override::D3D11ForcedAFResourceDecision::Allow) {
            std::atomic<int>* counter = &dx11_hook_g_DiagSamplerSkipUnsafeResource;
            const char* reason = ce::sampler_override::D3D11ForcedAFResourceDecisionName(resourceDecision);
            if (resourceDecision == ce::sampler_override::D3D11ForcedAFResourceDecision::UnsupportedFormat) {
                counter = &dx11_hook_g_DiagSamplerSkipFormat;
            } else if (resourceDecision == ce::sampler_override::D3D11ForcedAFResourceDecision::SingleVisibleMip) {
                counter = &dx11_hook_g_DiagSamplerSkipSingleMip;
            } else if (resourceDecision == ce::sampler_override::D3D11ForcedAFResourceDecision::NonColorFormat) {
                counter = &dx11_hook_g_DiagSamplerSkipNonColorResource;
            }
            int idx = counter->fetch_add(1, std::memory_order_relaxed);
            if (idx < 24) {
                HookLogImportant(
                    "DX11: AF skip sampler (%s, decision=%d srvFmt=%d sampleFmt=%d texFmt=%d dim=%d size=%ux%u mips=%u "
                    "viewMip=%u mostMip=%u array=%u samples=%u bind=0x%X misc=0x%X sampler=s%u texture=t%u "
                    "sampledTextures=%u)",
                    reason, (int)resourceDecision, srvDesc.Format, resourceInfo.format, resourceInfo.textureFormat,
                    resourceInfo.viewDimension, resourceInfo.width, resourceInfo.height, resourceInfo.mipLevels,
                    resourceInfo.viewMipLevels, resourceInfo.mostDetailedMip, resourceInfo.arraySize,
                    resourceInfo.sampleCount, resourceInfo.bindFlags, resourceInfo.miscFlags, slot, textureSlot,
                    textureCount);
            }
            view->Release();
            return false;
        }

        if (!capturedFirstResource) {
            firstSrvDesc = srvDesc;
            firstResourceInfo = resourceInfo;
            capturedFirstResource = true;
        }
        view->Release();
    }

    int idx = dx11_hook_g_DiagSamplerAllowsAF.fetch_add(1, std::memory_order_relaxed);
    if (ce::sampler_override::D3D11ShaderSamplerUsesLodSample(metadata.usage, slot)) {
        dx11_hook_g_DiagSamplerAllowLodSample.fetch_add(1, std::memory_order_relaxed);
    }
    if (idx < 48) {
        HookLogImportant(
            "DX11: AF allow shader-paired sampler slot=s%u sampledTextures=%u first=t%u last=t%u "
            "Filter=0x%X Aniso=%u Addr=%d/%d/%d sampleKinds(implicit=%d bias=%d lod=%d) "
            "srvFmt=%d sampleFmt=%d texFmt=%d dim=%d "
            "size=%ux%u mips=%u viewMip=%u "
            "mostMip=%u array=%u samples=%u bind=0x%X misc=0x%X (#%d)",
            slot, textureCount, firstTextureSlot, lastTextureSlot, desc.Filter, desc.MaxAnisotropy, desc.AddressU,
            desc.AddressV, desc.AddressW, metadata.usage.samplerUsesImplicitSample[slot] ? 1 : 0,
            ce::sampler_override::D3D11ShaderSamplerUsesBiasSample(metadata.usage, slot) ? 1 : 0,
            ce::sampler_override::D3D11ShaderSamplerUsesLodSample(metadata.usage, slot) ? 1 : 0, firstSrvDesc.Format,
            firstResourceInfo.format, firstResourceInfo.textureFormat, firstResourceInfo.viewDimension,
            firstResourceInfo.width, firstResourceInfo.height, firstResourceInfo.mipLevels,
            firstResourceInfo.viewMipLevels, firstResourceInfo.mostDetailedMip, firstResourceInfo.arraySize,
            firstResourceInfo.sampleCount, firstResourceInfo.bindFlags, firstResourceInfo.miscFlags, idx + 1);
    }
    return true;
}

inline ID3D11SamplerState* GetOrCreateReplacementSampler11(ID3D11DeviceContext* context, D3D11ShaderStage stage,
                                                           UINT slot, ID3D11SamplerState* original) {
    if (!context || !original) {
        return original;
    }

    if (IsReplacementSampler11(original)) {
        return original;
    }

    const auto& gfx = GetActiveGraphicsConfigCached();
    EnsureSamplerCacheFresh11(gfx);

    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    if (!device) {
        return original;
    }

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D11_SAMPLER_DESC desc = {};
    original->GetDesc(&desc);

    const bool allowAnisotropicOverride = ShouldForceAnisotropyForStageSlot(device, context, stage, slot, desc, gfx);
    const bool modified = DX11Hook_ApplySamplerOverrides(desc, gfx, allowAnisotropicOverride);
    if (!modified) {
        device->Release();
        return original;
    }

    if (ID3D11SamplerState* cached = FindReplacementSampler11(device, desc)) {
        device->Release();
        return cached;
    }

    ID3D11SamplerState* replacement = nullptr;
    const HRESULT hr = dx11_hook_oCreateSamplerState ? dx11_hook_oCreateSamplerState(device, &desc, &replacement)
                                           : device->CreateSamplerState(&desc, &replacement);

    if (FAILED(hr) || !replacement) {
        device->Release();
        int idx = dx11_hook_g_DiagSamplerReplacementCreated.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            HookLogImportant("DX11: Replacement sampler creation FAILED hr=0x%08X (stage=%d slot=%u)", hr, (int)stage,
                             slot);
        }
        return original;
    }

    AddReplacementSampler11(device, desc, replacement);
    AddToReplacementSet11(replacement);
    device->Release();
    int idx = dx11_hook_g_DiagSamplerReplacementCreated.fetch_add(1, std::memory_order_relaxed);
    if (idx < 48) {
        HookLog("DX11: Created replacement sampler (stage=%d slot=%u Filter=0x%X Aniso=%u Bias=%.2f) #%d", (int)stage,
                slot, desc.Filter, desc.MaxAnisotropy, desc.MipLODBias, idx + 1);
    }
    return replacement;
}

inline int ReconcileStageSamplers11(SetSamplers11_t originalFn, ID3D11DeviceContext* context, D3D11ShaderStage stage,
                                    UINT startSlot, UINT numSlots, uint32_t slotMask) {
    if (!originalFn || !context || !g_GraphicsOverridesActive.load(std::memory_order_acquire) || dx11_hook_g_InOverlayRender) {
        return 0;
    }
    if (startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT || numSlots == 0 || slotMask == 0) {
        return 0;
    }

    const UINT maxSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const UINT actualSlots = (numSlots < maxSlots) ? numSlots : maxSlots;
    int rebound = 0;
    int visitedSlots = 0;
    for (UINT i = 0; i < actualSlots; ++i) {
        const UINT slot = startSlot + i;
        if ((slotMask & (1u << slot)) == 0) {
            continue;
        }
        ++visitedSlots;
        ID3D11SamplerState* logicalSampler = GetTrackedSampler11(context, stage, slot);
        if (!logicalSampler) {
            continue;
        }

        ID3D11SamplerState* desiredSampler = GetOrCreateReplacementSampler11(context, stage, slot, logicalSampler);
        ID3D11SamplerState* rememberedSampler = GetRememberedRealSampler11(context, stage, slot);
        if (rememberedSampler != desiredSampler) {
            originalFn(context, slot, 1, &desiredSampler);
            RememberRealSampler11(context, stage, slot, desiredSampler);
            ++rebound;
            int idx = dx11_hook_g_DiagSamplerRebound.fetch_add(1, std::memory_order_relaxed);
            if (idx < 48) {
                // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
                D3D11_SAMPLER_DESC desc = {};
                desiredSampler->GetDesc(&desc);
                HookLogImportant("DX11: AF reconciled sampler stage=%s slot=%u Filter=0x%X Aniso=%u Bias=%.2f (#%d)",
                                 GetStageName11(stage), slot, desc.Filter, desc.MaxAnisotropy, desc.MipLODBias,
                                 idx + 1);
            }
        }
        if (rememberedSampler) {
            rememberedSampler->Release();
        }
        logicalSampler->Release();
    }
    if (visitedSlots != 0) {
        dx11_hook_g_DiagSamplerReconcileSlots.fetch_add(visitedSlots, std::memory_order_relaxed);
    }
    return rebound;
}

inline void SetSamplersWithOverrides11(SetSamplers11_t originalFn, ID3D11DeviceContext* context, D3D11ShaderStage stage,
                                       UINT startSlot, UINT numSamplers, ID3D11SamplerState* const* ppSamplers) {
    if (!originalFn) {
        return;
    }
    if (DX11Hook_IsWrapperContextForwarding()) {
        originalFn(context, startSlot, numSamplers, ppSamplers);
        return;
    }
    if (numSamplers == 0 || startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT ||
        !g_GraphicsOverridesActive.load(std::memory_order_acquire) || dx11_hook_g_InOverlayRender) {
        originalFn(context, startSlot, numSamplers, ppSamplers);
        RememberRealSamplerRange11(context, stage, startSlot, numSamplers, ppSamplers);
        return;
    }

    const UINT maxSamplers = static_cast<UINT>(D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT);
    const UINT actualNum = (numSamplers < (maxSamplers - startSlot)) ? numSamplers : (maxSamplers - startSlot);
    const uint32_t changedMask = UpdateStageSamplers(context, stage, startSlot, actualNum, ppSamplers);
    int idx = dx11_hook_g_DiagSamplerBindDeferred.fetch_add(1, std::memory_order_relaxed);
    if (idx < 24) {
        HookLog("DX11: AF sampler bind tracked stage=%s start=%u num=%u changedMask=0x%04X (#%d)",
                GetStageName11(stage), startSlot, numSamplers, changedMask, idx + 1);
    }

    if (stage != D3D11ShaderStage::Pixel) {
        originalFn(context, startSlot, numSamplers, ppSamplers);
        RememberRealSamplerRange11(context, stage, startSlot, numSamplers, ppSamplers);
        return;
    }

    const uint32_t rangeMask = SamplerRangeMask11(startSlot, actualNum);
    const uint32_t dirtyMask = PeekPixelSamplerDirtyMask11(context, rangeMask);
    if (dirtyMask == 0) {
        int skipIdx = dx11_hook_g_DiagSamplerEffectiveBindSkips.fetch_add(1, std::memory_order_relaxed);
        if (skipIdx < 24) {
            HookLog("DX11: AF sampler bind skipped stage=PS start=%u num=%u rangeMask=0x%04X (#%d)", startSlot,
                    numSamplers, rangeMask, skipIdx + 1);
        }
        return;
    }

    int rebound = 0;
    int resolved = 0;
    for (UINT i = 0; i < actualNum; ++i) {
        const UINT slot = startSlot + i;
        const uint32_t bit = (1u << slot);
        if ((dirtyMask & bit) == 0) {
            continue;
        }
        ++resolved;
        ID3D11SamplerState* logicalSampler = GetTrackedSampler11(context, stage, slot);
        ID3D11SamplerState* desiredSampler =
            logicalSampler ? GetOrCreateReplacementSampler11(context, stage, slot, logicalSampler) : nullptr;
        ID3D11SamplerState* rememberedSampler = GetRememberedRealSampler11(context, stage, slot);
        if (rememberedSampler != desiredSampler) {
            originalFn(context, slot, 1, &desiredSampler);
            RememberRealSampler11(context, stage, slot, desiredSampler);
            ++rebound;
        }
        if (rememberedSampler) {
            rememberedSampler->Release();
        }
        if (logicalSampler) {
            logicalSampler->Release();
        }
    }
    ClearPixelSamplerDirtyMask11(context, rangeMask);

    const int totalRebound = dx11_hook_g_DiagSamplerEffectiveBinds.fetch_add(rebound, std::memory_order_relaxed) + rebound;
    int bindIdx = dx11_hook_g_DiagSamplerEffectiveBindCalls.fetch_add(1, std::memory_order_relaxed);
    if (bindIdx < 48) {
        HookLog(
            "DX11: AF sampler bind effective stage=PS start=%u num=%u dirtyMask=0x%04X changedMask=0x%04X "
            "resolved=%d rebound=%d totalRebound=%d",
            startSlot, numSamplers, dirtyMask, changedMask, resolved, rebound, totalRebound);
    }
}

inline void InstallContextVTableHooks11(ID3D11DeviceContext* context, const char* source);

inline HRESULT STDMETHODCALLTYPE DetourCreatePixelShader11(ID3D11Device* device, const void* shaderBytecode,
                                                           SIZE_T bytecodeLength, ID3D11ClassLinkage* classLinkage,
                                                           ID3D11PixelShader** pixelShader) {
    const HRESULT hr = dx11_hook_oCreatePixelShader11(device, shaderBytecode, bytecodeLength, classLinkage, pixelShader);
    if (SUCCEEDED(hr) && pixelShader && *pixelShader) {
        RegisterWrapperPixelShaderAFMetadata(*pixelShader, shaderBytecode, bytecodeLength);
    }
    return hr;
}

inline HRESULT STDMETHODCALLTYPE DetourCreateDeferredContext11(ID3D11Device* device, UINT contextFlags,
                                                               ID3D11DeviceContext** deferredContext) {
    if (!dx11_hook_oCreateDeferredContext11) {
        return E_FAIL;
    }
    const HRESULT hr = dx11_hook_oCreateDeferredContext11(device, contextFlags, deferredContext);
    if (SUCCEEDED(hr) && deferredContext && *deferredContext) {
        InstallContextVTableHooks11(*deferredContext, "CreateDeferredContext");
        int idx = dx11_hook_g_DiagCreateDeferredContext11.fetch_add(1, std::memory_order_relaxed);
        if (idx < 16) {
            void** vtable = *reinterpret_cast<void***>(*deferredContext);
            HookLogImportant("DX11: CreateDeferredContext returned ctx=%p flags=0x%X vtable=%p (#%d)",
                             (void*)*deferredContext, contextFlags, (void*)vtable, idx + 1);
        }
    }
    return hr;
}

inline void STDMETHODCALLTYPE DetourPSSetShader11(ID3D11DeviceContext* context, ID3D11PixelShader* pixelShader,
                                                  ID3D11ClassInstance* const* classInstances, UINT numClassInstances) {
    PSSetShader11_t original =
        ResolveContextOriginal11(context, 9, &D3D11ContextVTableOriginals::psSetShader, dx11_hook_oPSSetShader11);
    if (!original) {
        return;
    }
    original(context, pixelShader, classInstances, numClassInstances);
    if (dx11_hook_g_InOverlayRender || DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    UpdateTrackedPixelShader11(context, pixelShader);
}

inline void STDMETHODCALLTYPE DetourPSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    SetShaderResources11_t original = ResolveContextOriginal11(
        context, 8, &D3D11ContextVTableOriginals::psSetShaderResources, dx11_hook_oPSSetShaderResources11);
    if (!original) {
        return;
    }
    original(context, startSlot, numViews, ppShaderResourceViews);
    if (DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    UpdateStageShaderResources(context, D3D11ShaderStage::Pixel, startSlot, numViews, ppShaderResourceViews);
}

inline void STDMETHODCALLTYPE DetourVSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    SetShaderResources11_t original = ResolveContextOriginal11(
        context, 25, &D3D11ContextVTableOriginals::vsSetShaderResources, dx11_hook_oVSSetShaderResources11);
    if (!original) {
        return;
    }

    original(context, startSlot, numViews, ppShaderResourceViews);
    if (DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    UpdateStageShaderResources(context, D3D11ShaderStage::Vertex, startSlot, numViews, ppShaderResourceViews);
}

inline void STDMETHODCALLTYPE DetourGSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    SetShaderResources11_t original = ResolveContextOriginal11(
        context, 31, &D3D11ContextVTableOriginals::gsSetShaderResources, dx11_hook_oGSSetShaderResources11);
    if (!original) {
        return;
    }
    original(context, startSlot, numViews, ppShaderResourceViews);
    if (DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    UpdateStageShaderResources(context, D3D11ShaderStage::Geometry, startSlot, numViews, ppShaderResourceViews);
}

inline void STDMETHODCALLTYPE DetourHSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    SetShaderResources11_t original = ResolveContextOriginal11(
        context, 59, &D3D11ContextVTableOriginals::hsSetShaderResources, dx11_hook_oHSSetShaderResources11);
    if (!original) {
        return;
    }
    original(context, startSlot, numViews, ppShaderResourceViews);
    if (DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    UpdateStageShaderResources(context, D3D11ShaderStage::Hull, startSlot, numViews, ppShaderResourceViews);
}

inline void STDMETHODCALLTYPE DetourDSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    SetShaderResources11_t original = ResolveContextOriginal11(
        context, 63, &D3D11ContextVTableOriginals::dsSetShaderResources, dx11_hook_oDSSetShaderResources11);
    if (!original) {
        return;
    }
    original(context, startSlot, numViews, ppShaderResourceViews);
    if (DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    UpdateStageShaderResources(context, D3D11ShaderStage::Domain, startSlot, numViews, ppShaderResourceViews);
}

inline void STDMETHODCALLTYPE DetourCSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    SetShaderResources11_t original = ResolveContextOriginal11(
        context, 67, &D3D11ContextVTableOriginals::csSetShaderResources, dx11_hook_oCSSetShaderResources11);
    if (!original) {
        return;
    }
    original(context, startSlot, numViews, ppShaderResourceViews);
    if (DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    UpdateStageShaderResources(context, D3D11ShaderStage::Compute, startSlot, numViews, ppShaderResourceViews);
}

inline void STDMETHODCALLTYPE DetourPSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers) {
    SetSamplers11_t original =
        ResolveContextOriginal11(context, 10, &D3D11ContextVTableOriginals::psSetSamplers, dx11_hook_oPSSetSamplers11);
    SetSamplersWithOverrides11(original, context, D3D11ShaderStage::Pixel, startSlot, numSamplers, ppSamplers);
}

inline void STDMETHODCALLTYPE DetourVSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers) {
    SetSamplers11_t original =
        ResolveContextOriginal11(context, 26, &D3D11ContextVTableOriginals::vsSetSamplers, dx11_hook_oVSSetSamplers11);
    SetSamplersWithOverrides11(original, context, D3D11ShaderStage::Vertex, startSlot, numSamplers, ppSamplers);
}

inline void STDMETHODCALLTYPE DetourGSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers) {
    SetSamplers11_t original =
        ResolveContextOriginal11(context, 32, &D3D11ContextVTableOriginals::gsSetSamplers, dx11_hook_oGSSetSamplers11);
    SetSamplersWithOverrides11(original, context, D3D11ShaderStage::Geometry, startSlot, numSamplers, ppSamplers);
}

inline void STDMETHODCALLTYPE DetourHSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers) {
    SetSamplers11_t original =
        ResolveContextOriginal11(context, 61, &D3D11ContextVTableOriginals::hsSetSamplers, dx11_hook_oHSSetSamplers11);
    SetSamplersWithOverrides11(original, context, D3D11ShaderStage::Hull, startSlot, numSamplers, ppSamplers);
}

inline void STDMETHODCALLTYPE DetourDSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers) {
    SetSamplers11_t original =
        ResolveContextOriginal11(context, 65, &D3D11ContextVTableOriginals::dsSetSamplers, dx11_hook_oDSSetSamplers11);
    SetSamplersWithOverrides11(original, context, D3D11ShaderStage::Domain, startSlot, numSamplers, ppSamplers);
}

inline void STDMETHODCALLTYPE DetourCSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers) {
    SetSamplers11_t original =
        ResolveContextOriginal11(context, 70, &D3D11ContextVTableOriginals::csSetSamplers, dx11_hook_oCSSetSamplers11);
    SetSamplersWithOverrides11(original, context, D3D11ShaderStage::Compute, startSlot, numSamplers, ppSamplers);
}

inline void ReconcilePixelSamplersBeforeDraw11(ID3D11DeviceContext* context) {
    if (!context || dx11_hook_g_InOverlayRender || DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    if (dx11_hook_g_D3D11DirtyContextCount.load(std::memory_order_acquire) == 0) {
        return;
    }
    SetSamplers11_t original =
        ResolveContextOriginal11(context, 10, &D3D11ContextVTableOriginals::psSetSamplers, dx11_hook_oPSSetSamplers11);
    if (!original) {
        return;
    }
    const uint32_t dirtyMask = ConsumePixelSamplerDirtyMask11(context);
    if (dirtyMask == 0) {
        return;
    }
    const int rebound = ReconcileStageSamplers11(original, context, D3D11ShaderStage::Pixel, 0,
                                                 D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, dirtyMask);
    int idx = dx11_hook_g_DiagSamplerDrawReconcileCalls.fetch_add(1, std::memory_order_relaxed);
    if (idx < 24) {
        HookLog("DX11: AF draw reconcile ctx=%p dirtyMask=0x%04X rebound=%d (#%d)", (void*)context, dirtyMask, rebound,
                idx + 1);
    }
}

inline void STDMETHODCALLTYPE DetourDrawIndexed11(ID3D11DeviceContext* context, UINT indexCount,
                                                  UINT startIndexLocation, INT baseVertexLocation) {
    ReconcilePixelSamplersBeforeDraw11(context);
    DrawIndexed11_t original =
        ResolveContextOriginal11(context, 12, &D3D11ContextVTableOriginals::drawIndexed, dx11_hook_oDrawIndexed11);
    if (original) {
        original(context, indexCount, startIndexLocation, baseVertexLocation);
    }
}

inline void STDMETHODCALLTYPE DetourDraw11(ID3D11DeviceContext* context, UINT vertexCount, UINT startVertexLocation) {
    ReconcilePixelSamplersBeforeDraw11(context);
    Draw11_t original = ResolveContextOriginal11(context, 13, &D3D11ContextVTableOriginals::draw, dx11_hook_oDraw11);
    if (original) {
        original(context, vertexCount, startVertexLocation);
    }
}

inline void STDMETHODCALLTYPE DetourDrawIndexedInstanced11(ID3D11DeviceContext* context, UINT indexCountPerInstance,
                                                           UINT instanceCount, UINT startIndexLocation,
                                                           INT baseVertexLocation, UINT startInstanceLocation) {
    ReconcilePixelSamplersBeforeDraw11(context);
    DrawIndexedInstanced11_t original = ResolveContextOriginal11(
        context, 20, &D3D11ContextVTableOriginals::drawIndexedInstanced, dx11_hook_oDrawIndexedInstanced11);
    if (original) {
        original(context, indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation,
                 startInstanceLocation);
    }
}

inline void STDMETHODCALLTYPE DetourDrawInstanced11(ID3D11DeviceContext* context, UINT vertexCountPerInstance,
                                                    UINT instanceCount, UINT startVertexLocation,
                                                    UINT startInstanceLocation) {
    ReconcilePixelSamplersBeforeDraw11(context);
    DrawInstanced11_t original =
        ResolveContextOriginal11(context, 21, &D3D11ContextVTableOriginals::drawInstanced, dx11_hook_oDrawInstanced11);
    if (original) {
        original(context, vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation);
    }
}

inline void STDMETHODCALLTYPE DetourDrawAuto11(ID3D11DeviceContext* context) {
    ReconcilePixelSamplersBeforeDraw11(context);
    DrawAuto11_t original = ResolveContextOriginal11(context, 38, &D3D11ContextVTableOriginals::drawAuto, dx11_hook_oDrawAuto11);
    if (original) {
        original(context);
    }
}

inline void STDMETHODCALLTYPE DetourDrawIndexedInstancedIndirect11(ID3D11DeviceContext* context,
                                                                   ID3D11Buffer* bufferForArgs,
                                                                   UINT alignedByteOffsetForArgs) {
    ReconcilePixelSamplersBeforeDraw11(context);
    DrawIndexedInstancedIndirect11_t original = ResolveContextOriginal11(
        context, 39, &D3D11ContextVTableOriginals::drawIndexedInstancedIndirect, dx11_hook_oDrawIndexedInstancedIndirect11);
    if (original) {
        original(context, bufferForArgs, alignedByteOffsetForArgs);
    }
}

inline void STDMETHODCALLTYPE DetourDrawInstancedIndirect11(ID3D11DeviceContext* context, ID3D11Buffer* bufferForArgs,
                                                            UINT alignedByteOffsetForArgs) {
    ReconcilePixelSamplersBeforeDraw11(context);
    DrawInstancedIndirect11_t original = ResolveContextOriginal11(
        context, 40, &D3D11ContextVTableOriginals::drawInstancedIndirect, dx11_hook_oDrawInstancedIndirect11);
    if (original) {
        original(context, bufferForArgs, alignedByteOffsetForArgs);
    }
}

inline void STDMETHODCALLTYPE DetourExecuteCommandList11(ID3D11DeviceContext* context, ID3D11CommandList* commandList,
                                                         BOOL restoreContextState) {
    int idx = dx11_hook_g_DiagExecuteCommandList11.fetch_add(1, std::memory_order_relaxed);
    if (idx < 24) {
        HookLogImportant(
            "DX11: ExecuteCommandList ctx=%p commandList=%p restore=%d deferredContexts=%d "
            "drawReconcile=%d (#%d)",
            (void*)context, (void*)commandList, restoreContextState ? 1 : 0,
            dx11_hook_g_DiagCreateDeferredContext11.load(std::memory_order_relaxed),
            dx11_hook_g_DiagSamplerDrawReconcileCalls.load(std::memory_order_relaxed), idx + 1);
    }

    ExecuteCommandList11_t original =
        ResolveContextOriginal11(context, 58, &D3D11ContextVTableOriginals::executeCommandList, dx11_hook_oExecuteCommandList11);
    if (original) {
        original(context, commandList, restoreContextState);
    }
    if (!restoreContextState) {
        ClearTrackedContextState11(context);
    }
}

// Local copy of the real original D3D11CreateDeviceAndSwapChain function address.
// HookExport calls PatchIATAllModules which overwrites the shared
// oD3D11CreateDeviceAndSwapChain (also used by wrapper_hooks.cpp) with the
// address of Wrapped_D3D11CreateDeviceAndSwapChain — causing the DX11 detour
// to call back into the wrapper instead of the real function, leading to
// infinite IAT recursion and stack overflow (0xC00000FD).
// Save the real GetProcAddress result separately so DetourD3D11CreateDeviceAndSwapChain
// can always reach the actual d3d11.dll code.
inline PFN_D3D11CreateDeviceAndSwapChain dx11_hook_s_oRealD3D11CreateDeviceAndSwapChain = NULL;

inline CreateSwapChain_t dx11_hook_oCreateSwapChain = NULL;

inline CreateSwapChainForHwnd_t dx11_hook_oCreateSwapChainForHwnd = NULL;

inline ResizeBuffers_t dx11_hook_oResizeBuffers = NULL;

// Forward Declarations (non-static for cross-file hook collision detection from
// dx12_hook.cpp) Helper to get VSync override settings (reduces duplication)
inline VSyncOverride GetDX11VSyncOverride() {
    return GetVSyncOverride();  // Use the shared helper from hook_common.h
}

inline bool ShouldSkipWindowForNvPresent(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd))
        return true;
    if (!IsWindowVisible(hwnd))
        return true;

    RECT clientRect = {};
    if (GetClientRect(hwnd, &clientRect) &&
        (clientRect.right <= clientRect.left || clientRect.bottom <= clientRect.top)) {
        return true;
    }

    return false;
}

inline void ProcessDX11FrameWithOverlayOrdering(IDXGISwapChain* pSwapChain);

inline void InstallVTableHooks(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, IDXGISwapChain* pSwapChain);

inline HRESULT WINAPI DetourD3D11CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType,
                                                          HMODULE Software, UINT Flags,
                                                          const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
                                                          UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                                          IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
                                                          D3D_FEATURE_LEVEL* pFeatureLevel,
                                                          ID3D11DeviceContext** ppImmediateContext) {
    // Unconditional log to verify hook is being called
    HookLog("DetourD3D11CreateDeviceAndSwapChain: ENTER");

    if (pSwapChainDesc) {
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
            EarlyLog("DX11: D3D11CreateDeviceAndSwapChain called. Width=%u Height=%u", pSwapChainDesc->BufferDesc.Width,
                     pSwapChainDesc->BufferDesc.Height);
        }
    } else {
        EarlyLog("DX11: D3D11CreateDeviceAndSwapChain called (pSwapChainDesc=NULL)");
    }

    DXGI_SWAP_CHAIN_DESC desc;
    const DXGI_SWAP_CHAIN_DESC* pFinalDesc = pSwapChainDesc;

    if (pSwapChainDesc) {
        const GraphicsConfig& gfx = GetActiveGraphicsConfig();
        desc = *pSwapChainDesc;
        bool modified = false;

        // Backbuffer Count
        modified = ApplyDX11BackbufferCountOverride(desc, "CreateDeviceAndSwapChain") || modified;

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
                if (strcmp(msaa, "2x") == 0)
                    samples = 2;
                else if (strcmp(msaa, "4x") == 0)
                    samples = 4;
                else if (strcmp(msaa, "8x") == 0)
                    samples = 8;

                if (samples > 1) {
                    desc.SampleDesc.Count = samples;
                    desc.SampleDesc.Quality = 0;
                    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;  // MSAA requires DISCARD in D3D11
                    modified = true;
                    HookLog("DX11: CreateDeviceAndSwapChain: Forcing MSAA %dx", samples);
                }
            }
        }

        if (modified)
            pFinalDesc = &desc;
    }

    // Use the local copy of the real original.  The shared oD3D11CreateDeviceAndSwapChain
    // (from dx11_hook.h) may have been overwritten by HookExport -> PatchIATAllModules,
    // which sets it to Wrapped_D3D11CreateDeviceAndSwapChain when a prior IAT hook exists.
    // Calling that would re-enter the wrapper -> infinite recursion -> stack overflow.
    PFN_D3D11CreateDeviceAndSwapChain realOriginal =
        dx11_hook_s_oRealD3D11CreateDeviceAndSwapChain ? dx11_hook_s_oRealD3D11CreateDeviceAndSwapChain : oD3D11CreateDeviceAndSwapChain;
    HRESULT hr = realOriginal(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
                              pFinalDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);

    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        DX11Hook_RegisterDeviceIdentity(*ppDevice, "D3D11CreateDeviceAndSwapChain", true);
        const GraphicsConfig& gfx = GetActiveGraphicsConfig();
        if (ppSwapChain && *ppSwapChain && HasBackbufferCountOverride(gfx.backbufferCount)) {
            DXGI_SWAP_CHAIN_DESC actualDesc = {};
            if (SUCCEEDED((*ppSwapChain)->GetDesc(&actualDesc))) {
                HookLogImportant(
                    "DX11: CreateDeviceAndSwapChain actual BufferCount=%u requested=%d "
                    "size=%ux%u swapEffect=%d",
                    actualDesc.BufferCount, gfx.backbufferCount, actualDesc.BufferDesc.Width,
                    actualDesc.BufferDesc.Height, actualDesc.SwapEffect);
            }
        }

        // Wrap the swapchain returned by D3D11CreateDeviceAndSwapChain so our
        // wrapper captures Present
        if (ppSwapChain && *ppSwapChain) {
            IUnknown* pDev = (ppDevice && *ppDevice) ? *ppDevice : nullptr;
            IDXGISwapChain* pReal = *ppSwapChain;
            *ppSwapChain = (IDXGISwapChain*)new CWrapDXGISwapChain(pReal, pDev);
            pReal->Release();
            HookLogImportant("DX11: Wrapped swapchain from D3D11CreateDeviceAndSwapChain");
        }

        IDXGISwapChain* sc = (ppSwapChain && *ppSwapChain) ? *ppSwapChain : nullptr;
        ID3D11DeviceContext* ctx = (ppImmediateContext && *ppImmediateContext) ? *ppImmediateContext : nullptr;
        // If immediate context not provided, get it from device
        if (!ctx)
            (*ppDevice)->GetImmediateContext(&ctx);  // AddRef'd

        InstallVTableHooks(*ppDevice, ctx, sc);

        // Note: Factory vtable hooks removed - wrappers handle swapchain creation

        if (!ppImmediateContext && ctx)
            ctx->Release();

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

inline HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory* pFactory, IUnknown* pDevice,
                                                       DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain) {
    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
        if (pDesc) {
            EarlyLog(
                "DX11: CreateSwapChain called. Width=%u Height=%u Windowed=%d "
                "BufferCount=%u SwapEffect=%d",
                pDesc->BufferDesc.Width, pDesc->BufferDesc.Height, pDesc->Windowed, pDesc->BufferCount,
                pDesc->SwapEffect);
        }
    }

    DXGI_SWAP_CHAIN_DESC modifiedDesc = {};
    DXGI_SWAP_CHAIN_DESC* pDescToUse = pDesc;
    if (pDesc) {
        modifiedDesc = *pDesc;
        ApplyDX11BackbufferCountOverride(modifiedDesc, "CreateSwapChain");
        pDescToUse = &modifiedDesc;
    }

    HRESULT hr = dx11_hook_oCreateSwapChain(pFactory, DeWrap(pDevice), pDescToUse, ppSwapChain);

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        DXGI_SWAP_CHAIN_DESC actualDesc = {};
        if (SUCCEEDED((*ppSwapChain)->GetDesc(&actualDesc))) {
            const GraphicsConfig& gfx = GetActiveGraphicsConfig();
            if (HasBackbufferCountOverride(gfx.backbufferCount)) {
                HookLogImportant(
                    "DX11: CreateSwapChain created sc=%p actualBufferCount=%u requested=%d "
                    "size=%ux%u swapEffect=%d",
                    *ppSwapChain, actualDesc.BufferCount, gfx.backbufferCount, actualDesc.BufferDesc.Width,
                    actualDesc.BufferDesc.Height, actualDesc.SwapEffect);
            }
        }
        // FSR4/FG swapchain recreation detection (shared with
        // CreateSwapChainForHwnd)
        bool isGameSizedSwapchain =
            pDescToUse && pDescToUse->BufferDesc.Width >= 1920 && pDescToUse->BufferDesc.Height >= 1080;
        if (isGameSizedSwapchain) {
            if (dx11_hook_g_FirstGameSwapchainCreated) {
                // Recreation - likely FG taking over
                HookLog(
                    "DX11: CreateSwapChain: Game-sized swapchain recreated - "
                    "invalidating DX12 overlay");
                DX12_SignalFSR4SwapchainRecreated();
            } else {
                dx11_hook_g_FirstGameSwapchainCreated = true;
                HookLog("DX11: CreateSwapChain: First game-sized swapchain created (%ux%u)",
                        pDescToUse->BufferDesc.Width, pDescToUse->BufferDesc.Height);
            }
        }

        // First try D3D12 - DX12 games create swapchains via DXGI too
        ID3D12CommandQueue* pD3D12Queue = nullptr;
        ID3D12Device* pD3D12Device = nullptr;

        // Check for CommandQueue (standard DX12)
        if (pDevice && SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&pD3D12Queue))) {
            HookLog("DX11: CreateSwapChain - Detected DX12 CommandQueue.");
            DX12_SignalFSR4SwapchainRecreated();
            // Hook the command queue vtable so ExecuteCommandLists is intercepted
            // This is needed to capture the command queue for overlay rendering
            DX12_HookQueueVTable(pD3D12Queue);
            HookLog("DX11: Hooked DX12 CommandQueue vtable from DX11 path");
            pD3D12Queue->Release();
        }
        // Check for D3D12 Device (non-standard but possible, or checking on
        // swapchain itself)
        else if (ppSwapChain && *ppSwapChain &&
                 SUCCEEDED((*ppSwapChain)->GetDevice(__uuidof(ID3D12Device), (void**)&pD3D12Device))) {
            HookLog("DX11: CreateSwapChain - Detected DX12 Device from SwapChain.");
            DX12_SignalFSR4SwapchainRecreated();
            HookLog(
                "DX11: Skipping DX12 hook installation from DX11 path (Device "
                "detection)");
            pD3D12Device->Release();
        } else {
            // DX11 path - original logic
            ID3D11Device* pD3D11Device = nullptr;
            if (SUCCEEDED((*ppSwapChain)->GetDevice(__uuidof(ID3D11Device), (void**)&pD3D11Device))) {
                ID3D11DeviceContext* ctx = nullptr;
                pD3D11Device->GetImmediateContext(&ctx);
                InstallVTableHooks(pD3D11Device, ctx, *ppSwapChain);
                if (ctx)
                    ctx->Release();
                pD3D11Device->Release();
            } else {
                // Fallback for D3D10/10.1 or other versions
                // CRITICAL FIX: Ensure we don't hook a DX12 swapchain that missed the
                // checks above
                InstallVTableHooks(NULL, NULL, *ppSwapChain);
            }
        }
    }
    return hr;
}

inline HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd,
                                                              const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                              const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                                              IDXGIOutput* pRestrictToOutput,
                                                              IDXGISwapChain1** ppSwapChain) {
    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
        if (pDesc) {
            EarlyLog(
                "DX11: CreateSwapChainForHwnd called. Width=%u Height=%u "
                "BufferCount=%u SwapEffect=%d",
                pDesc->Width, pDesc->Height, pDesc->BufferCount, pDesc->SwapEffect);
        }
    }

    DXGI_SWAP_CHAIN_DESC1 modifiedDesc = {};
    const DXGI_SWAP_CHAIN_DESC1* pDescToUse = pDesc;
    if (pDesc) {
        modifiedDesc = *pDesc;
        ApplyDX11BackbufferCountOverride(modifiedDesc, "CreateSwapChainForHwnd");
        pDescToUse = &modifiedDesc;
    }

    // CRITICAL FG FIX: Track game-sized swapchain recreation for FG overlay
    // safety We DON'T invalidate BEFORE creation - that can cause DXGI lock
    // issues (E_ACCESSDENIED) Instead, we invalidate AFTER successful recreation
    // to clean up stale overlay resources
    bool isGameSizedSwapchain = pDescToUse && pDescToUse->Width >= 1920 && pDescToUse->Height >= 1080;
    bool wasRecreation = isGameSizedSwapchain && dx11_hook_g_FirstGameSwapchainCreated;

    HookLog("DX11: BEFORE oCreateSwapChainForHwnd call");
    HRESULT hr = dx11_hook_oCreateSwapChainForHwnd(pFactory, DeWrap(pDevice), hWnd, pDescToUse, pFullscreenDesc,
                                         pRestrictToOutput, ppSwapChain);
    HookLog("DX11: AFTER oCreateSwapChainForHwnd call (hr=0x%08X)", hr);

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        DXGI_SWAP_CHAIN_DESC actualDesc = {};
        if (SUCCEEDED((*ppSwapChain)->GetDesc(&actualDesc))) {
            const GraphicsConfig& gfx = GetActiveGraphicsConfig();
            if (HasBackbufferCountOverride(gfx.backbufferCount)) {
                HookLogImportant(
                    "DX11: CreateSwapChainForHwnd created sc=%p actualBufferCount=%u requested=%d "
                    "size=%ux%u swapEffect=%d",
                    *ppSwapChain, actualDesc.BufferCount, gfx.backbufferCount, actualDesc.BufferDesc.Width,
                    actualDesc.BufferDesc.Height, actualDesc.SwapEffect);
            }
        }
        // Post-creation: Signal invalidation ONLY for successful recreation
        if (wasRecreation) {
            HookLog(
                "DX11: CreateSwapChainForHwnd: Game-sized swapchain RECREATED "
                "successfully - invalidating overlay");
            DX12_InvalidateSwapchain();
            DX12_SignalFSR4SwapchainRecreated();
        }
        // Post-creation tracking
        if (isGameSizedSwapchain) {
            if (!dx11_hook_g_FirstGameSwapchainCreated) {
                dx11_hook_g_FirstGameSwapchainCreated = true;
                HookLog(
                    "DX11: CreateSwapChainForHwnd: First game-sized swapchain "
                    "created (%ux%u)",
                    pDescToUse->Width, pDescToUse->Height);
            }
        }

        // First try D3D12 - DX12 games create swapchains via DXGI too
        ID3D12CommandQueue* pD3D12Queue = nullptr;
        ID3D12Device* pD3D12Device = nullptr;

        // Check for CommandQueue (standard DX12)
        if (pDevice && SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&pD3D12Queue))) {
            HookLog("DX11: CreateSwapChainForHwnd - Detected DX12 CommandQueue.");
            DX12_SignalFSR4SwapchainRecreated();
            // Hook the command queue vtable so ExecuteCommandLists is intercepted
            // This is needed to capture the command queue for overlay rendering
            DX12_HookQueueVTable(pD3D12Queue);
            HookLog("DX11: Hooked DX12 CommandQueue vtable from DX11 path");
            pD3D12Queue->Release();
        }
        // Check for D3D12 Device (non-standard but possible, or checking on
        // swapchain itself)
        else if (ppSwapChain && *ppSwapChain &&
                 SUCCEEDED((*ppSwapChain)->GetDevice(__uuidof(ID3D12Device), (void**)&pD3D12Device))) {
            HookLog(
                "DX11: CreateSwapChainForHwnd - Detected DX12 Device from "
                "SwapChain.");
            DX12_SignalFSR4SwapchainRecreated();
            HookLog(
                "DX11: Skipping DX12 hook installation from DX11 path (Device "
                "detection)");
            pD3D12Device->Release();
        } else {
            // DX11 path - original logic
            ID3D11Device* pD3D11Device = nullptr;
            if (SUCCEEDED((*ppSwapChain)->GetDevice(__uuidof(ID3D11Device), (void**)&pD3D11Device))) {
                ID3D11DeviceContext* ctx = nullptr;
                pD3D11Device->GetImmediateContext(&ctx);
                InstallVTableHooks(pD3D11Device, ctx, *ppSwapChain);
                if (ctx)
                    ctx->Release();
                pD3D11Device->Release();
            } else {
                // Fallback for D3D10/10.1 or other versions
                // CRITICAL FIX: Ensure we don't hook a DX12 swapchain that missed the
                // checks above This prevents infinite ResizeBuffers loops in games like
                // Strange Brigade DX12
                InstallVTableHooks(NULL, NULL, *ppSwapChain);
            }
        }
    }
    return hr;
}

inline HRESULT STDMETHODCALLTYPE DetourCreateSamplerState(ID3D11Device* pDevice, const D3D11_SAMPLER_DESC* pSamplerDesc,
                                                          ID3D11SamplerState** ppSamplerState);

inline HRESULT STDMETHODCALLTYPE DetourCreatePixelShader11(ID3D11Device* device, const void* shaderBytecode,
                                                           SIZE_T bytecodeLength, ID3D11ClassLinkage* classLinkage,
                                                           ID3D11PixelShader** pixelShader);

inline HRESULT STDMETHODCALLTYPE DetourCreateDeferredContext11(ID3D11Device* device, UINT contextFlags,
                                                               ID3D11DeviceContext** deferredContext);

inline void STDMETHODCALLTYPE DetourPSSetShader11(ID3D11DeviceContext* context, ID3D11PixelShader* pixelShader,
                                                  ID3D11ClassInstance* const* classInstances, UINT numClassInstances);

inline void STDMETHODCALLTYPE DetourDrawIndexed11(ID3D11DeviceContext* context, UINT indexCount,
                                                  UINT startIndexLocation, INT baseVertexLocation);

inline void STDMETHODCALLTYPE DetourDraw11(ID3D11DeviceContext* context, UINT vertexCount, UINT startVertexLocation);

inline void STDMETHODCALLTYPE DetourDrawIndexedInstanced11(ID3D11DeviceContext* context, UINT indexCountPerInstance,
                                                           UINT instanceCount, UINT startIndexLocation,
                                                           INT baseVertexLocation, UINT startInstanceLocation);

inline void STDMETHODCALLTYPE DetourDrawInstanced11(ID3D11DeviceContext* context, UINT vertexCountPerInstance,
                                                    UINT instanceCount, UINT startVertexLocation,
                                                    UINT startInstanceLocation);

inline void STDMETHODCALLTYPE DetourDrawAuto11(ID3D11DeviceContext* context);

inline void STDMETHODCALLTYPE DetourDrawIndexedInstancedIndirect11(ID3D11DeviceContext* context,
                                                                   ID3D11Buffer* bufferForArgs,
                                                                   UINT alignedByteOffsetForArgs);

inline void STDMETHODCALLTYPE DetourDrawInstancedIndirect11(ID3D11DeviceContext* context, ID3D11Buffer* bufferForArgs,
                                                            UINT alignedByteOffsetForArgs);

inline void STDMETHODCALLTYPE DetourExecuteCommandList11(ID3D11DeviceContext* context, ID3D11CommandList* commandList,
                                                         BOOL restoreContextState);

inline void STDMETHODCALLTYPE DetourPSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);

inline void STDMETHODCALLTYPE DetourVSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);

inline void STDMETHODCALLTYPE DetourGSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);

inline void STDMETHODCALLTYPE DetourHSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);

inline void STDMETHODCALLTYPE DetourDSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);

inline void STDMETHODCALLTYPE DetourCSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);

inline void STDMETHODCALLTYPE DetourPSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);

inline void STDMETHODCALLTYPE DetourVSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);

inline void STDMETHODCALLTYPE DetourGSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);

inline void STDMETHODCALLTYPE DetourHSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);

inline void STDMETHODCALLTYPE DetourDSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);

inline void STDMETHODCALLTYPE DetourCSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);

inline HRESULT STDMETHODCALLTYPE DetourCreateSamplerState10(ID3D10Device* pDevice,
                                                            const D3D10_SAMPLER_DESC* pSamplerDesc,
                                                            ID3D10SamplerState** ppSamplerState);

template <typename Fn>
static bool EnsureVTableHookSlot11(void** vtable, UINT index, LPVOID detour, Fn& original, const char* name) {
    if (!vtable || !detour) {
        return false;
    }

    void** slot = &vtable[index];
    void* current = *slot;
    if (current == detour) {
        return true;
    }

    void* knownOriginal = original ? reinterpret_cast<void*>(original) : nullptr;
    if (knownOriginal && current != knownOriginal) {
        static std::atomic<int> s_mismatchLogs{0};
        int idx = s_mismatchLogs.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            HookLogImportant("DX11: %s hook skipped on alternate vtable (slot=%u current=%p knownOriginal=%p)", name,
                             index, current, knownOriginal);
        }
        return false;
    }

    LPVOID capturedOriginal = nullptr;
    LPVOID* originalOut = original ? &capturedOriginal : reinterpret_cast<LPVOID*>(&original);
    VTableHook::Status status = VTableHook::Create(reinterpret_cast<void*>(slot), detour, originalOut);
    if (status == VTableHook::Success) {
        int idx = dx11_hook_g_DiagSamplerRuntimeHookInstalled.fetch_add(1, std::memory_order_relaxed);
        if (idx < 24) {
            HookLog("DX11: %s hook installed%s (slot=%u)", name, knownOriginal ? " on additional vtable" : "", index);
        }
        return true;
    }

    static std::atomic<int> s_failureLogs{0};
    int idx = s_failureLogs.fetch_add(1, std::memory_order_relaxed);
    if (idx < 12) {
        HookLogImportant("DX11: %s hook install failed status=%d slot=%u current=%p", name, status, index, current);
    }
    return false;
}

template <typename Fn>
static bool InstallContextVTableHookSlot11(void** vtable, UINT index, LPVOID detour, Fn& globalOriginal,
                                           Fn D3D11ContextVTableOriginals::* member, const char* name,
                                           const char* source) {
    if (!vtable || !detour) {
        return false;
    }

    void** slot = &vtable[index];
    void* current = *slot;
    if (current == detour) {
        return true;
    }

    LPVOID capturedOriginal = nullptr;
    VTableHook::Status status = VTableHook::Create(reinterpret_cast<void*>(slot), detour, &capturedOriginal);
    if (status == VTableHook::Success && *slot == detour && capturedOriginal) {
        void* vtableKey = reinterpret_cast<void*>(vtable);
        bool newVTable = false;
        {
            std::unique_lock<std::shared_mutex> lock(dx11_hook_g_D3D11ContextVTableOriginalsMutex);
            auto [it, inserted] = dx11_hook_g_D3D11ContextVTableOriginals.try_emplace(vtableKey);
            newVTable = inserted;
            it->second.*member = reinterpret_cast<Fn>(capturedOriginal);
        }
        dx11_hook_g_D3D11ContextVTableOriginalsGeneration.fetch_add(1, std::memory_order_acq_rel);

        if (newVTable) {
            void* expected = nullptr;
            if (dx11_hook_g_PrimaryD3D11ContextVTable.compare_exchange_strong(expected, vtableKey, std::memory_order_acq_rel)) {
                int idx = dx11_hook_g_DiagD3D11ContextVTablesHooked.fetch_add(1, std::memory_order_relaxed);
                if (idx < 12) {
                    HookLogImportant("DX11: Context vtable registered primary vtable=%p via %s (#%d)", vtableKey,
                                     source ? source : "unknown", idx + 1);
                }
            } else if (expected != vtableKey) {
                int idx = dx11_hook_g_DiagD3D11ContextVTablesHooked.fetch_add(1, std::memory_order_relaxed);
                if (idx < 12) {
                    HookLogImportant("DX11: Context vtable registered alternate vtable=%p primary=%p via %s (#%d)",
                                     vtableKey, expected, source ? source : "unknown", idx + 1);
                }
            }
        }

        if (!globalOriginal || dx11_hook_g_PrimaryD3D11ContextVTable.load(std::memory_order_acquire) == vtableKey) {
            globalOriginal = reinterpret_cast<Fn>(capturedOriginal);
        }

        int idx = dx11_hook_g_DiagSamplerRuntimeHookInstalled.fetch_add(1, std::memory_order_relaxed);
        if (idx < 32) {
            HookLog("DX11: %s hook installed on context vtable=%p via %s (slot=%u original=%p)", name,
                    reinterpret_cast<void*>(vtable), source ? source : "unknown", index, capturedOriginal);
        }
        return true;
    }

    int idx = dx11_hook_g_DiagD3D11ContextHookSkips.fetch_add(1, std::memory_order_relaxed);
    if (idx < 16) {
        HookLogImportant("DX11: %s context hook skipped status=%d slot=%u source=%s current=%p captured=%p now=%p",
                         name, status, index, source ? source : "unknown", current, capturedOriginal, *slot);
    }
    return false;
}

inline void InstallContextVTableHooks11(ID3D11DeviceContext* context, const char* source) {
    if (!context) {
        return;
    }

    void** pContextVTable = *(void***)context;

    InstallContextVTableHookSlot11(pContextVTable, 8, (LPVOID)&DetourPSSetShaderResources11, dx11_hook_oPSSetShaderResources11,
                                   &D3D11ContextVTableOriginals::psSetShaderResources, "PSSetShaderResources", source);
    InstallContextVTableHookSlot11(pContextVTable, 9, (LPVOID)&DetourPSSetShader11, dx11_hook_oPSSetShader11,
                                   &D3D11ContextVTableOriginals::psSetShader, "PSSetShader", source);
    InstallContextVTableHookSlot11(pContextVTable, 10, (LPVOID)&DetourPSSetSamplers11, dx11_hook_oPSSetSamplers11,
                                   &D3D11ContextVTableOriginals::psSetSamplers, "PSSetSamplers", source);
    InstallContextVTableHookSlot11(pContextVTable, 12, (LPVOID)&DetourDrawIndexed11, dx11_hook_oDrawIndexed11,
                                   &D3D11ContextVTableOriginals::drawIndexed, "DrawIndexed", source);
    InstallContextVTableHookSlot11(pContextVTable, 13, (LPVOID)&DetourDraw11, dx11_hook_oDraw11,
                                   &D3D11ContextVTableOriginals::draw, "Draw", source);
    InstallContextVTableHookSlot11(pContextVTable, 20, (LPVOID)&DetourDrawIndexedInstanced11, dx11_hook_oDrawIndexedInstanced11,
                                   &D3D11ContextVTableOriginals::drawIndexedInstanced, "DrawIndexedInstanced", source);
    InstallContextVTableHookSlot11(pContextVTable, 21, (LPVOID)&DetourDrawInstanced11, dx11_hook_oDrawInstanced11,
                                   &D3D11ContextVTableOriginals::drawInstanced, "DrawInstanced", source);
    InstallContextVTableHookSlot11(pContextVTable, 25, (LPVOID)&DetourVSSetShaderResources11, dx11_hook_oVSSetShaderResources11,
                                   &D3D11ContextVTableOriginals::vsSetShaderResources, "VSSetShaderResources", source);
    InstallContextVTableHookSlot11(pContextVTable, 26, (LPVOID)&DetourVSSetSamplers11, dx11_hook_oVSSetSamplers11,
                                   &D3D11ContextVTableOriginals::vsSetSamplers, "VSSetSamplers", source);
    InstallContextVTableHookSlot11(pContextVTable, 31, (LPVOID)&DetourGSSetShaderResources11, dx11_hook_oGSSetShaderResources11,
                                   &D3D11ContextVTableOriginals::gsSetShaderResources, "GSSetShaderResources", source);
    InstallContextVTableHookSlot11(pContextVTable, 32, (LPVOID)&DetourGSSetSamplers11, dx11_hook_oGSSetSamplers11,
                                   &D3D11ContextVTableOriginals::gsSetSamplers, "GSSetSamplers", source);
    InstallContextVTableHookSlot11(pContextVTable, 38, (LPVOID)&DetourDrawAuto11, dx11_hook_oDrawAuto11,
                                   &D3D11ContextVTableOriginals::drawAuto, "DrawAuto", source);
    InstallContextVTableHookSlot11(
        pContextVTable, 39, (LPVOID)&DetourDrawIndexedInstancedIndirect11, dx11_hook_oDrawIndexedInstancedIndirect11,
        &D3D11ContextVTableOriginals::drawIndexedInstancedIndirect, "DrawIndexedInstancedIndirect", source);
    InstallContextVTableHookSlot11(pContextVTable, 40, (LPVOID)&DetourDrawInstancedIndirect11, dx11_hook_oDrawInstancedIndirect11,
                                   &D3D11ContextVTableOriginals::drawInstancedIndirect, "DrawInstancedIndirect",
                                   source);
    InstallContextVTableHookSlot11(pContextVTable, 58, (LPVOID)&DetourExecuteCommandList11, dx11_hook_oExecuteCommandList11,
                                   &D3D11ContextVTableOriginals::executeCommandList, "ExecuteCommandList", source);
    InstallContextVTableHookSlot11(pContextVTable, 59, (LPVOID)&DetourHSSetShaderResources11, dx11_hook_oHSSetShaderResources11,
                                   &D3D11ContextVTableOriginals::hsSetShaderResources, "HSSetShaderResources", source);
    InstallContextVTableHookSlot11(pContextVTable, 61, (LPVOID)&DetourHSSetSamplers11, dx11_hook_oHSSetSamplers11,
                                   &D3D11ContextVTableOriginals::hsSetSamplers, "HSSetSamplers", source);
    InstallContextVTableHookSlot11(pContextVTable, 63, (LPVOID)&DetourDSSetShaderResources11, dx11_hook_oDSSetShaderResources11,
                                   &D3D11ContextVTableOriginals::dsSetShaderResources, "DSSetShaderResources", source);
    InstallContextVTableHookSlot11(pContextVTable, 65, (LPVOID)&DetourDSSetSamplers11, dx11_hook_oDSSetSamplers11,
                                   &D3D11ContextVTableOriginals::dsSetSamplers, "DSSetSamplers", source);
    InstallContextVTableHookSlot11(pContextVTable, 67, (LPVOID)&DetourCSSetShaderResources11, dx11_hook_oCSSetShaderResources11,
                                   &D3D11ContextVTableOriginals::csSetShaderResources, "CSSetShaderResources", source);
    InstallContextVTableHookSlot11(pContextVTable, 70, (LPVOID)&DetourCSSetSamplers11, dx11_hook_oCSSetSamplers11,
                                   &D3D11ContextVTableOriginals::csSetSamplers, "CSSetSamplers", source);
}

// Helper to install vtable hooks
inline void InstallVTableHooks(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, IDXGISwapChain* pSwapChain) {
    // Hook D3D11 Device methods
    if (pDevice) {
        DX11Hook_RegisterDeviceIdentity(pDevice, "D3D11 device hook installation");
        InstallD3D11IdentityQueryHook(pDevice, "device");
        void** pDeviceVTable = *(void***)pDevice;
        EnsureVTableHookSlot11(pDeviceVTable, 15, (LPVOID)&DetourCreatePixelShader11, dx11_hook_oCreatePixelShader11,
                               "CreatePixelShader");
        // Index 23 is CreateSamplerState for D3D11
        EnsureVTableHookSlot11(pDeviceVTable, 23, (LPVOID)&DetourCreateSamplerState, dx11_hook_oCreateSamplerState,
                               "CreateSamplerState");
        EnsureVTableHookSlot11(pDeviceVTable, 27, (LPVOID)&DetourCreateDeferredContext11, dx11_hook_oCreateDeferredContext11,
                               "CreateDeferredContext");
    }

    InstallD3D11IdentityQueryHook(pContext, "context");
    InstallContextVTableHooks11(pContext, "immediate");

    // Some DX11 implementations expose D3D10 compatibility interfaces too.
    // Only install the D3D10 runtime hooks when the swapchain actually belongs
    // to a D3D10 device.
    if (pSwapChain && DetectSwapChainAPITypeForDX11Hook(pSwapChain) == DXGIShared::APIType::D3D10) {
        ID3D10Device* pDevice10 = nullptr;
        HRESULT hr = pSwapChain->GetDevice(__uuidof(ID3D10Device), (void**)&pDevice10);
        if (SUCCEEDED(hr) && pDevice10) {
            void** pDeviceVTable = *(void***)pDevice10;

            // CreateSamplerState (Index 9)
            if (dx11_hook_oCreateSamplerState10 == NULL) {
                if (VTableHook::Create(reinterpret_cast<void*>(&pDeviceVTable[9]), (LPVOID)&DetourCreateSamplerState10,
                                       (LPVOID*)&dx11_hook_oCreateSamplerState10) == VTableHook::Success) {
                    HookLog("DX10: CreateSamplerState hook installed");
                }
            }
            pDevice10->Release();
        }
    }
}

inline HWND dx11_hook_g_CachedHwnd = NULL;

// DX11 Capture Implementation extending HookCaptureBase
// Supports both DX11 and DX10 games.
// DX10 capture must copy on the real D3D10 device and publish DXGI shared
// texture handles that the media-side D3D11 device can open.
class DX11Capture : public HookCaptureBase {
public:
    std::recursive_mutex captureMutex;
    ID3D11Texture2D* sharedTextures[CAPTURE_TEXTURE_COUNT]{};
    ID3D11Query* copyQueries[CAPTURE_TEXTURE_COUNT]{};  // GPU sync queries
    ID3D11Device* cachedDevice = nullptr;
    ID3D11DeviceContext* cachedContext = nullptr;
    IUnknown* cachedSwapChainIdentity = nullptr;
    bool generationResetPending = false;

    // DX10 capture must stay on the real D3D10 device to avoid invalid
    // cross-device copies from a DX10 swapchain buffer into D3D11-owned
    // textures, which can produce corrupted output.
    ID3D10Device* cachedDevice10 = nullptr;
    ID3D10Texture2D* sharedTextures10[CAPTURE_TEXTURE_COUNT]{};
    ID3D10Query* copyQueries10[CAPTURE_TEXTURE_COUNT]{};
    bool isDX10Mode = false;

    // For DXVK games: the game's D3D11 is DXVK's (Vulkan-backed). Shared handles
    // from DXVK are Vulkan-internal IDs the real encoder D3D11 can't open.
    // Fix: create ring buffer textures in a real system D3D11 device (ownedDevice),
    // then import each into DXVK's device for CopyResource.
    ID3D11Device* ownedDevice = nullptr;
    ID3D11DeviceContext* ownedContext = nullptr;
    bool isDXVKMode = false;
    ID3D11Texture2D* dxvkImportedTextures[CAPTURE_TEXTURE_COUNT]{};  // DXVK-side imports

    // DX11.3 Fence Support
    ID3D11Fence* fence = nullptr;
    ID3D11DeviceContext4* context4 = nullptr;  // Needed for Signal
    bool useFences = false;
    UINT64 fenceValue = 0;
    UINT64 slotFenceValues[CAPTURE_TEXTURE_COUNT]{};

    // Keyed Mutex Support (Proper Fix)
    IDXGIKeyedMutex* keyedMutexes[CAPTURE_TEXTURE_COUNT]{};
    bool useKeyedMutex = false;
    bool sharedTextureHandlesAreNt[CAPTURE_TEXTURE_COUNT]{};

    // sharedTextureHandles are in base class

    void Cleanup() override {
        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        CaptureBase::StopCaptureThread();
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
            HANDLE handle = sharedTextureHandles[i].exchange(NULL, std::memory_order_acq_rel);
            if (handle && sharedTextureHandlesAreNt[i]) {
                CloseHandle(handle);
            }
            sharedTextureHandlesAreNt[i] = false;
        }
        HANDLE fenceHandle = sharedFenceHandle.exchange(NULL, std::memory_order_acq_rel);
        if (fenceHandle) {
            CloseHandle(fenceHandle);
        }

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            g_DeferredRelease.Queue(sharedTextures[i]);
            sharedTextures[i] = nullptr;

            g_DeferredRelease.Queue(copyQueries[i]);
            copyQueries[i] = nullptr;

            g_DeferredRelease.Queue(sharedTextures10[i]);
            sharedTextures10[i] = nullptr;

            g_DeferredRelease.Queue(copyQueries10[i]);
            copyQueries10[i] = nullptr;

            slotFenceValues[i] = 0;

            g_DeferredRelease.Queue(dxvkImportedTextures[i]);
            dxvkImportedTextures[i] = nullptr;
        }

        g_DeferredRelease.Queue(fence);
        fence = nullptr;

        g_DeferredRelease.Queue(context4);
        context4 = nullptr;

        g_DeferredRelease.Queue(ownedContext);
        ownedContext = nullptr;

        g_DeferredRelease.Queue(ownedDevice);
        ownedDevice = nullptr;

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            g_DeferredRelease.Queue(keyedMutexes[i]);
            keyedMutexes[i] = nullptr;
        }

        g_DeferredRelease.Queue(cachedDevice10);
        cachedDevice10 = nullptr;

        // GetImmediateContext returns an owned reference. Keeping it across every
        // resize leaked a device/context generation and its driver allocations.
        g_DeferredRelease.Queue(cachedContext);
        cachedContext = nullptr;

        g_DeferredRelease.Queue(cachedSwapChainIdentity);
        cachedSwapChainIdentity = nullptr;

        cachedDevice = nullptr;
        initialized = false;
        generationResetPending = false;
        useFences = false;
        useKeyedMutex = false;
        isDX10Mode = false;
        isDXVKMode = false;
        fenceValue = 0;  // Reset fence value for next session
    }

    void RequestGenerationReset(IDXGISwapChain* swapChain) {
        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        if (!initialized || !swapChain)
            return;

        IUnknown* identity = nullptr;
        const HRESULT identityHr = swapChain->QueryInterface(IID_PPV_ARGS(&identity));
        const bool matchesCaptureSwapChain =
            SUCCEEDED(identityHr) && identity && cachedSwapChainIdentity && identity == cachedSwapChainIdentity;
        if (identity)
            identity->Release();
        if (!matchesCaptureSwapChain)
            return;

        initialized = false;
        generationResetPending = true;
        HookLog("DX11Capture: Swapchain resized; deferring capture generation rebuild until frame leases drain");
    }

    void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override {
        // This virtual method is called by CheckCaptureInit or manually
        // We need the device to create resources, so we'll store it in Init
    }

    // Initialize for DX10 games - capture stays on the real D3D10 device and
    // publishes DXGI shared handles that the media-side D3D11 device opens.
    bool InitDX10(IDXGISwapChain* swapChain) {
        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        ID3D10Device* device10 = nullptr;

        if (FAILED(swapChain->GetDevice(__uuidof(ID3D10Device), (void**)&device10))) {
            ID3D10Device1* device10_1 = nullptr;
            if (FAILED(swapChain->GetDevice(__uuidof(ID3D10Device1), (void**)&device10_1)) || !device10_1) {
                HookLog("DX10: Failed to get device from swapchain");
                return false;
            }
            device10 = device10_1;
        }

        if (!device10) {
            HookLog("DX10: Swapchain returned null device");
            return false;
        }

        cachedDevice10 = device10;  // Keep GetDevice() reference until Cleanup()

        if (SUCCEEDED(device10->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC adapterDesc;
                if (SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    luidLow = adapterDesc.AdapterLuid.LowPart;
                    luidHigh = adapterDesc.AdapterLuid.HighPart;

                    SystemMetricsCollector::Get().Initialize(luidLow, luidHigh);
                    SystemMetricsCollector::Get().SetVRAMTotal(adapterDesc.DedicatedVideoMemory);
                }
                adapter->Release();
            }
            dxgiDevice->Release();
        }

        isDX10Mode = true;
        return true;
    }

    // Detect if the loaded d3d11.dll is DXVK's (not the system one).
    // DXVK games place their d3d11.dll in the game directory, shadowing System32.
    static bool IsCurrentD3D11DXVK() {
        HMODULE hD3D11 = GetModuleHandleA("d3d11.dll");
        if (!hD3D11)
            return false;
        char path[MAX_PATH] = {};
        if (!GetModuleFileNameA(hD3D11, path, MAX_PATH))
            return false;
        char sysDir[MAX_PATH] = {};
        if (!GetSystemDirectoryA(sysDir, MAX_PATH))
            return false;
        uint32_t sysLen = (uint32_t)strlen(sysDir);
        return !(_strnicmp(path, sysDir, sysLen) == 0 && path[sysLen] == '\\');
    }

    // Create a real system D3D11 device on the GPU identified by a LUID.
    // Used for DXVK games so ring buffer textures carry valid Windows NT handles.
    bool CreateSystemD3D11DeviceForLUID(int32_t luidLowPart, int32_t luidHighPart) {
        char systemDir[MAX_PATH] = {};
        if (GetSystemDirectoryA(systemDir, MAX_PATH) == 0)
            return false;
        std::string dxgiPath = std::string(systemDir) + "\\dxgi.dll";
        std::string d3d11Path = std::string(systemDir) + "\\d3d11.dll";

        HMODULE hDXGI = LoadLibraryA(dxgiPath.c_str());
        if (!hDXGI) {
            EarlyLog("DX11-DXVK: System DXGI not found");
            return false;
        }
        typedef HRESULT(WINAPI * PFN_CREATE_DXGI_FACTORY1)(REFIID, void**);
        PFN_CREATE_DXGI_FACTORY1 pCreateDXGIFactory1 =
            (PFN_CREATE_DXGI_FACTORY1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
        if (!pCreateDXGIFactory1)
            return false;

        IDXGIFactory1* factory = nullptr;
        if (FAILED(pCreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return false;

        // Find the real adapter by LUID
        IDXGIAdapter1* adapter = nullptr;
        IDXGIAdapter1* matchedAdapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (desc.AdapterLuid.LowPart == (DWORD)luidLowPart && desc.AdapterLuid.HighPart == luidHighPart) {
                matchedAdapter = adapter;
                break;
            }
            adapter->Release();
        }
        factory->Release();

        if (!matchedAdapter) {
            EarlyLog("DX11-DXVK: No system DXGI adapter found for LUID");
            return false;
        }

        HMODULE hD3D11 = LoadLibraryA(d3d11Path.c_str());
        if (!hD3D11) {
            matchedAdapter->Release();
            EarlyLog("DX11-DXVK: System D3D11 not found");
            return false;
        }
        // Redirect system d3d11.dll's dxgi.dll IAT to system dxgi.dll
        RedirectModuleImports(hD3D11, "dxgi.dll", hDXGI);

        typedef HRESULT(WINAPI * PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                          const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                                          D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
        PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice =
            (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
        if (!pD3D11CreateDevice) {
            matchedAdapter->Release();
            return false;
        }

        D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        D3D_FEATURE_LEVEL featureLevel;
        HRESULT hr = pD3D11CreateDevice(matchedAdapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, featureLevels, 2,
                                        D3D11_SDK_VERSION, &ownedDevice, &featureLevel, &ownedContext);
        matchedAdapter->Release();
        if (FAILED(hr)) {
            EarlyLog("DX11-DXVK: Failed to create system D3D11 device (hr=0x%08x)", hr);
            return false;
        }
        EarlyLog("DX11-DXVK: System D3D11 device created for cross-device capture (fl=%d)", featureLevel);
        return true;
    }

    void Init(ID3D11Device* device, IDXGISwapChain* swapChain) {
        if (initialized)
            return;

        if (!cachedSwapChainIdentity) {
            const HRESULT identityHr =
                swapChain ? swapChain->QueryInterface(IID_PPV_ARGS(&cachedSwapChainIdentity)) : E_POINTER;
            if (FAILED(identityHr) || !cachedSwapChainIdentity) {
                EarlyLog("%s Capture Init: failed to retain swapchain identity hr=0x%08X", isDX10Mode ? "DX10" : "DX11",
                         identityHr);
                Cleanup();
                return;
            }
        }

        DXGI_SWAP_CHAIN_DESC desc = {};
        const HRESULT descHr = swapChain ? swapChain->GetDesc(&desc) : E_POINTER;
        if (SUCCEEDED(descHr) && (desc.BufferDesc.Width == 0 || desc.BufferDesc.Height == 0 ||
                                  desc.BufferDesc.Format == DXGI_FORMAT_UNKNOWN)) {
            // Width/height may have been HWND-derived at creation. Resolve the
            // concrete values from the actual buffer before rejecting capture.
            if (isDX10Mode) {
                ID3D10Texture2D* buffer = nullptr;
                if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer))) && buffer) {
                    D3D10_TEXTURE2D_DESC bufferDesc = {};
                    buffer->GetDesc(&bufferDesc);
                    desc.BufferDesc.Width = bufferDesc.Width;
                    desc.BufferDesc.Height = bufferDesc.Height;
                    desc.BufferDesc.Format = bufferDesc.Format;
                    buffer->Release();
                }
            } else {
                ID3D11Texture2D* buffer = nullptr;
                if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer))) && buffer) {
                    D3D11_TEXTURE2D_DESC bufferDesc = {};
                    buffer->GetDesc(&bufferDesc);
                    desc.BufferDesc.Width = bufferDesc.Width;
                    desc.BufferDesc.Height = bufferDesc.Height;
                    desc.BufferDesc.Format = bufferDesc.Format;
                    buffer->Release();
                }
            }
        }
        if (FAILED(descHr) || desc.BufferDesc.Width == 0 || desc.BufferDesc.Height == 0 ||
            desc.BufferDesc.Format == DXGI_FORMAT_UNKNOWN) {
            EarlyLog("%s Capture Init: invalid swapchain description hr=0x%08X size=%ux%u format=%d",
                     isDX10Mode ? "DX10" : "DX11", descHr, desc.BufferDesc.Width, desc.BufferDesc.Height,
                     static_cast<int>(desc.BufferDesc.Format));
            Cleanup();
            return;
        }

        width = desc.BufferDesc.Width;
        height = desc.BufferDesc.Height;
        format = desc.BufferDesc.Format;

        // Determine which device to use for creating textures
        ID3D11Device* captureDevice = device;

        if (isDX10Mode) {
            if (!cachedDevice10) {
                EarlyLog("DX10: Missing cached D3D10 device during capture init");
                return;
            }

            D3D10_TEXTURE2D_DESC texDesc10 = {};
            texDesc10.Width = width;
            texDesc10.Height = height;
            texDesc10.MipLevels = 1;
            texDesc10.ArraySize = 1;
            texDesc10.Format = (DXGI_FORMAT)format;
            texDesc10.SampleDesc.Count = 1;
            texDesc10.Usage = D3D10_USAGE_DEFAULT;
            texDesc10.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
            texDesc10.CPUAccessFlags = 0;
            texDesc10.MiscFlags = D3D10_RESOURCE_MISC_SHARED;

            useFences = false;
            sharedFenceHandle.store(NULL, std::memory_order_release);

            bool success = true;
            for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
                HRESULT hr = cachedDevice10->CreateTexture2D(&texDesc10, NULL, &sharedTextures10[i]);
                if (SUCCEEDED(hr) && sharedTextures10[i]) {
                    IDXGIResource* resource = nullptr;
                    if (SUCCEEDED(sharedTextures10[i]->QueryInterface(IID_PPV_ARGS(&resource))) && resource) {
                        HANDLE hTemp = NULL;
                        hr = resource->GetSharedHandle(&hTemp);
                        sharedTextureHandles[i].store(hTemp, std::memory_order_release);
                        sharedTextureHandlesAreNt[i] = false;
                        resource->Release();
                    } else {
                        EarlyLog("DX10: Error - Failed to query IDXGIResource for texture %d", i);
                        success = false;
                    }

                    if (sharedTextureHandles[i].load(std::memory_order_acquire) == NULL) {
                        EarlyLog("DX10: Critical - Shared handle is NULL for texture %d", i);
                        success = false;
                    }
                } else {
                    success = false;
                    HookLog("DX10: Failed to create shared texture %d (hr=0x%08x)", i, hr);
                }
            }

            if (success) {
                D3D10_QUERY_DESC queryDesc = {};
                queryDesc.Query = D3D10_QUERY_EVENT;
                queryDesc.MiscFlags = 0;

                for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
                    HRESULT queryHr = cachedDevice10->CreateQuery(&queryDesc, &copyQueries10[i]);
                    if (FAILED(queryHr)) {
                        HookLog("DX10: Failed to create copy query %d (hr=0x%08x)", i, queryHr);
                        copyQueries10[i] = nullptr;
                    }
                }
            }

            if (success) {
                if (g_IPC) {
                    PublishToSharedMemory(g_IPC);
                }
                initialized = true;
                HookLogImportant("DX10 Capture Initialized: %dx%d (Fence: OFF, Queries: %s, DXVK: OFF)", width, height,
                                 (copyQueries10[0] != nullptr) ? "ON" : "OFF");
            } else {
                EarlyLog("DX10 Capture Init FAILED (success=false)");
                Cleanup();
            }
            return;
        } else {
            // Get LUID from DX11 device
            IDXGIDevice* dxgiDevice = nullptr;
            if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
                IDXGIAdapter* adapter = nullptr;
                if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                    DXGI_ADAPTER_DESC adapterDesc;
                    adapter->GetDesc(&adapterDesc);
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
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

            // Detect DXVK: if d3d11.dll is not from System32, this is DXVK.
            // DXVK's GetSharedHandle returns Vulkan-internal IDs, not valid Windows
            // NT handles. Fix: create a real system D3D11 device on the same GPU
            // for ring buffer textures, then import them into DXVK for CopyResource.
            if (IsCurrentD3D11DXVK() && !isDXVKMode) {
                EarlyLog("DX11: DXVK d3d11 detected - creating system D3D11 for ring buffer");
                if (CreateSystemD3D11DeviceForLUID(luidLow, luidHigh)) {
                    isDXVKMode = true;
                    captureDevice = ownedDevice;  // Ring buffer textures come from real D3D11
                    // cachedDevice/cachedContext remain as DXVK's for CopyResource
                } else {
                    EarlyLog("DX11: DXVK detected but system D3D11 creation failed, capture may produce bad handles");
                }
            }
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
        useKeyedMutex = false;  // Disabled - using Fence instead

        // Try to create D3D11 Fence for async GPU synchronization (DX11.3+)
        // Skip for DXVK: fence lives in system D3D11 device but copy happens via
        // DXVK context - fence can't be signaled cross-device from DXVK.
        ID3D11Device5* device5 = nullptr;
        D3D11InternalIdentityProbeScope identityProbeScope;
        if (!isDXVKMode && SUCCEEDED(captureDevice->QueryInterface(IID_PPV_ARGS(&device5)))) {
            HRESULT hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence));
            if (SUCCEEDED(hr)) {
                // Get shared fence handle for cross-process
                HANDLE hTemp = NULL;
                hr = fence->CreateSharedHandle(NULL, GENERIC_ALL, NULL, &hTemp);
                sharedFenceHandle.store(hTemp, std::memory_order_release);
                if (SUCCEEDED(hr)) {
                    // Get Context4 for Signal()
                    ID3D11DeviceContext* immCtx = nullptr;
                    captureDevice->GetImmediateContext(&immCtx);
                    if (immCtx && SUCCEEDED(immCtx->QueryInterface(IID_PPV_ARGS(&context4)))) {
                        useFences = true;
                        EarlyLog("DX11: D3D11 Fence created (async GPU sync enabled)");
                    } else {
                        EarlyLog("DX11: Warning - ID3D11DeviceContext4 not available; using implicit shared sync");
                        HANDLE unusableFenceHandle = sharedFenceHandle.exchange(NULL, std::memory_order_acq_rel);
                        if (unusableFenceHandle)
                            CloseHandle(unusableFenceHandle);
                        fence->Release();
                        fence = nullptr;
                    }
                    if (immCtx)
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
                IDXGIResource1* pResource1 = NULL;
                // Use IDXGIResource1 for NT Handles
                if (SUCCEEDED(sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&pResource1)))) {
                    HANDLE hTemp = NULL;
                    hr = pResource1->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                        NULL, &hTemp);
                    sharedTextureHandles[i].store(hTemp, std::memory_order_release);
                    sharedTextureHandlesAreNt[i] = SUCCEEDED(hr) && hTemp != NULL;
                    pResource1->Release();
                } else {
                    // Fallback to legacy KMT if Resource1 not valid (should not happen
                    // with NTHANDLE flag) But if we requested NTHANDLE, GetSharedHandle
                    // (KMT) will fail on some drivers. We should log this specific
                    // failure path.
                    IDXGIResource* pResource = NULL;
                    if (SUCCEEDED(sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&pResource))) &&
                        (texDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED)) {
                        HANDLE hTemp = NULL;
                        pResource->GetSharedHandle(&hTemp);
                        sharedTextureHandles[i].store(hTemp, std::memory_order_release);
                        sharedTextureHandlesAreNt[i] = false;
                        pResource->Release();
                        EarlyLog(
                            "DX11: Warning - Fallback to KMT handle for KeyedMutex "
                            "(NT Handle QI failed)");
                    } else {
                        EarlyLog(
                            "DX11: Error - Failed to get any shared handle interface "
                            "for texture %d",
                            i);
                    }
                }

                if (sharedTextureHandles[i].load() == NULL) {
                    EarlyLog("DX11: Critical - Shared Handle is NULL for texture %d", i);
                    success = false;
                } else {
                    EarlyLog("DX11: Created Texture %d Handle %p", i, sharedTextureHandles[i].load());
                }

            } else {
                // Fallback to legacy shared if NT Handle not supported
                if (hr == E_INVALIDARG && (texDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
                    EarlyLog(
                        "DX11: NT Handle not supported, falling back to legacy "
                        "shared textures");
                    texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
                    i--;  // Retry this index
                    continue;
                }

                success = false;
                HookLog("%s: Failed to create texture %d (hr=0x%08x)", isDX10Mode ? "DX10" : "DX11", i, hr);
            }
        }

        if (success) {
            // For DXVK mode: open each system D3D11 texture in DXVK's device.
            // The copy at capture time will use DXVK's context to copy the game
            // backbuffer into the DXVK-imported texture. The encoder opens the
            // original system D3D11 NT handles normally.
            if (isDXVKMode) {
                ID3D11Device1* dxvkDevice1 = nullptr;
                D3D11InternalIdentityProbeScope identityProbeScope;
                if (SUCCEEDED(cachedDevice->QueryInterface(IID_PPV_ARGS(&dxvkDevice1)))) {
                    for (int i = 0; i < CAPTURE_TEXTURE_COUNT && success; i++) {
                        HANDLE ntHandle = sharedTextureHandles[i].load();
                        if (!ntHandle) {
                            EarlyLog("DX11-DXVK: NT handle %d is NULL, cannot import", i);
                            success = false;
                            break;
                        }
                        HRESULT hr = dxvkDevice1->OpenSharedResource1(ntHandle, IID_PPV_ARGS(&dxvkImportedTextures[i]));
                        if (FAILED(hr)) {
                            EarlyLog("DX11-DXVK: Failed to open texture %d in DXVK device (hr=0x%08x)", i, hr);
                            success = false;
                        }
                    }
                    dxvkDevice1->Release();
                } else {
                    EarlyLog("DX11-DXVK: DXVK device doesn't support ID3D11Device1 - disabling DXVK mode");
                    success = false;
                }
            }
        }

        if (success) {
            // Create GPU synchronization queries for each texture
            // These queries are used to ensure CopyResource completes before reusing
            // the texture. Skip for DXVK: queries live in system D3D11 but context
            // is DXVK's - they can't be used cross-device.
            if (!isDXVKMode) {
                D3D11_QUERY_DESC queryDesc = {};
                queryDesc.Query = D3D11_QUERY_EVENT;
                queryDesc.MiscFlags = 0;

                for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
                    HRESULT queryHr = captureDevice->CreateQuery(&queryDesc, &copyQueries[i]);
                    if (FAILED(queryHr)) {
                        HookLog("%s: Failed to create copy query %d (hr=0x%08x)", isDX10Mode ? "DX10" : "DX11", i,
                                queryHr);
                        copyQueries[i] = nullptr;
                    }
                }
            }

            if (g_IPC) {
                PublishToSharedMemory(g_IPC);
            }
            initialized = true;
            HookLogImportant("%s Capture Initialized: %dx%d (Fence: %s, Queries: %s, DXVK: %s)",
                             isDX10Mode ? "DX10" : "DX11", width, height, useFences ? "ON" : "OFF",
                             (copyQueries[0] != nullptr) ? "ON" : "OFF", isDXVKMode ? "ON" : "OFF");
        } else {
            EarlyLog("%s Capture Init FAILED (success=false)", isDX10Mode ? "DX10" : "DX11");
            Cleanup();
        }
    }

    // Wait for a specific query to complete (with timeout)
    bool WaitForCopy(ID3D11DeviceContext* context, int idx, DWORD timeoutMs = 10) {
        if (!copyQueries[idx])
            return true;  // No query = assume complete
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

    // Get the context to use for DX11 capture operations
    ID3D11DeviceContext* GetCaptureContext() {
        return cachedContext;
    }

    // Capture a frame from the swapchain to shared texture
    // Returns true if frame was captured, false if skipped/dropped
    bool CaptureFrame(IDXGISwapChain* swapChain) {
        std::unique_lock<std::recursive_mutex> captureLock(captureMutex, std::try_to_lock);
        if (!captureLock.owns_lock()) {
            if (g_IPC && g_IPC->GetSharedMem()) {
                g_IPC->GetSharedMem()->runtimeState.injectProducerCaptureLockDrops.fetch_add(1,
                                                                                             std::memory_order_relaxed);
            }
            static std::atomic<int> s_contentionLogCount{0};
            if (s_contentionLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
                HookLog("DX11Capture: Skipping concurrent capture while another Present/cleanup owns resources");
            }
            return false;
        }

        static std::atomic<int> s_captureFrameCount{0};
        int frameNum = s_captureFrameCount.fetch_add(1, std::memory_order_relaxed) + 1;

        if (!swapChain) {
            HookLog("DX11Capture: [%d] swapChain is null", frameNum);
            return false;
        }

        // Check if we should throttle capture (encoder is falling behind)
        if (g_IPC && g_IPC->GetSharedMem()) {
            if (g_IPC->GetSharedMem()->throttleCapture.load(std::memory_order_acquire)) {
                return false;
            }
        }

        // Initialize capture if needed (GetDevice only called during init to avoid per-frame COM overhead)
        if (!initialized) {
            HookLog("DX11Capture: [%d] Not initialized, initializing...", frameNum);
            if (generationResetPending) {
                SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
                if (HasOutstandingCaptureFrameLeases(sharedMem)) {
                    static std::atomic<int> s_generationLeaseLogCount{0};
                    if (s_generationLeaseLogCount.fetch_add(1, std::memory_order_relaxed) < 12) {
                        HookLog("DX11Capture: [%d] Waiting for old frame leases before rebuilding resources", frameNum);
                    }
                    return false;
                }
                Cleanup();
            }
            const DXGIShared::APIType swapChainApi = DetectSwapChainAPITypeForDX11Hook(swapChain);
            if (swapChainApi == DXGIShared::APIType::D3D10) {
                if (!InitDX10(swapChain)) {
                    HookLog("DX11Capture: [%d] InitDX10 failed", frameNum);
                    return false;
                }
                Init(nullptr, swapChain);
            } else if (swapChainApi == DXGIShared::APIType::D3D11) {
                ID3D11Device* device = nullptr;
                HRESULT initHr = swapChain->GetDevice(IID_PPV_ARGS(&device));
                if (FAILED(initHr) || !device) {
                    HookLog("DX11Capture: [%d] GetDevice failed hr=0x%08X", frameNum, initHr);
                    return false;
                }
                Init(device, swapChain);
                device->Release();
            } else {
                HookLog("DX11Capture: [%d] Unsupported swapchain API %s during init", frameNum,
                        GetDX11HookBaseAPIName(swapChainApi));
                return false;
            }
        }

        if (!initialized) {
            HookLog("DX11Capture: [%d] Still not initialized after Init", frameNum);
            return false;
        }

        if (isDX10Mode) {
            if (!cachedDevice10) {
                HookLog("DX10Capture: [%d] cachedDevice10 is null", frameNum);
                return false;
            }

            ID3D10Texture2D* backbuffer10 = nullptr;
            UINT bufferIndex = ResolveDX11BackBufferIndex(swapChain);
            HRESULT hr = swapChain->GetBuffer(bufferIndex, __uuidof(ID3D10Texture2D), (void**)&backbuffer10);
            if (FAILED(hr) || !backbuffer10) {
                HookLog("DX10Capture: [%d] GetBuffer(%u) failed hr=0x%08X", frameNum, bufferIndex, hr);
                return false;
            }

            int writeIdx = writeIndex % CAPTURE_TEXTURE_COUNT;
            SharedMemoryLayout* captureSharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
            uint32_t cpuBusySlots = 0;
            uint32_t gpuBusySlots = 0;
            writeIdx = FindAvailableCaptureTextureSlotIf(
                captureSharedMem, writeIdx, CAPTURE_TEXTURE_COUNT,
                [&](int32_t candidate) {
                    ID3D10Query* query = copyQueries10[candidate];
                    if (!query)
                        return true;
                    BOOL complete = FALSE;
                    return query->GetData(&complete, sizeof(complete), D3D10_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
                },
                &cpuBusySlots, &gpuBusySlots);
            if (writeIdx < 0) {
                if (captureSharedMem) {
                    if (cpuBusySlots != 0)
                        captureSharedMem->runtimeState.injectProducerCpuLeaseBusyDrops.fetch_add(
                            1, std::memory_order_relaxed);
                    if (gpuBusySlots != 0)
                        captureSharedMem->runtimeState.injectProducerGpuBusyDrops.fetch_add(1,
                                                                                            std::memory_order_relaxed);
                }
                droppedFrames.fetch_add(1, std::memory_order_relaxed);
                backbuffer10->Release();
                return false;
            }
            writeIndex.store(writeIdx, std::memory_order_relaxed);
            if (frameNum <= 20 || frameNum % 60 == 0) {
                HookLog("DX10Capture: [%d] Copying to texture %d (writeIndex=%d)", frameNum, writeIdx,
                        writeIndex.load());
            }

            LARGE_INTEGER qpc;
            QueryPerformanceCounter(&qpc);
            int64_t timestamp = qpc.QuadPart;

            cachedDevice10->CopyResource(sharedTextures10[writeIdx], backbuffer10);
            backbuffer10->Release();

            if (copyQueries10[writeIdx]) {
                copyQueries10[writeIdx]->End();
            }

            // D3D10->D3D11 shared-texture interop requires a producer-side Flush()
            // so the media process sees the latest contents of the shared surface.
            cachedDevice10->Flush();

            if (g_IPC) {
                SignalFrameReady(g_IPC, writeIdx, timestamp, 0);
                if (frameNum <= 10) {
                    HookLog("DX10Capture: [%d] Signaled frame ready via IPC (texture=%d)", frameNum, writeIdx);
                }
            } else {
                HookLog("DX10Capture: [%d] g_IPC is NULL - cannot signal frame", frameNum);
            }

            AdvanceWriteIndex();
            return true;
        }

        // Get immediate context for copy
        ID3D11DeviceContext* context = GetCaptureContext();
        if (!context) {
            HookLog("DX11Capture: [%d] GetCaptureContext returned null", frameNum);
            return false;
        }

        ID3D11Texture2D* backbuffer = nullptr;

        // When capture is intentionally ordered after overlay, prefer the RTV
        // resource that overlay rendered to on this frame.
        if (!isDX10Mode && dx11_hook_g_CaptureUsesOverlayRTV && dx11_hook_g_mainRenderTargetView) {
            ID3D11Resource* rtResource = nullptr;
            dx11_hook_g_mainRenderTargetView->GetResource(&rtResource);
            if (rtResource) {
                rtResource->QueryInterface(IID_PPV_ARGS(&backbuffer));
                rtResource->Release();
            }
        }

        // Fallback: resolve backbuffer index directly from swapchain.
        UINT bufferIndex = 0;
        if (!backbuffer) {
            bufferIndex = ResolveDX11BackBufferIndex(swapChain);
            HRESULT hr = swapChain->GetBuffer(bufferIndex, IID_PPV_ARGS(&backbuffer));
            if (FAILED(hr) || !backbuffer) {
                HookLog("DX11Capture: [%d] GetBuffer(%u) failed hr=0x%08X", frameNum, bufferIndex, hr);
                return false;
            }
        }

        // Determine which texture slot to write to
        int writeIdx = writeIndex % CAPTURE_TEXTURE_COUNT;
        SharedMemoryLayout* captureSharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        const UINT64 completedFenceValue = (useFences && fence) ? fence->GetCompletedValue() : 0;
        if (completedFenceValue == UINT64_MAX) {
            HookLog("DX11Capture: Producer fence reported device removal");
            backbuffer->Release();
            return false;
        }
        uint32_t cpuBusySlots = 0;
        uint32_t gpuBusySlots = 0;
        writeIdx = FindAvailableCaptureTextureSlotIf(
            captureSharedMem, writeIdx, CAPTURE_TEXTURE_COUNT,
            [&](int32_t candidate) {
                if (useFences && fence) {
                    const UINT64 requiredValue = slotFenceValues[candidate];
                    return requiredValue == 0 || completedFenceValue >= requiredValue;
                }
                ID3D11Query* query = copyQueries[candidate];
                if (!query)
                    return true;
                BOOL complete = FALSE;
                return context->GetData(query, &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
            },
            &cpuBusySlots, &gpuBusySlots);
        if (writeIdx < 0) {
            if (captureSharedMem) {
                if (cpuBusySlots != 0)
                    captureSharedMem->runtimeState.injectProducerCpuLeaseBusyDrops.fetch_add(1,
                                                                                             std::memory_order_relaxed);
                if (gpuBusySlots != 0)
                    captureSharedMem->runtimeState.injectProducerGpuBusyDrops.fetch_add(1, std::memory_order_relaxed);
            }
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
            backbuffer->Release();
            return false;
        }
        writeIndex.store(writeIdx, std::memory_order_relaxed);

        if (frameNum <= 20 || frameNum % 60 == 0) {
            HookLog("DX11Capture: [%d] Copying to texture %d (writeIndex=%d)", frameNum, writeIdx, writeIndex.load());
        }

        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        int64_t timestamp = qpc.QuadPart;

        // Perform GPU copy: backbuffer -> shared texture
        // For DXVK: copy into the DXVK-imported texture (system D3D11-owned,
        // imported into DXVK's device). The encoder opens the system D3D11 NT handle.
        ID3D11Texture2D* copyTarget =
            (isDXVKMode && dxvkImportedTextures[writeIdx]) ? dxvkImportedTextures[writeIdx] : sharedTextures[writeIdx];
        context->CopyResource(copyTarget, backbuffer);
        backbuffer->Release();

        // Issue query for GPU completion tracking
        if (copyQueries[writeIdx]) {
            context->End(copyQueries[writeIdx]);
        }

        // Signal fence if using D3D11.3 fences
        uint64_t currentFenceValue = 0;
        if (useFences && fence && context4) {
            currentFenceValue = ++fenceValue;
            const HRESULT signalHr = context4->Signal(fence, currentFenceValue);
            if (FAILED(signalHr)) {
                HookLog(
                    "DX11Capture: Fence Signal failed value=%llu hr=0x%08X; falling back to implicit shared "
                    "synchronization",
                    static_cast<unsigned long long>(currentFenceValue), signalHr);
                useFences = false;
                currentFenceValue = 0;
                context->Flush();
                if (cachedDevice && FAILED(cachedDevice->GetDeviceRemovedReason())) {
                    return false;
                }
            } else {
                slotFenceValues[writeIdx] = currentFenceValue;
            }
        } else {
            // A legacy shared texture has no explicit cross-process completion
            // primitive. Submit the copy before publishing its ring entry so the
            // media device cannot indefinitely observe an older texture version.
            context->Flush();
        }

        // Signal frame ready to media process via IPC
        // Note: EnqueueFrame to internal pendingRing is skipped for inject mode
        // since SignalFrameReady writes directly to the shared memory ring buffer
        if (g_IPC) {
            SignalFrameReady(g_IPC, writeIdx, timestamp, currentFenceValue);
            if (frameNum <= 10) {
                HookLog("DX11Capture: [%d] Signaled frame ready via IPC (texture=%d)", frameNum, writeIdx);
            }
        } else {
            HookLog("DX11Capture: [%d] g_IPC is NULL - cannot signal frame", frameNum);
        }

        // Advance write index
        AdvanceWriteIndex();

        return true;
    }
};

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
inline DX11Capture dx11_hook_g_DX11Capture;

inline OverlayConfig GetActiveDX11OverlayConfig(SharedMemoryLayout* shm) {
    OverlayConfig cfg{};
    cfg.captureIncludeOverlay = true;
    cfg.screenshotIncludeOverlay = true;
    if (shm) {
        cfg = shm->ReadOverlayConfig();
    }
    return cfg;
}

inline void CaptureDX11Screenshot(IDXGISwapChain* pSwapChain, SharedMemoryLayout* shm, uint64_t requestId) {
    bool queued = false;
    ID3D11Texture2D* backbuffer = nullptr;
    UINT bbIdx = ResolveDX11BackBufferIndex(pSwapChain);
    pSwapChain->GetBuffer(bbIdx, IID_PPV_ARGS(&backbuffer));
    if (backbuffer) {
        ID3D11Device* device = nullptr;
        backbuffer->GetDevice(&device);
        if (device) {
            ID3D11DeviceContext* context = nullptr;
            device->GetImmediateContext(&context);
            if (context) {
                D3D11_TEXTURE2D_DESC textureDesc{};
                backbuffer->GetDesc(&textureDesc);
                const auto presentationEncoding =
                    DXGIShared::ResolveSwapChainPresentationEncoding(pSwapChain, textureDesc.Format);
                queued = SaveD3D11TextureAsScreenshotRaw(device, context, backbuffer, shm, requestId,
                                                         presentationEncoding);
                context->Release();
            }
            device->Release();
        }
        backbuffer->Release();
    }
    if (!queued)
        CompleteScreenshotRequest(shm, requestId, ScreenshotRequestStatus::Failed, ERROR_READ_FAULT);
}

inline void CaptureDX10Screenshot(IDXGISwapChain* pSwapChain, SharedMemoryLayout* shm, uint64_t requestId) {
    ID3D10Device* device = nullptr;
    if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D10Device), (void**)&device))) {
        ID3D10Device1* device10_1 = nullptr;
        if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D10Device1), (void**)&device10_1))) {
            CompleteScreenshotRequest(shm, requestId, ScreenshotRequestStatus::Failed, ERROR_NOT_SUPPORTED);
            return;
        }
        device = device10_1;
    }

    bool queued = false;
    ID3D10Texture2D* backbuffer = nullptr;
    UINT bbIdx = ResolveDX11BackBufferIndex(pSwapChain);
    if (SUCCEEDED(pSwapChain->GetBuffer(bbIdx, __uuidof(ID3D10Texture2D), (void**)&backbuffer))) {
        D3D10_TEXTURE2D_DESC bbDesc;
        backbuffer->GetDesc(&bbDesc);

        D3D10_TEXTURE2D_DESC stagingDesc = bbDesc;
        stagingDesc.Usage = D3D10_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;

        ID3D10Texture2D* staging = nullptr;
        if (SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))) {
            device->CopyResource(staging, backbuffer);
            D3D10_MAPPED_TEXTURE2D mapped;
            if (SUCCEEDED(staging->Map(0, D3D10_MAP_READ, 0, &mapped))) {
                ScreenshotPixelFormat pixelFormat = ScreenshotPixelFormat::BGRA8;
                ScreenshotColorEncoding colorEncoding = ScreenshotColorEncoding::SRGB;
                const auto presentationEncoding =
                    DXGIShared::ResolveSwapChainPresentationEncoding(pSwapChain, bbDesc.Format);
                if (bbDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || bbDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                    pixelFormat = ScreenshotPixelFormat::RGBA8;
                } else if (bbDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
                    pixelFormat = ScreenshotPixelFormat::R10G10B10A2;
                    colorEncoding = presentationEncoding == ce::presentation_color::Encoding::Hdr10Pq
                                        ? ScreenshotColorEncoding::BT2020_PQ
                                        : ScreenshotColorEncoding::BT709_G22;
                } else if (bbDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                    pixelFormat = ScreenshotPixelFormat::RGBA16F;
                    colorEncoding = ScreenshotColorEncoding::LinearScRGB;
                }
                if (presentationEncoding != ce::presentation_color::Encoding::Unsupported) {
                    queued = QueueScreenshotPixels(shm, requestId, static_cast<const uint8_t*>(mapped.pData),
                                                   bbDesc.Width, bbDesc.Height, mapped.RowPitch, pixelFormat,
                                                   colorEncoding);
                }
                staging->Unmap(0);
            }
            staging->Release();
        }
        backbuffer->Release();
    }

    device->Release();
    if (!queued)
        CompleteScreenshotRequest(shm, requestId, ScreenshotRequestStatus::Failed, ERROR_READ_FAULT);
}

inline void CaptureRequestedDX11Screenshot(IDXGISwapChain* pSwapChain, SharedMemoryLayout* shm, uint64_t requestId) {
    if (!shm || requestId == 0)
        return;

    const DXGIShared::APIType swapChainApi = DetectSwapChainAPITypeForDX11Hook(pSwapChain);
    if (swapChainApi == DXGIShared::APIType::D3D10) {
        CaptureDX10Screenshot(pSwapChain, shm, requestId);
        return;
    }

    if (swapChainApi != DXGIShared::APIType::D3D11) {
        HookLog("DX11 Screenshot: Unsupported swapchain API %s", GetDX11HookBaseAPIName(swapChainApi));
        CompleteScreenshotRequest(shm, requestId, ScreenshotRequestStatus::Failed, ERROR_NOT_SUPPORTED);
        return;
    }

    CaptureDX11Screenshot(pSwapChain, shm, requestId);
}

inline void ProcessDX11FrameWithOverlayOrdering(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return;

    UINT frameBufferIndex = ResolveDX11BackBufferIndex(pSwapChain);
    dx11_hook_g_ForcedCaptureBackBufferIndex = static_cast<int>(frameBufferIndex);
    auto indexGuard = ce::make_scope_guard([]() { dx11_hook_g_ForcedCaptureBackBufferIndex = -1; });

    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    DXGI_SWAP_CHAIN_DESC swapChainDesc{};
    if (shm) {
        auto presentationEncoding = ce::presentation_color::Encoding::Unsupported;
        if (SUCCEEDED(pSwapChain->GetDesc(&swapChainDesc))) {
            presentationEncoding =
                DXGIShared::ResolveSwapChainPresentationEncoding(pSwapChain, swapChainDesc.BufferDesc.Format);
        }
        shm->SetIsHDR(ce::presentation_color::IsHDR(presentationEncoding));
    }
    OverlayConfig overlayCfg = GetActiveDX11OverlayConfig(shm);
    const bool shouldDrawOverlay = shm && overlayCfg.showOverlay;
    const bool captureAfterOverlay = shouldDrawOverlay && overlayCfg.captureIncludeOverlay;
    const uint64_t screenshotRequestId = GetPendingScreenshotRequestId(shm);
    const bool screenshotRequested = screenshotRequestId != 0;
    const bool screenshotAfterOverlay = shouldDrawOverlay && overlayCfg.screenshotIncludeOverlay;

    auto doCapture = [&](bool afterOverlay) {
        if (g_IPC && g_IPC->IsRecording() && !ShouldSkipCaptureForTargetCadence(shm, "DX11")) {
            dx11_hook_g_CaptureUsesOverlayRTV = afterOverlay;
            auto captureGuard = ce::make_scope_guard([]() { dx11_hook_g_CaptureUsesOverlayRTV = false; });
            dx11_hook_g_DX11Capture.CaptureFrame(pSwapChain);
        }
    };

    auto doScreenshot = [&]() {
        if (screenshotRequested) {
            CaptureRequestedDX11Screenshot(pSwapChain, shm, screenshotRequestId);
        }
    };

    if (!captureAfterOverlay) {
        doCapture(false);
    }
    if (screenshotRequested && !screenshotAfterOverlay) {
        doScreenshot();
    }
    if (shouldDrawOverlay) {
        DrawDX11Overlay(pSwapChain);
    }
    if (captureAfterOverlay) {
        doCapture(true);
    }
    if (screenshotRequested && screenshotAfterOverlay) {
        doScreenshot();
    }
}
