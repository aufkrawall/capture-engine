#include "opengl_hook_internal.h"

static GLsizei ParseGLMSAA(const char* msaa) {
    if (strcmp(msaa, "2x") == 0)
        return 2;
    if (strcmp(msaa, "4x") == 0)
        return 4;
    if (strcmp(msaa, "8x") == 0)
        return 8;
    return 1;
}

static void WINAPI DetourGlRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internalformat,
                                                          GLsizei width, GLsizei height) {
    if (g_IPC && g_IPC->GetSharedMem()) {
        const char* msaa = g_IPC->GetSharedMem()->graphicsConfig.msaaSamples;
        if (msaa[0] != 'd') {
            if (strcmp(msaa, "off") == 0)
                samples = 0;
            else {
                GLsizei s = ParseGLMSAA(msaa);
                if (s > 1) {
                    samples = s;
                    // HookLog("OpenGL: Forcing MSAA %dx for Renderbuffer", s);
                }
            }
        }
    }
    if (opengl_hook_pglRenderbufferStorageMultisample)
        opengl_hook_pglRenderbufferStorageMultisample(target, samples, internalformat, width, height);
}

static void WINAPI DetourGlTexImage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                                                 GLsizei height, GLboolean fixedsamplelocations) {
    if (g_IPC && g_IPC->GetSharedMem()) {
        const char* msaa = g_IPC->GetSharedMem()->graphicsConfig.msaaSamples;
        if (msaa[0] != 'd') {
            if (strcmp(msaa, "off") == 0)
                samples = 1;
            else {
                GLsizei s = ParseGLMSAA(msaa);
                if (s > 1) {
                    samples = s;
                    // HookLog("OpenGL: Forcing MSAA %dx for Texture", s);
                }
            }
        }
    }
    if (opengl_hook_pglTexImage2DMultisample)
        opengl_hook_pglTexImage2DMultisample(target, samples, internalformat, width, height, fixedsamplelocations);
}

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
static OpenGLCapture g_OpenGLCapture;

static void ResetTrackedOpenGLState(HGLRC contextToReset) {
    const bool resetAll = (contextToReset == NULL);
    const bool resetCapture = resetAll || contextToReset == opengl_hook_g_CaptureContext;
    const bool resetOverlay = resetAll || contextToReset == opengl_hook_g_OverlayContext;
    const bool resetVersionState = resetAll || contextToReset == opengl_hook_g_CurrentTrackedContext;

    const HGLRC previousContext = wglGetCurrentContext();
    const HDC previousDC = wglGetCurrentDC();
    bool switchedToCaptureContext = false;
    bool canCleanCaptureContext =
        !resetCapture || !g_OpenGLCapture.initialized || !opengl_hook_g_CaptureContext || previousContext == opengl_hook_g_CaptureContext;
    if (!canCleanCaptureContext && opengl_hook_oWglMakeCurrent && opengl_hook_g_CaptureHDC) {
        switchedToCaptureContext = opengl_hook_oWglMakeCurrent(opengl_hook_g_CaptureHDC, opengl_hook_g_CaptureContext) == TRUE;
        canCleanCaptureContext = switchedToCaptureContext;
        if (!canCleanCaptureContext) {
            HookLog("OpenGL: Deferring cleanup for owner context %p because it could not be made current",
                    opengl_hook_g_CaptureContext);
        }
    }

    bool captureCleanupHandledOverlay = false;
    if (resetCapture && g_OpenGLCapture.initialized && canCleanCaptureContext) {
        g_OpenGLCapture.TryCleanup(true);
        captureCleanupHandledOverlay = true;
    }

    if (resetOverlay && !captureCleanupHandledOverlay && g_OverlayAdapter.IsInitialized() &&
        (!opengl_hook_g_OverlayContext || wglGetCurrentContext() == opengl_hook_g_OverlayContext)) {
        g_OverlayAdapter.Shutdown();
    }

    if (resetCapture && (!g_OpenGLCapture.initialized || canCleanCaptureContext)) {
        opengl_hook_g_CaptureContext = NULL;
        opengl_hook_g_CaptureHDC = NULL;
    }
    if (resetOverlay) {
        opengl_hook_g_OverlayContext = NULL;
    }
    if (resetVersionState) {
        opengl_hook_g_CurrentTrackedContext = NULL;
        opengl_hook_g_VersionChecked = false;
        opengl_hook_g_LegacyContext = false;
        opengl_hook_g_OpenGLApiLabel = "OpenGL";
    }

    if (switchedToCaptureContext) {
        const BOOL restored =
            previousContext ? opengl_hook_oWglMakeCurrent(previousDC, previousContext) : opengl_hook_oWglMakeCurrent(nullptr, nullptr);
        if (!restored) {
            HookLog("OpenGL: Failed to restore application context %p after capture cleanup", previousContext);
        }
    }
}

static bool TrackOpenGLContext(HDC hdc) {
    HGLRC currentCtx = wglGetCurrentContext();
    if (!currentCtx)
        return false;

    if (currentCtx != opengl_hook_g_CurrentTrackedContext) {
        if (opengl_hook_g_CurrentTrackedContext) {
            HookLog("OpenGL: Switching tracked context from %p to %p", opengl_hook_g_CurrentTrackedContext, currentCtx);
            ResetTrackedOpenGLState(opengl_hook_g_CurrentTrackedContext);
        }
        opengl_hook_g_CurrentTrackedContext = currentCtx;
    }

    HWND hwnd = WindowFromDC(hdc);
    if (hwnd && hwnd != opengl_hook_g_CachedHwnd) {
        opengl_hook_g_CachedHwnd = hwnd;
        InputManager::Get().HookWindow(hwnd);
        g_OverlayAdapter.SetHwnd(hwnd);
    }

    return true;
}

// Load OpenGL functions
static bool LoadGLFunctions() {
    if (opengl_hook_g_FunctionsLoaded)
        return true;

    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl)
        return false;

    typedef PROC(WINAPI * wglGetProcAddress_t)(LPCSTR);
    wglGetProcAddress_t wglGetProcAddress_ptr = (wglGetProcAddress_t)GetProcAddress(gl, "wglGetProcAddress");
    if (!wglGetProcAddress_ptr)
        return false;

    // Load base GL functions from opengl32.dll
    opengl_hook_pglGetError = (glGetError_t)GetProcAddress(gl, "glGetError");
    opengl_hook_pglGetIntegerv = (glGetIntegerv_t)GetProcAddress(gl, "glGetIntegerv");
    opengl_hook_pglFlush = (glFlush_t)GetProcAddress(gl, "glFlush");
    opengl_hook_pglFinish = (glFinish_t)GetProcAddress(gl, "glFinish");

// Load extension functions via wglGetProcAddress
#define LOAD_GL(name)                                 \
    opengl_hook_p##name = (name##_t)wglGetProcAddress_ptr(#name); \
    if (!opengl_hook_p##name)                                     \
    opengl_hook_p##name = (name##_t)GetProcAddress(gl, #name)

    LOAD_GL(glGenTextures);
    LOAD_GL(glDeleteTextures);
    LOAD_GL(glBindTexture);
    LOAD_GL(glTexImage2D);
    LOAD_GL(glGenFramebuffers);
    LOAD_GL(glDeleteFramebuffers);
    LOAD_GL(glBindFramebuffer);
    LOAD_GL(glFramebufferTexture2D);
    LOAD_GL(glCheckFramebufferStatus);
    LOAD_GL(glBlitFramebuffer);
    LOAD_GL(glGenBuffers);
    LOAD_GL(glDeleteBuffers);
    LOAD_GL(glBindBuffer);
    LOAD_GL(glBufferData);
    LOAD_GL(glReadPixels);
    LOAD_GL(glMapBuffer);
    LOAD_GL(glUnmapBuffer);
    LOAD_GL(glFenceSync);
    LOAD_GL(glDeleteSync);
    LOAD_GL(glClientWaitSync);
    LOAD_GL(glCopyTexSubImage2D);

    // Check for WGL_NV_DX_interop
    opengl_hook_wglDXOpenDeviceNV = (wglDXOpenDeviceNV_t)wglGetProcAddress_ptr("wglDXOpenDeviceNV");
    opengl_hook_wglDXCloseDeviceNV = (wglDXCloseDeviceNV_t)wglGetProcAddress_ptr("wglDXCloseDeviceNV");
    opengl_hook_wglDXRegisterObjectNV = (wglDXRegisterObjectNV_t)wglGetProcAddress_ptr("wglDXRegisterObjectNV");
    opengl_hook_wglDXUnregisterObjectNV = (wglDXUnregisterObjectNV_t)wglGetProcAddress_ptr("wglDXUnregisterObjectNV");
    opengl_hook_wglDXLockObjectsNV = (wglDXLockObjectsNV_t)wglGetProcAddress_ptr("wglDXLockObjectsNV");
    opengl_hook_wglDXUnlockObjectsNV = (wglDXUnlockObjectsNV_t)wglGetProcAddress_ptr("wglDXUnlockObjectsNV");

    opengl_hook_g_NVInteropAvailable = (opengl_hook_wglDXOpenDeviceNV && opengl_hook_wglDXCloseDeviceNV && opengl_hook_wglDXRegisterObjectNV &&
                            opengl_hook_wglDXUnregisterObjectNV && opengl_hook_wglDXLockObjectsNV && opengl_hook_wglDXUnlockObjectsNV);

    opengl_hook_g_FunctionsLoaded = true;
    HookLog("OpenGL: Functions loaded (NV Interop: %s)", opengl_hook_g_NVInteropAvailable ? "Available" : "Not Available");
    return true;
}

// Helper to load extensions
static void LoadOpenGLExtensions() {
    if (opengl_hook_pglMinSampleShading)
        return;  // Already loaded

    // We need a current context to load extensions
    HGLRC hRC = wglGetCurrentContext();
    if (!hRC)
        return;

    // Use oWglGetProcAddress if available, otherwise assume standard loading
    typedef PROC(WINAPI * wglGetProcAddress_t)(LPCSTR);
    wglGetProcAddress_t wglGetProcAddress_ptr = nullptr;
    if (opengl_hook_oWglGetProcAddress)
        wglGetProcAddress_ptr = (wglGetProcAddress_t)opengl_hook_oWglGetProcAddress;
    else
        wglGetProcAddress_ptr =
            (wglGetProcAddress_t)GetProcAddress(GetModuleHandleA("opengl32.dll"), "wglGetProcAddress");

    if (!wglGetProcAddress_ptr)
        return;

    opengl_hook_pglMinSampleShading = (glMinSampleShading_t)wglGetProcAddress_ptr("glMinSampleShading");
    if (!opengl_hook_pglMinSampleShading)
        opengl_hook_pglMinSampleShading = (glMinSampleShading_t)wglGetProcAddress_ptr("glMinSampleShadingARB");

    opengl_hook_pglEnable = (glEnable_t)GetProcAddress(GetModuleHandleA("opengl32.dll"), "glEnable");
}

// Detect GPU LUID for system metrics
static void DetectGPU(HDC hdc) {
    if (opengl_hook_g_LuidReported)
        return;

    HMODULE hD3D11 = GetModuleHandleA("d3d11.dll");
    if (!hD3D11)
        hD3D11 = ce::security::LoadSystemLibrary(L"d3d11.dll");
    if (!hD3D11)
        return;

    typedef HRESULT(WINAPI * PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                      const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                                      D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
    if (!pD3D11CreateDevice)
        return;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL featureLevel;
    D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};

    HRESULT hr = pD3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, featureLevels, 2, D3D11_SDK_VERSION,
                                    &device, &featureLevel, &context);
    if (SUCCEEDED(hr)) {
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter* adapter = nullptr;

            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC desc;
                adapter->GetDesc(&desc);

                uint32_t lLow = desc.AdapterLuid.LowPart;
                uint32_t lHigh = desc.AdapterLuid.HighPart;

                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                SystemMetricsCollector::Get().Initialize(lLow, lHigh);
                ReportLUID(lLow, lHigh);

                opengl_hook_g_LuidReported = true;
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
            opengl_hook_g_CachedHwnd = hwnd;
            InputManager::Get().HookWindow(hwnd);
            g_OverlayAdapter.SetHwnd(hwnd);
        }

        bool initResult = g_OverlayAdapter.InitOpenGL();
        HookLog("OpenGL: InitOpenGL returned %d", initResult ? 1 : 0);

        if (initResult) {
            opengl_hook_g_OverlayContext = currentCtx;
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

    g_OverlayAdapter.SetMetrics(&opengl_hook_g_PerfMetrics);
    g_OverlayAdapter.SetIPCClient(g_IPC);
    g_OverlayAdapter.SetDroppedFrames(g_OpenGLCapture.droppedFrames.load(std::memory_order_relaxed));
    g_OverlayAdapter.SetGraphicsAPI(opengl_hook_g_OpenGLApiLabel.c_str(), "active OpenGL context version/profile");

    HWND targetHwnd = WindowFromDC(hdc);
    if (!targetHwnd)
        targetHwnd = opengl_hook_g_CachedHwnd;

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
    if (opengl_hook_g_SwapRecurse == 0) {
        opengl_hook_g_LastOverlayUs = 0;
        TrackOpenGLContext(hdc);

        if (!opengl_hook_g_FunctionsLoaded) {
            HookLog("OpenGL: First SwapBegin - Loading functions...");
            LoadGLFunctions();
            HookLog("OpenGL: Functions loaded.");
        }

        if (opengl_hook_g_FunctionsLoaded && !opengl_hook_g_VersionChecked) {
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
                        opengl_hook_pglGetIntegerv) {
                        GLint queriedProfileMask = 0;
                        opengl_hook_pglGetIntegerv(GL_CONTEXT_PROFILE_MASK, &queriedProfileMask);
                        profileMask = static_cast<unsigned>(queriedProfileMask);
                    }
                    const auto identity =
                        ce::graphics_api_identity::ResolveOpenGLIdentity((const char*)verStr, profileMask);
                    opengl_hook_g_OpenGLApiLabel = ce::graphics_api_identity::FormatOpenGLLabel(identity);
                    opengl_hook_g_LegacyContext = !identity.valid || identity.major < 3;
                    if (opengl_hook_g_LegacyContext) {
                        HookLog(
                            "OpenGL: Legacy Context detected (%s, label=%s). Switching to GL2 "
                            "backend.",
                            (const char*)verStr, opengl_hook_g_OpenGLApiLabel.c_str());
                    } else {
                        HookLog("OpenGL: Modern Context detected (%s, label=%s). Using GL3 backend.",
                                (const char*)verStr, opengl_hook_g_OpenGLApiLabel.c_str());
                    }
                    opengl_hook_g_VersionChecked = true;
                } else {
                    // If glGetString is NULL, it's likely we don't have a current context
                    // yet. We remain in !g_VersionChecked so we can try again next
                    // SwapBuffers. HookLog("OpenGL: glGetString returned NULL - waiting
                    // for current context...");
                }
            } else {
                HookLog("OpenGL: Failed to get glGetString address!");
                opengl_hook_g_LegacyContext = true;  // Assume legacy if we can't check
                opengl_hook_g_OpenGLApiLabel = "OpenGL";
                opengl_hook_g_VersionChecked = true;
            }
        }
    }
    // We increments recurse BEFORE potential early returns to keep it balanced.
    if (opengl_hook_g_SwapRecurse == 0) {
        static int swapFrameCount = 0;
        bool diagSwap = (swapFrameCount++ < 10);
        if (opengl_hook_g_FunctionsLoaded) {
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
                    if (!g_OpenGLCapture.initialized && !opengl_hook_g_LegacyContext) {
                        g_OpenGLCapture.Init(hdc);
                        if (g_OpenGLCapture.initialized) {
                            opengl_hook_g_CaptureContext = wglGetCurrentContext();
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
                if (opengl_hook_pglReadPixels) {
                    // Get viewport dimensions from the DC window
                    RECT rc;
                    if (GetClientRect(WindowFromDC(hdc), &rc)) {
                        int w = rc.right;
                        int h = rc.bottom;
                        if (w > 0 && h > 0 && w <= 16384 && h <= 16384) {
                            const size_t rowPitch = static_cast<size_t>(w) * 4;
                            std::vector<uint8_t> pixels(rowPitch * static_cast<size_t>(h));
                            opengl_hook_pglReadPixels(0, 0, w, h, 0x80E1 /*GL_BGRA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, pixels.data());
                            if (!opengl_hook_pglGetError || opengl_hook_pglGetError() == 0 /* GL_NO_ERROR */) {
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

            if (!opengl_hook_g_LegacyContext && !captureIncludeOverlay)
                doCapture();
            if (screenshotBeforeOverlay)
                doScreenshot();
            const int64_t overlayStartUs = PerfLogger::Get().IsEnabled() ? PerfLogger::GetQpcUs() : 0;
            doOverlay();
            if (overlayStartUs != 0) {
                opengl_hook_g_LastOverlayUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - overlayStartUs);
            }
            if (!opengl_hook_g_LegacyContext && captureIncludeOverlay)
                doCapture();
            if (screenshotAfterOverlay)
                doScreenshot();
        }
    }
    opengl_hook_g_SwapRecurse++;
}

static void SwapEnd(HDC hdc) {
    opengl_hook_g_SwapRecurse--;

    if (opengl_hook_g_SwapRecurse == 0 && opengl_hook_g_FunctionsLoaded) {
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
        opengl_hook_g_PerfMetrics.Update(us);

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
            perfMetrics.overlayUs = opengl_hook_g_LastOverlayUs;
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
    BOOL result = opengl_hook_oSwapBuffers(hdc);
    SwapEnd(hdc);
    return result;
}

// Hook: wglSwapBuffers
static BOOL WINAPI DetourWglSwapBuffers(HDC hdc) {
    SwapBegin(hdc);
    BOOL result = opengl_hook_oWglSwapBuffers(hdc);
    SwapEnd(hdc);
    return result;
}

// Hook: wglSwapLayerBuffers
static BOOL WINAPI DetourWglSwapLayerBuffers(HDC hdc, UINT fuPlanes) {
    SwapBegin(hdc);
    BOOL result = opengl_hook_oWglSwapLayerBuffers(hdc, fuPlanes);
    SwapEnd(hdc);
    return result;
}

// Hook: wglDeleteContext - cleanup when context is destroyed
static BOOL WINAPI DetourWglDeleteContext(HGLRC hglrc) {
    HookLog("OpenGL: wglDeleteContext called (ctx=0x%p)", hglrc);
    ResetTrackedOpenGLState(hglrc);

    {
        std::lock_guard<std::mutex> lock(opengl_hook_g_PrerenderMutex);
        auto it = opengl_hook_g_PrerenderStates.find(hglrc);
        if (it != opengl_hook_g_PrerenderStates.end()) {
            if (wglGetCurrentContext() == hglrc && opengl_hook_pglDeleteSync) {
                for (GLsync sync : it->second.syncs) {
                    if (sync)
                        opengl_hook_pglDeleteSync(sync);
                }
            }
            opengl_hook_g_PrerenderStates.erase(it);
        }
    }

    return opengl_hook_oWglDeleteContext(hglrc);
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
    if (!opengl_hook_oWglSwapIntervalEXT) {
        // If we are here, DetourWglGetProcAddress should have found it, OR we need
        // to fetch it now. Careful about recursion if we call wglGetProcAddress. We
        // use oWglGetProcAddress if available.
        if (opengl_hook_oWglGetProcAddress) {
            // ... existing hooks
            opengl_hook_oWglSwapIntervalEXT = (wglSwapIntervalEXT_t)opengl_hook_oWglGetProcAddress("wglSwapIntervalEXT");
        }
    }

    if (opengl_hook_oWglSwapIntervalEXT) {
        return opengl_hook_oWglSwapIntervalEXT(interval);
    }
    return FALSE;
}

static BOOL WINAPI DetourWglMakeCurrent(HDC hdc, HGLRC hrc) {
    const BOOL result = opengl_hook_oWglMakeCurrent(hdc, hrc);
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
        PROC proc = opengl_hook_oWglGetProcAddress(lpszProc);
        if (proc)
            opengl_hook_oWglSwapIntervalEXT = (wglSwapIntervalEXT_t)proc;
        return (PROC)DetourWglSwapIntervalEXT;
    }

    if (strcmp(lpszProc, "glRenderbufferStorageMultisample") == 0) {
        PROC proc = opengl_hook_oWglGetProcAddress(lpszProc);
        if (proc)
            opengl_hook_pglRenderbufferStorageMultisample = (glRenderbufferStorageMultisample_t)proc;
        return (PROC)DetourGlRenderbufferStorageMultisample;
    }

    if (strcmp(lpszProc, "glTexImage2DMultisample") == 0) {
        PROC proc = opengl_hook_oWglGetProcAddress(lpszProc);
        if (proc)
            opengl_hook_pglTexImage2DMultisample = (glTexImage2DMultisample_t)proc;
        return (PROC)DetourGlTexImage2DMultisample;
    }

    PROC original = opengl_hook_oWglGetProcAddress(lpszProc);
    return ce::opengl_sampler_override::InterceptProcAddress(lpszProc, original, opengl_hook_oWglGetProcAddress);
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
    IATHook::RegisterDynamicHook("SwapBuffers", (LPVOID)&DetourSwapBuffers, (LPVOID*)&opengl_hook_oSwapBuffers);
    // Patch explicit imports
    IATHook::PatchIATAllModules("gdi32.dll", "SwapBuffers", (LPVOID)&DetourSwapBuffers, (LPVOID*)&opengl_hook_oSwapBuffers);

    // Hook wglSwapBuffers
    IATHook::RegisterDynamicHook("wglSwapBuffers", (LPVOID)&DetourWglSwapBuffers, (LPVOID*)&opengl_hook_oWglSwapBuffers);
    IATHook::PatchIATAllModules("opengl32.dll", "wglSwapBuffers", (LPVOID)&DetourWglSwapBuffers,
                                (LPVOID*)&opengl_hook_oWglSwapBuffers);

    // Hook wglSwapLayerBuffers
    IATHook::RegisterDynamicHook("wglSwapLayerBuffers", (LPVOID)&DetourWglSwapLayerBuffers,
                                 (LPVOID*)&opengl_hook_oWglSwapLayerBuffers);
    IATHook::PatchIATAllModules("opengl32.dll", "wglSwapLayerBuffers", (LPVOID)&DetourWglSwapLayerBuffers,
                                (LPVOID*)&opengl_hook_oWglSwapLayerBuffers);

    // Hook wglDeleteContext
    IATHook::RegisterDynamicHook("wglDeleteContext", (LPVOID)&DetourWglDeleteContext, (LPVOID*)&opengl_hook_oWglDeleteContext);
    IATHook::PatchIATAllModules("opengl32.dll", "wglDeleteContext", (LPVOID)&DetourWglDeleteContext,
                                (LPVOID*)&opengl_hook_oWglDeleteContext);

    // Hook wglGetProcAddress
    // Critical for intercepting extensions
    IATHook::RegisterDynamicHook("wglGetProcAddress", (LPVOID)&DetourWglGetProcAddress, (LPVOID*)&opengl_hook_oWglGetProcAddress);
    IATHook::PatchIATAllModules("opengl32.dll", "wglGetProcAddress", (LPVOID)&DetourWglGetProcAddress,
                                (LPVOID*)&opengl_hook_oWglGetProcAddress);

    ce::opengl_sampler_override::Initialize();

    // Hook wglMakeCurrent
    IATHook::RegisterDynamicHook("wglMakeCurrent", (LPVOID)&DetourWglMakeCurrent, (LPVOID*)&opengl_hook_oWglMakeCurrent);
    IATHook::PatchIATAllModules("opengl32.dll", "wglMakeCurrent", (LPVOID)&DetourWglMakeCurrent,
                                (LPVOID*)&opengl_hook_oWglMakeCurrent);

    opengl_hook_g_HooksInitialized = true;
    HookLog("OpenGLHook: All hooks registered (IAT/Dynamic)");
}

void OpenGLHook::Shutdown() {
    HookLog("OpenGLHook::Shutdown()");
    ce::opengl_sampler_override::Shutdown();
    ResetTrackedOpenGLState(NULL);

    {
        std::lock_guard<std::mutex> lock(opengl_hook_g_PrerenderMutex);
        const HGLRC current = wglGetCurrentContext();
        if (opengl_hook_pglDeleteSync && current) {
            auto it = opengl_hook_g_PrerenderStates.find(current);
            if (it != opengl_hook_g_PrerenderStates.end()) {
                for (GLsync sync : it->second.syncs) {
                    if (sync)
                        opengl_hook_pglDeleteSync(sync);
                }
            }
        }
        opengl_hook_g_PrerenderStates.clear();
    }
    // IAT hooks remain until process exit
}

void OpenGLHook::OnHostDisconnect() {
    HookLog("OpenGLHook::OnHostDisconnect()");
    ResetTrackedOpenGLState(NULL);
}
