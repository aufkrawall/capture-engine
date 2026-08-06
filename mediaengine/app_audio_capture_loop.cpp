#include "app_audio_capture_internal.h"

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

                    }
                    if (!drainingAfterStop) {
                        attemptReactivate("GetBuffer_fatal", hr);
                    }
                } else if (getBufferErrLogCount++ < kErrLogCap) {
                    DLL_Log("[AppAudioCapture] GetBuffer failed: 0x%lx", static_cast<unsigned long>(hr));
                }
                break;
            }
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
                    if (qpcSanitizeLogCount++ < kErrLogCap) {
                        DLL_Log(
                            "[AppAudioCapture] WARNING: out-of-domain WASAPI qpcPosition=%llu substituted with "
                            "nowQpc=%llu (PID=%lu frames=%u flags=0x%lx%s)",
                            static_cast<unsigned long long>(qpcPosition), static_cast<unsigned long long>(nowQpc100ns),
                            targetPID.load(), numFramesAvailable, flags,
                            (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0 ? " TIMESTAMP_ERROR" : "");
                    }
                    qpcPosition = sanitizedQpc;
                }
            }

            size_t bytes = 0;
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            const bool frameCountValid = ce::audio::IsWasapiCapturePacketFrameCountValid(
                packetLength, numFramesAvailable, bufferFrameCount, pwfx ? pwfx->nSamplesPerSec : 0);
            const bool byteSizeValid =
                pwfx && ce::audio::TryComputeAudioPacketByteSize(numFramesAvailable, pwfx->nBlockAlign, &bytes);
            if (!frameCountValid || !byteSizeValid || (!silent && !pData)) {
                if (invalidPacketLogCount++ < kErrLogCap) {
                    DLL_Log(
                        "[AppAudioCapture] WARNING: rejecting malformed WASAPI packet for PID %lu: announced=%u "
                        "actual=%u bufferFrames=%u sampleRate=%u maxPacketFrames=%u blockAlign=%u data=%p "
                        "flags=0x%lx",
                        targetPID.load(), packetLength, numFramesAvailable, bufferFrameCount,
                        pwfx ? pwfx->nSamplesPerSec : 0,
                        ce::audio::MaxWasapiCapturePacketFrames(pwfx ? pwfx->nSamplesPerSec : 0),
                        pwfx ? pwfx->nBlockAlign : 0, pData, flags);
                }
                const HRESULT releaseHr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
                if (FAILED(releaseHr)) {
                    if (releaseBufferErrLogCount++ < kErrLogCap) {
                        DLL_Log("[AppAudioCapture] ReleaseBuffer failed after malformed packet: 0x%lx",
                                static_cast<unsigned long>(releaseHr));
                    }
                    if (!drainingAfterStop) {
                        attemptReactivate("ReleaseBuffer_malformed", releaseHr);
                    }
                    packetLength = 0;
                    break;
                }
                if (!readNextPacketSize("after malformed packet")) {
                    break;
                }
                continue;
            }
            if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0 && discontinuityLogCount < kErrLogCap) {
                ++discontinuityLogCount;
                DLL_Log(
                    "[AppAudioCapture] WASAPI data discontinuity for PID %lu: frames=%u devPos=%llu qpc=%llu "
                    "(occurrence=%d)",
                    targetPID.load(), numFramesAvailable, static_cast<unsigned long long>(devicePosition),
                    static_cast<unsigned long long>(qpcPosition), discontinuityLogCount);
            }

            // Healthy, validated delivery: reset the silent-stall window and
            // clear backoff so future recovery is responsive.
            lastPacketTick = GetTickCount64();
            if (!currentActivationQualified) {
                currentActivationQualified = true;
                ++qualifiedActivationCount;
                DLL_Log(
                    "[AppAudioCapture] First-packet qualification succeeded: PID=%lu epoch=%llu mode=%s "
                    "elapsed=%llums frames=%u flags=0x%lx targetSessionObservedAtActivation=%d",
                    targetPID.load(), static_cast<unsigned long long>(captureEpoch.load(std::memory_order_relaxed)),
                    (activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 ? "event-driven" : "polling",
                    static_cast<unsigned long long>(
                        lastPacketTick >= currentActivationStartTick ? lastPacketTick - currentActivationStartTick : 0),
                    numFramesAvailable, flags,
                    activationHadObservedTargetSession_.load(std::memory_order_acquire) ? 1 : 0);
            }
            sawAnyPacket = true;
            recoveryBackoffMs = 0;
            if (drainingAfterStop) {
                ++finalDrainPackets;
                finalDrainFrames += numFramesAvailable;
            }

            const uint64_t logicalFramePos = devicePosition > 0 ? devicePosition : logicalFrameCursor;

            // Debug: Check drift. Process loopback often reports devicePosition=0,
            // so fall back to the cumulative frame count to keep telemetry alive.
            if (!firstSet && qpcPosition > 0) {
                firstLogicalFramePos = logicalFramePos;
                firstQpcPos = qpcPosition;
                firstSet = true;
                const uint64_t packetDuration100ns =
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    ce::audio::AudioFramesToHundredNanoseconds(numFramesAvailable, pwfx->nSamplesPerSec);
                const uint64_t packetStartQpc = ce::audio::ApplyProcessLoopbackPacketTimestampCompensation(
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    firstQpcPos, numFramesAvailable, pwfx->nSamplesPerSec);
                // streamLatency telemetry only; placement uses packetStartQpc (period-center). The
                // wouldAdvanceQpc shows the retired GetStreamLatency audio-advance, not applied.
                const uint64_t wouldAdvanceQpc =
                    ce::audio::ApplyCaptureLatencyCompensation(packetStartQpc, streamLatency100ns, true);
                DLL_Log(
                    "[AppAudioCapture] Source Sync Start (%lu): Frames=%llu QPC=%llu placedQPC=%llu "
                    "wouldAdvanceQpc=%llu streamLatency=%lluus packetFrames=%u packetDuration=%lluus "
                    "processLoopbackPacketBias=half_period (latency via video delay, not applied)",
                    targetPID.load(), firstLogicalFramePos, firstQpcPos, packetStartQpc, wouldAdvanceQpc,
                    static_cast<unsigned long long>(streamLatency100ns / 10), numFramesAvailable,
                    static_cast<unsigned long long>(packetDuration100ns / 10));
            } else if (firstSet && qpcPosition > firstQpcPos && logCounter++ % 500 == 0) {  // ~5 seconds
                double samplesDuration = (double)(logicalFramePos - firstLogicalFramePos) / pwfx->nSamplesPerSec;
                double qpcDuration = ce::audio::HundredNanosecondsToSeconds(qpcPosition - firstQpcPos);
                double driftMs = (samplesDuration - qpcDuration) * 1000.0;

                DLL_Log(
                    "[AppAudioCapture] Source Sync (%lu): Duration Samples=%.4fs, "
                    "QPC=%.4fs, Drift=%.2f ms (%.4f%%)",
                    targetPID.load(), samplesDuration, qpcDuration, driftMs,
                    qpcDuration > 0 ? (driftMs / (qpcDuration * 1000.0) * 100.0) : 0.0);
                // Diagnostics: is the captured CONTENT real or silent this window? peakAbs≈0 with
                // high silent/zero frame ratios while the stream stays alive points to a stuck-silent
                // process loopback; peakAbs>0 means real audio is captured and any track silence is
                // a downstream placement/consumption problem, not capture.
                const double silentPct =
                    windowTotalFrames > 0
                        ? (100.0 * static_cast<double>(windowSilentFlagFrames + windowZeroContentFrames) /
                           static_cast<double>(windowTotalFrames))
                        : 0.0;
                DLL_Log(
                    "[AppAudioCapture] Content (%lu): peakAbs=%.5f silentFlagFrames=%llu zeroFrames=%llu "
                    "totalFrames=%llu silent=%.1f%%",
                    targetPID.load(), windowPeakAbs, static_cast<unsigned long long>(windowSilentFlagFrames),
                    static_cast<unsigned long long>(windowZeroContentFrames),
                    static_cast<unsigned long long>(windowTotalFrames), silentPct);
                windowTotalFrames = 0;
                windowSilentFlagFrames = 0;
                windowZeroContentFrames = 0;
                windowPeakAbs = 0.0f;
            }

            // Build packet with format info
            AudioPacket packet{};
            packet.channels = pwfx->nChannels;
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            packet.sampleRate = pwfx->nSamplesPerSec;
            packet.bitsPerSample = pwfx->wBitsPerSample;
            packet.blockAlign = pwfx->nBlockAlign;
            packet.validBitsPerSample = 0;
            packet.channelMask = 0;
            packet.devicePosition = devicePosition;     // Store for debug drift analysis
            packet.rawQpcPosition = rawQpcPosition;     // Store raw WASAPI timestamp for debug drift analysis
            packet.streamLatency = streamLatency100ns;  // telemetry only (see below)
            packet.captureEpoch = captureEpoch.load(std::memory_order_acquire);
            // Period-center bias only (process loopback reports end-of-period QPCs). Do NOT advance
            // by streamLatency: the render->loopback A/V offset is corrected by delaying video
            // content (audio/PTS untouched), never by advancing live audio (the earlier samples do
            // not exist; the CFR pipeline absorbs the shift). Process loopback shares the render
            // endpoint, so it inherits audio_capture_latency_ms and is handled by the video delay.
            packet.qpcPosition = ce::audio::ApplyProcessLoopbackPacketTimestampCompensation(
                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                qpcPosition, numFramesAvailable, pwfx->nSamplesPerSec);

            // Check for float format
            packet.isFloat = false;
            if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                packet.isFloat = true;
            } else if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                WAVEFORMATEXTENSIBLE* wfex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx);
                packet.channelMask = static_cast<uint32_t>(wfex->dwChannelMask);
                if (IsIEEEFloat(wfex->SubFormat)) {
                    packet.isFloat = true;
                }
                packet.validBitsPerSample = wfex->Samples.wValidBitsPerSample;
            }
            if (packet.channelMask == 0) {
                if (packet.channels == 1) {
                    packet.channelMask = SPEAKER_FRONT_CENTER;
                } else if (packet.channels == 2) {
                    packet.channelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
                }
            }

            packet.timestamp = (int64_t)ce::audio::HundredNanosecondsToMilliseconds(packet.qpcPosition);

            // Copy or generate silence
            try {
                packet.data.resize(bytes);
            } catch (const std::exception& error) {
                if (allocationFailureLogCount++ < kErrLogCap) {
                    DLL_Log(
                        "[AppAudioCapture] WARNING: packet allocation failed for PID %lu: %zu byte(s) / %u "
                        "frame(s): %s; dropping this packet without terminating capture",
                        targetPID.load(), bytes, numFramesAvailable, error.what());
                }
                const HRESULT releaseHr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
                if (FAILED(releaseHr)) {
                    if (releaseBufferErrLogCount++ < kErrLogCap) {
                        DLL_Log("[AppAudioCapture] ReleaseBuffer failed after packet allocation error: 0x%lx",
                                static_cast<unsigned long>(releaseHr));
                    }
                    if (!drainingAfterStop) {
                        attemptReactivate("ReleaseBuffer_allocation_failed", releaseHr);
                    }
                    packetLength = 0;
                    break;
                }
                if (!readNextPacketSize("after packet allocation failure")) {
                    break;
                }
                continue;
            }

            windowTotalFrames += numFramesAvailable;
            if (silent) {
                memset(packet.data.data(), 0, bytes);
                windowSilentFlagFrames += numFramesAvailable;
            } else {
                memcpy(packet.data.data(), pData, bytes);
                // Content amplitude probe (diagnostics): our requested capture format is
                // 32-bit IEEE float, so scan the packet for the peak |sample| and whether it
                // is all-zero despite no SILENT flag. This distinguishes "loopback delivers
                // real audio" from "loopback stuck delivering silence" downstream confusion.
                if (packet.isFloat && pwfx->wBitsPerSample == 32) {
                    const float* samples = reinterpret_cast<const float*>(pData);
                    const size_t sampleCount = bytes / sizeof(float);
                    float packetPeak = 0.0f;
                    for (size_t i = 0; i < sampleCount; ++i) {
                        const float a = samples[i] < 0.0f ? -samples[i] : samples[i];
                        if (a > packetPeak) {
                            packetPeak = a;
                        }
                    }
                    if (packetPeak > windowPeakAbs) {
                        windowPeakAbs = packetPeak;
                    }
                    if (packetPeak == 0.0f) {
                        windowZeroContentFrames += numFramesAvailable;
                    }
                }
            }

            bool logQueueDrop = false;
            uint64_t droppedPacketsToLog = 0;
            uint64_t droppedFramesToLog = 0;
            try {
                std::lock_guard<std::mutex> lock(queueMutex);
                // Transactional live-edge retention: if deque growth fails,
                // every already-queued packet remains intact.
                packetQueue.emplace_back(std::move(packet));
                if (packetReadyEvent_) {
                    SetEvent(packetReadyEvent_);
                }
                if (packetQueue.size() > kMaxQueuedPackets) {
                    auto droppedIt = std::find_if(packetQueue.begin(), packetQueue.end(), [](const auto& queued) {
                        return queued.recordType == AudioPacketRecordType::Data;
                    });
                    if (droppedIt == packetQueue.end()) {
                        --droppedIt;  // The packet just appended above is always a data record.
                    }
                    const AudioPacket& droppedPacket = *droppedIt;
                    uint64_t droppedFrames = 0;
                    if (droppedPacket.blockAlign > 0) {
                        droppedFrames = droppedPacket.data.size() / static_cast<size_t>(droppedPacket.blockAlign);
                        queueDropFrames += droppedFrames;
                    }
                    queueDropPackets++;
                    queueOverrunPackets.fetch_add(1, std::memory_order_relaxed);
                    queueOverrunFrames.fetch_add(droppedFrames, std::memory_order_relaxed);
                    packetQueue.erase(droppedIt);

                    const uint64_t queueNowTick = GetTickCount64();
                    if (queueNowTick - lastQueueDropLogTick >= 1000) {
                        logQueueDrop = true;
                        droppedPacketsToLog = queueDropPackets;
                        droppedFramesToLog = queueDropFrames;
                        queueDropPackets = 0;
                        queueDropFrames = 0;
                        lastQueueDropLogTick = queueNowTick;
                    }
                }
            } catch (const std::exception& error) {
                if (allocationFailureLogCount++ < kErrLogCap) {
                    DLL_Log(
                        "[AppAudioCapture] WARNING: capture queue insertion failed for PID %lu: %s; dropping "
                        "the new packet without terminating capture",
                        targetPID.load(), error.what());
                }
            }
            if (logQueueDrop) {
                DLL_Log(
                    "[AppAudioCapture] WARNING: capture queue overrun - dropped %llu packet(s) / %llu frame(s) "
                    "for PID %lu while keeping newest audio (depth=%zu)",
                    static_cast<unsigned long long>(droppedPacketsToLog),
                    static_cast<unsigned long long>(droppedFramesToLog), targetPID.load(), kMaxQueuedPackets);
            }

            if (devicePosition > 0) {
                logicalFrameCursor = devicePosition + numFramesAvailable;
            } else {
                logicalFrameCursor += numFramesAvailable;
            }

            const HRESULT releaseHr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
            if (FAILED(releaseHr)) {
                if (releaseBufferErrLogCount++ < kErrLogCap) {
                    DLL_Log("[AppAudioCapture] ReleaseBuffer failed: 0x%lx - re-activating stream",
                            static_cast<unsigned long>(releaseHr));
                }
                if (!drainingAfterStop) {
                    attemptReactivate("ReleaseBuffer_failed", releaseHr);
                }
                packetLength = 0;
                break;
            }
            if (!readNextPacketSize("inner drain")) {
                break;
            }
        }
        if (drainingAfterStop) {
            break;
        }
    }

    DLL_Log("[AppAudioCapture] Capture loop exited");
    DLL_Log(
        "[AppAudioCapture] First-packet qualification summary for PID %lu: everPacket=%d currentQualified=%d "
        "qualifiedActivations=%llu eventFallback=%llu/%llu finalEpoch=%llu finalMode=%s",
        targetPID.load(), sawAnyPacket ? 1 : 0, currentActivationQualified ? 1 : 0,
        static_cast<unsigned long long>(qualifiedActivationCount),
        static_cast<unsigned long long>(eventFallbackSuccesses), static_cast<unsigned long long>(eventFallbackAttempts),
        static_cast<unsigned long long>(captureEpoch.load(std::memory_order_relaxed)),
        (activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 ? "event-driven" : "polling");
    if (reactivateAttempts > 0) {
        DLL_Log("[AppAudioCapture] Stream recovery summary for PID %lu: %llu re-activation attempt(s), %llu succeeded",
                targetPID.load(), static_cast<unsigned long long>(reactivateAttempts),
                static_cast<unsigned long long>(reactivateSuccesses));
    }
    if (finalDrainPackets > 0 || finalDrainFrames > 0) {
        DLL_Log("[AppAudioCapture] Final stop drain queued %llu packet(s) / %llu frame(s) for PID %lu",
                static_cast<unsigned long long>(finalDrainPackets), static_cast<unsigned long long>(finalDrainFrames),
                targetPID.load());
    }
    if (queueDropPackets > 0 || queueDropFrames > 0) {
        DLL_Log("[AppAudioCapture] Final queue overrun summary for PID %lu: dropped %llu packet(s) / %llu frame(s)",
                targetPID.load(), static_cast<unsigned long long>(queueDropPackets),
                static_cast<unsigned long long>(queueDropFrames));
    }
    if (captureEpochOpen) {
        const uint64_t closingEpoch = captureEpoch.load(std::memory_order_acquire);
        if (QueueCaptureEpochMarker(AudioPacketRecordType::EndOfStream, closingEpoch, "capture loop exit")) {
            captureEpochOpen = false;
        }
    } else {
        DLL_Log("[AppAudioCapture] Capture loop exit found the current epoch already closed by stream recovery");
    }
    isCapturing.store(false);
    startPendingValid.store(false, std::memory_order_release);
    if (SUCCEEDED(coInitHr)) {
        CoUninitialize();
    }
}

