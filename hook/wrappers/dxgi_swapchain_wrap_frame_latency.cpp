#include "dxgi_swapchain_wrap_internal.h"


void CWrapDXGISwapChain::WaitFrameLatency() {
    const auto& gfx = GetActiveGraphicsConfig();
    if (!HasBackbufferCountOverride(gfx.backbufferCount))
        return;

    HANDLE waitable = EnsureFrameLatencyWaitable("backbuffer pacing");
    if (waitable && waitable != INVALID_HANDLE_VALUE) {
        DWORD waitResult = WaitForSingleObject(waitable, INFINITE);
        if (waitResult != WAIT_OBJECT_0) {
            static std::atomic<int> s_waitFailLogCount{0};
            if (s_waitFailLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                WrapperLog("WaitFrameLatency: wait failed result=%lu error=%lu", waitResult, GetLastError());
            }
        }
    }
}

HANDLE CWrapDXGISwapChain::EnsureFrameLatencyWaitable(const char* reason) {
    if (m_FrameLatencyWaitableQueried) {
        return m_hFrameLatencyWaitable;
    }

    EnsurePromoted();
    m_FrameLatencyWaitableQueried = true;

    if (!m_pReal2) {
        static std::atomic<int> s_noSwapchain2Log{0};
        const int n = s_noSwapchain2Log.fetch_add(1, std::memory_order_relaxed);
        if (n < 10) {
            WrapperLog("Frame latency waitable unavailable for %s: IDXGISwapChain2 not available",
                       reason ? reason : "unknown");
        }
        m_hFrameLatencyWaitable = nullptr;
        return m_hFrameLatencyWaitable;
    }

    m_hFrameLatencyWaitable = m_pReal2->GetFrameLatencyWaitableObject();
    if (m_hFrameLatencyWaitable && m_hFrameLatencyWaitable != INVALID_HANDLE_VALUE) {
        static std::atomic<int> s_waitableLog{0};
        const int n = s_waitableLog.fetch_add(1, std::memory_order_relaxed);
        if (n < 20) {
            WrapperLog("Frame latency waitable obtained for %s (waitable=%p)", reason ? reason : "unknown",
                       m_hFrameLatencyWaitable);
        }
        return m_hFrameLatencyWaitable;
    }

    static std::atomic<int> s_invalidWaitableLog{0};
    const int n = s_invalidWaitableLog.fetch_add(1, std::memory_order_relaxed);
    if (n < 20) {
        WrapperLog("Frame latency waitable unavailable for %s: GetFrameLatencyWaitableObject returned %p",
                   reason ? reason : "unknown", m_hFrameLatencyWaitable);
    }
    m_hFrameLatencyWaitable = nullptr;
    return m_hFrameLatencyWaitable;
}

void CWrapDXGISwapChain::WaitD3D12FocusLossOverlayFenceAfterPresent(
    const char* presentName, int callCount, UINT syncInterval, UINT presentFlags, HRESULT presentHr,
    const ce::dx12_overlay_policy::D3D12DeferredOverlaySignalFlushInfo& flushInfo) {
    if (!m_IsD3D12) {
        return;
    }

    HWND foregroundWindow = nullptr;
    DWORD foregroundPid = 0;
    const bool processHasForeground = ResolveCurrentProcessForeground(&foregroundWindow, &foregroundPid);
    ce::dx12_overlay_policy::D3D12FocusLossOverlayFenceWaitContext context = {};
    context.presentName = presentName;
    context.callCount = callCount;
    context.isD3D12Swapchain = m_IsD3D12;
    context.isFullscreen = m_State.isFullscreen;
    context.processHasForeground = processHasForeground;
    context.isIconic = (m_hWnd != nullptr) && IsIconic(m_hWnd);
    context.hasZeroSize = (m_State.width == 0 || m_State.height == 0);
    context.presentSucceeded = SUCCEEDED(presentHr);
    context.presentDeviceLost = IsD3D12PresentDeviceLostHRESULT(presentHr);
    context.frameGenerationActive = g_FGCompat.IsFGActive();
    context.runtimeOwnedPresentation =
        DXGIShared::DoesFGRuntimeOwnSwapchain() || DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration();
    context.usingDedicatedQueue = false;
    context.foregroundWindow = foregroundWindow;
    context.foregroundPid = foregroundPid;
    context.gameWindow = m_hWnd;
    context.processId = GetCurrentProcessId();
    context.syncInterval = syncInterval;
    context.presentFlags = presentFlags;
    context.presentHr = presentHr;
    DX12_WaitForFocusLossOverlayFenceAfterPresent(&context, &flushInfo);
}

void CWrapDXGISwapChain::ProbeD3D12FocusLossFrameLatencyAfterPresent(const char* presentName, int callCount,
                                                                     UINT syncInterval, UINT presentFlags,
                                                                     HRESULT presentHr) {
    if (!m_IsD3D12) {
        return;
    }

    HWND foregroundWindow = nullptr;
    DWORD foregroundPid = 0;
    const bool processHasForeground = ResolveCurrentProcessForeground(&foregroundWindow, &foregroundPid);
    const bool isIconic = (m_hWnd != nullptr) && IsIconic(m_hWnd);
    const bool hasZeroSize = (m_State.width == 0 || m_State.height == 0);
    const bool presentSucceeded = SUCCEEDED(presentHr);
    const bool frameGenerationActive = g_FGCompat.IsFGActive();
    const bool runtimeOwnedPresentation =
        DXGIShared::DoesFGRuntimeOwnSwapchain() || DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration();

    const bool focusLossTelemetryCandidate = m_IsD3D12 && !m_State.isFullscreen && !processHasForeground && !isIconic &&
                                             !hasZeroSize && presentSucceeded && !frameGenerationActive &&
                                             !runtimeOwnedPresentation;
    HANDLE waitable = INVALID_HANDLE_VALUE;
    const bool hasFrameLatencyWaitable = waitable && waitable != INVALID_HANDLE_VALUE;
    const bool shouldWait = DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(
        m_IsD3D12, m_State.isFullscreen, processHasForeground, isIconic, hasZeroSize, presentSucceeded,
        frameGenerationActive, runtimeOwnedPresentation, hasFrameLatencyWaitable);

    if (!shouldWait) {
        if (!processHasForeground) {
            static std::atomic<int> s_focusSkipLog{0};
            const int n = s_focusSkipLog.fetch_add(1, std::memory_order_relaxed);
            if (n < 20 || (n % 1000) == 0) {
                WrapperLog(
                    "%s#%d: D3D12 focus-loss frame-latency waitable telemetry probe skipped "
                    "(fg=%p/%lu ours=%p/%lu sync=%u flags=0x%08X presentHr=0x%08X fullscreen=%d iconic=%d "
                    "zeroSize=%d fgActive=%d runtimeOwned=%d waitable=%p available=%d candidate=%d "
                    "reason=present-passthrough-v7)",
                    presentName, callCount, foregroundWindow, foregroundPid, m_hWnd, GetCurrentProcessId(),
                    syncInterval, presentFlags, (unsigned)presentHr, m_State.isFullscreen ? 1 : 0, isIconic ? 1 : 0,
                    hasZeroSize ? 1 : 0, frameGenerationActive ? 1 : 0, runtimeOwnedPresentation ? 1 : 0,
                    hasFrameLatencyWaitable ? waitable : nullptr, hasFrameLatencyWaitable ? 1 : 0,
                    focusLossTelemetryCandidate ? 1 : 0);
            }
        }
        return;
    }

    constexpr DWORD kFocusLossFrameLatencyProbeMs = 0;
    DWORD waitResult = WaitForSingleObject(waitable, kFocusLossFrameLatencyProbeMs);
    DWORD waitLastError = (waitResult == WAIT_FAILED) ? GetLastError() : 0;

    static std::atomic<int> s_focusWaitLog{0};
    const int n = s_focusWaitLog.fetch_add(1, std::memory_order_relaxed);
    if (n < 50 || waitResult != WAIT_OBJECT_0 || (n % 300) == 0) {
        WrapperLog(
            "%s#%d: D3D12 focus-loss frame-latency waitable telemetry probe result=%s(0x%08lX) "
            "fg=%p/%lu ours=%p/%lu sync=%u flags=0x%08X waitable=%p available=1 timeoutMs=%lu gle=%lu",
            presentName, callCount, WaitResultName(waitResult), waitResult, foregroundWindow, foregroundPid, m_hWnd,
            GetCurrentProcessId(), syncInterval, presentFlags, waitable, kFocusLossFrameLatencyProbeMs, waitLastError);
    }
}
