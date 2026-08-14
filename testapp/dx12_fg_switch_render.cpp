#include "dx12_fg_switch_test_internal.h"

bool LoadStreamlineAndInitSerialized(const char* reason) {
    if (dx12_fg_switch_test_g_SlInitialized) {
        return true;
    }
    std::lock_guard<std::mutex> lock(dx12_fg_switch_test_g_RuntimeLoadMutex);
    if (dx12_fg_switch_test_g_SlInitialized) {
        return true;
    }
    testapp::Log("[FG-DIAG] Streamline serialized load begin reason=%s\n", reason ? reason : "unknown");
    const auto begin = std::chrono::high_resolution_clock::now();
    bool loaded = LoadStreamlineAndInit();
    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    testapp::Log("[FG-DIAG] Streamline serialized load end reason=%s ok=%d elapsedMs=%lld\n",
                 reason ? reason : "unknown", loaded ? 1 : 0, static_cast<long long>(elapsedMs));
    return loaded;
}

bool LoadFSRRuntimeSerialized(const char* reason) {
    if (dx12_fg_switch_test_g_FsrRuntimeLoaded && dx12_fg_switch_test_g_FfxModule && dx12_fg_switch_test_g_FfxCreateContext && dx12_fg_switch_test_g_FfxConfigure && dx12_fg_switch_test_g_FfxDispatch &&
        dx12_fg_switch_test_g_FfxDestroyContext) {
        return true;
    }
    std::lock_guard<std::mutex> lock(dx12_fg_switch_test_g_RuntimeLoadMutex);
    if (dx12_fg_switch_test_g_FsrRuntimeLoaded && dx12_fg_switch_test_g_FfxModule && dx12_fg_switch_test_g_FfxCreateContext && dx12_fg_switch_test_g_FfxConfigure && dx12_fg_switch_test_g_FfxDispatch &&
        dx12_fg_switch_test_g_FfxDestroyContext) {
        return true;
    }
    testapp::Log("[FG-DIAG] FSR runtime serialized load begin reason=%s\n", reason ? reason : "unknown");
    const auto begin = std::chrono::high_resolution_clock::now();
    bool loaded = LoadFSRRuntime();
    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    testapp::Log("[FG-DIAG] FSR runtime serialized load end reason=%s ok=%d elapsedMs=%lld\n",
                 reason ? reason : "unknown", loaded ? 1 : 0, static_cast<long long>(elapsedMs));
    return loaded;
}

void ShutdownStreamlineSerialized(const char* reason) {
    std::lock_guard<std::mutex> lock(dx12_fg_switch_test_g_RuntimeLoadMutex);
    testapp::Log("[FG-DIAG] Streamline serialized shutdown reason=%s initialized=%d module=%p\n",
                 reason ? reason : "unknown", dx12_fg_switch_test_g_SlInitialized ? 1 : 0, dx12_fg_switch_test_g_SlModule);
    ShutdownStreamline();
}

void UnloadFSRRuntimeSerialized(const char* reason) {
    std::lock_guard<std::mutex> lock(dx12_fg_switch_test_g_RuntimeLoadMutex);
    UnloadFSRRuntime(reason);
}

void StartAsyncFSRRuntimePreload(const char* reason) {
    if (dx12_fg_switch_test_g_FsrPreloadThread.joinable() && !dx12_fg_switch_test_g_FsrPreloadInProgress.load()) {
        dx12_fg_switch_test_g_FsrPreloadThread.join();
        dx12_fg_switch_test_g_FsrPreloadStarted = false;
    }
    if (!dx12_fg_switch_test_g_AsyncRuntimePreload || dx12_fg_switch_test_g_FsrRuntimeLoaded || dx12_fg_switch_test_g_FsrPreloadInProgress.load() ||
        dx12_fg_switch_test_g_FsrPreloadStarted.exchange(true)) {
        return;
    }
    std::string reasonText = reason ? reason : "unknown";
    dx12_fg_switch_test_g_FsrPreloadInProgress = true;
    dx12_fg_switch_test_g_FsrPreloadSucceeded = false;
    testapp::Log("[FG-DIAG] Async FSR runtime preload scheduled reason=%s\n", reasonText.c_str());
    dx12_fg_switch_test_g_FsrPreloadThread = std::thread([reasonText]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        bool loaded = LoadFSRRuntimeSerialized(reasonText.c_str());
        dx12_fg_switch_test_g_FsrPreloadSucceeded = loaded;
        dx12_fg_switch_test_g_FsrPreloadInProgress = false;
        testapp::Log("[FG-DIAG] Async FSR runtime preload finished reason=%s ok=%d\n", reasonText.c_str(),
                     loaded ? 1 : 0);
        testapp::LogFlush();
    });
}

void StartAsyncStreamlinePreload(const char* reason) {
    if (dx12_fg_switch_test_g_StreamlinePreloadThread.joinable() && !dx12_fg_switch_test_g_StreamlinePreloadInProgress.load()) {
        dx12_fg_switch_test_g_StreamlinePreloadThread.join();
        dx12_fg_switch_test_g_StreamlinePreloadStarted = false;
    }
    const bool fsrOwnsPresentation =
        dx12_fg_switch_test_g_FsrEnabled || dx12_fg_switch_test_g_FsrInitialized || dx12_fg_switch_test_g_FfxCtx || dx12_fg_switch_test_g_FfxSwapChainCtx || dx12_fg_switch_test_g_SwapChainOwner == SwapChainOwner::FSR;
    if (fsrOwnsPresentation) {
        static std::atomic<uint64_t> s_skipLogCount{0};
        const uint64_t skipLogCount = s_skipLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (skipLogCount <= 12 || (skipLogCount % 60) == 0) {
            testapp::Log(
                "[FG-DIAG] Async Streamline preload skipped during active/native FSR ownership reason=%s "
                "fsrEnabled=%d fsrInitialized=%d fgCtx=%p swapchainCtx=%p owner=%s log=%llu\n",
                reason ? reason : "unknown", dx12_fg_switch_test_g_FsrEnabled ? 1 : 0, dx12_fg_switch_test_g_FsrInitialized ? 1 : 0, (void*)dx12_fg_switch_test_g_FfxCtx,
                (void*)dx12_fg_switch_test_g_FfxSwapChainCtx, SwapChainOwnerName(dx12_fg_switch_test_g_SwapChainOwner),
                static_cast<unsigned long long>(skipLogCount));
        }
        return;
    }
    if (!dx12_fg_switch_test_g_AsyncRuntimePreload || dx12_fg_switch_test_g_SlInitialized || dx12_fg_switch_test_g_SlModule || dx12_fg_switch_test_g_StreamlinePreloadInProgress.load() ||
        dx12_fg_switch_test_g_StreamlinePreloadStarted.exchange(true)) {
        return;
    }
    std::string reasonText = reason ? reason : "unknown";
    dx12_fg_switch_test_g_StreamlinePreloadInProgress = true;
    dx12_fg_switch_test_g_StreamlinePreloadSucceeded = false;
    testapp::Log("[FG-DIAG] Async Streamline preload scheduled reason=%s\n", reasonText.c_str());
    dx12_fg_switch_test_g_StreamlinePreloadThread = std::thread([reasonText]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        bool loaded = LoadStreamlineAndInitSerialized(reasonText.c_str());
        dx12_fg_switch_test_g_StreamlinePreloadSucceeded = loaded;
        dx12_fg_switch_test_g_StreamlinePreloadInProgress = false;
        testapp::Log("[FG-DIAG] Async Streamline preload finished reason=%s ok=%d\n", reasonText.c_str(),
                     loaded ? 1 : 0);
        testapp::LogFlush();
    });
}

void JoinAsyncRuntimePreloadThreads(const char* reason) {
    if (dx12_fg_switch_test_g_FsrPreloadThread.joinable()) {
        testapp::Log("[FG-DIAG] Joining async FSR runtime preload reason=%s inProgress=%d succeeded=%d\n",
                     reason ? reason : "unknown", dx12_fg_switch_test_g_FsrPreloadInProgress.load() ? 1 : 0,
                     dx12_fg_switch_test_g_FsrPreloadSucceeded.load() ? 1 : 0);
        dx12_fg_switch_test_g_FsrPreloadThread.join();
    }
    if (dx12_fg_switch_test_g_StreamlinePreloadThread.joinable()) {
        testapp::Log("[FG-DIAG] Joining async Streamline preload reason=%s inProgress=%d succeeded=%d\n",
                     reason ? reason : "unknown", dx12_fg_switch_test_g_StreamlinePreloadInProgress.load() ? 1 : 0,
                     dx12_fg_switch_test_g_StreamlinePreloadSucceeded.load() ? 1 : 0);
        dx12_fg_switch_test_g_StreamlinePreloadThread.join();
    }
}

void ReleaseDX12RendererResourcesForSwitch(const char* reason) {
    if (g_Device) {
        testapp::Log("[FG-DIAG] Releasing DX12 renderer resources for runtime switch (%s) owner=%s streamline=%d\n",
                     reason ? reason : "runtime switch", SwapChainOwnerName(dx12_fg_switch_test_g_SwapChainOwner),
                     dx12_fg_switch_test_g_SwapChainUsesStreamline ? 1 : 0);
        WaitForGpu();
    }
    // Device-bound upscaling state must die with the renderer (idempotent if already gone).
    DestroyFSRUpscaleContext();
    dx12_fg_switch_test_g_Taa.Release();
    dx12_fg_switch_test_g_PresentBlit.Release();
    testapp::dx12fg::ReleaseAuxiliaryResources(g_FgInputs);
    // Degenerate-UI placeholder is device-bound; release it with the renderer so it is recreated against the
    // new device on the next registration (preserves the multi-switch FSR->DLSS->FSR coverage).
    dx12_fg_switch_test_g_FsrDegenerateUiTexture.Reset();
    g_Scene.Release();
    for (auto& renderTarget : g_RenderTargets) {
        renderTarget.Reset();
    }
    for (auto& allocator : g_CommandAllocators) {
        allocator.Reset();
    }
    g_CommandList.Reset();
    g_RtvHeap.Reset();
    g_SwapChain.Reset();
    dx12_fg_switch_test_g_SwapChainUsesStreamline = false;
    g_FrameLatencyWaitHandle = nullptr;
    g_DxgiVideoMemoryQueryAdapter.Reset();
    g_CommandQueue.Reset();
    g_Fence.Reset();
    g_Device.Reset();
    if (g_FenceEvent) {
        CloseHandle(g_FenceEvent);
        g_FenceEvent = nullptr;
    }
    g_FrameIndex = 0;
    g_SwapChainBufferCount = dx12_fg_switch_test_kRequestedBackBuffers;
    for (UINT i = 0; i < dx12_fg_switch_test_kMaxSwapChainBuffers; ++i) {
        g_FenceValues[i] = 0;
    }
}

bool ReinitializeDX12ForFSR(const char* reason) {
    ReleaseDX12RendererResourcesForSwitch(reason);
    if (dx12_fg_switch_test_g_SlInitialized || dx12_fg_switch_test_g_SlModule) {
        testapp::Log("[FG-DIAG] Shutting down Streamline before creating FSR renderer (%s)\n",
                     reason ? reason : "enter FSR");
        ShutdownStreamlineSerialized(reason ? reason : "enter FSR");
    }
    return InitDX12(dx12_fg_switch_test_g_Hwnd, true, reason ? reason : "enter FSR mode");
}

// Leaving DLSS mode for OFF must fully tear down the Streamline proxy swapchain and recreate a
// truly native one: keeping the proxy would keep its present pacer alive, and Reflex must stay on
// while that pacer presents (turning it off wedges the GPU; see dx12_fg_switch_streamline.inl).
// Only the teardown lets Reflex genuinely turn off, removing its frame cap in OFF mode -- exactly
// like a real game that rebuilds presentation when FG is switched off for good.
bool ReinitializeDX12ForNativeOff(const char* reason) {
    ReleaseDX12RendererResourcesForSwitch(reason);
    if (dx12_fg_switch_test_g_SlInitialized || dx12_fg_switch_test_g_SlModule) {
        ShutdownStreamlineSerialized(reason ? reason : "enter OFF mode");
    }
    return InitDX12(dx12_fg_switch_test_g_Hwnd, false, reason ? reason : "enter OFF mode");
}

bool EnsureStreamlineReadyForDLSS(const char* reason) {
    if (!dx12_fg_switch_test_g_SlInitialized) {
        testapp::Log("[FG-DIAG] Loading Streamline on demand for DLSS mode (%s)\n", reason ? reason : "unknown");
        if (!LoadStreamlineAndInitSerialized(reason ? reason : "DLSS mode")) {
            testapp::Log("[FG-DIAG] Streamline load failed for DLSS mode (%s)\n", reason ? reason : "unknown");
            return false;
        }
    }
    if (!dx12_fg_switch_test_g_SlDeviceSet && dx12_fg_switch_test_g_SlSetD3DDevice && g_Device) {
        sl::Result deviceResult = dx12_fg_switch_test_g_SlSetD3DDevice(g_Device.Get());
        dx12_fg_switch_test_g_SlDeviceSet = deviceResult == sl::Result::eOk;
        testapp::Log("[FG-DIAG] slSetD3DDevice(%s) result=%d (%s)\n", reason ? reason : "DLSS mode",
                     static_cast<int>(deviceResult), SlResultName(deviceResult));
    }
    if (!dx12_fg_switch_test_g_SlDeviceSet) {
        testapp::Log("[FG-DIAG] Streamline device binding unavailable for DLSS mode (%s) device=%p setFn=%p\n",
                     reason ? reason : "unknown", g_Device.Get(), (void*)dx12_fg_switch_test_g_SlSetD3DDevice);
        return false;
    }
    if (!dx12_fg_switch_test_g_DlssInitialized) {
        dx12_fg_switch_test_g_DlssInitialized = TryInitDLSSFG();
        testapp::Log("[FG-DIAG] DLSS init on demand (%s) state=%d\n", reason ? reason : "DLSS mode",
                     dx12_fg_switch_test_g_DlssInitialized ? 1 : 0);
    }
    return dx12_fg_switch_test_g_DlssInitialized;
}

bool ToggleCurrentFGSuspension(FGMode mode, const char* reason, UINT frameIndex) {
    dx12_fg_switch_test_g_SuspensionTogglePending = false;
    if (mode != dx12_fg_switch_test_g_CurrentMode) {
        testapp::Log("[FG-DIAG] Ignoring suspension toggle for %s while current mode is %s (%s)\n", ModeName(mode),
                     ModeName(dx12_fg_switch_test_g_CurrentMode), reason ? reason : "unknown");
        testapp::LogFlush();
        return false;
    }

    bool ok = false;
    if (mode == FGMode::FSR) {
        if (!dx12_fg_switch_test_g_FfxCtx || !dx12_fg_switch_test_g_FsrInitialized) {
            testapp::Log("[FG-DIAG] Cannot toggle FSR suspension: ctx=%p initialized=%d enabled=%d suspended=%d\n",
                         (void*)dx12_fg_switch_test_g_FfxCtx, dx12_fg_switch_test_g_FsrInitialized ? 1 : 0, dx12_fg_switch_test_g_FsrEnabled ? 1 : 0, dx12_fg_switch_test_g_FsrSuspended ? 1 : 0);
            testapp::LogFlush();
            return false;
        }

        const bool enable = dx12_fg_switch_test_g_FsrSuspended || !dx12_fg_switch_test_g_FsrEnabled;
        ID3D12Resource* backbuffer = frameIndex < g_SwapChainBufferCount ? g_RenderTargets[frameIndex].Get() : nullptr;
        ok = ConfigureFSR(enable, backbuffer, enable ? "manual resume FSR FG" : "manual suspend FSR FG", true);
        if (ok) {
            dx12_fg_switch_test_g_FsrEnabled = true;
            dx12_fg_switch_test_g_FsrSuspended = !enable;
            dx12_fg_switch_test_g_LastFsrSuspendResumeToggleTime = std::chrono::high_resolution_clock::now();
            dx12_fg_switch_test_g_LastFsrSuspendResumeToggleFrameId = dx12_fg_switch_test_g_FrameIdCounter;
            if (enable) {
                RegisterFSRUiResource();
            }
        }
        testapp::Log("[FG-DIAG] Manual FSR suspension toggle: %s ok=%d frameID=%llu frameIndex=%u ctx=%p\n",
                     enable ? "resume" : "suspend", ok ? 1 : 0, static_cast<unsigned long long>(dx12_fg_switch_test_g_FrameIdCounter),
                     frameIndex, (void*)dx12_fg_switch_test_g_FfxCtx);
    } else if (mode == FGMode::DLSS) {
        if (!dx12_fg_switch_test_g_DlssInitialized || !dx12_fg_switch_test_g_SlDLSSGSetOptions) {
            testapp::Log(
                "[FG-DIAG] Cannot toggle DLSS suspension: initialized=%d setOptions=%p enabled=%d "
                "suspended=%d\n",
                dx12_fg_switch_test_g_DlssInitialized ? 1 : 0, (void*)dx12_fg_switch_test_g_SlDLSSGSetOptions, dx12_fg_switch_test_g_DlssEnabled ? 1 : 0, dx12_fg_switch_test_g_DlssSuspended ? 1 : 0);
            testapp::LogFlush();
            return false;
        }

        const bool enable = dx12_fg_switch_test_g_DlssSuspended || !dx12_fg_switch_test_g_DlssEnabled;
        ok = SetDLSSFGMode(enable);
        if (ok) {
            dx12_fg_switch_test_g_DlssEnabled = enable;
            dx12_fg_switch_test_g_DlssSuspended = !enable;
            if (enable) {
                PollDLSSFGState();
            }
        }
        testapp::Log("[FG-DIAG] Manual DLSS suspension toggle: %s ok=%d frameID=%llu frameIndex=%u\n",
                     enable ? "resume" : "suspend", ok ? 1 : 0, static_cast<unsigned long long>(dx12_fg_switch_test_g_FrameIdCounter),
                     frameIndex);
    }

    dx12_fg_switch_test_g_LastModeSwitchFrameId = dx12_fg_switch_test_g_FrameIdCounter;
    testapp::Log(
        "[FG-DIAG] Suspension toggle state: mode=%s ok=%d fsr=%d fsrSuspended=%d dlss=%d "
        "dlssSuspended=%d\n",
        ModeName(dx12_fg_switch_test_g_CurrentMode), ok ? 1 : 0, dx12_fg_switch_test_g_FsrEnabled ? 1 : 0, dx12_fg_switch_test_g_FsrSuspended ? 1 : 0, dx12_fg_switch_test_g_DlssEnabled ? 1 : 0,
        dx12_fg_switch_test_g_DlssSuspended ? 1 : 0);
    UpdateWindowTitle();
    testapp::LogFlush();
    return ok;
}

void RunAutoSequence(float elapsedSeconds) {
    if (dx12_fg_switch_test_g_ManualMode) {
        return;
    }
    if (dx12_fg_switch_test_g_AutoStage == 0 && elapsedSeconds >= static_cast<float>(dx12_fg_switch_test_g_AutoFsrStartSeconds)) {
        dx12_fg_switch_test_g_AutoStage = 1;
        RequestMode(FGMode::FSR, "auto FSR stage", false);
    } else if (dx12_fg_switch_test_g_AutoStage == 1 && elapsedSeconds >= static_cast<float>(dx12_fg_switch_test_g_AutoDlssStartSeconds)) {
        dx12_fg_switch_test_g_AutoStage = 2;
        RequestMode(FGMode::DLSS, "auto DLSS stage after FSR suspend/resume", false);
    } else if (dx12_fg_switch_test_g_AutoStage == 2 && elapsedSeconds >= static_cast<float>(dx12_fg_switch_test_g_AutoReturnFsrSeconds)) {
        dx12_fg_switch_test_g_AutoStage = 3;
        RequestMode(FGMode::FSR, "auto return to FSR stage", false);
    }
}

// Auto one-shot DLSS-FG stress stages (off by default). Once DLSS FG ran for the configured
// interval: --dlss-suspend-stress suspends DLSS-G once and HOLDS it; --dlss-off-stress switches
// DLSS->OFF once and holds. With BOTH flags the stages chain (suspend at T, OFF after another
// interval), reproducing the full manual scenario "FG active -> suspend -> hold -> OFF" without key
// presses. A suspend/resume *cycle* does NOT reproduce the pacer failure class: each switch resets
// Streamline's present-pacer, so those regressions only show up when the app HOLDS the post-FG
// state (the GPU device hung after ~hundreds of suspended frames when the proxy swapchain was
// presented with SyncInterval=1, and within ~100 frames when Reflex was switched off under the
// suspended proxy's live pacer).
void MaybeToggleDLSSSuspensionStress() {
    if ((!dx12_fg_switch_test_g_DlssSuspendResumeStress && !dx12_fg_switch_test_g_DlssOffAfterActiveStress) || dx12_fg_switch_test_g_ManualMode || dx12_fg_switch_test_g_ModeSwitchPending ||
        dx12_fg_switch_test_g_CurrentMode != FGMode::DLSS || !dx12_fg_switch_test_g_DlssInitialized || !dx12_fg_switch_test_g_SlDLSSGSetOptions) {
        return;
    }
    const bool suspendPending = dx12_fg_switch_test_g_DlssSuspendResumeStress && !dx12_fg_switch_test_g_DlssStressDidSuspend;
    const bool offPending = dx12_fg_switch_test_g_DlssOffAfterActiveStress && !dx12_fg_switch_test_g_DlssStressDidRequestOff && !suspendPending;
    if (!suspendPending && !offPending) {
        return;
    }
    const auto now = std::chrono::high_resolution_clock::now();
    const float elapsed = std::chrono::duration<float>(now - dx12_fg_switch_test_g_LastDlssSuspendResumeToggleTime).count();
    if (elapsed < static_cast<float>(dx12_fg_switch_test_g_DlssSuspendResumeIntervalSeconds)) {
        return;
    }
    if (offPending) {
        dx12_fg_switch_test_g_DlssStressDidRequestOff = true;
        RequestMode(FGMode::Off, "dlss off-after-active stress", false);
        return;
    }
    const bool ok = SetDLSSFGMode(false);
    if (ok) {
        dx12_fg_switch_test_g_DlssEnabled = false;
        dx12_fg_switch_test_g_DlssSuspended = true;
        dx12_fg_switch_test_g_DlssStressDidSuspend = true;
        // Re-anchor so a chained --dlss-off-stress stage holds the suspended state for a full
        // interval before switching OFF.
        dx12_fg_switch_test_g_LastDlssSuspendResumeToggleTime = now;
    }
    testapp::Log("[FG-DIAG] DLSS suspend-and-hold stress: %s frameID=%llu\n",
                 ok ? "suspended FG (holding suspended)" : "suspend FAILED (state unchanged)",
                 static_cast<unsigned long long>(dx12_fg_switch_test_g_FrameIdCounter));
    testapp::LogFlush();
    UpdateWindowTitle();
}

// Renders the frame-generation scene inputs: a real 3D cube into the hud-less color +
// motion-vector targets (so FG interpolates the moving cube to the output/generated rate), and
// the HUD + status into the UI-layer texture. The UI layer is registered with FSR / tagged for
// DLSS, so it is composited crisp on every real and generated frame at the base rate (no
// ghosting). The static camera/floor/sky carry zero motion, which is correct.
void RenderSwitchSceneInputs(float elapsedSeconds, LONG hudX, LONG hudY) {
    testapp::dx12fg::AuxiliaryResources& aux = g_FgInputs;
    if (!aux.valid || !g_CommandList) {
        return;
    }

    // With upscaling the jittered scene renders into the render-res sceneColor and the upscale
    // stage produces the display-res hudlessColor; without upscaling the scene renders straight
    // into hudlessColor (render == display), reproducing the legacy pipeline exactly.
    const bool upscaling = UpscalingActive();
    ID3D12Resource* sceneTarget = upscaling ? aux.sceneColor.Get() : aux.hudlessColor.Get();
    D3D12_RESOURCE_STATES& sceneTargetState = upscaling ? aux.sceneState : aux.hudlessState;
    const D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv = upscaling ? aux.SceneRtv() : aux.HudlessRtv();

    testapp::dx12fg::Transition(g_CommandList.Get(), sceneTarget, sceneTargetState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    testapp::dx12fg::Transition(g_CommandList.Get(), aux.uiColor.Get(), aux.uiState,
                                D3D12_RESOURCE_STATE_RENDER_TARGET);
    testapp::dx12fg::Transition(g_CommandList.Get(), aux.motionVectors.Get(), aux.motionState,
                                D3D12_RESOURCE_STATE_RENDER_TARGET);
    testapp::dx12fg::Transition(g_CommandList.Get(), aux.depth.Get(), aux.depthState, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    // Clear the FG/upscaler inputs. The scene (sky + floor + cube) fills the scene color; motion
    // starts at zero (static parts stay zero); depth clears to far. The UI layer starts
    // transparent. The reactive/transparency masks are cleared to zero (no reactive content yet --
    // ready for future render elements like particles or animated screens).
    const float clearBlack[] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float uiClear[] = {0.0f, 0.0f, 0.0f, 0.0f};
    const float motionClear[] = {0.0f, 0.0f, 0.0f, 0.0f};
    g_CommandList->ClearRenderTargetView(aux.UiRtv(), uiClear, 0, nullptr);
    g_CommandList->ClearRenderTargetView(aux.MotionRtv(), motionClear, 0, nullptr);
    g_CommandList->ClearDepthStencilView(aux.DepthDsv(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    if (upscaling) {
        const float maskClear[] = {0.0f, 0.0f, 0.0f, 0.0f};
        testapp::dx12fg::Transition(g_CommandList.Get(), aux.reactiveMask.Get(), aux.reactiveState,
                                    D3D12_RESOURCE_STATE_RENDER_TARGET);
        testapp::dx12fg::Transition(g_CommandList.Get(), aux.transparencyMask.Get(), aux.transparencyState,
                                    D3D12_RESOURCE_STATE_RENDER_TARGET);
        g_CommandList->ClearRenderTargetView(aux.ReactiveRtv(), maskClear, 0, nullptr);
        g_CommandList->ClearRenderTargetView(aux.TransparencyRtv(), maskClear, 0, nullptr);
        testapp::dx12fg::Transition(g_CommandList.Get(), aux.reactiveMask.Get(), aux.reactiveState,
                                    testapp::dx12fg::kColorReadState);
        testapp::dx12fg::Transition(g_CommandList.Get(), aux.transparencyMask.Get(), aux.transparencyState,
                                    testapp::dx12fg::kColorReadState);
    }
    // Simulated GPU load: repeated scene-target clears (the scene overwrites every pixel afterwards).
    for (int pass = 0; pass < dx12_fg_switch_test_g_GpuLoadPasses; ++pass) {
        g_CommandList->ClearRenderTargetView(sceneRtv, clearBlack, 0, nullptr);
    }
    g_CommandList->ClearRenderTargetView(sceneRtv, clearBlack, 0, nullptr);

    // Scene: sky + procedural checker floor (spatial depth/fog) + a lit moving cube, rendered at
    // render resolution with the per-frame jittered projection. The cube carries per-pixel motion
    // vectors -> interpolated by FG (output rate); floor/sky are static.
    g_Scene.Render(g_CommandList.Get(), sceneRtv, aux, g_FrameIndex, dx12_fg_switch_test_g_RenderWidth, dx12_fg_switch_test_g_RenderHeight, elapsedSeconds,
                   dx12_fg_switch_test_g_CurrentJitter.x, dx12_fg_switch_test_g_CurrentJitter.y);

    // UI layer: animated "100 HP" HUD + health bar + status, into the registered/tagged UI resource.
    DrawHudOverlay(g_CommandList.Get(), aux.UiRtv(), hudX, hudY);
    DrawStatusText(g_CommandList.Get(), aux.UiRtv());

    testapp::dx12fg::Transition(g_CommandList.Get(), sceneTarget, sceneTargetState, testapp::dx12fg::kColorReadState);
    testapp::dx12fg::Transition(g_CommandList.Get(), aux.uiColor.Get(), aux.uiState, testapp::dx12fg::kColorReadState);
    testapp::dx12fg::Transition(g_CommandList.Get(), aux.motionVectors.Get(), aux.motionState,
                                testapp::dx12fg::kColorReadState);
    testapp::dx12fg::Transition(g_CommandList.Get(), aux.depth.Get(), aux.depthState, testapp::dx12fg::kDepthReadState);
}

void Render() {
    auto now = std::chrono::high_resolution_clock::now();
    if (dx12_fg_switch_test_g_FramePacingInitialized) {
        const double deltaMs = std::chrono::duration<double, std::milli>(now - dx12_fg_switch_test_g_LastFramePacingTime).count();
        dx12_fg_switch_test_g_LastFrameDeltaMs = static_cast<float>(deltaMs);
        if (deltaMs > dx12_fg_switch_test_g_MaxFrameDeltaMs) {
            dx12_fg_switch_test_g_MaxFrameDeltaMs = deltaMs;
        }
        if (deltaMs >= 25.0) {
            ++dx12_fg_switch_test_g_FramePacingSpikeCount;
            if (dx12_fg_switch_test_g_FramePacingSpikeCount <= 12 || (dx12_fg_switch_test_g_FramePacingSpikeCount % 30) == 0) {
                testapp::Log(
                    "[FG-DIAG] Frame pacing spike frameID=%llu mode=%s deltaMs=%.2f "
                    "asyncFSR=%d asyncSL=%d fsr=%d fsrSuspended=%d dlss=%d dlssSuspended=%d\n",
                    static_cast<unsigned long long>(dx12_fg_switch_test_g_FrameIdCounter), ModeName(dx12_fg_switch_test_g_CurrentMode), deltaMs,
                    dx12_fg_switch_test_g_FsrPreloadInProgress.load() ? 1 : 0, dx12_fg_switch_test_g_StreamlinePreloadInProgress.load() ? 1 : 0,
                    dx12_fg_switch_test_g_FsrEnabled ? 1 : 0, dx12_fg_switch_test_g_FsrSuspended ? 1 : 0, dx12_fg_switch_test_g_DlssEnabled ? 1 : 0, dx12_fg_switch_test_g_DlssSuspended ? 1 : 0);
            }
        }
    } else {
        dx12_fg_switch_test_g_FramePacingInitialized = true;
    }
    dx12_fg_switch_test_g_LastFramePacingTime = now;
    float elapsed = std::chrono::duration<float>(now - dx12_fg_switch_test_g_StartTime).count();

    if (dx12_fg_switch_test_g_AutoExitSeconds > 0 && elapsed >= static_cast<float>(dx12_fg_switch_test_g_AutoExitSeconds)) {
        testapp::Log(
            "[FG-DIAG] Auto exit after %d seconds at frameID=%llu mode=%s fsr=%d fsrSuspended=%d "
            "dlss=%d dlssSuspended=%d\n",
            dx12_fg_switch_test_g_AutoExitSeconds, static_cast<unsigned long long>(dx12_fg_switch_test_g_FrameIdCounter), ModeName(dx12_fg_switch_test_g_CurrentMode),
            dx12_fg_switch_test_g_FsrEnabled ? 1 : 0, dx12_fg_switch_test_g_FsrSuspended ? 1 : 0, dx12_fg_switch_test_g_DlssEnabled ? 1 : 0, dx12_fg_switch_test_g_DlssSuspended ? 1 : 0);
        testapp::LogFlush();
        dx12_fg_switch_test_g_Running = false;
        DestroyWindow(dx12_fg_switch_test_g_Hwnd);
        return;
    }

    UINT frameIndex;
    {
        std::lock_guard<std::mutex> lock(g_FrameSyncMutex);
        frameIndex = g_FrameIndex;
        if (frameIndex >= g_SwapChainBufferCount) {
            frameIndex %= g_SwapChainBufferCount;
        }
    }

    if (dx12_fg_switch_test_g_ModeSwitchingArmed) {
        RunAutoSequence(elapsed);
    }
    if (dx12_fg_switch_test_g_ModeSwitchingArmed && dx12_fg_switch_test_g_ModeSwitchPending) {
        SwitchMode(dx12_fg_switch_test_g_PendingMode, dx12_fg_switch_test_g_ManualMode ? "manual" : "auto", frameIndex);
        if (!dx12_fg_switch_test_g_Running || !g_SwapChain) {
            testapp::Log("[FG-DIAG] Render aborted after mode switch because swapchain is unavailable (running=%d)\n",
                         dx12_fg_switch_test_g_Running ? 1 : 0);
            testapp::LogFlush();
            return;
        }
        std::lock_guard<std::mutex> lock(g_FrameSyncMutex);
        frameIndex = g_FrameIndex;
        if (frameIndex >= g_SwapChainBufferCount) {
            frameIndex %= g_SwapChainBufferCount;
        }
    }
    if (dx12_fg_switch_test_g_ModeSwitchingArmed && dx12_fg_switch_test_g_SuspensionTogglePending) {
        ToggleCurrentFGSuspension(dx12_fg_switch_test_g_PendingSuspensionToggleMode, "manual", frameIndex);
        if (!dx12_fg_switch_test_g_Running || !g_SwapChain) {
            testapp::Log(
                "[FG-DIAG] Render aborted after suspension toggle because swapchain is unavailable "
                "(running=%d)\n",
                dx12_fg_switch_test_g_Running ? 1 : 0);
            testapp::LogFlush();
            return;
        }
    }

    sl::FrameToken* frameToken = BeginStreamlineFrame();
    SetPCLMarker(frameToken, sl::PCLMarker::eSimulationStart, "SimulationStart");
    SetPCLMarker(frameToken, sl::PCLMarker::eSimulationEnd, "SimulationEnd");
    ++dx12_fg_switch_test_g_FrameIdCounter;
    MaybeToggleFSRSuspensionStress(frameIndex);
    MaybeToggleDLSSSuspensionStress();

    // Per-frame sub-pixel camera jitter for the temporal upscalers (Halton 2,3 over the
    // scale-dependent phase count); zero when upscaling is disabled (legacy behavior).
    dx12_fg_switch_test_g_CurrentJitter = UpscalingActive() ? testapp::fg::ComputeJitter(dx12_fg_switch_test_g_FrameIdCounter, dx12_fg_switch_test_g_JitterPhaseCount)
                                        : testapp::fg::JitterOffset{0.0f, 0.0f};

    const UINT presentSyncInterval = ResolvePresentSyncInterval();
    const UINT presentFlags = ResolvePresentFlags(presentSyncInterval);
    if ((dx12_fg_switch_test_g_FrameIdCounter % 60) == 0) {
        // Measured fps per heartbeat window makes a lingering Reflex frame cap (FG half-rate ~69 or
        // the normal low-latency cap) objectively visible in the log after FG off/suspend.
        static std::chrono::high_resolution_clock::time_point s_lastHeartbeatTime;
        static uint64_t s_lastHeartbeatFrameId = 0;
        double fps = 0.0;
        if (s_lastHeartbeatFrameId != 0 && dx12_fg_switch_test_g_FrameIdCounter > s_lastHeartbeatFrameId) {
            const double windowSeconds = std::chrono::duration<double>(now - s_lastHeartbeatTime).count();
            if (windowSeconds > 0.0) {
                fps = static_cast<double>(dx12_fg_switch_test_g_FrameIdCounter - s_lastHeartbeatFrameId) / windowSeconds;
            }
        }
        s_lastHeartbeatTime = now;
        s_lastHeartbeatFrameId = dx12_fg_switch_test_g_FrameIdCounter;
        testapp::Log(
            "[FG-DIAG] heartbeat frameID=%llu frameIndex=%u mode=%s fsr=%d dlss=%d manual=%d autoStage=%d "
            "fsrConfigureEveryFrame=%d fsrSuspended=%d dlssSuspended=%d presentSync=%u presentFlags=0x%x "
            "configuredVsync=%d lastSuspendToggleFrame=%llu fps=%.1f reflexLL=%d render=%dx%d upscaler=%s\n",
            static_cast<unsigned long long>(dx12_fg_switch_test_g_FrameIdCounter), frameIndex, ModeName(dx12_fg_switch_test_g_CurrentMode),
            dx12_fg_switch_test_g_FsrEnabled ? 1 : 0, dx12_fg_switch_test_g_DlssEnabled ? 1 : 0, dx12_fg_switch_test_g_ManualMode ? 1 : 0, dx12_fg_switch_test_g_AutoStage,
            dx12_fg_switch_test_g_FsrConfigureEveryFrame ? 1 : 0, dx12_fg_switch_test_g_FsrSuspended ? 1 : 0, dx12_fg_switch_test_g_DlssSuspended ? 1 : 0, presentSyncInterval,
            presentFlags, dx12_fg_switch_test_g_VSync, static_cast<unsigned long long>(dx12_fg_switch_test_g_LastFsrSuspendResumeToggleFrameId), fps,
            dx12_fg_switch_test_g_ReflexLowLatencyActive ? 1 : 0, dx12_fg_switch_test_g_RenderWidth, dx12_fg_switch_test_g_RenderHeight, ActiveUpscalerName());
    }

    if (dx12_fg_switch_test_g_FsrEnabled && dx12_fg_switch_test_g_FsrConfigureEveryFrame) {
        ConfigureFSR(!dx12_fg_switch_test_g_FsrSuspended, frameIndex < g_SwapChainBufferCount ? g_RenderTargets[frameIndex].Get() : nullptr,
                     dx12_fg_switch_test_g_FsrSuspended ? "per-frame suspended refresh" : "per-frame active refresh", false);
    }
    RunDxgiVideoMemoryQueryStress();

    WaitForSwapChainFrameLatency();
    g_CommandAllocators[frameIndex]->Reset();
    g_CommandList->Reset(g_CommandAllocators[frameIndex].Get(), nullptr);
    const float hudSweep = std::sin(elapsed * 1.2f) * 0.5f + 0.5f;
    LONG hudSpan = dx12_fg_switch_test_g_WindowWidth - 280;
    if (hudSpan < 0) {
        hudSpan = 0;
    }
    const LONG hudX = 40 + static_cast<LONG>(hudSweep * static_cast<float>(hudSpan));
    const LONG hudY = dx12_fg_switch_test_g_WindowHeight > 220 ? dx12_fg_switch_test_g_WindowHeight - 90 : 40;
    RenderSwitchSceneInputs(elapsed, hudX, hudY);

    // Streamline constants + ALL resource tags (FG + super-resolution) must be recorded for this
    // frame token BEFORE slEvaluateFeature(kFeatureDLSS) runs inside the upscale stage.
    SubmitStreamlineFrameInputs(frameToken, frameIndex);
    // Upscale stage: fills the display-res hudlessColor from the render-res scene inputs
    // (TAA/TAAU for OFF mode and fallbacks, DLSS SR or FSR upscale otherwise).
    RunUpscaleStage(frameToken, frameIndex, dx12_fg_switch_test_g_LastFrameDeltaMs);

    // Compose the presented backbuffer from the (FP16) hud-less scene via the dithered present
    // blit, then draw the UI on top so all-FG-off and real frames match the FG UI layer
    // (hud-less + UI == presented frame).
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_RenderTargets[frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_CommandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<UINT64>(frameIndex) * g_RtvDescriptorSize;
    dx12_fg_switch_test_g_PresentBlit.Render(g_CommandList.Get(), g_FgInputs.hudlessColor.Get(), rtvHandle,
                         static_cast<UINT>(dx12_fg_switch_test_g_WindowWidth), static_cast<UINT>(dx12_fg_switch_test_g_WindowHeight), frameIndex,
                         static_cast<uint32_t>(dx12_fg_switch_test_g_FrameIdCounter));
    DrawHudOverlay(g_CommandList.Get(), rtvHandle, hudX, hudY);
    DrawStatusText(g_CommandList.Get(), rtvHandle);
    if (!dx12_fg_switch_test_g_FsrSuspended) {
        DispatchFSRPrepare(dx12_fg_switch_test_g_LastFrameDeltaMs);
    }
    if (dx12_fg_switch_test_g_FsrEnabled && !dx12_fg_switch_test_g_FsrSuspended) {
        RegisterFSRUiResource();
    }

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_CommandList->ResourceBarrier(1, &barrier);
    g_CommandList->Close();

    ID3D12CommandList* lists[] = {g_CommandList.Get()};
    SetPCLMarker(frameToken, sl::PCLMarker::eRenderSubmitStart, "RenderSubmitStart");
    g_CommandQueue->ExecuteCommandLists(1, lists);
    SetPCLMarker(frameToken, sl::PCLMarker::eRenderSubmitEnd, "RenderSubmitEnd");
    SetPCLMarker(frameToken, sl::PCLMarker::ePresentStart, "PresentStart");
    const bool fsrExitPassthroughPresent =
        dx12_fg_switch_test_g_FsrExitTransitionStage == testapp::fg::FsrExitTransitionStage::PresentPending;
    const bool dlssReplacementPassthroughPresent =
        dx12_fg_switch_test_g_FsrExitTransitionStage == testapp::fg::FsrExitTransitionStage::ReplacementPresentPending;
    HRESULT presentHr = g_SwapChain->Present(presentSyncInterval, presentFlags);
    if (fsrExitPassthroughPresent || dlssReplacementPassthroughPresent || FAILED(presentHr) ||
        (dx12_fg_switch_test_g_LastModeSwitchFrameId != 0 && dx12_fg_switch_test_g_FrameIdCounter - dx12_fg_switch_test_g_LastModeSwitchFrameId <= 3)) {
        testapp::Log(
            "[FG-DIAG] Present frameID=%llu mode=%s sync=%u flags=0x%x configuredVsync=%d "
            "fsrExitPassthrough=%d dlssReplacementPassthrough=%d hr=0x%08lx\n",
            static_cast<unsigned long long>(dx12_fg_switch_test_g_FrameIdCounter), ModeName(dx12_fg_switch_test_g_CurrentMode), presentSyncInterval,
            presentFlags, dx12_fg_switch_test_g_VSync, fsrExitPassthroughPresent ? 1 : 0, dlssReplacementPassthroughPresent ? 1 : 0,
            static_cast<unsigned long>(presentHr));
        testapp::LogFlush();
    }
    if (fsrExitPassthroughPresent) {
        if (SUCCEEDED(presentHr)) {
            dx12_fg_switch_test_g_FsrExitTransitionStage = testapp::fg::FsrExitTransitionStage::PassthroughPresented;
        } else {
            testapp::Log(
                "[FG-DIAG] WARN FSR exit passthrough Present failed; keeping the proxy alive for a retry "
                "(frameID=%llu hr=0x%08lx)\n",
                static_cast<unsigned long long>(dx12_fg_switch_test_g_FrameIdCounter), static_cast<unsigned long>(presentHr));
            testapp::LogFlush();
        }
    }
    if (dlssReplacementPassthroughPresent) {
        if (SUCCEEDED(presentHr)) {
            dx12_fg_switch_test_g_FsrExitTransitionStage = testapp::fg::FsrExitTransitionStage::ReplacementPresented;
        } else {
            testapp::Log(
                "[FG-DIAG] WARN DLSS replacement passthrough Present failed; deferring activation and "
                "keeping the proxy alive for a retry (frameID=%llu hr=0x%08lx)\n",
                static_cast<unsigned long long>(dx12_fg_switch_test_g_FrameIdCounter), static_cast<unsigned long>(presentHr));
            testapp::LogFlush();
        }
    }
    if (SUCCEEDED(presentHr) && dx12_fg_switch_test_g_FsrRuntimeRetirementPendingForDlss && dx12_fg_switch_test_g_CurrentMode == FGMode::DLSS &&
        dx12_fg_switch_test_g_DlssEnabled) {
        // Keep FSR module work out of both the replacement passthrough and DLSS-G's first active
        // Present. Once that active image was delivered, retirement cannot lengthen either gap.
        MaybeUnloadFSRRuntimeAfterSwitch("after first active DLSS Present");
        StartAsyncFSRRuntimePreload("after first active DLSS Present");
        dx12_fg_switch_test_g_FsrRuntimeRetirementPendingForDlss = false;
    }
    if (IsDeviceRemovedHr(presentHr)) {
        // Dump DRED (once, internal guard) and stop the loop instead of live-locking on a dead device.
        DumpDredOnDeviceRemoved("Present device-removed");
        static bool s_loggedDeviceRemovedStop = false;
        if (!s_loggedDeviceRemovedStop) {
            s_loggedDeviceRemovedStop = true;
            testapp::Log("[FG-DIAG] Stopping main loop after device removal at frameID=%llu mode=%s suspended=%d\n",
                         static_cast<unsigned long long>(dx12_fg_switch_test_g_FrameIdCounter), ModeName(dx12_fg_switch_test_g_CurrentMode),
                         dx12_fg_switch_test_g_DlssSuspended ? 1 : 0);
            testapp::LogFlush();
        }
        dx12_fg_switch_test_g_Running = false;
    }
    SetPCLMarker(frameToken, sl::PCLMarker::ePresentEnd, "PresentEnd");
    MoveToNextFrame();
    if (dx12_fg_switch_test_g_DlssEnabled && (dx12_fg_switch_test_g_FrameTokenIndex % 120) == 0) {
        PollDLSSFGState();
    }
}

void Cleanup() {
    if (g_Device) {
        testapp::Log(
            "[FG-DIAG] Cleanup leaves %s without recreating a native swapchain "
            "(fsrEnabled=%d fsrSuspended=%d dlssEnabled=%d dlssSuspended=%d fsrCtx=%p fsrSwapchainCtx=%p)\n",
            ModeName(dx12_fg_switch_test_g_CurrentMode), dx12_fg_switch_test_g_FsrEnabled ? 1 : 0, dx12_fg_switch_test_g_FsrSuspended ? 1 : 0, dx12_fg_switch_test_g_DlssEnabled ? 1 : 0,
            dx12_fg_switch_test_g_DlssSuspended ? 1 : 0, (void*)dx12_fg_switch_test_g_FfxCtx, (void*)dx12_fg_switch_test_g_FfxSwapChainCtx);
        if (dx12_fg_switch_test_g_DlssEnabled) {
            SetDLSSFGMode(false);
            dx12_fg_switch_test_g_DlssEnabled = false;
        }
        dx12_fg_switch_test_g_DlssSuspended = false;
        WaitForGpu();
    }
    DestroyFSRContexts();
    dx12_fg_switch_test_g_Taa.Release();
    dx12_fg_switch_test_g_PresentBlit.Release();
    testapp::dx12fg::ReleaseAuxiliaryResources(g_FgInputs);
    // Degenerate-UI placeholder is device-bound; release it with the renderer so it is recreated against the
    // new device on the next registration (preserves the multi-switch FSR->DLSS->FSR coverage).
    dx12_fg_switch_test_g_FsrDegenerateUiTexture.Reset();
    g_Scene.Release();
    for (auto& renderTarget : g_RenderTargets) {
        renderTarget.Reset();
    }
    for (auto& allocator : g_CommandAllocators) {
        allocator.Reset();
    }
    g_CommandList.Reset();
    g_RtvHeap.Reset();
    g_SwapChain.Reset();
    dx12_fg_switch_test_g_SwapChainUsesStreamline = false;
    g_DxgiVideoMemoryQueryAdapter.Reset();
    g_CommandQueue.Reset();
    g_Fence.Reset();
    g_Device.Reset();
    JoinAsyncRuntimePreloadThreads("cleanup");
    ShutdownStreamlineSerialized("cleanup");
    UnloadFSRRuntimeSerialized("cleanup");
    UnloadFSRUpscalerRuntime("cleanup");
    if (g_FenceEvent) {
        CloseHandle(g_FenceEvent);
        g_FenceEvent = nullptr;
    }
    g_FrameLatencyWaitHandle = nullptr;
}
