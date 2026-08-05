#include "dx12_fg_switch_test_internal.h"

// Derives the render resolution + jitter phase count from the upscaling config. Called once after
// the display size is final (the upscaling settings are fixed for the run).
void UpdateRenderResolution() {
    if (!dx12_fg_switch_test_g_UpscalingEnabled) {
        dx12_fg_switch_test_g_RenderWidth = dx12_fg_switch_test_g_WindowWidth;
        dx12_fg_switch_test_g_RenderHeight = dx12_fg_switch_test_g_WindowHeight;
        dx12_fg_switch_test_g_JitterPhaseCount = 8;
        dx12_fg_switch_test_g_CurrentJitter = {0.0f, 0.0f};
        testapp::Log("[FG-DIAG] Upscaling disabled: native rendering at %dx%d (legacy behavior)\n", dx12_fg_switch_test_g_WindowWidth,
                     dx12_fg_switch_test_g_WindowHeight);
        return;
    }
    const testapp::fg::RenderSize renderSize =
        testapp::fg::ComputeRenderSize(static_cast<unsigned>(dx12_fg_switch_test_g_WindowWidth), static_cast<unsigned>(dx12_fg_switch_test_g_WindowHeight),
                                       dx12_fg_switch_test_g_UpscaleQuality, dx12_fg_switch_test_g_UpscaleScalePercent);
    dx12_fg_switch_test_g_RenderWidth = static_cast<int>(renderSize.width);
    dx12_fg_switch_test_g_RenderHeight = static_cast<int>(renderSize.height);
    dx12_fg_switch_test_g_JitterPhaseCount =
        testapp::fg::JitterPhaseCount(static_cast<unsigned>(dx12_fg_switch_test_g_RenderWidth), static_cast<unsigned>(dx12_fg_switch_test_g_WindowWidth));
    testapp::Log(
        "[FG-DIAG] Upscaling: quality=%s scaleOverride=%d%% display=%dx%d render=%dx%d jitterPhases=%d "
        "dlssPreset=%c dlssHdr=%d fsrVersion=%d sharpening=%d\n",
        testapp::fg::UpscaleQualityName(dx12_fg_switch_test_g_UpscaleQuality), dx12_fg_switch_test_g_UpscaleScalePercent, dx12_fg_switch_test_g_WindowWidth, dx12_fg_switch_test_g_WindowHeight,
        dx12_fg_switch_test_g_RenderWidth, dx12_fg_switch_test_g_RenderHeight, dx12_fg_switch_test_g_JitterPhaseCount, dx12_fg_switch_test_g_DlssPresetConfig ? dx12_fg_switch_test_g_DlssPresetConfig : '-',
        dx12_fg_switch_test_g_DlssHdrInput ? 1 : 0, dx12_fg_switch_test_g_FsrUpscaleVersionConfig, dx12_fg_switch_test_g_FsrSharpeningEnabled ? 1 : 0);
}

testapp::fg::ProxyPresentPolicy ResolvePresentPolicy() {
    const bool dlssProxyTarget =
        dx12_fg_switch_test_g_CurrentMode == FGMode::DLSS || testapp::fg::IsDlssReplacementSurfaceStage(dx12_fg_switch_test_g_FsrExitTransitionStage);
    return testapp::fg::ResolveProxyPresentPolicy(dlssProxyTarget, dx12_fg_switch_test_g_VSync, dx12_fg_switch_test_g_CurrentSwapChainAllowTearing);
}

UINT ResolvePresentSyncInterval() {
    return ResolvePresentPolicy().syncInterval;
}

UINT ResolvePresentFlags(UINT syncInterval) {
    if (syncInterval == 0 && ResolvePresentPolicy().allowTearing) {
        return DXGI_PRESENT_ALLOW_TEARING;
    }
    return 0;
}

void RequestMode(FGMode mode, const char* reason, bool manual) {
    dx12_fg_switch_test_g_PendingMode = mode;
    dx12_fg_switch_test_g_ModeSwitchPending = true;
    dx12_fg_switch_test_g_SuspensionTogglePending = false;
    if (manual) {
        dx12_fg_switch_test_g_ManualMode = true;
    }
    testapp::Log("[FG-DIAG] Mode request: %s -> %s (%s manual=%d armed=%d fsrSuspended=%d dlssSuspended=%d)\n",
                 ModeName(dx12_fg_switch_test_g_CurrentMode), ModeName(mode), reason ? reason : "unknown", manual ? 1 : 0,
                 dx12_fg_switch_test_g_ModeSwitchingArmed ? 1 : 0, dx12_fg_switch_test_g_FsrSuspended ? 1 : 0, dx12_fg_switch_test_g_DlssSuspended ? 1 : 0);
    testapp::LogFlush();
}

void RequestSuspensionToggle(FGMode mode, const char* reason) {
    dx12_fg_switch_test_g_PendingSuspensionToggleMode = mode;
    dx12_fg_switch_test_g_SuspensionTogglePending = true;
    dx12_fg_switch_test_g_ManualMode = true;
    testapp::Log(
        "[FG-DIAG] Suspension toggle request: current=%s target=%s (%s) fsr=%d fsrSuspended=%d dlss=%d "
        "dlssSuspended=%d armed=%d\n",
        ModeName(dx12_fg_switch_test_g_CurrentMode), ModeName(mode), reason ? reason : "unknown", dx12_fg_switch_test_g_FsrEnabled ? 1 : 0,
        dx12_fg_switch_test_g_FsrSuspended ? 1 : 0, dx12_fg_switch_test_g_DlssEnabled ? 1 : 0, dx12_fg_switch_test_g_DlssSuspended ? 1 : 0, dx12_fg_switch_test_g_ModeSwitchingArmed ? 1 : 0);
    testapp::LogFlush();
}

void RequestModeOrToggle(FGMode mode, const char* reason) {
    if ((mode == FGMode::DLSS || mode == FGMode::FSR) && dx12_fg_switch_test_g_CurrentMode == mode && !dx12_fg_switch_test_g_ModeSwitchPending) {
        RequestSuspensionToggle(mode, reason);
        return;
    }
    RequestMode(mode, reason, true);
}

void ResetFSRSuspensionStressState(const char* reason) {
    if (dx12_fg_switch_test_g_FsrSuspended) {
        testapp::Log("[FG-DIAG] FSR suspension stress reset to resumed (%s)\n", reason ? reason : "unknown");
    }
    dx12_fg_switch_test_g_FsrSuspended = false;
    dx12_fg_switch_test_g_LastFsrSuspendResumeToggleTime = std::chrono::high_resolution_clock::now();
    dx12_fg_switch_test_g_LastFsrSuspendResumeToggleFrameId = dx12_fg_switch_test_g_FrameIdCounter;
}

void ResetFSRPresentCallbackStressState(const char* reason) {
    dx12_fg_switch_test_g_FsrPresentCallbackStressStartTime = std::chrono::high_resolution_clock::now();
    dx12_fg_switch_test_g_FsrLastConfigureUsedPresentCallback = !dx12_fg_switch_test_g_FsrPresentCallbackStress;
    testapp::Log(
        "[FG-DIAG] FSR present-callback stress reset (%s): stress=%d interval=%ds initialRoute=%s frameID=%llu\n",
        reason ? reason : "unknown", dx12_fg_switch_test_g_FsrPresentCallbackStress ? 1 : 0, dx12_fg_switch_test_g_FsrPresentCallbackToggleIntervalSeconds,
        dx12_fg_switch_test_g_FsrLastConfigureUsedPresentCallback ? "app-callback" : "amd-internal",
        static_cast<unsigned long long>(dx12_fg_switch_test_g_FrameIdCounter));
}

bool SameAdapterLuid(const LUID& a, const LUID& b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

void InitDxgiVideoMemoryQueryStressAdapter(const char* reason) {
    g_DxgiVideoMemoryQueryAdapter.Reset();
    if (!dx12_fg_switch_test_g_DxgiVideoMemoryQueryStress || !g_Device) {
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

void RunDxgiVideoMemoryQueryStress() {
    if (!dx12_fg_switch_test_g_DxgiVideoMemoryQueryStress || dx12_fg_switch_test_g_DxgiVideoMemoryQueryCountPerFrame <= 0 || dx12_fg_switch_test_g_CurrentMode != FGMode::FSR ||
        !g_DxgiVideoMemoryQueryAdapter) {
        return;
    }

    HRESULT lastHr = S_OK;
    DXGI_QUERY_VIDEO_MEMORY_INFO lastInfo = {};
    DXGI_MEMORY_SEGMENT_GROUP lastSegment = DXGI_MEMORY_SEGMENT_GROUP_LOCAL;
    for (int i = 0; i < dx12_fg_switch_test_g_DxgiVideoMemoryQueryCountPerFrame; ++i) {
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
        FAILED(lastHr) || dx12_fg_switch_test_g_FrameIdCounter <= 5 || dx12_fg_switch_test_g_FrameIdCounter - dx12_fg_switch_test_g_LastDxgiVideoMemoryQueryStressLogFrame >= 120;
    if (shouldLog) {
        dx12_fg_switch_test_g_LastDxgiVideoMemoryQueryStressLogFrame = dx12_fg_switch_test_g_FrameIdCounter;
        testapp::Log(
            "[FG-DIAG] DXGI video-memory stress frameID=%llu queries=%d segment=%d hr=0x%08lx "
            "budgetMB=%llu usageMB=%llu fsrSuspended=%d\n",
            static_cast<unsigned long long>(dx12_fg_switch_test_g_FrameIdCounter), dx12_fg_switch_test_g_DxgiVideoMemoryQueryCountPerFrame,
            static_cast<int>(lastSegment), static_cast<unsigned long>(lastHr),
            static_cast<unsigned long long>(lastInfo.Budget / (1024 * 1024)),
            static_cast<unsigned long long>(lastInfo.CurrentUsage / (1024 * 1024)), dx12_fg_switch_test_g_FsrSuspended ? 1 : 0);
        if (FAILED(lastHr)) {
            testapp::LogFlush();
        }
    }
}

void MaybeToggleFSRSuspensionStress(UINT frameIndex) {
    if (!dx12_fg_switch_test_g_FsrSuspendResumeStress || dx12_fg_switch_test_g_ManualMode || dx12_fg_switch_test_g_CurrentMode != FGMode::FSR || !dx12_fg_switch_test_g_FsrEnabled || !dx12_fg_switch_test_g_FfxCtx) {
        return;
    }

    const auto now = std::chrono::high_resolution_clock::now();
    const float elapsedSinceToggle = std::chrono::duration<float>(now - dx12_fg_switch_test_g_LastFsrSuspendResumeToggleTime).count();
    if (elapsedSinceToggle < static_cast<float>(dx12_fg_switch_test_g_FsrSuspendResumeIntervalSeconds)) {
        return;
    }

    dx12_fg_switch_test_g_FsrSuspended = !dx12_fg_switch_test_g_FsrSuspended;
    dx12_fg_switch_test_g_LastFsrSuspendResumeToggleTime = now;
    dx12_fg_switch_test_g_LastFsrSuspendResumeToggleFrameId = dx12_fg_switch_test_g_FrameIdCounter;

    const bool enable = !dx12_fg_switch_test_g_FsrSuspended;
    testapp::Log("[FG-DIAG] FSR suspension stress: %s frameID=%llu frameIndex=%u interval=%ds\n",
                 enable ? "resume FG" : "suspend FG", static_cast<unsigned long long>(dx12_fg_switch_test_g_FrameIdCounter), frameIndex,
                 dx12_fg_switch_test_g_FsrSuspendResumeIntervalSeconds);
    if (ConfigureFSR(enable, frameIndex < g_SwapChainBufferCount ? g_RenderTargets[frameIndex].Get() : nullptr,
                     enable ? "stress resume FSR FG" : "stress suspend FSR FG", true) &&
        enable) {
        RegisterFSRUiResource();
    }
    UpdateWindowTitle();
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DESTROY:
            dx12_fg_switch_test_g_Running = false;
            PostQuitMessage(0);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                dx12_fg_switch_test_g_Running = false;
                DestroyWindow(hWnd);
            } else if (wParam == '1') {
                RequestMode(FGMode::Off, "key 1", true);
            } else if (wParam == '2') {
                RequestModeOrToggle(FGMode::DLSS, "key 2");
            } else if (wParam == '3') {
                RequestModeOrToggle(FGMode::FSR, "key 3");
            }
            return 0;
        default:
            break;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}
