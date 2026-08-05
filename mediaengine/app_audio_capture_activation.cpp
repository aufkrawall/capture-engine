#include "app_audio_capture_internal.h"

bool AppAudioCapture::BeginAsyncStartForPID(DWORD pid) {
    std::lock_guard<std::mutex> lock(startMutex);
    if (pendingStartFuture.valid()) {
        pendingStartFuture.wait();
        bool started = false;
        try {
            started = pendingStartFuture.get();
        } catch (const std::exception& error) {
            DLL_Log("[AppAudioCapture] Prior async start raised for PID %lu: %s", targetPID.load(), error.what());
        }
        if (!started) {
            DLL_Log("[AppAudioCapture] Async start failed for PID %lu", targetPID.load());
        }
    }

    asyncStartInProgress.store(true, std::memory_order_release);
    startPendingValid.store(false, std::memory_order_release);
    try {
        pendingStartFuture = std::async(std::launch::async, [this, pid]() {
            bool ok = false;
            try {
                ok = InitializeCaptureForPID(pid);
            } catch (const std::exception& error) {
                DLL_Log("[AppAudioCapture] Async initialization raised for PID %lu: %s", pid, error.what());
                CleanupCapture();
            }
            startPendingResult.store(ok, std::memory_order_release);
            startPendingValid.store(true, std::memory_order_release);
            asyncStartInProgress.store(false, std::memory_order_release);
            // Wake the process-loopback worker even when activation failed and
            // therefore produced no epoch/data record. This keeps worker state
            // transitions event-driven instead of requiring an activation poll.
            if (packetReadyEvent_) {
                SetEvent(packetReadyEvent_);
            }
            return ok;
        });
    } catch (const std::exception& error) {
        DLL_Log("[AppAudioCapture] Failed to launch async start for PID %lu: %s", pid, error.what());
        asyncStartInProgress.store(false, std::memory_order_release);
        startPendingValid.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void AppAudioCapture::FinalizePendingAsyncStart() {
    if (!startPendingValid.load(std::memory_order_acquire)) {
        return;
    }

    std::lock_guard<std::mutex> lock(startMutex);
    if (pendingStartFuture.valid() &&
        pendingStartFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        bool ok = false;
        try {
            ok = pendingStartFuture.get();
        } catch (const std::exception& error) {
            DLL_Log("[AppAudioCapture] Async start completion raised for PID %lu: %s", targetPID.load(), error.what());
        }
        startPendingValid.store(false, std::memory_order_release);
        if (ok) {
            DLL_Log("[AppAudioCapture] Async start completed for PID %lu", targetPID.load());
        } else {
            DLL_Log("[AppAudioCapture] Async start failed for PID %lu", targetPID.load());
        }
        if (!ok && !isMonitoring.load(std::memory_order_acquire)) {
            targetPID.store(0, std::memory_order_release);
        }
    }
}

bool AppAudioCapture::StartCaptureThreadForCurrentClient() {
    // ActivateClientForPID has already committed the epoch-start marker. Never
    // clear the queue here: the worker must observe that lifecycle record before
    // the first data packet from the thread launched below.
    queueOverrunPackets.store(0, std::memory_order_relaxed);
    queueOverrunFrames.store(0, std::memory_order_relaxed);

    if (captureThread.joinable()) {
        captureThread.join();
    }

    isCapturing.store(true, std::memory_order_release);
    try {
        captureThread = std::thread(&AppAudioCapture::CaptureLoop, this);
    } catch (const std::exception& error) {
        DLL_Log("[AppAudioCapture] Failed to create capture thread for PID %lu: %s", targetPID.load(), error.what());
        isCapturing.store(false, std::memory_order_release);
        (void)QueueCaptureEpochMarker(AudioPacketRecordType::EndOfStream,
                                      captureEpoch.load(std::memory_order_acquire), "capture thread creation failure");
        CleanupCapture();
        return false;
    }
    return true;
}

bool AppAudioCapture::QueueCaptureEpochMarker(AudioPacketRecordType recordType, uint64_t epoch, const char* reason) {
    if ((recordType != AudioPacketRecordType::EpochStart && recordType != AudioPacketRecordType::EndOfStream) ||
        epoch == 0) {
        DLL_Log("[AppAudioCapture] ERROR: Refusing invalid capture epoch marker: record=%u epoch=%llu reason=%s",
                static_cast<unsigned>(recordType), static_cast<unsigned long long>(epoch), reason ? reason : "unknown");
        return false;
    }

    try {
        AudioPacket marker;
        marker.captureEpoch = epoch;
        marker.recordType = recordType;
        marker.endOfStream = recordType == AudioPacketRecordType::EndOfStream;
        size_t markerQueueDepth = 0;
        DWORD signalError = ERROR_SUCCESS;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            // Lifecycle markers are ordered with data and are never subject to
            // the bounded data-packet retention policy.
            packetQueue.emplace_back(std::move(marker));
            if (packetReadyEvent_ && !SetEvent(packetReadyEvent_)) {
                signalError = GetLastError();
            }
            markerQueueDepth = packetQueue.size();
        }
        DLL_Log("[AppAudioCapture] Queued ordered capture-%s marker: epoch=%llu queueDepth=%zu reason=%s",
                recordType == AudioPacketRecordType::EpochStart ? "start" : "end",
                static_cast<unsigned long long>(epoch), markerQueueDepth, reason ? reason : "unknown");
        if (signalError != ERROR_SUCCESS) {
            DLL_Log("[AppAudioCapture] ERROR: Failed to signal queued capture epoch marker: error=0x%lx",
                    signalError);
        }
        return true;
    } catch (const std::exception& error) {
        DLL_Log("[AppAudioCapture] ERROR: Failed to queue capture epoch marker: record=%u epoch=%llu reason=%s: %s",
                static_cast<unsigned>(recordType), static_cast<unsigned long long>(epoch), reason ? reason : "unknown",
                error.what());
        return false;
    }
}

bool AppAudioCapture::InitializeCaptureForPID(DWORD pid) {
    if (!audioSessionMonitor_.IsRunning()) {
        if (audioSessionMonitor_.Start(stopEvent_)) {
            DLL_Log(
                "[AppAudioCapture] Audio-session creation monitor armed before process-loopback activation: "
                "registeredEndpoints=%zu activeEndpoints=%zu existingSessions=%zu registrationFailures=%zu "
                "firstFailure=0x%lx",
                audioSessionMonitor_.RegisteredEndpointCount(), audioSessionMonitor_.ActiveEndpointCount(),
                audioSessionMonitor_.ExistingSessionCount(), audioSessionMonitor_.RegistrationFailureCount(),
                static_cast<unsigned long>(audioSessionMonitor_.FirstRegistrationFailure()));
        } else {
            DLL_Log(
                "[AppAudioCapture] WARNING: audio-session creation monitor unavailable (hr=0x%lx); "
                "initial process-loopback activation cannot observe a render session created after startup",
                static_cast<unsigned long>(audioSessionMonitor_.StartupResult()));
        }
    }
    if (!ActivateClientForPID(pid, true)) {
        return false;
    }
    return StartCaptureThreadForCurrentClient();
}

bool AppAudioCapture::ReactivateClientForPID(DWORD pid, bool allowEventDriven) {
    // Drop the dead client without releasing the process-loopback COM interfaces
    // (releasing them crashes AudioSes CLoopbackMixer cleanup). The capture
    // thread, packet queue, and downstream track stay alive; we just swap in a
    // freshly activated client so packets resume on the same source.
    AbandonClientInterfaces();
    return ActivateClientForPID(pid, allowEventDriven);
}

bool AppAudioCapture::ActivateAudioInterfaceForPID(DWORD pid, IAudioClient** audioClient) {
    if (!audioClient) {
        return false;
    }
    *audioClient = nullptr;

    // Set up activation parameters for per-process loopback
    AUDIOCLIENT_ACTIVATION_PARAMS audioParams = {};
    audioParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    audioParams.ProcessLoopbackParams.TargetProcessId = pid;
    audioParams.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activateParams = {};
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(audioParams);
    activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&audioParams);

    // Each activation owns its completion event through its handler. Reusing a
    // shared event lets a timed-out old callback wake a later activation and makes
    // the later attempt consume the wrong result.
    auto* handler = new (std::nothrow) ActivationHandler();
    if (!handler || !handler->GetEvent()) {
        DLL_Log("[AppAudioCapture] Failed to create process-loopback activation handler/event: 0x%lx", GetLastError());
        if (handler) {
            handler->Release();
        }
        return false;
    }

    IActivateAudioInterfaceAsyncOperation* asyncOp = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                             &activateParams, handler, &asyncOp);

    if (FAILED(hr)) {
        DLL_Log("[AppAudioCapture] ActivateAudioInterfaceAsync failed: 0x%x", hr);
        handler->Release();

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
        const auto processTree = app_audio_capture_SnapshotProcessTree();
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
