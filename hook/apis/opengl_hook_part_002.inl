                return false;
            }
            sharedTextureHandles[i].store(hShared, std::memory_order_release);
        }

        if (!InitPBOFence()) {
            HookLog("OpenGL: D3D11.3 PBO fence unavailable; using legacy shared-resource Flush synchronization");
        }
        pboSyncSupported = pglFenceSync && pglClientWaitSync && pglDeleteSync;
        if (!pboSyncSupported) {
            HookLog("OpenGL: GL sync objects unavailable; PBO readback may use the legacy mapping path");
        }
        usePBO = true;
        HookLog("OpenGL: PBO fallback initialized");
        return true;
    }

    void Init(HDC hDC) {
        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        if (initialized)
            return;
        HookLog("OpenGLCapture: Init(HDC=0x%p)", hDC);

        // Safety: Ensure required functions are loaded
        if (!pglGenFramebuffers || !pglBindFramebuffer || !pglFramebufferTexture2D || !pglCheckFramebufferStatus ||
            !pglGenTextures || !pglBindTexture || !pglTexImage2D || !pglBlitFramebuffer || !pglGetIntegerv) {
            HookLog("OpenGLCapture: FBO extensions not available. FBO capture disabled.");
            return;
        }

        g_CaptureHDC = hDC;
        g_CaptureContext = wglGetCurrentContext();

        // Get window size
        HWND hwnd = WindowFromDC(hDC);
        RECT rect = {};
        width = 0;
        height = 0;
        if (hwnd && GetClientRect(hwnd, &rect)) {
            width = rect.right - rect.left;
            height = rect.bottom - rect.top;
        }
        format = DXGI_FORMAT_B8G8R8A8_UNORM;

        if (width == 0 || height == 0) {
            HookLog("OpenGL: Invalid window size");
            return;
        }

        // Initialization temporarily binds CE-owned GL objects. Preserve the
        // application's state even on a partial initialization failure.
        GLint previousReadFramebuffer = 0;
        GLint previousDrawFramebuffer = 0;
        GLint previousTexture2D = 0;
        GLint previousPixelPackBuffer = 0;
        pglGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
        pglGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
        pglGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture2D);
        pglGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPixelPackBuffer);
        auto restoreApplicationBindings = [&]() {
            pglBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previousPixelPackBuffer));
            pglBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture2D));
            pglBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
            pglBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));
        };

        // Create D3D11 device for interop
        if (!CreateD3D11Device()) {
            restoreApplicationBindings();
            CleanupGL();
            return;
        }

        // Create FBO for capturing
        pglGenFramebuffers(1, &fbo);
        pglGenTextures(1, &captureTexture);

        pglBindTexture(GL_TEXTURE_2D, captureTexture);
        pglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

        pglBindFramebuffer(GL_FRAMEBUFFER, fbo);
        pglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, captureTexture, 0);

        if (pglCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            HookLog("OpenGL: FBO not complete");
            CleanupGL();
            restoreApplicationBindings();
            return;
        }

        // Try NV interop first, fallback to PBO
        bool captureReady = false;
        if (g_NVInteropAvailable) {
            captureReady = InitNVInterop();
        }

        if (!captureReady) {
            // NV interop can fail after creating only part of its texture ring.
            // Tear that generation down before PBO creation so COM output slots,
            // handles, and GL names are never overwritten/leaked.
            CleanupTransportResources();
            captureReady = InitPBOFallback();
        }

        if (!captureReady) {
            HookLog("OpenGL: Failed to initialize capture");
            CleanupGL();
            restoreApplicationBindings();
            return;
        }

        // NV interop transfers ownership back to D3D on unlock. Queueing a
        // D3D11 fence immediately afterwards gives the media process an exact
        // cross-process completion point, just like the PBO upload path.
        if (usingNVInterop && !InitPBOFence()) {
            HookLog("OpenGL: NV interop fence unavailable; using interop's implicit synchronization");
        }

        // Publish to shared memory
        if (g_IPC) {
            PublishToSharedMemory(g_IPC);
        }

        // Re-assert MakeWindowAssociation after NV interop init: wglDXOpenDeviceNV
        // may re-enable DXGI window/cursor management. Repeat the call to ensure it
        // stays suppressed.
        if (usingNVInterop) {
            IDXGIDevice* dxgiDev = nullptr;
            if (SUCCEEDED(d3d11Device->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) {
                IDXGIAdapter* adapter = nullptr;
                if (SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
                    IDXGIFactory* factory = nullptr;
                    if (SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
                        HWND wnd = WindowFromDC(hDC);
                        if (wnd)
                            factory->MakeWindowAssociation(wnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
                        factory->Release();
                    }
                    adapter->Release();
                }
                dxgiDev->Release();
            }
        }

        restoreApplicationBindings();
        initialized = true;
        HookLog("OpenGL Capture Initialized: %dx%d (NV Interop: %s)", width, height,
                usingNVInterop ? "Yes" : "No (PBO Fallback)");
    }

    void CaptureFrame(HDC hDC) {
        std::unique_lock<std::recursive_mutex> captureLock(captureMutex, std::try_to_lock);
        if (!captureLock.owns_lock()) {
            static std::atomic<int> s_contentionLogCount{0};
            if (s_contentionLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
                HookLog("OpenGL: Skipping concurrent SwapBuffers capture while capture resources are in use");
            }
            return;
        }
        if (!initialized)
            return;

        const HGLRC currentContext = wglGetCurrentContext();
        if (!currentContext || currentContext != g_CaptureContext || hDC != g_CaptureHDC) {
            static std::atomic<int> s_foreignContextLogCount{0};
            if (s_foreignContextLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
                HookLog(
                    "OpenGL: Skipping capture from non-owner context/HDC (currentRC=%p ownerRC=%p hdc=%p "
                    "ownerHdc=%p)",
                    currentContext, g_CaptureContext, hDC, g_CaptureHDC);
            }
            return;
        }

        HWND hwnd = hDC ? WindowFromDC(hDC) : nullptr;
        RECT currentRect = {};
        if (hwnd && GetClientRect(hwnd, &currentRect)) {
            const uint32_t currentWidth =
                static_cast<uint32_t>(std::max<LONG>(0, currentRect.right - currentRect.left));
            const uint32_t currentHeight =
                static_cast<uint32_t>(std::max<LONG>(0, currentRect.bottom - currentRect.top));
            if (currentWidth > 0 && currentHeight > 0 && (currentWidth != width || currentHeight != height)) {
                HookLogImportant("OpenGL: Capture resize detected (%ux%u -> %ux%u), rebuilding shared transport", width,
                                 height, currentWidth, currentHeight);
                if (!TryCleanup(false))
                    return;
                Init(hDC);
                if (!initialized)
                    return;
            }
        }

        // Check if we should throttle capture (encoder is falling behind)
        if (g_IPC && g_IPC->GetSharedMem()) {
            if (g_IPC->GetSharedMem()->throttleCapture.load(std::memory_order_acquire)) {
                return;
            }
        }

        // Cadence gating: skip frames to maintain target FPS cadence.
        // Replaces the old hard-coded 500fps cap with proper deadline-based pacing.
        SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        if (ShouldSkipCaptureForTargetCadence(shm, "OpenGL")) {
            return;
        }

        const int idx = FindAvailableCaptureTextureSlot(shm, writeIndex.load(std::memory_order_relaxed));
        bool framePublished = false;
        if (idx < 0) {
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        writeIndex.store(idx, std::memory_order_relaxed);

        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);

        // Preserve application bindings. Capture runs at SwapBuffers, but many
        // engines assume these bindings carry into construction of the next frame.
        GLint previousReadFramebuffer = 0;
        GLint previousDrawFramebuffer = 0;
        GLint previousTexture2D = 0;
        GLint previousPixelPackBuffer = 0;
        pglGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
        pglGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
        pglGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture2D);
        pglGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPixelPackBuffer);

        // Blit backbuffer to capture texture
        pglBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        pglBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        // Flip Y during blit: OpenGL framebuffer is bottom-up (y=0=bottom) but
        // D3D11 textures are top-down (row 0=top). Swapping srcY0/srcY1 flips
        // the image so that the captured texture has row 0 = top of screen.
        pglBlitFramebuffer(0, height, width, 0, 0, 0, width, height, 0x4000 /*GL_COLOR_BUFFER_BIT*/,
                           0x2600 /*GL_NEAREST*/);
        pglBindFramebuffer(GL_FRAMEBUFFER, 0);

        if (usingNVInterop) {
            // Lock D3D11-backed GL texture, copy framebuffer contents into it, then unlock.
            // Only signal the frame if the lock actually succeeded; signaling on lock failure
            // would push stale (previously-written) texture data to the encoder.
            if (wglDXLockObjectsNV(nvDevice, 1, &nvTextureHandles[idx])) {
                pglBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
                pglBindTexture(GL_TEXTURE_2D, glTextures[idx]);
                pglCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, (GLsizei)width, (GLsizei)height);
                pglBindTexture(GL_TEXTURE_2D, 0);
                pglBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                if (wglDXUnlockObjectsNV(nvDevice, 1, &nvTextureHandles[idx])) {
                    uint64_t publishedFenceValue = 0;
                    bool completionPublished = true;
                    if (useFences && fence && context4) {
                        publishedFenceValue = ++fenceValue;
                        const HRESULT signalHr = context4->Signal(fence, publishedFenceValue);
                        if (FAILED(signalHr)) {
                            completionPublished = false;
                            useFences = false;
                            HookLog(
                                "OpenGL: NV interop fence Signal failed value=%llu hr=0x%08X; "
                                "falling back to implicit sync on later frames",
                                static_cast<unsigned long long>(publishedFenceValue), signalHr);
                        }
                        d3d11Context->Flush();
                    }
                    if (completionPublished) {
                        SignalFrameReady(g_IPC, idx, qpc.QuadPart, publishedFenceValue);
                        framePublished = true;
                    }
                } else {
                    static std::atomic<int> s_unlockFailureLogCount{0};
                    if (s_unlockFailureLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
                        HookLog("OpenGL: NV interop unlock failed for texture %d; stale frame not published", idx);
                    }
                }
            } else {
                static std::atomic<int> s_lockFailureLogCount{0};
                if (s_lockFailureLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
                    HookLog("OpenGL: NV interop lock failed for texture %d; frame skipped", idx);
                }
            }
        } else if (usePBO) {
            auto uploadPBO = [&](int readPBO) {
                pglBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[readPBO]);
                void* data = pglMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
                if (!data)
                    return;

                d3d11Context->UpdateSubresource(sharedTextures[idx], 0, NULL, data, width * 4, 0);
                const GLboolean unmapSucceeded = pglUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                if (unmapSucceeded != GL_TRUE) {
                    HookLog("OpenGL: PBO %d data became invalid while mapped; frame not published", readPBO);
                    return;
                }

                uint64_t publishedFenceValue = 0;
                bool uploadSubmitted = true;
                if (useFences && fence && context4) {
                    publishedFenceValue = ++fenceValue;
                    const HRESULT signalHr = context4->Signal(fence, publishedFenceValue);
                    if (FAILED(signalHr)) {
                        uploadSubmitted = false;
                        useFences = false;
                        HookLog("OpenGL: PBO upload fence Signal failed value=%llu hr=0x%08X",
                                static_cast<unsigned long long>(publishedFenceValue), signalHr);
                    }
                }
                d3d11Context->Flush();  // Submit UpdateSubresource and optional fence without a CPU wait.
                if (uploadSubmitted) {
                    SignalFrameReady(g_IPC, idx, pboTimestampQpc[readPBO], publishedFenceValue);
                    framePublished = true;
                }
            };

            if (pboSyncSupported) {
                // Consume one completed readback without ever waiting on the
                // Present thread. A zero-timeout wait is only a readiness query.
                int readyPBO = -1;
                for (int i = 0; i < 2; ++i) {
                    if (!pboSyncs[i])
                        continue;
                    const GLenum waitResult = pglClientWaitSync(pboSyncs[i], GL_SYNC_FLUSH_COMMANDS_BIT, 0);
                    if (waitResult == GL_ALREADY_SIGNALED || waitResult == GL_CONDITION_SATISFIED) {
                        if (readyPBO < 0 || pboTimestampQpc[i] < pboTimestampQpc[readyPBO])
                            readyPBO = i;
                    } else if (waitResult == GL_WAIT_FAILED) {
                        static std::atomic<int> s_pboWaitFailureLogCount{0};
                        if (s_pboWaitFailureLogCount.fetch_add(1, std::memory_order_relaxed) < 8)
                            HookLog("OpenGL: PBO %d sync readiness query failed; dropping readback", i);
                        // The sync's state is unknown. Finish before recycling
                        // its PBO so a failed readiness query cannot turn into a
                        // read/write race on the next glReadPixels.
                        if (pglFinish)
                            pglFinish();
                        pglDeleteSync(pboSyncs[i]);
                        pboSyncs[i] = nullptr;
                    }
                }
                if (readyPBO >= 0) {
                    pglDeleteSync(pboSyncs[readyPBO]);
                    pboSyncs[readyPBO] = nullptr;
                    uploadPBO(readyPBO);
                }

                // Queue this source frame only when a PBO slot is free. If both
                // reads are still in flight, dropping capture is preferable to
                // stalling the game's SwapBuffers call.
                int writePBO = -1;
                for (int offset = 0; offset < 2; ++offset) {
                    const int candidate = (currentPBO + offset) % 2;
                    if (!pboSyncs[candidate]) {
                        writePBO = candidate;
                        break;
                    }
                }
                if (writePBO >= 0) {
                    pglBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[writePBO]);
                    pglBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
                    pglReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, 0);
                    pglBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                    pboTimestampQpc[writePBO] = qpc.QuadPart;
                    pboSyncs[writePBO] = pglFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                    if (!pboSyncs[writePBO]) {
                        // Rare GL error: finish only this failed synchronization
                        // attempt so the PBO cannot be reused while still busy.
                        HookLog("OpenGL: Failed to create PBO sync object; completing this readback synchronously");
                        if (pglFinish)
                            pglFinish();
                        uploadPBO(writePBO);
                    }
                    currentPBO = (writePBO + 1) % 2;
                }
            } else {
                // Compatibility path for contexts with PBOs but no GL sync
                // objects. Retains capture support for old drivers.
                const int readPBO = currentPBO;
                const int writePBO = (currentPBO + 1) % 2;
                pglBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[writePBO]);
                pglBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
                pglReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, 0);
                pglBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                pboTimestampQpc[writePBO] = qpc.QuadPart;
                if (pboPopulated)
                    uploadPBO(readPBO);
                currentPBO = writePBO;
                pboPopulated = true;
            }
            pglBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        }

        pglBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previousPixelPackBuffer));
        pglBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture2D));
        pglBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
        pglBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));

        if (framePublished)
            AdvanceWriteIndex();
    }
};

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
    if (pglRenderbufferStorageMultisample)
        pglRenderbufferStorageMultisample(target, samples, internalformat, width, height);
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
    if (pglTexImage2DMultisample)
        pglTexImage2DMultisample(target, samples, internalformat, width, height, fixedsamplelocations);
}

static OpenGLCapture g_OpenGLCapture;

static void ResetTrackedOpenGLState(HGLRC contextToReset) {
    const bool resetAll = (contextToReset == NULL);
    const bool resetCapture = resetAll || contextToReset == g_CaptureContext;
    const bool resetOverlay = resetAll || contextToReset == g_OverlayContext;
    const bool resetVersionState = resetAll || contextToReset == g_CurrentTrackedContext;

    const HGLRC previousContext = wglGetCurrentContext();
    const HDC previousDC = wglGetCurrentDC();
    bool switchedToCaptureContext = false;
    bool canCleanCaptureContext =
        !resetCapture || !g_OpenGLCapture.initialized || !g_CaptureContext || previousContext == g_CaptureContext;
    if (!canCleanCaptureContext && oWglMakeCurrent && g_CaptureHDC) {
        switchedToCaptureContext = oWglMakeCurrent(g_CaptureHDC, g_CaptureContext) == TRUE;
        canCleanCaptureContext = switchedToCaptureContext;
        if (!canCleanCaptureContext) {
            HookLog("OpenGL: Deferring cleanup for owner context %p because it could not be made current",
                    g_CaptureContext);
        }
    }

    bool captureCleanupHandledOverlay = false;
    if (resetCapture && g_OpenGLCapture.initialized && canCleanCaptureContext) {
        g_OpenGLCapture.TryCleanup(true);
        captureCleanupHandledOverlay = true;
    }

    if (resetOverlay && !captureCleanupHandledOverlay && g_OverlayAdapter.IsInitialized() &&
        (!g_OverlayContext || wglGetCurrentContext() == g_OverlayContext)) {
        g_OverlayAdapter.Shutdown();
    }

    if (resetCapture && (!g_OpenGLCapture.initialized || canCleanCaptureContext)) {
        g_CaptureContext = NULL;
        g_CaptureHDC = NULL;
    }
    if (resetOverlay) {
        g_OverlayContext = NULL;
    }
    if (resetVersionState) {
        g_CurrentTrackedContext = NULL;
        g_VersionChecked = false;
        g_LegacyContext = false;
        g_OpenGLApiLabel = "OpenGL";
    }

    if (switchedToCaptureContext) {
        const BOOL restored =
            previousContext ? oWglMakeCurrent(previousDC, previousContext) : oWglMakeCurrent(nullptr, nullptr);
        if (!restored) {
            HookLog("OpenGL: Failed to restore application context %p after capture cleanup", previousContext);
        }
    }
}

static bool TrackOpenGLContext(HDC hdc) {
    HGLRC currentCtx = wglGetCurrentContext();
    if (!currentCtx)
        return false;

    if (currentCtx != g_CurrentTrackedContext) {
        if (g_CurrentTrackedContext) {
            HookLog("OpenGL: Switching tracked context from %p to %p", g_CurrentTrackedContext, currentCtx);
            ResetTrackedOpenGLState(g_CurrentTrackedContext);
        }
        g_CurrentTrackedContext = currentCtx;
    }

    HWND hwnd = WindowFromDC(hdc);
    if (hwnd && hwnd != g_CachedHwnd) {
        g_CachedHwnd = hwnd;
        InputManager::Get().HookWindow(hwnd);
        g_OverlayAdapter.SetHwnd(hwnd);
    }

    return true;
}

// Load OpenGL functions
static bool LoadGLFunctions() {
    if (g_FunctionsLoaded)
        return true;

    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl)
        return false;

    typedef PROC(WINAPI * wglGetProcAddress_t)(LPCSTR);
    wglGetProcAddress_t wglGetProcAddress_ptr = (wglGetProcAddress_t)GetProcAddress(gl, "wglGetProcAddress");
    if (!wglGetProcAddress_ptr)
        return false;

    // Load base GL functions from opengl32.dll
    pglGetError = (glGetError_t)GetProcAddress(gl, "glGetError");
    pglGetIntegerv = (glGetIntegerv_t)GetProcAddress(gl, "glGetIntegerv");
    pglFlush = (glFlush_t)GetProcAddress(gl, "glFlush");
    pglFinish = (glFinish_t)GetProcAddress(gl, "glFinish");

// Load extension functions via wglGetProcAddress
#define LOAD_GL(name)                                 \
    p##name = (name##_t)wglGetProcAddress_ptr(#name); \
    if (!p##name)                                     \
    p##name = (name##_t)GetProcAddress(gl, #name)

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
    wglDXOpenDeviceNV = (wglDXOpenDeviceNV_t)wglGetProcAddress_ptr("wglDXOpenDeviceNV");
    wglDXCloseDeviceNV = (wglDXCloseDeviceNV_t)wglGetProcAddress_ptr("wglDXCloseDeviceNV");
    wglDXRegisterObjectNV = (wglDXRegisterObjectNV_t)wglGetProcAddress_ptr("wglDXRegisterObjectNV");
    wglDXUnregisterObjectNV = (wglDXUnregisterObjectNV_t)wglGetProcAddress_ptr("wglDXUnregisterObjectNV");
    wglDXLockObjectsNV = (wglDXLockObjectsNV_t)wglGetProcAddress_ptr("wglDXLockObjectsNV");
    wglDXUnlockObjectsNV = (wglDXUnlockObjectsNV_t)wglGetProcAddress_ptr("wglDXUnlockObjectsNV");

    g_NVInteropAvailable = (wglDXOpenDeviceNV && wglDXCloseDeviceNV && wglDXRegisterObjectNV &&
                            wglDXUnregisterObjectNV && wglDXLockObjectsNV && wglDXUnlockObjectsNV);

    g_FunctionsLoaded = true;
    HookLog("OpenGL: Functions loaded (NV Interop: %s)", g_NVInteropAvailable ? "Available" : "Not Available");
    return true;
}

// Helper to load extensions
static void LoadOpenGLExtensions() {
    if (pglMinSampleShading)
        return;  // Already loaded

    // We need a current context to load extensions
    HGLRC hRC = wglGetCurrentContext();
    if (!hRC)
        return;

    // Use oWglGetProcAddress if available, otherwise assume standard loading
    typedef PROC(WINAPI * wglGetProcAddress_t)(LPCSTR);
    wglGetProcAddress_t wglGetProcAddress_ptr = nullptr;
    if (oWglGetProcAddress)
        wglGetProcAddress_ptr = (wglGetProcAddress_t)oWglGetProcAddress;
    else
        wglGetProcAddress_ptr =
            (wglGetProcAddress_t)GetProcAddress(GetModuleHandleA("opengl32.dll"), "wglGetProcAddress");

    if (!wglGetProcAddress_ptr)
        return;

    pglMinSampleShading = (glMinSampleShading_t)wglGetProcAddress_ptr("glMinSampleShading");
    if (!pglMinSampleShading)
        pglMinSampleShading = (glMinSampleShading_t)wglGetProcAddress_ptr("glMinSampleShadingARB");

    pglEnable = (glEnable_t)GetProcAddress(GetModuleHandleA("opengl32.dll"), "glEnable");
}

// Detect GPU LUID for system metrics
static void DetectGPU(HDC hdc) {
    if (g_LuidReported)
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
