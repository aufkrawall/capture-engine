#include "shared_capture_d3d12_internal.h"
#include "common/logging/log_meter.h"

bool SharedCaptureD3D12::CaptureFrame(ID3D12CommandQueue* pCommandQueue, IDXGISwapChain* pSwapChain,
                                     UINT backBufferIndex, int64_t timestampQpc,
                                     SharedCaptureExecuteCommandListsPtr executeCommandLists,
                                     CaptureCompletionSignal completionSignal) {
    std::unique_lock<std::recursive_mutex> stateLock(m_StateLock, std::try_to_lock);
    if (!stateLock.owns_lock())
        return false;
    // The previous copy's Signal goes on its queue before this copy is recorded:
    // one held at a time, and a slot is never waited on behind an unqueued Signal.
    if (!ReleaseHeldCompletionSignal("next copy", false))
        return false;
    ReapRetiredGenerations();
    if (m_InlineCompletion && FAILED(m_InlineCompletion->Refresh(m_Fence.Get())))
        return false;
    if (!m_Active.load(std::memory_order_acquire) || !CaptureManager::Get().IsCaptureEnabled() || !pCommandQueue ||
        !pSwapChain) {
        return false;
    }
    if (!m_SwapChainBinding.Matches(pSwapChain)) {
        static std::atomic<int> s_foreignSwapChainLog{0};
        if (s_foreignSwapChainLog.fetch_add(1, std::memory_order_relaxed) < 10) {
            EarlyLog("DX12: SharedCapture - Rejected swapchain %p that this generation was not initialized for "
                     "(bound identity=%p)",
                     pSwapChain, m_SwapChainBinding.Key());
        }
        return false;
    }

    ComPtr<ID3D12Device> queueDevice;
    HRESULT hr = pCommandQueue->GetDevice(IID_PPV_ARGS(&queueDevice));
    if (FAILED(hr) || !queueDevice || queueDevice.Get() != m_pDevice.Get()) {
        static std::atomic<int> s_wrongQueueDeviceLog{0};
        if (s_wrongQueueDeviceLog.fetch_add(1, std::memory_order_relaxed) < 10) {
            EarlyLog(
                "DX12: SharedCapture - Rejected queue from another device hr=0x%08X queueDevice=%p "
                "captureDevice=%p",
                hr, queueDevice.Get(), m_pDevice.Get());
        }
        return false;
    }

    if (m_InlineCompletion) {
        if (!executeCommandLists || (m_EmbeddedQueue && m_EmbeddedQueue.Get() != pCommandQueue))
            return false;
        if (!m_EmbeddedQueue)
            m_EmbeddedQueue = pCommandQueue;
    }

    // Check if we should throttle capture (encoder is falling behind)
    if (g_IPC && g_IPC->GetSharedMem()) {
        if (g_IPC->GetSharedMem()->throttleCapture.load(std::memory_order_acquire)) {
            return false;
        }
    }

    // Get the back buffer
    ComPtr<ID3D12Resource> backBuffer;
    hr = pSwapChain->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer) {
        static std::atomic<int> s_getBufferFailureLog{0};
        if (s_getBufferFailureLog.fetch_add(1, std::memory_order_relaxed) < 10) {
            EarlyLog("DX12: SharedCapture - GetBuffer(%u) failed hr=0x%08X", backBufferIndex, hr);
        }
        return false;
    }

    const D3D12_RESOURCE_DESC resDesc = backBuffer->GetDesc();
    const D3D12_RESOURCE_DESC sharedDesc = m_SharedResources[0]->GetDesc();
    if (resDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || resDesc.Width != sharedDesc.Width ||
        resDesc.Height != sharedDesc.Height || resDesc.Format != sharedDesc.Format || resDesc.SampleDesc.Count != 1) {
        static std::atomic<int> s_descMismatchLog{0};
        if (s_descMismatchLog.fetch_add(1, std::memory_order_relaxed) < 10) {
            EarlyLog(
                "DX12: SharedCapture - Backbuffer changed without reinitialize "
                "(%llux%u/%d/%u -> %llux%u/%d/%u)",
                static_cast<unsigned long long>(sharedDesc.Width), sharedDesc.Height,
                static_cast<int>(sharedDesc.Format), sharedDesc.SampleDesc.Count,
                static_cast<unsigned long long>(resDesc.Width), resDesc.Height, static_cast<int>(resDesc.Format),
                resDesc.SampleDesc.Count);
        }
        m_Active.store(false, std::memory_order_release);
        return false;
    }
    namespace probe = ce::resize_reference_probe;
    probe::CaptureStageReferences stageReferences;
    stageReferences.entry = probe::HeldByOthersBesidesCaller(backBuffer.Get());

    UINT writeIdx = m_WriteIndex.load(std::memory_order_relaxed);
    SharedMemoryLayout* captureSharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    const UINT64 completedValue = m_Fence->GetCompletedValue();
    if (completedValue == UINT64_MAX) {
        EarlyLog("DX12: SharedCapture - Fence reported device removal");
        m_Active.store(false, std::memory_order_release);
        return false;
    }
    uint32_t cpuBusySlots = 0;
    uint32_t gpuBusySlots = 0;
    const int32_t availableSlot = FindAvailableCaptureTextureSlotIf(
        captureSharedMem, static_cast<int32_t>(writeIdx), static_cast<uint32_t>(kSharedTextureCount),
        [&](int32_t candidate) {
            const UINT64 requiredValue = m_FenceValues[static_cast<UINT>(candidate)];
            return requiredValue == 0 || completedValue >= requiredValue;
        },
        &cpuBusySlots, &gpuBusySlots);
    if (availableSlot < 0) {
        if (captureSharedMem) {
            if (cpuBusySlots != 0)
                captureSharedMem->runtimeState.injectProducerCpuLeaseBusyDrops.fetch_add(1, std::memory_order_relaxed);
            if (gpuBusySlots != 0)
                captureSharedMem->runtimeState.injectProducerGpuBusyDrops.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    }
    writeIdx = static_cast<UINT>(availableSlot);
    m_WriteIndex.store(writeIdx, std::memory_order_relaxed);

    // Reset and record copy command
    hr = m_CommandAllocators[writeIdx]->Reset();
    if (FAILED(hr)) {
        EarlyLog("DX12: SharedCapture - Allocator Reset failed slot=%u hr=0x%08X", writeIdx, hr);
        m_Active.store(false, std::memory_order_release);
        return false;
    }
    hr = m_CommandList->Reset(m_CommandAllocators[writeIdx].Get(), nullptr);
    if (FAILED(hr)) {
        EarlyLog("DX12: SharedCapture - Command-list Reset failed slot=%u hr=0x%08X", writeIdx, hr);
        m_Active.store(false, std::memory_order_release);
        return false;
    }

    // GPU timestamps around the copy for the Present-stall attribution; inside
    // this list, so the game's queue gets no extra submission or Signal.
    const bool timed = !m_InlineCompletion && m_CopyGpuTimer.BeginCopy(m_CommandList.Get(), writeIdx);

    // Batch both pre-copy transitions into one call so the driver can pipeline them
    D3D12_RESOURCE_BARRIER preCopyBarriers[2] = {};
    preCopyBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    preCopyBarriers[0].Transition.pResource = backBuffer.Get();
    preCopyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    preCopyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    preCopyBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    preCopyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    preCopyBarriers[1].Transition.pResource = m_SharedResources[writeIdx].Get();
    preCopyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    preCopyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    preCopyBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_CommandList->ResourceBarrier(2, preCopyBarriers);

    // Copy to shared resource
    m_CommandList->CopyResource(m_SharedResources[writeIdx].Get(), backBuffer.Get());

    // Batch both post-copy transitions into one call
    D3D12_RESOURCE_BARRIER postCopyBarriers[2] = {};
    postCopyBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    postCopyBarriers[0].Transition.pResource = backBuffer.Get();
    postCopyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    postCopyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    postCopyBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    postCopyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    postCopyBarriers[1].Transition.pResource = m_SharedResources[writeIdx].Get();
    postCopyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    postCopyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    postCopyBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_CommandList->ResourceBarrier(2, postCopyBarriers);
    if (timed)
        m_CopyGpuTimer.EndCopy(m_CommandList.Get(), writeIdx);

    UINT64 fenceVal = m_FenceValue.fetch_add(1, std::memory_order_relaxed) + 1;
    if (m_InlineCompletion && static_cast<uint32_t>(fenceVal) == 0)
        fenceVal = m_FenceValue.fetch_add(1, std::memory_order_relaxed) + 1;
    if (m_InlineCompletion && !m_InlineCompletion->Record(m_CommandList.Get(), writeIdx, fenceVal)) {
        m_CommandList->Close();
        return false;
    }

    hr = m_CommandList->Close();
    if (FAILED(hr)) {
        EarlyLog("DX12: SharedCapture - Command-list Close failed slot=%u hr=0x%08X", writeIdx, hr);
        m_Active.store(false, std::memory_order_release);
        return false;
    }
    stageReferences.recorded = probe::HeldByOthersBesidesCaller(backBuffer.Get());

    // Timestamp the source frame before queue submission so PTS reflects the
    // frame's visual time, not GPU copy latency.
    LARGE_INTEGER qpc{};
    if (timestampQpc > 0) {
        qpc.QuadPart = timestampQpc;
    } else {
        ce::hook_clock::QueryCounter(&qpc);
    }

    // Execute
    ID3D12CommandList* cmdLists[] = {m_CommandList.Get()};
    if (executeCommandLists) {
        executeCommandLists(pCommandQueue, 1, cmdLists);
    } else {
        pCommandQueue->ExecuteCommandLists(1, cmdLists);
    }
    if (timed) {
        LARGE_INTEGER submittedQpc{};
        ce::hook_clock::QueryCounter(&submittedQpc);
        m_CopyGpuTimer.Submitted(pCommandQueue, writeIdx, fenceVal, submittedQpc.QuadPart);
    }
    stageReferences.executed = probe::HeldByOthersBesidesCaller(backBuffer.Get());

    // Signal fence
    const CaptureCompletionSignal appliedSignal =
        m_InlineCompletion ? CaptureCompletionSignal::kImmediate : completionSignal;
    if (m_InlineCompletion) {
        m_InlineCompletion->Submitted(writeIdx, fenceVal);
        hr = m_InlineCompletion->Refresh(m_Fence.Get());
    } else if (appliedSignal == CaptureCompletionSignal::kAfterPresent) {
        m_HeldSignal.Hold(pCommandQueue, fenceVal);
        hr = S_OK;
    } else {
        hr = pCommandQueue->Signal(m_Fence.Get(), fenceVal);
    }
    {
        static ce::log_meter::ChangeGate s_signalTimingLog;
        const auto verdict =
            s_signalTimingLog.Observe(ce::log_meter::FieldKey(appliedSignal, m_InlineCompletion != nullptr));
        if (verdict) {
            EarlyLog("DX12: SharedCapture completion Signal %s (embedded=%d fence=%llu queue=%p)%s",
                     CaptureCompletionSignalName(appliedSignal), m_InlineCompletion ? 1 : 0,
                     static_cast<unsigned long long>(fenceVal), pCommandQueue,
                     ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
    }
    stageReferences.signaled = probe::HeldByOthersBesidesCaller(backBuffer.Get());
    {
        static std::atomic<uint32_t> s_stageAnomalyLogs{0};
        const uint32_t anomalyLogIndex = probe::StagesAddedReferences(stageReferences)
                                             ? s_stageAnomalyLogs.fetch_add(1, std::memory_order_relaxed)
                                             : 0;
        if (probe::ShouldLogCaptureStageReferences(m_FrameCounter, stageReferences, anomalyLogIndex)) {
            EarlyLog(
                "DX12: SharedCapture back-buffer references held by others bb=%u entry=%lu recorded=%lu "
                "executed=%lu signaled=%lu copy=%u slot=%u queue=%p",
                backBufferIndex, static_cast<unsigned long>(stageReferences.entry),
                static_cast<unsigned long>(stageReferences.recorded),
                static_cast<unsigned long>(stageReferences.executed),
                static_cast<unsigned long>(stageReferences.signaled), m_FrameCounter + 1, writeIdx, pCommandQueue);
        }
    }
    if (FAILED(hr)) {
        EarlyLog("DX12: SharedCapture - Queue Signal failed slot=%u fence=%llu hr=0x%08X", writeIdx,
                 static_cast<unsigned long long>(fenceVal), hr);
        m_AbandonResourcesOnReset.store(true, std::memory_order_release);
        m_Active.store(false, std::memory_order_release);
        return false;
    }

    if (m_LastCopyQueue.Get() != pCommandQueue)
        m_LastCopyQueue = pCommandQueue;

    // Store completion value for this allocator
    m_FenceValues[writeIdx] = fenceVal;

    // Update frame descriptor
    {
        std::lock_guard<std::mutex> lock(m_Lock);

        m_CurrentFrame.sharedHandle = m_SharedHandles[writeIdx];
        m_CurrentFrame.width = (UINT)resDesc.Width;
        m_CurrentFrame.height = resDesc.Height;
        m_CurrentFrame.format = resDesc.Format;
        m_CurrentFrame.fenceValue = fenceVal;
        m_CurrentFrame.presentTime = qpc.QuadPart;
        m_CurrentFrame.frameNumber = ++m_FrameCounter;
        m_CurrentFrame.textureIndex = (int32_t)writeIdx;
        m_CurrentFrame.ready = true;
    }

    // Advance through the full producer ring so temporary GPU backlog does not
    // collapse capture onto just two textures.
    m_WriteIndex.store((writeIdx + 1) % kSharedTextureCount, std::memory_order_relaxed);

    return true;
}

bool SharedCaptureD3D12::GetCurrentFrame(SharedFrameDescriptor* pDesc) {
    if (!pDesc)
        return false;

    std::lock_guard<std::mutex> lock(m_Lock);
    if (!m_CurrentFrame.ready)
        return false;

    *pDesc = m_CurrentFrame;
    return true;
}

void SharedCaptureD3D12::ReleaseFrame(UINT frameNumber) {
    std::lock_guard<std::mutex> lock(m_Lock);
    if (m_CurrentFrame.frameNumber == frameNumber) {
        m_CurrentFrame.ready = false;
    }
}
