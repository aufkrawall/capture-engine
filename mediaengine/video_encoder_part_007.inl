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
                        if (!g_HandleFailureCache.ShouldSkipFence(fenceHandle)) {
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
                    if (FAILED(hr) && !g_HandleFailureCache.ShouldSkipTexture(sharedHandle)) {
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
                        if (FAILED(hr) && !g_HandleFailureCache.ShouldSkipTexture(sharedHandleAlt)) {
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
    }  // End of isShmem else block

    // 2. Wait on Synchronization using D3D11 Fence
    // PROTECTED: Immediate Context access
    D3D11ScopedLock lock;

    HRESULT hr = S_OK;

    // D3D11 FENCE PATH (Async GPU sync)
    auto beforeFence = PerfTimer::now();
    if (d3d11Fence) {
        // CPU-side timeout to prevent GPU hangs (Resilience improvement)
        if (!fenceEvent) {
            fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        }

        // Check if fence is already satisfied (avoid SetEvent overhead if possible)
        if (d3d11Fence->GetCompletedValue() < fenceValue) {
            d3d11Fence->SetEventOnCompletion(fenceValue, fenceEvent);
            // Non-blocking fence check: poll with 0ms timeout instead of
            // blocking the encoder thread. At 100% GPU, the fence may take
            // 1-5ms to signal — blocking for 200ms collapses the cadence.
            // If the fence isn't ready, we skip this frame (return false)
            // and the Bresenham produces a duplicate. Stutter > corruption.
            DWORD waitRes = WaitForSingleObject(fenceEvent, 0);
            if (waitRes == WAIT_TIMEOUT) {
                // Fence not ready — skip this frame, encoder thread stays responsive
                bgraTex->Release();
                d3d11Fence->Release();
                d3d11Fence = nullptr;
                lastFrameDeferred.store(true, std::memory_order_relaxed);
                return false;
            }
        }

        // Async GPU Wait (plus CPU timeout check above)
        d3d11Context->Wait(d3d11Fence, fenceValue);
    }
    auto afterFence = PerfTimer::now();
    stats.fenceWaitMs = PerfTimer::elapsed_ms(beforeFence, afterFence);

    if (d3d11Fence) {
        d3d11Fence->Release();
        d3d11Fence = nullptr;
    }

    if (FAILED(hr)) {
        DLL_Log("[VideoEncoder] Frame %d: Failed to Wait on Fence. HR=%x", encodeFrameCounter, hr);
        bgraTex->Release();
        return false;
    }

    auto afterOpen = PerfTimer::now();
    const AVPixelFormat activeSwFormat = GetActiveD3D11SwFormat();
    const bool useDirectRgbPath = IsDirectRgbD3D11SwFormat(activeSwFormat);

    // 4. Ensure Video Processor is initialized before RGB -> YUV conversion.
    if (!useDirectRgbPath && !videoProcessorInit) {
        if (!InitVideoProcessor()) {
            DLL_Log("[VideoEncoder] Frame %d: VP init failed", encodeFrameCounter);
            bgraTex->Release();
            return false;
        }
    }

    auto beforeConvert = PerfTimer::now();
    AVFrame* d3d11Frame = av_frame_alloc();
    if (!d3d11Frame) {
        bgraTex->Release();
        return false;
    }
    d3d11Frame->format = AV_PIX_FMT_D3D11;
    d3d11Frame->width = codecCtx->width;
    d3d11Frame->height = codecCtx->height;
    d3d11Frame->hw_frames_ctx = av_buffer_ref(d3d11FramesCtx);

    if (useDirectRgbPath) {
        const int frameRet = av_hwframe_get_buffer(d3d11FramesCtx, d3d11Frame, 0);
        if (frameRet < 0 || !d3d11Frame->data[0]) {
            DLL_Log("[VideoEncoder] Frame %d: Failed to allocate D3D11 HW frame for direct RGB path: %d",
                    encodeFrameCounter, frameRet);
            bgraTex->Release();
            av_frame_free(&d3d11Frame);
            return false;
        }

        if (!PrepareD3D11TextureForEncode(bgraTex, (ID3D11Texture2D*)d3d11Frame->data[0], CursorCompositionActive(), 0,
                                          0)) {
            DLL_Log("[VideoEncoder] Frame %d: Direct D3D11 RGB preparation failed", encodeFrameCounter);
            bgraTex->Release();
            av_frame_free(&d3d11Frame);
            return false;
        }
    } else {
        // 6. Point-composite a separate cursor in RGB, then convert the one
        // deterministic RGB stream to NV12/P010 on the GPU.
        if (!ConvertBGRAtoNV12(bgraTex, d3d11Frame, CursorCompositionActive(), true)) {
            DLL_Log("[VideoEncoder] Frame %d: GPU color conversion failed", encodeFrameCounter);
            bgraTex->Release();
            av_frame_free(&d3d11Frame);
            return false;
        }

        d3d11Frame->width = scalingEnabled ? outputWidth : width;
        d3d11Frame->height = scalingEnabled ? outputHeight : height;
    }

    auto afterConvert = PerfTimer::now();
    stats.colorConvertMs = PerfTimer::elapsed_ms(beforeConvert, afterConvert);
    bgraTex->Release();
    if (!ApplyFrameColorMetadata(d3d11Frame, codecCtx, savedConfig.hdrNominalPeakNits)) {
        av_frame_free(&d3d11Frame);
        return false;
    }

    // Calculate relative PTS (start from 0) — timestamp is in microseconds
    const bool commitsStartPts = startPts.load(std::memory_order_relaxed) < 0;
    const int64_t effectiveStartPts = commitsStartPts ? timestamp : startPts.load(std::memory_order_relaxed);
    const int64_t targetPts = ComputeTargetVideoPts(timestamp, savedConfig.useVFR, savedConfig.fps, effectiveStartPts,
                                                    lastAssignedVideoPts, false);

    // 5. Encode (Direct D3D11 Frame) - with proper packet draining
    AVPacket* pkt = av_packet_alloc();
    int packetCount = 0;

    // Pure encoding time measurement (excluding color conversion/wait)
    auto encodeStart = PerfTimer::now();

    // Helper lambda to drain all available packets
    auto drainPackets = [&]() {
        while (true) {
            const auto receiveStart = PerfTimer::now();
            int ret = avcodec_receive_packet(codecCtx, pkt);
            const auto receiveEnd = PerfTimer::now();
            encoderReceiveAccumUs +=
                static_cast<uint64_t>(std::max(0.0, PerfTimer::elapsed_ms(receiveStart, receiveEnd) * 1000.0));
            ++encoderReceiveCalls;
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
                DLL_Log("[VideoEncoder] avcodec_receive_packet failed: %d (%s)", ret, errbuf);
                break;
            }

            packetCount++;
            const int64_t packetKey = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
            const auto submitIt = encoderSubmitQpcByPts.find(packetKey);
            if (submitIt != encoderSubmitQpcByPts.end()) {
                LARGE_INTEGER nowQpc = {};
                LARGE_INTEGER frequency = {};
                QueryPerformanceCounter(&nowQpc);
                QueryPerformanceFrequency(&frequency);
                if (frequency.QuadPart > 0 && nowQpc.QuadPart >= submitIt->second) {
                    const uint64_t latencyUs = static_cast<uint64_t>(nowQpc.QuadPart - submitIt->second) * 1000000ull /
                                               static_cast<uint64_t>(frequency.QuadPart);
                    encoderPacketLatencyAccumUs += latencyUs;
                    ++encoderPacketLatencySamples;
                    encoderPacketLatencyMaxUs = std::max(encoderPacketLatencyMaxUs, SaturatingToUint32(latencyUs));
                }
                encoderSubmitQpcByPts.erase(submitIt);
            }
            pkt->stream_index = stream->index;  // Ensure video stream index

            // Duration Logic
            if (savedConfig.useVFR) {
                // For VFR, duration is variable. Best guess is target frame duration.
                // Since time_base is 1us, duration is in us.
                pkt->duration = 1000000 / savedConfig.fps;
            } else {
                // For CFR, duration is 1 unit (1/FPS)
                pkt->duration = 1;
            }

            if (onPacket)
                onPacket(pkt);
            av_packet_unref(pkt);
        }
    };

    auto sendFrame = [&](AVFrame* frame) -> bool {
        drainPackets();
        auto timedSend = [&](AVFrame* sendTarget) {
            const auto sendStart = PerfTimer::now();
            const int result = avcodec_send_frame(codecCtx, sendTarget);
            const auto sendEnd = PerfTimer::now();
            encoderSendAccumUs +=
                static_cast<uint64_t>(std::max(0.0, PerfTimer::elapsed_ms(sendStart, sendEnd) * 1000.0));
            ++encoderSendCalls;
            return result;
        };
        int ret = timedSend(frame);
        int retries = 0;
        if (ret == AVERROR(EAGAIN))
            ++encoderEagainDrainCount;
        while (ret == AVERROR(EAGAIN) && retries < 10) {
            if (retries == 0) {
                lastEncoderOverloadTickMs.store(GetTickCount64(), std::memory_order_relaxed);
                PublishRuntimeState();
            }
            drainPackets();
            ret = timedSend(frame);
            retries++;
        }
        if (ret == AVERROR(EAGAIN)) {
            DLL_Log("[VideoEncoder] avcodec_send_frame remained EAGAIN after %d drain attempts", retries);
            return false;
        }
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
            DLL_Log("[VideoEncoder] avcodec_send_frame failed: %d (%s)", ret, errbuf);
            return false;
        }
        if (frame && frame->pts != AV_NOPTS_VALUE) {
            LARGE_INTEGER submitted = {};
            QueryPerformanceCounter(&submitted);
            encoderSubmitQpcByPts[frame->pts] = submitted.QuadPart;
            while (encoderSubmitQpcByPts.size() > 256)
                encoderSubmitQpcByPts.erase(encoderSubmitQpcByPts.begin());
        }
        drainPackets();
        return true;
    };

    bool success = true;

    d3d11Frame->pts = targetPts;
    AVFrame* encoderInputFrame = PrepareEncoderInputFrame(d3d11Frame);

    if (encoderInputFrame) {
        success = sendFrame(encoderInputFrame);
    } else {
        success = false;
    }
    if (encoderInputFrame != d3d11Frame) {
        av_frame_free(&encoderInputFrame);
    }

    const uint64_t timingNow = GetTickCount64();
    if (encoderTimingLastLogTick == 0 || timingNow - encoderTimingLastLogTick >= 1000) {
        const uint64_t sendAvgUs = encoderSendCalls > 0 ? encoderSendAccumUs / encoderSendCalls : 0;
        const uint64_t receiveAvgUs = encoderReceiveCalls > 0 ? encoderReceiveAccumUs / encoderReceiveCalls : 0;
        const uint64_t packetLatencyAvgUs =
            encoderPacketLatencySamples > 0 ? encoderPacketLatencyAccumUs / encoderPacketLatencySamples : 0;
        DLL_Log(
            "[VideoEncoder Timing] sendAvg=%lluus receiveAvg=%lluus submitToPacket=%llu/%uus "
            "eagainDrain=%u pendingPts=%zu",
            static_cast<unsigned long long>(sendAvgUs), static_cast<unsigned long long>(receiveAvgUs),
            static_cast<unsigned long long>(packetLatencyAvgUs), encoderPacketLatencyMaxUs, encoderEagainDrainCount,
            encoderSubmitQpcByPts.size());
        encoderSendAccumUs = 0;
        encoderSendCalls = 0;
        encoderReceiveAccumUs = 0;
        encoderReceiveCalls = 0;
        encoderPacketLatencyAccumUs = 0;
        encoderPacketLatencySamples = 0;
        encoderPacketLatencyMaxUs = 0;
        encoderEagainDrainCount = 0;
        encoderTimingLastLogTick = timingNow;
    }

    auto afterEncode = PerfTimer::now();
    stats.ptsMs = RoundUsToMs(timestamp);
    stats.textureOpenMs = PerfTimer::elapsed_ms(frameStart, afterOpen);
    stats.colorConvertMs = PerfTimer::elapsed_ms(afterOpen, afterConvert);
    stats.encodeMs = PerfTimer::elapsed_ms(encodeStart, afterEncode);
    stats.totalMs = PerfTimer::elapsed_ms(frameStart, afterEncode);

    // Update last frame encode time (in microseconds)
    // This is robust against timer noise/underflow compared to (Total - Wait).
    lastEncodeTimeUs = (int64_t)(PerfTimer::elapsed_ms(afterFence, afterEncode) * 1000.0);
    lastFenceWaitUs = (int64_t)(stats.fenceWaitMs * 1000.0);
    stats.packetsProduced = packetCount;

    av_packet_free(&pkt);

    if (!success) {
        av_frame_free(&d3d11Frame);
        return false;
    }

    if (commitsStartPts) {
        startPts.store(effectiveStartPts, std::memory_order_relaxed);
        DLL_Log("[VideoEncoder] Recording started at PTS %lld us", static_cast<long long>(effectiveStartPts));
    }
    g_lastFramePts = timestamp;
    lastAssignedVideoPts = d3d11Frame->pts;

    // Update global stats
    g_framesEncoded++;
    outputFrameCount++;
    CacheRepeatFrameTexture(reinterpret_cast<ID3D11Texture2D*>(d3d11Frame->data[0]));
    g_totalFenceWait += stats.fenceWaitMs;
    g_totalColorConvert += stats.colorConvertMs;
    g_totalEncode += stats.encodeMs;
    if (stats.totalMs > g_maxFrameTime)
        g_maxFrameTime = stats.totalMs;
    if (stats.totalMs > expectedFrameMs * 2)
        g_slowFrameCount++;

    // Log individual slow frames for debugging
    // Log more frequently for performance tuning (every 30 frames)
    if (stats.totalMs > expectedFrameMs * 2 || encodeFrameCounter <= 5 || encodeFrameCounter % 30 == 0) {
        std::string features = "";
        if (IsConfiguredNvencLookaheadActive(savedConfig.lookahead))
            features += "Lookahead ";
        if (savedConfig.spatialAq || savedConfig.temporalAq)
            features += "AQ ";
        if (savedConfig.bFrames > 0)
            features += "B-Frames ";
        if (IsConfiguredNvencMultipassActive(savedConfig))
            features += "Multipass ";

        const char* slowLabel = (stats.totalMs > expectedFrameMs * 2) ? "(SLOW!)" : "";

        DLL_Log(
            "[PERF] Frame %d: TOTAL=%.2fms %s fence=%.2f convert=%.2f "
            "encode=%.2f pts=%lldms packets=%d [Features: %s] timing=cpu-wall-or-submit",
            encodeFrameCounter, stats.totalMs, slowLabel, stats.fenceWaitMs, stats.colorConvertMs, stats.encodeMs,
            stats.ptsMs, stats.packetsProduced, features.c_str());
    }

    // Periodic performance summary (about once per second at the configured FPS)
    if (encodeFrameCounter % fpsLogIntervalFrames == 0) {
        double avgFence = g_totalFenceWait / g_framesEncoded;
        double avgConvert = g_totalColorConvert / g_framesEncoded;
        double avgEncode = g_totalEncode / g_framesEncoded;
        double avgTotal = avgFence + avgConvert + avgEncode;

        // Identify bottleneck
        const char* bottleneck = "ENCODE";
        double maxTime = avgEncode;
        if (avgFence > maxTime) {
            bottleneck = "FENCE_WAIT";
            maxTime = avgFence;
        }
        if (avgConvert > maxTime) {
            bottleneck = "COLOR_CONV";
            maxTime = avgConvert;
        }

        DLL_Log(
            "[PERF SUMMARY] Frames=%lld Avg: total=%.2fms fence=%.2f "
            "convert=%.2f "
            "encode=%.2f | Max=%.2fms SlowFrames=%d | Bottleneck=%s | timing=cpu-wall-or-submit",
            g_framesEncoded, avgTotal, avgFence, avgConvert, avgEncode, g_maxFrameTime, g_slowFrameCount, bottleneck);

        // Frame timing analysis for smoothness
        if (stats.actualPtsDiff > 0) {
            const double jitter = static_cast<double>(stats.actualPtsDiff - stats.expectedPtsDiff);
            DLL_Log("[SMOOTHNESS] Expected=%0.2fms Actual=%0.2fms Jitter=%0.2fms",
                    static_cast<double>(stats.expectedPtsDiff), static_cast<double>(stats.actualPtsDiff), jitter);
        }
    }

    av_frame_free(&d3d11Frame);  // Releases D3D11 Tex

    return true;
}

// EncodeFrameD3D11: Direct D3D11 texture encoding for framegrab
// mode Zero-copy path - texture is converted RGB/BGRA -> NV12/P010 directly on GPU
bool VideoEncoder::PrepareFrameD3D11(ID3D11Texture2D* bgraTexture, uint32_t frameWidth, uint32_t frameHeight,
                                     bool isHDR) {
    if (!recordingRequested)
        return false;

    ReleaseInjectDeviceStateForScreenGrab();
