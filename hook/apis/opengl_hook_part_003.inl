            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC desc;
                adapter->GetDesc(&desc);

                uint32_t lLow = desc.AdapterLuid.LowPart;
                uint32_t lHigh = desc.AdapterLuid.HighPart;

                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                SystemMetricsCollector::Get().Initialize(lLow, lHigh);
                ReportLUID(lLow, lHigh);

                g_LuidReported = true;
                HookLog("OpenGL: GPU Detected via D3D11 Interop (LUID: %08x)", lLow);

                // Prevent DXGI from associating with the game window (cursor theft)
                IDXGIFactory* factory = nullptr;
                if (SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
                    HWND wnd = WindowFromDC(hdc);
                    if (wnd)
                        factory->MakeWindowAssociation(wnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
                    factory->Release();
                }

                adapter->Release();
            }
            dxgiDevice->Release();
        }
        context->Release();
        device->Release();
    }
}

// Draw overlay using CustomOverlay
static void DrawOpenGLOverlay(HDC hdc) {
    if (IsVulkanPrimary())
        return;

    HGLRC currentCtx = wglGetCurrentContext();
    if (!currentCtx)
        return;
    if (!TrackOpenGLContext(hdc))
        return;

    static bool initLogged = false;
    if (!g_OverlayAdapter.IsInitialized()) {
        if (!initLogged) {
            HookLog(
                "OpenGL: DrawOpenGLOverlay - OverlayAdapter not initialized, "
                "calling InitOpenGL...");
            initLogged = true;
        }

        DetectGPU(hdc);
        HWND hwnd = WindowFromDC(hdc);
        if (hwnd) {
            g_CachedHwnd = hwnd;
            InputManager::Get().HookWindow(hwnd);
            g_OverlayAdapter.SetHwnd(hwnd);
        }

        bool initResult = g_OverlayAdapter.InitOpenGL();
        HookLog("OpenGL: InitOpenGL returned %d", initResult ? 1 : 0);

        if (initResult) {
            g_OverlayContext = currentCtx;
            if (hwnd) {
                g_OverlayAdapter.SetHwnd(hwnd);
            }
        } else {
            HookLog("OpenGL: InitOpenGL failed - GL context = %p", currentCtx);
            return;
        }
    }

    if (!g_OverlayAdapter.IsInitialized()) {
        return;
    }

    g_OverlayAdapter.SetMetrics(&g_PerfMetrics);
    g_OverlayAdapter.SetIPCClient(g_IPC);
    g_OverlayAdapter.SetDroppedFrames(g_OpenGLCapture.droppedFrames.load(std::memory_order_relaxed));
    g_OverlayAdapter.SetGraphicsAPI(g_OpenGLApiLabel.c_str(), "active OpenGL context version/profile");

    HWND targetHwnd = WindowFromDC(hdc);
    if (!targetHwnd)
        targetHwnd = g_CachedHwnd;

    RECT rect;
    if (targetHwnd && GetClientRect(targetHwnd, &rect)) {
        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;
        if (width > 0 && height > 0) {
            g_OverlayAdapter.RenderOverlay(width, height);
        }
    }
}

// Swap hook logic
static void SwapBegin(HDC hdc) {
    if (g_SwapRecurse == 0) {
        g_LastOverlayUs = 0;
        TrackOpenGLContext(hdc);

        if (!g_FunctionsLoaded) {
            HookLog("OpenGL: First SwapBegin - Loading functions...");
            LoadGLFunctions();
            HookLog("OpenGL: Functions loaded.");
        }

        if (g_FunctionsLoaded && !g_VersionChecked) {
            typedef const GLubyte*(WINAPI * glGetString_t)(GLenum);
            glGetString_t pglGetString = (glGetString_t)GetProcAddress(GetModuleHandleA("opengl32.dll"), "glGetString");
            if (pglGetString) {
                const GLubyte* verStr = pglGetString(0x1F02 /*GL_VERSION*/);
                if (verStr) {
                    HookLog("OpenGL: Version String: %s", (const char*)verStr);
                    const auto parsedWithoutProfile =
                        ce::graphics_api_identity::ResolveOpenGLIdentity((const char*)verStr, 0);
                    unsigned profileMask = 0;
                    if (parsedWithoutProfile.valid &&
                        (parsedWithoutProfile.major > 3 ||
                         (parsedWithoutProfile.major == 3 && parsedWithoutProfile.minor >= 2)) &&
                        pglGetIntegerv) {
                        GLint queriedProfileMask = 0;
                        pglGetIntegerv(GL_CONTEXT_PROFILE_MASK, &queriedProfileMask);
                        profileMask = static_cast<unsigned>(queriedProfileMask);
                    }
                    const auto identity =
                        ce::graphics_api_identity::ResolveOpenGLIdentity((const char*)verStr, profileMask);
                    g_OpenGLApiLabel = ce::graphics_api_identity::FormatOpenGLLabel(identity);
                    g_LegacyContext = !identity.valid || identity.major < 3;
                    if (g_LegacyContext) {
                        HookLog(
                            "OpenGL: Legacy Context detected (%s, label=%s). Switching to GL2 "
                            "backend.",
                            (const char*)verStr, g_OpenGLApiLabel.c_str());
                    } else {
                        HookLog("OpenGL: Modern Context detected (%s, label=%s). Using GL3 backend.",
                                (const char*)verStr, g_OpenGLApiLabel.c_str());
                    }
                    g_VersionChecked = true;
                } else {
                    // If glGetString is NULL, it's likely we don't have a current context
                    // yet. We remain in !g_VersionChecked so we can try again next
                    // SwapBuffers. HookLog("OpenGL: glGetString returned NULL - waiting
                    // for current context...");
                }
            } else {
                HookLog("OpenGL: Failed to get glGetString address!");
                g_LegacyContext = true;  // Assume legacy if we can't check
                g_OpenGLApiLabel = "OpenGL";
                g_VersionChecked = true;
            }
        }
    }
    // We increments recurse BEFORE potential early returns to keep it balanced.
    if (g_SwapRecurse == 0) {
        static int swapFrameCount = 0;
        bool diagSwap = (swapFrameCount++ < 10);
        if (g_FunctionsLoaded) {
            SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
            bool shouldDraw = shm && shm->overlayConfig.showOverlay;
            if (diagSwap)
                HookLog(
                    "OpenGL: SwapBegin(HDC=0x%p) - shm=%p, shouldDraw=%d, "
                    "isRecording=%d",
                    hdc, shm, (int)shouldDraw, (int)(g_IPC && g_IPC->IsRecording()));

            bool captureIncludeOverlay = shm ? shm->overlayConfig.captureIncludeOverlay : true;
            bool screenshotIncludeOverlay = shm ? shm->overlayConfig.screenshotIncludeOverlay : true;
            bool shouldDrawOverlay = shouldDraw;
            bool isRecording = g_IPC && g_IPC->IsRecording();
            const uint64_t screenshotRequestId = GetPendingScreenshotRequestId(shm);
            const bool screenshotRequested = screenshotRequestId != 0;
            const bool screenshotAfterOverlay = screenshotRequested && shouldDrawOverlay && screenshotIncludeOverlay;
            const bool screenshotBeforeOverlay = screenshotRequested && !screenshotAfterOverlay;

            // Lambda for capture operation
            auto doCapture = [hdc, isRecording]() {
                if (isRecording) {
                    if (!g_OpenGLCapture.initialized && !g_LegacyContext) {
                        g_OpenGLCapture.Init(hdc);
                        if (g_OpenGLCapture.initialized) {
                            g_CaptureContext = wglGetCurrentContext();
                        }
                    }
                    if (g_OpenGLCapture.initialized) {
                        g_OpenGLCapture.CaptureFrame(hdc);
                    }
                } else if (g_OpenGLCapture.initialized) {
                    g_OpenGLCapture.Cleanup();
                }
            };

            // Lambda for overlay drawing
            auto doOverlay = [hdc, shouldDrawOverlay]() {
                if (shouldDrawOverlay) {
                    DrawOpenGLOverlay(hdc);
                }
            };

            auto doScreenshot = [hdc, shm, screenshotRequestId]() {
                if (!shm)
                    return;
                bool queued = false;
                if (pglReadPixels) {
                    // Get viewport dimensions from the DC window
                    RECT rc;
                    if (GetClientRect(WindowFromDC(hdc), &rc)) {
                        int w = rc.right;
                        int h = rc.bottom;
                        if (w > 0 && h > 0 && w <= 16384 && h <= 16384) {
                            const size_t rowPitch = static_cast<size_t>(w) * 4;
                            std::vector<uint8_t> pixels(rowPitch * static_cast<size_t>(h));
                            pglReadPixels(0, 0, w, h, 0x80E1 /*GL_BGRA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, pixels.data());
                            if (!pglGetError || pglGetError() == 0 /* GL_NO_ERROR */) {
                                std::vector<uint8_t> row(rowPitch);
                                for (int y = 0; y < h / 2; ++y) {
                                    uint8_t* top = pixels.data() + static_cast<size_t>(y) * rowPitch;
                                    uint8_t* bottom = pixels.data() + static_cast<size_t>(h - 1 - y) * rowPitch;
                                    memcpy(row.data(), top, rowPitch);
                                    memcpy(top, bottom, rowPitch);
                                    memcpy(bottom, row.data(), rowPitch);
                                }
                                queued = QueueScreenshotPixels(
                                    shm, screenshotRequestId, pixels.data(), static_cast<uint32_t>(w),
                                    static_cast<uint32_t>(h), static_cast<uint32_t>(rowPitch),
                                    ScreenshotPixelFormat::BGRA8, ScreenshotColorEncoding::SRGB);
                            }
                        }
                    }
                }
                if (!queued) {
                    CompleteScreenshotRequest(shm, screenshotRequestId, ScreenshotRequestStatus::Failed,
                                              ERROR_READ_FAULT);
                }
            };

            if (!g_LegacyContext && !captureIncludeOverlay)
                doCapture();
            if (screenshotBeforeOverlay)
                doScreenshot();
            const int64_t overlayStartUs = PerfLogger::Get().IsEnabled() ? PerfLogger::GetQpcUs() : 0;
            doOverlay();
            if (overlayStartUs != 0) {
                g_LastOverlayUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - overlayStartUs);
            }
            if (!g_LegacyContext && captureIncludeOverlay)
                doCapture();
            if (screenshotAfterOverlay)
                doScreenshot();
        }
    }
    g_SwapRecurse++;
}

static void SwapEnd(HDC hdc) {
    g_SwapRecurse--;

    if (g_SwapRecurse == 0 && g_FunctionsLoaded) {
        // Update performance metrics
        static int64_t qpcFreq = 0;
        if (qpcFreq == 0) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            qpcFreq = f.QuadPart;
        }
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;
        g_PerfMetrics.Update(us);

        // Order capture/overlay logic was moved to SwapBegin

        // Apply FPS limiter
        g_SharedFpsLimiter.SetIPCClient(g_IPC);
        g_SharedFpsLimiter.Apply();

        // CPU Prerender Limit
        if (g_IPC && g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit >= 0) {
            ApplyPrerenderLimitGL(g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit);
        }

        // Performance logging for PerfLogger
        if (PerfLogger::Get().IsEnabled()) {
            static uint64_t s_perfFrameNum = 0;
            static int64_t s_lastFrameUs = 0;
            FrameMetrics perfMetrics;
            perfMetrics.frameNum = ++s_perfFrameNum;
            perfMetrics.qpcUs = us;
            if (s_lastFrameUs > 0) {
                perfMetrics.totalUs = static_cast<int32_t>(us - s_lastFrameUs);
            }
            s_lastFrameUs = us;
            perfMetrics.overlayUs = g_LastOverlayUs;
            strncpy(perfMetrics.api, "OpenGL", sizeof(perfMetrics.api) - 1);
            perfMetrics.api[sizeof(perfMetrics.api) - 1] = '\0';
            PerfLogger::Get().LogFrame(perfMetrics);
        }
    }
}

// Hook: SwapBuffers (GDI32)
static BOOL WINAPI DetourSwapBuffers(HDC hdc) {
    SwapBegin(hdc);
    if (g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        LoadOpenGLExtensions();
    }
    BOOL result = oSwapBuffers(hdc);
    SwapEnd(hdc);
    return result;
}

// Hook: wglSwapBuffers
static BOOL WINAPI DetourWglSwapBuffers(HDC hdc) {
    SwapBegin(hdc);
    BOOL result = oWglSwapBuffers(hdc);
    SwapEnd(hdc);
    return result;
}

// Hook: wglSwapLayerBuffers
static BOOL WINAPI DetourWglSwapLayerBuffers(HDC hdc, UINT fuPlanes) {
    SwapBegin(hdc);
    BOOL result = oWglSwapLayerBuffers(hdc, fuPlanes);
    SwapEnd(hdc);
    return result;
}

// Hook: wglDeleteContext - cleanup when context is destroyed
static BOOL WINAPI DetourWglDeleteContext(HGLRC hglrc) {
    HookLog("OpenGL: wglDeleteContext called (ctx=0x%p)", hglrc);
    ResetTrackedOpenGLState(hglrc);

    {
        std::lock_guard<std::mutex> lock(g_PrerenderMutex);
        auto it = g_PrerenderStates.find(hglrc);
        if (it != g_PrerenderStates.end()) {
            if (wglGetCurrentContext() == hglrc && pglDeleteSync) {
                for (GLsync sync : it->second.syncs) {
                    if (sync)
                        pglDeleteSync(sync);
                }
            }
            g_PrerenderStates.erase(it);
        }
    }

    return oWglDeleteContext(hglrc);
}

// Hook: wglSwapIntervalEXT (VSync)
static BOOL WINAPI DetourWglSwapIntervalEXT(int interval) {
    if (g_IPC) {
        const auto& gfx = GetActiveGraphicsConfig();
        if (gfx.vsyncMode != "default" && !gfx.vsyncMode.empty()) {
            if (gfx.vsyncMode == "off" || gfx.vsyncMode == "mailbox")
                interval = 0;
            else if (gfx.vsyncMode == "fifo")
                interval = 1;
            else if (gfx.vsyncMode == "adaptive")
                interval = -1;
        }
    }

    // We need to call the real function. Since it's an extension, we likely need
    // to fetch it. But we might not have 'oWglSwapIntervalEXT' if we intercepted
    // GetProcAddress. We should try to fetch it if null.
    if (!oWglSwapIntervalEXT) {
        // If we are here, DetourWglGetProcAddress should have found it, OR we need
        // to fetch it now. Careful about recursion if we call wglGetProcAddress. We
        // use oWglGetProcAddress if available.
        if (oWglGetProcAddress) {
            // ... existing hooks
            oWglSwapIntervalEXT = (wglSwapIntervalEXT_t)oWglGetProcAddress("wglSwapIntervalEXT");
        }
    }

    if (oWglSwapIntervalEXT) {
        return oWglSwapIntervalEXT(interval);
    }
    return FALSE;
}

static BOOL WINAPI DetourWglMakeCurrent(HDC hdc, HGLRC hrc) {
    const BOOL result = oWglMakeCurrent(hdc, hrc);
    if (result) {
        ce::opengl_sampler_override::NotifyContextChanged();
    }
    return result;
}

// Hook: wglGetProcAddress
static PROC WINAPI DetourWglGetProcAddress(LPCSTR lpszProc) {
    if (!lpszProc)
        return NULL;

    // Log important requests
    if (strstr(lpszProc, "Context") || strstr(lpszProc, "Swap")) {
        HookLog("OpenGL: wglGetProcAddress('%s')", lpszProc);
    }

    // Check for VSync hook
    if (strcmp(lpszProc, "wglSwapIntervalEXT") == 0) {
        // Fetch original to call later
        PROC proc = oWglGetProcAddress(lpszProc);
        if (proc)
            oWglSwapIntervalEXT = (wglSwapIntervalEXT_t)proc;
        return (PROC)DetourWglSwapIntervalEXT;
    }

    if (strcmp(lpszProc, "glRenderbufferStorageMultisample") == 0) {
        PROC proc = oWglGetProcAddress(lpszProc);
        if (proc)
            pglRenderbufferStorageMultisample = (glRenderbufferStorageMultisample_t)proc;
        return (PROC)DetourGlRenderbufferStorageMultisample;
    }

    if (strcmp(lpszProc, "glTexImage2DMultisample") == 0) {
        PROC proc = oWglGetProcAddress(lpszProc);
        if (proc)
            pglTexImage2DMultisample = (glTexImage2DMultisample_t)proc;
        return (PROC)DetourGlTexImage2DMultisample;
    }

    PROC original = oWglGetProcAddress(lpszProc);
    return ce::opengl_sampler_override::InterceptProcAddress(lpszProc, original, oWglGetProcAddress);
}

void OpenGLHook::Init() {
    HookLog("OpenGLHook::Init()");

    // Check if opengl32.dll is loaded
    HMODULE glModule = GetModuleHandleA("opengl32.dll");
    if (!glModule) {
        return;
    }

    HMODULE gdi32Module = GetModuleHandleA("gdi32.dll");
    if (!gdi32Module) {
        return;
    }

    // Hook SwapBuffers (GDI32)
    // Register for dynamic loading via GetProcAddress
    IATHook::RegisterDynamicHook("SwapBuffers", (LPVOID)&DetourSwapBuffers, (LPVOID*)&oSwapBuffers);
    // Patch explicit imports
    IATHook::PatchIATAllModules("gdi32.dll", "SwapBuffers", (LPVOID)&DetourSwapBuffers, (LPVOID*)&oSwapBuffers);

    // Hook wglSwapBuffers
    IATHook::RegisterDynamicHook("wglSwapBuffers", (LPVOID)&DetourWglSwapBuffers, (LPVOID*)&oWglSwapBuffers);
    IATHook::PatchIATAllModules("opengl32.dll", "wglSwapBuffers", (LPVOID)&DetourWglSwapBuffers,
                                (LPVOID*)&oWglSwapBuffers);

    // Hook wglSwapLayerBuffers
    IATHook::RegisterDynamicHook("wglSwapLayerBuffers", (LPVOID)&DetourWglSwapLayerBuffers,
                                 (LPVOID*)&oWglSwapLayerBuffers);
    IATHook::PatchIATAllModules("opengl32.dll", "wglSwapLayerBuffers", (LPVOID)&DetourWglSwapLayerBuffers,
                                (LPVOID*)&oWglSwapLayerBuffers);

    // Hook wglDeleteContext
    IATHook::RegisterDynamicHook("wglDeleteContext", (LPVOID)&DetourWglDeleteContext, (LPVOID*)&oWglDeleteContext);
    IATHook::PatchIATAllModules("opengl32.dll", "wglDeleteContext", (LPVOID)&DetourWglDeleteContext,
                                (LPVOID*)&oWglDeleteContext);

    // Hook wglGetProcAddress
    // Critical for intercepting extensions
    IATHook::RegisterDynamicHook("wglGetProcAddress", (LPVOID)&DetourWglGetProcAddress, (LPVOID*)&oWglGetProcAddress);
    IATHook::PatchIATAllModules("opengl32.dll", "wglGetProcAddress", (LPVOID)&DetourWglGetProcAddress,
                                (LPVOID*)&oWglGetProcAddress);

    ce::opengl_sampler_override::Initialize();

    // Hook wglMakeCurrent
    IATHook::RegisterDynamicHook("wglMakeCurrent", (LPVOID)&DetourWglMakeCurrent, (LPVOID*)&oWglMakeCurrent);
    IATHook::PatchIATAllModules("opengl32.dll", "wglMakeCurrent", (LPVOID)&DetourWglMakeCurrent,
                                (LPVOID*)&oWglMakeCurrent);

    g_HooksInitialized = true;
    HookLog("OpenGLHook: All hooks registered (IAT/Dynamic)");
}

void OpenGLHook::Shutdown() {
    HookLog("OpenGLHook::Shutdown()");
    ce::opengl_sampler_override::Shutdown();
    ResetTrackedOpenGLState(NULL);

    {
        std::lock_guard<std::mutex> lock(g_PrerenderMutex);
        const HGLRC current = wglGetCurrentContext();
        if (pglDeleteSync && current) {
            auto it = g_PrerenderStates.find(current);
            if (it != g_PrerenderStates.end()) {
                for (GLsync sync : it->second.syncs) {
                    if (sync)
                        pglDeleteSync(sync);
                }
            }
        }
        g_PrerenderStates.clear();
    }
    // IAT hooks remain until process exit
}

void OpenGLHook::OnHostDisconnect() {
    HookLog("OpenGLHook::OnHostDisconnect()");
    ResetTrackedOpenGLState(NULL);
}
