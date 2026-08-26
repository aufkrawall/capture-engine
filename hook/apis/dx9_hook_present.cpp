#include "dx9_hook_internal.h"

// Present hook helpers
void DX9_PresentBegin(IDirect3DDevice9* device, IDirect3DSurface9*& backBuffer) {
    if (HookIsShuttingDown())
        return;
    if (ShouldBypassDX9HooksForDevice(device))
        return;
    if (ShouldSkipDX9PresentForVulkan())
        return;

    // Heartbeat for freeze watchdog (d3d12.dll may be loaded in DX9 games)
    g_RenderWatchdog.Heartbeat();

    static std::atomic<bool> s_LiveHookBootstrapDone{false};
    bool expectedBootstrap = false;
    if ((!dx9_hook_oSetSamplerState || !dx9_hook_oGetSamplerState || !dx9_hook_oSetTexture || !dx9_hook_oSetTextureStageState || !dx9_hook_oReset || !dx9_hook_oEndScene) &&
        s_LiveHookBootstrapDone.compare_exchange_strong(expectedBootstrap, true, std::memory_order_acq_rel,
                                                        std::memory_order_relaxed)) {
        HookLogImportant(
            "DX9: Present bootstrap before install (device=%p, inline=%d, "
            "oReset=%p, oEndScene=%p, oSetTexture=%p, oGetSamplerState=%p, oSetSamplerState=%p, "
            "oSetTextureStageState=%p)",
            device, dx9_hook_g_InlineHooksInstalled.load(std::memory_order_acquire) ? 1 : 0, (void*)dx9_hook_oReset, (void*)dx9_hook_oEndScene,
            (void*)dx9_hook_oSetTexture, (void*)dx9_hook_oGetSamplerState, (void*)dx9_hook_oSetSamplerState, (void*)dx9_hook_oSetTextureStageState);
        InstallDeviceHooks(device);
        HookLogImportant(
            "DX9: Present bootstrap after install (oReset=%p, oSetTexture=%p, oGetSamplerState=%p, "
            "oSetSamplerState=%p, oSetTextureStageState=%p, oEndScene=%p)",
            (void*)dx9_hook_oReset, (void*)dx9_hook_oSetTexture, (void*)dx9_hook_oGetSamplerState, (void*)dx9_hook_oSetSamplerState,
            (void*)dx9_hook_oSetTextureStageState, (void*)dx9_hook_oEndScene);

        IDirect3DSwapChain9* swapChain = nullptr;
        if (SUCCEEDED(device->GetSwapChain(0, &swapChain)) && swapChain) {
            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
            D3DPRESENT_PARAMETERS pp = {};
            if (SUCCEEDED(swapChain->GetPresentParameters(&pp))) {
                dx9_hook_g_WindowedPresent = !!pp.Windowed;
                dx9_hook_g_LivePresentInterval.store(pp.PresentationInterval, std::memory_order_release);
                HookLogImportant("DX9: Live present params windowed=%d interval=%u backBufferCount=%u",
                                 pp.Windowed ? 1 : 0, pp.PresentationInterval, pp.BackBufferCount);
                const auto& gfx = GetActiveGraphicsConfig();
                VSyncOverride vsync = GetVSyncOverride();
                if (vsync.shouldOverride && pp.PresentationInterval != (UINT)vsync.presentInterval) {
                    const bool fullscreenFallback = !pp.Windowed && vsync.presentInterval > 0 &&
                                                    pp.PresentationInterval != (UINT)vsync.presentInterval;
                    HookLogImportant(
                        "DX9: Live interval mismatch cfg=%s desired=%u actual=%u "
                        "windowed=%d fallback=%s",
                        gfx.vsyncMode.c_str(), (UINT)vsync.presentInterval, pp.PresentationInterval,
                        pp.Windowed ? 1 : 0, fullscreenFallback ? "qpc" : "windowed-auto");
                }
                if (gfx.backbufferCount >= 2 && gfx.backbufferCount <= 6) {
                    const UINT desiredBackBufferCount = (UINT)gfx.backbufferCount - 1;
                    if (pp.BackBufferCount != desiredBackBufferCount) {
                        HookLogImportant("DX9: Live backbuffer mismatch cfg=%d desired=%u actual=%u",
                                         gfx.backbufferCount, desiredBackBufferCount, pp.BackBufferCount);
                    }
                }
            }
            swapChain->Release();
        }
    }

    static int debugLogCount = 0;
    if (debugLogCount < 10) {
        SharedMemoryLayout* dbgShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        auto overlayCfg = dbgShm ? dbgShm->ReadOverlayConfig() : OverlayConfig{};
        EarlyLog("DX9 Present #%d: IPC=%p, SHM=%p, showOverlay=%d, initialized=%d, inlineHooks=%d", debugLogCount,
                 (void*)g_IPC, (void*)dbgShm, overlayCfg.showOverlay ? 1 : 0, g_OverlayAdapter.IsInitialized() ? 1 : 0,
                 dx9_hook_g_InlineHooksInstalled.load() ? 1 : 0);
        debugLogCount++;
    }
    // Update frame config cache once per frame to avoid overhead in hot hooks
    dx9_hook_g_FrameConfig = GetActiveGraphicsConfig();
    const D3D9SamplerCallbacks samplerCallbacks = ResolveD3D9SamplerCallbacks(device);
    ce::dx9_sampler_state::RefreshConfiguration(device, samplerCallbacks.setSamplerState,
                                                samplerCallbacks.getSamplerState);

    // Start timing
    static int64_t qpcFreq = 0;
    if (qpcFreq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcFreq = f.QuadPart;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    dx9_hook_g_Timing.startTime = qpc.QuadPart;
    dx9_hook_g_Timing.fpsLimitTime = 0;
    dx9_hook_g_Timing.presentCallTime = 0;

    dx9_hook_g_PresentRecurse++;
    if (dx9_hook_g_PresentRecurse == 1) {
        std::lock_guard<std::mutex> lock(dx9_hook_g_PresentMutex);  // Protect against concurrent calls

        dx9_hook_g_overlayDrawnInPresentEndScene = false;
        dx9_hook_g_captureDeferredToPresentEndScene = false;
        dx9_hook_g_screenshotDeferredToPresentEndScene = 0;
        dx9_hook_g_sawPresentNestedEndScene = false;

        static bool luidReported = false;
        if (!luidReported) {
            IDirect3D9* d3d = nullptr;
            if (SUCCEEDED(device->GetDirect3D(&d3d))) {
                D3DDEVICE_CREATION_PARAMETERS cp;
                if (SUCCEEDED(device->GetCreationParameters(&cp))) {
                    IDirect3D9Ex* d3dEx = nullptr;
                    if (SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(&d3dEx)))) {
                        LUID luid;
                        if (SUCCEEDED(d3dEx->GetAdapterLUID(cp.AdapterOrdinal, &luid))) {
                            ReportLUID(luid.LowPart, luid.HighPart);
                            SystemMetricsCollector::Get().Initialize((int32_t)luid.LowPart, (int32_t)luid.HighPart);
                            luidReported = true;
                        }
                        d3dEx->Release();
                    }

                    // Fallback for non-Ex: map D3D9 adapter ordinal to a DXGI adapter
                    // index. This is usually correct on single-GPU systems and is good
                    // enough to feed the out-of-process metrics poller.
                    if (!luidReported) {
                        IDXGIFactory1* factory = nullptr;
                        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) && factory) {
                            IDXGIAdapter1* adapter = nullptr;
                            if (SUCCEEDED(factory->EnumAdapters1(cp.AdapterOrdinal, &adapter)) && adapter) {
                                DXGI_ADAPTER_DESC1 desc;
                                if (SUCCEEDED(adapter->GetDesc1(&desc))) {
                                    ReportLUID(desc.AdapterLuid.LowPart, desc.AdapterLuid.HighPart);
                                    SystemMetricsCollector::Get().Initialize((int32_t)desc.AdapterLuid.LowPart,
                                                                             (int32_t)desc.AdapterLuid.HighPart);
                                    SystemMetricsCollector::Get().SetVRAMTotal(desc.DedicatedVideoMemory);
                                    luidReported = true;
                                }
                                adapter->Release();
                            }
                            factory->Release();
                        }
                    }
                }
                d3d->Release();
            }
        }

        // Get backbuffer
        if (FAILED(device->GetRenderTarget(0, &backBuffer))) {
            backBuffer = nullptr;
        }

        // ... (logging every 60 frames) ...
        static int frameCount = 0;
        frameCount++;
        IPCClient* ipc = g_IPC;

        // Draw overlay
        int64_t overlayStart = 0;
        QueryPerformanceCounter(&qpc);
        overlayStart = qpc.QuadPart;

        SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        bool captureIncludeOverlay = shm ? shm->overlayConfig.captureIncludeOverlay : true;
        static int isTrine3Process = -1;
        if (isTrine3Process < 0) {
            char exePath[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, exePath, MAX_PATH);
            const char* exeName = strrchr(exePath, '\\');
            exeName = exeName ? (exeName + 1) : exePath;
            isTrine3Process = (_stricmp(exeName, "trine3.exe") == 0) ? 1 : 0;
        }
        const bool dxvkVulkanCapture =
            IsDXVKD3D9WrapperLoaded() && shm && shm->runtimeState.vulkanLayerActive.load(std::memory_order_acquire);
        static bool dxvkVulkanCaptureLogged = false;
        if (dxvkVulkanCapture && !dxvkVulkanCaptureLogged) {
            HookLogImportant("DX9: DXVK+VulkanLayer mode - deferring capture and FPS limiter to Vulkan layer");
            dxvkVulkanCaptureLogged = true;
        }
        const bool skipDX9Overlay = ShouldSkipDX9OverlayForVulkan();
        bool shouldDrawOverlay = shm && shm->overlayConfig.showOverlay && !skipDX9Overlay;
        bool endSceneHookActive = false;
        uintptr_t* vtable = *(uintptr_t**)device;
        if (vtable && !skipDX9Overlay) {
            endSceneHookActive = ((void*)vtable[42] == (void*)&DetourEndScene);
            if (!endSceneHookActive) {
                void* driftedTarget = (void*)vtable[42];
                VTableHook::Status esStatus =
                    VTableHook::Create(&vtable[42], (void*)&DetourEndScene, (void**)&dx9_hook_oEndScene);
                endSceneHookActive = ((void*)vtable[42] == (void*)&DetourEndScene);
                static int endSceneRehookLogCount = 0;
                if (endSceneRehookLogCount < 12) {
                    HookLogImportant("DX9: EndScene hook drift detected target=%p status=%d active=%d next=%p",
                                     driftedTarget, (int)esStatus, endSceneHookActive ? 1 : 0, (void*)dx9_hook_oEndScene);
                    endSceneRehookLogCount++;
                }
            }
        }
        bool preferEndSceneOverlay = shouldDrawOverlay && endSceneHookActive;
        const bool captureAfterOverlay = captureIncludeOverlay;
        const bool captureBeforeOverlay = !captureAfterOverlay;
        const uint64_t screenshotRequestId = GetPendingScreenshotRequestId(shm);
        const bool screenshotRequested = screenshotRequestId != 0;
        const bool screenshotAfterOverlay =
            screenshotRequested && shouldDrawOverlay && (shm ? shm->overlayConfig.screenshotIncludeOverlay : true);
        const bool screenshotBeforeOverlay = screenshotRequested && !screenshotAfterOverlay;
        const bool deferCaptureToPresentEndScene = captureAfterOverlay && shouldDrawOverlay &&
                                                   dx9_hook_g_PreferOverlayInPresentEndScene.load(std::memory_order_acquire);
        const bool deferScreenshotToPresentEndScene = screenshotAfterOverlay && preferEndSceneOverlay;

        // Lambda for overlay drawing — skip if EndScene already handled it for this frame
        auto doOverlay = [&]() {
            if (shouldDrawOverlay && !preferEndSceneOverlay && !dx9_hook_g_overlayDrawnBeforePresent &&
                !dx9_hook_g_overlayDrawnInPresentEndScene) {
                DrawDX9Overlay(device);
            }
        };

        QueryPerformanceCounter(&qpc);
        dx9_hook_g_Timing.overlayTime = qpc.QuadPart - overlayStart;

        // CPU Prerender Limit
        int64_t prerenderStart = 0;
        QueryPerformanceCounter(&qpc);
        prerenderStart = qpc.QuadPart;

        if (!dxvkVulkanCapture) {
            float limit = GetActivePrerenderLimit();
            if (limit > -0.5f) {  // Active if >= 0.0
                dx9_hook_g_DX9Capture.WaitPrerender(device, limit);
            }
        }

        QueryPerformanceCounter(&qpc);
        dx9_hook_g_Timing.prerenderTime = qpc.QuadPart - prerenderStart;

        // Capture logic
        int64_t captureStart = 0;
        QueryPerformanceCounter(&qpc);
        captureStart = qpc.QuadPart;

        // Lambda for capture operation
        auto doCapture = [&]() {
            if (dxvkVulkanCapture) {
                if (dx9_hook_g_DX9Capture.initialized) {
                    HookLogImportant("DX9: DXVK+VulkanLayer active, cleaning up DX9 capture");
                    dx9_hook_g_DX9Capture.Cleanup();
                }
                return;
            }

            // Pre-initialize capture on first Present call (before recording starts).
            // This ensures shared texture handles are published to shared memory early,
            // so the media engine has valid data when recording begins.
            if (!dx9_hook_g_DX9Capture.initialized && !dx9_hook_g_DX9Capture.initializationFailed && backBuffer) {
                static bool earlyInitLogged = false;
                if (!earlyInitLogged) {
                    HookLogImportant("DX9: Pre-initializing capture on first Present (early init)");
                    earlyInitLogged = true;
                }
                dx9_hook_g_DX9Capture.Init(device);
            }

            // Log recording transitions, not a steady Present-path heartbeat.
            static bool captureStateLogged = false;
            static bool lastRec = false;
            bool isRec = ipc && ipc->IsRecording();
            if (!captureStateLogged || isRec != lastRec) {
                EarlyLog("DX9: Capture state frame=%d ipc=%p isRecording=%d initialized=%d backBuffer=%p", frameCount,
                         ipc, isRec ? 1 : 0, dx9_hook_g_DX9Capture.initialized, backBuffer);
                captureStateLogged = true;
                lastRec = isRec;
            }
            if (ipc && ipc->IsRecording()) {
                if (dx9_hook_g_DX9Capture.initialized && backBuffer) {
                    if (deferCaptureToPresentEndScene) {
                        static int deferredCaptureLogCount = 0;
                        if (deferredCaptureLogCount < 8) {
                            HookLogImportant("DX9: Deferring capture until nested EndScene overlay draw");
                            deferredCaptureLogCount++;
                        }
                        dx9_hook_g_captureDeferredToPresentEndScene = true;
                    } else {
                        SharedMemoryLayout* shm = ipc ? ipc->GetSharedMem() : nullptr;
                        if (!ShouldSkipCaptureForTargetCadence(shm, "DX9")) {
                            dx9_hook_g_DX9Capture.CaptureFrame(device, backBuffer);
                        }
                    }
                }
            }
            // Don't cleanup when recording stops - keep initialized for next recording
        };

        auto doScreenshot = [&]() {
            if (screenshotRequested && shm) {
                CaptureDX9Screenshot(device, shm, screenshotRequestId);
            }
        };

        if (captureBeforeOverlay) {
            doCapture();
        }
        if (screenshotBeforeOverlay) {
            doScreenshot();
        }
        doOverlay();
        if (captureAfterOverlay) {
            doCapture();
        }
        if (deferScreenshotToPresentEndScene) {
            dx9_hook_g_screenshotDeferredToPresentEndScene = screenshotRequestId;
        } else if (screenshotAfterOverlay) {
            doScreenshot();
        }

        QueryPerformanceCounter(&qpc);
        dx9_hook_g_Timing.captureTime = qpc.QuadPart - captureStart;

        // FPS limiter moved to PresentEnd (after PostPresentReadback) so that
        // SmartWait accounts for ALL hook overhead including readback.
        dx9_hook_g_Timing.fpsLimitTime = 0;

        if (backBuffer) {
            backBuffer->Release();
        }
    }
}

void DX9_PresentEnd(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer) {
    if (dx9_hook_g_PresentRecurse <= 0 || ShouldBypassDX9HooksForDevice(device)) {
        return;
    }

    // Complete deferred readback AFTER Present returned (GPU->CPU DMA no longer
    // blocks the Present call, reducing present_call_us from ~3.5ms to ~0.2ms)
    // Only run at the outermost Present level to avoid double-processing when
    // both VTable and inline hooks fire for the same call.
    if (dx9_hook_g_PresentRecurse == 1) {
        dx9_hook_g_DX9Capture.PostPresentReadback(device);
    }

    // Apply FPS limiter AFTER PostPresentReadback so SmartWait accounts for
    // all hook overhead (capture + present + readback). This eliminates the
    // bimodal frame time distribution where frames with variable query wait
    // had zero limiter wait.
    if (dx9_hook_g_PresentRecurse == 1) {
        static int64_t limiterFreq = 0;
        if (limiterFreq == 0) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            limiterFreq = f.QuadPart;
        }
        LARGE_INTEGER limiterQpc;
        QueryPerformanceCounter(&limiterQpc);
        int64_t fpsLimitStart = limiterQpc.QuadPart;
        g_SharedFpsLimiter.SetIPCClient(g_IPC);
        g_SharedFpsLimiter.Apply();
        QueryPerformanceCounter(&limiterQpc);
        dx9_hook_g_Timing.fpsLimitTime = limiterQpc.QuadPart - fpsLimitStart;

        // Update performance metrics AFTER limiter — the post-blocking QPC ensures
        // inter-frame intervals reflect the limited rate (e.g. 120fps, not 144fps).
        {
            int64_t us = (limiterQpc.QuadPart * 1000000) / limiterFreq;
            dx9_hook_g_PerfMetrics.Update(us);
        }

        // Update recording state for CSV logging
        bool isRecording = g_IPC && g_IPC->IsRecording();
        dx9_hook_g_PerfMetrics.SetRecording(isRecording);
    }

    if (dx9_hook_g_PresentRecurse == 1) {
        static int64_t qpcFreq = 0;
        if (qpcFreq == 0) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            qpcFreq = f.QuadPart;
        }
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        int64_t totalTime = qpc.QuadPart - dx9_hook_g_Timing.startTime;
        int64_t totalUs = (totalTime * 1000000) / qpcFreq;
        int64_t fpsLimitUs = (dx9_hook_g_Timing.fpsLimitTime * 1000000) / qpcFreq;

        // Merge present call timing from inline hooks (separate TLS due to decl order)
        if (dx9_hook_g_Timing.presentCallTime == 0 && dx9_hook_g_PresentCallTiming.presentCallTime != 0) {
            dx9_hook_g_Timing.presentCallTime = dx9_hook_g_PresentCallTiming.presentCallTime;
            dx9_hook_g_PresentCallTiming.presentCallTime = 0;
        }

        // Overhead excludes FPS limiter wait (intentional pacing, not overhead)
        int64_t overheadUs = totalUs - fpsLimitUs;

        // Log if actual overhead (excluding FPS limiter) is excessive (> 5ms)
        static thread_local int64_t s_LastOverheadWarnQpc = 0;
        static thread_local uint32_t s_SuppressedOverheadWarns = 0;
        if (overheadUs > 5000) {
            if (s_LastOverheadWarnQpc == 0 || (qpc.QuadPart - s_LastOverheadWarnQpc) >= qpcFreq) {
                if (s_SuppressedOverheadWarns > 0) {
                    HookLog(LogLevel::Warn,
                            "DX9: High Present Overhead detected: %lld us "
                            "(total=%lld us, fpsLimit=%lld us, %u suppressed)",
                            overheadUs, totalUs, fpsLimitUs, s_SuppressedOverheadWarns);
                    s_SuppressedOverheadWarns = 0;
                } else {
                    HookLog(LogLevel::Warn,
                            "DX9: High Present Overhead detected: %lld us "
                            "(total=%lld us, fpsLimit=%lld us)",
                            overheadUs, totalUs, fpsLimitUs);
                }
                s_LastOverheadWarnQpc = qpc.QuadPart;
            } else {
                ++s_SuppressedOverheadWarns;
            }
        }

        // Performance logging for PerfLogger
        if (PerfLogger::Get().IsEnabled()) {
            FrameMetrics perfMetrics;
            static uint64_t s_perfFrameNum = 0;
            perfMetrics.frameNum = ++s_perfFrameNum;
            perfMetrics.qpcUs = (dx9_hook_g_Timing.startTime * 1000000) / qpcFreq;
            perfMetrics.totalUs = static_cast<int32_t>(totalUs);
            perfMetrics.overlayUs = static_cast<int32_t>((dx9_hook_g_Timing.overlayTime * 1000000) / qpcFreq);
            perfMetrics.captureUs = static_cast<int32_t>((dx9_hook_g_Timing.captureTime * 1000000) / qpcFreq);
            perfMetrics.prerenderWaitUs = static_cast<int32_t>((dx9_hook_g_Timing.prerenderTime * 1000000) / qpcFreq);
            perfMetrics.fpsLimitWaitUs = static_cast<int32_t>(fpsLimitUs);
            perfMetrics.presentCallUs = static_cast<int32_t>((dx9_hook_g_Timing.presentCallTime * 1000000) / qpcFreq);
            strncpy(perfMetrics.api, "DX9", sizeof(perfMetrics.api) - 1);
            perfMetrics.api[sizeof(perfMetrics.api) - 1] = '\0';

            // DX9-specific capture breakdown (staging OR zero-copy)
            if (dx9_hook_g_DX9Capture.useD3D11Staging) {
                perfMetrics.stretchRectUs = dx9_hook_g_DX9Capture.stagingStretchRectUs;
                perfMetrics.readbackSubmitUs = dx9_hook_g_DX9Capture.stagingReadbackSubmitUs;
                perfMetrics.queryWaitUs = dx9_hook_g_DX9Capture.stagingQueryWaitUs;
                perfMetrics.lockRectUs = dx9_hook_g_DX9Capture.stagingLockRectUs;
                perfMetrics.d3d11UploadUs = dx9_hook_g_DX9Capture.stagingD3D11UploadUs;
                perfMetrics.stagingDepth = dx9_hook_g_DX9Capture.stagingCurrentDepth;
                perfMetrics.stagingDropped = dx9_hook_g_DX9Capture.stagingTotalDropped;
            } else {
                perfMetrics.queryWaitUs = dx9_hook_g_DX9Capture.zeroCopyQueryWaitUs;
                perfMetrics.readbackSubmitUs = dx9_hook_g_DX9Capture.zeroCopyReadbackUs;
            }

            PerfLogger::Get().LogFrame(perfMetrics);
        }

        // Per-second capture stats summary to hook_debug.log
        if (dx9_hook_g_DX9Capture.useD3D11Staging) {
            static thread_local int64_t s_StatsLastQpc = 0;
            static thread_local uint32_t s_StatsFrameCount = 0;
            static thread_local int64_t s_StatsTotalSubmitUs = 0;
            static thread_local int64_t s_StatsTotalConsumeUs = 0;
            static thread_local uint32_t s_StatsDropped = 0;

            s_StatsFrameCount++;
            s_StatsTotalSubmitUs += dx9_hook_g_DX9Capture.stagingStretchRectUs + dx9_hook_g_DX9Capture.stagingReadbackSubmitUs;
            s_StatsTotalConsumeUs +=
                dx9_hook_g_DX9Capture.stagingQueryWaitUs + dx9_hook_g_DX9Capture.stagingLockRectUs + dx9_hook_g_DX9Capture.stagingD3D11UploadUs;

            if (s_StatsLastQpc == 0) {
                s_StatsLastQpc = qpc.QuadPart;
            } else if ((qpc.QuadPart - s_StatsLastQpc) >= qpcFreq) {
                const int64_t avgSubmitUs = s_StatsFrameCount > 0 ? s_StatsTotalSubmitUs / s_StatsFrameCount : 0;
                const int64_t avgConsumeUs = s_StatsFrameCount > 0 ? s_StatsTotalConsumeUs / s_StatsFrameCount : 0;
                HookLog(LogLevel::Info,
                        "DX9 Capture Stats: %u frames, avg submit=%lld us, "
                        "avg consume=%lld us, pipeline depth=%d/%d, dropped=%u",
                        s_StatsFrameCount, avgSubmitUs, avgConsumeUs, dx9_hook_g_DX9Capture.stagingCurrentDepth,
                        CAPTURE_TEXTURE_COUNT, dx9_hook_g_DX9Capture.stagingTotalDropped);
                s_StatsFrameCount = 0;
                s_StatsTotalSubmitUs = 0;
                s_StatsTotalConsumeUs = 0;
                s_StatsDropped = 0;
                s_StatsLastQpc = qpc.QuadPart;
            }
        }
    }

    if (dx9_hook_g_PresentRecurse == 1) {
        if (dx9_hook_g_PreferOverlayInPresentEndScene.load(std::memory_order_acquire) && !dx9_hook_g_sawPresentNestedEndScene &&
            !IsD3D9On12Loaded()) {
            dx9_hook_g_PreferOverlayInPresentEndScene.store(false, std::memory_order_release);
            static int nestedFallbackLogCount = 0;
            if (nestedFallbackLogCount < 8) {
                HookLogImportant(
                    "DX9: Nested EndScene missing during Present, falling back to top-level EndScene overlay");
                nestedFallbackLogCount++;
            }
        }
        dx9_hook_g_captureDeferredToPresentEndScene = false;
        dx9_hook_g_screenshotDeferredToPresentEndScene = 0;
        dx9_hook_g_overlayDrawnBeforePresent = false;
        dx9_hook_g_overlayDrawnInPresentEndScene = false;
        dx9_hook_g_sawPresentNestedEndScene = false;
    }
    dx9_hook_g_PresentRecurse--;
}
