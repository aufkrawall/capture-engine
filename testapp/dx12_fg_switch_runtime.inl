// Extracted from dx12_fg_switch_test.cpp to stay under the AGENTS.md
// size ceiling. Included at exactly the point these definitions used to sit,
// so declaration order is unchanged.

static bool LoadStreamlineAndInitSerialized(const char* reason) {
    if (g_SlInitialized) {
        return true;
    }
    std::lock_guard<std::mutex> lock(g_RuntimeLoadMutex);
    if (g_SlInitialized) {
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

static bool LoadFSRRuntimeSerialized(const char* reason) {
    if (g_FsrRuntimeLoaded && g_FfxModule && g_FfxCreateContext && g_FfxConfigure && g_FfxDispatch &&
        g_FfxDestroyContext) {
        return true;
    }
    std::lock_guard<std::mutex> lock(g_RuntimeLoadMutex);
    if (g_FsrRuntimeLoaded && g_FfxModule && g_FfxCreateContext && g_FfxConfigure && g_FfxDispatch &&
        g_FfxDestroyContext) {
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

static void ShutdownStreamlineSerialized(const char* reason) {
    std::lock_guard<std::mutex> lock(g_RuntimeLoadMutex);
    testapp::Log("[FG-DIAG] Streamline serialized shutdown reason=%s initialized=%d module=%p\n",
                 reason ? reason : "unknown", g_SlInitialized ? 1 : 0, g_SlModule);
    ShutdownStreamline();
}

static void UnloadFSRRuntimeSerialized(const char* reason) {
    std::lock_guard<std::mutex> lock(g_RuntimeLoadMutex);
    UnloadFSRRuntime(reason);
}

static void StartAsyncFSRRuntimePreload(const char* reason) {
    if (g_FsrPreloadThread.joinable() && !g_FsrPreloadInProgress.load()) {
        g_FsrPreloadThread.join();
        g_FsrPreloadStarted = false;
    }
    if (!g_AsyncRuntimePreload || g_FsrRuntimeLoaded || g_FsrPreloadInProgress.load() ||
        g_FsrPreloadStarted.exchange(true)) {
        return;
    }
    std::string reasonText = reason ? reason : "unknown";
    g_FsrPreloadInProgress = true;
    g_FsrPreloadSucceeded = false;
    testapp::Log("[FG-DIAG] Async FSR runtime preload scheduled reason=%s\n", reasonText.c_str());
    g_FsrPreloadThread = std::thread([reasonText]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        bool loaded = LoadFSRRuntimeSerialized(reasonText.c_str());
        g_FsrPreloadSucceeded = loaded;
        g_FsrPreloadInProgress = false;
        testapp::Log("[FG-DIAG] Async FSR runtime preload finished reason=%s ok=%d\n", reasonText.c_str(),
                     loaded ? 1 : 0);
        testapp::LogFlush();
    });
}

static void StartAsyncStreamlinePreload(const char* reason) {
    if (g_StreamlinePreloadThread.joinable() && !g_StreamlinePreloadInProgress.load()) {
        g_StreamlinePreloadThread.join();
        g_StreamlinePreloadStarted = false;
    }
    const bool fsrOwnsPresentation =
        g_FsrEnabled || g_FsrInitialized || g_FfxCtx || g_FfxSwapChainCtx || g_SwapChainOwner == SwapChainOwner::FSR;
    if (fsrOwnsPresentation) {
        static std::atomic<uint64_t> s_skipLogCount{0};
        const uint64_t skipLogCount = s_skipLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (skipLogCount <= 12 || (skipLogCount % 60) == 0) {
            testapp::Log(
                "[FG-DIAG] Async Streamline preload skipped during active/native FSR ownership reason=%s "
                "fsrEnabled=%d fsrInitialized=%d fgCtx=%p swapchainCtx=%p owner=%s log=%llu\n",
                reason ? reason : "unknown", g_FsrEnabled ? 1 : 0, g_FsrInitialized ? 1 : 0, (void*)g_FfxCtx,
                (void*)g_FfxSwapChainCtx, SwapChainOwnerName(g_SwapChainOwner),
                static_cast<unsigned long long>(skipLogCount));
        }
        return;
    }
    if (!g_AsyncRuntimePreload || g_SlInitialized || g_SlModule || g_StreamlinePreloadInProgress.load() ||
        g_StreamlinePreloadStarted.exchange(true)) {
        return;
    }
    std::string reasonText = reason ? reason : "unknown";
    g_StreamlinePreloadInProgress = true;
    g_StreamlinePreloadSucceeded = false;
    testapp::Log("[FG-DIAG] Async Streamline preload scheduled reason=%s\n", reasonText.c_str());
    g_StreamlinePreloadThread = std::thread([reasonText]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        bool loaded = LoadStreamlineAndInitSerialized(reasonText.c_str());
        g_StreamlinePreloadSucceeded = loaded;
        g_StreamlinePreloadInProgress = false;
        testapp::Log("[FG-DIAG] Async Streamline preload finished reason=%s ok=%d\n", reasonText.c_str(),
                     loaded ? 1 : 0);
        testapp::LogFlush();
    });
}

static void JoinAsyncRuntimePreloadThreads(const char* reason) {
    if (g_FsrPreloadThread.joinable()) {
        testapp::Log("[FG-DIAG] Joining async FSR runtime preload reason=%s inProgress=%d succeeded=%d\n",
                     reason ? reason : "unknown", g_FsrPreloadInProgress.load() ? 1 : 0,
                     g_FsrPreloadSucceeded.load() ? 1 : 0);
        g_FsrPreloadThread.join();
    }
    if (g_StreamlinePreloadThread.joinable()) {
        testapp::Log("[FG-DIAG] Joining async Streamline preload reason=%s inProgress=%d succeeded=%d\n",
                     reason ? reason : "unknown", g_StreamlinePreloadInProgress.load() ? 1 : 0,
                     g_StreamlinePreloadSucceeded.load() ? 1 : 0);
        g_StreamlinePreloadThread.join();
    }
}

static void ReleaseDX12RendererResourcesForSwitch(const char* reason) {
    if (g_Device) {
        testapp::Log("[FG-DIAG] Releasing DX12 renderer resources for runtime switch (%s) owner=%s streamline=%d\n",
                     reason ? reason : "runtime switch", SwapChainOwnerName(g_SwapChainOwner),
                     g_SwapChainUsesStreamline ? 1 : 0);
        WaitForGpu();
    }
    // Device-bound upscaling state must die with the renderer (idempotent if already gone).
    DestroyFSRUpscaleContext();
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
    g_SwapChainBufferCount = kRequestedBackBuffers;
    for (UINT i = 0; i < kMaxSwapChainBuffers; ++i) {
        g_FenceValues[i] = 0;
    }
}

static bool ReinitializeDX12ForFSR(const char* reason) {
    ReleaseDX12RendererResourcesForSwitch(reason);
    if (g_SlInitialized || g_SlModule) {
        testapp::Log("[FG-DIAG] Shutting down Streamline before creating FSR renderer (%s)\n",
                     reason ? reason : "enter FSR");
        ShutdownStreamlineSerialized(reason ? reason : "enter FSR");
    }
    return InitDX12(g_Hwnd, true, reason ? reason : "enter FSR mode");
}

// Leaving DLSS mode for OFF must fully tear down the Streamline proxy swapchain and recreate a
// truly native one: keeping the proxy would keep its present pacer alive, and Reflex must stay on
// while that pacer presents (turning it off wedges the GPU; see dx12_fg_switch_streamline.inl).
// Only the teardown lets Reflex genuinely turn off, removing its frame cap in OFF mode -- exactly
// like a real game that rebuilds presentation when FG is switched off for good.
static bool ReinitializeDX12ForNativeOff(const char* reason) {
    ReleaseDX12RendererResourcesForSwitch(reason);
    if (g_SlInitialized || g_SlModule) {
        ShutdownStreamlineSerialized(reason ? reason : "enter OFF mode");
    }
    return InitDX12(g_Hwnd, false, reason ? reason : "enter OFF mode");
}

static bool EnsureStreamlineReadyForDLSS(const char* reason) {
    if (!g_SlInitialized) {
        testapp::Log("[FG-DIAG] Loading Streamline on demand for DLSS mode (%s)\n", reason ? reason : "unknown");
        if (!LoadStreamlineAndInitSerialized(reason ? reason : "DLSS mode")) {
            testapp::Log("[FG-DIAG] Streamline load failed for DLSS mode (%s)\n", reason ? reason : "unknown");
            return false;
        }
    }
    if (!g_SlDeviceSet && g_SlSetD3DDevice && g_Device) {
        sl::Result deviceResult = g_SlSetD3DDevice(g_Device.Get());
        g_SlDeviceSet = deviceResult == sl::Result::eOk;
        testapp::Log("[FG-DIAG] slSetD3DDevice(%s) result=%d (%s)\n", reason ? reason : "DLSS mode",
                     static_cast<int>(deviceResult), SlResultName(deviceResult));
    }
    if (!g_SlDeviceSet) {
        testapp::Log("[FG-DIAG] Streamline device binding unavailable for DLSS mode (%s) device=%p setFn=%p\n",
                     reason ? reason : "unknown", g_Device.Get(), (void*)g_SlSetD3DDevice);
        return false;
    }
    if (!g_DlssInitialized) {
        g_DlssInitialized = TryInitDLSSFG();
        testapp::Log("[FG-DIAG] DLSS init on demand (%s) state=%d\n", reason ? reason : "DLSS mode",
                     g_DlssInitialized ? 1 : 0);
    }
    return g_DlssInitialized;
}

static bool ToggleCurrentFGSuspension(FGMode mode, const char* reason, UINT frameIndex) {
    g_SuspensionTogglePending = false;
    if (mode != g_CurrentMode) {
        testapp::Log("[FG-DIAG] Ignoring suspension toggle for %s while current mode is %s (%s)\n", ModeName(mode),
                     ModeName(g_CurrentMode), reason ? reason : "unknown");
        testapp::LogFlush();
        return false;
    }

    bool ok = false;
    if (mode == FGMode::FSR) {
        if (!g_FfxCtx || !g_FsrInitialized) {
            testapp::Log("[FG-DIAG] Cannot toggle FSR suspension: ctx=%p initialized=%d enabled=%d suspended=%d\n",
                         (void*)g_FfxCtx, g_FsrInitialized ? 1 : 0, g_FsrEnabled ? 1 : 0, g_FsrSuspended ? 1 : 0);
            testapp::LogFlush();
            return false;
        }

        const bool enable = g_FsrSuspended || !g_FsrEnabled;
        ID3D12Resource* backbuffer = frameIndex < g_SwapChainBufferCount ? g_RenderTargets[frameIndex].Get() : nullptr;
        ok = ConfigureFSR(enable, backbuffer, enable ? "manual resume FSR FG" : "manual suspend FSR FG", true);
        if (ok) {
            g_FsrEnabled = true;
            g_FsrSuspended = !enable;
            g_LastFsrSuspendResumeToggleTime = std::chrono::high_resolution_clock::now();
            g_LastFsrSuspendResumeToggleFrameId = g_FrameIdCounter;
            if (enable) {
                RegisterFSRUiResource();
            }
        }
        testapp::Log("[FG-DIAG] Manual FSR suspension toggle: %s ok=%d frameID=%llu frameIndex=%u ctx=%p\n",
                     enable ? "resume" : "suspend", ok ? 1 : 0, static_cast<unsigned long long>(g_FrameIdCounter),
                     frameIndex, (void*)g_FfxCtx);
    } else if (mode == FGMode::DLSS) {
        if (!g_DlssInitialized || !g_SlDLSSGSetOptions) {
            testapp::Log(
                "[FG-DIAG] Cannot toggle DLSS suspension: initialized=%d setOptions=%p enabled=%d "
                "suspended=%d\n",
                g_DlssInitialized ? 1 : 0, (void*)g_SlDLSSGSetOptions, g_DlssEnabled ? 1 : 0, g_DlssSuspended ? 1 : 0);
            testapp::LogFlush();
            return false;
        }

        const bool enable = g_DlssSuspended || !g_DlssEnabled;
        ok = SetDLSSFGMode(enable);
        if (ok) {
            g_DlssEnabled = enable;
            g_DlssSuspended = !enable;
            if (enable) {
                PollDLSSFGState();
            }
        }
        testapp::Log("[FG-DIAG] Manual DLSS suspension toggle: %s ok=%d frameID=%llu frameIndex=%u\n",
                     enable ? "resume" : "suspend", ok ? 1 : 0, static_cast<unsigned long long>(g_FrameIdCounter),
                     frameIndex);
    }

    g_LastModeSwitchFrameId = g_FrameIdCounter;
    testapp::Log(
        "[FG-DIAG] Suspension toggle state: mode=%s ok=%d fsr=%d fsrSuspended=%d dlss=%d "
        "dlssSuspended=%d\n",
        ModeName(g_CurrentMode), ok ? 1 : 0, g_FsrEnabled ? 1 : 0, g_FsrSuspended ? 1 : 0, g_DlssEnabled ? 1 : 0,
        g_DlssSuspended ? 1 : 0);
    UpdateWindowTitle();
    testapp::LogFlush();
    return ok;
}

static bool SwitchMode(FGMode target, const char* reason, UINT frameIndex) {
    if (target == g_CurrentMode && !g_ModeSwitchPending) {
        return true;
    }

    testapp::Log("[FG-DIAG] Switching FG mode: %s -> %s (%s frameID=%llu frameIndex=%u)\n", ModeName(g_CurrentMode),
                 ModeName(target), reason ? reason : "unknown", static_cast<unsigned long long>(g_FrameIdCounter),
                 frameIndex);

    if (g_FsrExitTransitionStage == testapp::fg::FsrExitTransitionStage::ReplacementPresentPending) {
        if (g_SwapChain && g_SwapChainUsesStreamline) {
            const bool defersDlssActivation = testapp::fg::ShouldDeferDlssActivationUntilReplacementPresent(
                target == FGMode::DLSS, g_FsrExitTransitionStage);
            testapp::Log(
                "[FG-DIAG] DLSS replacement passthrough Present still pending; deferring mode switch "
                "(target=%s deferDlssActivation=%d frameID=%llu swapChain=%p)\n",
                ModeName(target), defersDlssActivation ? 1 : 0, static_cast<unsigned long long>(g_FrameIdCounter),
                g_SwapChain.Get());
            testapp::LogFlush();
            return true;
        }
        testapp::Log(
            "[FG-DIAG] WARN cannot retry DLSS replacement passthrough Present because its proxy "
            "topology is unavailable; restarting the requested transition (target=%s swapChain=%p "
            "streamline=%d)\n",
            ModeName(target), g_SwapChain.Get(), g_SwapChainUsesStreamline ? 1 : 0);
        g_FsrExitTransitionStage = testapp::fg::FsrExitTransitionStage::None;
        g_FsrRuntimeRetirementPendingForDlss = false;
    } else if (g_FsrExitTransitionStage == testapp::fg::FsrExitTransitionStage::ReplacementPresented) {
        testapp::Log(
            "[FG-DIAG] DLSS replacement passthrough Present completed; %s "
            "(target=%s frameID=%llu swapChain=%p)\n",
            target == FGMode::DLSS ? "activation may proceed" : "following the newer mode request", ModeName(target),
            static_cast<unsigned long long>(g_FrameIdCounter), g_SwapChain.Get());
        g_FsrExitTransitionStage = testapp::fg::FsrExitTransitionStage::None;
        if (target == FGMode::FSR) {
            g_FsrRuntimeRetirementPendingForDlss = false;
        }
    }
    WaitForGpu();

    bool ok = true;
    if (target == FGMode::FSR && g_FsrExitTransitionStage != testapp::fg::FsrExitTransitionStage::None) {
        testapp::Log(
            "[FG-DIAG] Cancelling staged FSR exit because the pending target returned to FSR "
            "(stage=%d frameID=%llu)\n",
            static_cast<int>(g_FsrExitTransitionStage), static_cast<unsigned long long>(g_FrameIdCounter));
        g_FsrExitTransitionStage = testapp::fg::FsrExitTransitionStage::None;
    }

    const testapp::fg::FsrExitTransitionAction fsrExitAction = testapp::fg::ResolveFsrExitTransitionAction(
        g_CurrentMode == FGMode::FSR, target == FGMode::FSR, g_FsrEnabled, g_FsrExitTransitionStage);
    bool fsrExitHandled = false;
    if (fsrExitAction == testapp::fg::FsrExitTransitionAction::PresentPassthrough) {
        fsrExitHandled = true;
        if (g_SwapChain && g_SwapChainOwner == SwapChainOwner::FSR && g_FfxSwapChainCtx) {
            testapp::Log(
                "[FG-DIAG] FSR exit passthrough Present still pending; deferring teardown "
                "(target=%s frameID=%llu)\n",
                ModeName(target), static_cast<unsigned long long>(g_FrameIdCounter));
            testapp::LogFlush();
            return true;
        }
        testapp::Log(
            "[FG-DIAG] WARN cannot retry FSR exit passthrough Present because its proxy topology "
            "is unavailable; continuing teardown (target=%s swapChain=%p owner=%s ctx=%p)\n",
            ModeName(target), g_SwapChain.Get(), SwapChainOwnerName(g_SwapChainOwner), (void*)g_FfxSwapChainCtx);
        g_FsrExitTransitionStage = testapp::fg::FsrExitTransitionStage::PassthroughPresented;
    } else if (fsrExitAction == testapp::fg::FsrExitTransitionAction::DisableAndPresentPassthrough) {
        fsrExitHandled = true;
        const bool disabled = ConfigureFSR(false, nullptr, "leave FSR mode", true);
        g_FsrEnabled = false;
        ok = ok && disabled;
        if (disabled && g_SwapChain && g_SwapChainOwner == SwapChainOwner::FSR && g_FfxSwapChainCtx) {
            // Keep the disabled proxy alive for one ordinary application Present. For a DLSS
            // target it remains alive after that Present while Streamline is initialized, so the
            // cold runtime load cannot remove the only visible DWM presentation surface.
            g_FsrSuspended = true;
            g_FsrExitTransitionStage = testapp::fg::FsrExitTransitionStage::PresentPending;
            testapp::Log(
                "[FG-DIAG] FSR disabled; staging one passthrough Present before teardown "
                "(target=%s frameID=%llu swapChain=%p ctx=%p)\n",
                ModeName(target), static_cast<unsigned long long>(g_FrameIdCounter), g_SwapChain.Get(),
                (void*)g_FfxSwapChainCtx);
            UpdateWindowTitle();
            testapp::LogFlush();
            return true;
        }
        testapp::Log(
            "[FG-DIAG] WARN FSR exit cannot stage a passthrough Present; using immediate teardown "
            "(disabled=%d target=%s swapChain=%p owner=%s ctx=%p)\n",
            disabled ? 1 : 0, ModeName(target), g_SwapChain.Get(), SwapChainOwnerName(g_SwapChainOwner),
            (void*)g_FfxSwapChainCtx);
        ResetFSRSuspensionStressState("leave FSR mode without passthrough Present");
        ok = ok && WaitForFSRSwapChainPresents("leave FSR mode");
        WaitForGpu();
    }
    if (g_FsrExitTransitionStage == testapp::fg::FsrExitTransitionStage::PassthroughPresented) {
        fsrExitHandled = true;
        const bool dlssReady = g_SlInitialized && g_SlDeviceSet && g_DlssInitialized;
        if (testapp::fg::ShouldPrepareDlssBeforeFsrPresentationBreak(true, target == FGMode::DLSS, dlssReady,
                                                                     g_FsrExitTransitionStage)) {
            testapp::Log(
                "[FG-DIAG] FSR exit passthrough Present completed; preparing Streamline while the "
                "disabled FSR presentation surface remains alive (frameID=%llu swapChain=%p ctx=%p)\n",
                static_cast<unsigned long long>(g_FrameIdCounter), g_SwapChain.Get(), (void*)g_FfxSwapChainCtx);
            testapp::LogFlush();
            if (!EnsureStreamlineReadyForDLSS("prepare DLSS before FSR presentation break")) {
                testapp::Log(
                    "[FG-DIAG] Streamline preparation failed before FSR presentation break; "
                    "rolling back to active FSR without destroying its proxy\n");
                g_FsrExitTransitionStage = testapp::fg::FsrExitTransitionStage::None;
                if (g_SlInitialized || g_SlModule) {
                    ShutdownStreamlineSerialized("rollback failed DLSS preparation");
                }
                ResetFSRSuspensionStressState("rollback failed DLSS preparation");
                const UINT activeFrameIndex = g_FrameIndex < g_SwapChainBufferCount ? g_FrameIndex : frameIndex;
                const bool resumed = ConfigureFSR(
                    true, activeFrameIndex < g_SwapChainBufferCount ? g_RenderTargets[activeFrameIndex].Get() : nullptr,
                    "rollback failed DLSS preparation", true);
                g_FsrEnabled = resumed;
                if (resumed) {
                    RegisterFSRUiResource();
                }
                g_ModeSwitchPending = false;
                UpdateWindowTitle();
                testapp::Log("[FG-DIAG] FSR rollback after failed DLSS preparation resumed=%d current=%s\n",
                             resumed ? 1 : 0, ModeName(g_CurrentMode));
                testapp::LogFlush();
                return false;
            }
        }
        const bool preparedDlss = g_SlInitialized && g_SlDeviceSet && g_DlssInitialized;
        if (!testapp::fg::CanCommitFsrPresentationBreak(true, target == FGMode::DLSS, preparedDlss,
                                                        g_FsrExitTransitionStage)) {
            testapp::Log(
                "[FG-DIAG] WARN refusing FSR presentation break before its DLSS replacement is ready "
                "(target=%s slInit=%d slDevice=%d dlssInit=%d)\n",
                ModeName(target), g_SlInitialized ? 1 : 0, g_SlDeviceSet ? 1 : 0, g_DlssInitialized ? 1 : 0);
            testapp::LogFlush();
            return true;
        }
        testapp::Log(
            "[FG-DIAG] FSR exit replacement ready; committing presentation break "
            "(target=%s frameID=%llu slInit=%d slDevice=%d dlssInit=%d)\n",
            ModeName(target), static_cast<unsigned long long>(g_FrameIdCounter), g_SlInitialized ? 1 : 0,
            g_SlDeviceSet ? 1 : 0, g_DlssInitialized ? 1 : 0);
        ResetFSRSuspensionStressState("leave FSR mode after passthrough Present");
        ok = ok && WaitForFSRSwapChainPresents("leave FSR mode after passthrough Present");
        WaitForGpu();
    } else if (!fsrExitHandled && g_FsrEnabled) {
        const bool disabled = ConfigureFSR(false, nullptr, "leave FSR mode", true);
        g_FsrEnabled = false;
        ResetFSRSuspensionStressState("leave FSR mode");
        ok = ok && disabled;
        ok = ok && WaitForFSRSwapChainPresents("leave FSR mode");
        WaitForGpu();
    }
    if (g_DlssEnabled) {
        const bool disabled = SetDLSSFGMode(false);
        g_DlssEnabled = false;
        g_DlssSuspended = false;
        ok = ok && disabled;
        WaitForGpu();
    } else if (g_CurrentMode == FGMode::DLSS && g_DlssSuspended) {
        testapp::Log("[FG-DIAG] Leaving suspended DLSS mode without another disable call (%s)\n",
                     reason ? reason : "unknown");
        g_DlssSuspended = false;
    }

    if (target == FGMode::FSR) {
        if (!LoadFSRRuntimeSerialized("enter FSR mode") || !g_FfxCreateContext) {
            testapp::Log("[FG-DIAG] Cannot switch to FSR FG: FSR runtime is not loaded\n");
            ok = false;
        }
        if (ok && (g_SwapChainOwner != SwapChainOwner::FSR || !g_FfxSwapChainCtx)) {
            DestroyFSRContexts();
            g_FsrInitialized = false;
            ok = ReinitializeDX12ForFSR("enter FSR mode") && ok;
        }
        if (ok && !g_FsrInitialized) {
            g_FsrInitialized = TryInitFSR();
            ok = ok && g_FsrInitialized;
        }
        if (ok && UpscalingActive() && !g_FfxUpscaleCtx) {
            // Non-fatal: without the upscaler context the upscale stage falls back to TAA/TAAU.
            TryInitFSRUpscaleContext();
        }
        ResetFSRPresentCallbackStressState("enter FSR mode");
        UINT activeFrameIndex = g_FrameIndex < g_SwapChainBufferCount ? g_FrameIndex : frameIndex;
        if (ok &&
            ConfigureFSR(true,
                         activeFrameIndex < g_SwapChainBufferCount ? g_RenderTargets[activeFrameIndex].Get() : nullptr,
                         "enter FSR mode", true)) {
            g_FsrEnabled = true;
            ResetFSRSuspensionStressState("enter FSR mode");
            RegisterFSRUiResource();
        } else if (ok) {
            testapp::Log("[FG-DIAG] FSR FG enable failed during switch\n");
            ok = false;
        }
    } else if (target == FGMode::DLSS) {
        const bool enteringFromFsr = g_SwapChainOwner == SwapChainOwner::FSR || g_FfxCtx || g_FfxSwapChainCtx;
        bool recreatedDlssSurface = false;
        if (ok && !EnsureStreamlineReadyForDLSS(enteringFromFsr ? "enter DLSS mode after FSR"
                                                                : "enter DLSS mode from native")) {
            ok = false;
        }
        if (ok && enteringFromFsr) {
            DestroyFSRContexts();
            g_FsrInitialized = false;
            // Streamline, its device binding, and its feature entry points were prepared while the
            // disabled FSR proxy was still the visible surface. Reuse the existing device/queue and
            // replace only swapchain-bound resources, matching the already smooth FSR->OFF handoff.
            recreatedDlssSurface = RecreateSwapChain(false, "enter DLSS mode after prepared FSR exit");
            ok = recreatedDlssSurface && ok;
            g_FsrRuntimeRetirementPendingForDlss = recreatedDlssSurface;
        }
        if (ok && !g_SwapChainUsesStreamline) {
            // Native->DLSS (including FSR->OFF->DLSS) also prepares Streamline before releasing the
            // visible native chain, then performs the shortest possible swapchain-only handoff.
            recreatedDlssSurface = RecreateSwapChain(false, "enter DLSS mode from prepared native surface");
            ok = recreatedDlssSurface && ok;
        }
        if (ok && recreatedDlssSurface) {
            // A newly created Streamline proxy has not displayed anything yet. Enabling DLSS-G now
            // makes its first Present also perform the feature's large lazy resource/model setup,
            // leaving DWM without a replacement image in the meantime. Present one covered FG-off
            // frame through this same proxy first; the pending request activates DLSS next frame.
            g_CurrentMode = FGMode::Off;
            g_FsrExitTransitionStage = testapp::fg::FsrExitTransitionStage::ReplacementPresentPending;
            g_Taa.Reset();
            testapp::Log(
                "[FG-DIAG] DLSS replacement surface ready; staging one covered FG-off passthrough "
                "Present before activation (frameID=%llu swapChain=%p fromFsr=%d)\n",
                static_cast<unsigned long long>(g_FrameIdCounter), g_SwapChain.Get(), enteringFromFsr ? 1 : 0);
            UpdateWindowTitle();
            testapp::LogFlush();
            return true;
        }
        if (!g_DlssInitialized) {
            testapp::Log("[FG-DIAG] Cannot switch to DLSS FG: Streamline DLSS-G was not initialized\n");
            ok = false;
        } else if (ok && SetDLSSFGMode(true)) {
            g_DlssEnabled = true;
            g_DlssSuspended = false;
            // Anchor the one-shot DLSS stress interval to the FG enable. Without this the stress
            // timer (armed at startup) was already expired on entering DLSS mode and suspended FG in
            // the SAME Render iteration / frame token as the enable, before a single present consumed
            // it -- Streamline flags that as "Repeated slDLSSGSetOptions() call ... race condition
            // with Present()" and it leaves the pacer half-initialized. The stress must exercise the
            // realistic sequence: FG actually generates frames for the interval, THEN suspends.
            g_LastDlssSuspendResumeToggleTime = std::chrono::high_resolution_clock::now();
            if (UpscalingActive()) {
                SetDLSSSROptions(true);
            }
            PollDLSSFGState();
        } else if (ok) {
            testapp::Log("[FG-DIAG] DLSS FG enable failed during switch\n");
            ok = false;
        }
    } else {
        if (ok && g_SwapChainOwner == SwapChainOwner::FSR && g_FfxCtx && g_FfxSwapChainCtx) {
            testapp::Log(
                "[FG-DIAG] OFF mode destroys the FSR swapchain/context and recreates a native swapchain "
                "(oldSwapChain=%p swapchainCtx=%p fgCtx=%p)\n",
                g_SwapChain.Get(), (void*)g_FfxSwapChainCtx, (void*)g_FfxCtx);
            DestroyFSRContexts();
            g_FsrInitialized = false;
            ok = RecreateSwapChain(false, "enter OFF mode after FSR") && ok;
            MaybeUnloadFSRRuntimeAfterSwitch("enter OFF mode after FSR");
            StartAsyncFSRRuntimePreload("after entering OFF mode from FSR");
        } else if (ok && (g_FfxCtx || g_FfxSwapChainCtx)) {
            DestroyFSRContexts();
            g_FsrInitialized = false;
            ok = RecreateSwapChain(false, "enter OFF mode") && ok;
            MaybeUnloadFSRRuntimeAfterSwitch("enter OFF mode");
            StartAsyncFSRRuntimePreload("after entering OFF mode");
        } else if (ok && (g_SwapChainUsesStreamline || g_SlInitialized || g_SlModule)) {
            testapp::Log(
                "[FG-DIAG] OFF mode tears down the Streamline proxy swapchain and Streamline itself, then "
                "recreates a native swapchain (swapChain=%p streamline=%d) so Reflex can genuinely turn off\n",
                g_SwapChain.Get(), g_SwapChainUsesStreamline ? 1 : 0);
            ok = ReinitializeDX12ForNativeOff("enter OFF mode after DLSS") && ok;
        }
        if (ok && g_FsrRuntimeRetirementPendingForDlss) {
            MaybeUnloadFSRRuntimeAfterSwitch("after cancelled DLSS replacement entered OFF");
            StartAsyncFSRRuntimePreload("after cancelled DLSS replacement entered OFF");
            g_FsrRuntimeRetirementPendingForDlss = false;
        }
    }

    if (!ok) {
        if (g_FsrRuntimeRetirementPendingForDlss) {
            if (g_SwapChain) {
                MaybeUnloadFSRRuntimeAfterSwitch("after failed DLSS activation");
                StartAsyncFSRRuntimePreload("after failed DLSS activation");
            }
            g_FsrRuntimeRetirementPendingForDlss = false;
        }
        if (target != FGMode::Off) {
            target = g_FsrEnabled ? FGMode::FSR : ((g_DlssEnabled || g_DlssSuspended) ? FGMode::DLSS : FGMode::Off);
        }
        if (!g_SwapChain) {
            testapp::Log("[FG-DIAG] Fatal switch failure: no swapchain after %s request; stopping main loop\n",
                         ModeName(target));
            g_Running = false;
        }
    }
    g_CurrentMode = target;
    g_ModeSwitchPending = false;
    g_LastModeSwitchFrameId = g_FrameIdCounter;
    // Temporal history from the previous mode is meaningless after a switch (different upscaler /
    // recreated resources); restart accumulation cleanly.
    g_Taa.Reset();
    if (ok && target == FGMode::FSR) {
        StartAsyncStreamlinePreload("after entering FSR mode");
    }
    testapp::Log("[FG-DIAG] Mode now %s (ok=%d fsr=%d fsrSuspended=%d dlss=%d dlssSuspended=%d)\n",
                 ModeName(g_CurrentMode), ok ? 1 : 0, g_FsrEnabled ? 1 : 0, g_FsrSuspended ? 1 : 0,
                 g_DlssEnabled ? 1 : 0, g_DlssSuspended ? 1 : 0);
    UpdateWindowTitle();
    testapp::LogFlush();
    return ok;
}

