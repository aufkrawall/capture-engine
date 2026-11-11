#include "dx12_hook_internal.h"
#include "hook/runtime/hook_clock.h"
#include "dx12_hook_process_session.h"

ProcessFrameFlow FrameProcessSession::SelectAllocatorSlot() {
    allocatorPoolSize = static_cast<int>(dx12_hook_g_State.allocators.size());
    if (allocatorPoolSize <= 0) {
        return ProcessFrameFlow::kOverlayDone;
    }

    const int nextInRing = dx12_hook_g_State.allocIndex % allocatorPoolSize;
    const bool runtimeOwnedPresentation = dx12_hook_g_FGRuntimeOwnsSwapchain || HookHasRuntimeOwnedNativeFGPresentPath() ||
                                          DXGIShared::DoesFGRuntimeOwnSwapchain();
    const bool reuseFinishedSlot = ce::dx12_overlay_policy::ShouldReuseFinishedOverlaySlot(
        dx12_hook_g_State.fence != nullptr, slFGActive || g_FGCompat.IsFGActive(), runtimeOwnedPresentation,
        dx12_hook_g_State.gpuCompletionUnprovable);
    // Without a fence nothing is proved, and the first slot from the ring on that has none to prove is still right.
    const UINT64 completed = dx12_hook_g_State.fence ? dx12_hook_g_State.fence->GetCompletedValue() : 0;
    const int finished = dx12_hook_g_State.slotPicker.Pick(dx12_hook_g_State.fenceValues.data(), allocatorPoolSize,
                                                           reuseFinishedSlot ? 0 : nextInRing, completed);
    idx = finished >= 0 ? finished : nextInRing;
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
    listRecording = SUCCEEDED(listResetHr);
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
    // The selected renderer prepares its resources while recording the draw (DX12Backend::Render uploads its
    // font before sampling it; the descriptor-free backend needs no texture upload). Preparing the other adapter
    // in a separate submission and waiting 100 ms added a blank startup window without proving GPU completion.

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
