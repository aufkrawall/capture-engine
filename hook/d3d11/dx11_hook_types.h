#pragma once

class D3D11InternalIdentityProbeScope;

struct D3D11ContextVTableOriginals;

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

#include "common/capture/frame_timing.h"

#include "common/platform/raii_helpers.h"

#include "hook/capture/capture_base.h"

#include "hook/pacing/capture_pacing.h"

#include "hook/runtime/deferred_release.h"

#include "hook/runtime/dll_utils.h"

#include "hook/fg/fg_detection.h"

#include "hook/pacing/fps_limiter.h"

#include "hook/runtime/graphics_api_identity.h"

#include "hook/runtime/hook_common.h"

#include "hook/d3d11/d3d11_context_dispatch.h"

#include "hook/d3d11/dx11_back_buffer_view_cache.h"

#include "hook/overlay/overlay_adapter.h"

#include "hook/overlay/overlay_compat.h"

#include "hook/overlay/overlay_metrics_publisher.h"

#include "hook/metrics/perf_logger.h"

#include "hook/overrides/sampler_override_utils.h"

#include "hook/capture/screenshot_hook.h"

#include "hook/metrics/system_metrics.h"

#include "hook/wrappers/dxgi_swapchain_wrap.h"

#include "hook/hooking/iat_hook.h"

#include "hook/wrappers/wrapper_base.h"

#include "dx11_hook.h"

#include "hook/d3d12/dx12_hook.h"

#include "hook/present/dxgi_shared.h"

#include "hook/runtime/graphics_hook.h"

#include "hook/ddraw/lod_helper.h"

#include "hook/metrics/performance_metrics.h"

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


#include "hook/hooking/custom_hook.h"

#include "hook/wrappers/d3d11_devicecontext_wrap.h"

#include "hook/hooking/iat_hook.h"

#include "hook/hooking/vtable_hook.h"

#include "hook/wrappers/wrapper_base.h"

extern thread_local unsigned g_D3D11InternalIdentityProbeDepth;

typedef HRESULT(STDMETHODCALLTYPE* D3D11QueryInterface_t)(IUnknown*, REFIID, void**);

typedef HRESULT(STDMETHODCALLTYPE* ResizeBuffers_t)(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width,
                                                    UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);

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

typedef void(STDMETHODCALLTYPE* ClearState11_t)(ID3D11DeviceContext* pContext);

typedef HRESULT(STDMETHODCALLTYPE* FinishCommandList11_t)(ID3D11DeviceContext* pContext,
                                                          BOOL RestoreDeferredContextState,
                                                          ID3D11CommandList** ppCommandList);

typedef void(STDMETHODCALLTYPE* SwapDeviceContextState11_t)(ID3D11DeviceContext1* pContext,
                                                            ID3DDeviceContextState* pState,
                                                            ID3DDeviceContextState** ppPreviousState);

typedef void(STDMETHODCALLTYPE* PSGetSamplers11_t)(ID3D11DeviceContext* pContext, UINT StartSlot, UINT NumSamplers,
                                                   ID3D11SamplerState** ppSamplers);

// D3D10 CreateSamplerState hook
typedef HRESULT(STDMETHODCALLTYPE* CreateSamplerState10_t)(ID3D10Device* pDevice,
                                                           const D3D10_SAMPLER_DESC* pSamplerDesc,
                                                           ID3D10SamplerState** ppSamplerState);

#include <algorithm>

#include <atomic>

#include <shared_mutex>

#include <unordered_map>

#include <unordered_set>


// Type definitions moved out of dx11_hook_internal.h so every unit stays <= 800 lines.
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

void DX11Hook_ReportApiUse(ID3D11Device* device, unsigned minorVersion, const char* evidence, const void* caller,
                           const void* runtimeCode);

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

// Filters the frame this Present will put on screen, before capture and
// before the overlay draws. See hook/d3d11/dx11_hook_sharpen.cpp.
void SharpenDX11PresentedFrame(IDXGISwapChain* pSwapChain);

void ReleaseDX11SharpenResources(bool releaseObjects = true);

bool DX11Hook_ShouldPassThroughCurrentPresent();

void ApplyDeferredSamplerOverrides11(IDXGISwapChain* pSwapChain);

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

// The view the overlay drew on this Present; dx11_hook_g_overlayViews owns the reference.
inline ID3D11RenderTargetView* dx11_hook_g_mainRenderTargetView = NULL;

inline IDXGISwapChain* dx11_hook_g_D3D11IdentitySwapChain = NULL;

inline ID3D11Device* dx11_hook_g_D3D11IdentityDevice = NULL;

// The D3D10 overlay's view this Present; dx11_hook_g_overlayViews10 owns the reference.
inline ID3D10RenderTargetView* dx11_hook_g_mainRenderTargetView10 = NULL;

inline BackBufferViewCache<ID3D11RenderTargetView> dx11_hook_g_overlayViews;

inline BackBufferViewCache<ID3D10RenderTargetView> dx11_hook_g_overlayViews10;

// Drops every retained overlay view. `release` false only forgets them: the device is already
// being torn down and a Release could crash.
inline void ReleaseDX11OverlayViews(bool release) {
    if (release) {
        dx11_hook_g_overlayViews.Clear([](ID3D11RenderTargetView* view) { view->Release(); });
        dx11_hook_g_overlayViews10.Clear([](ID3D10RenderTargetView* view) { view->Release(); });
    } else {
        dx11_hook_g_overlayViews.Clear([](ID3D11RenderTargetView*) {});
        dx11_hook_g_overlayViews10.Clear([](ID3D10RenderTargetView*) {});
    }
    dx11_hook_g_mainRenderTargetView = NULL;
    dx11_hook_g_mainRenderTargetView10 = NULL;
}

inline const char* dx11_hook_g_DetectedAPI = "DX11";

inline std::mutex dx11_hook_g_GraphicsApiIdentityMutex;

inline std::unordered_map<ID3D10Device*, bool> dx11_hook_g_D3D10DeviceIdentities;

inline std::unordered_map<IDXGISwapChain*, bool> dx11_hook_g_D3D10SwapChainIdentities;

inline ce::graphics_api_identity::ScopedIdentityRegistry<unsigned> dx11_hook_g_D3D11MinorUse;

class D3D11InternalIdentityProbeScope {
public:
    D3D11InternalIdentityProbeScope() {
        DX11Hook_BeginInternalIdentityProbe();
    }~D3D11InternalIdentityProbeScope();
};
bool ResolveD3D10Is10_1(ID3D10Device* device, IDXGISwapChain* swapChain);
unsigned ResolveD3D11MinorUse(ID3D11Device* device);

inline std::unordered_map<void**, D3D11QueryInterface_t> dx11_hook_g_D3D11QueryInterfaceOriginals;
unsigned D3D11DeviceMinorFromIID(REFIID iid);
unsigned D3D11ContextMinorFromIID(REFIID iid);
HRESULT STDMETHODCALLTYPE DetourD3D11QueryInterface(IUnknown* object, REFIID iid, void** result);
void InstallD3D11IdentityQueryHook(IUnknown* object, const char* source);

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

// Capture can optionally reuse the RTV that overlay rendered to on this frame.
// Leave this off when capture must happen before overlay.
inline thread_local bool dx11_hook_g_CaptureUsesOverlayRTV = false;
inline thread_local bool dx11_hook_g_UnsafeSwapChainObserved = false;

// DX11 runtimes can expose D3D10 compatibility interfaces on the same swapchain.
// Prefer the highest actual device API so DX11 swapchains do not fall into the
// DX10 overlay/capture paths just because ID3D10 queries happen to succeed.
DXGIShared::APIType DetectSwapChainAPITypeForDX11Hook(IDXGISwapChain* swapChain);
const char* GetDX11HookBaseAPIName(DXGIShared::APIType api);
bool IsDeferredAFBootstrapped11(ID3D11DeviceContext* context);
void MarkDeferredAFBootstrapped11(ID3D11DeviceContext* context);
bool ApplyDX11BackbufferCountOverride(DXGI_SWAP_CHAIN_DESC& desc, const char* source, bool* ceAddedWaitableOut = nullptr);
bool ApplyDX11BackbufferCountOverride(DXGI_SWAP_CHAIN_DESC1& desc, const char* source, bool* ceAddedWaitableOut = nullptr);
bool IsDXVKD3D10OrD3D11Loaded();
const char* GetDX11HookOverlayAPIName(DXGIShared::APIType api);
bool IsUnityProcess();


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

inline ClearState11_t dx11_hook_oClearState11 = NULL;

inline FinishCommandList11_t dx11_hook_oFinishCommandList11 = NULL;

inline SwapDeviceContextState11_t dx11_hook_oSwapDeviceContextState11 = NULL;

inline PSGetSamplers11_t dx11_hook_oPSGetSamplers11 = NULL;

inline SetShaderResources11_t dx11_hook_oPSSetShaderResources11 = NULL;

inline SetSamplers11_t dx11_hook_oPSSetSamplers11 = NULL;

struct D3D11ContextVTableOriginals {
    SetShaderResources11_t psSetShaderResources = nullptr;
    PSSetShader11_t psSetShader = nullptr;
    SetSamplers11_t psSetSamplers = nullptr;
    DrawIndexed11_t drawIndexed = nullptr;
    Draw11_t draw = nullptr;
    DrawIndexedInstanced11_t drawIndexedInstanced = nullptr;
    DrawInstanced11_t drawInstanced = nullptr;
    DrawAuto11_t drawAuto = nullptr;
    DrawIndexedInstancedIndirect11_t drawIndexedInstancedIndirect = nullptr;
    DrawInstancedIndirect11_t drawInstancedIndirect = nullptr;
    ExecuteCommandList11_t executeCommandList = nullptr;
    ClearState11_t clearState = nullptr;
    FinishCommandList11_t finishCommandList = nullptr;
    SwapDeviceContextState11_t swapDeviceContextState = nullptr;
    PSGetSamplers11_t psGetSamplers = nullptr;
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
    // A context CE dispatches: whatever the runtime's own table holds right now, which may be
    // a variant the runtime switched to since the last call (d3d11_context_dispatch.h).
    if (void** runtime = ce::d3d11_dispatch::RuntimeTable(context)) {
        return reinterpret_cast<Fn>(runtime[slot]);
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
        lock.unlock();
        // An adopted context whose vtable pointer another tool replaced with a copy of CE's table.
        if (void** runtime = ce::d3d11_dispatch::RuntimeTableOfAdopted(context)) {
            return reinterpret_cast<Fn>(runtime[slot]);
        }
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

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
inline std::shared_mutex dx11_hook_g_SamplerCacheMutex11;

inline uint64_t dx11_hook_g_SamplerConfigHash11 = 0;

inline std::atomic<uint64_t> dx11_hook_g_SamplerConfigHash11Fast{0};

// Advances with every sampler policy change and host disconnect, invalidating every memoized
// forced-AF choice (ce::d3d11_af::SamplerChoice) and replacement link. Starts at 1: epoch 0 means "unresolved".
inline std::atomic<uint64_t> dx11_hook_g_SamplerCacheEpoch11{1};

inline thread_local bool dx11_hook_g_InOverlayRender = false;
bool SameSamplerDesc11(const D3D11_SAMPLER_DESC& a, const D3D11_SAMPLER_DESC& b);
void AdvanceSamplerPolicyEpoch11Unlocked();
void EnsureSamplerCacheFresh11(const GraphicsConfig& gfx);
void EnsureSamplerCacheFreshForHash11(uint64_t configHash);
// The active config plus the replacement-cache epoch it is valid for. Hashes the
// sampler settings only when the shared config version changes.
struct SamplerPolicySnapshot11 {
    const GraphicsConfig* gfx = nullptr;
    uint64_t epoch = 0;
    uint64_t hash = 0;  // HashSamplerOverrideConfig(*gfx)
};
SamplerPolicySnapshot11 AcquireSamplerPolicy11();
