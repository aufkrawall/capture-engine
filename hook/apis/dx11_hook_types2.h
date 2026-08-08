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

// Additional type definitions moved out of dx11_hook_internal.h (second page).
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
void MarkPixelSamplersDirty11Locked(D3D11PerContextState& state, uint32_t mask);size_t GetStageIndex(D3D11ShaderStage stage);const char* GetStageName11(D3D11ShaderStage stage);uint32_t SamplerRangeMask11(UINT startSlot, UINT numSamplers);uint32_t TrackedPixelSamplerMask11Locked(const D3D11PerContextState& state);uint32_t PixelSamplerDirtyMaskForResourceRange11Locked(const D3D11PerContextState& state, UINT startSlot,
                                                              UINT numViews);void GetStageShaderResources11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot,
                                      UINT numViews, ID3D11ShaderResourceView** views);void GetStageSamplers11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot, UINT numSamplers,
                               ID3D11SamplerState** samplers);void ReleaseTrackedContextState11(D3D11PerContextState& state);void ReleaseTrackedShaderResources11Unlocked();void ClearTrackedContextState11(ID3D11DeviceContext* context);void ReleaseTrackedShaderResources11();void UpdateStageShaderResources(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot,
                                       UINT numViews, ID3D11ShaderResourceView* const* ppShaderResourceViews);uint32_t UpdateStageSamplers(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot,
                                    UINT numSamplers, ID3D11SamplerState* const* ppSamplers);void RememberRealSampler11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT slot,
                                  ID3D11SamplerState* sampler);void RememberRealSamplerRange11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot,
                                       UINT numSamplers, ID3D11SamplerState* const* ppSamplers);ID3D11SamplerState* GetRememberedRealSampler11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT slot);uint32_t PeekPixelSamplerDirtyMask11(ID3D11DeviceContext* context, uint32_t slotMask);void ClearPixelSamplerDirtyMask11(ID3D11DeviceContext* context, uint32_t slotMask);ID3D11ShaderResourceView* GetTrackedShaderResourceView11(ID3D11DeviceContext* context, D3D11ShaderStage stage,
                                                                UINT slot);ID3D11SamplerState* GetTrackedSampler11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT slot);void UpdateTrackedPixelShader11(ID3D11DeviceContext* context, ID3D11PixelShader* shader);uint32_t ConsumePixelSamplerDirtyMask11(ID3D11DeviceContext* context);bool GetTrackedPixelShaderMetadata11(ID3D11DeviceContext* context, bool* hasShader,
                                            WrapperPixelShaderAFMetadata* metadata);void RefreshPixelShaderFromContext11(ID3D11DeviceContext* context);void RefreshStageShaderResourcesFromContext11(ID3D11DeviceContext* context, D3D11ShaderStage stage,
                                                     UINT startSlot, UINT numViews);void RefreshStageSamplersFromContext11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot,
                                              UINT numSamplers);ce::sampler_override::D3D11ForcedAFResourceDecision ClassifyViewForForcedAF11(
    ID3D11Device* device, ID3D11ShaderResourceView* view,
    ce::sampler_override::D3D11Texture2DForcedAFInfo* outInfo = nullptr);bool SamplerAllowsForcedAF(const D3D11_SAMPLER_DESC& desc, const GraphicsConfig& gfx);bool ShouldForceAnisotropyForStageSlot(ID3D11Device* device, ID3D11DeviceContext* context,
                                              D3D11ShaderStage stage, UINT slot, const D3D11_SAMPLER_DESC& desc,
                                              const GraphicsConfig& gfx);ID3D11SamplerState* GetOrCreateReplacementSampler11(ID3D11DeviceContext* context, D3D11ShaderStage stage,
                                                           UINT slot, ID3D11SamplerState* original);int ReconcileStageSamplers11(SetSamplers11_t originalFn, ID3D11DeviceContext* context, D3D11ShaderStage stage,
                                    UINT startSlot, UINT numSlots, uint32_t slotMask);void SetSamplersWithOverrides11(SetSamplers11_t originalFn, ID3D11DeviceContext* context, D3D11ShaderStage stage,
                                       UINT startSlot, UINT numSamplers, ID3D11SamplerState* const* ppSamplers);

void InstallContextVTableHooks11(ID3D11DeviceContext* context, const char* source);HRESULT STDMETHODCALLTYPE DetourCreatePixelShader11(ID3D11Device* device, const void* shaderBytecode,
                                                           SIZE_T bytecodeLength, ID3D11ClassLinkage* classLinkage,
                                                           ID3D11PixelShader** pixelShader);HRESULT STDMETHODCALLTYPE DetourCreateDeferredContext11(ID3D11Device* device, UINT contextFlags,
                                                               ID3D11DeviceContext** deferredContext);void STDMETHODCALLTYPE DetourPSSetShader11(ID3D11DeviceContext* context, ID3D11PixelShader* pixelShader,
                                                  ID3D11ClassInstance* const* classInstances, UINT numClassInstances);void STDMETHODCALLTYPE DetourPSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);void STDMETHODCALLTYPE DetourVSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);void STDMETHODCALLTYPE DetourGSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);void STDMETHODCALLTYPE DetourHSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);void STDMETHODCALLTYPE DetourDSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);void STDMETHODCALLTYPE DetourCSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);void STDMETHODCALLTYPE DetourPSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);void STDMETHODCALLTYPE DetourVSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);void STDMETHODCALLTYPE DetourGSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);void STDMETHODCALLTYPE DetourHSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);void STDMETHODCALLTYPE DetourDSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);void STDMETHODCALLTYPE DetourCSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);void ReconcilePixelSamplersBeforeDraw11(ID3D11DeviceContext* context);void STDMETHODCALLTYPE DetourDrawIndexed11(ID3D11DeviceContext* context, UINT indexCount,
                                                  UINT startIndexLocation, INT baseVertexLocation);void STDMETHODCALLTYPE DetourDraw11(ID3D11DeviceContext* context, UINT vertexCount, UINT startVertexLocation);void STDMETHODCALLTYPE DetourDrawIndexedInstanced11(ID3D11DeviceContext* context, UINT indexCountPerInstance,
                                                           UINT instanceCount, UINT startIndexLocation,
                                                           INT baseVertexLocation, UINT startInstanceLocation);void STDMETHODCALLTYPE DetourDrawInstanced11(ID3D11DeviceContext* context, UINT vertexCountPerInstance,
                                                    UINT instanceCount, UINT startVertexLocation,
                                                    UINT startInstanceLocation);void STDMETHODCALLTYPE DetourDrawAuto11(ID3D11DeviceContext* context);void STDMETHODCALLTYPE DetourDrawIndexedInstancedIndirect11(ID3D11DeviceContext* context,
                                                                   ID3D11Buffer* bufferForArgs,
                                                                   UINT alignedByteOffsetForArgs);void STDMETHODCALLTYPE DetourDrawInstancedIndirect11(ID3D11DeviceContext* context, ID3D11Buffer* bufferForArgs,
                                                            UINT alignedByteOffsetForArgs);void STDMETHODCALLTYPE DetourExecuteCommandList11(ID3D11DeviceContext* context, ID3D11CommandList* commandList,
                                                         BOOL restoreContextState);

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
VSyncOverride GetDX11VSyncOverride();bool ShouldSkipWindowForNvPresent(HWND hwnd);

void ProcessDX11FrameWithOverlayOrdering(IDXGISwapChain* pSwapChain);

void InstallVTableHooks(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, IDXGISwapChain* pSwapChain);HRESULT WINAPI DetourD3D11CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType,
                                                          HMODULE Software, UINT Flags,
                                                          const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
                                                          UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                                          IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
                                                          D3D_FEATURE_LEVEL* pFeatureLevel,
                                                          ID3D11DeviceContext** ppImmediateContext);HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory* pFactory, IUnknown* pDevice,
                                                       DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain);HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd,
                                                              const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                              const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                                              IDXGIOutput* pRestrictToOutput,
                                                              IDXGISwapChain1** ppSwapChain);

HRESULT STDMETHODCALLTYPE DetourCreateSamplerState(ID3D11Device* pDevice, const D3D11_SAMPLER_DESC* pSamplerDesc,
                                                          ID3D11SamplerState** ppSamplerState);

HRESULT STDMETHODCALLTYPE DetourCreatePixelShader11(ID3D11Device* device, const void* shaderBytecode,
                                                           SIZE_T bytecodeLength, ID3D11ClassLinkage* classLinkage,
                                                           ID3D11PixelShader** pixelShader);

HRESULT STDMETHODCALLTYPE DetourCreateDeferredContext11(ID3D11Device* device, UINT contextFlags,
                                                               ID3D11DeviceContext** deferredContext);

void STDMETHODCALLTYPE DetourPSSetShader11(ID3D11DeviceContext* context, ID3D11PixelShader* pixelShader,
                                                  ID3D11ClassInstance* const* classInstances, UINT numClassInstances);

void STDMETHODCALLTYPE DetourDrawIndexed11(ID3D11DeviceContext* context, UINT indexCount,
                                                  UINT startIndexLocation, INT baseVertexLocation);

void STDMETHODCALLTYPE DetourDraw11(ID3D11DeviceContext* context, UINT vertexCount, UINT startVertexLocation);

void STDMETHODCALLTYPE DetourDrawIndexedInstanced11(ID3D11DeviceContext* context, UINT indexCountPerInstance,
                                                           UINT instanceCount, UINT startIndexLocation,
                                                           INT baseVertexLocation, UINT startInstanceLocation);

void STDMETHODCALLTYPE DetourDrawInstanced11(ID3D11DeviceContext* context, UINT vertexCountPerInstance,
                                                    UINT instanceCount, UINT startVertexLocation,
                                                    UINT startInstanceLocation);

void STDMETHODCALLTYPE DetourDrawAuto11(ID3D11DeviceContext* context);

void STDMETHODCALLTYPE DetourDrawIndexedInstancedIndirect11(ID3D11DeviceContext* context,
                                                                   ID3D11Buffer* bufferForArgs,
                                                                   UINT alignedByteOffsetForArgs);

void STDMETHODCALLTYPE DetourDrawInstancedIndirect11(ID3D11DeviceContext* context, ID3D11Buffer* bufferForArgs,
                                                            UINT alignedByteOffsetForArgs);

void STDMETHODCALLTYPE DetourExecuteCommandList11(ID3D11DeviceContext* context, ID3D11CommandList* commandList,
                                                         BOOL restoreContextState);

void STDMETHODCALLTYPE DetourPSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);

void STDMETHODCALLTYPE DetourVSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);

void STDMETHODCALLTYPE DetourGSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);

void STDMETHODCALLTYPE DetourHSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);

void STDMETHODCALLTYPE DetourDSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);

void STDMETHODCALLTYPE DetourCSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);

void STDMETHODCALLTYPE DetourPSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);

void STDMETHODCALLTYPE DetourVSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);

void STDMETHODCALLTYPE DetourGSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);

void STDMETHODCALLTYPE DetourHSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);

void STDMETHODCALLTYPE DetourDSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);

void STDMETHODCALLTYPE DetourCSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);

HRESULT STDMETHODCALLTYPE DetourCreateSamplerState10(ID3D10Device* pDevice,
                                                            const D3D10_SAMPLER_DESC* pSamplerDesc,
                                                            ID3D10SamplerState** ppSamplerState);

inline void LogVTableHookInstalled11(const char* name, bool additionalVtable, UINT index) {
    if (additionalVtable) {
        HookLog("DX11: %s hook installed on additional vtable (slot=%u)", name, index);
    } else {
        HookLog("DX11: %s hook installed (slot=%u)", name, index);
    }
}

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
            LogVTableHookInstalled11(name, knownOriginal != nullptr, index);
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
}void InstallContextVTableHooks11(ID3D11DeviceContext* context, const char* source);

// Helper to install vtable hooks
void InstallVTableHooks(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, IDXGISwapChain* pSwapChain);

inline HWND dx11_hook_g_CachedHwnd = NULL;

// DX11 Capture Implementation extending HookCaptureBase
// Supports both DX11 and DX10 games.
// DX10 capture must copy on the real D3D10 device and publish DXGI shared
// texture handles that the media-side D3D11 device can open.
