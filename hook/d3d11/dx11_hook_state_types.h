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

#include "hook/overlay/overlay_adapter.h"

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

// Additional type definitions moved out of dx11_hook_internal.h (second page).
//
// The physical sampler forced AF chose for one logical pixel sampler, within one
// replacement-cache epoch. The shader/view verdict varies per draw, so each
// verdict's choice is kept.
struct D3D11PixelSamplerChoice {
    uint64_t epoch = 0;  // 0: nothing resolved yet
    bool isReplacement = false;
    bool descAllowsAF = false;
    bool resolved[2] = {};
    ID3D11SamplerState* physical[2] = {};  // [AF allowed]; acquired, null keeps the logical sampler
};

// Choices per logical sampler object, so a game that alternates samplers in a
// slot reuses them. Each entry holds a reference to its sampler: the address
// cannot be reused by another object while cached. Bounded (sampler states own no
// texture memory); a full probe window evicts its least recently used entry.
struct D3D11SamplerChoiceCache {
    static constexpr size_t kEntries = 128;
    static constexpr size_t kProbe = 4;
    struct Entry {
        ID3D11SamplerState* logical = nullptr;  // acquired
        D3D11PixelSamplerChoice choice;
        uint64_t stamp = 0;
    };
    Entry entries[kEntries] = {};
    uint64_t clock = 0;
};

struct D3D11StageState {
    ID3D11ShaderResourceView* srvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    ID3D11SamplerState* realSamplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    // Forced-AF class of each tracked view: 0 unknown, else D3D11ForcedAFResourceDecision + 1.
    // A view's class never changes; the entry is cleared whenever its slot changes.
    uint8_t srvAFClass[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
};

// Raw-context forced-AF work, counted under the state mutex the hooks already
// hold (no atomics on the game's thread). Summed by DX11Hook_ReportSamplerCostIfDue.
struct D3D11SamplerCostCounters {
    uint64_t srvBinds = 0;
    uint64_t samplerBinds = 0;
    uint64_t shaderBinds = 0;
    uint64_t passes = 0;      // dirty-slot reconciliations (draw or sampler bind)
    uint64_t fastSlots = 0;   // slots resolved from a memoized choice
    uint64_t slowSlots = 0;   // slots that classified sampler, shader or views
    uint64_t viewClasses = 0; // view AF classes read back for a changed texture
    uint64_t rebinds = 0;     // physical PSSetSamplers calls CE issued
    uint64_t timedPasses = 0; // passes whose duration was sampled
    uint64_t timedTicks = 0;
    uint64_t timedBinds = 0;  // SRV/sampler/shader bind hooks whose tracking time was sampled
    uint64_t timedBindTicks = 0;
    uint64_t choiceEvictions = 0;

    void Add(const D3D11SamplerCostCounters& other) {
        srvBinds += other.srvBinds;
        samplerBinds += other.samplerBinds;
        shaderBinds += other.shaderBinds;
        passes += other.passes;
        fastSlots += other.fastSlots;
        slowSlots += other.slowSlots;
        viewClasses += other.viewClasses;
        rebinds += other.rebinds;
        timedPasses += other.timedPasses;
        timedTicks += other.timedTicks;
        timedBinds += other.timedBinds;
        timedBindTicks += other.timedBindTicks;
        choiceEvictions += other.choiceEvictions;
    }
};

struct D3D11PerContextState {
    D3D11StageState stages[6];
    ID3D11PixelShader* pixelShader = nullptr;
    ce::sampler_override::D3D11ShaderAFPlan pixelShaderAFPlan;
    bool hasPixelShaderMetadata = false;
    bool pixelShaderTracked = false;  // a PSSetShader (or refresh) set pixelShader, null included
    uint32_t pixelSamplerDirtyMask = 0;
    D3D11SamplerChoiceCache samplerChoices;
    D3D11SamplerCostCounters cost;
};

// What classifying one pixel slot's logical sampler established; memoized by
// StorePixelSamplerChoice11 when valid (the replacement exists or was not needed).
struct D3D11PixelSamplerFacts11 {
    bool valid = false;
    bool isReplacement = false;
    bool descAllowsAF = false;
    bool allowAF = false;
};

inline std::mutex dx11_hook_g_D3D11ContextStateMutex;

inline std::unordered_map<ID3D11DeviceContext*, D3D11PerContextState> dx11_hook_g_D3D11ContextStates;

inline std::atomic<uint32_t> dx11_hook_g_D3D11DirtyContextCount{0};

// Counters of context states that were cleared; guarded by dx11_hook_g_D3D11ContextStateMutex.
inline D3D11SamplerCostCounters dx11_hook_g_RetiredSamplerCost11;

// The raw-vtable fallback is normally bypassed by the context wrapper. For
// callers that retain a real context pointer, let clean draws test one atomic
// and return without taking the process-global state mutex.
void MarkPixelSamplersDirty11Locked(D3D11PerContextState& state, uint32_t mask);
size_t GetStageIndex(D3D11ShaderStage stage);
const char* GetStageName11(D3D11ShaderStage stage);
uint32_t SamplerRangeMask11(UINT startSlot, UINT numSamplers);
uint32_t TrackedPixelSamplerMask11Locked(const D3D11PerContextState& state);
void GetStageShaderResources11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot, UINT numViews,
                               ID3D11ShaderResourceView** views);
void GetStageSamplers11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot, UINT numSamplers,
                        ID3D11SamplerState** samplers);
void ReleaseTrackedContextState11(D3D11PerContextState& state);
void ReleaseTrackedShaderResources11Unlocked();
void ClearTrackedContextState11(ID3D11DeviceContext* context);
void ReleaseTrackedShaderResources11();
void UpdateStageShaderResources(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot, UINT numViews,
                                ID3D11ShaderResourceView* const* ppShaderResourceViews);
uint32_t UpdateStageSamplers(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot, UINT numSamplers,
                             ID3D11SamplerState* const* ppSamplers);
void RememberRealSampler11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT slot,
                           ID3D11SamplerState* sampler);
void RememberRealSamplerRange11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot, UINT numSamplers,
                                ID3D11SamplerState* const* ppSamplers);
ID3D11SamplerState* GetRememberedRealSampler11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT slot);
void ClearPixelSamplerDirtyMask11(ID3D11DeviceContext* context, uint32_t slotMask);
ID3D11ShaderResourceView* GetTrackedShaderResourceView11(ID3D11DeviceContext* context, D3D11ShaderStage stage,
                                                         UINT slot);
ID3D11SamplerState* GetTrackedSampler11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT slot);
void UpdateTrackedPixelShader11(ID3D11DeviceContext* context, ID3D11PixelShader* shader);
bool GetTrackedPixelShaderSamplerPlan11(ID3D11DeviceContext* context, UINT slot, bool* hasShader,
                                        ce::sampler_override::D3D11AFSamplerPlan* samplerPlan, bool* failed);
void RefreshPixelShaderFromContext11(ID3D11DeviceContext* context);
void RefreshStageShaderResourcesFromContext11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot,
                                              UINT numViews);
void RefreshStageSamplersFromContext11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot,
                                       UINT numSamplers);
ce::sampler_override::D3D11ForcedAFResourceDecision ClassifyViewForForcedAF11(
    ID3D11Device* device, ID3D11ShaderResourceView* view,
    ce::sampler_override::D3D11Texture2DForcedAFInfo* outInfo = nullptr);
bool SamplerAllowsForcedAF(const D3D11_SAMPLER_DESC& desc, const GraphicsConfig& gfx);
bool ShouldForceAnisotropyForStageSlot(ID3D11Device* device, ID3D11DeviceContext* context, D3D11ShaderStage stage,
                                       UINT slot, const D3D11_SAMPLER_DESC& desc, const GraphicsConfig& gfx);
ID3D11SamplerState* GetOrCreateReplacementSampler11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT slot,
                                                    ID3D11SamplerState* original,
                                                    D3D11PixelSamplerFacts11* facts = nullptr);
void ResetPixelSamplerChoice11(D3D11PixelSamplerChoice& choice);
void ReleaseSamplerChoiceCache11(D3D11SamplerChoiceCache& cache);
const D3D11PixelSamplerChoice* FindPixelSamplerChoiceLocked11(D3D11SamplerChoiceCache& cache,
                                                              ID3D11SamplerState* logical);
// Samples the duration of one bind hook in 32 for the [D3D11 AF COST] line.
class D3D11BindCostSample11 {
public:
    D3D11BindCostSample11();
    void Finish(D3D11PerContextState& state);  // under the state lock
private:
    int64_t startTicks_ = 0;
};
void NotePixelViewAFClass11(ID3D11DeviceContext* context, UINT textureSlot, ID3D11ShaderResourceView* view,
                            ce::sampler_override::D3D11ForcedAFResourceDecision decision);
void StorePixelSamplerChoice11(ID3D11DeviceContext* context, UINT slot, ID3D11SamplerState* logical, uint64_t epoch,
                               const D3D11PixelSamplerFacts11& facts, ID3D11SamplerState* physical);
// Consumes the dirty pixel sampler bits inside `rangeMask` and binds the physical
// sampler each one needs. `bindEmptySlots`: a slot whose logical sampler is null
// is unbound too (sampler binds); draws leave such slots as the game set them.
int ReconcilePixelSamplers11(SetSamplers11_t originalFn, ID3D11DeviceContext* context, uint32_t rangeMask,
                             bool bindEmptySlots, uint32_t* consumedMask = nullptr);
D3D11SamplerCostCounters DX11Hook_SamplerCostTotalsForTesting();
void SetSamplersWithOverrides11(SetSamplers11_t originalFn, ID3D11DeviceContext* context, D3D11ShaderStage stage,
                                UINT startSlot, UINT numSamplers, ID3D11SamplerState* const* ppSamplers);

void InstallContextVTableHooks11(ID3D11DeviceContext* context, const char* source);
HRESULT STDMETHODCALLTYPE DetourCreatePixelShader11(ID3D11Device* device, const void* shaderBytecode,
                                                    SIZE_T bytecodeLength, ID3D11ClassLinkage* classLinkage,
                                                    ID3D11PixelShader** pixelShader);
HRESULT STDMETHODCALLTYPE DetourCreateDeferredContext11(ID3D11Device* device, UINT contextFlags,
                                                        ID3D11DeviceContext** deferredContext);
void STDMETHODCALLTYPE DetourPSSetShader11(ID3D11DeviceContext* context, ID3D11PixelShader* pixelShader,
                                           ID3D11ClassInstance* const* classInstances, UINT numClassInstances);
void STDMETHODCALLTYPE DetourPSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                    ID3D11ShaderResourceView* const* ppShaderResourceViews);
void STDMETHODCALLTYPE DetourPSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                             ID3D11SamplerState* const* ppSamplers);
void ReconcilePixelSamplersBeforeDraw11(ID3D11DeviceContext* context);
void STDMETHODCALLTYPE DetourDrawIndexed11(ID3D11DeviceContext* context, UINT indexCount, UINT startIndexLocation,
                                           INT baseVertexLocation);
void STDMETHODCALLTYPE DetourDraw11(ID3D11DeviceContext* context, UINT vertexCount, UINT startVertexLocation);
void STDMETHODCALLTYPE DetourDrawIndexedInstanced11(ID3D11DeviceContext* context, UINT indexCountPerInstance,
                                                    UINT instanceCount, UINT startIndexLocation, INT baseVertexLocation,
                                                    UINT startInstanceLocation);
void STDMETHODCALLTYPE DetourDrawInstanced11(ID3D11DeviceContext* context, UINT vertexCountPerInstance,
                                             UINT instanceCount, UINT startVertexLocation, UINT startInstanceLocation);
void STDMETHODCALLTYPE DetourDrawAuto11(ID3D11DeviceContext* context);
void STDMETHODCALLTYPE DetourDrawIndexedInstancedIndirect11(ID3D11DeviceContext* context, ID3D11Buffer* bufferForArgs,
                                                            UINT alignedByteOffsetForArgs);
void STDMETHODCALLTYPE DetourDrawInstancedIndirect11(ID3D11DeviceContext* context, ID3D11Buffer* bufferForArgs,
                                                     UINT alignedByteOffsetForArgs);
void STDMETHODCALLTYPE DetourExecuteCommandList11(ID3D11DeviceContext* context, ID3D11CommandList* commandList,
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
VSyncOverride GetDX11VSyncOverride();
bool ShouldSkipWindowForNvPresent(HWND hwnd);

void ProcessDX11FrameWithOverlayOrdering(IDXGISwapChain* pSwapChain);

void InstallVTableHooks(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, IDXGISwapChain* pSwapChain);
HRESULT WINAPI DetourD3D11CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
                                                   UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels,
                                                   UINT FeatureLevels, UINT SDKVersion,
                                                   const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                                   IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
                                                   D3D_FEATURE_LEVEL* pFeatureLevel,
                                                   ID3D11DeviceContext** ppImmediateContext);
HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory* pFactory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                                IDXGISwapChain** ppSwapChain);
HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd,
                                                       const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                                       IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain);

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

void STDMETHODCALLTYPE DetourPSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);

HRESULT STDMETHODCALLTYPE DetourCreateSamplerState10(ID3D10Device* pDevice,
                                                            const D3D10_SAMPLER_DESC* pSamplerDesc,
                                                            ID3D10SamplerState** ppSamplerState);

inline void LogVTableHookInstalled11(const char* name, bool additionalVtable, UINT slotIndex) {
    if (additionalVtable) {
        HookLog("DX11: %s hook installed on additional vtable (slot=%u)", name, slotIndex);
    } else {
        HookLog("DX11: %s hook installed (slot=%u)", name, slotIndex);
    }
}

template <typename Fn>
static bool EnsureVTableHookSlot11(void** vtable, UINT slotIndex, LPVOID detour, Fn& original, const char* name) {
    if (!vtable || !detour) {
        return false;
    }

    void** slot = &vtable[slotIndex];
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
                             slotIndex, current, knownOriginal);
        }
        return false;
    }

    LPVOID capturedOriginal = nullptr;
    LPVOID* originalOut = original ? &capturedOriginal : reinterpret_cast<LPVOID*>(&original);
    VTableHook::Status status = VTableHook::Create(reinterpret_cast<void*>(slot), detour, originalOut);
    if (status == VTableHook::Success) {
        int idx = dx11_hook_g_DiagSamplerRuntimeHookInstalled.fetch_add(1, std::memory_order_relaxed);
        if (idx < 24) {
            LogVTableHookInstalled11(name, knownOriginal != nullptr, slotIndex);
        }
        return true;
    }

    static std::atomic<int> s_failureLogs{0};
    int idx = s_failureLogs.fetch_add(1, std::memory_order_relaxed);
    if (idx < 12) {
        HookLogImportant("DX11: %s hook install failed status=%d slot=%u current=%p", name, status, slotIndex, current);
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
