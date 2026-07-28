        if (asyncOp) {
            asyncOp->Release();
        }
        return false;
    }

    // Wait for activation to complete, but let Stop wake the asynchronous start
    // immediately through a dedicated manual-reset cancellation event. Do not use
    // captureEvent_ here: abandoned process-loopback clients can continue signaling
    // their old capture callback event during a recovery activation.
    HANDLE waitHandles[] = {handler->GetEvent(), stopEvent_};
    const DWORD waitHandleCount = stopEvent_ ? 2 : 1;
    const DWORD waitResult = WaitForMultipleObjects(waitHandleCount, waitHandles, FALSE, 5000);
    if (waitHandleCount == 2 && waitResult == WAIT_OBJECT_0 + 1) {
        DLL_Log("[AppAudioCapture] Activation cancelled for PID %lu during shutdown", pid);
        handler->Release();
        if (asyncOp) {
            asyncOp->Release();
        }
        return false;
    }
    if (waitResult != WAIT_OBJECT_0) {
        DLL_Log("[AppAudioCapture] Activation wait failed for PID %lu: result=0x%lx error=0x%lx", pid, waitResult,
                waitResult == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
        handler->Release();
        if (asyncOp) {
            asyncOp->Release();
        }
        return false;
    }

    // Get the result
    hr = handler->GetResult();
    if (FAILED(hr)) {
        DLL_Log("[AppAudioCapture] Activation failed: 0x%x", hr);
        handler->Release();
        if (asyncOp) {
            asyncOp->Release();
        }
        return false;
    }

    *audioClient = handler->TakeAudioClient();
    handler->Release();
    if (asyncOp) {
        asyncOp->Release();
    }

    if (!*audioClient) {
        DLL_Log("[AppAudioCapture] No audio client obtained");
        return false;
    }
    return true;
}

bool AppAudioCapture::ActivateClientForPID(DWORD pid, bool allowEventDriven) {
    const HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coInitHr) && coInitHr != RPC_E_CHANGED_MODE) {
        DLL_Log("[AppAudioCapture] CoInitializeEx failed: 0x%x", coInitHr);
        return false;
    }
    // S_FALSE is a successful call and increments COM's per-thread init count.
    const bool coInitNeedsUninitialize = SUCCEEDED(coInitHr);
    CE_SCOPE_EXIT(if (coInitNeedsUninitialize) { CoUninitialize(); });

    // Process loopback does not reliably expose the app's native mix format, so
    // request the resolved output layout and let AUTOCONVERTPCM do only the
    // unavoidable source conversion.

    // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
    const GUID kKsDataFormatSubtypeIeeeFloat = {
        0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

    struct ClientAttempt {
        DWORD streamFlags;
        REFERENCE_TIME bufferDuration;
        bool requiresEvent;
        const char* description;
    };
    const ClientAttempt attempts[] = {
        {AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, 2000000, false,
         "polling loopback/autoconvert"},
        {AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, 100000,
         true, "event-driven loopback/autoconvert"},
        {0, 2000000, false, "polling plain"},
    };

    HRESULT hr = E_FAIL;
    bool initialized = false;
    for (const ClientAttempt& attempt : attempts) {
        if (shouldStop.load(std::memory_order_acquire)) {
            DLL_Log("[AppAudioCapture] Activation cancelled for PID %lu during shutdown", pid);
            return false;
        }
        if (attempt.requiresEvent && (!allowEventDriven || !captureEvent_)) {
            continue;
        }

        IAudioClient* activatedClient = nullptr;
        // Atomically record the notification boundary and the render-session
        // processes already known at that boundary. Windows can emit silent
        // placeholder packets even when process loopback was activated before
        // the target's first session existed, so packet arrival alone cannot
        // prove that this client attached to the target session.
        const auto processTree = SnapshotProcessTree();
        std::array<DWORD, 1024> observedSessionProcessIds{};
        size_t observedSessionProcessCount = 0;
        const uint64_t activationGeneration = audioSessionMonitor_.SnapshotGenerationAndObservedProcessIds(
            observedSessionProcessIds.data(), observedSessionProcessIds.size(), &observedSessionProcessCount);
        const bool hadObservedTargetSession =
            std::any_of(observedSessionProcessIds.begin(),
                        observedSessionProcessIds.begin() + observedSessionProcessCount, [&](DWORD sessionProcessId) {
                            return ce::process_loopback::ProcessBelongsToTree(processTree, sessionProcessId, pid);
                        });
        activationHadObservedTargetSession_.store(hadObservedTargetSession, std::memory_order_release);
        activationAudioSessionGeneration_.store(activationGeneration, std::memory_order_release);
        DLL_Log(
            "[AppAudioCapture] Process-loopback activation boundary: PID=%lu generation=%llu "
            "observedSessionProcesses=%zu targetSessionObserved=%d mode=%s",
            pid, static_cast<unsigned long long>(activationGeneration), observedSessionProcessCount,
            hadObservedTargetSession ? 1 : 0, attempt.description);
        if (!ActivateAudioInterfaceForPID(pid, &activatedClient)) {
            // Activation is independent of the event/polling flags used later by
            // IAudioClient::Initialize. Repeating a five-second activation timeout
            // once per mode only stalls capture and shutdown for up to 15 seconds.
            DLL_Log(
                "[AppAudioCapture] Audio-interface activation failed for %s; capture-mode fallbacks were not "
                "reached",
                attempt.description);
            return false;
        }
        pAudioClient = activatedClient;
        if (shouldStop.load(std::memory_order_acquire)) {
            DLL_Log("[AppAudioCapture] Activation completed for PID %lu after shutdown began; abandoning client", pid);
            AbandonClientInterfaces();
            return false;
        }

        pwfx = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
        if (!pwfx) {
            DLL_Log("[AppAudioCapture] Failed to allocate capture format");
            AbandonClientInterfaces();
            return false;
        }
        std::memset(pwfx, 0, sizeof(WAVEFORMATEXTENSIBLE));
        auto* wfex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx);
        wfex->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wfex->Format.nChannels = static_cast<WORD>(std::clamp(requestedChannels, 1, 8));
        wfex->Format.nSamplesPerSec = static_cast<DWORD>(requestedSampleRate > 0 ? requestedSampleRate : 48000);
        wfex->Format.wBitsPerSample = 32;
        wfex->Format.nBlockAlign = wfex->Format.nChannels * wfex->Format.wBitsPerSample / 8;
        const uint64_t avgBytesPerSec = static_cast<uint64_t>(wfex->Format.nSamplesPerSec) * wfex->Format.nBlockAlign;
        if (avgBytesPerSec > std::numeric_limits<DWORD>::max()) {
            DLL_Log("[AppAudioCapture] Requested format byte rate is not representable: rate=%lu blockAlign=%u",
                    wfex->Format.nSamplesPerSec, wfex->Format.nBlockAlign);
            AbandonClientInterfaces();
            return false;
        }
        wfex->Format.nAvgBytesPerSec = static_cast<DWORD>(avgBytesPerSec);
        wfex->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wfex->Samples.wValidBitsPerSample = 32;
        wfex->dwChannelMask = requestedChannelMask;
        wfex->SubFormat = kKsDataFormatSubtypeIeeeFloat;

        hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, attempt.streamFlags, attempt.bufferDuration, 0, pwfx,
                                      nullptr);
        if (FAILED(hr)) {
            DLL_Log("[AppAudioCapture] Initialize failed for %s: 0x%x; trying next mode", attempt.description, hr);
            // IAudioClient cannot be safely initialized again. Obtain a fresh
            // process-loopback interface for every fallback attempt.
            AbandonClientInterfaces();
            continue;
        }
        activeStreamFlags = attempt.streamFlags;

        if (attempt.requiresEvent) {
            const BOOL resetOk = ResetEvent(captureEvent_);
            hr = resetOk ? pAudioClient->SetEventHandle(captureEvent_) : HRESULT_FROM_WIN32(GetLastError());
            if (FAILED(hr)) {
                DLL_Log(
                    "[AppAudioCapture] SetEventHandle failed for initialized event client: 0x%x; "
                    "re-activating in polling mode",
                    hr);
                // Clearing activeStreamFlags alone is incorrect: the initialized
                // client still requires an event and Start would fail or never wake.
                AbandonClientInterfaces();
                continue;
            }
        }

        initialized = true;
        break;
    }

    if (!initialized || !pAudioClient || !pwfx) {
        DLL_Log("[AppAudioCapture] All process-loopback initialization modes failed for PID %lu", pid);
        AbandonClientInterfaces();
        return false;
    }

    REFERENCE_TIME streamLatency = 0;
    hr = pAudioClient->GetStreamLatency(&streamLatency);
    if (SUCCEEDED(hr)) {
        streamLatency100ns = static_cast<uint64_t>(std::max<REFERENCE_TIME>(0, streamLatency));
    } else if (hr == E_NOTIMPL) {
        DLL_Log("[AppAudioCapture] GetStreamLatency is not implemented for this process-loopback client; "
                "automatic render-endpoint latency probing remains authoritative");
        streamLatency100ns = 0;
    } else {
        DLL_Log("[AppAudioCapture] GetStreamLatency unavailable: 0x%x; automatic render-endpoint latency probing "
                "remains authoritative",
                hr);
        streamLatency100ns = 0;
    }
    REFERENCE_TIME defaultPeriod = 0;
    REFERENCE_TIME minPeriod = 0;
    if (SUCCEEDED(pAudioClient->GetDevicePeriod(&defaultPeriod, &minPeriod))) {
        defaultDevicePeriod100ns = static_cast<uint64_t>(std::max<REFERENCE_TIME>(0, defaultPeriod));
        minDevicePeriod100ns = static_cast<uint64_t>(std::max<REFERENCE_TIME>(0, minPeriod));
    } else {
        defaultDevicePeriod100ns = 0;
        minDevicePeriod100ns = 0;
    }
    UINT32 bufferFrames = 0;
    bufferFrameCount = SUCCEEDED(pAudioClient->GetBufferSize(&bufferFrames)) ? bufferFrames : 0;

    // Get capture client
    hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&pCaptureClient));
    if (FAILED(hr) || !pCaptureClient) {
        DLL_Log("[AppAudioCapture] GetService IAudioCaptureClient failed: 0x%x", hr);
        CleanupCapture();
        return false;
    }

    // Start the audio client
    hr = pAudioClient->Start();
    if (FAILED(hr)) {
        DLL_Log("[AppAudioCapture] Start failed: 0x%x", hr);
        CleanupCapture();
        return false;
    }
    const uint64_t activatedEpoch = captureEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (!QueueCaptureEpochMarker(AudioPacketRecordType::EpochStart, activatedEpoch, "audio client started")) {
        DLL_Log("[AppAudioCapture] Failed to publish epoch start for PID %lu; abandoning the unpublishable client",
                pid);
        CleanupCapture();
        return false;
    }

    const uint64_t bufferDurationUs =
        (pwfx->nSamplesPerSec > 0)
            ? (static_cast<uint64_t>(bufferFrameCount) * 1000000ull) / static_cast<uint64_t>(pwfx->nSamplesPerSec)
            : 0;
    DLL_Log(
        "[AppAudioCapture] Started: PID=%lu epoch=%llu channels=%d rate=%d bits=%d streamLatency=%lluus "
        "devicePeriod=%lluus minPeriod=%lluus bufferFrames=%u bufferDur=%lluus "
        "(latency routed via video content delay, not audio advance)",
        pid, static_cast<unsigned long long>(activatedEpoch), pwfx->nChannels, pwfx->nSamplesPerSec,
        pwfx->wBitsPerSample, static_cast<unsigned long long>(streamLatency100ns / 10),
        static_cast<unsigned long long>(defaultDevicePeriod100ns / 10),
        static_cast<unsigned long long>(minDevicePeriod100ns / 10), bufferFrameCount,
        static_cast<unsigned long long>(bufferDurationUs));

    DLL_Log("[AppAudioCapture] Capture mode contract: selected=%s preference=polling-first eventFallbackAllowed=%d",
            (activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 ? "event-driven" : "polling",
            allowEventDriven ? 1 : 0);

    return true;
}

void AppAudioCapture::AbandonClientInterfaces() {
    // Process loopback is backed by AudioSes' CLoopbackMixer. On current Windows 11
    // builds the mixer can crash in AudioLimiterAPO cleanup when the process-loopback
    // COM interfaces are released, especially with duplicate process-loopback
    // captures. This object runs in a disposable CaptureEngine worker, so leave
    // these OS-owned interfaces for process teardown instead of touching the
    // crash-prone AudioSes cleanup path. A re-activation emits a new epoch; the
    // worker then exits for immediate recycling, bounding abandoned wrappers to
    // one worker generation even during a long recording.
    if (pCaptureClient || pAudioClient) {
        DLL_Log(
            "[AppAudioCapture] Abandoning process-loopback COM interfaces "
            "(audioClient=%p captureClient=%p flags=0x%lx) to avoid AudioSes CLoopbackMixer teardown crash",
            pAudioClient, pCaptureClient, activeStreamFlags);
        pCaptureClient = nullptr;
        pAudioClient = nullptr;
    }

    if (pwfx) {
        CoTaskMemFree(pwfx);
        pwfx = nullptr;
    }

    activeStreamFlags = 0;
    streamLatency100ns = 0;
    defaultDevicePeriod100ns = 0;
    minDevicePeriod100ns = 0;
    bufferFrameCount = 0;
}

void AppAudioCapture::CleanupCapture() {
    if (captureEvent_) {
        ResetEvent(captureEvent_);
    }
    AbandonClientInterfaces();
}

void AppAudioCapture::CaptureLoop() {
    const HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coInitHr) && coInitHr != RPC_E_CHANGED_MODE) {
        DLL_Log("[AppAudioCapture] CaptureLoop CoInitializeEx failed for PID %lu: 0x%x", targetPID.load(), coInitHr);
        isCapturing.store(false, std::memory_order_release);
        startPendingValid.store(false, std::memory_order_release);
        return;
    }
    DLL_Log("[AppAudioCapture] Capture loop started for PID %lu", targetPID.load());

    UINT32 packetLength = 0;
    HRESULT hr;
    BYTE* pData;
    UINT32 numFramesAvailable;
    DWORD flags;
    UINT64 devicePosition;

    UINT64 qpcPosition;

    // Debug: Track packet-timeline drift using devicePosition when available,
    // otherwise fall back to the cumulative captured frame count.
    uint64_t firstLogicalFramePos = 0;
    uint64_t firstQpcPos = 0;
    bool firstSet = false;
    int logCounter = 0;
    uint64_t logicalFrameCursor = 0;
    // Content telemetry (diagnostics): per-window silent-flag / amplitude tracking so we can
    // tell whether process loopback keeps delivering REAL audio or goes (and stays) silent
    // while the stream stays alive. Reset each Source Sync log window (~5 s).
    uint64_t windowTotalFrames = 0;
    uint64_t windowSilentFlagFrames = 0;
    uint64_t windowZeroContentFrames = 0;
    float windowPeakAbs = 0.0f;
    uint64_t lastProcessCheckTick = 0;
    uint64_t queueDropPackets = 0;
    uint64_t queueDropFrames = 0;
    uint64_t lastQueueDropLogTick = 0;
    uint64_t finalDrainPackets = 0;
    uint64_t finalDrainFrames = 0;
    uint64_t finalDrainFrameBudget = 0;
    LARGE_INTEGER qpcFreqLI = {};
    const uint64_t qpcFreq =
        QueryPerformanceFrequency(&qpcFreqLI) && qpcFreqLI.QuadPart > 0 ? static_cast<uint64_t>(qpcFreqLI.QuadPart) : 0;
    int qpcSanitizeLogCount = 0;

    // --- Mid-recording stream recovery state (device-invalidation + silent stall) ---
    const ce::audio::StreamRecoveryConfig recoveryCfg = recoveryConfig_;
    uint64_t lastPacketTick = GetTickCount64();  // arms the silent-stall window from stream start
    uint64_t lastNoPacketDiagnosticTick = 0;
    uint64_t lastReactivateTick = 0;
    uint64_t recoveryBackoffMs = 0;
    bool sawAnyPacket = false;
    bool currentActivationQualified = false;
    uint64_t currentActivationStartTick = lastPacketTick;
    uint64_t qualifiedActivationCount = 0;
    uint64_t eventFallbackAttempts = 0;
    uint64_t eventFallbackSuccesses = 0;
    uint64_t reactivateAttempts = 0;
    uint64_t reactivateSuccesses = 0;
    bool captureEpochOpen = true;
    // Per-session throttled error logging (NOT static: must reset every session, or
    // a long-lived process silently swallows every error after the first session).
    int fatalErrLogCount = 0;
    int transientErrLogCount = 0;
    int getBufferErrLogCount = 0;
    int emptyBufferLogCount = 0;
    int releaseBufferErrLogCount = 0;
    int invalidPacketLogCount = 0;
    int allocationFailureLogCount = 0;
    int discontinuityLogCount = 0;
    constexpr int kErrLogCap = 8;
    // Require a few consecutive misses before declaring the process gone, so a
    // transient OpenProcess hiccup cannot permanently kill an otherwise-live capture.
    int processMissingStreak = 0;
    constexpr int kProcessMissingStreakToExit = 3;

    // Tear down the dead client and re-activate in place. Honors backoff so a stream
    // that cannot be recovered (or an app that is legitimately paused) is not hammered.
    auto attemptReactivate = [&](const char* reason, long hrCode, bool allowEventDriven = true) -> bool {
        const uint64_t now = GetTickCount64();
        if (!ce::audio::RecoveryBackoffElapsed(now, lastReactivateTick, recoveryBackoffMs)) {
            return false;
        }
        const DWORD pid = targetPID.load();
        ++reactivateAttempts;
        lastReactivateTick = now;
        recoveryBackoffMs = ce::audio::NextRecoveryBackoffMs(recoveryBackoffMs, recoveryCfg);
        DLL_Log(
            "[AppAudioCapture] Re-activating process-loopback stream for PID %lu (reason=%s hr=0x%lx "
            "attempt=%llu nextBackoffMs=%llu)",
            pid, reason, static_cast<unsigned long>(hrCode), static_cast<unsigned long long>(reactivateAttempts),
            static_cast<unsigned long long>(recoveryBackoffMs));
        if (captureEpochOpen) {
            const uint64_t closingEpoch = captureEpoch.load(std::memory_order_acquire);
            if (!QueueCaptureEpochMarker(AudioPacketRecordType::EndOfStream, closingEpoch, reason)) {
                DLL_Log(
                    "[AppAudioCapture] Re-activation deferred because epoch %llu could not be closed transactionally",
                    static_cast<unsigned long long>(closingEpoch));
                return false;
            }
            captureEpochOpen = false;
        }
        const bool ok = ReactivateClientForPID(pid, allowEventDriven);
        // A successful replacement must qualify with its own first packet before
        // another silent-stall recovery can arm. A failed/null client still retries
        // through the explicit backoff path below.
        lastPacketTick = GetTickCount64();
        currentActivationStartTick = lastPacketTick;
        currentActivationQualified = false;
        if (ok) {
            ++reactivateSuccesses;
            captureEpochOpen = true;
            firstSet = false;
            DLL_Log("[AppAudioCapture] Re-activation succeeded for PID %lu (attempt=%llu mode=%s)", pid,
                    static_cast<unsigned long long>(reactivateAttempts),
                    (activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 ? "event-driven" : "polling");
        } else {
            DLL_Log("[AppAudioCapture] Re-activation FAILED for PID %lu (attempt=%llu) - will retry with backoff", pid,
                    static_cast<unsigned long long>(reactivateAttempts));
        }
        return ok;
    };

    auto readNextPacketSize = [&](const char* context) -> bool {
        packetLength = 0;
        const HRESULT packetHr = pCaptureClient->GetNextPacketSize(&packetLength);
        if (SUCCEEDED(packetHr)) {
            return true;
        }
        if (ce::audio::IsFatalWasapiStreamError(packetHr)) {
            if (fatalErrLogCount++ < kErrLogCap) {
                DLL_Log(
                    "[AppAudioCapture] FATAL stream error from GetNextPacketSize (%s): 0x%lx (PID %lu) - "
                    "attempting re-activation",
                    context, static_cast<unsigned long>(packetHr), targetPID.load());
            }
            if (isCapturing.load(std::memory_order_acquire) && !shouldStop.load(std::memory_order_acquire)) {
                attemptReactivate("GetNextPacketSize_fatal", packetHr);
            }
        } else if (transientErrLogCount++ < kErrLogCap) {
            DLL_Log("[AppAudioCapture] GetNextPacketSize failed (%s): 0x%lx", context,
                    static_cast<unsigned long>(packetHr));
        }
        return false;
    };

    while (true) {
        const bool drainingAfterStop =
            !isCapturing.load(std::memory_order_acquire) || shouldStop.load(std::memory_order_acquire);
        if (drainingAfterStop) {
            // Stop wakes the worker, then the worker owns one non-blocking drain
            // of packets already committed by WASAPI. This preserves the audio
            // tail without waiting for new data or reactivating a dying stream.
            if (finalDrainFrameBudget == 0) {
                const uint32_t fallbackFrames =
                    ce::audio::MaxWasapiCapturePacketFrames(pwfx ? pwfx->nSamplesPerSec : 0);
                finalDrainFrameBudget = bufferFrameCount != 0 ? bufferFrameCount : fallbackFrames;
                if (finalDrainFrameBudget == 0) {
                    finalDrainFrameBudget = 1;
                }
            }
            if (!pCaptureClient || !readNextPacketSize("final stop drain") || packetLength == 0) {
                break;
            }
        }

        const uint64_t nowTick = GetTickCount64();
        if (!drainingAfterStop && nowTick - lastProcessCheckTick >= 500) {
            lastProcessCheckTick = nowTick;
            const DWORD activePid = targetPID.load(std::memory_order_acquire);
            if (!IsProcessRunning(activePid)) {
                if (++processMissingStreak >= kProcessMissingStreakToExit) {
                    DLL_Log("[AppAudioCapture] Target process %lu exited (missed %d consecutive checks)", activePid,
                            processMissingStreak);
                    targetPID.store(0, std::memory_order_release);
                    break;
                }
                DLL_Log("[AppAudioCapture] Target process %lu not found on check %d/%d - deferring exit", activePid,
                        processMissingStreak, kProcessMissingStreakToExit);
            } else {
                processMissingStreak = 0;
            }
        }

        const HANDLE sessionActivityEvent = audioSessionMonitor_.GetActivityEvent();
        if (!drainingAfterStop && (activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 && captureEvent_) {
            HANDLE waitHandles[] = {captureEvent_, sessionActivityEvent};
            const DWORD waitHandleCount = sessionActivityEvent ? 2 : 1;
            const DWORD waitResult = WaitForMultipleObjects(waitHandleCount, waitHandles, FALSE, 200);
            if (waitResult == WAIT_FAILED) {
                DLL_Log("[AppAudioCapture] Capture/session notification wait failed: 0x%lx", GetLastError());
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        } else if (!drainingAfterStop) {
            if (sessionActivityEvent) {
                WaitForSingleObject(sessionActivityEvent, 10);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        if (!drainingAfterStop) {
            std::array<ce::process_loopback::AudioSessionCreation, 64> sessionCreations{};
            const size_t sessionCreationCount =
                audioSessionMonitor_.TakeSessionCreations(sessionCreations.data(), sessionCreations.size());
            if (sessionCreationCount != 0) {
                const uint64_t activationGeneration = activationAudioSessionGeneration_.load(std::memory_order_acquire);
                const bool activationHadObservedTargetSession =
                    activationHadObservedTargetSession_.load(std::memory_order_acquire);
                const DWORD activePid = targetPID.load(std::memory_order_acquire);
                const auto processTree = SnapshotProcessTree();
                for (size_t creationIndex = 0; creationIndex < sessionCreationCount; ++creationIndex) {
                    const auto& creation = sessionCreations[creationIndex];
                    if (!ce::process_loopback::ShouldRecycleCaptureForSessionCreation(
                            processTree, currentActivationQualified, activationHadObservedTargetSession, activePid,
                            creation.processId, activationGeneration, creation.generation)) {
                        continue;
                    }
                    workerRecycleRequested.store(true, std::memory_order_release);
                    if (packetReadyEvent_) {
                        SetEvent(packetReadyEvent_);
                    }
                    DLL_Log(
                        "[AppAudioCapture] Target render session requires a fresh process-loopback binding: "
                        "targetPID=%lu sessionPID=%lu notificationGeneration=%llu activationGeneration=%llu "
                        "activationQualified=%d targetSessionObservedAtActivation=%d epoch=%llu; recycling the "
                        "disposable worker",
                        activePid, creation.processId, static_cast<unsigned long long>(creation.generation),
                        static_cast<unsigned long long>(activationGeneration), currentActivationQualified ? 1 : 0,
                        activationHadObservedTargetSession ? 1 : 0,
                        static_cast<unsigned long long>(captureEpoch.load(std::memory_order_relaxed)));
                    break;
                }
                if (workerRecycleRequested.load(std::memory_order_acquire)) {
                    break;
                }
            }
        }

        // A previous re-activation may have failed and abandoned the client; recover
        // it here (with backoff) before any client call, so we never deref nullptr.
        if (!pCaptureClient) {
            if (!drainingAfterStop) {
                attemptReactivate("client_null", 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            continue;
        }

        if (!drainingAfterStop && !readNextPacketSize("outer")) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        if (packetLength == 0) {
            if (drainingAfterStop) {
                break;
            }
            const uint64_t noPacketNow = GetTickCount64();
            const uint64_t activationElapsedMs =
                noPacketNow >= currentActivationStartTick ? noPacketNow - currentActivationStartTick : 0;
            const bool eventDriven = (activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0;
            if (ce::audio::ShouldFallbackUnqualifiedEventCapture(eventDriven, currentActivationQualified,
                                                                 activationElapsedMs, recoveryCfg) &&
                ce::audio::RecoveryBackoffElapsed(noPacketNow, lastReactivateTick, recoveryBackoffMs)) {
                ++eventFallbackAttempts;
                DLL_Log(
                    "[AppAudioCapture] WARNING: event-driven activation failed first-packet qualification for "
                    "PID %lu after %llu ms (epoch=%llu); re-activating polling-only and recycling the worker "
                    "generation to bound abandoned AudioSes state",
                    targetPID.load(), static_cast<unsigned long long>(activationElapsedMs),
                    static_cast<unsigned long long>(captureEpoch.load(std::memory_order_relaxed)));
                if (attemptReactivate("event_first_packet_timeout", 0, false)) {
                    ++eventFallbackSuccesses;
                }
                continue;
            }
            if (!currentActivationQualified && activationElapsedMs >= 3000 &&
                (lastNoPacketDiagnosticTick == 0 || noPacketNow - lastNoPacketDiagnosticTick >= 30000)) {
                DLL_Log(
                    "[AppAudioCapture] WARNING: process-loopback stream is active but has delivered no data "
                    "packets for %llu ms: PID=%lu process=%s epoch=%llu mode=%s processAlive=%d everPacket=%d. "
                    "sessionMonitor=%d activationSessionGeneration=%llu observedSessionGeneration=%llu "
                    "targetSessionObservedAtActivation=%d. The "
                    "polling route remains expected timeline silence; a matching post-activation render-session "
                    "notification will trigger exact recovery. Verify process-tree root selection and target audio "
                    "activity",
                    static_cast<unsigned long long>(activationElapsedMs), targetPID.load(),
                    targetProcessName.empty() ? "<pid-mode>" : targetProcessName.c_str(),
                    static_cast<unsigned long long>(captureEpoch.load(std::memory_order_relaxed)),
                    (activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 ? "event-driven" : "polling",
                    IsProcessRunning(targetPID.load()) ? 1 : 0, sawAnyPacket ? 1 : 0,
                    audioSessionMonitor_.IsRunning() ? 1 : 0,
                    static_cast<unsigned long long>(activationAudioSessionGeneration_.load(std::memory_order_acquire)),
                    static_cast<unsigned long long>(audioSessionMonitor_.Generation()),
                    activationHadObservedTargetSession_.load(std::memory_order_acquire) ? 1 : 0);
                lastNoPacketDiagnosticTick = noPacketNow;
            }
            // Silent-stall watchdog: a process-loopback stream that delivered audio
            // before but has gone fully silent (no packets, no error) while its
            // process is still running usually means the app tore down and recreated
            // its audio session (e.g. a game auto-muting on alt-tab) and the mixer
            // did not reattach. Re-activate to reattach to the live session.
            if (ce::audio::ShouldReactivateForSilentStall(currentActivationQualified, GetTickCount64(), lastPacketTick,
                                                          lastReactivateTick, recoveryBackoffMs, recoveryCfg)) {
                DLL_Log(
                    "[AppAudioCapture] Process-loopback silent stall for PID %lu (%llu ms without packets, process "
                    "alive) - re-activating",
                    targetPID.load(), static_cast<unsigned long long>(GetTickCount64() - lastPacketTick));
                attemptReactivate("silent_stall", 0);
            }
            continue;
        }

        while (packetLength != 0) {
            if (drainingAfterStop && finalDrainFrames >= finalDrainFrameBudget) {
                DLL_Log(
                    "[AppAudioCapture] Final drain reached the endpoint-buffer bound (%llu frame(s)); leaving "
                    "newly-arrived data for stream teardown (PID %lu)",
                    static_cast<unsigned long long>(finalDrainFrameBudget), targetPID.load());
                packetLength = 0;
                break;
            }
            hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, &devicePosition, &qpcPosition);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) {
                // No packet was acquired for this success status. In particular,
                // do not call ReleaseBuffer: that would be AUDCLNT_E_OUT_OF_ORDER.
                if (emptyBufferLogCount++ < kErrLogCap) {
                    DLL_Log(
                        "[AppAudioCapture] GetBuffer returned AUDCLNT_S_BUFFER_EMPTY for PID %lu after announcing "
                        "%u frame(s); waiting for the next capture notification",
                        targetPID.load(), packetLength);
                }
                packetLength = 0;
                break;
            }
            if (FAILED(hr)) {
                if (ce::audio::IsFatalWasapiStreamError(hr)) {
                    if (fatalErrLogCount++ < kErrLogCap) {
                        DLL_Log(
                            "[AppAudioCapture] FATAL stream error from GetBuffer: 0x%lx (PID %lu) - attempting "
                            "re-activation",
                            static_cast<unsigned long>(hr), targetPID.load());
