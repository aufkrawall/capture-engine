#include "dx12_fg_switch_test_internal.h"

const char* ModeName(FGMode mode) {
    switch (mode) {
        case FGMode::Off:
            return "OFF";
        case FGMode::DLSS:
            return "DLSS FG";
        case FGMode::FSR:
            return "FSR FG";
        default:
            return "UNKNOWN";
    }
}

const char* SwapChainOwnerName(SwapChainOwner owner) {
    switch (owner) {
        case SwapChainOwner::Native:
            return "native";
        case SwapChainOwner::FSR:
            return "fsr-wrapper";
        default:
            return "unknown";
    }
}

static const wchar_t* kWindowClass = L"CaptureTestDX12FGSwitch";

ComPtr<ID3D12Device> g_Device;

ComPtr<ID3D12CommandQueue> g_CommandQueue;

ComPtr<IDXGIAdapter3> g_DxgiVideoMemoryQueryAdapter;

ComPtr<IDXGISwapChain3> g_SwapChain;

ComPtr<ID3D12DescriptorHeap> g_RtvHeap;

ComPtr<ID3D12Resource> g_RenderTargets[dx12_fg_switch_test_kMaxSwapChainBuffers];

ComPtr<ID3D12CommandAllocator> g_CommandAllocators[dx12_fg_switch_test_kMaxSwapChainBuffers];

ComPtr<ID3D12GraphicsCommandList> g_CommandList;

ComPtr<ID3D12Fence> g_Fence;

testapp::fg::GpuFrameTimer g_GpuFrameTimer;

testapp::fg::FramePhaseTimers g_FramePhases;

testapp::dx12fg::AuxiliaryResources g_FgInputs;

testapp::dx12fg::SceneRenderer g_Scene;

HANDLE g_FenceEvent = nullptr;

HANDLE g_FrameLatencyWaitHandle = nullptr;

UINT64 g_FenceValues[dx12_fg_switch_test_kMaxSwapChainBuffers] = {};

UINT g_FrameIndex = 0;

UINT g_SwapChainBufferCount = dx12_fg_switch_test_kRequestedBackBuffers;

UINT g_MaxFrameLatency = dx12_fg_switch_test_kRequestedBackBuffers;

UINT g_RtvDescriptorSize = 0;

std::mutex g_FrameSyncMutex;

const char* SlResultName(sl::Result result) {
    switch (result) {
        case sl::Result::eOk:
            return "eOk";
        case sl::Result::eErrorDriverOutOfDate:
            return "eErrorDriverOutOfDate";
        case sl::Result::eErrorOSOutOfDate:
            return "eErrorOSOutOfDate";
        case sl::Result::eErrorOSDisabledHWS:
            return "eErrorOSDisabledHWS";
        case sl::Result::eErrorDeviceNotCreated:
            return "eErrorDeviceNotCreated";
        case sl::Result::eErrorNoSupportedAdapterFound:
            return "eErrorNoSupportedAdapterFound";
        case sl::Result::eErrorNotInitialized:
            return "eErrorNotInitialized";
        case sl::Result::eErrorFeatureNotSupported:
            return "eErrorFeatureNotSupported";
        case sl::Result::eErrorFeatureMissing:
            return "eErrorFeatureMissing";
        case sl::Result::eErrorFeatureFailedToLoad:
            return "eErrorFeatureFailedToLoad";
        case sl::Result::eErrorInvalidParameter:
            return "eErrorInvalidParameter";
        case sl::Result::eErrorInvalidState:
            return "eErrorInvalidState";
        default:
            return "unknown";
    }
}

const char* FfxReturnName(ffxReturnCode_t code) {
    switch (code) {
        case FFX_API_RETURN_OK:
            return "OK";
        case FFX_API_RETURN_ERROR:
            return "ERROR";
        case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE:
            return "UNKNOWN_DESCTYPE";
        case FFX_API_RETURN_ERROR_RUNTIME_ERROR:
            return "RUNTIME_ERROR";
        case FFX_API_RETURN_NO_PROVIDER:
            return "NO_PROVIDER";
        case FFX_API_RETURN_ERROR_MEMORY:
            return "MEMORY";
        case FFX_API_RETURN_ERROR_PARAMETER:
            return "PARAMETER";
        case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE:
            return "PROVIDER_NO_SUPPORT_NEW_DESCTYPE";
        default:
            return "unknown";
    }
}

    // NOLINTNEXTLINE(bugprone-exception-escape) - standalone test harness: an unexpected exception terminating the process is acceptable and yields a nonzero exit
int main(int argc, char* argv[]) {
    LoadConfig();
    ParseCommandLine(argc, argv);

    testapp::EnableGameDpiAwareness();
    testapp::ApplyGameScheduling();
    testapp::OpenLogFile();
    EnableDredIfRequested();  // must run before any D3D12 device is created
    testapp::Log("DX12 FG Switch Test App\n");
    testapp::Log("=======================\n");
    testapp::Log("Resolution: %dx%d\n", dx12_fg_switch_test_g_WindowWidth, dx12_fg_switch_test_g_WindowHeight);
    testapp::Log("Upscaling: enabled=%d quality=%s scaleOverride=%d%% dlssPreset=%c fsrVersion=%d sharpening=%d\n",
                 dx12_fg_switch_test_g_UpscalingEnabled ? 1 : 0, testapp::fg::UpscaleQualityName(dx12_fg_switch_test_g_UpscaleQuality), dx12_fg_switch_test_g_UpscaleScalePercent,
                 dx12_fg_switch_test_g_DlssPresetConfig ? dx12_fg_switch_test_g_DlssPresetConfig : '-', dx12_fg_switch_test_g_FsrUpscaleVersionConfig,
                 dx12_fg_switch_test_g_FsrSharpeningEnabled ? 1 : 0);
    testapp::Log("GPU Load Passes: %d\n", dx12_fg_switch_test_g_GpuLoadPasses);
    testapp::Log("Back Buffers (requested): %d\n", dx12_fg_switch_test_kRequestedBackBuffers);
    testapp::Log("Process ID: %lu\n", GetCurrentProcessId());
    testapp::Log("Auto: OFF -> FSR at %ds, suspend/resume FSR every %ds, DLSS at %ds, FSR at %ds\n",
                 dx12_fg_switch_test_g_AutoFsrStartSeconds, dx12_fg_switch_test_g_FsrSuspendResumeIntervalSeconds, dx12_fg_switch_test_g_AutoDlssStartSeconds,
                 dx12_fg_switch_test_g_AutoReturnFsrSeconds);
    testapp::Log("Keys: 1=OFF 2=DLSS FG 3=FSR FG ESC=exit\n\n");
    testapp::Log(
        "Stress: FSR active mode re-sends ffxConfigure every frame to mimic engines that refresh FG descriptors\n");
    testapp::Log("Stress: DXGI video-memory query bursts during FSR mode = %d (%d queries/frame)\n",
                 dx12_fg_switch_test_g_DxgiVideoMemoryQueryStress ? 1 : 0, dx12_fg_switch_test_g_DxgiVideoMemoryQueryCountPerFrame);
    testapp::Log("Stress: FSR runtime unload/reload on mode switches = %d\n", dx12_fg_switch_test_g_FsrReloadRuntimeOnSwitch ? 1 : 0);
    testapp::Log("Stress: Preload Streamline during initial OFF = %d\n", dx12_fg_switch_test_g_StreamlinePreloadInitialOff ? 1 : 0);
    testapp::Log("Stress: Keep FSR runtime loaded during initial OFF = %d\n", dx12_fg_switch_test_g_FsrKeepRuntimeLoadedInitialOff ? 1 : 0);
    testapp::Log("Stress: Create disabled FSR context during initial OFF = %d\n",
                 dx12_fg_switch_test_g_FsrStartupDisabledContextStress ? 1 : 0);
    testapp::Log("Stress: FSR suspend/resume while keeping context/swapchain alive = %d\n",
                 dx12_fg_switch_test_g_FsrSuspendResumeStress ? 1 : 0);
    testapp::Log("Stress: FSR present callback alternates with AMD internal no-callback route = %d (%ds)\n\n",
                 dx12_fg_switch_test_g_FsrPresentCallbackStress ? 1 : 0, dx12_fg_switch_test_g_FsrPresentCallbackToggleIntervalSeconds);
    testapp::Log("Stress: Isolated native swapchain wrapper probes before/after session = %d\n",
                 dx12_fg_switch_test_g_BootstrapNativeSwapchainStressCount);
    testapp::Log("Stress: Startup native swapchain recreates before FG runtimes load = %d\n",
                 dx12_fg_switch_test_g_StartupNativeSwapchainRecreateCount);
    testapp::Log("Stress: Async FG runtime preload after visible startup = %d\n", dx12_fg_switch_test_g_AsyncRuntimePreload ? 1 : 0);
    testapp::Log("Stress: DLSS one-shot suspend-and-hold = %d, off-after-active hold = %d (interval %ds)\n",
                 dx12_fg_switch_test_g_DlssSuspendResumeStress ? 1 : 0, dx12_fg_switch_test_g_DlssOffAfterActiveStress ? 1 : 0,
                 dx12_fg_switch_test_g_DlssSuspendResumeIntervalSeconds);
    testapp::Log("Stress: Auto exit seconds = %d\n\n", dx12_fg_switch_test_g_AutoExitSeconds);
    testapp::LogFlush();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    ATOM classAtom = RegisterClassExW(&wc);
    DWORD classError = classAtom ? ERROR_SUCCESS : GetLastError();
    testapp::Log("[FG-DIAG] RegisterClassEx main window atom=%u gle=%lu\n", static_cast<unsigned>(classAtom),
                 classError);

    const int configuredWindowWidth = dx12_fg_switch_test_g_WindowWidth;
    const int configuredWindowHeight = dx12_fg_switch_test_g_WindowHeight;
    RECT monitorRect = testapp::GetPrimaryMonitorRect();
    if (dx12_fg_switch_test_g_Fullscreen) {
        dx12_fg_switch_test_g_WindowWidth = monitorRect.right - monitorRect.left;
        dx12_fg_switch_test_g_WindowHeight = monitorRect.bottom - monitorRect.top;
    }
    testapp::Log(
        "[FG-DIAG] Display resolved: configured=%dx%d actual=%dx%d fullscreen=%d monitor=(%ld,%ld)-(%ld,%ld)\n",
        configuredWindowWidth, configuredWindowHeight, dx12_fg_switch_test_g_WindowWidth, dx12_fg_switch_test_g_WindowHeight, dx12_fg_switch_test_g_Fullscreen, monitorRect.left,
        monitorRect.top, monitorRect.right, monitorRect.bottom);
    UpdateRenderResolution();
    DWORD style = dx12_fg_switch_test_g_Fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    RECT rc = testapp::AdjustWindowRectForClientSize(style, 0, dx12_fg_switch_test_g_WindowWidth, dx12_fg_switch_test_g_WindowHeight);
    dx12_fg_switch_test_g_Hwnd = CreateWindowW(kWindowClass, L"DX12 FG Switch Test", style, dx12_fg_switch_test_g_Fullscreen ? monitorRect.left : 0,
                           dx12_fg_switch_test_g_Fullscreen ? monitorRect.top : 0, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr,
                           wc.hInstance, nullptr);
    if (!dx12_fg_switch_test_g_Hwnd) {
        testapp::Log("[FG-DIAG] CreateWindowW main window failed gle=%lu\n", GetLastError());
    }
    const bool runEarlyNativeStartupStress = dx12_fg_switch_test_g_StartupNativeSwapchainRecreateCount > 0;
    if (runEarlyNativeStartupStress) {
        if (!testapp::PrimeWindowForBenchmark(dx12_fg_switch_test_g_Hwnd, dx12_fg_switch_test_g_Fullscreen != 0, dx12_fg_switch_test_g_WindowWidth, dx12_fg_switch_test_g_WindowHeight, 0)) {
            testapp::Log("[FG-DIAG] Initial PrimeWindowForBenchmark failed hwnd=%p isWindow=%d\n", dx12_fg_switch_test_g_Hwnd,
                         dx12_fg_switch_test_g_Hwnd ? (IsWindow(dx12_fg_switch_test_g_Hwnd) ? 1 : 0) : 0);
            testapp::CloseLogFile();
            return 0;
        }
        if (!InitDX12(dx12_fg_switch_test_g_Hwnd)) {
            testapp::Log("Failed to initialize early native DX12 bootstrap renderer\n");
            Cleanup();
            testapp::CloseLogFile();
            return 1;
        }
        if (!testapp::PrimeWindowForBenchmark(dx12_fg_switch_test_g_Hwnd, dx12_fg_switch_test_g_Fullscreen != 0, dx12_fg_switch_test_g_WindowWidth, dx12_fg_switch_test_g_WindowHeight, 0)) {
            testapp::Log("[FG-DIAG] Post-DX12 PrimeWindowForBenchmark failed hwnd=%p isWindow=%d\n", dx12_fg_switch_test_g_Hwnd,
                         dx12_fg_switch_test_g_Hwnd ? (IsWindow(dx12_fg_switch_test_g_Hwnd) ? 1 : 0) : 0);
            Cleanup();
            testapp::CloseLogFile();
            return 0;
        }
        testapp::Log("[FG-DIAG] Early native OFF swapchain stress path active; presenting first OFF frames\n");
        Render();
        Render();
        WaitForGpu();
        testapp::LogFlush();

        for (int i = 0; i < dx12_fg_switch_test_g_StartupNativeSwapchainRecreateCount; ++i) {
            testapp::Log(
                "[FG-DIAG] Startup native swapchain recreate stress %d/%d while mode remains OFF before FG "
                "runtime preload\n",
                i + 1, dx12_fg_switch_test_g_StartupNativeSwapchainRecreateCount);
            if (!RecreateSwapChain(false, "startup native recreate stress")) {
                testapp::Log("[FG-DIAG] Startup native swapchain recreate stress failed\n");
                Cleanup();
                testapp::CloseLogFile();
                return 1;
            }
            WaitForGpu();
        }

        testapp::Log(
            "[FG-DIAG] Destroying early native bootstrap DX12 renderer before loading FG runtimes; final renderer will "
            "be created through the selected runtime path\n");
        Cleanup();
        dx12_fg_switch_test_g_CurrentMode = FGMode::Off;
        dx12_fg_switch_test_g_PendingMode = FGMode::Off;
        dx12_fg_switch_test_g_ModeSwitchPending = false;
        dx12_fg_switch_test_g_SuspensionTogglePending = false;
        dx12_fg_switch_test_g_ManualMode = false;
        dx12_fg_switch_test_g_SwapChainOwner = SwapChainOwner::Native;
    } else {
        testapp::Log(
            "[FG-DIAG] Early native startup stress disabled; window will be shown after final renderer init\n");
    }

    bool streamlineLoaded = false;
    if (dx12_fg_switch_test_g_StreamlinePreloadInitialOff) {
        testapp::Log("Loading Streamline before final DXGI/D3D12 during initial OFF preload...\n");
        streamlineLoaded = LoadStreamlineAndInitSerialized("initial OFF preload");
        if (!streamlineLoaded) {
            testapp::Log("[FG-DIAG] Streamline runtime unavailable; DLSS mode will load on demand if possible\n");
        }
    } else {
        testapp::Log("[FG-DIAG] Streamline initial OFF preload disabled; DLSS mode will load it on demand\n");
    }
    if (dx12_fg_switch_test_g_FsrKeepRuntimeLoadedInitialOff || dx12_fg_switch_test_g_FsrStartupDisabledContextStress) {
        testapp::Log("Loading FSR runtime during initial OFF preload...\n");
        bool fsrRuntimeLoaded = LoadFSRRuntimeSerialized("initial OFF preload");
        if (!fsrRuntimeLoaded) {
            testapp::Log("[FG-DIAG] FSR runtime unavailable; FSR mode will retry on demand\n");
        } else if (dx12_fg_switch_test_g_FsrReloadRuntimeOnSwitch && !dx12_fg_switch_test_g_FsrKeepRuntimeLoadedInitialOff &&
                   !dx12_fg_switch_test_g_FsrStartupDisabledContextStress) {
            UnloadFSRRuntimeSerialized("startup stress before first FSR enable");
        } else if (dx12_fg_switch_test_g_FsrReloadRuntimeOnSwitch) {
            testapp::Log(
                "[FG-DIAG] Keeping FSR runtime loaded during initial OFF to mimic games that preload FFX "
                "beside Streamline before FG is enabled\n");
        }
    } else {
        testapp::Log("[FG-DIAG] FSR initial OFF preload disabled; FSR mode will load it on demand\n");
    }
    testapp::LogFlush();

    if (!InitDX12(dx12_fg_switch_test_g_Hwnd)) {
        testapp::Log("Failed to initialize final DX12 renderer after FG runtime preload\n");
        Cleanup();
        testapp::CloseLogFile();
        return 1;
    }
    if (!testapp::PrimeWindowForBenchmark(dx12_fg_switch_test_g_Hwnd, dx12_fg_switch_test_g_Fullscreen != 0, dx12_fg_switch_test_g_WindowWidth, dx12_fg_switch_test_g_WindowHeight, 0)) {
        testapp::Log("[FG-DIAG] Final PrimeWindowForBenchmark failed hwnd=%p isWindow=%d\n", dx12_fg_switch_test_g_Hwnd,
                     dx12_fg_switch_test_g_Hwnd ? (IsWindow(dx12_fg_switch_test_g_Hwnd) ? 1 : 0) : 0);
        Cleanup();
        testapp::CloseLogFile();
        return 0;
    }
    if (!dx12_fg_switch_test_g_FsrRuntimeLoaded && !dx12_fg_switch_test_g_FsrStartupDisabledContextStress) {
        StartAsyncFSRRuntimePreload("initial visible OFF phase");
    }

    testapp::Log("[FG-DIAG] FSR runtime state=%d (context will be created when FSR mode is selected)\n",
                 dx12_fg_switch_test_g_FsrRuntimeLoaded ? 1 : 0);
    if (dx12_fg_switch_test_g_FsrStartupDisabledContextStress && dx12_fg_switch_test_g_FsrRuntimeLoaded && dx12_fg_switch_test_g_FfxCreateContext && !dx12_fg_switch_test_g_FfxCtx) {
        testapp::Log(
            "[FG-DIAG] Creating startup disabled FSR context on native swapchain while app mode remains OFF\n");
        dx12_fg_switch_test_g_FsrInitialized = TryInitFSR();
        testapp::Log("[FG-DIAG] Startup disabled FSR context state=%d ctx=%p owner=%s\n", dx12_fg_switch_test_g_FsrInitialized ? 1 : 0,
                     (void*)dx12_fg_switch_test_g_FfxCtx, SwapChainOwnerName(dx12_fg_switch_test_g_SwapChainOwner));
    }
    dx12_fg_switch_test_g_DlssInitialized = streamlineLoaded && TryInitDLSSFG();
    testapp::Log("[FG-DIAG] DLSS init state=%d\n", dx12_fg_switch_test_g_DlssInitialized ? 1 : 0);
    if (dx12_fg_switch_test_g_DlssInitialized) {
        SetDLSSFGMode(false);
    }
    RunBootstrapNativeSwapchainStress();
    UpdateWindowTitle();
    dx12_fg_switch_test_g_StartTime = std::chrono::high_resolution_clock::now();
    dx12_fg_switch_test_g_LastFsrSuspendResumeToggleTime = dx12_fg_switch_test_g_StartTime;
    dx12_fg_switch_test_g_LastDlssSuspendResumeToggleTime = dx12_fg_switch_test_g_StartTime;
    dx12_fg_switch_test_g_FsrPresentCallbackStressStartTime = dx12_fg_switch_test_g_StartTime;
    dx12_fg_switch_test_g_FramePacingInitialized = false;
    dx12_fg_switch_test_g_MaxFrameDeltaMs = 0.0;
    dx12_fg_switch_test_g_FramePacingSpikeCount = 0;
    dx12_fg_switch_test_g_ModeSwitchingArmed = true;
    testapp::Log(
        "[FG-DIAG] Auto sequence clock reset after startup initialization; visible OFF phase now begins "
        "(next auto FSR at %ds, mode switching armed)\n",
        dx12_fg_switch_test_g_AutoFsrStartSeconds);
    testapp::LogFlush();

    MSG msg = {};
    while (dx12_fg_switch_test_g_Running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (dx12_fg_switch_test_g_Running) {
            Render();
        }
    }
    Cleanup();
    RunBootstrapNativeSwapchainStress();
    testapp::Log("[FG-DIAG] Frame pacing summary: maxDeltaMs=%.2f spikes=%llu\n", dx12_fg_switch_test_g_MaxFrameDeltaMs,
                 static_cast<unsigned long long>(dx12_fg_switch_test_g_FramePacingSpikeCount));
    testapp::Log("Exiting (total frames rendered: %llu)\n", static_cast<unsigned long long>(dx12_fg_switch_test_g_FrameIdCounter));
    testapp::CloseLogFile();
    return dx12_fg_switch_test_g_ProcessExitCode;
}
