#include "dxgi_shared_internal.h"
#include "present_pacing_policy.h"
#include "swapchain_flag_apply.h"
#include "vulkan_dxgi_fifo_policy.h"

#include <cstring>

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
    const auto& cfg = GetActiveGraphicsConfigCached();

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
    const auto& gfx = GetActiveGraphicsConfigCached();
    const bool overrideActive = HasBackbufferCountOverride(gfx.backbufferCount);
    const bool vulkanOwnsPresentation = IsVulkanActive();
    const bool pacingLatchedOff = s_flipQueuePacingWedged.load(std::memory_order_relaxed);
    // Ownership is read only once every cheaper reason to skip is gone.
    const bool pacingOtherwiseDue = ce::present_pacing_policy::ShouldWaitForFlipQueueRoom(
        overrideActive, vulkanOwnsPresentation, pacingLatchedOff, /*ceAddedWaitableObject=*/true);
    const bool ceAddedWaitable =
        pacingOtherwiseDue && ce::swapchain_flag_policy::DidCeAddFrameLatencyWaitable(pSwapChain);
    if (!ce::present_pacing_policy::ShouldWaitForFlipQueueRoom(overrideActive, vulkanOwnsPresentation,
                                                               pacingLatchedOff, ceAddedWaitable)) {
        if (pacingOtherwiseDue) {
            // Either the creator owns the waitable and waits on it itself, or the chain has none
            // because CE withheld it (no ResizeBuffers reconciliation). Classified only when logged.
            static std::atomic<int> s_foreignWaitableLogCount{0};
            const int skipNum = s_foreignWaitableLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (skipNum <= 3 || (skipNum % 5000) == 0) {
                auto owner = ce::swapchain_flag_policy::FrameLatencyWaitableOwner::Creator;
                UINT flags = 0;
                DXGI_SWAP_CHAIN_DESC desc{};
                const bool classified = ce::swapchain_flag_policy::TryClassifySwapChainFrameLatencyWaitable(
                    pSwapChain, &owner, &flags, &desc);
                if (classified && owner == ce::swapchain_flag_policy::FrameLatencyWaitableOwner::None &&
                    ce::swapchain_flag_policy::ClassifyBackbufferCountWithoutWaitable(
                        desc.SwapEffect, desc.BufferCount, gfx.backbufferCount) ==
                        ce::swapchain_flag_policy::WaitablelessBackbufferOutcome::CarriedByBufferCount) {
                    HookLogImportant(
                        "WaitBackbufferFrameLatency: no flip-queue pacing needed #%d for sc=%p - backbuffer_count=%d "
                        "is carried by the physical BufferCount=%u (swapEffect=%d flags=0x%X, no frame-latency "
                        "waitable)",
                        skipNum, (void*)pSwapChain, gfx.backbufferCount, desc.BufferCount,
                        static_cast<int>(desc.SwapEffect), flags);
                } else if (classified && owner == ce::swapchain_flag_policy::FrameLatencyWaitableOwner::None) {
                    HookLogImportant(
                        "WaitBackbufferFrameLatency: skipping flip-queue pacing #%d for sc=%p - the flip chain keeps "
                        "BufferCount=%u above backbuffer_count=%d and has no frame-latency waitable (flags=0x%X): the "
                        "application did not request one and CE could not add it (resizeReconciliation=%d), so the "
                        "configured depth cannot be enforced",
                        skipNum, (void*)pSwapChain, desc.BufferCount, gfx.backbufferCount, flags,
                        ReconcilesApplicationResizeFlags() ? 1 : 0);
                } else {
                    HookLogImportant(
                        "WaitBackbufferFrameLatency: skipping flip-queue pacing #%d for sc=%p - its frame-latency "
                        "waitable was requested by the application or FG runtime that created it, which waits on "
                        "it itself; a second wait by CE would take the count its next frame needs (flags=0x%X%s)",
                        skipNum, (void*)pSwapChain, flags, classified ? "" : ", descriptor unavailable");
                }
            }
        } else if (!overrideActive) {
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

    if (!detail::PresentPacingScope::ClaimWait(pSwapChain)) return;
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

    // DXGI accepts a swapchain-level maximum frame latency only on a chain created with the
    // frame-latency waitable; on any other chain the call is a guaranteed DXGI_ERROR_INVALID_CALL.
    auto owner = ce::swapchain_flag_policy::FrameLatencyWaitableOwner::Creator;
    UINT creationFlags = 0;
    if (ce::swapchain_flag_policy::TryClassifySwapChainFrameLatencyWaitable(pSwapChain, &owner, &creationFlags) &&
        !ce::swapchain_flag_policy::CanSetSwapChainMaximumFrameLatency(owner)) {
        const bool prerenderLimiterApplies = source && std::strcmp(source, "cpu_prerender_limit") == 0;
        HookLogImportant(
            "ApplyPresentFrameLatencyOverrides: not setting the swapchain maximum frame latency %u (%s) on sc=%p - "
            "it was created without a frame-latency waitable (flags=0x%X, resizeReconciliation=%d); %s",
            requested, source ? source : "override", (void*)pSwapChain, creationFlags,
            ReconcilesApplicationResizeFlags() ? 1 : 0,
            prerenderLimiterApplies ? "CE's per-API prerender limiter enforces the limit instead"
                                    : "this setting has no other enforcement on this swapchain");
        s_lastSwapchain = scKey;
        s_lastRequested = requested;
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
void ProcessPresentVSyncOverride(UINT& syncInterval, UINT& flags, IDXGISwapChain* pSwapChain) {
    if (pSwapChain && DX12_IsPresentInterposerPrivateSwapchain(pSwapChain)) {
        // A present interposer's own output present (NVIDIA Smooth Motion) is the FINAL physical
        // flip, and that is the one place a vertical-blank contract can still be stated for a
        // metered frame generator. This is the Portal RTX result of 2026-09-13, restated for DXGI:
        // what unpaced the generated group there was forcing the present MODE above the generator,
        // not the interval on its flip. CE changes nothing above NvPresent64 - the pair arrives
        // here already spread by the driver's own metering (it presents SyncInterval=0 with
        // ALLOW_TEARING, which IS that scheduling) - so quantizing an already-correctly-spread
        // group onto vertical blanks is vertical-blank synchronization, not judder.
        //
        // Only a vblank-paced request applies. `off` and `mailbox` are not a vertical-blank
        // contract, and the interposer's own parameters already are those.
        const VSyncOverride interposerOverride = GetVSyncOverride();
        const bool vblankPacedRequested =
            interposerOverride.shouldOverride && !interposerOverride.useMailbox &&
            interposerOverride.presentInterval >= 1;
        uint32_t rewrittenInterval = syncInterval;
        uint32_t rewrittenFlags = flags;
        const bool rewritten = ce::vulkan_dxgi_fifo_policy::ApplyFinalDxgiFifoParameters(
            vblankPacedRequested, rewrittenInterval, rewrittenFlags);
        if (rewritten) {
            static std::atomic<int> s_interposerFifoLogCount{0};
            const int logCount = s_interposerFifoLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 5 || (logCount % 4096) == 0) {
                HookLogImportant(
                    "DXGI: stating the vertical blank on the present interposer's own flip (sc=%p sync=%u->%u "
                    "flags=0x%X->0x%X) #%d — nothing above the generator is touched",
                    pSwapChain, syncInterval, rewrittenInterval, flags, rewrittenFlags, logCount + 1);
            }
            syncInterval = rewrittenInterval;
            flags = rewrittenFlags;
        }
        return;
    }
    if (ce::present_pacing_policy::ShouldPreserveNativeFGOutputVSync(
            HookHasRuntimeOwnedNativeFGPresentPath(), DX12_IsFFXProxyPresentHookInstalled(),
            g_StreamlineFGRunning.load(std::memory_order_acquire))) {
        // Do not re-read user intent here: these frames may already have been
        // scheduled before a config change. AMD owns their output parameters.
        static thread_local bool logged = false;
        if (!logged) {
            logged = true;
            HookLogImportant("DXGI: preserving FFX output VSync (sync=%u flags=0x%X); "
                             "user intent applied at FFX proxy input", syncInterval, flags);
        }
        return;
    }
    ProcessVSyncOverride(syncInterval, flags);
}
}
