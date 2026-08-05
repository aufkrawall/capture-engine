#include "audio_capture_internal.h"

void AudioCapture::CaptureLoop() {
    workerThreadId_ = GetCurrentThreadId();
    const HRESULT coInitHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(coInitHr) && coInitHr != RPC_E_CHANGED_MODE) {
        DLL_Log("[AudioCapture] CaptureLoop CoInitializeEx failed: 0x%x", coInitHr);
        isCapturing.store(false, std::memory_order_release);
        CompleteStartup(false);
        workerThreadId_ = 0;
        return;
    }
    const bool coInitNeedsUninitialize = SUCCEEDED(coInitHr);

    bool startupSucceeded = false;
    try {
        HRESULT hr = CoCreateInstance(audio_capture_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, audio_capture_IID_IMMDeviceEnumerator,
                                      reinterpret_cast<void**>(&pEnumerator));
        if (FAILED(hr) || !pEnumerator) {
            DLL_Log("[AudioCapture] Worker CoCreateInstance(MMDeviceEnumerator) failed: 0x%x", hr);
        } else if (!isCapturing.load(std::memory_order_acquire)) {
            DLL_Log("[AudioCapture] Worker initialization cancelled before endpoint resolution");
        } else if (!ResolveCaptureDevice()) {
            DLL_Log("[AudioCapture] Worker could not resolve the requested endpoint");
        } else if (!isCapturing.load(std::memory_order_acquire)) {
            DLL_Log("[AudioCapture] Worker initialization cancelled before client activation");
        } else if (!ActivateAndStartClientOnDevice()) {
            DLL_Log("[AudioCapture] Worker could not activate/start the requested endpoint");
        } else if (!isCapturing.load(std::memory_order_acquire)) {
            DLL_Log("[AudioCapture] Worker initialization cancelled after client activation");
        } else {
            startupSucceeded = true;
        }
    } catch (const std::exception& error) {

        DLL_Log("[AudioCapture] Worker initialization threw an exception: %s", error.what());
    } catch (...) {
        DLL_Log("[AudioCapture] Worker initialization threw an unknown exception");
    }

    if (!startupSucceeded) {
        ReleaseAllInterfacesOnWorkerThread();
        workerThreadId_ = 0;
        isCapturing.store(false, std::memory_order_release);
        CompleteStartup(false);
        if (coInitNeedsUninitialize) {
            CoUninitialize();
        }
        return;
    }

    DLL_Log("[AudioCapture] CaptureLoop started (workerThread=%lu; all WASAPI interfaces worker-owned)",
            static_cast<unsigned long>(GetCurrentThreadId()));
    CompleteStartup(true);

    UINT32 packetLength = 0;
    HRESULT hr;
    BYTE* pData;
    UINT32 numFramesAvailable;
    DWORD flags;
    UINT64 devicePosition;

    UINT64 qpcPosition;

    // Debug: Drift tracking variables (non-static for per-session tracking)
    uint64_t firstDevicePos = 0;
    uint64_t firstQpcPos = 0;
    bool firstSet = false;
    int logCounter = 0;
    int errCount = 0;             // Count GetNextPacketSize errors (reset each session)
    int loopCount = 0;            // Count packets seen (reset each session)
    int qpcSanitizeLogCount = 0;  // Throttle out-of-domain QPC warnings (reset each session)

    // Cache QPC frequency once for converting the live performance counter into the
    // same 100-ns domain WASAPI reports its qpcPosition in, so we can validate it.
    LARGE_INTEGER qpcFreqLI = {};
    const uint64_t qpcFreq =
        QueryPerformanceFrequency(&qpcFreqLI) && qpcFreqLI.QuadPart > 0 ? static_cast<uint64_t>(qpcFreqLI.QuadPart) : 0;
    uint64_t queueDropPackets = 0;
    uint64_t queueDropFrames = 0;
    uint64_t lastQueueDropLogTick = 0;
    uint64_t finalDrainPackets = 0;
    uint64_t finalDrainFrames = 0;
    uint64_t finalDrainFrameBudget = 0;

    // --- Mid-recording stream recovery (endpoint device invalidation) ---
    const ce::audio::StreamRecoveryConfig recoveryCfg = recoveryConfig_;
    uint64_t lastReactivateTick = 0;
    uint64_t recoveryBackoffMs = 0;
    uint64_t reactivateAttempts = 0;
    uint64_t reactivateSuccesses = 0;
    int fatalErrLogCount = 0;
    int getBufferErrLogCount = 0;
    int emptyBufferLogCount = 0;
    int releaseBufferErrLogCount = 0;
    int invalidPacketLogCount = 0;
    int allocationFailureLogCount = 0;
    int discontinuityLogCount = 0;
    constexpr int kErrLogCap = 8;
    auto attemptReactivate = [&](const char* reason, long hrCode) -> bool {
        std::lock_guard<std::mutex> reactivationGate(reactivationMutex_);
        if (!isCapturing.load(std::memory_order_acquire)) {
            return false;
        }
        const uint64_t now = GetTickCount64();
        if (!ce::audio::RecoveryBackoffElapsed(now, lastReactivateTick, recoveryBackoffMs)) {
            return false;
        }
        ++reactivateAttempts;
        lastReactivateTick = now;
        recoveryBackoffMs = ce::audio::NextRecoveryBackoffMs(recoveryBackoffMs, recoveryCfg);
        DLL_Log("[AudioCapture] Re-activating %s stream (reason=%s hr=0x%lx attempt=%llu nextBackoffMs=%llu)",
                isLoopback_ ? "loopback" : "capture", reason, static_cast<unsigned long>(hrCode),
                static_cast<unsigned long long>(reactivateAttempts),
                static_cast<unsigned long long>(recoveryBackoffMs));
        bool ok = false;
        try {
            ok = ReactivateClient();
        } catch (const std::exception& error) {
            DLL_Log("[AudioCapture] Re-activation threw an exception: %s; partial client state will be cleared",
                    error.what());
            ReleaseActiveClientOnWorkerThread(true);
        } catch (...) {
            DLL_Log("[AudioCapture] Re-activation threw an unknown exception; partial client state will be cleared");
            ReleaseActiveClientOnWorkerThread(true);
        }
        if (ok) {
            ++reactivateSuccesses;
            firstSet = false;  // drift baseline restarts on the fresh client (devicePosition resets)
            DLL_Log("[AudioCapture] Re-activation succeeded (attempt=%llu mode=%s)",
                    static_cast<unsigned long long>(reactivateAttempts),
                    (activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 ? "event-driven" : "polling");
        } else if (isCapturing.load(std::memory_order_acquire)) {
            DLL_Log("[AudioCapture] Re-activation FAILED (attempt=%llu) - will retry with backoff",
                    static_cast<unsigned long long>(reactivateAttempts));
        } else {
            DLL_Log("[AudioCapture] Re-activation cancelled by stop (attempt=%llu)",
                    static_cast<unsigned long long>(reactivateAttempts));
        }
        return ok;
    };

    auto readNextPacketSize = [&](const char* context, bool allowRecovery) -> bool {
        packetLength = 0;
        const HRESULT packetHr = pCaptureClient->GetNextPacketSize(&packetLength);
        if (SUCCEEDED(packetHr)) {
            return true;
        }
        if (ce::audio::IsFatalWasapiStreamError(packetHr)) {
            if (fatalErrLogCount++ < kErrLogCap) {
                DLL_Log("[AudioCapture] FATAL stream error from GetNextPacketSize (%s): 0x%lx (loopback=%d%s)", context,
                        static_cast<unsigned long>(packetHr), isLoopback_ ? 1 : 0,
                        allowRecovery ? "; attempting re-activation" : "; final drain will stop");
            }
            if (allowRecovery) {
                attemptReactivate("GetNextPacketSize_fatal", packetHr);
            } else {
                DLL_Log("[AudioCapture] Final drain stopped by fatal GetNextPacketSize error: 0x%lx",
                        static_cast<unsigned long>(packetHr));
            }
        } else if (errCount++ < kErrLogCap) {
            DLL_Log("[AudioCapture] GetNextPacketSize failed (%s): 0x%lx", context,
                    static_cast<unsigned long>(packetHr));
        }
        return false;
    };

    bool finalDrainPassDone = false;
    while (true) {
        const bool stopRequestedBeforeWait = !isCapturing.load(std::memory_order_acquire);
        if (stopRequestedBeforeWait) {
            if (finalDrainPassDone) {
                break;
            }
            // Exactly one final pass starts without waiting. It drains every
            // packet currently announced by WASAPI and never reactivates a
            // device while Stop() is joining this worker.
            finalDrainPassDone = true;
        } else if ((activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 && captureEvent_) {
            const DWORD waitResult = WaitForSingleObject(captureEvent_, 200);
            if (waitResult == WAIT_FAILED) {
                DLL_Log("[AudioCapture] WaitForSingleObject failed: 0x%lx", GetLastError());
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        } else if (captureEvent_) {
            // In polling mode the same event is still a prompt stop wake-up;
            // it simply is not registered as the WASAPI notification handle.
            WaitForSingleObject(captureEvent_, 10);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const bool drainingAfterStop = !isCapturing.load(std::memory_order_acquire);
        if (drainingAfterStop) {
            finalDrainPassDone = true;
            if (finalDrainFrameBudget == 0) {
                const uint32_t fallbackFrames =
                    ce::audio::MaxWasapiCapturePacketFrames(pwfx ? pwfx->nSamplesPerSec : 0);
                finalDrainFrameBudget = bufferFrameCount_ != 0 ? bufferFrameCount_ : fallbackFrames;
                if (finalDrainFrameBudget == 0) {
                    finalDrainFrameBudget = 1;
                }
            }
        }

        // A previous re-activation may have failed and left no client; recover it
        // here (with backoff) before any client call so we never deref nullptr.
        if (!pCaptureClient) {
            if (drainingAfterStop) {
                break;
            }
            attemptReactivate("client_null", 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        if (!readNextPacketSize(drainingAfterStop ? "final drain" : "outer", !drainingAfterStop)) {
            if (drainingAfterStop) {
                break;
            }
            continue;
        }

        if (loopCount++ < 3 && packetLength > 0) {
            DLL_Log("[AudioCapture] Got packetLength=%u", packetLength);
        }

        while (packetLength != 0) {
            if (drainingAfterStop && finalDrainFrames >= finalDrainFrameBudget) {
                DLL_Log(
                    "[AudioCapture] Final drain reached the endpoint-buffer bound (%llu frame(s)); leaving any "
                    "newly-arrived data for stream teardown",
                    static_cast<unsigned long long>(finalDrainFrameBudget));
                packetLength = 0;
                break;
            }
            hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, &devicePosition, &qpcPosition);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) {
                // This success status means no packet was acquired, so calling
                // ReleaseBuffer would itself be an out-of-order WASAPI error.
                if (emptyBufferLogCount++ < kErrLogCap) {
                    DLL_Log(
                        "[AudioCapture] GetBuffer returned AUDCLNT_S_BUFFER_EMPTY after announcing %u frame(s); "
                        "waiting for the next capture notification",
                        packetLength);
                }
                packetLength = 0;
                break;
            }
            if (FAILED(hr)) {
                if (ce::audio::IsFatalWasapiStreamError(hr)) {
                    if (fatalErrLogCount++ < kErrLogCap) {
                        DLL_Log("[AudioCapture] FATAL stream error from GetBuffer: 0x%lx (loopback=%d%s)",
                                static_cast<unsigned long>(hr), isLoopback_ ? 1 : 0,
                                drainingAfterStop ? "; final drain will stop" : "; attempting re-activation");
                    }
                    if (!drainingAfterStop) {
                        attemptReactivate("GetBuffer_fatal", hr);
                    }
                } else if (getBufferErrLogCount++ < kErrLogCap) {
                    DLL_Log("[AudioCapture] GetBuffer failed: 0x%lx", static_cast<unsigned long>(hr));
                }
                break;
            }
            recoveryBackoffMs = 0;  // healthy delivery resets backoff for responsive future recovery
            if (drainingAfterStop) {
                ++finalDrainPackets;
                finalDrainFrames += numFramesAvailable;
            }

            // WASAPI driver timestamp sanity guard. The qpcPosition returned by
            // GetBuffer is device/driver-provided and not always trustworthy: some
            // endpoints (observed on a 192 kHz loopback device) report a garbage
            // first-packet position hundreds of days in the future. Used verbatim,
            // that bogus value makes the media-engine timeline math request an
            // unbounded leading-silence allocation (multi-TB std::vector ->
            // std::bad_alloc -> std::terminate). Validate against the live QPC and
            // substitute it for out-of-domain values so the timeline stays in-domain
            // on every device; healthy positions pass through bit-identical. The raw
            // value is still preserved on the packet for diagnostics below.
            const uint64_t rawQpcPosition = qpcPosition;
            {
                LARGE_INTEGER nowQpcLI = {};
                const uint64_t nowQpc100ns =
                    qpcFreq != 0 && QueryPerformanceCounter(&nowQpcLI) && nowQpcLI.QuadPart >= 0
                        ? ce::audio::RawQpcToHundredNanoseconds(static_cast<uint64_t>(nowQpcLI.QuadPart), qpcFreq)
                        : 0;
                const uint64_t sanitizedQpc = ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0 && nowQpc100ns != 0)
                                                  ? nowQpc100ns
                                                  : ce::audio::SanitizeCaptureQpcPosition(qpcPosition, nowQpc100ns);
                if (sanitizedQpc != qpcPosition) {
                    if (qpcSanitizeLogCount++ < 8) {
                        DLL_Log(
                            "[AudioCapture] WARNING: out-of-domain WASAPI qpcPosition=%llu substituted with "
                            "nowQpc=%llu (devPos=%llu rate=%u loopback=%d) - driver reported an invalid capture "
                            "timestamp%s",
                            (unsigned long long)qpcPosition, (unsigned long long)nowQpc100ns,
                            (unsigned long long)devicePosition, pwfx ? pwfx->nSamplesPerSec : 0, isLoopback_ ? 1 : 0,
                            (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0 ? " (TIMESTAMP_ERROR)" : "");
                    }
                    qpcPosition = sanitizedQpc;
                }
            }

            size_t bytes = 0;
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            const bool frameCountValid = ce::audio::IsWasapiCapturePacketFrameCountValid(
                packetLength, numFramesAvailable, bufferFrameCount_, pwfx ? pwfx->nSamplesPerSec : 0);
            const bool byteSizeValid =
                pwfx && ce::audio::TryComputeAudioPacketByteSize(numFramesAvailable, pwfx->nBlockAlign, &bytes);
            if (!frameCountValid || !byteSizeValid || (!silent && !pData)) {
                if (invalidPacketLogCount++ < kErrLogCap) {
                    DLL_Log(
                        "[AudioCapture] WARNING: rejecting malformed WASAPI packet: announced=%u actual=%u "
                        "bufferFrames=%u sampleRate=%u maxPacketFrames=%u blockAlign=%u data=%p flags=0x%lx "
                        "loopback=%d",
                        packetLength, numFramesAvailable, bufferFrameCount_, pwfx ? pwfx->nSamplesPerSec : 0,
                        ce::audio::MaxWasapiCapturePacketFrames(pwfx ? pwfx->nSamplesPerSec : 0),
                        pwfx ? pwfx->nBlockAlign : 0, pData, flags, isLoopback_ ? 1 : 0);
                }
                const HRESULT releaseHr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
                if (FAILED(releaseHr)) {
                    if (releaseBufferErrLogCount++ < kErrLogCap) {
                        DLL_Log("[AudioCapture] ReleaseBuffer failed after malformed packet: 0x%lx",
                                static_cast<unsigned long>(releaseHr));
                    }
                    if (!drainingAfterStop) {
                        attemptReactivate("ReleaseBuffer_malformed", releaseHr);
                    }
                    packetLength = 0;
                    break;
                }
                if (!readNextPacketSize("after malformed packet", !drainingAfterStop)) {
                    break;
                }
                continue;
            }
            if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0 && discontinuityLogCount < kErrLogCap) {
                ++discontinuityLogCount;
                DLL_Log(
                    "[AudioCapture] WASAPI data discontinuity: frames=%u devPos=%llu qpc=%llu loopback=%d "
                    "(occurrence=%d)",
                    numFramesAvailable, static_cast<unsigned long long>(devicePosition),
                    static_cast<unsigned long long>(qpcPosition), isLoopback_ ? 1 : 0, discontinuityLogCount);
            }

            // Debug: Check drift between Device Position (samples) and QPC time.
            // WASAPI already converts qpcPosition to 100-ns units.
            // devicePosition is cumulative frame count
            // qpcPosition is the timestamp of that position in 100-ns units.

            if (!firstSet && devicePosition > 0) {
                firstDevicePos = devicePosition;
                firstQpcPos = qpcPosition;
                firstSet = true;
                // streamLatency is telemetry only; placement uses the raw QPC (the A/V offset is
                // corrected by delaying video content, not by advancing audio). wouldAdvanceQpc
                // shows what the retired GetStreamLatency audio-advance would have produced.
                const uint64_t wouldAdvanceQpc =
                    ce::audio::ApplyCaptureLatencyCompensation(firstQpcPos, streamLatency100ns_, isLoopback_);
                DLL_Log(
                    "[AudioCapture] Source Sync Start: DevPos=%llu QPC=%llu (placed raw; "
                    "wouldAdvanceQpc=%llu streamLatency=%lluus, not applied)",
                    firstDevicePos, firstQpcPos, wouldAdvanceQpc,
                    static_cast<unsigned long long>(streamLatency100ns_ / 10));
            } else if (firstSet && logCounter++ % 500 == 0) {  // Log every ~5 seconds
                double samplesDuration = (double)(devicePosition - firstDevicePos) / pwfx->nSamplesPerSec;
                double qpcDuration = ce::audio::HundredNanosecondsToSeconds(qpcPosition - firstQpcPos);
                double driftMs = (samplesDuration - qpcDuration) * 1000.0;

                DLL_Log(
                    "[AudioCapture] Source Sync: Duration Samples=%.4fs, "
                    "QPC=%.4fs, Drift=%.2f ms (%.4f%%)",
                    samplesDuration, qpcDuration, driftMs, (driftMs / (qpcDuration * 1000.0) * 100.0));
            }

            // Build packet with format info
            AudioPacket packet{};
            audio_capture_FillPacketFormatFromWaveFormat(pwfx, &packet);
            packet.captureEpoch = captureEpoch_.load(std::memory_order_acquire);
            packet.devicePosition = devicePosition;      // Store for debugging if needed
            packet.rawQpcPosition = rawQpcPosition;      // Store unmodified WASAPI timestamp for debugging
            packet.streamLatency = streamLatency100ns_;  // telemetry only (see below)
            // A/V capture latency is corrected by DELAYING video content (and equalizing faster
            // audio sources up to it), never by advancing live audio: the earlier samples do not
            // exist, so advancing only manufactures a live-edge deficit the CFR pipeline pads and
            // the encoded cursor re-pins (the shift is absorbed, not corrected). The render->loopback
            // offset is auto-measured/configured (audio_capture_latency_ms) and routed entirely
            // through the video content delay. So place the packet at the raw WASAPI QPC; do NOT
            // subtract streamLatency here (it double-counts with the video delay on devices that
            // report a nonzero GetStreamLatency, and on HDMI/AVR it is 0 anyway).
            packet.qpcPosition = qpcPosition;

            packet.timestamp = (int64_t)ce::audio::HundredNanosecondsToMilliseconds(packet.qpcPosition);

            // Copy data - or generate silence if silent flag is set (critical for A/V
            // sync!)
            try {
                packet.data.resize(bytes);
            } catch (const std::exception& error) {
                if (allocationFailureLogCount++ < kErrLogCap) {
                    DLL_Log(
                        "[AudioCapture] WARNING: packet allocation failed for %zu byte(s) / %u frame(s): %s; "
                        "dropping this packet without terminating capture",
                        bytes, numFramesAvailable, error.what());
                }
                const HRESULT releaseHr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
                if (FAILED(releaseHr)) {
                    if (releaseBufferErrLogCount++ < kErrLogCap) {
                        DLL_Log("[AudioCapture] ReleaseBuffer failed after packet allocation error: 0x%lx",
                                static_cast<unsigned long>(releaseHr));
                    }
                    if (!drainingAfterStop) {
                        attemptReactivate("ReleaseBuffer_allocation_failed", releaseHr);
                    }
                    packetLength = 0;
                    break;
                }
                if (!readNextPacketSize("after packet allocation failure", !drainingAfterStop)) {
                    break;
                }
                continue;
            }

            if (silent) {
                // Generate silence - DO NOT SKIP! Dropping silent packets causes
                // timeline compression
                memset(packet.data.data(), 0, bytes);
            } else {
                memcpy(packet.data.data(), pData, bytes);
            }

            bool logQueueDrop = false;
            uint64_t droppedPacketsToLog = 0;
            uint64_t droppedFramesToLog = 0;
            try {
                std::lock_guard<std::mutex> lock(queueMutex);
                // Append first so allocation failure preserves every packet
                // already queued. Only after the new packet is safely owned do
                // we trim the oldest one to retain the live edge.
                packetQueue.emplace_back(std::move(packet));
                if (packetQueue.size() > kMaxQueuedPackets) {
                    auto droppedIt = std::find_if(packetQueue.begin(), packetQueue.end(), [](const auto& queued) {
                        return queued.recordType == AudioPacketRecordType::Data;
                    });
                    if (droppedIt == packetQueue.end()) {
                        --droppedIt;  // The packet just appended above is always a data record.
                    }
                    const AudioPacket& droppedPacket = *droppedIt;
                    if (droppedPacket.blockAlign > 0) {
                        queueDropFrames += droppedPacket.data.size() / static_cast<size_t>(droppedPacket.blockAlign);
                    }
                    queueDropPackets++;
                    packetQueue.erase(droppedIt);

                    const uint64_t nowTick = GetTickCount64();
                    if (nowTick - lastQueueDropLogTick >= 1000) {
                        logQueueDrop = true;
                        droppedPacketsToLog = queueDropPackets;
                        droppedFramesToLog = queueDropFrames;
                        queueDropPackets = 0;
                        queueDropFrames = 0;
                        lastQueueDropLogTick = nowTick;
                    }
                }
            } catch (const std::exception& error) {
                if (allocationFailureLogCount++ < kErrLogCap) {
                    DLL_Log("[AudioCapture] WARNING: capture queue insertion failed: %s; dropping packet safely",
                            error.what());
                }
            }
            if (logQueueDrop) {
                DLL_Log(
                    "[AudioCapture] WARNING: capture queue overrun - dropped %llu packet(s) / %llu frame(s) "
                    "while keeping newest audio (depth=%zu)",
                    static_cast<unsigned long long>(droppedPacketsToLog),
                    static_cast<unsigned long long>(droppedFramesToLog), kMaxQueuedPackets);
            }

            const HRESULT releaseHr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
            if (FAILED(releaseHr)) {
                if (releaseBufferErrLogCount++ < kErrLogCap) {
                    DLL_Log("[AudioCapture] ReleaseBuffer failed: 0x%lx%s", static_cast<unsigned long>(releaseHr),
                            drainingAfterStop ? " during final drain" : " - re-activating stream");
                }
                if (!drainingAfterStop) {
                    attemptReactivate("ReleaseBuffer_failed", releaseHr);
                }
                packetLength = 0;
                break;
            }
            if (!readNextPacketSize(drainingAfterStop ? "final inner drain" : "inner drain", !drainingAfterStop)) {
                break;
            }
        }

        if (drainingAfterStop) {
            break;
        }
    }

    DLL_Log("[AudioCapture] CaptureLoop exited; releasing worker-owned WASAPI interfaces on thread %lu",
            static_cast<unsigned long>(GetCurrentThreadId()));
    if (reactivateAttempts > 0) {
        DLL_Log("[AudioCapture] Stream recovery summary (loopback=%d): %llu re-activation attempt(s), %llu succeeded",
                isLoopback_ ? 1 : 0, static_cast<unsigned long long>(reactivateAttempts),
                static_cast<unsigned long long>(reactivateSuccesses));
    }
    if (finalDrainPackets > 0 || finalDrainFrames > 0) {
        DLL_Log("[AudioCapture] Final stop drain inspected %llu packet(s) / %llu frame(s)",
                static_cast<unsigned long long>(finalDrainPackets), static_cast<unsigned long long>(finalDrainFrames));
    }
    if (queueDropPackets > 0 || queueDropFrames > 0) {
        DLL_Log("[AudioCapture] Final queue overrun summary: dropped %llu packet(s) / %llu frame(s)",
                static_cast<unsigned long long>(queueDropPackets), static_cast<unsigned long long>(queueDropFrames));
    }
    ReleaseAllInterfacesOnWorkerThread();
    workerThreadId_ = 0;
    // Every successful CoInitializeEx call, including S_FALSE, must be balanced
    // on this same worker after every COM interface has been released.
    if (coInitNeedsUninitialize) {
        CoUninitialize();
    }
}

bool AudioCapture::GetNextPacket(AudioPacket& packet) {
    AudioPacket queuedPacket;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (packetQueue.empty())
            return false;
        queuedPacket = std::move(packetQueue.front());
        packetQueue.pop_front();  // O(1) for deque vs O(n) for vector
    }
    // Destroy/replace any storage already held by the caller outside the producer
    // queue lock so capture is never delayed by an allocator free.
    packet = std::move(queuedPacket);
    return true;
}

void AudioCapture::DiscardPendingPackets() {
    std::deque<AudioPacket> discarded;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        discarded.swap(packetQueue);
    }
    if (!discarded.empty()) {
        DLL_Log("[AudioCapture] Discarding %zu queued packets", discarded.size());
    }
}

size_t AudioCapture::PendingPacketCount() {
    std::lock_guard<std::mutex> lock(queueMutex);
    return packetQueue.size();
}
