// Extracted from dx12_fg_switch_test.cpp to stay under the AGENTS.md
// size ceiling. Included at exactly the point these definitions used to sit,
// so declaration order is unchanged.

static testapp::fg::ProxyPresentPolicy ResolvePresentPolicy() {
    const bool dlssProxyTarget =
        g_CurrentMode == FGMode::DLSS || testapp::fg::IsDlssReplacementSurfaceStage(g_FsrExitTransitionStage);
    return testapp::fg::ResolveProxyPresentPolicy(dlssProxyTarget, g_VSync, g_CurrentSwapChainAllowTearing);
}

static UINT ResolvePresentSyncInterval() {
    return ResolvePresentPolicy().syncInterval;
}

static UINT ResolvePresentFlags(UINT syncInterval) {
    if (syncInterval == 0 && ResolvePresentPolicy().allowTearing) {
        return DXGI_PRESENT_ALLOW_TEARING;
    }
    return 0;
}

static void RequestMode(FGMode mode, const char* reason, bool manual) {
    g_PendingMode = mode;
    g_ModeSwitchPending = true;
    g_SuspensionTogglePending = false;
    if (manual) {
        g_ManualMode = true;
    }
    testapp::Log("[FG-DIAG] Mode request: %s -> %s (%s manual=%d armed=%d fsrSuspended=%d dlssSuspended=%d)\n",
                 ModeName(g_CurrentMode), ModeName(mode), reason ? reason : "unknown", manual ? 1 : 0,
                 g_ModeSwitchingArmed ? 1 : 0, g_FsrSuspended ? 1 : 0, g_DlssSuspended ? 1 : 0);
    testapp::LogFlush();
}

static void RequestSuspensionToggle(FGMode mode, const char* reason) {
    g_PendingSuspensionToggleMode = mode;
    g_SuspensionTogglePending = true;
    g_ManualMode = true;
    testapp::Log(
        "[FG-DIAG] Suspension toggle request: current=%s target=%s (%s) fsr=%d fsrSuspended=%d dlss=%d "
        "dlssSuspended=%d armed=%d\n",
        ModeName(g_CurrentMode), ModeName(mode), reason ? reason : "unknown", g_FsrEnabled ? 1 : 0,
        g_FsrSuspended ? 1 : 0, g_DlssEnabled ? 1 : 0, g_DlssSuspended ? 1 : 0, g_ModeSwitchingArmed ? 1 : 0);
    testapp::LogFlush();
}

static void RequestModeOrToggle(FGMode mode, const char* reason) {
    if ((mode == FGMode::DLSS || mode == FGMode::FSR) && g_CurrentMode == mode && !g_ModeSwitchPending) {
        RequestSuspensionToggle(mode, reason);
        return;
    }
    RequestMode(mode, reason, true);
}

static void ResetFSRSuspensionStressState(const char* reason) {
    if (g_FsrSuspended) {
        testapp::Log("[FG-DIAG] FSR suspension stress reset to resumed (%s)\n", reason ? reason : "unknown");
    }
    g_FsrSuspended = false;
    g_LastFsrSuspendResumeToggleTime = std::chrono::high_resolution_clock::now();
    g_LastFsrSuspendResumeToggleFrameId = g_FrameIdCounter;
}

static void ResetFSRPresentCallbackStressState(const char* reason) {
    g_FsrPresentCallbackStressStartTime = std::chrono::high_resolution_clock::now();
    g_FsrLastConfigureUsedPresentCallback = !g_FsrPresentCallbackStress;
    testapp::Log(
        "[FG-DIAG] FSR present-callback stress reset (%s): stress=%d interval=%ds initialRoute=%s frameID=%llu\n",
        reason ? reason : "unknown", g_FsrPresentCallbackStress ? 1 : 0, g_FsrPresentCallbackToggleIntervalSeconds,
        g_FsrLastConfigureUsedPresentCallback ? "app-callback" : "amd-internal",
        static_cast<unsigned long long>(g_FrameIdCounter));
}

static bool SameAdapterLuid(const LUID& a, const LUID& b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

static void InitDxgiVideoMemoryQueryStressAdapter(const char* reason) {
    g_DxgiVideoMemoryQueryAdapter.Reset();
    if (!g_DxgiVideoMemoryQueryStress || !g_Device) {
        return;
    }

    ComPtr<IDXGIFactory4> factory;
    HRESULT factoryHr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(factoryHr) || !factory) {
        testapp::Log("[FG-DIAG] DXGI video-memory stress adapter lookup failed: factory hr=0x%08lx\n",
                     static_cast<unsigned long>(factoryHr));
        return;
    }

    const LUID deviceLuid = g_Device->GetAdapterLuid();
    ComPtr<IDXGIAdapter1> fallbackAdapter;
    for (UINT adapterIndex = 0;; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT enumHr = factory->EnumAdapters1(adapterIndex, &adapter);
        if (enumHr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(enumHr) || !adapter) {
            continue;
        }

        DXGI_ADAPTER_DESC1 desc = {};
        if (FAILED(adapter->GetDesc1(&desc))) {
            continue;
        }
        if (!fallbackAdapter && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            fallbackAdapter = adapter;
        }
        if (SameAdapterLuid(desc.AdapterLuid, deviceLuid)) {
            if (SUCCEEDED(adapter.As(&g_DxgiVideoMemoryQueryAdapter)) && g_DxgiVideoMemoryQueryAdapter) {
                testapp::Log("[FG-DIAG] DXGI video-memory stress adapter matched device LUID for %s index=%u\n",
                             reason ? reason : "dx12 init", adapterIndex);
                return;
            }
        }
    }

    if (fallbackAdapter && SUCCEEDED(fallbackAdapter.As(&g_DxgiVideoMemoryQueryAdapter)) &&
        g_DxgiVideoMemoryQueryAdapter) {
        testapp::Log("[FG-DIAG] DXGI video-memory stress adapter using first hardware fallback for %s\n",
                     reason ? reason : "dx12 init");
    } else {
        testapp::Log("[FG-DIAG] DXGI video-memory stress adapter unavailable for %s\n", reason ? reason : "dx12 init");
    }
}

static void RunDxgiVideoMemoryQueryStress() {
    if (!g_DxgiVideoMemoryQueryStress || g_DxgiVideoMemoryQueryCountPerFrame <= 0 || g_CurrentMode != FGMode::FSR ||
        !g_DxgiVideoMemoryQueryAdapter) {
        return;
    }

    HRESULT lastHr = S_OK;
    DXGI_QUERY_VIDEO_MEMORY_INFO lastInfo = {};
    DXGI_MEMORY_SEGMENT_GROUP lastSegment = DXGI_MEMORY_SEGMENT_GROUP_LOCAL;
    for (int i = 0; i < g_DxgiVideoMemoryQueryCountPerFrame; ++i) {
        lastSegment = (i & 1) ? DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL : DXGI_MEMORY_SEGMENT_GROUP_LOCAL;
        DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
        lastHr = g_DxgiVideoMemoryQueryAdapter->QueryVideoMemoryInfo(0, lastSegment, &info);
        if (FAILED(lastHr)) {
            lastInfo = {};
            break;
        }
        lastInfo = info;
    }

    const bool shouldLog =
        FAILED(lastHr) || g_FrameIdCounter <= 5 || g_FrameIdCounter - g_LastDxgiVideoMemoryQueryStressLogFrame >= 120;
    if (shouldLog) {
        g_LastDxgiVideoMemoryQueryStressLogFrame = g_FrameIdCounter;
        testapp::Log(
            "[FG-DIAG] DXGI video-memory stress frameID=%llu queries=%d segment=%d hr=0x%08lx "
            "budgetMB=%llu usageMB=%llu fsrSuspended=%d\n",
            static_cast<unsigned long long>(g_FrameIdCounter), g_DxgiVideoMemoryQueryCountPerFrame,
            static_cast<int>(lastSegment), static_cast<unsigned long>(lastHr),
            static_cast<unsigned long long>(lastInfo.Budget / (1024 * 1024)),
            static_cast<unsigned long long>(lastInfo.CurrentUsage / (1024 * 1024)), g_FsrSuspended ? 1 : 0);
        if (FAILED(lastHr)) {
            testapp::LogFlush();
        }
    }
}

static void MaybeToggleFSRSuspensionStress(UINT frameIndex) {
    if (!g_FsrSuspendResumeStress || g_ManualMode || g_CurrentMode != FGMode::FSR || !g_FsrEnabled || !g_FfxCtx) {
        return;
    }

    const auto now = std::chrono::high_resolution_clock::now();
    const float elapsedSinceToggle = std::chrono::duration<float>(now - g_LastFsrSuspendResumeToggleTime).count();
    if (elapsedSinceToggle < static_cast<float>(g_FsrSuspendResumeIntervalSeconds)) {
        return;
    }

    g_FsrSuspended = !g_FsrSuspended;
    g_LastFsrSuspendResumeToggleTime = now;
    g_LastFsrSuspendResumeToggleFrameId = g_FrameIdCounter;

    const bool enable = !g_FsrSuspended;
    testapp::Log("[FG-DIAG] FSR suspension stress: %s frameID=%llu frameIndex=%u interval=%ds\n",
                 enable ? "resume FG" : "suspend FG", static_cast<unsigned long long>(g_FrameIdCounter), frameIndex,
                 g_FsrSuspendResumeIntervalSeconds);
    if (ConfigureFSR(enable, frameIndex < g_SwapChainBufferCount ? g_RenderTargets[frameIndex].Get() : nullptr,
                     enable ? "stress resume FSR FG" : "stress suspend FSR FG", true) &&
        enable) {
        RegisterFSRUiResource();
    }
    UpdateWindowTitle();
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DESTROY:
            g_Running = false;
            PostQuitMessage(0);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                g_Running = false;
                DestroyWindow(hWnd);
            } else if (wParam == '1') {
                RequestMode(FGMode::Off, "key 1", true);
            } else if (wParam == '2') {
                RequestModeOrToggle(FGMode::DLSS, "key 2");
            } else if (wParam == '3') {
                RequestModeOrToggle(FGMode::FSR, "key 3");
            }
            return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static bool LoadFSRRuntimeSerialized(const char* reason);
static void UnloadFSRRuntimeSerialized(const char* reason);

#include "dx12_fg_switch_common.inl"
#include "dx12_fg_switch_fsr.inl"
#include "dx12_fg_switch_streamline.inl"
#include "dx12_fg_switch_swapchain.inl"
#include "dx12_fg_switch_upscale.inl"
