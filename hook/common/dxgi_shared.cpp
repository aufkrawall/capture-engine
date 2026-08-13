#include "dxgi_shared_internal.h"

// Put shutdown check outside the DXGIShared namespace
bool IsShuttingDown() {
    return HookIsShuttingDown();
}

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
std::mutex s_thirdPartyOverlaySwapchainMutex;
}

namespace DXGIShared {
std::unordered_set<IDXGISwapChain*> s_thirdPartyOverlaySwapchains;
}

namespace DXGIShared {
std::unordered_set<IDXGISwapChain*> s_startupBlockingOverlayTaggedSwapchains;
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
// Global metrics for DXGI-based APIs
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
PerformanceMetrics dxgi_shared_g_DXGIPerfMetrics;
}

namespace DXGIShared {
std::atomic<uint32_t> g_LatestSourceFrameIndex{0};
}

namespace DXGIShared {
// Recursion detection globals (avoiding thread_local which requires runtime
// init)
std::atomic<DWORD> dxgi_shared_g_presentThreadId{0};
}

namespace DXGIShared {
std::atomic<int> dxgi_shared_g_presentDepth{0};
}

namespace DXGIShared {
std::atomic<DWORD> dxgi_shared_g_resizeThreadId{0};
}

namespace DXGIShared {
std::atomic<int> dxgi_shared_g_resizeDepth{0};
}

namespace DXGIShared {
// Present-scoped duplicate suppression for the explicit-OFF exact-proxy route.
// This is thread-local because a different Streamline worker Present represents
// a distinct output frame and must retain its own PostSL draw.
thread_local uint32_t s_postSLOffKeepAlivePresentScopeDepth = 0;
}

namespace DXGIShared {
thread_local bool s_postSLOffKeepAlivePrePresentDrawn = false;
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
// Helper to check if we're recursively entering from the same thread
bool IsRecursivePresent() {
    DWORD currentId = GetCurrentThreadId();
    DWORD expected = dxgi_shared_g_presentThreadId.load(std::memory_order_acquire);

    // Fast path: same thread re-entering (recursion)
    if (expected == currentId && dxgi_shared_g_presentDepth.load(std::memory_order_acquire) > 0) {
        return true;
    }

    // Try to claim ownership atomically — only one thread can succeed
    DWORD zero = 0;
    if (dxgi_shared_g_presentThreadId.compare_exchange_strong(zero, currentId, std::memory_order_acq_rel)) {
        dxgi_shared_g_presentDepth.store(1, std::memory_order_release);
        return false;
    }

    // Another thread owns it — if it's us (race between check and CAS), re-check
    if (dxgi_shared_g_presentThreadId.load(std::memory_order_acquire) == currentId) {
        dxgi_shared_g_presentDepth.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }

    // Different thread owns it — during Streamline DLSS FG, SL calls Present
    // from worker threads for generated frames while the game thread is still
    // inside oPresent (SL's wrapper).  These cross-thread Present calls MUST
    // be treated as re-entrant so that:
    //   1. PostSL overlay callback fires (correct timing: after SL's GPU work)
    //   2. Pre-SL overlay rendering is skipped (would conflict with SL's GPU work)
    //   3. oPresentBypass is used (avoids re-entering SL's hook chain)
    // Without this, the cross-thread call runs the full non-re-entrant path
    // including DX12ProcessFrame, causing DEVICE_HUNG from concurrent backbuffer
    // access between our overlay and SL's FG pipeline.
    if (expected != 0 && g_StreamlineFGRunning.load(std::memory_order_acquire)) {
        return true;
    }

    // Different thread owns it — treat as non-recursive, let it proceed
    // (this matches original behavior: each thread processes Present
    // independently)
    return false;
}
}

namespace DXGIShared {
void ReleasePresent() {
    if (dxgi_shared_g_presentDepth.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        dxgi_shared_g_presentThreadId.store(0, std::memory_order_release);
    }
}
}

namespace DXGIShared {
thread_local unsigned dxgi_shared_s_wrapperSetColorSpaceForwardDepth = 0;
}

namespace DXGIShared {
// Bypass trampolines — skip external E9/FF25 hooks (e.g. Streamline) at the
// function entry point by executing original prologue bytes read from disk.
// Used in re-entrant Present calls to actually present the frame without
// re-entering the external hook chain.
PFN_Present dxgi_shared_oPresentBypass = nullptr;
}

namespace DXGIShared {
PFN_Present1 dxgi_shared_oPresent1Bypass = nullptr;
}

namespace DXGIShared {
// Saved target of the external E9 JMP on dxgi!Present, installed by Steam overlay
// (gameoverlayrenderer64!OverlayHookD3D3).  Captured during InstallPresentInlineHooks
// BEFORE Streamline overwrites it with its own JMP.  CE may invoke this target
// from SL-originated Present stacks only while the Streamline plugin-lookup guard
// is active; otherwise those paths use the bypass trampoline.
PFN_Present dxgi_shared_g_externalOverlayPresentHook = nullptr;
}

namespace DXGIShared {
// Set when InstallPresentInlineHooks deliberately left the dxgi!Present entry to a
// multi-overlay foreign chain (see ShouldLeavePresentEntryToForeignOverlayChain). CE then
// owns no entry bytes, so every forward must run the live entry instead of a trampoline or
// a bypass; CE's own normal interception happens in the deep function body.
std::atomic<bool> dxgi_shared_s_presentEntryLeftToForeignChain{false};
}

namespace DXGIShared {
bool IsPresentEntryLeftToForeignChain() {
    return dxgi_shared_s_presentEntryLeftToForeignChain.load(std::memory_order_acquire);
}
}

namespace DXGIShared {
// Deep-body trampolines for the left-to-foreign-chain mode (see present_state_globals.h).
PFN_Present dxgi_shared_oPresentDeepBody = nullptr;
PFN_Present1 dxgi_shared_oPresent1DeepBody = nullptr;

bool IsPresentInterceptedBelowForeignChain() {
    return dxgi_shared_oPresentDeepBody != nullptr || dxgi_shared_oPresent1DeepBody != nullptr;
}

bool ArePresentMethodsInterceptedBelowForeignChain() {
    return dxgi_shared_oPresentDeepBody != nullptr && dxgi_shared_oPresent1DeepBody != nullptr;
}
}

namespace DXGIShared {
// Function-entry addresses CE prepended over at InstallPresentInlineHooks time (the real
// dxgi!Present / dxgi!Present1 entries, not the foreign E9 targets). Consumed only by the
// ownership-checked un-prepend when a wrapped FG runtime swapchain establishes a non-entry
// view of runtime presents (see MaybeTransitionPresentEntryToForeignChainForWrappedRuntimeSwapchain).
void* dxgi_shared_s_presentEntryAddress = nullptr;
void* dxgi_shared_s_present1EntryAddress = nullptr;
}

namespace DXGIShared {
thread_local int dxgi_shared_s_externalOverlayPresentInvokeDepth = 0;
}

namespace DXGIShared {
// Stored vtable pointer for unhooking Present when COM wrapper takes over
void** dxgi_shared_s_hookedVTable = nullptr;
}

namespace DXGIShared {
// Saved original vtable[8] Present COM method captured from the temp swapchain
// at InstallPresentInlineHooks time, before any vtable modifications.  This is
// the real IDXGISwapChain::Present COM method (dxgi!CDXGISwapChain::Present or
// equivalent), not the inner dxgi!Present function that Steam hooks with an E9
// JMP.  Used in the E9 JMP path of CallOriginalPresent and
// AttemptSteamDX12OverlayInit to ensure DXGI COM method state management runs
// before dxgi!Present is called with Steam's E9 JMP.  Without this, calling
// dxgi!Present directly skips COM state management, which causes black screen
// on some DX12 games (e.g. Strange Brigade).
PFN_Present dxgi_shared_s_originalVtable8Present = nullptr;
}

namespace DXGIShared {
// State for one-time Steam DX12 overlay initialization.
// Steam's OverlayHookD3D3 lazily initializes its internal "next" Present handler
// on first E9 JMP entry by reading vtable[8].  When vtable[8] = DetourPresent
// (our vtable hook), Steam's init fails and sets "next" = NULL, causing RIP=0.
//
// Fix: temporarily restore vtable[8] to the original dxgi!Present on the very
// first non-SL Steam overlay Present call, allowing Steam's init to complete.
// Re-hook vtable[8] to DetourPresent after Steam returns.
std::atomic<bool> dxgi_shared_s_steamDX12InitAttempted{false};
}

namespace DXGIShared {
bool dxgi_shared_s_steamInitCrashed = false;
}

namespace DXGIShared {
thread_local SteamNullCallbackRecoveryContext dxgi_shared_s_steamNullCallbackRecoveryContext;
}

namespace DXGIShared {
bool IsWrappedSwapChainObject(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) {
        return false;
    }

    void* pWrapper = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, &pWrapper))) {
        ((IUnknown*)pWrapper)->Release();
        return true;
    }

    return false;
}
}

namespace DXGIShared {
// Vulkan detection via ICD layer - returns false since we use layer approach
bool IsVulkanPrimary() {
    // VK_LAYER_CE_overlay handles Vulkan separately
    return false;
}
}

namespace DXGIShared {
UINT ResolvePresentFrameLatencyOverride(const char** sourceOut) {
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
void* GetPresentAddress(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return nullptr;

    void** vtable = *(void***)pSwapChain;
    return vtable[8];  // Present is at vtable slot 8
}
}

namespace DXGIShared {
// Helper to get Present1 function address from a swapchain's vtable
void* GetPresent1Address(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return nullptr;
    void** vtable = *(void***)pSwapChain;
    return vtable[22];  // Present1 is at vtable slot 22
}
}

namespace DXGIShared {
// Single source of truth for the DXGI present/resize pass-through decision.
// Maintained by CheckAndInstallHooks, which decides from D3D usage evidence
// (a DX12 UE5 process that merely loads vulkan-1.dll stays on the DXGI path).
// Read on every Present/Present1/ResizeBuffers call.
std::atomic<bool> s_dxgiVulkanActive{false};

void SetVulkanActiveForDXGIPresentPath(bool active) {
    const bool previous = s_dxgiVulkanActive.exchange(active, std::memory_order_acq_rel);
    if (active && !previous) {
        static std::atomic<int> s_vulkanActiveLogCount{0};
        if (s_vulkanActiveLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            HookLog("DXGIShared: Vulkan active (evidence-based), DXGI hooks will pass through");
        }
    } else if (!active && previous) {
        static std::atomic<int> s_vulkanInactiveLogCount{0};
        if (s_vulkanInactiveLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            HookLog("DXGIShared: Vulkan no longer active (D3D evidence), DXGI hooks will process presents");
        }
    }
}

bool IsVulkanActive() {
    return s_dxgiVulkanActive.load(std::memory_order_acquire);
}
}

namespace DXGIShared {
bool IsThirdPartyOverlayLoaded() {
    return ce::overlay_compat::IsThirdPartyOverlayLoaded();
}
}

namespace DXGIShared {
// Unified Detours
// For DX12 wrapped swapchains: CWrapDXGISwapChain handles Present, so when
// wrapper calls m_pReal->Present() and it re-enters here, we just passthrough.
// For DX12 pre-existing swapchains (not wrapped): full processing here.
// For DX11: full processing here.
bool IsReadableMemory(const void* ptr, size_t size) {
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
}

namespace DXGIShared {
// Streamline FG routing state.
//
// Problem: When SL hooks Present with an E9 JMP at the function entry, our
// inline hook trampoline (oPresentTrampoline) bypasses SL's hook entirely,
// because the trampoline contains the ORIGINAL function bytes (from before
// any hooks).  With SL bypassed, Frame Generation never runs.
//
// Solution: Detect SL's E9 JMP on the Present function and route through it
// instead of through the trampoline.  This way:
//   Game → vtable[8] (DetourPresent) → overlay render →
//   oPresent (has SL E9 JMP) → SL_Detour → SL trampoline (has our FF 25) →
//   DetourPresent (re-entrant, forwarded to oPresentTrampoline) →
//   real Present → SL post-Present FG → return
//
// The vtable already points to DetourPresent (from inline hook install).
// We just need to change the FINAL call from oPresentTrampoline to oPresent.
std::atomic<bool> dxgi_shared_s_slRoutingActive{false};
}

namespace DXGIShared {
bool IsSLInterposerLoaded() {
    // Latch once SL is seen (SL routing decisions assume SL stays present for the session).
    // The presence check is LOADER-FREE (cached loaded-set maintained by the seed + DLL
    // load/unload notifications); the old per-call GetModuleHandleA("sl.interposer.dll") was a
    // per-Present loader walk that stalled the present thread during the Alt+Tab mode-switch
    // DLL churn. SL loads via LoadLibrary/LdrLoadDll, which feed the cache, so detection timing
    // is equivalent.
    static std::atomic<bool> detected{false};
    if (detected.load(std::memory_order_acquire))
        return true;
    if (ce::overlay_compat::IsStreamlineInterposerModuleLoaded()) {
        detected.store(true, std::memory_order_release);
        return true;
    }
    return false;
}
}

namespace DXGIShared {
bool ShouldKeepSLPresentRoutingDisabledNow(ce::fg_runtime::RuntimeMode* runtimeModeOut ,
                                                  bool* runtimeOwnedNativeFGPresentPathOut ) {
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    const bool runtimeOwnedNativeFGPresentPath = HookHasRuntimeOwnedNativeFGPresentPath();
    if (runtimeModeOut) {
        *runtimeModeOut = runtimeMode;
    }
    if (runtimeOwnedNativeFGPresentPathOut) {
        *runtimeOwnedNativeFGPresentPathOut = runtimeOwnedNativeFGPresentPath;
    }
    return DXGIShared::ShouldKeepSLPresentRoutingDisabledForRuntimeState(runtimeMode, runtimeOwnedNativeFGPresentPath);
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
// Lazy hook installation - installs hooks on first Present if they were
// deferred during swapchain creation
IDXGISwapChain* dxgi_shared_s_PendingSwapChainForLazyHook = nullptr;
}

namespace DXGIShared {
std::atomic<bool> dxgi_shared_s_LazyHooksInstalled{false};
}
