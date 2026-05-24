// Included by dx12_fg_switch_test.cpp; shares that file's static DX12/FG state.

static void WaitForFenceValue(UINT64 fenceValue, const char* reason) {
    if (!g_Fence || g_Fence->GetCompletedValue() >= fenceValue) {
        return;
    }
    g_Fence->SetEventOnCompletion(fenceValue, g_FenceEvent);
    const uint64_t startMs = GetTickCount64();
    while (g_Fence->GetCompletedValue() < fenceValue) {
        const DWORD waitResult = WaitForSingleObject(g_FenceEvent, 100);
        if (waitResult == WAIT_OBJECT_0) {
            return;
        }
        if (GetTickCount64() - startMs >= 500) {
            static std::atomic<bool> s_LoggedSlowFence{false};
            if (!s_LoggedSlowFence.exchange(true)) {
                testapp::Log("[FG-DIAG] WARN slow fence wait (%s): waiting=%llu completed=%llu frameIndex=%u\n",
                             reason ? reason : "unknown", static_cast<unsigned long long>(fenceValue),
                             static_cast<unsigned long long>(g_Fence->GetCompletedValue()), g_FrameIndex);
                testapp::LogFlush();
            }
        }
    }
}

static void WaitForSwapChainFrameLatency() {
    if (g_FrameLatencyWaitHandle) {
        static bool s_loggedFsrNonBlockingWait = false;
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
        WaitForSingleObject(g_FrameLatencyWaitHandle, INFINITE);
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
