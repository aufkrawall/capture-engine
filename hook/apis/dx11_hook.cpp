#include <atomic>
#include <cstdint>
#include <cstdio>
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
#include "../common/hook_common.h"
#include "../common/overlay_adapter.h"
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
ce::DeferredReleaseQueue g_DeferredRelease;
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
#include "../wrappers/iat_hook.h"
#include "../wrappers/vtable_hook.h"
#include "../wrappers/wrapper_base.h"
// mutex already included

// Forward declaration for cross-file DX12 overlay invalidation
// Called when FSR4SwapchainProvider creates a new swapchain to signal pending
// cleanup Uses atomic flag to avoid cross-thread deadlocks - DX12 thread does
// actual cleanup
extern void DX12_SignalFSR4SwapchainRecreated();
// Called BEFORE new swapchain creation to immediately invalidate overlay
// (checked throughout Present)
extern void DX12_InvalidateSwapchain();

// Globals
// Cached device/context with AddRef — avoids calling GetDevice() every frame
// (which crashes during shutdown when the swapchain's internal device ref is
// freed)
static ID3D11Device* g_pd3dDevice = NULL;
static ID3D11DeviceContext* g_pd3dDeviceContext = NULL;
static ID3D11RenderTargetView* g_mainRenderTargetView = NULL;

static ID3D10Device* g_pd3d10Device = NULL;
static ID3D10RenderTargetView* g_mainRenderTargetView10 = NULL;

static IDXGISwapChain* g_pSwapChain = NULL;

static bool g_IsDX10Device = false;
static const char* g_DetectedAPI = "DX11";

// Prerender Limit Fencing
static std::vector<ID3D11Query*> g_PrerenderQueries;
static uint64_t g_PrerenderFrameIndex = 0;
static int64_t g_LastSleepUs = 0;

// Sampler override diagnostic counters (rate-limited logging)
static std::atomic<int> g_DiagSamplerAllowsAF{0};
static std::atomic<int> g_DiagSamplerSkipNoMips{0};
static std::atomic<int> g_DiagSamplerSkipBorder{0};
static std::atomic<int> g_DiagSamplerSkipReduction{0};
static std::atomic<int> g_DiagSamplerSkipComparison{0};
static std::atomic<int> g_DiagSamplerSkipSlot{0};
static std::atomic<int> g_DiagSamplerSkipNoSRV{0};
static std::atomic<int> g_DiagSamplerSkipFormat{0};
static std::atomic<int> g_DiagSamplerSkipSingleMip{0};
static std::atomic<int> g_DiagSamplerAFApplied{0};
static std::atomic<int> g_DiagSamplerReplacementCreated{0};
static std::atomic<int> g_DiagSamplerMipBiasApplied{0};
static std::atomic<int> g_DiagSamplerMipOverride{0};
static std::atomic<int> g_DiagPrerenderFrames{0};
static std::atomic<int> g_DiagPrerenderWaits{0};

// Cross-function tracking for FSR4/FG swapchain recreation detection
// Shared between DetourCreateSwapChain and DetourCreateSwapChainForHwnd
static bool g_FirstGameSwapchainCreated = false;
// Forces overlay and capture to use the same backbuffer index within one frame.
// This eliminates index races on FLIP swapchains.
static thread_local int g_ForcedCaptureBackBufferIndex = -1;
// Capture can optionally reuse the RTV that overlay rendered to on this frame.
// Leave this off when capture must happen before overlay.
static thread_local bool g_CaptureUsesOverlayRTV = false;
// Re-entrancy guard: prevents mutual recursion with other Present hooks (e.g. Steam overlay)
thread_local bool g_InPresentHook = false;

// DX11 runtimes can expose D3D10 compatibility interfaces on the same swapchain.
// Prefer the highest actual device API so DX11 swapchains do not fall into the
// DX10 overlay/capture paths just because ID3D10 queries happen to succeed.
static DXGIShared::APIType DetectSwapChainAPITypeForDX11Hook(IDXGISwapChain* swapChain) {
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

static const char* GetDX11HookBaseAPIName(DXGIShared::APIType api) {
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

static bool IsDXVKD3D10OrD3D11Loaded() {
    return IsDllFromProject("d3d11.dll", "dxvk") || IsDllFromProject("d3d10.dll", "dxvk") ||
           IsDllFromProject("d3d10_1.dll", "dxvk");
}

static const char* GetDX11HookOverlayAPIName(DXGIShared::APIType api) {
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

static UINT ResolveDX11BackBufferIndex(IDXGISwapChain* swapChain, const DXGI_SWAP_CHAIN_DESC* swapChainDesc = nullptr) {
    if (!swapChain)
        return 0;

    if (g_ForcedCaptureBackBufferIndex >= 0)
        return static_cast<UINT>(g_ForcedCaptureBackBufferIndex);

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

static bool IsUnityProcess() {
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

typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
typedef HRESULT(STDMETHODCALLTYPE* ResizeBuffers_t)(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width,
                                                    UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
static Present_t oPresent = NULL;
typedef HRESULT(STDMETHODCALLTYPE* Present1_t)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT PresentFlags,
                                               const DXGI_PRESENT_PARAMETERS* pPresentParameters);
static Present1_t oPresent1 = NULL;

#ifndef DXGI_PRESENT_ALLOW_TEARING
#define DXGI_PRESENT_ALLOW_TEARING 0x00000200UL
#endif
typedef HRESULT(STDMETHODCALLTYPE* CreateSamplerState_t)(ID3D11Device* pDevice, const D3D11_SAMPLER_DESC* pSamplerDesc,
                                                         ID3D11SamplerState** ppSamplerState);
static CreateSamplerState_t oCreateSamplerState = NULL;

typedef void(STDMETHODCALLTYPE* SetShaderResources11_t)(ID3D11DeviceContext* pContext, UINT StartSlot, UINT NumViews,
                                                        ID3D11ShaderResourceView* const* ppShaderResourceViews);
typedef void(STDMETHODCALLTYPE* SetSamplers11_t)(ID3D11DeviceContext* pContext, UINT StartSlot, UINT NumSamplers,
                                                 ID3D11SamplerState* const* ppSamplers);

static SetShaderResources11_t oPSSetShaderResources11 = NULL;
static SetShaderResources11_t oVSSetShaderResources11 = NULL;
static SetShaderResources11_t oGSSetShaderResources11 = NULL;
static SetShaderResources11_t oHSSetShaderResources11 = NULL;
static SetShaderResources11_t oDSSetShaderResources11 = NULL;
static SetShaderResources11_t oCSSetShaderResources11 = NULL;

static SetSamplers11_t oPSSetSamplers11 = NULL;
static SetSamplers11_t oVSSetSamplers11 = NULL;
static SetSamplers11_t oGSSetSamplers11 = NULL;
static SetSamplers11_t oHSSetSamplers11 = NULL;
static SetSamplers11_t oDSSetSamplers11 = NULL;
static SetSamplers11_t oCSSetSamplers11 = NULL;

// D3D10 CreateSamplerState hook
typedef HRESULT(STDMETHODCALLTYPE* CreateSamplerState10_t)(ID3D10Device* pDevice,
                                                           const D3D10_SAMPLER_DESC* pSamplerDesc,
                                                           ID3D10SamplerState** ppSamplerState);
static CreateSamplerState10_t oCreateSamplerState10 = NULL;

// Forward declaration
static void InstallRuntimeD3D10Hooks(ID3D10Device* pDevice);

// Typedefs for D3D10 SetSamplers (used by hooks) for runtime sampler
// replacement
typedef void(STDMETHODCALLTYPE* PSSetSamplers10_t)(ID3D10Device* pDevice, UINT StartSlot, UINT NumSamplers,
                                                   ID3D10SamplerState* const* ppSamplers);
typedef void(STDMETHODCALLTYPE* VSSetSamplers10_t)(ID3D10Device* pDevice, UINT StartSlot, UINT NumSamplers,
                                                   ID3D10SamplerState* const* ppSamplers);
typedef void(STDMETHODCALLTYPE* GSSetSamplers10_t)(ID3D10Device* pDevice, UINT StartSlot, UINT NumSamplers,
                                                   ID3D10SamplerState* const* ppSamplers);
static PSSetSamplers10_t oPSSetSamplers10 = NULL;
static VSSetSamplers10_t oVSSetSamplers10 = NULL;
static GSSetSamplers10_t oGSSetSamplers10 = NULL;

// Lock-free sampler replacement cache for D3D10
// Maps original sampler -> our modified replacement sampler
#include <algorithm>
#include <atomic>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Use vector instead of map to avoid STL template issues with Clang/MSVC
// headers
static std::vector<std::pair<ID3D10SamplerState*, ID3D10SamplerState*>> g_SamplerCache10;
static std::shared_mutex g_SamplerCacheMutex10;
static ID3D10Device* g_CachedD3D10Device = nullptr;
static std::vector<ID3D10SamplerState*> g_ReplacementSamplers10;  // Prevent recursive replacements
static uint64_t g_SamplerConfigHash10 = 0;

static std::vector<std::pair<ID3D11SamplerState*, ID3D11SamplerState*>> g_SamplerCache11;
static std::shared_mutex g_SamplerCacheMutex11;
static std::vector<ID3D11SamplerState*> g_ReplacementSamplers11;
static uint64_t g_SamplerConfigHash11 = 0;

static std::mutex g_D3D11FormatSupportMutex;
static std::unordered_map<DXGI_FORMAT, bool> g_D3D11FormatSupportCache;

static thread_local bool g_InOverlayRender = false;

enum class D3D11ShaderStage : uint32_t {
    Pixel,
    Vertex,
    Geometry,
    Hull,
    Domain,
    Compute,
};

// Helper for linear search
static ID3D10SamplerState* FindReplacementSampler(ID3D10SamplerState* original) {
    std::shared_lock<std::shared_mutex> lock(g_SamplerCacheMutex10);
    for (const auto& entry : g_SamplerCache10) {
        if (entry.first == original)
            return entry.second;
    }
    return nullptr;
}

static void AddReplacementSampler(ID3D10SamplerState* original, ID3D10SamplerState* replacement) {
    std::unique_lock<std::shared_mutex> lock(g_SamplerCacheMutex10);
    for (auto& entry : g_SamplerCache10) {
        if (entry.first == original) {
            entry.second = replacement;
            return;
        }
    }
    g_SamplerCache10.push_back({original, replacement});
}

static bool IsReplacementSampler(ID3D10SamplerState* sampler) {
    for (auto s : g_ReplacementSamplers10) {
        if (s == sampler)
            return true;
    }
    return false;
}

static void AddToReplacementSet(ID3D10SamplerState* sampler) {
    g_ReplacementSamplers10.push_back(sampler);
}

static void ClearReplacementSamplerCache10Unlocked() {
    for (auto* sampler : g_ReplacementSamplers10) {
        if (sampler) {
            sampler->Release();
        }
    }
    g_ReplacementSamplers10.clear();
    g_SamplerCache10.clear();
    g_SamplerConfigHash10 = 0;
}

static void ClearReplacementSamplerCache10() {
    std::unique_lock<std::shared_mutex> lock(g_SamplerCacheMutex10);
    ClearReplacementSamplerCache10Unlocked();
}

static void EnsureSamplerCacheFresh10(const GraphicsConfig& gfx) {
    const uint64_t configHash = ce::sampler_override::HashSamplerOverrideConfig(gfx);
    std::unique_lock<std::shared_mutex> lock(g_SamplerCacheMutex10);
    if (g_SamplerConfigHash10 == configHash) {
        return;
    }
    ClearReplacementSamplerCache10Unlocked();
    g_SamplerConfigHash10 = configHash;
}

static ID3D11SamplerState* FindReplacementSampler11(ID3D11SamplerState* original) {
    std::shared_lock<std::shared_mutex> lock(g_SamplerCacheMutex11);
    for (const auto& entry : g_SamplerCache11) {
        if (entry.first == original) {
            return entry.second;
        }
    }
    return nullptr;
}

static void AddReplacementSampler11(ID3D11SamplerState* original, ID3D11SamplerState* replacement) {
    std::unique_lock<std::shared_mutex> lock(g_SamplerCacheMutex11);
    for (auto& entry : g_SamplerCache11) {
        if (entry.first == original) {
            entry.second = replacement;
            return;
        }
    }
    g_SamplerCache11.push_back({original, replacement});
}

static bool IsReplacementSampler11(ID3D11SamplerState* sampler) {
    for (auto* replacement : g_ReplacementSamplers11) {
        if (replacement == sampler) {
            return true;
        }
    }
    return false;
}

static void AddToReplacementSet11(ID3D11SamplerState* sampler) {
    g_ReplacementSamplers11.push_back(sampler);
}

static void ClearReplacementSamplerCache11Unlocked() {
    for (auto* sampler : g_ReplacementSamplers11) {
        if (sampler) {
            sampler->Release();
        }
    }
    g_ReplacementSamplers11.clear();
    g_SamplerCache11.clear();
    g_SamplerConfigHash11 = 0;
}

static void ClearReplacementSamplerCache11() {
    std::unique_lock<std::shared_mutex> lock(g_SamplerCacheMutex11);
    ClearReplacementSamplerCache11Unlocked();
}

static void EnsureSamplerCacheFresh11(const GraphicsConfig& gfx) {
    const uint64_t configHash = ce::sampler_override::HashSamplerOverrideConfig(gfx);
    std::unique_lock<std::shared_mutex> lock(g_SamplerCacheMutex11);
    if (g_SamplerConfigHash11 == configHash) {
        return;
    }
    ClearReplacementSamplerCache11Unlocked();
    g_SamplerConfigHash11 = configHash;
}

struct D3D11StageState {
    ID3D11ShaderResourceView* srvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
};

struct D3D11PerContextState {
    D3D11StageState stages[6];
};

static std::mutex g_D3D11ContextStateMutex;
static std::unordered_map<ID3D11DeviceContext*, D3D11PerContextState> g_D3D11ContextStates;

static size_t GetStageIndex(D3D11ShaderStage stage) {
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

static void ReleaseTrackedShaderResources11Unlocked() {
    for (auto& [context, state] : g_D3D11ContextStates) {
        (void)context;
        for (D3D11StageState& stageState : state.stages) {
            for (ID3D11ShaderResourceView*& view : stageState.srvs) {
                if (view) {
                    view->Release();
                    view = nullptr;
                }
            }
        }
    }
    g_D3D11ContextStates.clear();
}

static void ReleaseTrackedShaderResources11() {
    std::lock_guard<std::mutex> lock(g_D3D11ContextStateMutex);
    ReleaseTrackedShaderResources11Unlocked();
}

static void UpdateStageShaderResources(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot,
                                       UINT numViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    if (!context) {
        return;
    }
    if (startSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_D3D11ContextStateMutex);
    D3D11PerContextState& contextState = g_D3D11ContextStates[context];
    D3D11StageState& state = contextState.stages[GetStageIndex(stage)];
    const UINT maxViews = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - startSlot;
    const UINT actualViews = (numViews < maxViews) ? numViews : maxViews;

    for (UINT i = 0; i < actualViews; ++i) {
        const UINT slot = startSlot + i;
        if (state.srvs[slot]) {
            state.srvs[slot]->Release();
            state.srvs[slot] = nullptr;
        }

        ID3D11ShaderResourceView* view = ppShaderResourceViews ? ppShaderResourceViews[i] : nullptr;
        if (view) {
            view->AddRef();
        }
        state.srvs[slot] = view;
    }
}

static ID3D11ShaderResourceView* GetTrackedShaderResourceView11(ID3D11DeviceContext* context, D3D11ShaderStage stage,
                                                                UINT slot) {
    if (!context || slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(g_D3D11ContextStateMutex);
    auto it = g_D3D11ContextStates.find(context);
    if (it == g_D3D11ContextStates.end()) {
        return nullptr;
    }

    ID3D11ShaderResourceView* view = it->second.stages[GetStageIndex(stage)].srvs[slot];
    if (view) {
        view->AddRef();
    }
    return view;
}

static bool SupportsD3D11SamplingFormat(ID3D11Device* device, DXGI_FORMAT format) {
    if (!device || format == DXGI_FORMAT_UNKNOWN) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_D3D11FormatSupportMutex);
        auto it = g_D3D11FormatSupportCache.find(format);
        if (it != g_D3D11FormatSupportCache.end()) {
            return it->second;
        }
    }

    UINT support = 0;
    bool result =
        SUCCEEDED(device->CheckFormatSupport(format, &support)) && (support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) != 0;

    std::lock_guard<std::mutex> lock(g_D3D11FormatSupportMutex);
    g_D3D11FormatSupportCache[format] = result;
    return result;
}

static bool IsPotentiallyProblematicDXGIFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_UNKNOWN:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32G32_SINT:
        case DXGI_FORMAT_R32G32B32_UINT:
        case DXGI_FORMAT_R32G32B32_SINT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SINT:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SINT:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return true;
        default:
            return false;
    }
}

static bool ViewHasMultipleVisibleMips(ID3D11ShaderResourceView* view) {
    if (!view) {
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    view->GetDesc(&srvDesc);

    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);
    if (!resource) {
        return false;
    }

    bool hasMultipleVisibleMips = false;

    ID3D11Texture2D* texture2D = nullptr;
    if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture2D))) && texture2D) {
        D3D11_TEXTURE2D_DESC textureDesc = {};
        texture2D->GetDesc(&textureDesc);

        const bool singleSample = textureDesc.SampleDesc.Count <= 1;
        const bool isArrayResource = textureDesc.ArraySize > 1;
        const UINT totalMipLevels =
            ce::sampler_override::ResolveFullMipCount2D(textureDesc.Width, textureDesc.Height, textureDesc.MipLevels);

        UINT mostDetailedMip = 0;
        UINT viewMipLevels = UINT_MAX;
        switch (srvDesc.ViewDimension) {
            case D3D11_SRV_DIMENSION_TEXTURE2D:
                mostDetailedMip = srvDesc.Texture2D.MostDetailedMip;
                viewMipLevels = srvDesc.Texture2D.MipLevels;
                break;
            case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
                mostDetailedMip = srvDesc.Texture2DArray.MostDetailedMip;
                viewMipLevels = srvDesc.Texture2DArray.MipLevels;
                break;
            case D3D11_SRV_DIMENSION_TEXTURECUBE:
                mostDetailedMip = srvDesc.TextureCube.MostDetailedMip;
                viewMipLevels = srvDesc.TextureCube.MipLevels;
                break;
            case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
                mostDetailedMip = srvDesc.TextureCubeArray.MostDetailedMip;
                viewMipLevels = srvDesc.TextureCubeArray.MipLevels;
                break;
            default:
                viewMipLevels = 0;
                break;
        }

        const UINT visibleMipLevels =
            ce::sampler_override::ResolveVisibleMipCount(totalMipLevels, mostDetailedMip, viewMipLevels);
        const bool safeDimension = !isArrayResource;
        const bool safeUsage = (textureDesc.BindFlags & D3D11_BIND_DEPTH_STENCIL) == 0 &&
                               (textureDesc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) == 0;
        const bool safeFormat = !IsPotentiallyProblematicDXGIFormat(textureDesc.Format);

        hasMultipleVisibleMips = singleSample && safeDimension && safeUsage && safeFormat && visibleMipLevels > 1;
        texture2D->Release();
    }

    resource->Release();
    return hasMultipleVisibleMips;
}

static bool SamplerAllowsForcedAF(const D3D11_SAMPLER_DESC& desc, const GraphicsConfig& gfx) {
    if (!ce::sampler_override::IsAnisotropicOverrideEnabled(gfx)) {
        return false;
    }
    if (desc.MaxLOD == 0.0f || desc.MinLOD == desc.MaxLOD) {
        int idx = g_DiagSamplerSkipNoMips.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            HookLogImportant("DX11: AF skip sampler (no mips) MaxLOD=%.1f MinLOD=%.1f", desc.MaxLOD, desc.MinLOD);
        }
        return false;
    }
    if (desc.AddressU == D3D11_TEXTURE_ADDRESS_BORDER || desc.AddressV == D3D11_TEXTURE_ADDRESS_BORDER ||
        desc.AddressW == D3D11_TEXTURE_ADDRESS_BORDER) {
        int idx = g_DiagSamplerSkipBorder.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            HookLogImportant("DX11: AF skip sampler (border address) U=%d V=%d W=%d",
                    desc.AddressU, desc.AddressV, desc.AddressW);
        }
        return false;
    }
    if (ce::sampler_override::IsD3D11ReductionFilter(desc.Filter)) {
        int idx = g_DiagSamplerSkipReduction.fetch_add(1, std::memory_order_relaxed);
        if (idx < 6) {
            HookLogImportant("DX11: AF skip sampler (reduction filter) Filter=0x%X", desc.Filter);
        }
        return false;
    }
    if (desc.ComparisonFunc != D3D11_COMPARISON_NEVER) {
        int idx = g_DiagSamplerSkipComparison.fetch_add(1, std::memory_order_relaxed);
        if (idx < 6) {
            HookLogImportant("DX11: AF skip sampler (comparison func) Func=%d", desc.ComparisonFunc);
        }
        return false;
    }
    return true;
}

static bool ShouldForceAnisotropyForStageSlot(ID3D11Device* device, ID3D11DeviceContext* context,
                                              D3D11ShaderStage stage, UINT slot, const D3D11_SAMPLER_DESC& desc,
                                              const GraphicsConfig& gfx) {
    if (!SamplerAllowsForcedAF(desc, gfx)) {
        return false;
    }
    // D3D11 does not guarantee sampler-slot == SRV-slot. Keep this runtime AF path
    // to the common low slots where most games pair sN/tN conventionally.
    if (slot >= 8) {
        int idx = g_DiagSamplerSkipSlot.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            HookLogImportant("DX11: AF skip sampler (slot %u >= 8, stage=%d)", slot, (int)stage);
        }
        return false;
    }
    if (slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        return false;
    }

    ID3D11ShaderResourceView* view = GetTrackedShaderResourceView11(context, stage, slot);
    if (!view) {
        int idx = g_DiagSamplerSkipNoSRV.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            HookLogImportant("DX11: AF skip sampler (no SRV at slot %u, stage=%d)", slot, (int)stage);
        }
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    view->GetDesc(&srvDesc);
    if (!SupportsD3D11SamplingFormat(device, srvDesc.Format)) {
        int idx = g_DiagSamplerSkipFormat.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            HookLogImportant("DX11: AF skip sampler (unsupported format %d at slot %u, stage=%d)",
                    srvDesc.Format, slot, (int)stage);
        }
        view->Release();
        return false;
    }

    const bool shouldForce = ViewHasMultipleVisibleMips(view);
    if (!shouldForce) {
        int idx = g_DiagSamplerSkipSingleMip.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            HookLogImportant("DX11: AF skip sampler (single mip at slot %u, stage=%d)", slot, (int)stage);
        }
    }
    view->Release();
    return shouldForce;
}

static bool ApplySamplerOverrides11(D3D11_SAMPLER_DESC& desc, const GraphicsConfig& gfx,
                                    bool allowAnisotropicOverride) {
    bool modified = false;

    const std::string& af = gfx.anisotropicFiltering;
    if (af != "default" && !af.empty()) {
        if (af == "off") {
            if (ce::sampler_override::IsD3D11AnisotropicFilter(desc.Filter)) {
                desc.Filter = ce::sampler_override::IsD3D11ComparisonFilter(desc.Filter)
                                  ? D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR
                                  : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                desc.MaxAnisotropy = 1;
                modified = true;
                HookLogImportant("DX11: AF override OFF Filter=0x%X->0x%X Aniso=%u->1",
                        desc.Filter, desc.Filter, desc.MaxAnisotropy);
            }
        } else if (allowAnisotropicOverride) {
            const UINT maxAniso = ce::sampler_override::GetConfiguredMaxAnisotropy(gfx);
            const D3D11_FILTER newFilter = ce::sampler_override::GetForcedAnisotropicFilter(desc.Filter);
            if (desc.Filter != newFilter || desc.MaxAnisotropy != maxAniso) {
                const D3D11_FILTER origFilter = desc.Filter;
                const UINT origAniso = desc.MaxAnisotropy;
                desc.Filter = newFilter;
                desc.MaxAnisotropy = maxAniso;
                modified = true;
                int idx = g_DiagSamplerAFApplied.fetch_add(1, std::memory_order_relaxed);
                if (idx < 48) {
                    HookLogImportant("DX11: AF override ON Filter=0x%X->0x%X Aniso=%u->%u (#%d)",
                            origFilter, desc.Filter, origAniso, desc.MaxAnisotropy, idx + 1);
                }
            }
        }
    }

    const std::string& mip = gfx.mipMapping;
    const bool isAniso = ce::sampler_override::IsD3D11AnisotropicFilter(desc.Filter);
    if (mip != "default" && !isAniso) {
        if (mip == "trilinear") {
            if (desc.Filter != D3D11_FILTER_MIN_MAG_MIP_LINEAR) {
                desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                modified = true;
                int idx = g_DiagSamplerMipOverride.fetch_add(1, std::memory_order_relaxed);
                if (idx < 12) {
                    HookLogImportant("DX11: Mip override trilinear applied (#%d)", idx + 1);
                }
            }
        } else if (mip == "bilinear") {
            if (desc.Filter != D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT) {
                desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
                modified = true;
                int idx = g_DiagSamplerMipOverride.fetch_add(1, std::memory_order_relaxed);
                if (idx < 12) {
                    HookLogImportant("DX11: Mip override bilinear applied (#%d)", idx + 1);
                }
            }
        }
    }

    float userBiasVal = 0.0f;
    const bool userBiasActive = TryParseConfiguredMipBias(gfx, userBiasVal);
    const float originalBias = desc.MipLODBias;
    desc.MipLODBias = ApplyConfiguredMipBias(gfx, originalBias);
    if (desc.MipLODBias != originalBias) {
        modified = true;
        int idx = g_DiagSamplerMipBiasApplied.fetch_add(1, std::memory_order_relaxed);
        if (idx < 24) {
            HookLogImportant("DX11: Mip bias override Bias=%.2f->%.2f (#%d)", originalBias, desc.MipLODBias, idx + 1);
        }
    }

    if (gfx.sgssaa && !gfx.disableAutoMipBias && !gfx.forceMipBiasClamp) {
        float sgBias = 0.0f;
        if (GetSGSSAABias(gfx.sgssaa, gfx.msaaSamples.c_str(), sgBias)) {
            desc.MipLODBias += sgBias;
            modified = true;
            HookLogImportant("DX11: SGSSAA bias applied (%.2f, total=%.2f)", sgBias, desc.MipLODBias);
        }
    }

    if (userBiasActive && userBiasVal < 0.0f && !gfx.sgssaa && IsUnityProcess() && !gfx.forceMipBiasClamp) {
        if (desc.MipLODBias < -0.5f) {
            desc.MipLODBias = -0.5f;
            modified = true;
            HookLogImportant("DX11: Unity mip bias clamp -0.5 applied");
        }
    }

    const float finalizedBias = FinalizeMipBias(gfx, desc.MipLODBias);
    if (finalizedBias != desc.MipLODBias) {
        desc.MipLODBias = finalizedBias;
        modified = true;
        HookLogImportant("DX11: Finalized mip bias %.2f->%.2f", desc.MipLODBias, finalizedBias);
    }

    return modified;
}

static ID3D11SamplerState* GetOrCreateReplacementSampler11(ID3D11DeviceContext* context, D3D11ShaderStage stage,
                                                           UINT slot, ID3D11SamplerState* original) {
    if (!context || !original) {
        return original;
    }

    if (IsReplacementSampler11(original)) {
        return original;
    }

    const auto& gfx = GetActiveGraphicsConfig();
    EnsureSamplerCacheFresh11(gfx);

    if (ID3D11SamplerState* cached = FindReplacementSampler11(original)) {
        return cached;
    }

    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    if (!device) {
        AddReplacementSampler11(original, original);
        return original;
    }

    D3D11_SAMPLER_DESC desc = {};
    original->GetDesc(&desc);

    const bool allowAnisotropicOverride = ShouldForceAnisotropyForStageSlot(device, context, stage, slot, desc, gfx);
    const bool modified = ApplySamplerOverrides11(desc, gfx, allowAnisotropicOverride);
    if (!modified) {
        AddReplacementSampler11(original, original);
        device->Release();
        return original;
    }

    ID3D11SamplerState* replacement = nullptr;
    const HRESULT hr = oCreateSamplerState ? oCreateSamplerState(device, &desc, &replacement)
                                           : device->CreateSamplerState(&desc, &replacement);
    device->Release();

    if (FAILED(hr) || !replacement) {
        int idx = g_DiagSamplerReplacementCreated.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            HookLogImportant("DX11: Replacement sampler creation FAILED hr=0x%08X (stage=%d slot=%u)", hr, (int)stage, slot);
        }
        AddReplacementSampler11(original, original);
        return original;
    }

    AddReplacementSampler11(original, replacement);
    AddToReplacementSet11(replacement);
    int idx = g_DiagSamplerReplacementCreated.fetch_add(1, std::memory_order_relaxed);
    if (idx < 48) {
        HookLog("DX11: Created replacement sampler (stage=%d slot=%u Filter=0x%X Aniso=%u Bias=%.2f) #%d",
                (int)stage, slot, desc.Filter, desc.MaxAnisotropy, desc.MipLODBias, idx + 1);
    }
    return replacement;
}

static void SetSamplersWithOverrides11(SetSamplers11_t originalFn, ID3D11DeviceContext* context, D3D11ShaderStage stage,
                                       UINT startSlot, UINT numSamplers, ID3D11SamplerState* const* ppSamplers) {
    if (!originalFn) {
        return;
    }
    if (!ppSamplers || numSamplers == 0 || !g_GraphicsOverridesActive.load(std::memory_order_acquire) ||
        g_InOverlayRender) {
        originalFn(context, startSlot, numSamplers, ppSamplers);
        return;
    }

    const UINT maxSamplers = static_cast<UINT>(D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT);
    const UINT actualNum = (numSamplers < maxSamplers) ? numSamplers : maxSamplers;
    ID3D11SamplerState* replaced[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    for (UINT i = 0; i < actualNum; ++i) {
        const UINT slot = startSlot + i;
        replaced[i] = GetOrCreateReplacementSampler11(context, stage, slot, ppSamplers[i]);
    }

    originalFn(context, startSlot, numSamplers, replaced);
}

static void STDMETHODCALLTYPE DetourPSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    UpdateStageShaderResources(context, D3D11ShaderStage::Pixel, startSlot, numViews, ppShaderResourceViews);
    oPSSetShaderResources11(context, startSlot, numViews, ppShaderResourceViews);
}

static void STDMETHODCALLTYPE DetourVSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    UpdateStageShaderResources(context, D3D11ShaderStage::Vertex, startSlot, numViews, ppShaderResourceViews);
    oVSSetShaderResources11(context, startSlot, numViews, ppShaderResourceViews);
}

static void STDMETHODCALLTYPE DetourGSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    UpdateStageShaderResources(context, D3D11ShaderStage::Geometry, startSlot, numViews, ppShaderResourceViews);
    oGSSetShaderResources11(context, startSlot, numViews, ppShaderResourceViews);
}

static void STDMETHODCALLTYPE DetourHSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    UpdateStageShaderResources(context, D3D11ShaderStage::Hull, startSlot, numViews, ppShaderResourceViews);
    oHSSetShaderResources11(context, startSlot, numViews, ppShaderResourceViews);
}

static void STDMETHODCALLTYPE DetourDSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    UpdateStageShaderResources(context, D3D11ShaderStage::Domain, startSlot, numViews, ppShaderResourceViews);
    oDSSetShaderResources11(context, startSlot, numViews, ppShaderResourceViews);
}

static void STDMETHODCALLTYPE DetourCSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    UpdateStageShaderResources(context, D3D11ShaderStage::Compute, startSlot, numViews, ppShaderResourceViews);
    oCSSetShaderResources11(context, startSlot, numViews, ppShaderResourceViews);
}

static void STDMETHODCALLTYPE DetourPSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers) {
    SetSamplersWithOverrides11(oPSSetSamplers11, context, D3D11ShaderStage::Pixel, startSlot, numSamplers, ppSamplers);
}

static void STDMETHODCALLTYPE DetourVSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers) {
    SetSamplersWithOverrides11(oVSSetSamplers11, context, D3D11ShaderStage::Vertex, startSlot, numSamplers, ppSamplers);
}

static void STDMETHODCALLTYPE DetourGSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers) {
    SetSamplersWithOverrides11(oGSSetSamplers11, context, D3D11ShaderStage::Geometry, startSlot, numSamplers,
                               ppSamplers);
}

static void STDMETHODCALLTYPE DetourHSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers) {
    SetSamplersWithOverrides11(oHSSetSamplers11, context, D3D11ShaderStage::Hull, startSlot, numSamplers, ppSamplers);
}

static void STDMETHODCALLTYPE DetourDSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers) {
    SetSamplersWithOverrides11(oDSSetSamplers11, context, D3D11ShaderStage::Domain, startSlot, numSamplers, ppSamplers);
}

static void STDMETHODCALLTYPE DetourCSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers) {
    SetSamplersWithOverrides11(oCSSetSamplers11, context, D3D11ShaderStage::Compute, startSlot, numSamplers,
                               ppSamplers);
}

// Use typedef from dx11_hook.h
// typedef HRESULT(WINAPI *D3D11CreateDeviceAndSwapChain_t)(...);
// Global original function pointer - set by IAT patching
PFN_D3D11CreateDeviceAndSwapChain oD3D11CreateDeviceAndSwapChain = NULL;

// Local copy of the real original D3D11CreateDeviceAndSwapChain function address.
// HookExport calls PatchIATAllModules which overwrites the shared
// oD3D11CreateDeviceAndSwapChain (also used by wrapper_hooks.cpp) with the
// address of Wrapped_D3D11CreateDeviceAndSwapChain — causing the DX11 detour
// to call back into the wrapper instead of the real function, leading to
// infinite IAT recursion and stack overflow (0xC00000FD).
// Save the real GetProcAddress result separately so DetourD3D11CreateDeviceAndSwapChain
// can always reach the actual d3d11.dll code.
static PFN_D3D11CreateDeviceAndSwapChain s_oRealD3D11CreateDeviceAndSwapChain = NULL;

typedef HRESULT(WINAPI* PFN_D3D10CreateDeviceAndSwapChain)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT,
                                                           DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D10Device**);
static PFN_D3D10CreateDeviceAndSwapChain oD3D10CreateDeviceAndSwapChain = NULL;

typedef HRESULT(WINAPI* PFN_D3D10CreateDeviceAndSwapChain1)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT,
                                                            D3D10_FEATURE_LEVEL1, UINT, DXGI_SWAP_CHAIN_DESC*,
                                                            IDXGISwapChain**, ID3D10Device1**);
static PFN_D3D10CreateDeviceAndSwapChain1 oD3D10CreateDeviceAndSwapChain1 = NULL;

typedef HRESULT(WINAPI* PFN_D3D10CreateDevice)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT, ID3D10Device**);
static PFN_D3D10CreateDevice oD3D10CreateDevice = NULL;

typedef HRESULT(WINAPI* PFN_D3D10CreateDevice1)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, D3D10_FEATURE_LEVEL1,
                                                UINT, ID3D10Device1**);
static PFN_D3D10CreateDevice1 oD3D10CreateDevice1 = NULL;

typedef HRESULT(WINAPI* PFN_CreateDXGIFactory)(REFIID, void**);
static PFN_CreateDXGIFactory oCreateDXGIFactory = NULL;

typedef HRESULT(WINAPI* PFN_CreateDXGIFactory1)(REFIID, void**);
static PFN_CreateDXGIFactory1 oCreateDXGIFactory1 = NULL;

typedef HRESULT(WINAPI* PFN_CreateDXGIFactory2)(UINT, REFIID, void**);
static PFN_CreateDXGIFactory2 oCreateDXGIFactory2 = NULL;

typedef HRESULT(STDMETHODCALLTYPE* CreateSwapChain_t)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                      IDXGISwapChain**);
static CreateSwapChain_t oCreateSwapChain = NULL;

typedef HRESULT(STDMETHODCALLTYPE* CreateSwapChainForHwnd_t)(IDXGIFactory2*, IUnknown*, HWND,
                                                             const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                             IDXGISwapChain1**);
static CreateSwapChainForHwnd_t oCreateSwapChainForHwnd = NULL;

static ResizeBuffers_t oResizeBuffers = NULL;

// Forward Declarations (non-static for cross-file hook collision detection from
// dx12_hook.cpp) Helper to get VSync override settings (reduces duplication)
static VSyncOverride GetDX11VSyncOverride() {
    return GetVSyncOverride();  // Use the shared helper from hook_common.h
}

static bool ShouldSkipWindowForNvPresent(HWND hwnd) {
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

// Forward Declarations
void CleanupDX11Resources(bool releaseDeviceContext = true);
void HandleDX11ProcessFrame(IDXGISwapChain* pSwapChain, bool isRealFrame);
void DrawDX11Overlay(IDXGISwapChain* pSwapChain);
static void ProcessDX11FrameWithOverlayOrdering(IDXGISwapChain* pSwapChain);
static void InstallVTableHooks(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, IDXGISwapChain* pSwapChain);

static float ResolveDX11PrerenderLimit() {
    return GetActivePrerenderLimit();
}

HRESULT STDMETHODCALLTYPE DetourDX11Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    // Performance metrics for this frame
    FrameMetrics perfMetrics;
    perfMetrics.qpcUs = PerfLogger::GetQpcUs();
    strncpy(perfMetrics.api, "DX11", sizeof(perfMetrics.api) - 1);
    perfMetrics.api[sizeof(perfMetrics.api) - 1] = '\0';
    static uint64_t s_perfFrameNum = 0;
    perfMetrics.frameNum = ++s_perfFrameNum;
    if (g_pSharedMem) {
        perfMetrics.sourceFrameIndex = DXGIShared::GetLatestSourceFrameIndex();
        perfMetrics.sourceCapturePhase = g_pSharedMem->runtimeState.capturePhase.load(std::memory_order_relaxed);
        perfMetrics.sourceEncoderQueueDepth = g_pSharedMem->encoderQueueDepth.load(std::memory_order_relaxed);
        perfMetrics.sourceMuxQueueKb =
            (g_pSharedMem->runtimeState.muxQueueBytes.load(std::memory_order_relaxed) + 1023u) / 1024u;
        perfMetrics.sourceOverloadFlags =
            g_pSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
    }
    if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
        perfMetrics.sourceCurrentFpsTimes100 = static_cast<int32_t>(perf->GetCurrentFPS() * 100.0f + 0.5f);
        perfMetrics.source1PctLowTimes100 = static_cast<int32_t>(perf->Get1PercentLowFPS() * 100.0f + 0.5f);
        perfMetrics.sourcePoint1PctLowTimes100 = static_cast<int32_t>(perf->Get01PercentLowFPS() * 100.0f + 0.5f);
        perfMetrics.sourceFrameTimeStdDevUs = static_cast<int32_t>(perf->GetWindowStdDev() + 0.5);
    }

    // Scope guard to log metrics on any exit path
    auto perfGuard = ce::make_scope_guard([&]() {
        if (PerfLogger::Get().IsEnabled()) {
            perfMetrics.totalUs = static_cast<int32_t>((PerfLogger::GetQpcUs() - perfMetrics.qpcUs));
            PerfLogger::Get().LogFrame(perfMetrics);
        }
    });

    // Skip performance logging if disabled
    if (!PerfLogger::Get().IsEnabled()) {
        perfGuard.dismiss();
    }

    // CRITICAL ULTIMATE FIX: If shutdown flag is set, return immediately WITHOUT
    // touching ANYTHING - no wrapper checks, no GetDesc, nothing. Just return.
    // The device may already be destroyed and any D3D call can crash.
    if (HookIsShuttingDown()) {
        return S_OK;
    }

    // WRAPPER ARCHITECTURE: Skip if called from within wrapper's Present
    // The wrapper sets a thread-local flag before calling the real Present
    extern bool IsInWrapperPresent();
    bool inWrapper = IsInWrapperPresent();
    static std::atomic<int> s_LogCount{0};
    if (s_LogCount.fetch_add(1) < 10) {
        HookLog("DetourDX11Present: IsInWrapperPresent=%d", inWrapper);
    }
    if (inWrapper) {
        // Wrapper is handling everything, just call original
        return oPresent(pSwapChain, SyncInterval, Flags);
    }

    // Re-entrancy guard: prevents mutual recursion with other Present hooks (e.g. Steam overlay)
    if (g_InPresentHook) {
        return oPresent(pSwapChain, SyncInterval, Flags);
    }
    g_InPresentHook = true;
    auto hookGuard = ce::make_scope_guard([&] { g_InPresentHook = false; });

    // SAFETY CHECK: Verify window is still valid before doing ANY D3D work
    // This prevents crashes when the app is shutting down and destroying its
    // window
    const bool nvPresentLoaded = g_FGCompat.IsNvPresentLoaded();
    DXGI_SWAP_CHAIN_DESC desc;
    if (SUCCEEDED(pSwapChain->GetDesc(&desc))) {
        // NVIDIA Smooth Motion compatibility: NvPresent64 can present through
        // hidden/ephemeral windows that must be passed through untouched.
        if (nvPresentLoaded && ShouldSkipWindowForNvPresent(desc.OutputWindow)) {
            return oPresent(pSwapChain, SyncInterval, Flags);
        }

        if (!desc.OutputWindow || !IsWindow(desc.OutputWindow)) {
            // Window is being destroyed - app is shutting down
            // Set shutdown flag and bail immediately without touching any D3D objects
            RequestHookShutdown();
            EarlyLog("DX11: Window destroyed (hwnd=%p), entering shutdown mode", desc.OutputWindow);
            return S_OK;
        }
    }

    // Process VSync Override
    VSyncOverride override = GetDX11VSyncOverride();
    if (override.shouldOverride) {
        // Skip forcing VSync when the FPS limiter is actively pacing frames.
        // The limiter's sleep-before-Present controls frame timing; SyncInterval=1
        // would add a vblank wait after and override our target FPS.
        if (!g_SharedFpsLimiter.IsActivelyLimiting()) {
            SyncInterval = override.presentInterval;
        }
        if (override.useMailbox) {
            Flags |= DXGI_PRESENT_ALLOW_TEARING;
        }
    }

    // Non-wrapper path: Draw overlay via vtable hook
    int64_t overlayStartUs = PerfLogger::GetQpcUs();
    HandleDX11ProcessFrame(pSwapChain, true);
    perfMetrics.overlayUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - overlayStartUs);

    // If window was invalid during overlay rendering, skip Present to avoid crash
    // The app is already tearing down its D3D resources
    if (HookIsShuttingDown()) {
        return S_OK;  // Return success to avoid cascading errors
    }

    // CPU Prerender Limit
    float prerenderLimit = ResolveDX11PrerenderLimit();
    if (prerenderLimit >= 0.0f) {
        int64_t prerenderStartUs = PerfLogger::GetQpcUs();
        ApplyPrerenderLimit(pSwapChain, prerenderLimit);
        perfMetrics.prerenderWaitUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - prerenderStartUs);
    }

    // FPS Limiter
    int64_t fpsLimitStartUs = PerfLogger::GetQpcUs();
    g_SharedFpsLimiter.Apply();
    perfMetrics.fpsLimitWaitUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - fpsLimitStartUs);

    return oPresent(pSwapChain, SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE DetourDX11Present1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT PresentFlags,
                                             const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    // CRITICAL: Check for shutdown first - if app is closing, don't touch
    // anything
    if (HookIsShuttingDown()) {
        return S_OK;
    }

    // WRAPPER ARCHITECTURE: Skip if called from within wrapper's Present
    extern bool IsInWrapperPresent();
    if (IsInWrapperPresent()) {
        // Wrapper is handling everything, just call original
        if (oPresent1)
            return oPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
        return oPresent(pSwapChain, SyncInterval, PresentFlags);
    }

    // SAFETY CHECK: Verify window is still valid before doing ANY D3D work
    const bool nvPresentLoaded = g_FGCompat.IsNvPresentLoaded();
    DXGI_SWAP_CHAIN_DESC desc;
    if (SUCCEEDED(pSwapChain->GetDesc(&desc))) {
        // NVIDIA Smooth Motion compatibility: pass through hidden/ephemeral windows
        if (nvPresentLoaded && ShouldSkipWindowForNvPresent(desc.OutputWindow)) {
            if (oPresent1)
                return oPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
            return oPresent(pSwapChain, SyncInterval, PresentFlags);
        }

        if (!desc.OutputWindow || !IsWindow(desc.OutputWindow)) {
            RequestHookShutdown();
            EarlyLog("DX11: Window destroyed (hwnd=%p), entering shutdown mode", desc.OutputWindow);
            return S_OK;
        }
    }

    // Vulkan coordination: Skip DX11 overlay if Vulkan Layer is active AND
    // presenting.
    if (g_pSharedMem) {
        uint64_t lastVulkan = g_pSharedMem->runtimeState.vulkanPresentTick.load(std::memory_order_acquire);
        if (g_pSharedMem->runtimeState.vulkanLayerActive && (GetTickCount64() - lastVulkan < 200)) {
            if (oPresent1)
                return oPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
            return oPresent(pSwapChain, SyncInterval, PresentFlags);
        }
    }

    // SAFETY CHECK: DX12 Detection
    // If this swapchain is actually DX12, we must NOT draw the DX11 overlay.
    {
        ID3D12Device* d12Dev = nullptr;
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D12Device), (void**)&d12Dev))) {
            d12Dev->Release();

            // DX12 detected. Skip DX11 overlay but delegate to DX12 for frame
            // processing.
            static bool s_LoggedDX12Mismatch = false;
            if (!s_LoggedDX12Mismatch) {
                HookLog(
                    "DX11: DetourDX11Present1 - DX12 Device detected! Delegating "
                    "to DX12_ProcessFrameExternal.");
                s_LoggedDX12Mismatch = true;
            }

            // Delegate to DX12 hook for overlay/capture processing
            extern void DX12_ProcessFrameExternal(IDXGISwapChain * pSwapChain);
            DX12_ProcessFrameExternal(pSwapChain);

            if (oPresent1)
                return oPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
            return oPresent(pSwapChain, SyncInterval, PresentFlags);
        }
    }

    // Re-entrancy guard: prevents mutual recursion with other Present hooks (e.g. Steam overlay)
    if (g_InPresentHook) {
        if (oPresent1)
            return oPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
        return oPresent(pSwapChain, SyncInterval, PresentFlags);
    }
    g_InPresentHook = true;
    auto hookGuard = ce::make_scope_guard([&] { g_InPresentHook = false; });

    // Process VSync Override
    VSyncOverride override = GetDX11VSyncOverride();
    if (override.shouldOverride) {
        SyncInterval = override.presentInterval;
        if (override.useMailbox) {
            PresentFlags |= DXGI_PRESENT_ALLOW_TEARING;
        }
    }

    // Non-wrapper path: Process overlay, capture, and screenshots via the shared ordering helper
    HandleDX11ProcessFrame(pSwapChain, true);

    // If window was invalid during overlay rendering, skip Present to avoid crash
    if (HookIsShuttingDown()) {
        return S_OK;
    }

    // CPU Prerender Limit
    float prerenderLimit = ResolveDX11PrerenderLimit();
    if (prerenderLimit >= 0.0f) {
        ApplyPrerenderLimit(pSwapChain, prerenderLimit);
    }

    if (oPresent1)
        return oPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
    // Fallback to Present if Present1 not hooked (should not happen if vtable
    // hooked correctly)
    return oPresent(pSwapChain, SyncInterval, PresentFlags);
}

static HRESULT WINAPI DetourD3D11CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType,
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
        int count = gfx.backbufferCount;
        if (count >= 2 && count <= 6) {
            bool isFlip = (desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                           desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            if (isFlip && (UINT)count < desc.BufferCount) {
                HookLog(
                    "DX11: CreateDeviceAndSwapChain: Skipping BufferCount override "
                    "%d < game's %u (flip model)",
                    count, desc.BufferCount);
            } else {
                desc.BufferCount = (UINT)count;
                modified = true;
                HookLog("DX11: CreateDeviceAndSwapChain: Overriding BufferCount to %d", count);
            }
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
    PFN_D3D11CreateDeviceAndSwapChain realOriginal = s_oRealD3D11CreateDeviceAndSwapChain
                                                         ? s_oRealD3D11CreateDeviceAndSwapChain
                                                         : oD3D11CreateDeviceAndSwapChain;
    HRESULT hr =
        realOriginal(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
                     pFinalDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);

    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
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

// Exported version of the detour for IAT patching access
HRESULT WINAPI DX11_DetourCreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
                                                   UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels,
                                                   UINT FeatureLevels, UINT SDKVersion,
                                                   const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                                   IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
                                                   D3D_FEATURE_LEVEL* pFeatureLevel,
                                                   ID3D11DeviceContext** ppImmediateContext) {
    return DetourD3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
                                               SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel,
                                               ppImmediateContext);
}

void DX11Hook_OnSwapChainCreated(IDXGISwapChain* pSwapChain);

// Update metrics for wrapper calls
void DX11_UpdatePerformanceMetrics(int64_t qpcUs);

void DX11Hook_OnSwapChainCreated(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return;

    // Check if it's really a D3D11 swapchain
    ID3D11Device* pDevice = nullptr;
    if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&pDevice))) {
        ID3D11DeviceContext* pContext = nullptr;
        pDevice->GetImmediateContext(&pContext);

        HookLog("DX11: Manual Hook Activation triggered for SwapChain %p", pSwapChain);
        InstallVTableHooks(pDevice, pContext, pSwapChain);

        if (pContext)
            pContext->Release();
        pDevice->Release();
    }
}

static HRESULT WINAPI DetourD3D10CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D10_DRIVER_TYPE DriverType,
                                                          HMODULE Software, UINT Flags, UINT SDKVersion,
                                                          DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                                          IDXGISwapChain** ppSwapChain, ID3D10Device** ppDevice) {
    EarlyLog("DX10: D3D10CreateDeviceAndSwapChain called");

    DXGI_SWAP_CHAIN_DESC desc;
    DXGI_SWAP_CHAIN_DESC* pFinalDesc = pSwapChainDesc;

    if (pSwapChainDesc) {
        const GraphicsConfig& gfx = GetActiveGraphicsConfig();
        desc = *pSwapChainDesc;
        bool modified = false;

        int count = gfx.backbufferCount;
        if (count >= 2 && count <= 6) {
            bool isFlip = (desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                           desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            if (!(isFlip && (UINT)count < desc.BufferCount)) {
                desc.BufferCount = (UINT)count;
                modified = true;
            }
        }

        if (modified)
            pFinalDesc = &desc;
    }

    HRESULT hr = oD3D10CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, SDKVersion, pFinalDesc,
                                                ppSwapChain, ppDevice);

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        InstallVTableHooks(NULL, NULL, *ppSwapChain);
    }
    return hr;
}

static HRESULT WINAPI DetourD3D10CreateDeviceAndSwapChain1(IDXGIAdapter* pAdapter, D3D10_DRIVER_TYPE DriverType,
                                                           HMODULE Software, UINT Flags,
                                                           D3D10_FEATURE_LEVEL1 HardwareLevel, UINT SDKVersion,
                                                           DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                                           IDXGISwapChain** ppSwapChain, ID3D10Device1** ppDevice) {
    EarlyLog("DX10.1: D3D10CreateDeviceAndSwapChain1 called");

    DXGI_SWAP_CHAIN_DESC desc;
    DXGI_SWAP_CHAIN_DESC* pFinalDesc = pSwapChainDesc;

    if (pSwapChainDesc) {
        const GraphicsConfig& gfx = GetActiveGraphicsConfig();
        desc = *pSwapChainDesc;
        bool modified = false;

        int count = gfx.backbufferCount;
        if (count >= 2 && count <= 6) {
            bool isFlip = (desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                           desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            if (!(isFlip && (UINT)count < desc.BufferCount)) {
                desc.BufferCount = (UINT)count;
                modified = true;
            }
        }

        if (modified)
            pFinalDesc = &desc;
    }

    HRESULT hr = oD3D10CreateDeviceAndSwapChain1(pAdapter, DriverType, Software, Flags, HardwareLevel, SDKVersion,
                                                 pFinalDesc, ppSwapChain, ppDevice);

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        InstallVTableHooks(NULL, NULL, *ppSwapChain);
    }
    return hr;
}

static HRESULT WINAPI DetourD3D10CreateDevice(IDXGIAdapter* pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software,
                                              UINT Flags, UINT SDKVersion, ID3D10Device** ppDevice) {
    return oD3D10CreateDevice(pAdapter, DriverType, Software, Flags, SDKVersion, ppDevice);
}

static HRESULT WINAPI DetourD3D10CreateDevice1(IDXGIAdapter* pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software,
                                               UINT Flags, D3D10_FEATURE_LEVEL1 HardwareLevel, UINT SDKVersion,
                                               ID3D10Device1** ppDevice) {
    return oD3D10CreateDevice1(pAdapter, DriverType, Software, Flags, HardwareLevel, SDKVersion, ppDevice);
}

static HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory* pFactory, IUnknown* pDevice,
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

    HRESULT hr = oCreateSwapChain(pFactory, DeWrap(pDevice), pDesc, ppSwapChain);

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        // FSR4/FG swapchain recreation detection (shared with
        // CreateSwapChainForHwnd)
        bool isGameSizedSwapchain = pDesc && pDesc->BufferDesc.Width >= 1920 && pDesc->BufferDesc.Height >= 1080;
        if (isGameSizedSwapchain) {
            if (g_FirstGameSwapchainCreated) {
                // Recreation - likely FG taking over
                HookLog(
                    "DX11: CreateSwapChain: Game-sized swapchain recreated - "
                    "invalidating DX12 overlay");
                DX12_SignalFSR4SwapchainRecreated();
            } else {
                g_FirstGameSwapchainCreated = true;
                HookLog("DX11: CreateSwapChain: First game-sized swapchain created (%ux%u)", pDesc->BufferDesc.Width,
                        pDesc->BufferDesc.Height);
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

static HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd,
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

    // CRITICAL FG FIX: Track game-sized swapchain recreation for FG overlay
    // safety We DON'T invalidate BEFORE creation - that can cause DXGI lock
    // issues (E_ACCESSDENIED) Instead, we invalidate AFTER successful recreation
    // to clean up stale overlay resources
    bool isGameSizedSwapchain = pDesc && pDesc->Width >= 1920 && pDesc->Height >= 1080;
    bool wasRecreation = isGameSizedSwapchain && g_FirstGameSwapchainCreated;

    HookLog("DX11: BEFORE oCreateSwapChainForHwnd call");
    HRESULT hr = oCreateSwapChainForHwnd(pFactory, DeWrap(pDevice), hWnd, pDesc, pFullscreenDesc, pRestrictToOutput,
                                         ppSwapChain);
    HookLog("DX11: AFTER oCreateSwapChainForHwnd call (hr=0x%08X)", hr);

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
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
            if (!g_FirstGameSwapchainCreated) {
                g_FirstGameSwapchainCreated = true;
                HookLog(
                    "DX11: CreateSwapChainForHwnd: First game-sized swapchain "
                    "created (%ux%u)",
                    pDesc->Width, pDesc->Height);
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

static HRESULT STDMETHODCALLTYPE DetourCreateSamplerState(ID3D11Device* pDevice, const D3D11_SAMPLER_DESC* pSamplerDesc,
                                                          ID3D11SamplerState** ppSamplerState);
static void STDMETHODCALLTYPE DetourPSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);
static void STDMETHODCALLTYPE DetourVSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);
static void STDMETHODCALLTYPE DetourGSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);
static void STDMETHODCALLTYPE DetourHSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);
static void STDMETHODCALLTYPE DetourDSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);
static void STDMETHODCALLTYPE DetourCSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);
static void STDMETHODCALLTYPE DetourPSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);
static void STDMETHODCALLTYPE DetourVSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);
static void STDMETHODCALLTYPE DetourGSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);
static void STDMETHODCALLTYPE DetourHSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);
static void STDMETHODCALLTYPE DetourDSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);
static void STDMETHODCALLTYPE DetourCSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);
static HRESULT STDMETHODCALLTYPE DetourCreateSamplerState10(ID3D10Device* pDevice,
                                                            const D3D10_SAMPLER_DESC* pSamplerDesc,
                                                            ID3D10SamplerState** ppSamplerState);
static void STDMETHODCALLTYPE DetourPSSetSamplers10(ID3D10Device* pDevice, UINT StartSlot, UINT NumSamplers,
                                                    ID3D10SamplerState* const* ppSamplers);
static void STDMETHODCALLTYPE DetourVSSetSamplers10(ID3D10Device* pDevice, UINT StartSlot, UINT NumSamplers,
                                                    ID3D10SamplerState* const* ppSamplers);
static void STDMETHODCALLTYPE DetourGSSetSamplers10(ID3D10Device* pDevice, UINT StartSlot, UINT NumSamplers,
                                                    ID3D10SamplerState* const* ppSamplers);
static void STDMETHODCALLTYPE DetourDraw10(ID3D10Device* pDevice, UINT VertexCount, UINT StartVertexLocation);
static void STDMETHODCALLTYPE DetourDrawIndexed10(ID3D10Device* pDevice, UINT IndexCount, UINT StartIndexLocation,
                                                  INT BaseVertexLocation);
static void STDMETHODCALLTYPE DetourDrawInstanced10(ID3D10Device* pDevice, UINT VertexCountPerInstance,
                                                    UINT InstanceCount, UINT StartVertexLocation,
                                                    UINT StartInstanceLocation);
static void STDMETHODCALLTYPE DetourDrawIndexedInstanced10(ID3D10Device* pDevice, UINT IndexCountPerInstance,
                                                           UINT InstanceCount, UINT StartIndexLocation,
                                                           INT BaseVertexLocation, UINT StartInstanceLocation);

// Helper to install vtable hooks
static void InstallVTableHooks(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, IDXGISwapChain* pSwapChain) {
    // Hook D3D11 Device methods
    if (pDevice) {
        void** pDeviceVTable = *(void***)pDevice;
        if (oCreateSamplerState == NULL) {
            // Index 23 is CreateSamplerState for D3D11
            if (VTableHook::Create(&pDeviceVTable[23], (LPVOID)&DetourCreateSamplerState,
                                   (LPVOID*)&oCreateSamplerState) == VTableHook::Success) {
                HookLog("DX11: CreateSamplerState hook installed");
            }
        }
    }

    if (pContext) {
        void** pContextVTable = *(void***)pContext;

        if (oPSSetShaderResources11 == NULL) {
            if (VTableHook::Create(&pContextVTable[8], (LPVOID)&DetourPSSetShaderResources11,
                                   (LPVOID*)&oPSSetShaderResources11) == VTableHook::Success) {
                HookLog("DX11: PSSetShaderResources hook installed");
            }
        }
        if (oPSSetSamplers11 == NULL) {
            if (VTableHook::Create(&pContextVTable[10], (LPVOID)&DetourPSSetSamplers11, (LPVOID*)&oPSSetSamplers11) ==
                VTableHook::Success) {
                HookLog("DX11: PSSetSamplers hook installed");
            }
        }
        if (oVSSetShaderResources11 == NULL) {
            if (VTableHook::Create(&pContextVTable[25], (LPVOID)&DetourVSSetShaderResources11,
                                   (LPVOID*)&oVSSetShaderResources11) == VTableHook::Success) {
                HookLog("DX11: VSSetShaderResources hook installed");
            }
        }
        if (oVSSetSamplers11 == NULL) {
            if (VTableHook::Create(&pContextVTable[26], (LPVOID)&DetourVSSetSamplers11, (LPVOID*)&oVSSetSamplers11) ==
                VTableHook::Success) {
                HookLog("DX11: VSSetSamplers hook installed");
            }
        }
        if (oGSSetShaderResources11 == NULL) {
            if (VTableHook::Create(&pContextVTable[31], (LPVOID)&DetourGSSetShaderResources11,
                                   (LPVOID*)&oGSSetShaderResources11) == VTableHook::Success) {
                HookLog("DX11: GSSetShaderResources hook installed");
            }
        }
        if (oGSSetSamplers11 == NULL) {
            if (VTableHook::Create(&pContextVTable[32], (LPVOID)&DetourGSSetSamplers11, (LPVOID*)&oGSSetSamplers11) ==
                VTableHook::Success) {
                HookLog("DX11: GSSetSamplers hook installed");
            }
        }
        if (oHSSetShaderResources11 == NULL) {
            if (VTableHook::Create(&pContextVTable[59], (LPVOID)&DetourHSSetShaderResources11,
                                   (LPVOID*)&oHSSetShaderResources11) == VTableHook::Success) {
                HookLog("DX11: HSSetShaderResources hook installed");
            }
        }
        if (oHSSetSamplers11 == NULL) {
            if (VTableHook::Create(&pContextVTable[61], (LPVOID)&DetourHSSetSamplers11, (LPVOID*)&oHSSetSamplers11) ==
                VTableHook::Success) {
                HookLog("DX11: HSSetSamplers hook installed");
            }
        }
        if (oDSSetShaderResources11 == NULL) {
            if (VTableHook::Create(&pContextVTable[63], (LPVOID)&DetourDSSetShaderResources11,
                                   (LPVOID*)&oDSSetShaderResources11) == VTableHook::Success) {
                HookLog("DX11: DSSetShaderResources hook installed");
            }
        }
        if (oDSSetSamplers11 == NULL) {
            if (VTableHook::Create(&pContextVTable[65], (LPVOID)&DetourDSSetSamplers11, (LPVOID*)&oDSSetSamplers11) ==
                VTableHook::Success) {
                HookLog("DX11: DSSetSamplers hook installed");
            }
        }
        if (oCSSetShaderResources11 == NULL) {
            if (VTableHook::Create(&pContextVTable[67], (LPVOID)&DetourCSSetShaderResources11,
                                   (LPVOID*)&oCSSetShaderResources11) == VTableHook::Success) {
                HookLog("DX11: CSSetShaderResources hook installed");
            }
        }
        if (oCSSetSamplers11 == NULL) {
            if (VTableHook::Create(&pContextVTable[70], (LPVOID)&DetourCSSetSamplers11, (LPVOID*)&oCSSetSamplers11) ==
                VTableHook::Success) {
                HookLog("DX11: CSSetSamplers hook installed");
            }
        }
    }

    // Some DX11 implementations expose D3D10 compatibility interfaces too.
    // Only install the D3D10 runtime hooks when the swapchain actually belongs
    // to a D3D10 device.
    if (pSwapChain && DetectSwapChainAPITypeForDX11Hook(pSwapChain) == DXGIShared::APIType::D3D10) {
        ID3D10Device* pDevice10 = nullptr;
        HRESULT hr = pSwapChain->GetDevice(__uuidof(ID3D10Device), (void**)&pDevice10);
        if (SUCCEEDED(hr) && pDevice10) {
            void** pDeviceVTable = *(void***)pDevice10;

            // CreateSamplerState (Index 9)
            if (oCreateSamplerState10 == NULL) {
                if (VTableHook::Create(&pDeviceVTable[9], (LPVOID)&DetourCreateSamplerState10,
                                       (LPVOID*)&oCreateSamplerState10) == VTableHook::Success) {
                    HookLog("DX10: CreateSamplerState hook installed");
                }
            }
            pDevice10->Release();
        }
    }
}

// Update metrics for wrapper calls
void DX11_UpdatePerformanceMetrics(int64_t qpcUs) {
    if (auto* m = DXGIShared::GetPerformanceMetrics()) {
        m->Update(qpcUs);
    }
}

void CleanupDX11Resources(bool releaseDeviceContext) {
    ReleaseTrackedShaderResources11();

    // When the window is being destroyed (releaseDeviceContext=false), skip ALL
    // releases because the underlying D3D device is already being torn down.
    // Calling Release() on any resource (even RTVs) can crash at this point.
    if (releaseDeviceContext) {
        if (g_mainRenderTargetView) {
            g_mainRenderTargetView->Release();
            g_mainRenderTargetView = nullptr;
        }
        if (g_mainRenderTargetView10) {
            g_mainRenderTargetView10->Release();
            g_mainRenderTargetView10 = nullptr;
        }
    } else {
        g_mainRenderTargetView = nullptr;
        g_mainRenderTargetView10 = nullptr;
    }

    // CRITICAL: Only call Shutdown() when doing normal cleanup (resize, etc.)
    // During app shutdown (releaseDeviceContext=false), skip Shutdown() - the
    // OverlayAdapter destructor will handle cleanup by leaking memory since
    // we've set skipDeviceRelease=true.
    if (releaseDeviceContext && g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    if (releaseDeviceContext) {
        if (g_pd3dDeviceContext) {
            g_pd3dDeviceContext->Release();
            g_pd3dDeviceContext = nullptr;
        }
        if (g_pd3dDevice) {
            g_pd3dDevice->Release();
            g_pd3dDevice = nullptr;
        }
    } else {
        g_pd3dDeviceContext = nullptr;
        g_pd3dDevice = nullptr;
    }
}

extern void DrawDX11Overlay(IDXGISwapChain* pSwapChain);

void HandleDX11ProcessFrame(IDXGISwapChain* pSwapChain, bool isRealFrame) {
    (void)isRealFrame;
    ProcessDX11FrameWithOverlayOrdering(pSwapChain);
}

void HandleDX11ResizeBegin() {
    CleanupDX11Resources();
}
static HWND g_CachedHwnd = NULL;

// Reentrancy guard for ResizeBuffers (Recursion Breaker)
thread_local int g_ResizeBuffersDepth = 0;

// DX11 Capture Implementation extending HookCaptureBase
// Supports both DX11 and DX10 games.
// DX10 capture must copy on the real D3D10 device and publish DXGI shared
// texture handles that the media-side D3D11 device can open.
class DX11Capture : public HookCaptureBase {
public:
    ID3D11Texture2D* sharedTextures[CAPTURE_TEXTURE_COUNT]{};
    ID3D11Query* copyQueries[CAPTURE_TEXTURE_COUNT]{};  // GPU sync queries
    ID3D11Device* cachedDevice = nullptr;
    ID3D11DeviceContext* cachedContext = nullptr;

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

    // Keyed Mutex Support (Proper Fix)
    IDXGIKeyedMutex* keyedMutexes[CAPTURE_TEXTURE_COUNT]{};
    bool useKeyedMutex = false;

    // sharedTextureHandles are in base class

    void Cleanup() override {
        CaptureBase::StopCaptureThread();
        // Close shared handles first to prevent leaks
        CaptureBase::CleanupSharedHandles();

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            g_DeferredRelease.Queue(sharedTextures[i]);
            sharedTextures[i] = nullptr;

            g_DeferredRelease.Queue(copyQueries[i]);
            copyQueries[i] = nullptr;

            g_DeferredRelease.Queue(sharedTextures10[i]);
            sharedTextures10[i] = nullptr;

            g_DeferredRelease.Queue(copyQueries10[i]);
            copyQueries10[i] = nullptr;

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

        cachedDevice = nullptr;
        cachedContext = nullptr;
        initialized = false;
        useFences = false;
        useKeyedMutex = false;
        isDX10Mode = false;
        isDXVKMode = false;
        fenceValue = 0;  // Reset fence value for next session
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

        DXGI_SWAP_CHAIN_DESC desc;
        swapChain->GetDesc(&desc);

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
                IDXGIResource1* pResource1 = NULL;
                // Use IDXGIResource1 for NT Handles
                if (SUCCEEDED(sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&pResource1)))) {
                    HANDLE hTemp = NULL;
                    hr = pResource1->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                        NULL, &hTemp);
                    sharedTextureHandles[i].store(hTemp, std::memory_order_release);
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
        static int s_captureFrameCount = 0;
        int frameNum = ++s_captureFrameCount;

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
            if (frameNum <= 20 || frameNum % 60 == 0) {
                HookLog("DX10Capture: [%d] Copying to texture %d (writeIndex=%d)", frameNum, writeIdx,
                        writeIndex.load());
            }

            if (copyQueries10[writeIdx]) {
                BOOL data = FALSE;
                HRESULT queryHr = copyQueries10[writeIdx]->GetData(&data, sizeof(data), 0);
                if (queryHr == S_FALSE) {
                    // Previous copy to this slot is still pending. We keep the current
                    // non-blocking behavior and rely on the ring depth to absorb it.
                }
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
        if (!isDX10Mode && g_CaptureUsesOverlayRTV && g_mainRenderTargetView) {
            ID3D11Resource* rtResource = nullptr;
            g_mainRenderTargetView->GetResource(&rtResource);
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

        if (frameNum <= 20 || frameNum % 60 == 0) {
            HookLog("DX11Capture: [%d] Copying to texture %d (writeIndex=%d)", frameNum, writeIdx, writeIndex.load());
        }

        // Check if this slot is still in use by encoder (non-blocking check)
        if (copyQueries[writeIdx]) {
            // Quick check without stall - if not ready, we'll issue the copy anyway
            // and let the query catch it next frame. The ring buffer depth (8)
            // provides enough padding that this is usually fine.
            BOOL data = FALSE;
            HRESULT hr = context->GetData(copyQueries[writeIdx], &data, sizeof(data), 0);
            if (hr == S_FALSE) {
                // Query still pending - frame may be dropped if encoder is slow
                // But we proceed anyway and let EnqueueFrame handle ring buffer full
            }
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
            context4->Signal(fence, currentFenceValue);
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

static DX11Capture g_DX11Capture;

static OverlayConfig GetActiveDX11OverlayConfig(SharedMemoryLayout* shm) {
    OverlayConfig cfg{};
    cfg.captureIncludeOverlay = true;
    cfg.screenshotIncludeOverlay = true;
    if (shm) {
        cfg = shm->ReadOverlayConfig();
    }
    return cfg;
}

static void CompleteRequestedDX11Screenshot(SharedMemoryLayout* shm) {
    if (!shm)
        return;

    shm->runtimeState.cmdTakeScreenshot.store(false, std::memory_order_release);
    shm->runtimeState.ackScreenshotTaken.store(true, std::memory_order_release);
    shm->runtimeState.notificationType.store(1, std::memory_order_release);
    shm->runtimeState.notificationExpiry.store(GetTickCount64() + 3000ULL, std::memory_order_release);
}

static void CaptureDX11Screenshot(IDXGISwapChain* pSwapChain, SharedMemoryLayout* shm) {
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
                D3D11_TEXTURE2D_DESC bbDesc;
                backbuffer->GetDesc(&bbDesc);
                bool isHDR =
                    (bbDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM || bbDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
                if (isHDR) {
                    bool isPQ = (bbDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
                    std::string rawPath(shm->runtimeState.screenshotPath);
                    rawPath += ".raw";
                    SaveD3D11TextureAsHDR(device, context, backbuffer, isPQ, rawPath.c_str());
                } else {
                    SaveD3D11TextureAsBMP(device, context, backbuffer, shm->runtimeState.screenshotPath);
                }
                context->Release();
            }
            device->Release();
        }
        backbuffer->Release();
    }

    CompleteRequestedDX11Screenshot(shm);
}

static void CaptureDX10Screenshot(IDXGISwapChain* pSwapChain, SharedMemoryLayout* shm) {
    ID3D10Device* device = nullptr;
    if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D10Device), (void**)&device))) {
        ID3D10Device1* device10_1 = nullptr;
        if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D10Device1), (void**)&device10_1))) {
            CompleteRequestedDX11Screenshot(shm);
            return;
        }
        device = device10_1;
    }

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
                WriteBMPFileAsync(shm->runtimeState.screenshotPath, static_cast<const uint8_t*>(mapped.pData),
                                  bbDesc.Width, bbDesc.Height, mapped.RowPitch);
                staging->Unmap(0);
            }
            staging->Release();
        }
        backbuffer->Release();
    }

    device->Release();
    CompleteRequestedDX11Screenshot(shm);
}

static void CaptureRequestedDX11Screenshot(IDXGISwapChain* pSwapChain, SharedMemoryLayout* shm) {
    if (!shm)
        return;

    const DXGIShared::APIType swapChainApi = DetectSwapChainAPITypeForDX11Hook(pSwapChain);
    if (swapChainApi == DXGIShared::APIType::D3D10) {
        CaptureDX10Screenshot(pSwapChain, shm);
        return;
    }

    if (swapChainApi != DXGIShared::APIType::D3D11) {
        HookLog("DX11 Screenshot: Unsupported swapchain API %s", GetDX11HookBaseAPIName(swapChainApi));
        CompleteRequestedDX11Screenshot(shm);
        return;
    }

    CaptureDX11Screenshot(pSwapChain, shm);
}

static void ProcessDX11FrameWithOverlayOrdering(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return;

    UINT frameBufferIndex = ResolveDX11BackBufferIndex(pSwapChain);
    g_ForcedCaptureBackBufferIndex = static_cast<int>(frameBufferIndex);
    auto indexGuard = ce::make_scope_guard([]() { g_ForcedCaptureBackBufferIndex = -1; });

    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    OverlayConfig overlayCfg = GetActiveDX11OverlayConfig(shm);
    const bool shouldDrawOverlay = shm && overlayCfg.showOverlay;
    const bool captureAfterOverlay = shouldDrawOverlay && overlayCfg.captureIncludeOverlay;
    const bool screenshotRequested = shm && shm->runtimeState.cmdTakeScreenshot.load(std::memory_order_acquire);
    const bool screenshotAfterOverlay = shouldDrawOverlay && overlayCfg.screenshotIncludeOverlay;

    auto doCapture = [&](bool afterOverlay) {
        if (g_IPC && g_IPC->IsRecording() && !ShouldSkipCaptureForTargetCadence(shm, "DX11")) {
            g_CaptureUsesOverlayRTV = afterOverlay;
            auto captureGuard = ce::make_scope_guard([]() { g_CaptureUsesOverlayRTV = false; });
            g_DX11Capture.CaptureFrame(pSwapChain);
        }
    };

    auto doScreenshot = [&]() {
        if (screenshotRequested) {
            CaptureRequestedDX11Screenshot(pSwapChain, shm);
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

// Called from DXGI SwapChain wrapper for frame capture (wrapper-only
// architecture)
void DX11_ProcessFrameExternal(IDXGISwapChain* pSwapChain) {
    HandleDX11ProcessFrame(pSwapChain, true);
}

// Helper to force rebind of all samplers (triggering our DetourSetSamplers)
// Returns true if any samplers were actually found and rebound
static bool RebindSamplers10(ID3D10Device* pDevice) {
    if (!pDevice)
        return false;

    bool foundAny = false;
    ID3D10SamplerState* samplers[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT] = {0};

    // Pixel Shader
    pDevice->PSGetSamplers(0, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
    int psCount = 0;
    for (UINT i = 0; i < D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT; i++) {
        if (samplers[i])
            psCount++;
    }

    if (psCount > 0) {
        pDevice->PSSetSamplers(0, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
        foundAny = true;
    }

    for (UINT i = 0; i < D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT; i++) {
        if (samplers[i])
            samplers[i]->Release();
        samplers[i] = nullptr;  // Reset for next stage
    }

    // Vertex Shader
    pDevice->VSGetSamplers(0, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
    int vsCount = 0;
    for (UINT i = 0; i < D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT; i++) {
        if (samplers[i])
            vsCount++;
    }

    if (vsCount > 0) {
        pDevice->VSSetSamplers(0, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
        foundAny = true;
    }

    for (UINT i = 0; i < D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT; i++) {
        if (samplers[i])
            samplers[i]->Release();
        samplers[i] = nullptr;
    }

    // Geometry Shader
    pDevice->GSGetSamplers(0, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
    int gsCount = 0;
    for (UINT i = 0; i < D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT; i++) {
        if (samplers[i])
            gsCount++;
    }

    if (gsCount > 0) {
        pDevice->GSSetSamplers(0, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
        foundAny = true;
    }

    for (UINT i = 0; i < D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT; i++) {
        if (samplers[i])
            samplers[i]->Release();
        samplers[i] = nullptr;
    }

    if (foundAny) {
        HookLog("DX10: Forced sampler rebind (Device=%p, PS=%d, VS=%d, GS=%d)", pDevice, psCount, vsCount, gsCount);
    }

    return foundAny;
}

static void DrawDX10Overlay(IDXGISwapChain* pSwapChain, HWND currentHwnd, int frameCount) {
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

        // Initialize System Metrics
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC adapterDesc;
                if (SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
                    SystemMetricsCollector::Get().Initialize(adapterDesc.AdapterLuid.LowPart,
                                                             adapterDesc.AdapterLuid.HighPart);
                }
                adapter->Release();
            }
            dxgiDevice->Release();
        }

        s_HookedDevice = device;
        s_DidRebind = false;
    }

    // One-time rebind for this device to ensure late initialization is caught
    if (!s_DidRebind && g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        if (RebindSamplers10(device)) {
            s_DidRebind = true;
        }
    }

    // Render the overlay
    if (!g_OverlayAdapter.IsInitialized()) {
        HookLog("DX10: Initializing OverlayAdapter...");
        g_OverlayAdapter.SetHwnd(currentHwnd);
        g_OverlayAdapter.InitDX10(device);
    }

    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
        g_OverlayAdapter.SetIPCClient(g_IPC);
        g_OverlayAdapter.SetGraphicsAPI("DX10");

        RECT rect;
        if (GetClientRect(currentHwnd, &rect)) {
            int width = rect.right - rect.left;
            int height = rect.bottom - rect.top;
            if (width > 0 && height > 0) {
                g_OverlayAdapter.RenderOverlay(width, height);
            }
        }
    }

    device->Release();
}

static bool IsReadableMemoryDX11(const void* ptr, size_t size) {
    if (!ptr)
        return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;
    return (mbi.Protect &
            (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) != 0;
}

void DrawDX11Overlay(IDXGISwapChain* pSwapChain) {
    // CRITICAL: Skip all rendering during shutdown to prevent crashes
    // when D3D device is destroyed while we're trying to use it
    if (HookIsShuttingDown()) {
        return;
    }

    // CRITICAL: Null pointer check
    if (!pSwapChain) {
        return;
    }

    static IDXGISwapChain* lastSwapChain = nullptr;
    static HWND lastHwnd = NULL;
    static int frameCount = 0;
    static IDXGISwapChain* s_lastNvPresentOverlaySwapChain = nullptr;
    static int64_t s_lastNvPresentOverlayUs = 0;
    static UINT s_lastNvPresentOverlayBufferIndex = 0xFFFFFFFFu;

    frameCount++;

    // SAFETY: Verify the swapchain pointer is valid before accessing it
    if (!IsReadableMemoryDX11(pSwapChain, sizeof(void*))) {
        EarlyLog("DX11: Swapchain memory not readable at frame %d — shutting down", frameCount);
        RequestHookShutdown();
        return;
    }

    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(pSwapChain->GetDesc(&desc))) {
        EarlyLog("DX11: GetDesc failed at frame %d — bailing", frameCount);
        return;
    }
    HWND currentHwnd = desc.OutputWindow;
    const bool isFlipSwapchain =
        (desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD || desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
    const UINT resolvedBufferIndex = ResolveDX11BackBufferIndex(pSwapChain, &desc);

    const bool nvPresentLoaded = g_FGCompat.IsNvPresentLoaded();
    if (nvPresentLoaded && ShouldSkipWindowForNvPresent(currentHwnd)) {
        return;
    }

    // NVIDIA Smooth Motion can trigger paired Present callbacks for the same
    // frame in quick succession. Only suppress near-immediate duplicates for the
    // exact same backbuffer to avoid dropping legitimate output frames.
    if (nvPresentLoaded) {
        static constexpr int64_t kNvPresentExactDuplicateUs = 500;
        int64_t nowUs = PerfLogger::GetQpcUs();
        bool hasPreviousSample = (s_lastNvPresentOverlayUs > 0 && nowUs > s_lastNvPresentOverlayUs);
        int64_t deltaUs = hasPreviousSample ? (nowUs - s_lastNvPresentOverlayUs) : 0;
        bool sameSwapChain = (s_lastNvPresentOverlaySwapChain == pSwapChain);
        bool sameBackBuffer = (!isFlipSwapchain || resolvedBufferIndex == s_lastNvPresentOverlayBufferIndex);

        bool skipExactDuplicate =
            sameSwapChain && sameBackBuffer && hasPreviousSample && deltaUs < kNvPresentExactDuplicateUs;

        if (skipExactDuplicate) {
            return;
        }
        s_lastNvPresentOverlaySwapChain = pSwapChain;
        s_lastNvPresentOverlayUs = nowUs;
        s_lastNvPresentOverlayBufferIndex = isFlipSwapchain ? resolvedBufferIndex : 0xFFFFFFFFu;
    }

    // Skip overlay if the window is being destroyed — the D3D device may already
    // be partially torn down, making GetDevice/GetBuffer unsafe.
    if (!currentHwnd || !IsWindow(currentHwnd)) {
        EarlyLog(
            "DX11: Window invalid at frame %d (hwnd=%p, IsWindow=%d) — "
            "shutting down",
            frameCount, currentHwnd, currentHwnd ? IsWindow(currentHwnd) : 0);
        // CRITICAL: Set shutdown flag and tell overlay adapter to skip cleanup
        // The app is tearing down and any Release() call can crash. Let OS clean
        // up.
        RequestHookShutdown();
        g_OverlayAdapter.SetShutdownMode(true);  // Tell adapter to skip destructor cleanup
        return;
    }

    const DXGIShared::APIType swapChainApi = DetectSwapChainAPITypeForDX11Hook(pSwapChain);
    if (swapChainApi == DXGIShared::APIType::D3D10) {
        g_DetectedAPI = GetDX11HookOverlayAPIName(swapChainApi);
        DrawDX10Overlay(pSwapChain, currentHwnd, frameCount);
        return;
    }
    if (swapChainApi != DXGIShared::APIType::D3D11) {
        static std::atomic<int> s_unexpectedApiLogCount{0};
        if (s_unexpectedApiLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            EarlyLog("DX11: DrawOverlay skipping swapchain classified as %s", GetDX11HookBaseAPIName(swapChainApi));
        }
        return;
    }
    g_DetectedAPI = GetDX11HookOverlayAPIName(swapChainApi);

    if (frameCount % 60 == 0) {
        EarlyLog("DX: DrawOverlay frame %d on SC %p (HWND %p, %ux%u)", frameCount, pSwapChain, currentHwnd,
                 desc.BufferDesc.Width, desc.BufferDesc.Height);
    }

    // Acquire device/context — use cached (AddRef'd) pointers for subsequent
    // frames. Calling pSwapChain->GetDevice() every frame is unsafe during
    // shutdown (the swapchain's internal device ref can be freed before our
    // Present hook stops being called).
    if (!g_pd3dDevice) {
        // First frame: cache the DX11 device with AddRef.
        ID3D11Device* device11 = NULL;
        HRESULT hrDevice = pSwapChain->GetDevice(IID_PPV_ARGS(&device11));
        if (FAILED(hrDevice) || !device11) {
            EarlyLog("%s: FAILED to get D3D11 device (hr=0x%08X)", g_DetectedAPI, hrDevice);
            return;
        }
        EarlyLog("%s: Identified as D3D11 device %p", g_DetectedAPI, device11);

        // Initialize System Metrics (one-time)
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(device11->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC adapterDesc;
                if (SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
                    SystemMetricsCollector::Get().Initialize(adapterDesc.AdapterLuid.LowPart,
                                                             adapterDesc.AdapterLuid.HighPart);
                }
                adapter->Release();
            }
            dxgiDevice->Release();
        }

        // Cache device — GetDevice already AddRef'd, so we keep that ref
        g_pd3dDevice = device11;
        // Cache context with AddRef
        g_pd3dDevice->GetImmediateContext(&g_pd3dDeviceContext);
    }

    ID3D11Device* device = g_pd3dDevice;
    ID3D11DeviceContext* context = g_pd3dDeviceContext;

    if (g_OverlayAdapter.IsInitialized() && currentHwnd != g_CachedHwnd) {
        HookLog("DX11: HWND changed, shutting down OverlayAdapter");
        g_OverlayAdapter.Shutdown();
    }

    if (!g_OverlayAdapter.IsInitialized() || currentHwnd != g_CachedHwnd) {
        if (g_OverlayAdapter.IsInitialized()) {
            g_OverlayAdapter.Shutdown();
        }
        g_CachedHwnd = currentHwnd;
        lastHwnd = currentHwnd;

        InputManager::Get().HookWindow(currentHwnd);
        g_OverlayAdapter.SetHwnd(currentHwnd);

        if (g_OverlayAdapter.InitDX11(device, context)) {
            g_OverlayAdapter.SetHwnd(currentHwnd);
            EarlyLog("DX11: OverlayAdapter initialized for HWND %p", currentHwnd);
        } else {
            EarlyLog("DX11: OverlayAdapter::InitDX11 FAILED for HWND %p", currentHwnd);
        }
    }

    // Detect HWND change (multi-window apps)
    if (currentHwnd != lastHwnd) {
        EarlyLog(
            "DX11: HWND changed from %p to %p. Overlay might not be visible "
            "on new window without re-init.",
            lastHwnd, currentHwnd);
        lastHwnd = currentHwnd;
    }

    // Determine if HDR is active
    bool isHDR = (desc.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                  desc.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
    g_OverlayAdapter.SetHDR(isHDR, (int)desc.BufferDesc.Format);

    // Propagate HDR state to media engine via shared memory
    if (g_pSharedMem) {
        g_pSharedMem->SetIsHDR(isHDR);
    }

    g_OverlayAdapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
    g_OverlayAdapter.SetIPCClient(g_IPC);
    g_OverlayAdapter.SetDroppedFrames(g_DX11Capture.droppedFrames.load(std::memory_order_relaxed));
    g_OverlayAdapter.SetGraphicsAPI(g_DetectedAPI);

    ID3D11RenderTargetView* overlayRTV = nullptr;
    bool usingBoundRTV = false;

    // When Smooth Motion is active, prefer the RTV currently bound by the game.
    // This better matches the actual frame target and reduces overlay flicker.
    if (nvPresentLoaded && context) {
        context->OMGetRenderTargets(1, &overlayRTV, NULL);
        if (overlayRTV) {
            usingBoundRTV = true;
        }
    }

    if (!usingBoundRTV) {
        // For FLIP swap chains, the back buffer rotates each Present.
        // Overlay must draw to the same buffer that CaptureFrame will read,
        // otherwise the captured frame will not contain the overlay (flicker).
        static UINT lastBufferIndex = 0xFFFFFFFF;

        // Create/recreate RTV if swapchain changed or FLIP buffer index rotated
        if (!g_mainRenderTargetView || pSwapChain != lastSwapChain || resolvedBufferIndex != lastBufferIndex) {
            if (g_mainRenderTargetView) {
                g_mainRenderTargetView->Release();
                g_mainRenderTargetView = nullptr;
            }

            EarlyLog("%s: Creating RTV for SwapChain %p (%ux%u) buffer=%u...", g_DetectedAPI, pSwapChain,
                     desc.BufferDesc.Width, desc.BufferDesc.Height, resolvedBufferIndex);

            ID3D11Texture2D* backbuffer = nullptr;
            HRESULT hr = pSwapChain->GetBuffer(resolvedBufferIndex, IID_PPV_ARGS(&backbuffer));
            if (FAILED(hr) || !backbuffer) {
                EarlyLog("%s: GetBuffer(%u) FAILED hr=0x%08X", g_DetectedAPI, resolvedBufferIndex, hr);
                return;
            }
            hr = device->CreateRenderTargetView(backbuffer, NULL, &g_mainRenderTargetView);
            backbuffer->Release();
            if (FAILED(hr)) {
                EarlyLog("%s: CreateRTV FAILED hr=0x%08X", g_DetectedAPI, hr);
                return;
            }
            lastSwapChain = pSwapChain;
            lastBufferIndex = resolvedBufferIndex;
            EarlyLog("%s: RTV created OK (buffer=%u)", g_DetectedAPI, resolvedBufferIndex);
        }

        overlayRTV = g_mainRenderTargetView;
    }

    EarlyLog("DX11: [frame %d] pre-render device=%p context=%p rtv=%p", frameCount, device, context, overlayRTV);

    // Preserve game OM/viewport state around overlay target binding.
    ID3D11RenderTargetView* previousRTV = nullptr;
    ID3D11DepthStencilView* previousDSV = nullptr;
    context->OMGetRenderTargets(1, &previousRTV, &previousDSV);

    D3D11_VIEWPORT previousViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT previousViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    context->RSGetViewports(&previousViewportCount, previousViewports);

    context->OMSetRenderTargets(1, &overlayRTV, NULL);

    // Explicitly set viewport
    D3D11_VIEWPORT vp;
    vp.Width = (float)desc.BufferDesc.Width;
    vp.Height = (float)desc.BufferDesc.Height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    context->RSSetViewports(1, &vp);

    EarlyLog("DX11: [frame %d] calling RenderOverlay", frameCount);

    // Render Custom Overlay
    g_InOverlayRender = true;
    g_OverlayAdapter.RenderOverlay(desc.BufferDesc.Width, desc.BufferDesc.Height);
    g_InOverlayRender = false;

    if (previousViewportCount > 0) {
        context->RSSetViewports(previousViewportCount, previousViewports);
    }
    context->OMSetRenderTargets(1, &previousRTV, previousDSV);

    if (previousRTV) {
        previousRTV->Release();
    }
    if (previousDSV) {
        previousDSV->Release();
    }

    if (usingBoundRTV && overlayRTV) {
        overlayRTV->Release();
    }
}

// Handle SwapChain resize - must release RTV and reinitialize ImGui
HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height,
                                              DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    // CRITICAL: Check for shutdown first - if app is closing, don't touch
    // anything
    if (HookIsShuttingDown()) {
        if (oResizeBuffers) {
            return oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        }
        return S_OK;
    }

    // RECURSION BREAKER: If we are calling ourselves recursively, bail out
    // immediately. This handles the "Hooked the Hook" scenario or infinite
    // unhook/rehook loops.
    if (g_ResizeBuffersDepth > 0) {
        // WrapperLog("DX11: ResizeBuffers recursion detected! Bailing to prevent
        // crash."); We must call original if possible, but if original points to
        // us, we can't. If oResizeBuffers == DetourResizeBuffers, we are stuck.
        // Safe bet: just return S_OK to stop the madness.
        return S_OK;
    }
    g_ResizeBuffersDepth++;

    // Guard for auto-resetting depth
    auto depthGuard = ce::make_scope_guard([&] { g_ResizeBuffersDepth--; });

    HookLog("DX11: ResizeBuffers called (%dx%d)", Width, Height);

    // Safety: Check if oResizeBuffers is valid
    if (!oResizeBuffers) {
        HookLog("DX11: ResizeBuffers - Original function pointer is NULL! Bailing.");
        return S_OK;
    }

    // Safety: Check if oResizeBuffers points to US (Cycle Detection)
    if ((void*)oResizeBuffers == (void*)DetourResizeBuffers) {
        HookLog(
            "DX11: ResizeBuffers - Original function points to DETOUR! Cycle "
            "detected. Bailing.");
        return S_OK;
    }

    {
        ID3D12Device* d12Dev = nullptr;
        if (SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&d12Dev))) && d12Dev) {
            d12Dev->Release();
            DX12_OnSwapchainResizeBegin();

            // SELF-DESTRUCT: We are a DX11 hook on a DX12 swapchain.
            // Unhook ourselves to prevent infinite loops.
            HookLog(
                "DX11: DetourResizeBuffers - DX12 detected. Unhooking DX11 "
                "ResizeBuffers from this SwapChain.");

            void** vtable = *(void***)pSwapChain;
            DWORD oldProtect;
            if (VirtualProtect(&vtable[13], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                // Double check we are overwriting OURSELVES (or a hook), not something
                // random But actually we just want to restore 'oResizeBuffers' (the
                // Real Original).
                vtable[13] = (void*)oResizeBuffers;
                VirtualProtect(&vtable[13], sizeof(void*), oldProtect, &oldProtect);
                HookLog("DX11: DetourResizeBuffers - VTable[13] restored to original.");
            } else {
                HookLog("DX11: DetourResizeBuffers - FAILED to restore VTable[13]!");
            }

            // Call original immediately
            HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
            // CRITICAL FIX: Reset the resize cleanup flag since we called Begin but
            // never called End
            DX12_OnSwapchainResizeEnd();
            return hr;
        }
    }

    // Release render target view before resize
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
    if (g_mainRenderTargetView10) {
        g_mainRenderTargetView10->Release();
        g_mainRenderTargetView10 = nullptr;
    }

    // Invalidate D3D11 resources for OverlayAdapter if needed
    // Typically OverlayAdapter Release/resize handling is done in Render logic or
    // internally But we can force a shutdown if we want fresh resources on resize
    if (g_OverlayAdapter.IsInitialized()) {
        // g_OverlayAdapter.Shutdown(); // Optional: Shutdown on resize?
        // Usually not needed for DX11 as backend handles it or uses swapchain
        // backbuffer which changes? Capture project uses OMSetRenderTargets. For
        // safety, let's just let it be.
    }

    // Check for Waitable Swapchain
    if (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        HookLog("DX11: ResizeBuffers: Waitable Swapchain detected");
    }

    // Apply backbuffer count override
    int count = GetActiveGraphicsConfig().backbufferCount;
    if (count >= 2 && count <= 6) {
        // Check swap effect — don't reduce buffer count for flip model swapchains
        DXGI_SWAP_CHAIN_DESC scDesc = {};
        bool isFlip = false;
        if (SUCCEEDED(pSwapChain->GetDesc(&scDesc))) {
            isFlip = (scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                      scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
        }
        UINT gameCount = BufferCount > 0 ? BufferCount : scDesc.BufferCount;
        if (isFlip && (UINT)count < gameCount) {
            HookLog(
                "DX11: ResizeBuffers: Skipping BufferCount override %d < game's %u "
                "(flip model)",
                count, gameCount);
        } else {
            BufferCount = (UINT)count;
            HookLog("DX11: ResizeBuffers: Overriding BufferCount to %d", count);
        }
    }

    // Call original ResizeBuffers
    HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);

    if (FAILED(hr)) {
        HookLog("DX11: ResizeBuffers FAILED hr=0x%08X", hr);
    } else {
        HookLog("DX11: ResizeBuffers SUCCESS");
    }

    return hr;
}

// --- Prerender Limit Support ---
void ApplyPrerenderLimit(IDXGISwapChain* pSwapChain, float limit) {
    if (limit < 0.0f)
        return;

    ID3D11Device* dev = nullptr;
    if (FAILED(pSwapChain->GetDevice(IID_PPV_ARGS(&dev)))) {
        HookLogImportant("DX11: Prerender limit FAILED - GetDevice failed");
        return;
    }

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
        HookLogImportant("DX11: Created manual prerender query ring buffer (size: %d, limit=%.2f)", (int)g_PrerenderQueries.size(), limit);
    }

    if (!g_PrerenderQueries.empty()) {
        bool isFractional = (limit > 0.01f && limit < 1.0f);

        if (limit == 0.0f) {
            // Strict Serial: Wait for current frame
            ID3D11Query* q = g_PrerenderQueries[g_PrerenderFrameIndex % g_PrerenderQueries.size()];
            ctx->End(q);
            int64_t waitStart = PerfLogger::GetQpcUs();
            while (ctx->GetData(q, nullptr, 0, 0) == S_FALSE) {
                SwitchToThread();
            }
            int64_t waitUs = PerfLogger::GetQpcUs() - waitStart;
            int idx = g_DiagPrerenderWaits.fetch_add(1, std::memory_order_relaxed);
            if (idx < 12) {
                HookLogImportant("DX11: Prerender serial wait frame=%llu wait=%lldus (#%d)",
                                 (unsigned long long)g_PrerenderFrameIndex, (long long)waitUs, idx + 1);
            }
        } else {
            // Buffered Limit: For fractional limits (e.g., 0.5), we use Buffered 1
            // (Lookback 1) combined with an idle gap to approximate sub-frame latency.
            int effectiveLimit = isFractional ? 1 : (int)limit;
            int lookback = effectiveLimit;

            ID3D11Query* currentQ = g_PrerenderQueries[g_PrerenderFrameIndex % g_PrerenderQueries.size()];
            ctx->End(currentQ);

            if (g_PrerenderFrameIndex >= (uint64_t)lookback) {
                ID3D11Query* waitQ = g_PrerenderQueries[(g_PrerenderFrameIndex - lookback) % g_PrerenderQueries.size()];
                int64_t waitStart = PerfLogger::GetQpcUs();
                while (ctx->GetData(waitQ, nullptr, 0, 0) == S_FALSE) {
                    SwitchToThread();
                }
                int64_t waitUs = PerfLogger::GetQpcUs() - waitStart;
                int idx = g_DiagPrerenderWaits.fetch_add(1, std::memory_order_relaxed);
                if (idx < 12) {
                    HookLogImportant("DX11: Prerender buffered wait lookback=%d frame=%llu wait=%lldus (#%d)",
                                     lookback, (unsigned long long)g_PrerenderFrameIndex, (long long)waitUs, idx + 1);
                }
            }
        }
        g_PrerenderFrameIndex++;
        g_DiagPrerenderFrames.fetch_add(1, std::memory_order_relaxed);

        // Strict Serial + Fixed Idle Gap for fractional limits
        if (isFractional) {
            float fps = 60.0f;
            if (auto* m = DXGIShared::GetPerformanceMetrics())
                fps = m->GetCurrentFPS();
            double targetFrameTimeUs = (fps > 1.0f) ? (1000000.0 / fps) : 16666.0;

            int64_t idleGapUs = (int64_t)(targetFrameTimeUs * (1.0 - limit) * 0.10);
            if (idleGapUs > 0) {
                if (idleGapUs > 10000)
                    idleGapUs = 10000;
                int idx = g_DiagPrerenderWaits.fetch_add(1, std::memory_order_relaxed);
                if (idx < 6) {
                    HookLogImportant("DX11: Prerender fractional idle gap fps=%.1f gap=%lldus (#%d)", fps, (long long)idleGapUs, idx + 1);
                }
                PrecisionSleep(idleGapUs);
            }
        }
    }

    ctx->Release();
    dev->Release();
}

namespace DXGIShared {
void HandleDX11ProcessFrame(IDXGISwapChain* pSwapChain, bool isRealFrame) {
    ::HandleDX11ProcessFrame(pSwapChain, isRealFrame);
}

void HandleDX11ResizeBegin() {
    CleanupDX11Resources();
}
}  // namespace DXGIShared

HRESULT STDMETHODCALLTYPE DetourCreateSamplerState(ID3D11Device* pDevice, const D3D11_SAMPLER_DESC* pSamplerDesc,
                                                   ID3D11SamplerState** ppSamplerState) {
    if (!pSamplerDesc)
        return oCreateSamplerState(pDevice, pSamplerDesc, ppSamplerState);
    if (!g_GraphicsOverridesActive.load(std::memory_order_acquire))
        return oCreateSamplerState(pDevice, pSamplerDesc, ppSamplerState);

    bool debug = false;
    D3D11_SAMPLER_DESC desc = *pSamplerDesc;

    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
        debug = true;
    }

    const auto& gfx = GetActiveGraphicsConfig();
    // Enable AF at create-time as well as bind-time. Many games (e.g. BioShock
    // Infinite) create samplers once at startup and never rebind them, so the
    // bind-only AF path never fires. Create-time AF enablement is slightly
    // broader (no SRV mip-count check), but the same mip/border/reduction/
    // comparison filters apply.
    const bool modified = ApplySamplerOverrides11(desc, gfx, true);

    HRESULT hr;
    if (modified) {
        hr = oCreateSamplerState(pDevice, &desc, ppSamplerState);
        if (FAILED(hr)) {
            if (debug) {
                EarlyLog(
                    "DX11: CreateSamplerState FAILED with modified desc "
                    "(hr=0x%08X). Filter=0x%X Bias=%.2f Aniso=%u",
                    hr, desc.Filter, desc.MipLODBias, desc.MaxAnisotropy);
            }
        } else {
            HookLogImportant("DX11: CreateSamplerState modified Filter=0x%X Aniso=%u Bias=%.2f (original Aniso=%u Bias=%.2f)",
                    desc.Filter, desc.MaxAnisotropy, desc.MipLODBias,
                    pSamplerDesc->MaxAnisotropy, pSamplerDesc->MipLODBias);
        }
    } else {
        hr = oCreateSamplerState(pDevice, pSamplerDesc, ppSamplerState);
    }
    return hr;
}

// Hook: D3D10 CreateSamplerState - Same logic as D3D11 version
HRESULT STDMETHODCALLTYPE DetourCreateSamplerState10(ID3D10Device* pDevice, const D3D10_SAMPLER_DESC* pSamplerDesc,
                                                     ID3D10SamplerState** ppSamplerState) {
    static int callCount = 0;
    callCount++;
    if (callCount <= 10) {
        EarlyLog("DX10: DetourCreateSamplerState10 called (count=%d)", callCount);
    }

    if (!pSamplerDesc)
        return oCreateSamplerState10(pDevice, pSamplerDesc, ppSamplerState);
    if (!g_GraphicsOverridesActive.load(std::memory_order_acquire))
        return oCreateSamplerState10(pDevice, pSamplerDesc, ppSamplerState);

    D3D10_SAMPLER_DESC desc = *pSamplerDesc;
    bool modified = false;
    bool debug = false;
    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
        debug = true;
    }

    // Skip overrides for samplers that have no mipmapping (MipLevels == 1 equivalent).
    // See CreateSamplerState (D3D11) above for full explanation of the limitation.
    bool overridesAllowed = true;
    if (pSamplerDesc->MaxLOD == 0.0f)
        overridesAllowed = false;
    if (pSamplerDesc->MinLOD == pSamplerDesc->MaxLOD)
        overridesAllowed = false;

    if (overridesAllowed && g_IPC) {
        const auto& gfx = GetActiveGraphicsConfig();
        // Anisotropic Filtering
        std::string af = gfx.anisotropicFiltering;
        if (af != "default") {
            if (af == "off") {
                if (ce::sampler_override::IsD3D10AnisotropicFilter(desc.Filter)) {
                    bool isComparison = ce::sampler_override::IsD3D10ComparisonFilter(desc.Filter);
                    desc.Filter =
                        isComparison ? D3D10_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR : D3D10_FILTER_MIN_MAG_MIP_LINEAR;
                    desc.MaxAnisotropy = 1;
                    modified = true;
                }
            } else {
                UINT maxAniso = ce::sampler_override::GetConfiguredMaxAnisotropy(gfx);

                if (desc.AddressU == D3D10_TEXTURE_ADDRESS_BORDER || desc.AddressV == D3D10_TEXTURE_ADDRESS_BORDER ||
                    desc.AddressW == D3D10_TEXTURE_ADDRESS_BORDER) {
                    // Skip AF override for Border address mode
                } else {
                    desc.Filter = ce::sampler_override::GetForcedAnisotropicFilter(desc.Filter);
                    desc.MaxAnisotropy = maxAniso;
                    modified = true;
                }
            }
        }

        // Mip Mapping
        std::string mip = gfx.mipMapping;
        bool isAniso = ce::sampler_override::IsD3D10AnisotropicFilter(desc.Filter);

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
        float userBiasVal = 0.0f;
        bool userBiasActive = TryParseConfiguredMipBias(gfx, userBiasVal);
        float originalBias = pSamplerDesc->MipLODBias;
        desc.MipLODBias = ApplyConfiguredMipBias(gfx, originalBias);
        if (desc.MipLODBias != originalBias) {
            modified = true;
        }

        // SGSSAA Auto-Bias
        if (gfx.sgssaa && !gfx.disableAutoMipBias && !gfx.forceMipBiasClamp) {
            float sgBias = 0.0f;
            if (GetSGSSAABias(gfx.sgssaa, gfx.msaaSamples.c_str(), sgBias)) {
                desc.MipLODBias += sgBias;
                modified = true;
            }
        }

        if (userBiasActive && userBiasVal < 0.0f && !gfx.sgssaa && IsUnityProcess() && !gfx.forceMipBiasClamp) {
            if (desc.MipLODBias < -0.5f) {
                desc.MipLODBias = -0.5f;
                modified = true;
            }
        }

        float finalizedBias = FinalizeMipBias(gfx, desc.MipLODBias);
        if (finalizedBias != desc.MipLODBias) {
            desc.MipLODBias = finalizedBias;
            modified = true;
        }
    }

    HRESULT hr;
    if (modified) {
        hr = oCreateSamplerState10(pDevice, &desc, ppSamplerState);
        if (FAILED(hr) && debug) {
            EarlyLog(
                "DX10: CreateSamplerState FAILED with modified desc "
                "(hr=0x%08X). Filter=0x%X Bias=%.2f Aniso=%u",
                hr, desc.Filter, desc.MipLODBias, desc.MaxAnisotropy);
        } else if (debug) {
            static int logCount = 0;
            if (logCount++ < 5) {
                EarlyLog(
                    "DX10: CreateSamplerState overridden. Filter=0x%X Bias=%.2f "
                    "Aniso=%u",
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
    if (!pOriginal)
        return nullptr;

    const auto& gfx = GetActiveGraphicsConfig();
    EnsureSamplerCacheFresh10(gfx);

    // 1. If it's already a replacement sampler, don't try to replace it again
    if (IsReplacementSampler(pOriginal)) {
        return pOriginal;
    }

    // 2. Check the cache
    ID3D10SamplerState* cached = FindReplacementSampler(pOriginal);
    if (cached) {
        return cached;
    }

    // Get original sampler description
    D3D10_SAMPLER_DESC originalDesc;
    pOriginal->GetDesc(&originalDesc);

    // Skip overrides for samplers that have no mipmapping (MipLevels == 1 equivalent).
    // See CreateSamplerState (D3D11) above for full explanation of the limitation.
    bool overridesAllowed = true;
    if (originalDesc.MaxLOD == 0.0f)
        overridesAllowed = false;
    if (originalDesc.MinLOD == originalDesc.MaxLOD)
        overridesAllowed = false;

    if (!overridesAllowed || !g_IPC) {
        AddReplacementSampler(pOriginal, pOriginal);  // Cache as no-op
        return pOriginal;
    }

    D3D10_SAMPLER_DESC desc = originalDesc;
    bool modified = false;

    // Anisotropic Filtering
    std::string af = gfx.anisotropicFiltering;
    if (af != "default") {
        if (af == "off") {
            if (ce::sampler_override::IsD3D10AnisotropicFilter(desc.Filter)) {
                bool isComparison = ce::sampler_override::IsD3D10ComparisonFilter(desc.Filter);
                desc.Filter =
                    isComparison ? D3D10_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR : D3D10_FILTER_MIN_MAG_MIP_LINEAR;
                desc.MaxAnisotropy = 1;
                modified = true;
            }
        } else {
            UINT maxAniso = ce::sampler_override::GetConfiguredMaxAnisotropy(gfx);

            if (desc.AddressU != D3D10_TEXTURE_ADDRESS_BORDER && desc.AddressV != D3D10_TEXTURE_ADDRESS_BORDER &&
                desc.AddressW != D3D10_TEXTURE_ADDRESS_BORDER) {
                desc.Filter = ce::sampler_override::GetForcedAnisotropicFilter(desc.Filter);
                desc.MaxAnisotropy = maxAniso;
                modified = true;
            }
        }
    }

    // Mip Mapping
    std::string mip = gfx.mipMapping;
    bool isAniso = ce::sampler_override::IsD3D10AnisotropicFilter(desc.Filter);

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
    float originalBias = desc.MipLODBias;
    desc.MipLODBias = ApplyConfiguredMipBias(gfx, originalBias);
    if (desc.MipLODBias != originalBias) {
        modified = true;
    }

    // SGSSAA Auto-Bias
    if (gfx.sgssaa && !gfx.disableAutoMipBias && !gfx.forceMipBiasClamp) {
        float sgBias = 0.0f;
        if (GetSGSSAABias(gfx.sgssaa, gfx.msaaSamples.c_str(), sgBias)) {
            desc.MipLODBias += sgBias;
            modified = true;
        }
    }

    float finalizedBias = FinalizeMipBias(gfx, desc.MipLODBias);
    if (finalizedBias != desc.MipLODBias) {
        desc.MipLODBias = finalizedBias;
        modified = true;
    }

    if (!modified) {
        AddReplacementSampler(pOriginal, pOriginal);  // Cache as no-op
        return pOriginal;
    }

    // Create the replacement sampler
    ID3D10SamplerState* pReplacement = nullptr;
    HRESULT hr = pDevice->CreateSamplerState(&desc, &pReplacement);
    if (SUCCEEDED(hr)) {
        AddReplacementSampler(pOriginal, pReplacement);
        AddToReplacementSet(pReplacement);
        static int logCount = 0;
        if (logCount++ < 10) {
            EarlyLog("DX10: Created replacement sampler %p -> %p (AF=%d, Bias=%.2f)", pOriginal, pReplacement,
                     desc.MaxAnisotropy, desc.MipLODBias);
        }
        return pReplacement;
    } else {
        AddReplacementSampler(pOriginal, pOriginal);  // Failed, use original
        return pOriginal;
    }
}

// D3D10 PSSetSamplers detour
static void STDMETHODCALLTYPE DetourPSSetSamplers10(ID3D10Device* pDevice, UINT StartSlot, UINT NumSamplers,
                                                    ID3D10SamplerState* const* ppSamplers) {
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
    if (!g_CachedD3D10Device)
        g_CachedD3D10Device = pDevice;

    // Clamp NumSamplers to avoid stack overflow
    UINT actualNum =
        (NumSamplers > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT) ? D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT : NumSamplers;

    ID3D10SamplerState* replacedSamplers[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT];
    for (UINT i = 0; i < actualNum; i++) {
        replacedSamplers[i] = GetOrCreateReplacementSampler10(pDevice, ppSamplers[i]);
    }

    // For anything beyond actualNum, we don't care about replacedSamplers

    oPSSetSamplers10(pDevice, StartSlot, NumSamplers, replacedSamplers);
}

// D3D10 VSSetSamplers detour
static void STDMETHODCALLTYPE DetourVSSetSamplers10(ID3D10Device* pDevice, UINT StartSlot, UINT NumSamplers,
                                                    ID3D10SamplerState* const* ppSamplers) {
    if (!ppSamplers || NumSamplers == 0 || !g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        oVSSetSamplers10(pDevice, StartSlot, NumSamplers, ppSamplers);
        return;
    }

    if (!g_CachedD3D10Device)
        g_CachedD3D10Device = pDevice;

    // Clamp NumSamplers to avoid stack overflow
    UINT actualNum =
        (NumSamplers > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT) ? D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT : NumSamplers;

    ID3D10SamplerState* replacedSamplers[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT];
    for (UINT i = 0; i < actualNum; i++) {
        replacedSamplers[i] = GetOrCreateReplacementSampler10(pDevice, ppSamplers[i]);
    }

    oVSSetSamplers10(pDevice, StartSlot, NumSamplers, replacedSamplers);
}

// D3D10 GSSetSamplers detour
static void STDMETHODCALLTYPE DetourGSSetSamplers10(ID3D10Device* pDevice, UINT StartSlot, UINT NumSamplers,
                                                    ID3D10SamplerState* const* ppSamplers) {
    if (!ppSamplers || NumSamplers == 0 || !g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        oGSSetSamplers10(pDevice, StartSlot, NumSamplers, ppSamplers);
        return;
    }

    if (!g_CachedD3D10Device)
        g_CachedD3D10Device = pDevice;

    // Clamp NumSamplers to avoid stack overflow
    UINT actualNum =
        (NumSamplers > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT) ? D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT : NumSamplers;

    ID3D10SamplerState* replacedSamplers[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT];
    for (UINT i = 0; i < actualNum; i++) {
        replacedSamplers[i] = GetOrCreateReplacementSampler10(pDevice, ppSamplers[i]);
    }

    oGSSetSamplers10(pDevice, StartSlot, NumSamplers, replacedSamplers);
}

// Helper: Install hooks on a specific D3D10 device VTable at runtime
// This ensures we catch the correct VTable even if it differs from our temp
// device
static void InstallRuntimeD3D10Hooks(ID3D10Device* pDevice) {
    if (!pDevice)
        return;

    void** pVTable = *(void***)pDevice;
    VTableHook::Status status;

    // PSSetSamplers (Index 6)
    status = VTableHook::Create(&pVTable[6], (LPVOID)&DetourPSSetSamplers10, (LPVOID*)&oPSSetSamplers10);
    if (status == VTableHook::Success) {
        HookLog("DX10: Runtime PSSetSamplers hook installed");
    }

    // VSSetSamplers (Index 20)
    status = VTableHook::Create(&pVTable[20], (LPVOID)&DetourVSSetSamplers10, (LPVOID*)&oVSSetSamplers10);
    if (status == VTableHook::Success) {
        HookLog("DX10: Runtime VSSetSamplers hook installed");
    }

    // GSSetSamplers (Index 23)
    status = VTableHook::Create(&pVTable[23], (LPVOID)&DetourGSSetSamplers10, (LPVOID*)&oGSSetSamplers10);
    if (status == VTableHook::Success) {
        HookLog("DX10: Runtime GSSetSamplers hook installed");
    }
}

void DX11Hook::ProcessDeferredReleases() {
    g_DeferredRelease.Process();
}

void DX11Hook::Init() {
    HookLog("DX11Hook::Init()");

    // CRITICAL FIX: Check if Vulkan is active before installing D3D11 hooks
    // Vulkan games using WSI-to-DXGI mapping can freeze if we hook D3D11/DXGI
    HMODULE hVulkan = GetModuleHandleW(L"vulkan-1.dll");
    if (hVulkan) {
        HookLog(
            "DX11: Vulkan detected (vulkan-1.dll), SKIPPING D3D11 hook "
            "installation");
        return;
    }

    // D3D11CreateDeviceAndSwapChain hook is now handled by IAT patching in
    // iat_hook.cpp The InitializeD3D11Hooks() function sets up the IAT hook
    HMODULE hD3D11 = GetModuleHandleA("d3d11.dll");
    if (hD3D11) {
        // Initialize IAT hooks now that d3d11.dll is loaded
        // This may have been called before d3d11.dll was loaded at startup
        IATHook::InitializeD3D11Hooks();

        // Also hook the export directly using CustomHook to catch calls that bypass
        // IAT (e.g., statically bound imports, GetProcAddress)
        CustomHook::Initialize();
        void* pTarget = (void*)GetProcAddress(hD3D11, "D3D11CreateDeviceAndSwapChain");
        if (pTarget) {
            HookLog("DX11: Hook target at %p", pTarget);
            // Save the real original BEFORE HookExport overwrites the shared
            // oD3D11CreateDeviceAndSwapChain (shared with wrapper_hooks.cpp).
            s_oRealD3D11CreateDeviceAndSwapChain = reinterpret_cast<PFN_D3D11CreateDeviceAndSwapChain>(pTarget);
            bool created = CustomHook::HookExport(
                               "d3d11.dll", "D3D11CreateDeviceAndSwapChain", (void*)DetourD3D11CreateDeviceAndSwapChain,
                               (void**)&oD3D11CreateDeviceAndSwapChain) == CustomHook::Status::Success;
            HookLog("DX11: HookExport result: %s", created ? "success" : "failed");
            HookLog("DX11: D3D11CreateDeviceAndSwapChain export hook installed.");
        } else {
            HookLog("DX11: Failed to get D3D11CreateDeviceAndSwapChain address!");
        }
        HookLog("DX11: D3D11CreateDeviceAndSwapChain hook installed.");
    }

    // 2. Hook D3D10 entry points
    // D3D10 hooking is handled by IAT in iat_hook.cpp / wrapper_hooks.cpp
    HMODULE hD3D10 = GetModuleHandleA("d3d10.dll");
    if (hD3D10) {
        HookLog("DX11: D3D10 hooks should be active via IAT.");
    }

    // 3. Hook DXGI Factory entry points
    // DXGI Factory hooking is handled by IAT.
    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    if (hDXGI) {
        HookLog("DX11: DXGI hooks should be active via IAT.");
    }

    // 4. Scan for EXISTING swapchains (late injection scenario)
    // If the game already created the device/swapchain before we injected,
    // we need to hook the vtable of an EXISTING swapchain.
    // We do this by creating a temporary swapchain using the hooked factory,
    // which will also trigger our InstallVTableHooks.
    HookLog("DX11: Scanning for pre-existing swapchains...");

    // First, try D3D10 route (the game is D3D10)
    if (hD3D10) {
        typedef HRESULT(WINAPI * PFN_D3D10CreateDevice)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT,
                                                        ID3D10Device**);
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
                            HWND tempHwnd = CreateWindowExA(0, "STATIC", "TempDXGI", WS_OVERLAPPEDWINDOW, 0, 0, 100,
                                                            100, NULL, NULL, GetModuleHandle(NULL), NULL);
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
                                // This call goes through our detour and will install vtable
                                // hooks!
                                hr = factory->CreateSwapChain(tempDevice, &scd, &tempSC);
                                if (SUCCEEDED(hr) && tempSC) {
                                    HookLog(
                                        "DX11: Temp D3D10 swapchain created to install "
                                        "vtable hooks");
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

    // If D3D10 isn't loaded (common for pure DX11 apps), still force install the
    // Present hook by creating a dummy D3D11 device+swapchain via the original
    // D3D11CreateDeviceAndSwapChain.
    if (hD3D11 && oD3D11CreateDeviceAndSwapChain) {
        HWND tempHwnd = CreateWindowExA(0, "STATIC", "TempD3D11", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL,
                                        GetModuleHandle(NULL), NULL);
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

            D3D_FEATURE_LEVEL flOut = D3D_FEATURE_LEVEL_11_0;
            D3D_FEATURE_LEVEL flReq[] = {D3D_FEATURE_LEVEL_11_0};
            ID3D11Device* dev = nullptr;
            ID3D11DeviceContext* ctx = nullptr;
            IDXGISwapChain* sc = nullptr;

            HRESULT hr = (s_oRealD3D11CreateDeviceAndSwapChain ? s_oRealD3D11CreateDeviceAndSwapChain
                                                                 : oD3D11CreateDeviceAndSwapChain)
                             (nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                              D3D11_CREATE_DEVICE_BGRA_SUPPORT, flReq, 1, D3D11_SDK_VERSION,
                              &scd, &sc, &dev, &flOut, &ctx);
            if (SUCCEEDED(hr) && sc) {
                InstallVTableHooks(dev, ctx, sc);
                CWrapDXGISwapChain* wrappedSc = nullptr;
                IDXGISwapChain* realSc = nullptr;
                if (SUCCEEDED(sc->QueryInterface(IID_CWrapDXGISwapChain, (void**)&wrappedSc)) && wrappedSc) {
                    realSc = wrappedSc->GetReal();
                    wrappedSc->Release();
                }
                HookLog("DX11: Temp D3D11 install target (wrapper=%p, real=%p)", sc, realSc);
                DXGIShared::InstallHooks(realSc ? realSc : sc, true);
                HookLog("DX11: Temp D3D11 swapchain created to install vtable hooks");
            }

            if (sc)
                sc->Release();
            if (ctx)
                ctx->Release();
            if (dev)
                dev->Release();
            DestroyWindow(tempHwnd);
        }
    }
}

void DX11Hook::Shutdown() {
    HookLog("DX11Hook::Shutdown()");

    // Log diagnostic summary for sampler/prerender overrides
    {
        int afApplied = g_DiagSamplerAFApplied.load(std::memory_order_relaxed);
        int afReplaced = g_DiagSamplerReplacementCreated.load(std::memory_order_relaxed);
        int afNoMips = g_DiagSamplerSkipNoMips.load(std::memory_order_relaxed);
        int afBorder = g_DiagSamplerSkipBorder.load(std::memory_order_relaxed);
        int afReduction = g_DiagSamplerSkipReduction.load(std::memory_order_relaxed);
        int afComparison = g_DiagSamplerSkipComparison.load(std::memory_order_relaxed);
        int afSlot = g_DiagSamplerSkipSlot.load(std::memory_order_relaxed);
        int afNoSRV = g_DiagSamplerSkipNoSRV.load(std::memory_order_relaxed);
        int afFormat = g_DiagSamplerSkipFormat.load(std::memory_order_relaxed);
        int afSingleMip = g_DiagSamplerSkipSingleMip.load(std::memory_order_relaxed);
        int mipBias = g_DiagSamplerMipBiasApplied.load(std::memory_order_relaxed);
        int mipOverride = g_DiagSamplerMipOverride.load(std::memory_order_relaxed);
        int prerenderFrames = g_DiagPrerenderFrames.load(std::memory_order_relaxed);
        int prerenderWaits = g_DiagPrerenderWaits.load(std::memory_order_relaxed);
        HookLog("DX11: Override summary: AF_applied=%d AF_replaced=%d AF_skip(noMips=%d border=%d reduction=%d comp=%d slot=%d noSRV=%d fmt=%d singleMip=%d) mipBias=%d mipOverride=%d prerender(frames=%d waits=%d)",
                afApplied, afReplaced, afNoMips, afBorder, afReduction, afComparison, afSlot, afNoSRV, afFormat, afSingleMip,
                mipBias, mipOverride, prerenderFrames, prerenderWaits);
    }

    // Cleanup OverlayAdapter
    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    // CRITICAL FIX: Clear sampler caches to prevent unbounded memory growth
    // These caches accumulate replacement samplers over time
    ClearReplacementSamplerCache10();
    ClearReplacementSamplerCache11();
    ReleaseTrackedShaderResources11();
    {
        std::lock_guard<std::mutex> lock(g_D3D11FormatSupportMutex);
        g_D3D11FormatSupportCache.clear();
    }

    // Clean up prerender queries
    for (auto* q : g_PrerenderQueries) {
        if (q)
            q->Release();
    }
    g_PrerenderQueries.clear();

    g_DX11Capture.Cleanup();
    if (g_mainRenderTargetView) {
        g_DeferredRelease.Queue(g_mainRenderTargetView);
        g_mainRenderTargetView = nullptr;
    }
    if (g_mainRenderTargetView10) {
        g_DeferredRelease.Queue(g_mainRenderTargetView10);
        g_mainRenderTargetView10 = nullptr;
    }

    // Flush deferred release queue
    g_DeferredRelease.Process();
}

void DX11Hook::OnHostDisconnect() {
    // Log diagnostic summary for sampler/prerender overrides
    {
        int afApplied = g_DiagSamplerAFApplied.load(std::memory_order_relaxed);
        int afReplaced = g_DiagSamplerReplacementCreated.load(std::memory_order_relaxed);
        int afNoMips = g_DiagSamplerSkipNoMips.load(std::memory_order_relaxed);
        int afBorder = g_DiagSamplerSkipBorder.load(std::memory_order_relaxed);
        int afReduction = g_DiagSamplerSkipReduction.load(std::memory_order_relaxed);
        int afComparison = g_DiagSamplerSkipComparison.load(std::memory_order_relaxed);
        int afSlot = g_DiagSamplerSkipSlot.load(std::memory_order_relaxed);
        int afNoSRV = g_DiagSamplerSkipNoSRV.load(std::memory_order_relaxed);
        int afFormat = g_DiagSamplerSkipFormat.load(std::memory_order_relaxed);
        int afSingleMip = g_DiagSamplerSkipSingleMip.load(std::memory_order_relaxed);
        int mipBias = g_DiagSamplerMipBiasApplied.load(std::memory_order_relaxed);
        int mipOverride = g_DiagSamplerMipOverride.load(std::memory_order_relaxed);
        int prerenderFrames = g_DiagPrerenderFrames.load(std::memory_order_relaxed);
        int prerenderWaits = g_DiagPrerenderWaits.load(std::memory_order_relaxed);
        HookLog("DX11: Override summary: AF_applied=%d AF_replaced=%d AF_skip(noMips=%d border=%d reduction=%d comp=%d slot=%d noSRV=%d fmt=%d singleMip=%d) mipBias=%d mipOverride=%d prerender(frames=%d waits=%d)",
                afApplied, afReplaced, afNoMips, afBorder, afReduction, afComparison, afSlot, afNoSRV, afFormat, afSingleMip,
                mipBias, mipOverride, prerenderFrames, prerenderWaits);
    }
    HookLog("DX11Hook::OnHostDisconnect() - ready for reconnection");
    // DX11 capture is synchronous, nothing to stop
    // Just cleanup for potential new session
    ClearReplacementSamplerCache10();
    ClearReplacementSamplerCache11();
    ReleaseTrackedShaderResources11();
    g_DX11Capture.Cleanup();
}
