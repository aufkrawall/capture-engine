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
                    ce::audio::AudioFramesToHundredNanoseconds(numFramesAvailable, pwfx->nSamplesPerSec);
                const uint64_t packetStartQpc = ce::audio::ApplyProcessLoopbackPacketTimestampCompensation(
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

void AppAudioCapture::ProcessMonitorLoop() {
    DLL_Log("[AppAudioCapture] Monitor loop started for '%s'", targetProcessName.c_str());

    while (isMonitoring.load() && !shouldStop.load()) {
        FinalizePendingAsyncStart();

        if (asyncStartInProgress.load(std::memory_order_acquire)) {
            for (int i = 0; i < 5 && isMonitoring.load() && !shouldStop.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        // Check if we're already capturing
        if (!isCapturing.load()) {
            // CaptureLoop owns the three-consecutive-miss process-exit policy.
            // Reap and clean its completed session before activating a replacement;
            // a second one-shot monitor probe used to bypass that tolerance and
            // permanently stop healthy capture on a transient OpenProcess failure.
            if (captureThread.joinable()) {
                captureThread.join();
                CleanupCapture();
                targetPID.store(0, std::memory_order_release);
            }

            // Not capturing - try to find the target process
            const auto selection = FindProcessByName(targetProcessName);
            const DWORD pid = selection.selectedProcessId;
            if (pid != 0) {
                DLL_Log("[AppAudioCapture] Selected process-tree root '%s' with PID %lu", targetProcessName.c_str(),
                        pid);
                targetPID.store(pid);
                if (!BeginAsyncStartForPID(pid)) {
                    targetPID.store(0, std::memory_order_release);
                }
            }
        }

        // Check every second, but use small intervals for responsive shutdown
        for (int i = 0; i < 10 && isMonitoring.load() && !shouldStop.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    DLL_Log("[AppAudioCapture] Monitor loop exited");
}

ce::process_loopback::ProcessNameSelection AppAudioCapture::FindProcessByName(const std::string& name,
                                                                              bool logSelection) {
    const auto processes = SnapshotProcessTree();
    const auto selection = ce::process_loopback::SelectProcessTreeRootByName(processes, name);
    if (logSelection && selection.selectedProcessId != 0) {
        DLL_Log(
            "[AppAudioCapture] Process-name tree resolution '%s': matches=%zu roots=%zu firstPID=%lu "
            "selectedRootPID=%lu selectedParentPID=%lu selectedNameMembers=%zu selectedProcessTreeMembers=%zu "
            "firstMatchWasRoot=%d",
            name.c_str(), selection.matchingProcessCount, selection.rootCandidateCount,
            static_cast<unsigned long>(selection.firstMatchProcessId),
            static_cast<unsigned long>(selection.selectedProcessId),
            static_cast<unsigned long>(selection.selectedParentProcessId), selection.selectedTreeSize,
            selection.selectedProcessTreeSize, selection.firstMatchProcessId == selection.selectedProcessId ? 1 : 0);
    }
    return selection;
}

bool AppAudioCapture::IsProcessRunning(DWORD pid) {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) {
        return false;
    }

    DWORD exitCode = 0;
    BOOL result = GetExitCodeProcess(hProcess, &exitCode);
    CloseHandle(hProcess);

    return result && exitCode == STILL_ACTIVE;
}
