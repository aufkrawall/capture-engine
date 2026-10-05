#include "dx12_hook_internal.h"
#include "hook/runtime/hook_clock.h"
#include "dx12_hook_process_session.h"

ProcessFrameFlow FrameProcessSession::SelectAllocatorSlot() {
    allocatorPoolSize = static_cast<int>(dx12_hook_g_State.allocators.size());
    if (allocatorPoolSize <= 0) {
        return ProcessFrameFlow::kOverlayDone;
    }

    idx = dx12_hook_g_State.allocIndex % allocatorPoolSize;
    dx12_hook_g_State.allocIndex = (idx + 1) % allocatorPoolSize;

    // With 16 allocators, we never need to wait under normal conditions.
    // However, during Alt+Tab / GPU throttle, the GPU may stall and the
    // fence value for this allocator slot won't advance.  We must check
    // before Reset() to avoid undefined behaviour (driver hang / crash).
    list = dx12_hook_g_State.cmdList;
    alloc = (idx < (int)dx12_hook_g_State.allocators.size()) ? dx12_hook_g_State.allocators[idx] : nullptr;
    return ProcessFrameFlow::kContinue;
}

ProcessFrameFlow FrameProcessSession::ResetAllocatorForDraw() {
    if (dx12_hook_g_State.fence && idx < (int)dx12_hook_g_State.fenceValues.size() && dx12_hook_g_State.fenceValues[idx] > 0) {
        UINT64 completed = dx12_hook_g_State.fence->GetCompletedValue();
        if (completed < dx12_hook_g_State.fenceValues[idx]) {
            if (activeDebugSample) {
                activeDebugSample->flags |= kPresentSampleFlagAllocatorBusy;
            }
            static std::atomic<int> s_allocSkipLogs{0};
            if (s_allocSkipLogs.fetch_add(1, std::memory_order_relaxed) < 30) {
                HookLog(
                    "DX12: Allocator[%d] still in-flight (completed=%llu, needed=%llu), "
                    "skipping overlay this frame",
                    idx, completed, dx12_hook_g_State.fenceValues[idx]);
            }
            return ProcessFrameFlow::kOverlayDone;
        }
    }
    allocResetHr = alloc->Reset();
    return ProcessFrameFlow::kContinue;
}

ProcessFrameFlow FrameProcessSession::ResetCommandListForDraw() {
    listResetHr = list->Reset(alloc, nullptr);
    // Log Reset results during FG for diagnostics
    if (g_FGCompat.IsFGActive() || slFGActive) {
        static std::atomic<int> s_fgResetLogs{0};
        int fgResetLog = s_fgResetLogs.fetch_add(1, std::memory_order_relaxed);
        if (fgResetLog < 5) {
            HookLogImportant(
                "DX12: FG overlay alloc/list Reset (allocHr=0x%08X listHr=0x%08X idx=%d)",
                (unsigned)allocResetHr, (unsigned)listResetHr, idx);
        }
    }
    return ProcessFrameFlow::kContinue;
}

ProcessFrameFlow FrameProcessSession::PrepareDrawResources() {
    BeginOverlayGpuBreadcrumbFrame(g_Device.load(std::memory_order_acquire));
    WriteOverlayGpuBreadcrumb(list, kOverlayBcStart);
    preserveLiveStartupOverlayDuringInactiveSL =
        ShouldPreserveLiveStartupOverlayDuringRuntimeInactiveStreamlineHandoff();
    hasPendingStartupOverlayResources = g_OverlayAdapter.HasPendingDX12Resources();
    shouldPrimeStartupOverlayResources =
        dx12_hook_s_startupOverlayResourcePrimeMs == 0 &&
        ce::dx12_overlay_policy::ShouldPrimeStartupOverlayResources(
            startupOverlayCompatibilityActive, hasPendingStartupOverlayResources,
            preserveLiveStartupOverlayDuringInactiveSL);
    if (startupOverlayCompatibilityActive && hasPendingStartupOverlayResources &&
        preserveLiveStartupOverlayDuringInactiveSL) {
        static std::atomic<int> s_skipStartupPrimeForLiveOverlayLogCount{0};
        const int skipPrimeLog =
            s_skipStartupPrimeForLiveOverlayLogCount.fetch_add(1, std::memory_order_relaxed);
        if (skipPrimeLog < 5 || (skipPrimeLog % 300) == 0) {
            HookLogImportant(
                "DX12: Skipping startup resource priming delay because live overlay is "
                "preserved through runtime-inactive Streamline handoff (log=%d)",
                skipPrimeLog + 1);
        }
    }
    if (shouldPrimeStartupOverlayResources) {
        // Check device before priming — after FG teardown the
        // device may already be removed (async GPU fault).
        {
            auto* primeDev = g_Device.load(std::memory_order_acquire);
            HRESULT primeDevHr = primeDev ? primeDev->GetDeviceRemovedReason() : E_FAIL;
            if (FAILED(primeDevHr)) {
                HookLogImportant("DX12: SKIPPING resource priming — device removed 0x%08X",
                                 (unsigned)primeDevHr);
                dx12_hook_g_DeviceRemoved.store(true, std::memory_order_release);
                return ProcessFrameFlow::kOverlayDone;
            }
        }
        HookLogImportant("DX12: Priming DX12 overlay resources before first GTA overlay draw");
        if (!g_OverlayAdapter.PrimeDX12Resources(list)) {
            HookLogImportant(
                "DX12: DX12 overlay resource priming failed; deferring first overlay draw");
            return ProcessFrameFlow::kOverlayDone;
        }

        HRESULT primeCloseResult = list->Close();
        if (FAILED(primeCloseResult)) {
            HookLog("DX12: Priming command list close failed hr=0x%08X, forcing reinit",
                    primeCloseResult);
            dx12_hook_g_State.syncInit = false;
            return ProcessFrameFlow::kOverlayDone;
        }

        // The priming list only uploads device-scoped resources
        // (font texture); it never touches the swapchain backbuffer,
        // so it is eligible for the dedicated overlay queue.
        if (!SubmitOverlayCommandList(gameQueue, list, idx, "startup resource priming",
                                      false, /*listTouchesBackbuffer=*/false)) {
            HookLogImportant(
                "DX12: Startup resource priming submission failed; deferring first overlay "
                "draw");
            return ProcessFrameFlow::kOverlayDone;
        }

        // Check device after priming submit — catch async GPU fault immediately
        {
            auto* postPrimeDev = g_Device.load(std::memory_order_acquire);
            HRESULT postPrimeDevHr =
                postPrimeDev ? postPrimeDev->GetDeviceRemovedReason() : E_FAIL;
            if (FAILED(postPrimeDevHr)) {
                HookLogImportant("DX12: Resource priming CAUSED device removal 0x%08X!",
                                 (unsigned)postPrimeDevHr);
                dx12_hook_g_DeviceRemoved.store(true, std::memory_order_release);
                return ProcessFrameFlow::kOverlayDone;
            }
        }

        dx12_hook_s_startupOverlayResourcePrimeMs = ce::hook_clock::TickCount64();
        HookLogImportant(
            "DX12: DX12 overlay resource priming submitted, delaying first overlay draw for "
            "%llums",
            dx12_hook_kStartupOverlayPostResourcePrimeSettleMs);
        return ProcessFrameFlow::kOverlayDone;
    }

    if (shouldRunStartupOverlayDrawProbe &&
        dx12_hook_s_startupOverlayFirstDrawProbeStage == StartupOverlayFirstDrawProbeStage::kNone) {
        // Probe system removed: go straight to rendering.
        // The 3-stage probe (backbuffer touch → pipeline state → real draw) caused
        // ERR_GFX_STATE in GTA5 Enhanced because even barrier-only probes on a
        // dedicated overlay queue conflict with the game's D3D12 state tracking.
        // With single-queue mode (fix for dedicated queue), we can render directly.
        dx12_hook_s_startupOverlayFirstDrawProbeStage = StartupOverlayFirstDrawProbeStage::kActualRender;
    }

    sc3 = dx12_hook_g_State.cachedSC3;
    if (!sc3) {
        if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
            sc3->Release();           // drop QI ref — weak cache is safe
            dx12_hook_g_State.cachedSC3 = sc3;  // because swapchain is alive during Present
        }
    }
    QueryPerformanceFrequency(&perfFreq);
    ce::hook_clock::QueryCounter(&perfQI);
    return ProcessFrameFlow::kContinue;
}

ce::dx12::BackBufferAcquisition<ID3D12Resource> FrameProcessSession::AcquireDrawBackBuffer() {
    swapchainBufferIdx = sc3->GetCurrentBackBufferIndex();
    currentBackBufferIdx = swapchainBufferIdx;
    hasCurrentBackBufferIdx = true;
    // CRITICAL FIX: Use actual swapchain buffer index directly
    // CreateRTVs now creates RTVs for all swapchain buffers (up to 8)
    // so no need to wrap the index - this prevents sync issues
    bufferIdx = swapchainBufferIdx;
    // Validate buffer index is within our allocated range
    if (bufferIdx >= (UINT)dx12_hook_g_State.bufferCount) {
        HookLog(
            "DX12: Buffer index %u exceeds allocated count %d, "
            "clamping",
            bufferIdx, dx12_hook_g_State.bufferCount);
        bufferIdx = dx12_hook_g_State.bufferCount - 1;
    }
    ce::hook_clock::QueryCounter(&perfGetBuf);
    ID3D12Resource* buffer = nullptr;
    const HRESULT result = sc3->GetBuffer(swapchainBufferIdx, IID_PPV_ARGS(&buffer));
    return {result, buffer};
}

void FrameProcessSession::RefreshDrawRenderTarget(ID3D12Resource* buffer) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = dx12_hook_g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)bufferIdx * dx12_hook_g_State.rtvDescriptorSize;
    g_Device.load()->CreateRenderTargetView(buffer, nullptr, rtv);
}

HRESULT FrameProcessSession::CloseDrawCommands() {
    closeHr = list->Close();
    // Log Close result during FG
    if (g_FGCompat.IsFGActive() || slFGActive) {
        static std::atomic<int> s_fgCloseLogs{0};
        if (s_fgCloseLogs.fetch_add(1, std::memory_order_relaxed) < 5) {
            HookLogImportant("DX12: FG overlay list->Close hr=0x%08X",
                             (unsigned)closeHr);
        }
    }
    // Always log Close result for first N reinit frames
    {
        static int s_reinitCloseLogCount = 0;
        if (dx12_hook_g_ResetReinitSubmitCounter.load(std::memory_order_relaxed))
            s_reinitCloseLogCount = 0;
        if (s_reinitCloseLogCount < 5) {
            s_reinitCloseLogCount++;
            auto* closeDev = g_Device.load(std::memory_order_acquire);
            HRESULT closeDevHr = closeDev ? closeDev->GetDeviceRemovedReason() : E_FAIL;
            HookLogImportant(
                "DX12: Reinit Close #%d hr=0x%08X devRemoved=0x%08X primaryOverlay=%d",
                s_reinitCloseLogCount, (unsigned)closeHr, (unsigned)closeDevHr,
                usedPrimaryOverlayBackend ? 1 : 0);
        }
    }
    return closeHr;
}
