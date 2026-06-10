// Included by dx12_fg_switch_test.cpp; shares that file's static DX12/FG state.

static void WaitForFenceValue(UINT64 fenceValue, const char* reason) {
    if (!g_Fence || g_Fence->GetCompletedValue() >= fenceValue) {
        return;
    }
    g_Fence->SetEventOnCompletion(fenceValue, g_FenceEvent);
    const uint64_t startMs = GetTickCount64();
    uint64_t nextLogMs = 500;
    int logCount = 0;
    while (g_Fence->GetCompletedValue() < fenceValue) {
        const DWORD waitResult = WaitForSingleObject(g_FenceEvent, 100);
        if (waitResult == WAIT_OBJECT_0) {
            return;
        }
        // Re-log periodically while a fence wait stalls so a hang leaves a clear, repeated trail
        // (the device-removed reason distinguishes a genuine GPU TDR from a present-pipeline stall
        // where the device stays alive but the queued Signal never executes).
        const uint64_t elapsedMs = GetTickCount64() - startMs;
        if (elapsedMs >= nextLogMs && logCount < 30) {
            ++logCount;
            nextLogMs += 1000;
            const HRESULT removedReason = g_Device ? g_Device->GetDeviceRemovedReason() : S_OK;
            testapp::Log("[FG-DIAG] WARN stalled fence wait (%s): elapsedMs=%llu waiting=%llu completed=%llu "
                         "frameIndex=%u mode=%s fsrSuspended=%d dlssSuspended=%d frameID=%llu deviceRemoved=0x%08lx\n",
                         reason ? reason : "unknown", static_cast<unsigned long long>(elapsedMs),
                         static_cast<unsigned long long>(fenceValue),
                         static_cast<unsigned long long>(g_Fence->GetCompletedValue()), g_FrameIndex,
                         ModeName(g_CurrentMode), g_FsrSuspended ? 1 : 0, g_DlssSuspended ? 1 : 0,
                         static_cast<unsigned long long>(g_FrameIdCounter),
                         static_cast<unsigned long>(removedReason));
            testapp::LogFlush();
            if (removedReason != S_OK) {
                // The fence can never signal on a removed device: dump DRED (once, internal guard)
                // and abandon the wait so the run ends with a diagnosed log instead of a live-lock.
                DumpDredOnDeviceRemoved(reason);
                testapp::Log("[FG-DIAG] Abandoning fence wait (%s) after device removal; stopping main loop\n",
                             reason ? reason : "unknown");
                testapp::LogFlush();
                g_Running = false;
                return;
            }
        }
    }
}

static void WaitForSwapChainFrameLatency() {
    if (g_FrameLatencyWaitHandle) {
        static bool s_loggedFsrNonBlockingWait = false;
        static bool s_loggedNativeWaitTimeout = false;
        if (g_SwapChainOwner == SwapChainOwner::FSR) {
            const DWORD probe = WaitForSingleObject(g_FrameLatencyWaitHandle, 0);
            if (!s_loggedFsrNonBlockingWait) {
                s_loggedFsrNonBlockingWait = true;
                testapp::Log("[FG-DIAG] FSR proxy swapchain uses non-blocking frame-latency probe "
                             "(waitable=%p firstProbe=%lu mode=%s enabled=%d suspended=%d)\n",
                             g_FrameLatencyWaitHandle, static_cast<unsigned long>(probe), ModeName(g_CurrentMode),
                             g_FsrEnabled ? 1 : 0, g_FsrSuspended ? 1 : 0);
                testapp::LogFlush();
            }
            return;
        }
        s_loggedFsrNonBlockingWait = false;
        const DWORD waitResult = WaitForSingleObject(g_FrameLatencyWaitHandle, 100);
        if (waitResult == WAIT_TIMEOUT && !s_loggedNativeWaitTimeout) {
            s_loggedNativeWaitTimeout = true;
            testapp::Log("[FG-DIAG] WARN native/proxy frame-latency wait timed out once; continuing to avoid a "
                         "self-induced startup stall (waitable=%p mode=%s enabled=%d suspended=%d owner=%s)\n",
                         g_FrameLatencyWaitHandle, ModeName(g_CurrentMode), g_FsrEnabled ? 1 : 0,
                         g_FsrSuspended ? 1 : 0, SwapChainOwnerName(g_SwapChainOwner));
            testapp::LogFlush();
        }
    }
}

static void WaitForGpu() {
    std::lock_guard<std::mutex> lock(g_FrameSyncMutex);
    if (!g_CommandQueue || !g_Fence) {
        return;
    }
    const UINT64 fenceValue = g_FenceValues[g_FrameIndex];
    g_CommandQueue->Signal(g_Fence.Get(), fenceValue);
    WaitForFenceValue(fenceValue, "WaitForGpu");
    g_FenceValues[g_FrameIndex]++;
}

static void MoveToNextFrame() {
    std::lock_guard<std::mutex> lock(g_FrameSyncMutex);
    const UINT64 currentFenceValue = g_FenceValues[g_FrameIndex];
    g_CommandQueue->Signal(g_Fence.Get(), currentFenceValue);
    UINT nextFrameIndex = g_SwapChain->GetCurrentBackBufferIndex();
    if (nextFrameIndex >= g_SwapChainBufferCount) {
        static std::atomic<bool> s_LoggedBadIndex{false};
        if (!s_LoggedBadIndex.exchange(true)) {
            testapp::Log("[FG-DIAG] WARN back-buffer index %u out of range (buffers=%u); clamping\n", nextFrameIndex,
                         g_SwapChainBufferCount);
            testapp::LogFlush();
        }
        nextFrameIndex %= g_SwapChainBufferCount;
    }
    WaitForFenceValue(g_FenceValues[nextFrameIndex], "MoveToNextFrame");
    g_FrameIndex = nextFrameIndex;
    g_FenceValues[g_FrameIndex] = currentFenceValue + 1;
}

static std::wstring ExeDirectoryW() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring dir = path;
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        dir.resize(pos);
    }
    return dir;
}
