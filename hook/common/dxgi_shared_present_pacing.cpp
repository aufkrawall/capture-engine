#include "dxgi_shared_internal.h"
#include "present_pacing_policy.h"

// Flip-queue pacing and present-queue latency overrides, split out of
// dxgi_shared.cpp. Both are CE's D3D-side implementation of `backbuffer_count`
// and `cpu_prerender_limit`, and both must run only on the swapchain the
// application actually presents through - see present_pacing_policy.h for why,
// and for the DOOM Eternal deadlock that proved it.

namespace DXGIShared {
namespace {
// Latched once a pacing wait misses its ceiling. Process-wide and lock-free on
// purpose: the hot path pays a single relaxed load, a waitable object that
// stopped signalling is a process-level anomaly rather than a per-swapchain
// one, and a raw swapchain pointer is not a safe identity to latch against
// across releases and address reuse.
std::atomic<bool> s_flipQueuePacingWedged{false};
}  // namespace
}  // namespace DXGIShared

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
// One implementation of the pacing wait for every transport that performs it
// (the shared Present/Present1 path and CWrapDXGISwapChain). Returns true when
// flip-queue room was actually granted.
bool WaitFlipQueuePacingObject(HANDLE waitable, const char* context) {
    if (!waitable || waitable == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (s_flipQueuePacingWedged.load(std::memory_order_relaxed)) {
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(waitable, ce::present_pacing_policy::kFlipQueuePacingWaitCeilingMs);
    if (!ce::present_pacing_policy::ShouldDisablePacingAfterWait(waitResult)) {
        return true;
    }

    const DWORD lastError = (waitResult == WAIT_FAILED) ? GetLastError() : 0;
    if (!s_flipQueuePacingWedged.exchange(true, std::memory_order_acq_rel)) {
        HookLogImportant(
            "WaitFlipQueuePacingObject: flip-queue pacing wait did not complete within %lu ms "
            "(context=%s waitable=%p result=%lu error=%lu) - disabling backbuffer_count pacing for this "
            "process so a waitable object that stopped signalling cannot stall every later present",
            ce::present_pacing_policy::kFlipQueuePacingWaitCeilingMs, context ? context : "present", waitable,
            waitResult, lastError);
    }
    return false;
}
}

namespace DXGIShared {
// Wait for DWM flip queue room when backbuffer_count override is active.
// Uses DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT (applied at
// creation) to pace presents so the effective vsync queue depth matches
// the override count, without changing the physical BufferCount.
void WaitBackbufferFrameLatency(IDXGISwapChain* pSwapChain) {
    const auto& gfx = GetActiveGraphicsConfig();
    const bool overrideActive = HasBackbufferCountOverride(gfx.backbufferCount);
    const bool vulkanOwnsPresentation = IsVulkanActive();
    if (!ce::present_pacing_policy::ShouldWaitForFlipQueueRoom(
            overrideActive, vulkanOwnsPresentation, s_flipQueuePacingWedged.load(std::memory_order_relaxed))) {
        if (!overrideActive) {
            static int s_logCount = 0;
            if (s_logCount++ < 3)
                HookLog("WaitBackbufferFrameLatency: no override (count=%d)", gfx.backbufferCount);
        } else if (vulkanOwnsPresentation) {
            // The CE Vulkan layer owns presentation, so this DXGI swapchain is
            // the Vulkan runtime's WSI transport and its presenter thread is
            // joined inside vkDestroySwapchainKHR. Blocking it there wedges the
            // game (DOOM Eternal session 20260819_020933).
            static std::atomic<int> s_vulkanSkipLogCount{0};
            const int skipNum = s_vulkanSkipLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (skipNum <= 3 || (skipNum % 5000) == 0)
                HookLogImportant(
                    "WaitBackbufferFrameLatency: skipping flip-queue pacing #%d for sc=%p - the CE Vulkan "
                    "layer owns presentation, so this swapchain is the Vulkan runtime's transport and the "
                    "layer already applies the configured queue depth on the real swapchain",
                    skipNum, (void*)pSwapChain);
        }
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

    if (WaitFlipQueuePacingObject(hWaitable, "WaitBackbufferFrameLatency")) {
        static int s_logCount = 0;
        if (s_logCount++ < 3)
            HookLog("WaitBackbufferFrameLatency: wait succeeded");
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

    if (!ce::present_pacing_policy::ShouldApplyCePresentationPolicy(IsVulkanActive())) {
        // Same ownership rule as the pacing wait: reprogramming the Vulkan
        // runtime's transport swapchain is what shrinks its flip queue to the
        // depth that made the pacing wait blockable in the first place.
        static std::atomic<int> s_vulkanSkipLogCount{0};
        const int skipNum = s_vulkanSkipLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (skipNum <= 3 || (skipNum % 5000) == 0)
            HookLogImportant(
                "ApplyPresentFrameLatencyOverrides: skipping #%d for sc=%p - the CE Vulkan layer owns "
                "presentation and applies the configured prerender depth on the real Vulkan swapchain",
                skipNum, (void*)pSwapChain);
        return;
    }

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
