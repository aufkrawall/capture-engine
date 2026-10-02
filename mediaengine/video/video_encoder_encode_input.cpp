#include "video_encoder_internal.h"

AVFrame* VideoEncoder::PrepareEncoderInputFrame(AVFrame* d3d11Frame) {
    if (!d3d11Frame) {
        return nullptr;
    }
    if (!UsesQsvHardwareFrames(savedConfig.encoder)) {
        return d3d11Frame;
    }
    if (!hwFramesCtx || d3d11Frame->format != AV_PIX_FMT_D3D11) {
        DLL_Log("[VideoEncoder] Cannot map QSV input: missing derived frames context or non-D3D11 source");
        return nullptr;
    }

    AVFrame* qsvFrame = av_frame_alloc();
    if (!qsvFrame) {
        return nullptr;
    }
    qsvFrame->format = AV_PIX_FMT_QSV;
    qsvFrame->width = d3d11Frame->width;
    qsvFrame->height = d3d11Frame->height;
    qsvFrame->hw_frames_ctx = av_buffer_ref(hwFramesCtx);
    if (!qsvFrame->hw_frames_ctx) {
        av_frame_free(&qsvFrame);
        return nullptr;
    }

    const int mapResult =
        av_hwframe_map(qsvFrame, d3d11Frame, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT);
    if (mapResult < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(mapResult, errbuf, sizeof(errbuf));
        qsvSurfaceMappingFailures++;
        if (qsvSurfaceMappingFailures <= 3 || qsvSurfaceMappingFailures % 300 == 0) {
            DLL_Log("[VideoEncoder] Direct D3D11-to-QSV surface mapping failed: %d (%s), failures=%u", mapResult,
                    errbuf, qsvSurfaceMappingFailures);
        }
        av_frame_free(&qsvFrame);
        return nullptr;
    }

    const int copyResult = av_frame_copy_props(qsvFrame, d3d11Frame);
    if (copyResult < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(copyResult, errbuf, sizeof(errbuf));
        qsvSurfaceMappingFailures++;
        if (qsvSurfaceMappingFailures <= 3 || qsvSurfaceMappingFailures % 300 == 0) {
            DLL_Log("[VideoEncoder] Failed to copy frame properties to QSV surface: %d (%s), failures=%u",
                    copyResult, errbuf, qsvSurfaceMappingFailures);
        }
        av_frame_free(&qsvFrame);
        return nullptr;
    }

    if (!qsvSurfaceMappingLogged) {
        DLL_Log("[VideoEncoder] First D3D11 frame mapped directly to a oneVPL/QSV surface (no CPU transfer)");
        qsvSurfaceMappingLogged = true;
    }
    return qsvFrame;
}

bool VideoEncoder::ResolveFrameInput(HANDLE sharedHandle, HANDLE fenceHandle, uint64_t fenceValue,
                                     uint32_t sourcePid, int format, bool isShmem, int shmemSlot,
                                     ID3D11Texture2D** outBgraTex, ID3D11Fence** outD3d11Fence) {
    ID3D11Texture2D* bgraTex = nullptr;
    ID3D11Fence* d3d11Fence = nullptr;
    int cacheSlot = -1;

    if (isShmem) {
        if (pShmem && pSharedMem && pSharedMem->GetShmemMappingCreated()) {
            // Shmem Path: Upload pixels to our owned texture
            int texIdx = 0;  // Reuse first shared capture texture (we own it)
            bgraTex = sharedCaptureTextures[texIdx];

            if (bgraTex) {
                // Validation of slot
                int slot = (shmemSlot >= 0 && shmemSlot < 2) ? shmemSlot : 0;
                uint8_t* pSrc = pShmem->GetData(slot);

                if (pSrc) {
                    D3D11_BOX box;
                    box.left = 0;
                    box.right = pSharedMem->GetWidth();  // Use current frame resolution
                    box.top = 0;
                    box.bottom = pSharedMem->GetHeight();
                    box.front = 0;
                    box.back = 1;

                    // We need a pitch. Use pSharedMem->width * 4 if not stored in
                    // ShmemBuffer Actually ShmemBuffer has pitch.
                    d3d11Context->UpdateSubresource(bgraTex, 0, &box, pSrc, pShmem->pitch, 0);
                }
                bgraTex->AddRef();     // For consistency with Release() below
                d3d11Fence = nullptr;  // No fence for shmem
            }
        }
    } else {
        // Check if layer told us to use our own encoder textures directly
        // (DXVK zero-copy path: layer imported our KMT handles into Vulkan)
        if (pSharedMem && pSharedMem->useEncoderTextures.load(std::memory_order_acquire) &&
            sharedCaptureTexturesCreated) {
            // Find which encoder texture matches by KMT handle
            int matchIdx = -1;
            for (int i = 0; i < ENCODER_TEXTURE_SLOT_COUNT; i++) {
                if (sharedCaptureKmtHandles[i] == sharedHandle) {
                    matchIdx = i;
                    break;
                }
            }
            if (matchIdx >= 0) {
                bgraTex = sharedCaptureTextures[matchIdx];
            }
            if (bgraTex) {
                bgraTex->AddRef();

                HANDLE directFenceHandle = fenceHandle;
                if ((!directFenceHandle || directFenceHandle == INVALID_HANDLE_VALUE) && pSharedMem) {
                    directFenceHandle = reinterpret_cast<HANDLE>(pSharedMem->encoderTextures.GetFenceHandle());
                }

                if (directFenceHandle && directFenceHandle != INVALID_HANDLE_VALUE && fenceValue > 0) {
                    HANDLE directFenceHandleAlt = NormalizeSourceHandleForWow64(directFenceHandle, sourcePid);
                    const bool hasDirectFenceAlt = (directFenceHandleAlt != directFenceHandle);

                    if (sourcePid > 0 && sourcePid == cachedSourcePid && cachedFenceHandle == directFenceHandle &&
                        cachedD3D11Fence) {
                        d3d11Fence = cachedD3D11Fence;
                        d3d11Fence->AddRef();
                    } else {
                        ce::HandleGuard hProcess(OpenProcess(PROCESS_DUP_HANDLE, FALSE, sourcePid));
                        HRESULT fenceHr = E_FAIL;
                        if (hProcess) {
                            ce::HandleGuard dupFence;
                            if (DuplicateHandle(hProcess.get(), directFenceHandle, GetCurrentProcess(),
                                                dupFence.addressof(), 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                                fenceHr = CallOpenSharedFence(d3d11Device, dupFence.get(), &d3d11Fence);
                            }
                            if (FAILED(fenceHr) && hasDirectFenceAlt) {
                                ce::HandleGuard dupFenceAlt;
                                if (DuplicateHandle(hProcess.get(), directFenceHandleAlt, GetCurrentProcess(),
                                                    dupFenceAlt.addressof(), 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                                    fenceHr = CallOpenSharedFence(d3d11Device, dupFenceAlt.get(), &d3d11Fence);
                                }
                            }
                        }
                        if (FAILED(fenceHr) && !video_encoder_g_HandleFailureCache.ShouldSkipFence(directFenceHandle)) {
                            fenceHr = CallOpenSharedFence(d3d11Device, directFenceHandle, &d3d11Fence);
                        }
                        if (FAILED(fenceHr) && hasDirectFenceAlt) {
                            fenceHr = CallOpenSharedFence(d3d11Device, directFenceHandleAlt, &d3d11Fence);
                        }

                        if (d3d11Fence) {
                            if (cachedD3D11Fence) {
                                cachedD3D11Fence->Release();
                            }
                            cachedD3D11Fence = d3d11Fence;
                            cachedD3D11Fence->AddRef();
                            cachedFenceHandle = directFenceHandle;
                            cachedSourcePid = sourcePid;
                        } else if (encodeFrameCounter < 20) {
                            DLL_Log(
                                "[VideoEncoder] Frame %d: Failed to open encoder-texture fence handle=%p value=%llu "
                                "pid=%u",
                                encodeFrameCounter, directFenceHandle, static_cast<unsigned long long>(fenceValue),
                                sourcePid);
                        }
                    }
                }
            }
            if (matchIdx >= 0 && encodeFrameCounter < 10) {
                DLL_Log(
                    "[VideoEncoder] Frame %d: Using encoder-owned texture[%d] directly (encoder fence=%p value=%llu)",
                    encodeFrameCounter, matchIdx, fenceHandle, static_cast<unsigned long long>(fenceValue));
            }
        }

        if (!bgraTex) {
            // Standard shared handle path
            HANDLE sharedHandleAlt = NormalizeSourceHandleForWow64(sharedHandle, sourcePid);
            HANDLE fenceHandleAlt = NormalizeSourceHandleForWow64(fenceHandle, sourcePid);
            const bool hasSharedAlt = (sharedHandleAlt != sharedHandle);
            const bool hasFenceAlt = (fenceHandleAlt != fenceHandle);

            // Check cache for valid fence and texture (Quad-Buffered Cache)
            // Texture caching works independently of fence (for D3D11 KMT path)
            cacheSlot = -1;
            bool skipFence = (fenceValue == 0 || fenceHandle == 0 || fenceHandle == INVALID_HANDLE_VALUE);
            bool fenceValid = !skipFence && (sourcePid > 0 && sourcePid == cachedSourcePid &&
                                             fenceHandle == cachedFenceHandle && cachedD3D11Fence);

            // For texture matching, we only need matching PID and handle
            // (fence-independent)
            bool pidMatches = (sourcePid > 0 && sourcePid == cachedSourcePid);

            // Search for cached texture by handle (works with or without fence)
            if (pidMatches) {
                for (int i = 0; i < SHARED_TEXTURE_SLOT_COUNT; i++) {
                    if (cachedTextureHandles[i] == sharedHandle && cachedSharedTextures[i]) {
                        cacheSlot = i;
                        break;
                    }
                }
            } else if (sourcePid > 0) {
                // New process -> Clear all cache
                for (int i = 0; i < SHARED_TEXTURE_SLOT_COUNT; i++) {
                    if (cachedSharedTextures[i]) {
                        cachedSharedTextures[i]->Release();
                        cachedSharedTextures[i] = nullptr;
                    }
                    cachedTextureHandles[i] = nullptr;
                }
                if (cachedD3D11Fence) {
                    cachedD3D11Fence->Release();
                    cachedD3D11Fence = nullptr;
                }
                cachedFenceHandle = nullptr;
                cachedSourcePid = sourcePid;  // Remember new PID
            }

            // ID3D11Texture2D *bgraTex = nullptr; // Moved up
            // ID3D11Fence *d3d11Fence = nullptr;   // Moved up

            if (cacheSlot >= 0) {

                // Full Cache Hit
                bgraTex = cachedSharedTextures[cacheSlot];
                d3d11Fence = cachedD3D11Fence;  // May be null for D3D11 KMT path (no fence)
                bgraTex->AddRef();
                if (d3d11Fence) {
                    d3d11Fence->AddRef();
                }

                if (encodeFrameCounter % kCacheLogIntervalFrames == 1) {
                    DLL_Log("[VideoEncoder] Using cached handles (pid=%u, slot=%d, frame=%d)", sourcePid, cacheSlot,
                            encodeFrameCounter);
                }
            } else {
                // Cache Miss (Partial or Full)
                // Use RAII to ensure handle is closed if we return early
                ce::HandleGuard hProcess(OpenProcess(PROCESS_DUP_HANDLE, FALSE, sourcePid));

                if (!hProcess) {
                    DLL_Log("[VideoEncoder] Frame %d: Failed to Open Process %u", encodeFrameCounter, sourcePid);
                    return false;
                }

                // 1. Handle Fence (Reuse if valid, Open if not)
                if (skipFence) {
                    d3d11Fence = nullptr;
                    if (encodeFrameCounter % 60 == 0)
                        DLL_Log("[VideoEncoder] Frame %d: SkipFence is true (Val=%llu Hnd=%p)", encodeFrameCounter,
                                fenceValue, fenceHandle);
                } else if (fenceValid) {
                    d3d11Fence = cachedD3D11Fence;
                    d3d11Fence->AddRef();
                } else {
                    ce::HandleGuard dupFence;
                    HRESULT hr = E_FAIL;

                    // CRITICAL: Always DuplicateHandle first to validate handles.
                    if (DuplicateHandle(hProcess.get(), fenceHandle, GetCurrentProcess(), dupFence.addressof(), 0,
                                        FALSE, DUPLICATE_SAME_ACCESS)) {
                        // Handle duplicated successfully - safe to call OpenSharedFence
                        hr = CallOpenSharedFence(d3d11Device, dupFence.get(), &d3d11Fence);
                        if (FAILED(hr) && encodeFrameCounter < 10) {
                            DLL_Log("[VideoEncoder] OpenSharedFence(dup) failed: HR=%x (Hnd=%p)", hr, dupFence.get());
                        }
                    } else {
                        DWORD err = GetLastError();
                        if (encodeFrameCounter < 10) {
                            DLL_Log("[VideoEncoder] DuplicateHandle failed: Err=%d (SrcPid=%u Hnd=%p)", err, sourcePid,
                                    fenceHandle);
                        }
                        // Last resort: try direct handle (may work for same-process or KMT handles)
                        if (!video_encoder_g_HandleFailureCache.ShouldSkipFence(fenceHandle)) {
                            hr = CallOpenSharedFence(d3d11Device, fenceHandle, &d3d11Fence);
                        }
                    }

                    // Alternate handle representation for WOW64 sources
                    if (FAILED(hr) && hasFenceAlt) {
                        ce::HandleGuard dupFenceAlt;
                        // Try direct first
                        hr = CallOpenSharedFence(d3d11Device, fenceHandleAlt, &d3d11Fence);
                        if (FAILED(hr)) {
                            if (DuplicateHandle(hProcess.get(), fenceHandleAlt, GetCurrentProcess(),
                                                dupFenceAlt.addressof(), 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                                hr = CallOpenSharedFence(d3d11Device, dupFenceAlt.get(), &d3d11Fence);
                            }
                        }
                    }

                    // Final fallback - try as generic shared resource
                    if (FAILED(hr)) {
                        hr = CallOpenSharedResource(d3d11Device, fenceHandle, IID_PPV_ARGS(&d3d11Fence));
                    }
                    if (FAILED(hr) && hasFenceAlt) {
                        hr = CallOpenSharedResource(d3d11Device, fenceHandleAlt, IID_PPV_ARGS(&d3d11Fence));
                    }
                    if (FAILED(hr) && encodeFrameCounter < 10) {
                        DLL_Log(
                            "[VideoEncoder] OpenSharedFence(direct) failed: HR=%x (Hnd=%p), trying "
                            "DuplicateHandle...",
                            hr, fenceHandle);
                    }

                    // Fallback to DuplicateHandle path (for handles that support it)
                    if (FAILED(hr)) {
                        if (DuplicateHandle(hProcess.get(), fenceHandle, GetCurrentProcess(), dupFence.addressof(), 0,
                                            FALSE, DUPLICATE_SAME_ACCESS)) {
                            hr = CallOpenSharedFence(d3d11Device, dupFence.get(), &d3d11Fence);
                            if (FAILED(hr)) {
                                DLL_Log("[VideoEncoder] OpenSharedFence(dup) failed: HR=%x (Hnd=%p)", hr,
                                        dupFence.get());
                            }
                        } else {
                            DWORD err = GetLastError();
                            DLL_Log(
                                "[VideoEncoder] DuplicateHandle failed: Err=%d (SrcPid=%u "
                                "Hnd=%p)",
                                err, sourcePid, fenceHandle);
                        }
                    }

                    // Alternate handle representation for WOW64 sources
                    if (FAILED(hr) && hasFenceAlt) {
                        ce::HandleGuard dupFenceAlt;
                        // Use DuplicateHandle first for safety
                        if (DuplicateHandle(hProcess.get(), fenceHandleAlt, GetCurrentProcess(),
                                            dupFenceAlt.addressof(), 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                            hr = CallOpenSharedFence(d3d11Device, dupFenceAlt.get(), &d3d11Fence);
                        }
                    }

                    // Final fallback - try as generic shared resource
                    if (FAILED(hr)) {
                        hr = CallOpenSharedResource(d3d11Device, fenceHandle, IID_PPV_ARGS(&d3d11Fence));
                    }
                    if (FAILED(hr) && hasFenceAlt) {
                        hr = CallOpenSharedResource(d3d11Device, fenceHandleAlt, IID_PPV_ARGS(&d3d11Fence));
                    }

                    if (d3d11Fence && encodeFrameCounter < 10) {
                        DLL_Log("[VideoEncoder] Successfully opened shared fence for PID %u", sourcePid);
                    }
                    // Cache Fence if successfully opened
                    if (d3d11Fence) {
                        if (cachedD3D11Fence)
                            cachedD3D11Fence->Release();
                        cachedD3D11Fence = d3d11Fence;
                        cachedD3D11Fence->AddRef();
                        cachedFenceHandle = fenceHandle;
                        cachedSourcePid = sourcePid;
                    }
                }

                // 2. Open Texture (We know it's missing if we are here)
                ce::HandleGuard dupTex;
                HRESULT hr = E_FAIL;
                HRESULT hrNtDirect = E_FAIL;
                HRESULT hrNtDup = E_FAIL;
                HRESULT hrKmtDup = E_FAIL;
                HRESULT hrNtAltDirect = E_FAIL;
                HRESULT hrNtAltDup = E_FAIL;
                HRESULT hrKmtAltDup = E_FAIL;
                HRESULT hrKmtDirect = E_FAIL;
                HRESULT hrKmtAltDirect = E_FAIL;

                if (encodeFrameCounter < 10) {
                    DLL_Log(
                        "[VideoEncoder] Frame %d: Opening shared texture: handle=%p, "
                        "sourcePid=%u (cached=%u, match=%s), format=%d",
                        encodeFrameCounter, sharedHandle, sourcePid, cachedSourcePid,
                        (sourcePid == cachedSourcePid) ? "yes" : "no", format);
                }

                if (sharedHandle == NULL) {
                    DLL_Log("[VideoEncoder] Frame %d: Error: sharedHandle is NULL", encodeFrameCounter);
                } else {
                    // D3D11 OpenSharedResource can throw SEH exceptions for invalid handles or
                    // incompatible formats. DuplicateHandle first to validate handle accessibility.
                    // Even duplicated handles can fail if D3D12/D3D11 devices are incompatible.
                    ce::HandleGuard dupTexDirect;
                    bool handleValid = DuplicateHandle(hProcess.get(), sharedHandle, GetCurrentProcess(),
                                                       dupTexDirect.addressof(), 0, FALSE, DUPLICATE_SAME_ACCESS);
                    if (handleValid) {
                        // Handle duplicated - try OpenSharedResource1 with the valid handle
                        hr = CallOpenSharedResource1(d3d11Device, dupTexDirect.get(), IID_PPV_ARGS(&bgraTex));
                        hrNtDup = hr;
                    } else {
                        hrNtDup = HRESULT_FROM_WIN32(GetLastError());
                        if (encodeFrameCounter < 10)
                            DLL_Log("[VideoEncoder] Frame %d: DuplicateHandle for texture failed: %p",
                                    encodeFrameCounter, sharedHandle);
                    }

                    if (FAILED(hr) && encodeFrameCounter < 10) {
                        DLL_Log(
                            "[VideoEncoder] Frame %d: OpenSharedResource1(direct=%p) "
                            "failed HR=%x. Trying KMT path...",
                            encodeFrameCounter, sharedHandle, hr);
                    }

                    // Fallback to KMT path with duplicated handle
                    if (FAILED(hr) && handleValid) {
                        hr = CallOpenSharedResource(d3d11Device, dupTexDirect.get(), IID_PPV_ARGS(&bgraTex));
                        hrKmtDup = hr;
                        if (SUCCEEDED(hr) && encodeFrameCounter < 10) {
                            DLL_Log("[VideoEncoder] Frame %d: Opened duplicated handle %p via KMT path",
                                    encodeFrameCounter, dupTexDirect.get());
                        }
                    }

                    // Try original handle as last resort (may work for same-process)
                    if (FAILED(hr) && !video_encoder_g_HandleFailureCache.ShouldSkipTexture(sharedHandle)) {
                        hr = CallOpenSharedResource(d3d11Device, sharedHandle, IID_PPV_ARGS(&bgraTex));
                        hrKmtDirect = hr;
                        if (SUCCEEDED(hr) && encodeFrameCounter < 10) {
                            DLL_Log("[VideoEncoder] Frame %d: Opened handle %p via KMT direct path", encodeFrameCounter,
                                    sharedHandle);
                        }
                    }

                    // WOW64 producers publish a 32-bit handle value in the
                    // shared ABI. Try its normalized representation once, but
                    // do not retry either representation after a sleep: ring
                    // publication already supplies the required ordering.
                    if (FAILED(hr) && hasSharedAlt) {
                        ce::HandleGuard dupTexAlt;
                        if (DuplicateHandle(hProcess.get(), sharedHandleAlt, GetCurrentProcess(), dupTexAlt.addressof(),
                                            0, FALSE, DUPLICATE_SAME_ACCESS)) {
                            hr = CallOpenSharedResource1(d3d11Device, dupTexAlt.get(), IID_PPV_ARGS(&bgraTex));
                            hrNtAltDup = hr;
                            if (FAILED(hr)) {
                                hr = CallOpenSharedResource(d3d11Device, dupTexAlt.get(), IID_PPV_ARGS(&bgraTex));
                                hrKmtAltDup = hr;
                            }
                        } else {
                            hrNtAltDup = HRESULT_FROM_WIN32(GetLastError());
                        }
                        if (FAILED(hr) && !video_encoder_g_HandleFailureCache.ShouldSkipTexture(sharedHandleAlt)) {
                            hr = CallOpenSharedResource(d3d11Device, sharedHandleAlt, IID_PPV_ARGS(&bgraTex));
                            hrKmtAltDirect = hr;
                        }
                    }

                    // Frame-ring publication uses release/acquire ordering, so
                    // a published handle cannot become more valid after an
                    // arbitrary sleep. Immediate retries only stalled the CFR
                    // encoder by up to 6 ms and repeated the same failing driver
                    // calls. Defer the frame to the existing bounded lineage
                    // retry path instead; a later publication/device state can
                    // then be observed without blocking the real-time thread.
                    if (FAILED(hr)) {
                        lastFrameDeferred.store(true, std::memory_order_relaxed);
                    }
                }  // end of else (sharedHandle != NULL)

                if (FAILED(hr)) {
                    static std::atomic<int> s_openDetailLogCount{0};
                    if (s_openDetailLogCount.fetch_add(1, std::memory_order_relaxed) < 16) {
                        DLL_Log(
                            "[VideoEncoder] Frame %d: Open detail h=%p alt=%p ntDir=%x ntDup=%x ntAltDir=%x "
                            "ntAltDup=%x "
                            "kmtDup=%x kmtAltDup=%x kmtDir=%x kmtAltDir=%x",
                            encodeFrameCounter, sharedHandle, sharedHandleAlt, hrNtDirect, hrNtDup, hrNtAltDirect,
                            hrNtAltDup, hrKmtDup, hrKmtAltDup, hrKmtDirect, hrKmtAltDirect);
                    }
                    DLL_Log(
                        "[VideoEncoder] Frame %d: Failed to OpenSharedResource (NT/KMT) "
                        "HR=%x, handle=%p, sourcePid=%u, format=%d",
                        encodeFrameCounter, hr, sharedHandle, sourcePid, format);
                    // Clean up fence if we opened it but failed texture
                    if (d3d11Fence) {
                        d3d11Fence->Release();
                    }
                    return false;
                }

                // Cache Texture
                // Find empty cache slot.
                int targetSlot = 0;
                for (int i = 0; i < SHARED_TEXTURE_SLOT_COUNT; i++) {
                    if (cachedTextureHandles[i] == nullptr) {
                        targetSlot = i;
                        break;
                    }
                    if (i == SHARED_TEXTURE_SLOT_COUNT - 1)
                        targetSlot = 0;  // Fallback to 0 if all full
                }

                if (cachedSharedTextures[targetSlot]) {
                    cachedSharedTextures[targetSlot]->Release();
                }

                cachedSharedTextures[targetSlot] = bgraTex;
                cachedSharedTextures[targetSlot]->AddRef();
                cachedTextureHandles[targetSlot] = sharedHandle;

                // hProcess, dupTex, dupFence are auto-closed by RAII here
                cacheSlot = targetSlot;
            }
        }  // End of if (!bgraTex) - standard shared handle path
    }
    *outBgraTex = bgraTex;
    *outD3d11Fence = d3d11Fence;
    return true;
}
