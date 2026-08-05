#include "opengl_hook_internal.h"


void OpenGLCapture::CaptureFrame(HDC hDC) {


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
        if (!currentContext || currentContext != opengl_hook_g_CaptureContext || hDC != opengl_hook_g_CaptureHDC) {
            static std::atomic<int> s_foreignContextLogCount{0};
            if (s_foreignContextLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
                HookLog(
                    "OpenGL: Skipping capture from non-owner context/HDC (currentRC=%p ownerRC=%p hdc=%p "
                    "ownerHdc=%p)",
                    currentContext, opengl_hook_g_CaptureContext, hDC, opengl_hook_g_CaptureHDC);
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
        opengl_hook_pglGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
        opengl_hook_pglGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
        opengl_hook_pglGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture2D);
        opengl_hook_pglGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPixelPackBuffer);

        // Blit backbuffer to capture texture
        opengl_hook_pglBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        opengl_hook_pglBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        // Flip Y during blit: OpenGL framebuffer is bottom-up (y=0=bottom) but
        // D3D11 textures are top-down (row 0=top). Swapping srcY0/srcY1 flips
        // the image so that the captured texture has row 0 = top of screen.
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        opengl_hook_pglBlitFramebuffer(0, height, width, 0, 0, 0, width, height, 0x4000 /*GL_COLOR_BUFFER_BIT*/,
                           0x2600 /*GL_NEAREST*/);
        opengl_hook_pglBindFramebuffer(GL_FRAMEBUFFER, 0);

        if (usingNVInterop) {
            // Lock D3D11-backed GL texture, copy framebuffer contents into it, then unlock.
            // Only signal the frame if the lock actually succeeded; signaling on lock failure
            // would push stale (previously-written) texture data to the encoder.
            if (opengl_hook_wglDXLockObjectsNV(nvDevice, 1, &nvTextureHandles[idx])) {
                opengl_hook_pglBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
                opengl_hook_pglBindTexture(GL_TEXTURE_2D, glTextures[idx]);
                opengl_hook_pglCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, (GLsizei)width, (GLsizei)height);
                opengl_hook_pglBindTexture(GL_TEXTURE_2D, 0);
                opengl_hook_pglBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                if (opengl_hook_wglDXUnlockObjectsNV(nvDevice, 1, &nvTextureHandles[idx])) {
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
                opengl_hook_pglBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[readPBO]);
                void* data = opengl_hook_pglMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
                if (!data)
                    return;

                d3d11Context->UpdateSubresource(sharedTextures[idx], 0, NULL, data, width * 4, 0);
                const GLboolean unmapSucceeded = opengl_hook_pglUnmapBuffer(GL_PIXEL_PACK_BUFFER);
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
                    const GLenum waitResult = opengl_hook_pglClientWaitSync(pboSyncs[i], GL_SYNC_FLUSH_COMMANDS_BIT, 0);
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
                        if (opengl_hook_pglFinish)
                            opengl_hook_pglFinish();
                        opengl_hook_pglDeleteSync(pboSyncs[i]);
                        pboSyncs[i] = nullptr;
                    }
                }
                if (readyPBO >= 0) {
                    opengl_hook_pglDeleteSync(pboSyncs[readyPBO]);
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
                    opengl_hook_pglBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[writePBO]);
                    opengl_hook_pglBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    opengl_hook_pglReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, 0);
                    opengl_hook_pglBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                    pboTimestampQpc[writePBO] = qpc.QuadPart;
                    pboSyncs[writePBO] = opengl_hook_pglFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                    if (!pboSyncs[writePBO]) {
                        // Rare GL error: finish only this failed synchronization
                        // attempt so the PBO cannot be reused while still busy.
                        HookLog("OpenGL: Failed to create PBO sync object; completing this readback synchronously");
                        if (opengl_hook_pglFinish)
                            opengl_hook_pglFinish();
                        uploadPBO(writePBO);
                    }
                    currentPBO = (writePBO + 1) % 2;
                }
            } else {
                // Compatibility path for contexts with PBOs but no GL sync
                // objects. Retains capture support for old drivers.
                const int readPBO = currentPBO;
                const int writePBO = (currentPBO + 1) % 2;
                opengl_hook_pglBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[writePBO]);
                opengl_hook_pglBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                opengl_hook_pglReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, 0);
                opengl_hook_pglBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                pboTimestampQpc[writePBO] = qpc.QuadPart;
                if (pboPopulated)
                    uploadPBO(readPBO);
                currentPBO = writePBO;
                pboPopulated = true;
            }
            opengl_hook_pglBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        }

        opengl_hook_pglBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previousPixelPackBuffer));
        opengl_hook_pglBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture2D));
        opengl_hook_pglBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
        opengl_hook_pglBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));

        if (framePublished)
            AdvanceWriteIndex();

}
