#pragma once
#include <d3d10.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>  // For DXGI_PRESENT_PARAMETERS
#include "common/config/config.h"
#include "hook/runtime/graphics_hook.h"

class DX11Hook : public GraphicsHook {
public:
    void Init() override;
    void Shutdown() override;
    void OnHostDisconnect() override;  // Called when captureengine disconnects
    void ProcessDeferredReleases();    // Cleanup resources on background thread
};

// D3D11 function typedefs for IAT patching
typedef HRESULT(WINAPI* PFN_D3D11CreateDeviceAndSwapChain)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                           const D3D_FEATURE_LEVEL*, UINT, UINT,
                                                           const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
                                                           ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

// Detour function - called when game calls D3D11CreateDeviceAndSwapChain
HRESULT WINAPI DX11_DetourCreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
                                                   UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels,
                                                   UINT FeatureLevels, UINT SDKVersion,
                                                   const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                                   IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
                                                   D3D_FEATURE_LEVEL* pFeatureLevel,
                                                   ID3D11DeviceContext** ppImmediateContext);

// Manual Hook Activation (for DXGI/DX12 interop fallbacks)
void DX11Hook_OnSwapChainCreated(IDXGISwapChain* pSwapChain);

// Install vtable hooks on D3D11 device and context immediately (for wrapper path)
void DX11Hook_InstallDeviceAndContextHooks(ID3D11Device* pDevice, ID3D11DeviceContext* pContext,
                                           IDXGISwapChain* pSwapChain);
void DX10Hook_RegisterDeviceIdentity(ID3D10Device* device, bool is10_1, const char* evidence);
void DX10Hook_RegisterSwapChainIdentity(IDXGISwapChain* swapChain, bool is10_1, const char* evidence);
void DX11Hook_RegisterDeviceIdentity(ID3D11Device* device, const char* evidence, bool newDevice = false);
void DX11Hook_ReportApiUse(ID3D11Device* device, unsigned minorVersion, const char* evidence);
// Hook service thread: logs the raw-context forced-AF cost once per 10 s window with activity.
void DX11Hook_ReportSamplerCostIfDue();
void DX11Hook_BeginInternalIdentityProbe();
void DX11Hook_EndInternalIdentityProbe();

// Update metrics for wrapper calls
#include <cstdint>
void DX11_UpdatePerformanceMetrics(int64_t qpcUs);

// Original function pointer (set by IAT patching)
extern PFN_D3D11CreateDeviceAndSwapChain oD3D11CreateDeviceAndSwapChain;

// CPU prerender limit for D3D10/11 swapchains, before the Present is forwarded
// (dx11_hook_prerender.cpp). A D3D11 limit of 1-6 leaves a fence marker owed by this thread:
// D3D11PrerenderAfterPresent signals it once the Present returned (D3D11PrerenderPresentScope).
void ApplyPrerenderLimit(IDXGISwapChain* pSwapChain, float limit);
bool D3D11PrerenderSignalPending();
void D3D11PrerenderAfterPresent();
// The wait of this thread's last ApplyPrerenderLimit, for the frame's perf CSV row; reading resets it.
int32_t TakeD3D11PrerenderWaitUs();
void ReleaseD3D11PrerenderState();

// Marks wrapper-to-real ID3D11DeviceContext forwarding so vtable hooks can
// bypass duplicate override tracking on the same real context.
void DX11Hook_BeginWrapperContextForwarding();
void DX11Hook_EndWrapperContextForwarding();
bool DX11Hook_IsWrapperContextForwarding();

void DX11Hook_BeginWrapperSamplerForwarding();
void DX11Hook_EndWrapperSamplerForwarding();
bool DX11Hook_IsWrapperSamplerForwarding();
bool DX11Hook_ApplySamplerOverrides(D3D11_SAMPLER_DESC& desc, const GraphicsConfig& gfx, bool allowAnisotropicOverride);
bool DX10Hook_ApplySamplerOverrides(D3D10_SAMPLER_DESC& desc, const GraphicsConfig& gfx);
