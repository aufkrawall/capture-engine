#include <atomic>
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
#include "../wrappers/d3d11_devicecontext_wrap.h"
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
static IDXGISwapChain* g_D3D11IdentitySwapChain = NULL;
static ID3D11Device* g_D3D11IdentityDevice = NULL;

static ID3D10Device* g_pd3d10Device = NULL;
static ID3D10RenderTargetView* g_mainRenderTargetView10 = NULL;

static IDXGISwapChain* g_pSwapChain = NULL;

static bool g_IsDX10Device = false;
static const char* g_DetectedAPI = "DX11";
static std::mutex g_GraphicsApiIdentityMutex;
static std::unordered_map<ID3D10Device*, bool> g_D3D10DeviceIdentities;
static std::unordered_map<IDXGISwapChain*, bool> g_D3D10SwapChainIdentities;
static ce::graphics_api_identity::ScopedIdentityRegistry<unsigned> g_D3D11MinorUse;
thread_local unsigned g_D3D11InternalIdentityProbeDepth = 0;

void DX11Hook_BeginInternalIdentityProbe() {
    ++g_D3D11InternalIdentityProbeDepth;
}

void DX11Hook_EndInternalIdentityProbe() {
    if (g_D3D11InternalIdentityProbeDepth != 0)
        --g_D3D11InternalIdentityProbeDepth;
}

class D3D11InternalIdentityProbeScope {
public:
    D3D11InternalIdentityProbeScope() {
        DX11Hook_BeginInternalIdentityProbe();
    }
    ~D3D11InternalIdentityProbeScope() {
        DX11Hook_EndInternalIdentityProbe();
    }
};

void DX10Hook_RegisterDeviceIdentity(ID3D10Device* device, bool is10_1, const char* evidence) {
    if (!device)
        return;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_GraphicsApiIdentityMutex);
        const auto it = g_D3D10DeviceIdentities.find(device);
        changed = it == g_D3D10DeviceIdentities.end() || it->second != is10_1;
        g_D3D10DeviceIdentities[device] = is10_1;
    }
    if (changed) {
        HookLogImportant("[GraphicsAPI] D3D10 device identity device=%p api=%s evidence=%s", device,
                         is10_1 ? "DX10.1" : "DX10", evidence ? evidence : "unknown");
    }
}

void DX10Hook_RegisterSwapChainIdentity(IDXGISwapChain* swapChain, bool is10_1, const char* evidence) {
    if (!swapChain)
        return;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_GraphicsApiIdentityMutex);
        const auto it = g_D3D10SwapChainIdentities.find(swapChain);
        changed = it == g_D3D10SwapChainIdentities.end() || it->second != is10_1;
        g_D3D10SwapChainIdentities[swapChain] = is10_1;
    }
    if (changed) {
        HookLogImportant("[GraphicsAPI] D3D10 swapchain identity swapChain=%p api=%s evidence=%s", swapChain,
                         is10_1 ? "DX10.1" : "DX10", evidence ? evidence : "unknown");
    }
}

void DX11Hook_RegisterDeviceIdentity(ID3D11Device* device, const char* evidence, bool newDevice) {
    if (!device)
        return;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_GraphicsApiIdentityMutex);
        if (newDevice) {
            unsigned previous = 0;
            changed = !g_D3D11MinorUse.TryGet(device, &previous) || previous != 0;
            g_D3D11MinorUse.Set(device, 0u);
        } else {
            changed = g_D3D11MinorUse.Ensure(device, 0u);
        }
    }
    if (changed) {
        HookLogImportant("[GraphicsAPI] D3D11 device identity device=%p api=DX11 evidence=%s", device,
                         evidence ? evidence : "unknown");
    }
}

void DX11Hook_ReportApiUse(ID3D11Device* device, unsigned minorVersion, const char* evidence) {
    if (!device || minorVersion == 0)
        return;
    unsigned previous = 0;
    unsigned updated = 0;
    {
        std::lock_guard<std::mutex> lock(g_GraphicsApiIdentityMutex);
        g_D3D11MinorUse.TryGet(device, &previous);
        updated = ce::graphics_api_identity::MergeD3D11Minor(previous, minorVersion);
        g_D3D11MinorUse.Set(device, updated);
    }
    if (updated != previous) {
        const std::string label = ce::graphics_api_identity::D3D11Label(updated, false);
        HookLogImportant("[GraphicsAPI] D3D11 API use device=%p api=%s evidence=%s", device, label.c_str(),
                         evidence ? evidence : "unknown");
    }
}

static bool ResolveD3D10Is10_1(ID3D10Device* device, IDXGISwapChain* swapChain) {
    std::lock_guard<std::mutex> lock(g_GraphicsApiIdentityMutex);
    if (device) {
        const auto deviceIt = g_D3D10DeviceIdentities.find(device);
        if (deviceIt != g_D3D10DeviceIdentities.end())
            return deviceIt->second;
    }
    const auto swapChainIt = g_D3D10SwapChainIdentities.find(swapChain);
    return swapChainIt != g_D3D10SwapChainIdentities.end() && swapChainIt->second;
}

static unsigned ResolveD3D11MinorUse(ID3D11Device* device) {
    std::lock_guard<std::mutex> lock(g_GraphicsApiIdentityMutex);
    unsigned identity = 0;
    g_D3D11MinorUse.TryGet(device, &identity);
    return identity;
}

typedef HRESULT(STDMETHODCALLTYPE* D3D11QueryInterface_t)(IUnknown*, REFIID, void**);
static std::unordered_map<void**, D3D11QueryInterface_t> g_D3D11QueryInterfaceOriginals;

static unsigned D3D11DeviceMinorFromIID(REFIID iid) {
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

static unsigned D3D11ContextMinorFromIID(REFIID iid) {
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

static HRESULT STDMETHODCALLTYPE DetourD3D11QueryInterface(IUnknown* object, REFIID iid, void** result) {
    D3D11QueryInterface_t original = nullptr;
    void** vtable = object ? *(void***)object : nullptr;
    {
        std::lock_guard<std::mutex> lock(g_GraphicsApiIdentityMutex);
        const auto it = g_D3D11QueryInterfaceOriginals.find(vtable);
        if (it != g_D3D11QueryInterfaceOriginals.end())
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

static void InstallD3D11IdentityQueryHook(IUnknown* object, const char* source) {
    if (!object)
        return;
    void** vtable = *(void***)object;
    std::lock_guard<std::mutex> lock(g_GraphicsApiIdentityMutex);
    if (g_D3D11QueryInterfaceOriginals.find(vtable) != g_D3D11QueryInterfaceOriginals.end())
        return;

    D3D11QueryInterface_t original = nullptr;
    if (VTableHook::Create(&vtable[0], (LPVOID)&DetourD3D11QueryInterface, (LPVOID*)&original) != VTableHook::Success ||
        !original) {
        HookLog("[GraphicsAPI] D3D11 QueryInterface hook failed object=%p source=%s", object,
                source ? source : "unknown");
        return;
    }
    g_D3D11QueryInterfaceOriginals.emplace(vtable, original);
    HookLog("[GraphicsAPI] D3D11 QueryInterface evidence hook installed object=%p source=%s", object,
            source ? source : "unknown");
}

// Prerender Limit Fencing
static std::vector<ID3D11Query*> g_PrerenderQueries;
static uint64_t g_PrerenderFrameIndex = 0;
static ID3D11Device* g_PrerenderQueryDevice = nullptr;
static ID3D10Device* g_PrerenderQueryDevice10 = nullptr;
static ID3D10Query* g_PrerenderSerialQuery10 = nullptr;
static std::mutex g_PrerenderMutex;
static int64_t g_LastSleepUs = 0;

// Deferred state bootstrap: used after Present hook installation so games that
// bound samplers before our vtable hooks still get a tracked logical state.
// This must be per-context: UE3 titles can create several short-lived D3D11
// devices before the real game swapchain, and a process-wide latch would make
// the final context keep blurry original samplers forever.
static std::mutex g_DeferredAFBootstrapMutex;
static std::unordered_set<uintptr_t> g_DeferredAFBootstrappedContexts;

// Sampler override diagnostic counters (rate-limited logging)
static std::atomic<int> g_DiagSamplerAllowsAF{0};
static std::atomic<int> g_DiagSamplerSkipNoMips{0};
static std::atomic<int> g_DiagSamplerSkipBorder{0};
static std::atomic<int> g_DiagSamplerSkipReduction{0};
static std::atomic<int> g_DiagSamplerSkipComparison{0};
static std::atomic<int> g_DiagSamplerSkipStage{0};
static std::atomic<int> g_DiagSamplerSkipNoSRV{0};
static std::atomic<int> g_DiagSamplerSkipFormat{0};
static std::atomic<int> g_DiagSamplerSkipSingleMip{0};
static std::atomic<int> g_DiagSamplerSkipNonColorResource{0};
static std::atomic<int> g_DiagSamplerSkipUnsafeResource{0};
static std::atomic<int> g_DiagSamplerSkipNoShader{0};
static std::atomic<int> g_DiagSamplerSkipNoShaderMetadata{0};
static std::atomic<int> g_DiagSamplerSkipShaderUnused{0};
static std::atomic<int> g_DiagSamplerSkipExplicitSample{0};
static std::atomic<int> g_DiagSamplerAllowLodSample{0};
static std::atomic<int> g_DiagSamplerAFApplied{0};
static std::atomic<int> g_DiagSamplerReplacementCreated{0};
static std::atomic<int> g_DiagSamplerRebound{0};
static std::atomic<int> g_DiagSamplerDrawReconcileCalls{0};
static std::atomic<int> g_DiagSamplerReconcileSlots{0};
static std::atomic<int> g_DiagSamplerBindDeferred{0};
static std::atomic<int> g_DiagSamplerEffectiveBindCalls{0};
static std::atomic<int> g_DiagSamplerEffectiveBinds{0};
static std::atomic<int> g_DiagSamplerEffectiveBindSkips{0};
static std::atomic<int> g_DiagDeferredAFBootstrapComplete{0};
static std::atomic<int> g_DiagDeferredAFBootstrapRetry{0};
static std::atomic<int> g_DiagDeferredAFBootstrapDisabled{0};
static std::atomic<int> g_DiagSamplerMipBiasApplied{0};
static std::atomic<int> g_DiagSamplerMipOverride{0};
static std::atomic<int> g_DiagSamplerRuntimeHookInstalled{0};
static std::atomic<int> g_DiagD3D11ContextVTablesHooked{0};
static std::atomic<int> g_DiagD3D11ContextHookSkips{0};
static std::atomic<int> g_DiagCreateDeferredContext11{0};
static std::atomic<int> g_DiagExecuteCommandList11{0};
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

static bool IsDeferredAFBootstrapped11(ID3D11DeviceContext* context) {
    if (!context) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_DeferredAFBootstrapMutex);
    return g_DeferredAFBootstrappedContexts.find(reinterpret_cast<uintptr_t>(context)) !=
           g_DeferredAFBootstrappedContexts.end();
}

static void MarkDeferredAFBootstrapped11(ID3D11DeviceContext* context) {
    if (!context) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_DeferredAFBootstrapMutex);
    g_DeferredAFBootstrappedContexts.insert(reinterpret_cast<uintptr_t>(context));
}

static void ClearDeferredAFBootstraps11() {
    std::lock_guard<std::mutex> lock(g_DeferredAFBootstrapMutex);
    g_DeferredAFBootstrappedContexts.clear();
}

static void ApplyDX11WaitableFlag(DXGI_SWAP_CHAIN_DESC& desc) {
    desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
}
static void ApplyDX11WaitableFlag(DXGI_SWAP_CHAIN_DESC1& desc) {
    desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
}

static bool ApplyDX11BackbufferCountOverride(DXGI_SWAP_CHAIN_DESC& desc, const char* source) {
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

static bool ApplyDX11BackbufferCountOverride(DXGI_SWAP_CHAIN_DESC1& desc, const char* source) {
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
typedef HRESULT(STDMETHODCALLTYPE* CreatePixelShader11_t)(ID3D11Device* pDevice, const void* pShaderBytecode,
                                                          SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage,
                                                          ID3D11PixelShader** ppPixelShader);
static CreatePixelShader11_t oCreatePixelShader11 = NULL;
typedef HRESULT(STDMETHODCALLTYPE* CreateDeferredContext11_t)(ID3D11Device* pDevice, UINT ContextFlags,
                                                              ID3D11DeviceContext** ppDeferredContext);
static CreateDeferredContext11_t oCreateDeferredContext11 = NULL;

typedef void(STDMETHODCALLTYPE* SetShaderResources11_t)(ID3D11DeviceContext* pContext, UINT StartSlot, UINT NumViews,
                                                        ID3D11ShaderResourceView* const* ppShaderResourceViews);
typedef void(STDMETHODCALLTYPE* SetSamplers11_t)(ID3D11DeviceContext* pContext, UINT StartSlot, UINT NumSamplers,
                                                 ID3D11SamplerState* const* ppSamplers);
typedef void(STDMETHODCALLTYPE* PSSetShader11_t)(ID3D11DeviceContext* pContext, ID3D11PixelShader* pPixelShader,
                                                 ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances);
static PSSetShader11_t oPSSetShader11 = NULL;
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
static DrawIndexed11_t oDrawIndexed11 = NULL;
static Draw11_t oDraw11 = NULL;
static DrawIndexedInstanced11_t oDrawIndexedInstanced11 = NULL;
static DrawInstanced11_t oDrawInstanced11 = NULL;
static DrawAuto11_t oDrawAuto11 = NULL;
static DrawIndexedInstancedIndirect11_t oDrawIndexedInstancedIndirect11 = NULL;
static DrawInstancedIndirect11_t oDrawInstancedIndirect11 = NULL;
static ExecuteCommandList11_t oExecuteCommandList11 = NULL;

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

static std::shared_mutex g_D3D11ContextVTableOriginalsMutex;
static std::unordered_map<void*, D3D11ContextVTableOriginals> g_D3D11ContextVTableOriginals;
static std::atomic<void*> g_PrimaryD3D11ContextVTable{nullptr};
static std::atomic<uint32_t> g_D3D11ContextVTableOriginalsGeneration{1};

template <typename Fn>
