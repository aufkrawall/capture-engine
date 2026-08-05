#include "dxgi_shared_internal.h"

namespace DXGIShared {
namespace {
// Private-data storage follows the swapchain COM identity without a global raw
// pointer map or a lifetime race. CWrapDXGISwapChain forwards private-data calls
// to the real object, so wrapped and unwrapped Present paths observe one value.
constexpr GUID kCESwapChainColorSpaceGuid = {
    0x72034fd8, 0xd63a, 0x4a6b, {0x98, 0x61, 0x4f, 0x6d, 0x99, 0xb7, 0x88, 0x21}};
}
}

namespace DXGIShared {
bool QuerySwapChainColorSpace(IDXGISwapChain* swapChain, DXGI_COLOR_SPACE_TYPE& colorSpace) {
    if (!swapChain)
        return false;
    int storedColorSpace = 0;
    UINT storedSize = sizeof(storedColorSpace);
    if (FAILED(swapChain->GetPrivateData(kCESwapChainColorSpaceGuid, &storedSize, &storedColorSpace)) ||
        storedSize != sizeof(storedColorSpace) || storedColorSpace < 0 ||
        storedColorSpace > static_cast<int>(DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020)) {
        return false;
    }
    colorSpace = static_cast<DXGI_COLOR_SPACE_TYPE>(storedColorSpace);
    return true;
}
}

namespace DXGIShared {
bool RecordSwapChainColorSpace(IDXGISwapChain* swapChain, DXGI_COLOR_SPACE_TYPE colorSpace, bool* changed) {
    if (changed) {
        *changed = false;
    }
    if (!swapChain) {
        return false;
    }

    DXGI_COLOR_SPACE_TYPE previousColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    if (QuerySwapChainColorSpace(swapChain, previousColorSpace) && previousColorSpace == colorSpace) {
        return true;
    }

    const int storedColorSpace = static_cast<int>(colorSpace);
    const HRESULT result =
        swapChain->SetPrivateData(kCESwapChainColorSpaceGuid, sizeof(storedColorSpace), &storedColorSpace);
    if (FAILED(result)) {
        static std::atomic<int> s_failureLogCount{0};
        if (s_failureLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            HookLogImportant("DXGI: Failed to retain swapchain color-space contract sc=%p cs=%d hr=0x%08X",
                             swapChain, storedColorSpace, static_cast<unsigned>(result));
        }
        return false;
    }

    if (changed) {
        *changed = true;
    }
    return true;
}
}

namespace DXGIShared {
ce::presentation_color::Encoding ResolveSwapChainPresentationEncoding(IDXGISwapChain* swapChain,
                                                                      DXGI_FORMAT format,
                                                                      DXGI_COLOR_SPACE_TYPE* trackedColorSpace,
                                                                      bool* hasTrackedColorSpace) {
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    const bool tracked = QuerySwapChainColorSpace(swapChain, colorSpace);
    if (trackedColorSpace)
        *trackedColorSpace = colorSpace;
    if (hasTrackedColorSpace)
        *hasTrackedColorSpace = tracked;
    return ce::presentation_color::ResolveDXGI(format, tracked, colorSpace);
}
}

namespace DXGIShared {
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
SharedState g_SharedState;
}

namespace DXGIShared {
std::mutex g_SharedMutex;
}

namespace DXGIShared {
static std::mutex s_thirdPartyOverlaySwapchainMutex;
}

namespace DXGIShared {
static std::unordered_set<IDXGISwapChain*> s_thirdPartyOverlaySwapchains;
}

namespace DXGIShared {
static std::unordered_set<IDXGISwapChain*> s_startupBlockingOverlayTaggedSwapchains;
}

namespace DXGIShared {
// Post-SL FG overlay callback (set by dx12_hook.cpp when SL FG is active).
std::atomic<PostSLOverlayRenderFn> g_PostSLOverlayRenderCallback{nullptr};
}

namespace DXGIShared {
// Post-SL startup activation service (set by dx12_hook.cpp).
std::atomic<PostSLStartupActivationServiceFn> g_PostSLStartupActivationService{nullptr};
}

namespace DXGIShared {
// Direct Streamline FG running signal (set by streamline_hook.cpp).
std::atomic<bool> g_StreamlineFGRunning{false};
}

namespace DXGIShared {
// Present call counter — incremented by DetourPresent and DetourPresent1, read by
// SL hook to detect bypass.
std::atomic<uint64_t> g_PresentCallCounter{0};
}

namespace DXGIShared {
void DX12_RegisterThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain, const char* creatorModulePath) {
    if (!pSwapChain) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_thirdPartyOverlaySwapchainMutex);
    s_thirdPartyOverlaySwapchains.insert(pSwapChain);
    if (ce::overlay_compat::IsStartupBlockingOverlayModulePath(creatorModulePath)) {
        s_startupBlockingOverlayTaggedSwapchains.insert(pSwapChain);
    } else {
        s_startupBlockingOverlayTaggedSwapchains.erase(pSwapChain);
    }
}
}

namespace DXGIShared {
void DX12_UnregisterThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_thirdPartyOverlaySwapchainMutex);
    s_thirdPartyOverlaySwapchains.erase(pSwapChain);
    s_startupBlockingOverlayTaggedSwapchains.erase(pSwapChain);
}
}

namespace DXGIShared {
bool DX12_IsThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) {
        return false;
    }

    std::lock_guard<std::mutex> lock(s_thirdPartyOverlaySwapchainMutex);
    return s_thirdPartyOverlaySwapchains.find(pSwapChain) != s_thirdPartyOverlaySwapchains.end();
}
}

namespace DXGIShared {
bool DX12_IsStartupBlockingOverlayTaggedSwapchain(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) {
        return false;
    }

    std::lock_guard<std::mutex> lock(s_thirdPartyOverlaySwapchainMutex);
    return s_startupBlockingOverlayTaggedSwapchains.find(pSwapChain) != s_startupBlockingOverlayTaggedSwapchains.end();
}
}

namespace DXGIShared {
static std::atomic<uint32_t> g_LatestSourceFrameIndex{0};
}

namespace DXGIShared {
static std::atomic<DWORD> g_resizeThreadId{0};
}

namespace DXGIShared {
static std::atomic<int> g_resizeDepth{0};
}

namespace DXGIShared {
// Present-scoped duplicate suppression for the explicit-OFF exact-proxy route.
// This is thread-local because a different Streamline worker Present represents
// a distinct output frame and must retain its own PostSL draw.
static thread_local uint32_t s_postSLOffKeepAlivePresentScopeDepth = 0;
}

namespace DXGIShared {
static thread_local bool s_postSLOffKeepAlivePrePresentDrawn = false;
}

namespace DXGIShared {
void BeginPostSLOffKeepAlivePresentScope() {
    if (s_postSLOffKeepAlivePresentScopeDepth++ == 0) {
        s_postSLOffKeepAlivePrePresentDrawn = false;
    }
}
}

namespace DXGIShared {
void EndPostSLOffKeepAlivePresentScope() {
    if (s_postSLOffKeepAlivePresentScopeDepth != 0) {
        --s_postSLOffKeepAlivePresentScopeDepth;
        if (s_postSLOffKeepAlivePresentScopeDepth == 0) {
        s_postSLOffKeepAlivePrePresentDrawn = false;
        }
    }
}
}

namespace DXGIShared {
void MarkPostSLOffKeepAlivePrePresentDrawn() {
    if (s_postSLOffKeepAlivePresentScopeDepth != 0) {
        s_postSLOffKeepAlivePrePresentDrawn = true;
    }
}
}

namespace DXGIShared {
bool WasPostSLOffKeepAlivePrePresentDrawn() {
    return s_postSLOffKeepAlivePresentScopeDepth != 0 && s_postSLOffKeepAlivePrePresentDrawn;
}
}

namespace DXGIShared {
static bool IsRecursiveResize() {
    DWORD currentId = GetCurrentThreadId();
    DWORD expected = g_resizeThreadId.load(std::memory_order_acquire);

    if (expected == currentId && g_resizeDepth.load(std::memory_order_acquire) > 0) {
        return true;
    }

    DWORD zero = 0;
    if (g_resizeThreadId.compare_exchange_strong(zero, currentId, std::memory_order_acq_rel)) {
        g_resizeDepth.store(1, std::memory_order_release);
        return false;
    }

    if (g_resizeThreadId.load(std::memory_order_acquire) == currentId) {
        g_resizeDepth.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }

    return false;
}
}

namespace DXGIShared {
static void ReleaseResize() {
    if (g_resizeDepth.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        g_resizeThreadId.store(0, std::memory_order_release);
    }
}
}

namespace DXGIShared {
static PFN_ResizeBuffers oResizeBuffers = nullptr;
}

namespace DXGIShared {
static PFN_ResizeBuffers1 oResizeBuffers1 = nullptr;
}

namespace DXGIShared {
static std::atomic<PFN_SetColorSpace1> oSetColorSpace1Trampoline{nullptr};
}

namespace DXGIShared {
static std::mutex s_setColorSpace1HookMutex;
}

namespace DXGIShared {
static thread_local unsigned s_wrapperSetColorSpaceForwardDepth = 0;
}

namespace DXGIShared {
// Vulkan detection via ICD layer - returns false since we use layer approach
bool IsVulkanPrimary() {
    // VK_LAYER_CE_overlay handles Vulkan separately
    return false;
}
}

namespace DXGIShared {
static UINT ResolvePresentFrameLatencyOverride(const char** sourceOut) {
    const auto& cfg = GetActiveGraphicsConfig();

    if (cfg.frameLatency > 0) {
        if (sourceOut)
            *sourceOut = "frame_latency";
        return static_cast<UINT>(cfg.frameLatency);
    }
    if (cfg.cpuPrerenderLimit > 0) {
        if (sourceOut)
            *sourceOut = "cpu_prerender_limit";
        return static_cast<UINT>(cfg.cpuPrerenderLimit);
    }
    if (HasBackbufferCountOverride(cfg.backbufferCount)) {
        if (sourceOut)
            *sourceOut = "backbuffer_count-equivalent-depth";
        return static_cast<UINT>(cfg.backbufferCount - 1);
    }

    if (sourceOut)
        *sourceOut = nullptr;
    return 0;
}
}

namespace DXGIShared {
// Wait for DWM flip queue room when backbuffer_count override is active.
// Uses DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT (applied at
// creation) to pace presents so the effective vsync queue depth matches
// the override count, without changing the physical BufferCount.
void WaitBackbufferFrameLatency(IDXGISwapChain* pSwapChain) {
    const auto& gfx = GetActiveGraphicsConfig();
    if (!HasBackbufferCountOverride(gfx.backbufferCount)) {
        static int s_logCount = 0;
        if (s_logCount++ < 3)
            HookLog("WaitBackbufferFrameLatency: no override (count=%d)", gfx.backbufferCount);
        return;
    }

    IDXGISwapChain2* pSC2 = nullptr;
    HRESULT hrQI = pSwapChain->QueryInterface(IID_PPV_ARGS(&pSC2));
    if (FAILED(hrQI) || !pSC2) {
        static int s_logCount = 0;
        if (s_logCount++ < 5)
            HookLog("WaitBackbufferFrameLatency: IDXGISwapChain2 QI failed hr=0x%08X", hrQI);
        return;
    }

    HANDLE hWaitable = pSC2->GetFrameLatencyWaitableObject();
    if (!hWaitable || hWaitable == INVALID_HANDLE_VALUE) {
        static int s_logCount = 0;
        if (s_logCount++ < 5)
            HookLog("WaitBackbufferFrameLatency: GetFrameLatencyWaitableObject returned invalid handle");
        pSC2->Release();
        return;
    }

    DWORD waitResult = WaitForSingleObject(hWaitable, INFINITE);
    if (waitResult == WAIT_OBJECT_0) {
        static int s_logCount = 0;
        if (s_logCount++ < 3)
            HookLog("WaitBackbufferFrameLatency: wait succeeded");
    } else {
        static std::atomic<int> s_waitFailLogCount{0};
        if (s_waitFailLogCount.fetch_add(1, std::memory_order_relaxed) < 10)
            HookLogImportant("WaitBackbufferFrameLatency: wait failed result=%lu error=%lu", waitResult,
                             GetLastError());
    }
    pSC2->Release();
}
}

namespace DXGIShared {
// Apply user-configured present-queue latency overrides to an existing swapchain.
// NOTE: backbuffer_count is handled at swapchain creation and resize time.
void ApplyPresentFrameLatencyOverrides(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return;

    const char* source = nullptr;
    UINT requested = ResolvePresentFrameLatencyOverride(&source);
    if (requested > 16)
        requested = 16;

    static std::mutex s_latencyOverrideMutex;
    static uint64_t s_lastSwapchain = 0;
    static UINT s_lastRequested = 0;

    const uint64_t scKey = reinterpret_cast<uint64_t>(pSwapChain);

    if (requested == 0) {
        return;
    }

    IDXGISwapChain2* sc2 = nullptr;
    if (FAILED(pSwapChain->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2)) || !sc2) {
        static std::atomic<int> s_qiFailLogCount{0};
        if (requested > 0 && s_qiFailLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            HookLogImportant("ApplyPresentFrameLatencyOverrides: IDXGISwapChain2 unavailable for %s",
                             source ? source : "override");
        }
        return;
    }

    std::lock_guard<std::mutex> lock(s_latencyOverrideMutex);

    if (s_lastSwapchain == scKey && s_lastRequested == requested) {
        sc2->Release();
        return;
    }

    HRESULT hr = sc2->SetMaximumFrameLatency(requested);
    if (SUCCEEDED(hr)) {
        HookLogImportant("ApplyPresentFrameLatencyOverrides: SetMaximumFrameLatency(%u) OK (%s)", requested,
                         source ? source : "override");
    } else {
        HookLogImportant("ApplyPresentFrameLatencyOverrides: SetMaximumFrameLatency(%u) failed hr=0x%08X (%s)",
                         requested, hr, source ? source : "override");
    }

    s_lastSwapchain = scKey;
    s_lastRequested = requested;

    sc2->Release();
}
}

namespace DXGIShared {
PerformanceMetrics* GetPerformanceMetrics() {
    return &dxgi_shared_g_DXGIPerfMetrics;
}
}

namespace DXGIShared {
uint32_t GetLatestSourceFrameIndex() {
    return g_LatestSourceFrameIndex.load(std::memory_order_relaxed);
}
}

namespace DXGIShared {
void SetLatestSourceFrameIndex(uint32_t frameIndex) {
    g_LatestSourceFrameIndex.store(frameIndex, std::memory_order_relaxed);
}
}

namespace DXGIShared {
APIType DetectAPIType(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return APIType::Unknown;

    // Fast path: avoid expensive GetDevice() calls every Present on the same
    // swapchain/thread.
    thread_local IDXGISwapChain* s_cachedSwapchain = nullptr;
    thread_local APIType s_cachedApi = APIType::Unknown;
    if (pSwapChain == s_cachedSwapchain && s_cachedApi != APIType::Unknown) {
        return s_cachedApi;
    }

    bool hasD3D12Device = false;
    bool hasD3D11Device = false;
    bool hasD3D10Device = false;

    ID3D12Device* d12Device = nullptr;
    if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D12Device), (void**)&d12Device)) && d12Device) {
        d12Device->Release();
        hasD3D12Device = true;
    }

    // Always try all three — do NOT short-circuit when D3D11 succeeds.
    // On Windows 10+ the D3D10 runtime is implemented on D3D11
    // (D3D10-on-D3D11).  A D3D10 device will QI for BOTH ID3D11Device
    // and ID3D10Device, so checking D3D11 first and skipping D3D10
    // would wrongly classify the swapchain as D3D11.
    ID3D11Device* d11Device = nullptr;
    if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&d11Device)) && d11Device) {
        d11Device->Release();
        hasD3D11Device = true;
    }

    ID3D10Device* d10Device = nullptr;
    if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D10Device), (void**)&d10Device)) && d10Device) {
        d10Device->Release();
        hasD3D10Device = true;
    } else {
        ID3D10Device1* d10Device1 = nullptr;
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D10Device1), (void**)&d10Device1)) && d10Device1) {
            d10Device1->Release();
            hasD3D10Device = true;
        }
    }

    APIType detected = SelectPrimarySwapChainAPIType(hasD3D12Device, hasD3D11Device, hasD3D10Device);

    s_cachedSwapchain = pSwapChain;
    s_cachedApi = detected;
    return detected;
}
}

namespace DXGIShared {
// Helper to get Present function address from a swapchain's vtable
static void* GetPresentAddress(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return nullptr;

    void** vtable = *(void***)pSwapChain;
    return vtable[8];  // Present is at vtable slot 8
}
}

namespace DXGIShared {
// Helper to get Present1 function address from a swapchain's vtable
static void* GetPresent1Address(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return nullptr;
    void** vtable = *(void***)pSwapChain;
    return vtable[22];  // Present1 is at vtable slot 22
}
}

namespace DXGIShared {
// Forward declaration for lazy hook installation
static void InstallHooksIfPending(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
static bool IsThirdPartyOverlayLoaded() {
    return ce::overlay_compat::IsThirdPartyOverlayLoaded();
}
}

namespace DXGIShared {
// Opt-in kill-switch for the Round-4 RTSS-style eager overlay draw during a runtime DLSS-FG
// toggle-ON. Default OFF; enable with CE_DLSS_TOGGLE_OVERLAY_EAGER=1 (Steam launch options or a
// system env var). Evaluated once and cached.
bool IsDlssToggleEagerOverlayEnabled() {
    static const bool enabled = []() {
        char b[8] = {};
        DWORD n = GetEnvironmentVariableA("CE_DLSS_TOGGLE_OVERLAY_EAGER", b, sizeof(b));
        return n > 0 && n < sizeof(b) && (b[0] == '1' || b[0] == 'y' || b[0] == 'Y' || b[0] == 't' || b[0] == 'T');
    }();
    return enabled;
}
}

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height,
                                              DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    // CRITICAL: Check for global shutdown - if app is closing, don't touch
    // anything
    if (IsShuttingDown()) {
        if (oResizeBuffers) {
            return oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        }
        return S_OK;
    }

    // Apply backbuffer count override from config
    // When the game calls ResizeBuffers (window resize, alt-tab, resolution change),
    // this ensures our buffer count is applied even if CreateSwapChain override was missed.
    {
        const auto& cfg = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(cfg.backbufferCount)) {
            UINT requested = static_cast<UINT>(cfg.backbufferCount);
            if (requested > 0 && requested != BufferCount) {
                // Check swap effect for flip-model safety
                DXGI_SWAP_CHAIN_DESC scDesc = {};
                bool canOverride = true;
                if (SUCCEEDED(pSwapChain->GetDesc(&scDesc))) {
                    bool isFlip = (scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                                   scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
                    if (isFlip && requested < BufferCount) {
                        SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
                        canOverride = false;
                        HookLog("DetourResizeBuffers: Skipping BufferCount override %u < game's %u (flip model)",
                                requested, BufferCount);
                    }
                }
                if (canOverride) {
                    HookLogImportant("DetourResizeBuffers: Overriding BufferCount %u -> %u", BufferCount, requested);
                    BufferCount = requested;
                }
            }
        }
    }

    // CRITICAL FIX: When Vulkan is active, pass through DXGI ResizeBuffers calls
    if (IsVulkanActive()) {
        return oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    // AGGRESSIVE RECURSION GUARD: Steam overlay causes infinite recursion through
    // hook chain
    if (IsRecursiveResize()) {
        // Recursion detected - call original directly through vtable to bypass
        // Steam's hook
        void** vtable = *(void***)pSwapChain;
        typedef HRESULT(STDMETHODCALLTYPE * PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
        PFN_ResizeBuffers originalResize = (PFN_ResizeBuffers)vtable[13];  // ResizeBuffers is at index 13
        return originalResize(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    if (g_SharedState.wrapperResizeDepth.fetch_add(1) > 0) {
        HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    // Check if this is our wrapper swapchain - if so, skip resize handling
    void* pWrapperTest = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, &pWrapperTest))) {
        ((IUnknown*)pWrapperTest)->Release();
        HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    g_SharedState.swapchainInvalid.store(true);

    APIType api = DetectAPIType(pSwapChain);

    // CRITICAL FIX: Skip DX12 resize handling during initial swapchain creation
    // Some games call ResizeBuffers immediately after CreateSwapChain
    static std::atomic<int> s_initialResizeCount{0};
    if (api == APIType::D3D12 && s_initialResizeCount.fetch_add(1) == 0) {
        HookLog("DXGI: ResizeBuffers - FIRST D3D12 resize, direct vtable call");
        // Call directly through vtable to bypass any hook chain issues
        void** vtable = *(void***)pSwapChain;
        typedef HRESULT(STDMETHODCALLTYPE * PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
        PFN_ResizeBuffers originalResize = (PFN_ResizeBuffers)vtable[13];
        HRESULT hr = originalResize(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        HookLog("DXGI: ResizeBuffers - first D3D12 resize returned hr=0x%08X", hr);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);

        ReleaseResize();
        return hr;
    }

    if (api == APIType::D3D12)
        HandleDX12ResizeBegin();
    else if (api == APIType::D3D11)
        HandleDX11ResizeBegin();

    HookLog("DXGI: ResizeBuffers - calling oResizeBuffers...");
    HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    HookLog("DXGI: ResizeBuffers - oResizeBuffers returned hr=0x%08X", hr);

    if (FAILED(hr)) {
        HookLog("DXGI: ResizeBuffers FAILED with 0x%08X", hr);
    } else {
        HookLog("DXGI: ResizeBuffers SUCCESS");
    }

    // Reset resize flags after resize completes
    if (api == APIType::D3D12) {
        HookLog("DXGI: ResizeBuffers - calling HandleDX12ResizeEnd...");
        HandleDX12ResizeEnd();
        HookLog("DXGI: ResizeBuffers - HandleDX12ResizeEnd returned");
    }

    g_SharedState.swapchainInvalid.store(false);
    g_SharedState.wrapperResizeDepth.fetch_sub(1);
    ReleaseResize();
    return hr;
}
}

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourResizeBuffers1(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height,
                                               DXGI_FORMAT NewFormat, UINT SwapChainFlags,
                                               const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue) {
    // Apply backbuffer count override from config
    {
        const auto& cfg = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(cfg.backbufferCount)) {
            UINT requested = static_cast<UINT>(cfg.backbufferCount);
            if (requested > 0 && requested != BufferCount) {
                DXGI_SWAP_CHAIN_DESC scDesc = {};
                bool canOverride = true;
                if (SUCCEEDED(pSwapChain->GetDesc(&scDesc))) {
                    bool isFlip = (scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                                   scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
                    if (isFlip && requested < BufferCount) {
                        SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
                        canOverride = false;
                        HookLog("DetourResizeBuffers1: Skipping BufferCount override %u < game's %u (flip model)",
                                requested, BufferCount);
                    }
                }
                if (canOverride) {
                    HookLogImportant("DetourResizeBuffers1: Overriding BufferCount %u -> %u", BufferCount, requested);
                    BufferCount = requested;
                }
            }
        }
    }

    // Vulkan passthrough
    if (IsVulkanActive()) {
        return oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask,
                               ppPresentQueue);
    }

    // AGGRESSIVE RECURSION GUARD: Steam overlay causes infinite recursion through
    // hook chain
    if (IsRecursiveResize()) {
        // Recursion detected - call original directly through vtable to bypass
        // Steam's hook
        void** vtable = *(void***)pSwapChain;
        typedef HRESULT(STDMETHODCALLTYPE * PFN_ResizeBuffers1)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                                                const UINT*, IUnknown* const*);
        PFN_ResizeBuffers1 originalResize1 = (PFN_ResizeBuffers1)vtable[39];  // ResizeBuffers1 is at index 39
        return originalResize1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask,
                               ppPresentQueue);
    }

    if (g_SharedState.wrapperResizeDepth.fetch_add(1) > 0) {
        HRESULT hr = oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags,
                                     pCreationNodeMask, ppPresentQueue);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    // Check if this is our wrapper swapchain - if so, skip resize handling
    void* pWrapperTest = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, &pWrapperTest))) {
        ((IUnknown*)pWrapperTest)->Release();
        HRESULT hr = oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags,
                                     pCreationNodeMask, ppPresentQueue);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    g_SharedState.swapchainInvalid.store(true);

    APIType api = DetectAPIType(pSwapChain);

    // CRITICAL FIX: Skip DX12 resize handling during initial swapchain creation
    // Some games call ResizeBuffers immediately after CreateSwapChain
    static std::atomic<int> s_initialResizeCount{0};
    if (api == APIType::D3D12 && s_initialResizeCount.fetch_add(1) == 0) {
        HookLog("DXGI: ResizeBuffers - FIRST D3D12 resize, direct vtable call");
        // Call directly through vtable to bypass any hook chain issues
        void** vtable = *(void***)pSwapChain;
        typedef HRESULT(STDMETHODCALLTYPE * PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
        PFN_ResizeBuffers originalResize = (PFN_ResizeBuffers)vtable[13];
        HRESULT hr = originalResize(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        HookLog("DXGI: ResizeBuffers - first D3D12 resize returned hr=0x%08X", hr);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    if (api == APIType::D3D12)
        HandleDX12ResizeBegin();
    else if (api == APIType::D3D11)
        HandleDX11ResizeBegin();

    HRESULT hr = oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask,
                                 ppPresentQueue);

    if (FAILED(hr)) {
        HookLog("DXGI: ResizeBuffers1 FAILED with 0x%08X", hr);
    } else {
        HookLog("DXGI: ResizeBuffers1 SUCCESS");
    }

    // Reset resize flags after resize completes
    if (api == APIType::D3D12)
        HandleDX12ResizeEnd();

    g_SharedState.swapchainInvalid.store(false);
    g_SharedState.wrapperResizeDepth.fetch_sub(1);
    ReleaseResize();
    return hr;
}
}

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourSetColorSpace1(IDXGISwapChain* pSwapChain, DXGI_COLOR_SPACE_TYPE colorSpace) {
    const PFN_SetColorSpace1 original = oSetColorSpace1Trampoline.load(std::memory_order_acquire);
    if (!original) {
        static std::atomic<int> s_missingTrampolineLogCount{0};
        if (s_missingTrampolineLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            HookLogImportant(
                "DXGI: SetColorSpace1 detour entered without a published trampoline; failing closed sc=%p cs=%d",
                pSwapChain, static_cast<int>(colorSpace));
        }
        return DXGI_ERROR_UNSUPPORTED;
    }

    const HRESULT result = original(pSwapChain, colorSpace);
    if (SUCCEEDED(result) &&
        ce::presentation_color::ShouldRecordDetouredColorSpaceChange(s_wrapperSetColorSpaceForwardDepth)) {
        bool changed = false;
        if (RecordSwapChainColorSpace(pSwapChain, colorSpace, &changed) && changed) {
            HookLogImportant("DXGI: Swapchain presentation color space changed source=inline sc=%p cs=%d",
                             pSwapChain, static_cast<int>(colorSpace));
        }
    }
    return result;
}
}

namespace DXGIShared {
HRESULT SetSwapChainColorSpaceFromWrapper(IDXGISwapChain3* callableSwapChain, IDXGISwapChain* identitySwapChain,
                                          DXGI_COLOR_SPACE_TYPE colorSpace) {
    if (!callableSwapChain) {
        return DXGI_ERROR_UNSUPPORTED;
    }

    ++s_wrapperSetColorSpaceForwardDepth;
    const auto depthGuard = ce::make_scope_guard([]() { --s_wrapperSetColorSpaceForwardDepth; });
    const HRESULT result = callableSwapChain->SetColorSpace1(colorSpace);
    if (SUCCEEDED(result)) {
        IDXGISwapChain* identity = identitySwapChain ? identitySwapChain : callableSwapChain;
        bool changed = false;
        if (RecordSwapChainColorSpace(identity, colorSpace, &changed) && changed) {
            HookLogImportant("DXGI: Swapchain presentation color space changed source=wrapper sc=%p cs=%d",
                             identity, static_cast<int>(colorSpace));
        }
    }
    return result;
}
}

namespace DXGIShared {
static void PublishSetColorSpace1Trampoline(void* trampoline, void*) {
    oSetColorSpace1Trampoline.store(reinterpret_cast<PFN_SetColorSpace1>(trampoline), std::memory_order_release);
}
}

namespace DXGIShared {
static bool InstallSetColorSpace1InlineHook(IDXGISwapChain* pSwapChain, const char* source) {
    if (!pSwapChain) {
        return false;
    }
    if (oSetColorSpace1Trampoline.load(std::memory_order_acquire)) {
        return true;
    }

    // Inline-hook the real DXGI implementation only. Hooking the wrapper method
    // would make the detour's trampoline call back into the wrapper and recreate
    // the unsafe wrapper/detour composition that this path is designed to avoid.
    if (IsWrappedSwapChainObject(pSwapChain)) {
        static std::atomic<int> s_wrapperTargetLogCount{0};
        if (s_wrapperTargetLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLog("DXGI: SetColorSpace1 inline tracking skipped for wrapped %s swapchain %p",
                    source ? source : "unknown", pSwapChain);
        }
        return false;
    }

    std::lock_guard<std::mutex> installLock(s_setColorSpace1HookMutex);
    if (oSetColorSpace1Trampoline.load(std::memory_order_acquire)) {
        return true;
    }

    IDXGISwapChain3* colorSpaceSwapChain = nullptr;
    if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&colorSpaceSwapChain))) || !colorSpaceSwapChain) {
        static std::atomic<int> s_unsupportedLogCount{0};
        if (s_unsupportedLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLog("DXGI: SetColorSpace1 tracking unavailable for %s swapchain %p (IDXGISwapChain3 unsupported)",
                    source ? source : "unknown", pSwapChain);
        }
        return false;
    }

    void** colorSpaceVtable = *reinterpret_cast<void***>(colorSpaceSwapChain);
    void* colorSpaceAddress =
        colorSpaceVtable && IsReadableMemory(reinterpret_cast<const void*>(&colorSpaceVtable[38]), sizeof(void*)) ? colorSpaceVtable[38] : nullptr;
    colorSpaceSwapChain->Release();
    if (!colorSpaceAddress || colorSpaceAddress == reinterpret_cast<void*>(DetourSetColorSpace1)) {
        HookLogImportant("DXGI: Refusing unsafe SetColorSpace1 hook target source=%s sc=%p target=%p",
                         source ? source : "unknown", pSwapChain, colorSpaceAddress);
        return false;
    }

    void* colorSpaceTrampoline = nullptr;
    if (!InlineHook::InstallPublished(colorSpaceAddress, reinterpret_cast<void*>(DetourSetColorSpace1),
                                      &colorSpaceTrampoline, PublishSetColorSpace1Trampoline, nullptr)) {
        if (oSetColorSpace1Trampoline.load(std::memory_order_acquire)) {
            return true;
        }
        HookLogImportant(
            "DXGI: SetColorSpace1 inline tracking unavailable source=%s sc=%p target=%p; wrapper tracking remains available",
            source ? source : "unknown", pSwapChain, colorSpaceAddress);
        return false;
    }

    HookLogImportant("DXGI: SetColorSpace1 inline tracking installed source=%s target=%p trampoline=%p",
                     source ? source : "unknown", colorSpaceAddress, colorSpaceTrampoline);
    return true;
}
}

namespace DXGIShared {
bool InstallHooks(IDXGISwapChain* pSwapChain, bool presentOnly) {
    // NOTE: This function should only be called for DX11/DX10 games.
    // DX12 games use wrapper-based Present interception (CWrapDXGISwapChain).
    // Calling this for DX12 can cause conflicts and stack overflow crashes
    // due to two competing Present interception mechanisms.
    // See: dx12_hook.cpp for the wrapper-based approach.

    if (!pSwapChain)
        return false;

    static std::atomic<int> s_installCount{0};
    int count = s_installCount.fetch_add(1);
    HookLog("DXGIShared::InstallHooks CALLED #%d (swapchain=%p, presentOnly=%d)", count, pSwapChain,
            presentOnly ? 1 : 0);

    InstallSetColorSpace1InlineHook(pSwapChain, "vtable-path");

    // Third-party overlays can install their own DXGI hooks and form recursive
    // Present chains with vtable patching. In that case the wrapper-based path
    // remains active and avoids hook wars.
    if (!DXGIShared::ShouldInstallSwapchainHooksWithThirdPartyOverlay(IsThirdPartyOverlayLoaded(),
                                                                      HasPresentDetourHooks())) {
        HookLog("DXGIShared::InstallHooks: External overlay detected, skipping DXGI swapchain hooks");
        return true;
    }

    if (dxgi_shared_s_hookedVTable) {
        void** newVTable = *(void***)pSwapChain;
        if (newVTable == dxgi_shared_s_hookedVTable) {
            HookLog("DXGIShared::InstallHooks: Hooks already installed on vtable %p", dxgi_shared_s_hookedVTable);
            return true;
        }
        // New swapchain with a DIFFERENT vtable — need to re-hook.
        HookLogImportant("DXGIShared::InstallHooks: NEW vtable detected (old=%p new=%p) — re-hooking", dxgi_shared_s_hookedVTable,
                         newVTable);
    }

    void** vtable = *(void***)pSwapChain;
    if (!vtable) {
        HookLog("DXGIShared::InstallHooks: Invalid vtable");
        return false;
    }

    DWORD oldProtect;
    if (!VirtualProtect(reinterpret_cast<void*>(vtable), 40 * sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        HookLog("DXGIShared::InstallHooks: VirtualProtect failed");
        return false;
    }

    dxgi_shared_s_hookedVTable = vtable;

    dxgi_shared_oPresent = (PFN_Present)vtable[8];
    vtable[8] = (void*)DetourPresent;
    HookLog("DXGIShared: Hooked Present at vtable[8] (original=%p, detour=%p)", dxgi_shared_oPresent, DetourPresent);

    dxgi_shared_oPresent1 = (PFN_Present1)vtable[22];
    vtable[22] = (void*)DetourPresent1;
    HookLog("DXGIShared: Hooked Present1 at vtable[22] (original=%p, detour=%p)", dxgi_shared_oPresent1, DetourPresent1);

    if (!presentOnly) {
        oResizeBuffers = (PFN_ResizeBuffers)vtable[13];
        vtable[13] = (void*)DetourResizeBuffers;
        HookLog(
            "DXGIShared: Hooked ResizeBuffers at vtable[13] (original=%p, "
            "detour=%p)",
            oResizeBuffers, DetourResizeBuffers);

        oResizeBuffers1 = (PFN_ResizeBuffers1)vtable[39];
        vtable[39] = (void*)DetourResizeBuffers1;
        HookLog(
            "DXGIShared: Hooked ResizeBuffers1 at vtable[39] (original=%p, "
            "detour=%p)",
            oResizeBuffers1, DetourResizeBuffers1);
    }

    VirtualProtect(reinterpret_cast<void*>(vtable), 40 * sizeof(void*), oldProtect, &oldProtect);
    HookLog("DXGIShared::InstallHooks: All vtable hooks installed successfully");
    return true;
}
}

namespace DXGIShared {
bool HasPresentInlineHooks() {
    return dxgi_shared_oPresentTrampoline != nullptr || dxgi_shared_oPresent1Trampoline != nullptr;
}
}

namespace DXGIShared {
bool HasPresentDetourHooks() {
    return dxgi_shared_s_hookedVTable != nullptr || dxgi_shared_oPresentTrampoline != nullptr || dxgi_shared_oPresent1Trampoline != nullptr;
}
}

namespace DXGIShared {
bool CanSafelyInstallExternalPresentDetourPath(bool requiresBypassTrampoline, bool bypassTrampolineAvailable) {
    return !requiresBypassTrampoline || bypassTrampolineAvailable;
}
}

namespace DXGIShared {
// Lazy hook installation - installs hooks on first Present if they were
// deferred during swapchain creation
static IDXGISwapChain* s_PendingSwapChainForLazyHook = nullptr;
}

namespace DXGIShared {
static std::atomic<bool> s_LazyHooksInstalled{false};
}

namespace DXGIShared {
void SetPendingSwapChainForLazyHook(IDXGISwapChain* pSwapChain) {
    if (pSwapChain) {
        pSwapChain->AddRef();
    }
    if (s_PendingSwapChainForLazyHook) {
        s_PendingSwapChainForLazyHook->Release();
    }
    s_PendingSwapChainForLazyHook = pSwapChain;
    HookLog("DXGIShared: SetPendingSwapChainForLazyHook called");
}
}

namespace DXGIShared {
static void InstallHooksIfPending(IDXGISwapChain* pSwapChain) {
    if (s_LazyHooksInstalled.load(std::memory_order_acquire))
        return;

    // Check if this is the pending swapchain
    if (pSwapChain == s_PendingSwapChainForLazyHook) {
        HookLog("DXGIShared: Installing hooks lazily on first Present");
        // CRITICAL: Use presentOnly=true to only hook Present/Present1
        // ResizeBuffers hooks can cause stack overflow crashes with some overlays
        InstallHooks(pSwapChain, true);
        s_LazyHooksInstalled.store(true, std::memory_order_release);
        if (s_PendingSwapChainForLazyHook) {
            s_PendingSwapChainForLazyHook->Release();
            s_PendingSwapChainForLazyHook = nullptr;
        }
    }
}
}

namespace DXGIShared {
void Init() {
    g_SharedState.lastSwapchainCreation = std::chrono::steady_clock::now();
    // Early detection of NVIDIA Smooth Motion module
    g_FGCompat.CheckForNvPresent();
}
}

namespace DXGIShared {
bool InstallPresentInlineHooks(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return false;

    InstallSetColorSpace1InlineHook(pSwapChain, "present-bootstrap");

    void* presentAddr = GetPresentAddress(pSwapChain);
    void* present1Addr = GetPresent1Address(pSwapChain);

    if (!presentAddr) {
        HookLog("InstallPresentInlineHooks: Failed to get Present address");
        return false;
    }

    // Save original vtable[8] before any modifications. This captures the real
    // COM method (dxgi!CDXGISwapChain::Present or equivalent) from the temp
    // swapchain, before CE patches it to DetourPresent. Used later in
    // CallOriginalPresent and AttemptSteamDX12OverlayInit to ensure DXGI COM
    // method state management runs before dxgi!Present is called with Steam's
    // E9 JMP.
    {
        void** vtable = *(void***)pSwapChain;
        if (vtable && IsReadableMemory(reinterpret_cast<const void*>(&vtable[8]), sizeof(void*))) {
            if (!dxgi_shared_s_originalVtable8Present) {
                dxgi_shared_s_originalVtable8Present = (PFN_Present)vtable[8];
                // Log the saved address and compare with GetPresentAddress
                HookLogImportant(
                    "InstallPresentInlineHooks: Saved s_originalVtable8Present=%p from temp swapchain %p "
                    "(presentAddr=%p, same=%d)",
                    (void*)dxgi_shared_s_originalVtable8Present, (void*)pSwapChain, presentAddr,
                    dxgi_shared_s_originalVtable8Present == (PFN_Present)presentAddr ? 1 : 0);
                // Log which module presentAddr belongs to for debugging
                HMODULE hAddrModule = nullptr;
                if (GetModuleHandleExA(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCSTR)presentAddr, &hAddrModule)) {
                    char modulePath[MAX_PATH] = {};
                    GetModuleFileNameA(hAddrModule, modulePath, sizeof(modulePath));
                    HookLogImportant("InstallPresentInlineHooks: presentAddr=%p is in module: %s", presentAddr,
                                     modulePath[0] ? modulePath : "(unknown)");
                }
            }
        } else {
            HookLog("InstallPresentInlineHooks: Cannot read vtable[8] from temp swapchain %p", (void*)pSwapChain);
        }
    }

    static bool s_inlineHooksInstalled = false;
    if (s_inlineHooksInstalled) {
        HookLog("InstallPresentInlineHooks: Inline hooks already installed");
        return true;
    }

    // CRITICAL: Check if an external overlay has already hooked Present
    // External overlays (NVIDIA, Steam, Discord, etc.) actively re-hook Present
    // Fighting them causes a hook war that corrupts the call chain
    const uint8_t* code = (const uint8_t*)presentAddr;
    bool externalJmpDetected = false;

    if (code[0] == 0xE9) {
        // JMP rel32 detected - check where it points
        int32_t disp;
        memcpy(&disp, code + 1, 4);
        uintptr_t jumpTarget = (uintptr_t)(code + 5) + disp;

        // Check if the jump target is outside dxgi.dll
        HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
        if (hDXGI) {
            MODULEINFO dxgiInfo;
            if (GetModuleInformation(GetCurrentProcess(), hDXGI, &dxgiInfo, sizeof(dxgiInfo))) {
                uintptr_t dxgiStart = (uintptr_t)hDXGI;
                uintptr_t dxgiEnd = dxgiStart + dxgiInfo.SizeOfImage;

                if (jumpTarget < dxgiStart || jumpTarget >= dxgiEnd) {
                    externalJmpDetected = true;
                    // JMP points outside dxgi.dll - external overlay detected
                    HookLog("InstallPresentInlineHooks: External overlay detected!");
                    HookLog("InstallPresentInlineHooks: JMP at %p targets %p (outside dxgi.dll %p-%p)", presentAddr,
                            (void*)jumpTarget, (void*)dxgiStart, (void*)dxgiEnd);

                    // Check if the target module is a known overlay
                    HMODULE hTargetModule = nullptr;
                    GetModuleHandleExA(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCSTR)jumpTarget, &hTargetModule);

                    if (hTargetModule) {
                        char moduleName[MAX_PATH] = {0};
                        GetModuleFileNameA(hTargetModule, moduleName, MAX_PATH);
                        HookLog("InstallPresentInlineHooks: External overlay module: %s", moduleName);

                        // Known overlay modules that we should cooperate with
                        std::string moduleLower = moduleName;
                        std::transform(moduleLower.begin(), moduleLower.end(), moduleLower.begin(),
                                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

                        if (moduleLower.find("nvidia") != std::string::npos ||
                            moduleLower.find("nvngx") != std::string::npos ||
                            moduleLower.find("steam") != std::string::npos ||
                            moduleLower.find("gameoverlay") != std::string::npos ||
                            moduleLower.find("discord") != std::string::npos ||
                            moduleLower.find("overlay") != std::string::npos) {
                            HookLog("InstallPresentInlineHooks: External hook detected - cooperating (known overlay)");
                        }
                    } else {
                        // Skip inline hooks to prevent prologue corruption (black screen fix)
                        HookLog("InstallPresentInlineHooks: Unknown external JMP - possibly stale hook");
                    }
                }
            }
        }
    }

    if (externalJmpDetected) {
#ifdef _WIN64
        constexpr bool kRequiresBypassTrampolineOnInstall = false;
#else
        constexpr bool kRequiresBypassTrampolineOnInstall = true;
#endif

        void* presentBypass = InlineHook::CreateBypassTrampoline(presentAddr);
        if (!presentBypass) {
            HookLog("InstallPresentInlineHooks: WARNING - Failed to create Present bypass trampoline");
            if (!CanSafelyInstallExternalPresentDetourPath(kRequiresBypassTrampolineOnInstall, false)) {
                HookLogImportant(
                    "InstallPresentInlineHooks: External Present hook detected but no bypass trampoline is available - "
                    "skipping DXGI Present detour path");
                return false;
            }
        }

        void* present1Bypass = nullptr;
        if (present1Addr) {
            present1Bypass = InlineHook::CreateBypassTrampoline(present1Addr);
            if (!present1Bypass &&
                !CanSafelyInstallExternalPresentDetourPath(kRequiresBypassTrampolineOnInstall, false)) {
                HookLogImportant(
                    "InstallPresentInlineHooks: External Present1 hook detected but no bypass trampoline is available "
                    "- skipping DXGI Present detour path");
                return false;
            }
        }

        HookLogImportant("InstallPresentInlineHooks: External E9 JMP detected — using vtable hook path");
        // Save the external overlay hook target (Steam's OverlayHookD3D3) so we
        // can invoke it explicitly later when SL FG routing bypasses Steam's JMP.
        // This is done BEFORE SL overwrites the JMP with its own.
        // If the JMP target is not resolved (e.g. non-E9 JMP or unknown pattern),
        // g_externalOverlayPresentHook stays NULL and Steam overlay will not
        // be explicitly invoked on the forced-bypass path — the overlay module
        // must hook a different Present entry point (e.g. vtable[8] or Present1).
        {
            void* hookTarget = ResolveE9JmpTarget(presentAddr);
            if (hookTarget) {
                dxgi_shared_g_externalOverlayPresentHook = (PFN_Present)hookTarget;
                HookLog("InstallPresentInlineHooks: External E9 JMP target = %p (saved, Steam overlay hook available)",
                        hookTarget);
            } else {
                HookLogImportant(
                    "InstallPresentInlineHooks: Could not resolve E9 JMP target at %p "
                    "(bytes: %02X %02X %02X %02X %02X) — external overlay hook not saved",
                    presentAddr, ((const uint8_t*)presentAddr)[0], ((const uint8_t*)presentAddr)[1],
                    ((const uint8_t*)presentAddr)[2], ((const uint8_t*)presentAddr)[3],
                    ((const uint8_t*)presentAddr)[4]);
            }
        }

        // External overlay (e.g. Streamline) has hooked Present with an E9 JMP.
        // DO NOT inline-hook the external detour — patching 14 bytes of the
        // external function's prologue corrupts its internal state and crashes
        // under Frame Generation (where more code paths are exercised).
        //
        // Instead, use a clean vtable hook:
        //   Game calls Present → vtable[8] → DetourPresent (our overlay) →
        //   CallOriginalPresent → oPresent (the SL-hooked function) →
        //   SL E9 JMP → SL processes FG normally → real Present
        //
        // Also create bypass trampolines from the original disk bytes so that
        // re-entrant Present calls (from SL's vtable callback) can call the real
        // DXGI Present without re-entering the external E9 JMP hook chain.

        void** vtable = *(void***)pSwapChain;
        if (!vtable) {
            HookLog("InstallPresentInlineHooks: External JMP detected but vtable is null");
            s_inlineHooksInstalled = true;
            return true;
        }

        DWORD oldProtect;
        if (!VirtualProtect(reinterpret_cast<void*>(vtable), 23 * sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            HookLog("InstallPresentInlineHooks: VirtualProtect failed for vtable hook");
            s_inlineHooksInstalled = true;
            return true;
        }

        dxgi_shared_s_hookedVTable = vtable;

        // === STEAM DX12 OVERLAY PRE-INITIALIZATION ===
        //
        // Steam's OverlayHookD3D3 lazily initializes internal Present-shaped
        // callback slots during its first E9 JMP entry. CE's vtable hook
        // (setting vtable[8] = DetourPresent below) prevents this natural
        // initialization because Steam's E9 JMP never fires on the game's
        // Present calls — they go through DetourPresent instead of dxgi!Present.
        //
        // Best-effort pre-init: Call Present on the temp swapchain through the
        // REAL dxgi!Present (vtable[8] is still unhooked at this point) BEFORE
        // installing our vtable hook.  This initializes Steam's "next" handler
        // but does NOT initialize every real game-swapchain callback slot
        // (the 2x2 hidden-window temp swapchain causes Steam to skip full init).
        //
        // The actual fix for the NULL rendering callback is the VEH-protected
        // call in AttemptSteamDX12OverlayInit (see below), which patches the
        // NULL pointer at crash time and lets Steam continue.
        //
        // Thread safety: InstallPresentInlineHooks runs once on the hook thread.
        // The temp swapchain is valid and vtable page is already writable
        // (from VirtualProtect at line ~2843).
            const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
            if (overlayModule && IsSteamOverlayModule(overlayModule)) {
                HookLogImportant(
                    "InstallPresentInlineHooks: Pre-initializing Steam overlay on temp swapchain %p "
                    "(vtable[8]=%p = dxgi!Present, before CE vtable hook)",
                    pSwapChain, (void*)vtable[8]);

                PFN_Present realPresent = (PFN_Present)vtable[8];
                HRESULT steamInitHr = realPresent(pSwapChain, 0, 0);

                HookLogImportant(
                    "InstallPresentInlineHooks: Steam overlay pre-init on temp swapchain "
                    "returned hr=0x%08X — proceeding with vtable hook installation",
                    (unsigned)steamInitHr);
        }
        // === END STEAM PRE-INIT ===

        dxgi_shared_oPresent = (PFN_Present)vtable[8];
        vtable[8] = (void*)DetourPresent;
        HookLogImportant(
            "InstallPresentInlineHooks: VTable hook on Present (original=%p, vtable=%p) — "
            "external E9 JMP detected, using non-invasive hook for FG compat",
            dxgi_shared_oPresent, vtable);

        if (presentBypass) {
            dxgi_shared_oPresentBypass = (PFN_Present)presentBypass;
            HookLog("InstallPresentInlineHooks: Present bypass trampoline created at %p", presentBypass);
        }

        if (present1Addr) {
            dxgi_shared_oPresent1 = (PFN_Present1)vtable[22];
            vtable[22] = (void*)DetourPresent1;
            HookLog("InstallPresentInlineHooks: VTable hook on Present1 (original=%p)", dxgi_shared_oPresent1);

            if (present1Bypass) {
                dxgi_shared_oPresent1Bypass = (PFN_Present1)present1Bypass;
                HookLog("InstallPresentInlineHooks: Present1 bypass trampoline created at %p", present1Bypass);
            }
        }

        VirtualProtect(reinterpret_cast<void*>(vtable), 23 * sizeof(void*), oldProtect, &oldProtect);

        s_inlineHooksInstalled = true;
        return true;
    }


    void* presentTrampoline = nullptr;
    if (!InlineHook::Install(presentAddr, (void*)DetourPresent, &presentTrampoline)) {
        HookLog("InstallPresentInlineHooks: Failed to install Present inline hook");
        return false;
    }
    dxgi_shared_oPresentTrampoline = (PFN_Present)presentTrampoline;
    dxgi_shared_oPresent = dxgi_shared_oPresentTrampoline;
    HookLogImportant(
        "InstallPresentInlineHooks: Present INLINE hook installed (addr=%p, "
        "trampoline=%p) — s_hookedVTable remains %p",
        presentAddr, presentTrampoline, dxgi_shared_s_hookedVTable);

    if (present1Addr) {
        void* present1Trampoline = nullptr;
        if (InlineHook::Install(present1Addr, (void*)DetourPresent1, &present1Trampoline)) {
            dxgi_shared_oPresent1Trampoline = (PFN_Present1)present1Trampoline;
            dxgi_shared_oPresent1 = dxgi_shared_oPresent1Trampoline;
            HookLog(
                "InstallPresentInlineHooks: Present1 inline hook installed "
                "(addr=%p, trampoline=%p)",
                present1Addr, present1Trampoline);
        }
    }

    s_inlineHooksInstalled = true;
    return true;
}
}

namespace DXGIShared {
void RemovePresentHooks() {
    InlineHook::RemoveAll();
    oSetColorSpace1Trampoline.store(nullptr, std::memory_order_release);

    dxgi_shared_s_slRoutingActive.store(false, std::memory_order_release);
    dxgi_shared_oPresentBypass = nullptr;
    dxgi_shared_oPresent1Bypass = nullptr;

    if (!dxgi_shared_s_hookedVTable)
        return;

    DWORD oldProtect;
    if (dxgi_shared_oPresent && dxgi_shared_s_hookedVTable[8] == (void*)DetourPresent) {
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), PAGE_READWRITE, &oldProtect);
        dxgi_shared_s_hookedVTable[8] = (void*)dxgi_shared_oPresent;
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed Present vtable hook");
    }

    if (dxgi_shared_oPresent1 && dxgi_shared_s_hookedVTable[22] == (void*)DetourPresent1) {
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[22]), sizeof(void*), PAGE_READWRITE, &oldProtect);
        dxgi_shared_s_hookedVTable[22] = (void*)dxgi_shared_oPresent1;
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[22]), sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed Present1 vtable hook");
    }

}
}

namespace DXGIShared {
void ReleaseSwapchainPresentVTableHooksForRuntimeHandoff(const char* reason) {
    if (!dxgi_shared_s_hookedVTable) {
        return;
    }
    if (!IsReadableMemory(reinterpret_cast<const void*>(dxgi_shared_s_hookedVTable), 23 * sizeof(void*))) {
        HookLogImportant(
            "DXGIShared: Cannot release Present vtable hooks for runtime handoff; vtable %p is not readable "
            "(reason=%s)",
            dxgi_shared_s_hookedVTable, reason ? reason : "unknown");
        return;
    }

    std::lock_guard<std::mutex> lock(g_SharedMutex);
    DWORD oldProtect = 0;
    bool restoredPresent = false;
    bool restoredPresent1 = false;

    if (dxgi_shared_oPresent && dxgi_shared_s_hookedVTable[8] == (void*)DetourPresent &&
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        dxgi_shared_s_hookedVTable[8] = (void*)dxgi_shared_oPresent;
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), oldProtect, &oldProtect);
        restoredPresent = true;
    }

    if (dxgi_shared_oPresent1 && dxgi_shared_s_hookedVTable[22] == (void*)DetourPresent1 &&
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[22]), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        dxgi_shared_s_hookedVTable[22] = (void*)dxgi_shared_oPresent1;
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[22]), sizeof(void*), oldProtect, &oldProtect);
        restoredPresent1 = true;
    }

    if (restoredPresent || restoredPresent1) {
        HookLogImportant(
            "DXGIShared: Released swapchain Present vtable hooks for runtime handoff "
            "(present=%d present1=%d vtable=%p restored8=%p restored22=%p reason=%s)",
            restoredPresent ? 1 : 0, restoredPresent1 ? 1 : 0, dxgi_shared_s_hookedVTable,
            restoredPresent ? (void*)dxgi_shared_oPresent : dxgi_shared_s_hookedVTable[8],
            restoredPresent1 ? (void*)dxgi_shared_oPresent1 : dxgi_shared_s_hookedVTable[22], reason ? reason : "unknown");
        dxgi_shared_s_hookedVTable = nullptr;
        dxgi_shared_s_slRoutingActive.store(false, std::memory_order_release);
        dxgi_shared_oPresentBypass = nullptr;
        dxgi_shared_oPresent1Bypass = nullptr;
    }
}
}

namespace DXGIShared {
void RepairVTableHooksIfNeeded() {
    // CRITICAL: Do NOT access the swapchain vtable during Streamline's critical
    // initialization window.  Inside Hooked_slDLSSGGetState (called during
    // sl_common!slGetPluginFunction from SL's DllMain), reading the vtable
    // triggers Steam's overlay hook chain (gameoverlayrenderer64!OverlayHookD3D3)
    // which may still be partially initialized and crash with a null function
    // pointer call (RIP=0, RAX=0).  This guard is state-based (PostSL confirmed
    // rendering) rather than timer-based because SL's background DllMain duration
    // varies and can exceed the startup window timer.
    if (DXGIShared::ShouldDeferVTableRepairDuringStreamlineStartup(
            g_StreamlineFGRunning.load(std::memory_order_acquire), DXGIShared::IsStreamlineStartupHandoffPending(),
            DXGIShared::IsStreamlineStartupTransitionWindowActive(), HookIsPostSLOverlayConfirmedRendering())) {
        return;
    }

    if (!dxgi_shared_s_hookedVTable) {
        static std::atomic<uint32_t> s_nullLogCount{0};
        if (s_nullLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLogImportant("DXGIShared: RepairVTable — s_hookedVTable is NULL, cannot repair");
        }
        return;
    }
    if (!IsReadableMemory(reinterpret_cast<const void*>(dxgi_shared_s_hookedVTable), 23 * sizeof(void*))) {
        HookLogImportant("DXGIShared: RepairVTable — s_hookedVTable %p not readable", dxgi_shared_s_hookedVTable);
        return;
    }

    bool repaired = false;
    DWORD oldProtect;

    // Check Present hook at vtable[8]
    if (dxgi_shared_s_hookedVTable[8] != (void*)DetourPresent) {
        HookLogImportant("DXGIShared: vtable[8] OVERWRITTEN! was=%p expected=%p — re-hooking", dxgi_shared_s_hookedVTable[8],
                         (void*)DetourPresent);
        dxgi_shared_oPresent = (PFN_Present)dxgi_shared_s_hookedVTable[8];
        if (VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            dxgi_shared_s_hookedVTable[8] = (void*)DetourPresent;
            VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), oldProtect, &oldProtect);
            repaired = true;
            HookLogImportant("DXGIShared: vtable[8] re-hooked (new oPresent=%p)", dxgi_shared_oPresent);
        }
    }

    // Check Present1 hook at vtable[22]
    if (dxgi_shared_s_hookedVTable[22] != (void*)DetourPresent1) {
        HookLogImportant("DXGIShared: vtable[22] OVERWRITTEN! was=%p expected=%p — re-hooking", dxgi_shared_s_hookedVTable[22],
                         (void*)DetourPresent1);
        dxgi_shared_oPresent1 = (PFN_Present1)dxgi_shared_s_hookedVTable[22];
        if (VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[22]), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            dxgi_shared_s_hookedVTable[22] = (void*)DetourPresent1;
            VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[22]), sizeof(void*), oldProtect, &oldProtect);
            repaired = true;
            HookLogImportant("DXGIShared: vtable[22] re-hooked (new oPresent1=%p)", dxgi_shared_oPresent1);
        }
    }

    static std::atomic<uint32_t> s_intactLogCount{0};
    if (repaired) {
        s_intactLogCount.store(0, std::memory_order_relaxed);
    } else {
        if (s_intactLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLogImportant("DXGIShared: RepairVTableHooksIfNeeded — hooks intact (vtable=%p, [8]=%p, [22]=%p)",
                             dxgi_shared_s_hookedVTable, dxgi_shared_s_hookedVTable[8], dxgi_shared_s_hookedVTable[22]);
        }
    }
}
}

namespace DXGIShared {
void RemoveSwapchainVTableHooks() {
    InlineHook::RemoveAll();
    oSetColorSpace1Trampoline.store(nullptr, std::memory_order_release);

    dxgi_shared_s_slRoutingActive.store(false, std::memory_order_release);
    dxgi_shared_oPresentBypass = nullptr;
    dxgi_shared_oPresent1Bypass = nullptr;

    if (!dxgi_shared_s_hookedVTable)
        return;

    DWORD oldProtect;

    if (dxgi_shared_oPresent && dxgi_shared_s_hookedVTable[8] == (void*)DetourPresent) {
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), PAGE_READWRITE, &oldProtect);
        dxgi_shared_s_hookedVTable[8] = (void*)dxgi_shared_oPresent;
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed Present vtable hook");
    }

    if (dxgi_shared_oPresent1 && dxgi_shared_s_hookedVTable[22] == (void*)DetourPresent1) {
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[22]), sizeof(void*), PAGE_READWRITE, &oldProtect);
        dxgi_shared_s_hookedVTable[22] = (void*)dxgi_shared_oPresent1;
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[22]), sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed Present1 vtable hook");
    }

    if (oResizeBuffers && dxgi_shared_s_hookedVTable[13] == (void*)DetourResizeBuffers) {
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[13]), sizeof(void*), PAGE_READWRITE, &oldProtect);
        dxgi_shared_s_hookedVTable[13] = (void*)oResizeBuffers;
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[13]), sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed ResizeBuffers vtable hook");
    }

    if (oResizeBuffers1 && dxgi_shared_s_hookedVTable[39] == (void*)DetourResizeBuffers1) {
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[39]), sizeof(void*), PAGE_READWRITE, &oldProtect);
        dxgi_shared_s_hookedVTable[39] = (void*)oResizeBuffers1;
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[39]), sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed ResizeBuffers1 vtable hook");
    }

    dxgi_shared_s_hookedVTable = nullptr;
    HookLog("DXGIShared: All swapchain vtable hooks removed");
}
}
