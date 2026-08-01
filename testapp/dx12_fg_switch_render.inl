// Extracted from dx12_fg_switch_test.cpp to stay under the AGENTS.md
// size ceiling. Included at exactly the point these definitions used to sit,
// so declaration order is unchanged.

static void RunAutoSequence(float elapsedSeconds) {
    if (g_ManualMode) {
        return;
    }
    if (g_AutoStage == 0 && elapsedSeconds >= static_cast<float>(g_AutoFsrStartSeconds)) {
        g_AutoStage = 1;
        RequestMode(FGMode::FSR, "auto FSR stage", false);
    } else if (g_AutoStage == 1 && elapsedSeconds >= static_cast<float>(g_AutoDlssStartSeconds)) {
        g_AutoStage = 2;
        RequestMode(FGMode::DLSS, "auto DLSS stage after FSR suspend/resume", false);
    } else if (g_AutoStage == 2 && elapsedSeconds >= static_cast<float>(g_AutoReturnFsrSeconds)) {
        g_AutoStage = 3;
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
static void MaybeToggleDLSSSuspensionStress() {
    if ((!g_DlssSuspendResumeStress && !g_DlssOffAfterActiveStress) || g_ManualMode || g_ModeSwitchPending ||
        g_CurrentMode != FGMode::DLSS || !g_DlssInitialized || !g_SlDLSSGSetOptions) {
        return;
    }
    const bool suspendPending = g_DlssSuspendResumeStress && !g_DlssStressDidSuspend;
    const bool offPending = g_DlssOffAfterActiveStress && !g_DlssStressDidRequestOff && !suspendPending;
    if (!suspendPending && !offPending) {
        return;
    }
    const auto now = std::chrono::high_resolution_clock::now();
    const float elapsed = std::chrono::duration<float>(now - g_LastDlssSuspendResumeToggleTime).count();
    if (elapsed < static_cast<float>(g_DlssSuspendResumeIntervalSeconds)) {
        return;
    }
    if (offPending) {
        g_DlssStressDidRequestOff = true;
        RequestMode(FGMode::Off, "dlss off-after-active stress", false);
        return;
    }
    const bool ok = SetDLSSFGMode(false);
    if (ok) {
        g_DlssEnabled = false;
        g_DlssSuspended = true;
        g_DlssStressDidSuspend = true;
        // Re-anchor so a chained --dlss-off-stress stage holds the suspended state for a full
        // interval before switching OFF.
        g_LastDlssSuspendResumeToggleTime = now;
    }
    testapp::Log("[FG-DIAG] DLSS suspend-and-hold stress: %s frameID=%llu\n",
                 ok ? "suspended FG (holding suspended)" : "suspend FAILED (state unchanged)",
                 static_cast<unsigned long long>(g_FrameIdCounter));
    testapp::LogFlush();
    UpdateWindowTitle();
}

// Renders the frame-generation scene inputs: a real 3D cube into the hud-less color +
// motion-vector targets (so FG interpolates the moving cube to the output/generated rate), and
// the HUD + status into the UI-layer texture. The UI layer is registered with FSR / tagged for
// DLSS, so it is composited crisp on every real and generated frame at the base rate (no
// ghosting). The static camera/floor/sky carry zero motion, which is correct.
static void RenderSwitchSceneInputs(float elapsedSeconds, LONG hudX, LONG hudY) {
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
    for (int pass = 0; pass < g_GpuLoadPasses; ++pass) {
        g_CommandList->ClearRenderTargetView(sceneRtv, clearBlack, 0, nullptr);
    }
    g_CommandList->ClearRenderTargetView(sceneRtv, clearBlack, 0, nullptr);

    // Scene: sky + procedural checker floor (spatial depth/fog) + a lit moving cube, rendered at
    // render resolution with the per-frame jittered projection. The cube carries per-pixel motion
    // vectors -> interpolated by FG (output rate); floor/sky are static.
    g_Scene.Render(g_CommandList.Get(), sceneRtv, aux, g_FrameIndex, g_RenderWidth, g_RenderHeight, elapsedSeconds,
                   g_CurrentJitter.x, g_CurrentJitter.y);

    // UI layer: animated "100 HP" HUD + health bar + status, into the registered/tagged UI resource.
    DrawHudOverlay(g_CommandList.Get(), aux.UiRtv(), hudX, hudY);
    DrawStatusText(g_CommandList.Get(), aux.UiRtv());

    testapp::dx12fg::Transition(g_CommandList.Get(), sceneTarget, sceneTargetState, testapp::dx12fg::kColorReadState);
    testapp::dx12fg::Transition(g_CommandList.Get(), aux.uiColor.Get(), aux.uiState, testapp::dx12fg::kColorReadState);
    testapp::dx12fg::Transition(g_CommandList.Get(), aux.motionVectors.Get(), aux.motionState,
                                testapp::dx12fg::kColorReadState);
    testapp::dx12fg::Transition(g_CommandList.Get(), aux.depth.Get(), aux.depthState, testapp::dx12fg::kDepthReadState);
}

static void Render() {
    auto now = std::chrono::high_resolution_clock::now();
    if (g_FramePacingInitialized) {
        const double deltaMs = std::chrono::duration<double, std::milli>(now - g_LastFramePacingTime).count();
        g_LastFrameDeltaMs = static_cast<float>(deltaMs);
        if (deltaMs > g_MaxFrameDeltaMs) {
            g_MaxFrameDeltaMs = deltaMs;
        }
        if (deltaMs >= 25.0) {
            ++g_FramePacingSpikeCount;
            if (g_FramePacingSpikeCount <= 12 || (g_FramePacingSpikeCount % 30) == 0) {
                testapp::Log(
                    "[FG-DIAG] Frame pacing spike frameID=%llu mode=%s deltaMs=%.2f "
                    "asyncFSR=%d asyncSL=%d fsr=%d fsrSuspended=%d dlss=%d dlssSuspended=%d\n",
                    static_cast<unsigned long long>(g_FrameIdCounter), ModeName(g_CurrentMode), deltaMs,
                    g_FsrPreloadInProgress.load() ? 1 : 0, g_StreamlinePreloadInProgress.load() ? 1 : 0,
                    g_FsrEnabled ? 1 : 0, g_FsrSuspended ? 1 : 0, g_DlssEnabled ? 1 : 0, g_DlssSuspended ? 1 : 0);
            }
        }
    } else {
        g_FramePacingInitialized = true;
    }
    g_LastFramePacingTime = now;
    float elapsed = std::chrono::duration<float>(now - g_StartTime).count();

    if (g_AutoExitSeconds > 0 && elapsed >= static_cast<float>(g_AutoExitSeconds)) {
        testapp::Log(
            "[FG-DIAG] Auto exit after %d seconds at frameID=%llu mode=%s fsr=%d fsrSuspended=%d "
            "dlss=%d dlssSuspended=%d\n",
            g_AutoExitSeconds, static_cast<unsigned long long>(g_FrameIdCounter), ModeName(g_CurrentMode),
            g_FsrEnabled ? 1 : 0, g_FsrSuspended ? 1 : 0, g_DlssEnabled ? 1 : 0, g_DlssSuspended ? 1 : 0);
        testapp::LogFlush();
        g_Running = false;
        DestroyWindow(g_Hwnd);
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

    if (g_ModeSwitchingArmed) {
        RunAutoSequence(elapsed);
    }
    if (g_ModeSwitchingArmed && g_ModeSwitchPending) {
        SwitchMode(g_PendingMode, g_ManualMode ? "manual" : "auto", frameIndex);
        if (!g_Running || !g_SwapChain) {
            testapp::Log("[FG-DIAG] Render aborted after mode switch because swapchain is unavailable (running=%d)\n",
                         g_Running ? 1 : 0);
            testapp::LogFlush();
            return;
        }
        std::lock_guard<std::mutex> lock(g_FrameSyncMutex);
        frameIndex = g_FrameIndex;
        if (frameIndex >= g_SwapChainBufferCount) {
            frameIndex %= g_SwapChainBufferCount;
        }
    }
    if (g_ModeSwitchingArmed && g_SuspensionTogglePending) {
        ToggleCurrentFGSuspension(g_PendingSuspensionToggleMode, "manual", frameIndex);
        if (!g_Running || !g_SwapChain) {
            testapp::Log(
                "[FG-DIAG] Render aborted after suspension toggle because swapchain is unavailable "
                "(running=%d)\n",
                g_Running ? 1 : 0);
            testapp::LogFlush();
            return;
        }
    }

    sl::FrameToken* frameToken = BeginStreamlineFrame();
    SetPCLMarker(frameToken, sl::PCLMarker::eSimulationStart, "SimulationStart");
    SetPCLMarker(frameToken, sl::PCLMarker::eSimulationEnd, "SimulationEnd");
    ++g_FrameIdCounter;
    MaybeToggleFSRSuspensionStress(frameIndex);
    MaybeToggleDLSSSuspensionStress();

    // Per-frame sub-pixel camera jitter for the temporal upscalers (Halton 2,3 over the
    // scale-dependent phase count); zero when upscaling is disabled (legacy behavior).
    g_CurrentJitter = UpscalingActive() ? testapp::fg::ComputeJitter(g_FrameIdCounter, g_JitterPhaseCount)
                                        : testapp::fg::JitterOffset{0.0f, 0.0f};

    const UINT presentSyncInterval = ResolvePresentSyncInterval();
    const UINT presentFlags = ResolvePresentFlags(presentSyncInterval);
    if ((g_FrameIdCounter % 60) == 0) {
        // Measured fps per heartbeat window makes a lingering Reflex frame cap (FG half-rate ~69 or
        // the normal low-latency cap) objectively visible in the log after FG off/suspend.
        static std::chrono::high_resolution_clock::time_point s_lastHeartbeatTime;
        static uint64_t s_lastHeartbeatFrameId = 0;
        double fps = 0.0;
        if (s_lastHeartbeatFrameId != 0 && g_FrameIdCounter > s_lastHeartbeatFrameId) {
            const double windowSeconds = std::chrono::duration<double>(now - s_lastHeartbeatTime).count();
            if (windowSeconds > 0.0) {
                fps = static_cast<double>(g_FrameIdCounter - s_lastHeartbeatFrameId) / windowSeconds;
            }
        }
        s_lastHeartbeatTime = now;
        s_lastHeartbeatFrameId = g_FrameIdCounter;
        testapp::Log(
            "[FG-DIAG] heartbeat frameID=%llu frameIndex=%u mode=%s fsr=%d dlss=%d manual=%d autoStage=%d "
            "fsrConfigureEveryFrame=%d fsrSuspended=%d dlssSuspended=%d presentSync=%u presentFlags=0x%x "
            "configuredVsync=%d lastSuspendToggleFrame=%llu fps=%.1f reflexLL=%d render=%dx%d upscaler=%s\n",
            static_cast<unsigned long long>(g_FrameIdCounter), frameIndex, ModeName(g_CurrentMode),
            g_FsrEnabled ? 1 : 0, g_DlssEnabled ? 1 : 0, g_ManualMode ? 1 : 0, g_AutoStage,
            g_FsrConfigureEveryFrame ? 1 : 0, g_FsrSuspended ? 1 : 0, g_DlssSuspended ? 1 : 0, presentSyncInterval,
            presentFlags, g_VSync, static_cast<unsigned long long>(g_LastFsrSuspendResumeToggleFrameId), fps,
            g_ReflexLowLatencyActive ? 1 : 0, g_RenderWidth, g_RenderHeight, ActiveUpscalerName());
    }

    if (g_FsrEnabled && g_FsrConfigureEveryFrame) {
        ConfigureFSR(!g_FsrSuspended, frameIndex < g_SwapChainBufferCount ? g_RenderTargets[frameIndex].Get() : nullptr,
                     g_FsrSuspended ? "per-frame suspended refresh" : "per-frame active refresh", false);
    }
    RunDxgiVideoMemoryQueryStress();

    WaitForSwapChainFrameLatency();
    g_CommandAllocators[frameIndex]->Reset();
    g_CommandList->Reset(g_CommandAllocators[frameIndex].Get(), nullptr);
    const float hudSweep = std::sin(elapsed * 1.2f) * 0.5f + 0.5f;
    LONG hudSpan = g_WindowWidth - 280;
    if (hudSpan < 0) {
        hudSpan = 0;
    }
    const LONG hudX = 40 + static_cast<LONG>(hudSweep * static_cast<float>(hudSpan));
    const LONG hudY = g_WindowHeight > 220 ? g_WindowHeight - 90 : 40;
    RenderSwitchSceneInputs(elapsed, hudX, hudY);

    // Streamline constants + ALL resource tags (FG + super-resolution) must be recorded for this
    // frame token BEFORE slEvaluateFeature(kFeatureDLSS) runs inside the upscale stage.
    SubmitStreamlineFrameInputs(frameToken, frameIndex);
    // Upscale stage: fills the display-res hudlessColor from the render-res scene inputs
    // (TAA/TAAU for OFF mode and fallbacks, DLSS SR or FSR upscale otherwise).
    RunUpscaleStage(frameToken, frameIndex, g_LastFrameDeltaMs);

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
    rtvHandle.ptr += frameIndex * g_RtvDescriptorSize;
    g_PresentBlit.Render(g_CommandList.Get(), g_FgInputs.hudlessColor.Get(), rtvHandle,
                         static_cast<UINT>(g_WindowWidth), static_cast<UINT>(g_WindowHeight), frameIndex,
                         static_cast<uint32_t>(g_FrameIdCounter));
    DrawHudOverlay(g_CommandList.Get(), rtvHandle, hudX, hudY);
    DrawStatusText(g_CommandList.Get(), rtvHandle);
    if (!g_FsrSuspended) {
        DispatchFSRPrepare(g_LastFrameDeltaMs);
    }
    if (g_FsrEnabled && !g_FsrSuspended) {
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
        g_FsrExitTransitionStage == testapp::fg::FsrExitTransitionStage::PresentPending;
    const bool dlssReplacementPassthroughPresent =
        g_FsrExitTransitionStage == testapp::fg::FsrExitTransitionStage::ReplacementPresentPending;
    HRESULT presentHr = g_SwapChain->Present(presentSyncInterval, presentFlags);
    if (fsrExitPassthroughPresent || dlssReplacementPassthroughPresent || FAILED(presentHr) ||
        (g_LastModeSwitchFrameId != 0 && g_FrameIdCounter - g_LastModeSwitchFrameId <= 3)) {
        testapp::Log(
            "[FG-DIAG] Present frameID=%llu mode=%s sync=%u flags=0x%x configuredVsync=%d "
            "fsrExitPassthrough=%d dlssReplacementPassthrough=%d hr=0x%08lx\n",
            static_cast<unsigned long long>(g_FrameIdCounter), ModeName(g_CurrentMode), presentSyncInterval,
            presentFlags, g_VSync, fsrExitPassthroughPresent ? 1 : 0, dlssReplacementPassthroughPresent ? 1 : 0,
            static_cast<unsigned long>(presentHr));
        testapp::LogFlush();
    }
    if (fsrExitPassthroughPresent) {
        if (SUCCEEDED(presentHr)) {
            g_FsrExitTransitionStage = testapp::fg::FsrExitTransitionStage::PassthroughPresented;
        } else {
            testapp::Log(
                "[FG-DIAG] WARN FSR exit passthrough Present failed; keeping the proxy alive for a retry "
                "(frameID=%llu hr=0x%08lx)\n",
                static_cast<unsigned long long>(g_FrameIdCounter), static_cast<unsigned long>(presentHr));
            testapp::LogFlush();
        }
    }
    if (dlssReplacementPassthroughPresent) {
        if (SUCCEEDED(presentHr)) {
            g_FsrExitTransitionStage = testapp::fg::FsrExitTransitionStage::ReplacementPresented;
        } else {
            testapp::Log(
                "[FG-DIAG] WARN DLSS replacement passthrough Present failed; deferring activation and "
                "keeping the proxy alive for a retry (frameID=%llu hr=0x%08lx)\n",
                static_cast<unsigned long long>(g_FrameIdCounter), static_cast<unsigned long>(presentHr));
            testapp::LogFlush();
        }
    }
    if (SUCCEEDED(presentHr) && g_FsrRuntimeRetirementPendingForDlss && g_CurrentMode == FGMode::DLSS &&
        g_DlssEnabled) {
        // Keep FSR module work out of both the replacement passthrough and DLSS-G's first active
        // Present. Once that active image was delivered, retirement cannot lengthen either gap.
        MaybeUnloadFSRRuntimeAfterSwitch("after first active DLSS Present");
        StartAsyncFSRRuntimePreload("after first active DLSS Present");
        g_FsrRuntimeRetirementPendingForDlss = false;
    }
    if (IsDeviceRemovedHr(presentHr)) {
        // Dump DRED (once, internal guard) and stop the loop instead of live-locking on a dead device.
        DumpDredOnDeviceRemoved("Present device-removed");
        static bool s_loggedDeviceRemovedStop = false;
        if (!s_loggedDeviceRemovedStop) {
            s_loggedDeviceRemovedStop = true;
            testapp::Log("[FG-DIAG] Stopping main loop after device removal at frameID=%llu mode=%s suspended=%d\n",
                         static_cast<unsigned long long>(g_FrameIdCounter), ModeName(g_CurrentMode),
                         g_DlssSuspended ? 1 : 0);
            testapp::LogFlush();
        }
        g_Running = false;
    }
    SetPCLMarker(frameToken, sl::PCLMarker::ePresentEnd, "PresentEnd");
    MoveToNextFrame();
    if (g_DlssEnabled && (g_FrameTokenIndex % 120) == 0) {
        PollDLSSFGState();
    }
}

static void Cleanup() {
    if (g_Device) {
        testapp::Log(
            "[FG-DIAG] Cleanup leaves %s without recreating a native swapchain "
            "(fsrEnabled=%d fsrSuspended=%d dlssEnabled=%d dlssSuspended=%d fsrCtx=%p fsrSwapchainCtx=%p)\n",
            ModeName(g_CurrentMode), g_FsrEnabled ? 1 : 0, g_FsrSuspended ? 1 : 0, g_DlssEnabled ? 1 : 0,
            g_DlssSuspended ? 1 : 0, (void*)g_FfxCtx, (void*)g_FfxSwapChainCtx);
        if (g_DlssEnabled) {
            SetDLSSFGMode(false);
            g_DlssEnabled = false;
        }
        g_DlssSuspended = false;
        WaitForGpu();
    }
    DestroyFSRContexts();
    g_Taa.Release();
    g_PresentBlit.Release();
    testapp::dx12fg::ReleaseAuxiliaryResources(g_FgInputs);
    // Degenerate-UI placeholder is device-bound; release it with the renderer so it is recreated against the
    // new device on the next registration (preserves the multi-switch FSR->DLSS->FSR coverage).
    g_FsrDegenerateUiTexture.Reset();
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
    g_SwapChainUsesStreamline = false;
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
