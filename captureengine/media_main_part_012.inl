                updateWgcIngressPressure(popped ? "post-select" : "post-hold");
            } else {
                // VFR: keep the existing lowest-latency newest-frame sampling.
                QueuedFrame temp;
                while (g_FrameQueue.Pop(temp, 0)) {
                    if (g_RejectInjectFrames.load(std::memory_order_acquire) && temp.isInjectMode) {
                        DiscardQueuedFrame(temp);
                        continue;
                    }
                    if (!ce::capture_policy::ShouldAcceptFrameForActiveCapturePath(useScreenGrab, temp.isInjectMode)) {
                        discardActivePathMismatchFrame(temp, "WGC VFR queue", true);
                        continue;
                    }
                    if (popped && !frame.isInjectMode && frame.texture) {
                        frame.texture->Release();
                    }
                    frame = std::move(temp);
                    popped = true;
                }
            }
        } else {
            if (!bufferedWgcFrames.empty()) {
                ClearBufferedWgcFrames();
            }
            if (g_RejectInjectFrames.load(std::memory_order_acquire) && !bufferedInjectFrames.empty()) {
                ClearBufferedInjectFrames();
            }

            if (!config.video.useVFR) {
                // Keep multiple inject frames in reserve so the encoder usually works on
                // textures whose GPU copy has already completed instead of blocking on the
                // newest frame's fence.
                drainedInjectFrames.clear();
                QueuedFrame temp;
                while (g_FrameQueue.Pop(temp, 0)) {
                    if (g_RejectInjectFrames.load(std::memory_order_acquire) && temp.isInjectMode) {
                        DiscardQueuedFrame(temp);
                        continue;
                    }
                    if (!ce::capture_policy::ShouldAcceptFrameForActiveCapturePath(useScreenGrab, temp.isInjectMode)) {
                        discardActivePathMismatchFrame(temp, "inject CFR queue", true);
                        continue;
                    }
                    if (temp.isInjectMode && temp.timestamp > 0) {
                        injectInputPredictor.Update(temp.timestamp, qpcFreq.QuadPart);
                        observeCaptureSyncPhaseSource("inject", injectCfrPhaseLock, temp.timestamp);
                    }
                    drainedInjectFrames.push_back(std::move(temp));
                }

                for (auto& drainedFrame : drainedInjectFrames) {
                    bufferedInjectFrames.push_back(std::move(drainedFrame));
                }

                // Track frame arrival rate for source-health telemetry. Use a short
                // window during warmup/startup so the EMA is already calibrated
                // when recording goes live, then widen to half-second for
                // steady-state stability. Timestamp-target playout, not this EMA,
                // decides which source frame represents each CFR output slot.
                pacingInputThisWindow += (uint32_t)drainedInjectFrames.size();
                pacingTicksThisWindow++;
                const uint32_t pacingWindowSize = (pacingEmaUpdates < 6) ? std::max((uint32_t)config.video.fps / 8, 8u)
                                                                         : (uint32_t)config.video.fps / 2;
                if (pacingTicksThisWindow >= pacingWindowSize) {
                    double measuredRate = (double)pacingInputThisWindow / (double)pacingTicksThisWindow;
                    // Adaptive alpha: converge fast during startup (0.7), steady-state (0.5),
                    // or when FPS transitions are detected (>20% deviation -> 0.8) so
                    // diagnostics follow rapid source-rate changes promptly.
                    double alpha = 0.5;
                    if (pacingEmaUpdates < 6) {
                        alpha = 0.7;
                    } else if (smoothedInputPerTick > 0.01) {
                        double deviation = std::abs(measuredRate - smoothedInputPerTick) / smoothedInputPerTick;
                        if (deviation > 0.20) {
                            alpha = 0.8;
                        }
                    }
                    smoothedInputPerTick = smoothedInputPerTick * (1.0 - alpha) + measuredRate * alpha;
                    pacingInputThisWindow = 0;
                    pacingTicksThisWindow = 0;
                    ++pacingEmaUpdates;
                }

                const size_t injectReserveFrames = ce::capture_policy::GetInjectReserveFrames(
                    config.video.useVFR, smoothedInjectFenceMs, frameIntervalMs);
                // Only the physical GPU/fence safety tail is protected from selection. The A/V
                // content delay is a timestamp target below; treating it as additional protected
                // frames hides every useful candidate at normal queue depth and creates trim/repeat
                // churn even when the game supplies one fresh frame per CFR tick.
                const size_t protectedInjectTailFrames =
                    ce::capture_policy::GetMinBufferedInjectFrames(injectReserveFrames, recordingOutputLive);
                const size_t maxBufferedInjectFrames =
                    std::max(ce::capture_policy::GetMaxBufferedInjectFrames(injectReserveFrames, recordingOutputLive,
                                                                            recordingLiveTick, GetTickCount64()),
                             injectReserveFrames + injectContentDelayFrames + 2);
                maxBufferedInjectDepthSinceLog = std::max(maxBufferedInjectDepthSinceLog, bufferedInjectFrames.size());
                uint32_t trimmedInjectFrames = 0;
                while (bufferedInjectFrames.size() > maxBufferedInjectFrames) {
                    QueuedFrame staleFrame = std::move(bufferedInjectFrames.front());
                    bufferedInjectFrames.pop_front();
                    DiscardQueuedFrame(staleFrame);
                    ++trimmedInjectFrames;
                    ++injectBufferCapTrimTotal;
                }
                if (trimmedInjectFrames > 0) {
                    pendingInjectTrimmedLogCount += trimmedInjectFrames;
                    g_InjectBufferedTrimmedFrames.fetch_add(trimmedInjectFrames, std::memory_order_relaxed);
                    if (g_pSharedMem) {
                        g_pSharedMem->runtimeState.injectTrimmedFrames.fetch_add(trimmedInjectFrames,
                                                                                 std::memory_order_relaxed);
                    }
                    if (lastDeferredLineage.IsValid() && !bufferedInjectFrames.empty() &&
                        !std::any_of(bufferedInjectFrames.begin(), bufferedInjectFrames.end(),
                                     [&](const QueuedFrame& candidate) {
                                         return MatchesInjectFrameLineage(candidate, lastDeferredLineage);
                                     })) {
                        lastDeferredLineage = {};
                    }
                }
                DWORD now = GetTickCount();
                if (pendingInjectTrimmedLogCount > 0 && now - lastInjectTrimLog >= 1000) {
                    LogInfo(
                        "[EncoderThread] Trimmed %u inject frame(s) at the hard buffer cap "
                        "(peak=%zu cap=%zu fenceTail=%zu delayFrames=%zu total=%llu)",
                        pendingInjectTrimmedLogCount, maxBufferedInjectDepthSinceLog, maxBufferedInjectFrames,
                        protectedInjectTailFrames, injectContentDelayFrames,
                        static_cast<unsigned long long>(injectBufferCapTrimTotal));
                    pendingInjectTrimmedLogCount = 0;
                    maxBufferedInjectDepthSinceLog = bufferedInjectFrames.size();
                    lastInjectTrimLog = now;
                }

                auto recordInjectTargetDrop = [&](QueuedFrame& stale) {
                    DiscardQueuedFrame(stale);
                    g_InjectCadenceDroppedFrames.fetch_add(1, std::memory_order_relaxed);
                    if (g_pSharedMem) {
                        g_pSharedMem->runtimeState.injectCadenceDrops.fetch_add(1, std::memory_order_relaxed);
                    }
                    ++injectTargetSupersededThisWindow;
                    ++injectTargetSupersededTotal;
                };
                auto eligibleInjectFrameCount = [&]() -> size_t {
                    return bufferedInjectFrames.size() > protectedInjectTailFrames
                               ? bufferedInjectFrames.size() - protectedInjectTailFrames
                               : 0;
                };
                auto isFreshInjectCandidate = [&](const QueuedFrame& candidate) {
                    return ce::capture_policy::IsInjectFrameFreshAfterLastEmission(candidate.timestamp,
                                                                                   lastEmittedInjectSourceQpc);
                };

                // Remove only frames that can never be emitted again. Unlike the old wall-age trim,
                // this is relative to committed source lineage and cannot delete an intentional
                // delayed frame merely because the encoder thread is currently later than it.
                while (eligibleInjectFrameCount() > 0 && !isFreshInjectCandidate(bufferedInjectFrames.front())) {
                    QueuedFrame obsolete = std::move(bufferedInjectFrames.front());
                    bufferedInjectFrames.pop_front();
                    recordInjectTargetDrop(obsolete);
                }

                if (!g_EncoderRunning && !bufferedInjectFrames.empty()) {
                    frame = std::move(bufferedInjectFrames.front());
                    bufferedInjectFrames.pop_front();
                    popped = true;
                    lastDeferredLineage = {};
                } else if (!recordingOutputLive || encoderGridStartQpc <= 0 || targetIntervalTicks <= 0) {
                    // Warmup/startup: the readiness gate below builds the content-delay history. Pop
                    // the oldest eligible source so the eventual first live frame is causal.
                    if (eligibleInjectFrameCount() > 0) {
                        frame = std::move(bufferedInjectFrames.front());
                        bufferedInjectFrames.pop_front();
                        popped = true;
                        lastDeferredLineage = {};
                    }
                } else {
                    const size_t availableCount = eligibleInjectFrameCount();
                    const int64_t liveTargetQpc =
                        scheduledOutputQpc > 0
                            ? scheduledOutputQpc
                            : ComputeIdealOutputQpc(encoderGridStartQpc, selectionGridTick, targetIntervalTicks);
                    const int64_t basePlayoutTargetQpc =
                        ComputeDelayedContentGridStartQpc(liveTargetQpc, avContentDelayQpc);
                    const int64_t phaseReferenceQpc =
                        bufferedInjectFrames.empty() ? 0 : bufferedInjectFrames.back().timestamp;
                    const int64_t playoutTargetQpc = applyCaptureSyncPhaseTarget(
                        "inject", injectCfrPhaseLock, basePlayoutTargetQpc, phaseReferenceQpc);
                    const int64_t leadToleranceQpc =
                        ce::capture_policy::GetInjectCfrSelectionLeadToleranceQpc(targetIntervalTicks);
                    auto isAllowedCandidate = [&](const QueuedFrame& candidate) {
                        return isFreshInjectCandidate(candidate) &&
                               !MatchesInjectFrameLineage(candidate, lastDeferredLineage);
                    };
                    size_t bestIdx = SelectFrameClosestToTimestampIf(bufferedInjectFrames, availableCount,
                                                                      playoutTargetQpc, isAllowedCandidate);
                    bool usedDeferredFallback = false;
                    if (bestIdx >= availableCount) {
                        bestIdx = SelectFrameClosestToTimestampIf(bufferedInjectFrames, availableCount,
                                                                  playoutTargetQpc, isFreshInjectCandidate);
                        usedDeferredFallback = lastDeferredLineage.IsValid() && bestIdx < availableCount;
                    }

                    if (bestIdx < availableCount) {
                        const int64_t selectedTimestamp = bufferedInjectFrames[bestIdx].timestamp;
                        const auto decision = ce::capture_policy::DecideCfrNearestPlayout(
                            selectedTimestamp, playoutTargetQpc, leadToleranceQpc, lastEmittedInjectSourceQpc);
                        if (decision.emit) {
                            if (bestIdx > 0) {
                                ++selectionLogCounter;
                                if (selectionLogCounter <= 12 || (selectionLogCounter % 240) == 0) {
                                    LogInfo(
                                        "[EncoderThread] Inject target select tick=%lld targetQpc=%lld chose idx=%zu "
                                        "frame=%u tex=%d ts=%lld oldest=%lld avail=%zu fenceTail=%zu "
                                        "delayFrames=%zu delayUs=%lld%s",
                                        static_cast<long long>(selectionGridTick),
                                        static_cast<long long>(playoutTargetQpc), bestIdx,
                                        bufferedInjectFrames[bestIdx].frameIndex,
                                        bufferedInjectFrames[bestIdx].textureIndex,
                                        static_cast<long long>(selectedTimestamp),
                                        static_cast<long long>(bufferedInjectFrames.front().timestamp), availableCount,
                                        protectedInjectTailFrames, injectContentDelayFrames,
                                        static_cast<long long>(qpcToUs(avContentDelayQpc)),
                                        usedDeferredFallback ? " fallback=deferred-only" : "");
                                }
                            }
                            for (size_t i = 0; i < bestIdx; ++i) {
                                QueuedFrame superseded = std::move(bufferedInjectFrames.front());
                                bufferedInjectFrames.pop_front();
                                recordInjectTargetDrop(superseded);
                            }
                            frame = std::move(bufferedInjectFrames.front());
                            bufferedInjectFrames.pop_front();
                            popped = true;
                            lastDeferredLineage = {};
                            ++injectTargetSelectThisWindow;
                            ++injectTargetSelectTotal;
                            if (qpcFreq.QuadPart > 0) {
                                const uint64_t residualUs =
                                    ce::capture_policy::GetCfrTimestampDistanceQpc(selectedTimestamp,
                                                                                  playoutTargetQpc) *
                                    1000000ull / static_cast<uint64_t>(qpcFreq.QuadPart);
                                injectTargetResidualMaxUs =
                                    std::max(injectTargetResidualMaxUs, SaturatingToUint32(residualUs));
                            }
                        } else if (decision.hold) {
                            ++injectTargetHoldThisWindow;
                            ++injectTargetHoldTotal;
                            ++injectTargetHoldWithCandidateThisWindow;
                            ++injectTargetHoldWithCandidateTotal;
                        }
                    } else {
                        ++injectTargetHoldThisWindow;
                        ++injectTargetHoldTotal;
                    }
                }
            } else {
                // VFR: keep the existing newest-frame sampling for the lowest latency.
                QueuedFrame temp;
                while (g_FrameQueue.Pop(temp, 0)) {
                    if (g_RejectInjectFrames.load(std::memory_order_acquire) && temp.isInjectMode) {
                        DiscardQueuedFrame(temp);
                        continue;
                    }
                    if (!ce::capture_policy::ShouldAcceptFrameForActiveCapturePath(useScreenGrab, temp.isInjectMode)) {
                        discardActivePathMismatchFrame(temp, "inject VFR queue", true);
                        continue;
                    }
                    if (popped && !frame.isInjectMode && frame.texture) {
                        frame.texture->Release();
                    }
                    frame = std::move(temp);
                    popped = true;
                }
            }
        }

        QueuedFrame* frameToProcess = nullptr;
        bool isDuplicate = false;
        bool duplicateFromDeferred = false;
        bool duplicateFromTimerRebase = false;
        bool wantsTrueRepeatLastFrame = false;

        if (popped && !ce::capture_policy::ShouldAcceptFrameForActiveCapturePath(useScreenGrab, frame.isInjectMode)) {
            discardActivePathMismatchFrame(frame, "selected frame", true);
            popped = false;
        }

        const bool canPreserveLastFrameAcrossPathHandoff =
            !config.video.useVFR &&
            ((useScreenGrab && MediaEngine_RepeatLastFrameWithTimeline) || MediaEngine_RepeatLastFrame) &&
            MediaEngine_CanRepeatLastFrame && MediaEngine_CanRepeatLastFrame();
        if (g_HasLastFrame &&
            !ce::capture_policy::ShouldAcceptFrameForActiveCapturePath(useScreenGrab, g_LastFrame.isInjectMode) &&
            !canPreserveLastFrameAcrossPathHandoff) {
            discardActivePathMismatchFrame(g_LastFrame, "cached last frame", false);
            g_HasLastFrame = false;
        }

        if (popped && frame.isInjectMode && g_RejectInjectFrames.load(std::memory_order_acquire)) {
            DiscardQueuedFrame(frame);
            popped = false;
        }

        if (g_HasLastFrame && g_LastFrame.isInjectMode && g_RejectInjectFrames.load(std::memory_order_acquire) &&
            !canPreserveLastFrameAcrossPathHandoff) {
            g_LastFrame = QueuedFrame{};
            g_HasLastFrame = false;
        }

        const bool hasRepeatLastFramePath =
            !config.video.useVFR &&
            ((useScreenGrab && MediaEngine_RepeatLastFrameWithTimeline) || MediaEngine_RepeatLastFrame);
        auto selectCursorStateForScheduledQpc = [&](int64_t scheduledQpc, const QueuedFrame& referenceFrame,
                                                    const char* outputKind) {
            ce::cursor::CaptureState cursorState = referenceFrame.cursorState;
            if (config.video.captureCursor && scheduledQpc > 0) {
                const uint32_t captureWidth = referenceFrame.cursorState.captureWidth != 0
                                                  ? referenceFrame.cursorState.captureWidth
                                                  : referenceFrame.width;
                const uint32_t captureHeight = referenceFrame.cursorState.captureHeight != 0
                                                   ? referenceFrame.cursorState.captureHeight
                                                   : referenceFrame.height;
                const bool cursorEmbedded = useScreenGrab && referenceFrame.wgcCursorEmbedded;
                // Source-embedded ownership belongs to the delayed reference
                // frame, not to this live pointer sample. Publishing it here
                // would delay suppression a second time and hide the cursor
                // after an embedded -> hardware-plane transition.
                const ce::cursor::CaptureState liveState =
                    CaptureCursorSnapshot(scheduledQpc, referenceFrame.captureLeft, referenceFrame.captureTop,
                                          captureWidth, captureHeight, false);
                ce::cursor::Timeline& timeline = useScreenGrab ? g_WgcCursorTimeline : g_InjectCursorTimeline;
                timeline.Publish(liveState);
                const int64_t cursorTargetQpc =
                    useScreenGrab ? std::max<int64_t>(1, scheduledQpc - getWgcEffectiveContentDelayQpc())
                                  : scheduledQpc;
                if (!timeline.SelectAtOrBefore(cursorTargetQpc, &cursorState)) {
                    cursorState = liveState;
                }
                // Pixel ownership is authoritative for exactly this selected
                // source frame. Remove stale source ownership from the cursor
                // history when the selected frame has a separate hardware
                // pointer, while preserving a real CURSOR_SUPPRESSED state.
                cursorState.SetSourceEmbedded(cursorEmbedded);

                static uint64_t s_cursorTimelineLogCount = 0;
                ++s_cursorTimelineLogCount;
                if (s_cursorTimelineLogCount <= 5 || (s_cursorTimelineLogCount % 600ull) == 0ull) {
                    LogInfo(
                        "[Cursor] CFR timeline backend=%s output=%s scheduled=%lld target=%lld source=%lld "
                        "selected=%lld observed=%lld deltaUs=%lld dpi=%u size=%ux%u bounds=(%d,%d %ux%u) "
                        "visible=%d embedded=%d fallback=%d coord=%s",
                        useScreenGrab ? "screen-grab" : "inject", outputKind ? outputKind : "unknown",
                        static_cast<long long>(scheduledQpc), static_cast<long long>(cursorTargetQpc),
                        static_cast<long long>(referenceFrame.cursorState.associationQpc),
                        static_cast<long long>(cursorState.associationQpc),
                        static_cast<long long>(cursorState.observedQpc),
                        static_cast<long long>(qpcFreq.QuadPart > 0
                                                   ? ((cursorTargetQpc - cursorState.associationQpc) * 1000000) /
                                                         qpcFreq.QuadPart
                                                   : 0),
                        cursorState.dpi, cursorState.requestedWidth, cursorState.requestedHeight,
                        cursorState.captureLeft, cursorState.captureTop, cursorState.captureWidth,
                        cursorState.captureHeight, cursorState.IsVisible() ? 1 : 0,
                        cursorState.IsSourceEmbedded() ? 1 : 0,
                        (cursorState.flags & ce::cursor::kStateHandleVisibilityFallback) != 0 ? 1 : 0,
                        cursorState.PositionIsShapeTopLeft() ? "shape-top-left" : "hotspot");
                }
            }
            return cursorState;
        };
        auto evaluateScreenGrabPrivacy = [&](const QueuedFrame* freshFrame) {
            if (!privacyRuntime.IsEnabled()) {
                return ce::screen_grab_privacy::GateDecision{};
            }
            HWND targetWindow = nullptr, confirmedWindow = nullptr;
            HMONITOR targetMonitor = nullptr, confirmedMonitor = nullptr;
            const auto captureBefore = g_WgcCap.Read();
            if (captureBefore) {
                captureBefore->GetTargetIdentity(&targetWindow, &targetMonitor);
            }
            const auto focus = ce::screen_grab_privacy::CaptureStableFullscreenFocus();
            const auto captureAfter = g_WgcCap.Read();
            if (captureAfter) {
                captureAfter->GetTargetIdentity(&confirmedWindow, &confirmedMonitor);
            }
            const bool stableCaptureTarget = captureBefore && captureAfter &&
                                             captureBefore.get() == captureAfter.get() &&
                                             targetWindow == confirmedWindow && targetMonitor == confirmedMonitor;
            const int64_t freshFrameQpc =
                freshFrame ? (freshFrame->rawTimestamp > 0 ? freshFrame->rawTimestamp : freshFrame->timestamp) : 0;
            return privacyRuntime.Evaluate(useScreenGrab, targetWindow, targetMonitor, stableCaptureTarget, focus,
                                           freshFrame != nullptr, freshFrameQpc);
        };
        auto requestPrivacyFailClosedStop = [&](const char* reason) {
            LogError(
                "[PrivacyBlackout] FAIL-CLOSED: %s; cached video is invalidated and recording will stop rather than "
                "expose captured pixels",
                reason ? reason : "opaque-black submission failed");
            privacyRuntime.ResetSource();
            g_PrivacyFailClosedStopRequested.store(true, std::memory_order_release);
            g_EncoderRunning.store(false, std::memory_order_release);
        };
        auto submitPrivacyBlackFrame = [&](const QueuedFrame& referenceFrame, int64_t mediaTimestampQpc,
                                           int64_t scheduledQpc, int64_t timelineElapsedUs) {
            if (!privacyRuntime.SubmitBlack(referenceFrame.texture, referenceFrame.isHDR, mediaTimestampQpc,
                                            scheduledQpc, timelineElapsedUs, useScreenGrab && !config.video.useVFR)) {
                requestPrivacyFailClosedStop("opaque-black frame submission failed");
                return false;
            }
            return true;
        };
        auto repeatLastFrameForScheduledQpc = [&](int64_t scheduledQpc) {
            if (useScreenGrab && privacyRuntime.IsEnabled()) {
                const auto privacyDecision = evaluateScreenGrabPrivacy(nullptr);
                if (privacyDecision.useBlackFrame) {
                    if (!g_HasLastFrame || g_LastFrame.isInjectMode) {
                        requestPrivacyFailClosedStop("no screen-grab texture is available for an opaque-black tick");
                        return false;
                    }
                    return submitPrivacyBlackFrame(g_LastFrame, scheduledQpc,
                                                   scheduledQpc, computeLiveTimelineElapsedUs(scheduledQpc));
                }
            }
            ce::cursor::CaptureState cursorState;
            if (config.video.captureCursor && g_HasLastFrame && !privacyRuntime.RepeatCacheIsBlack()) {
                cursorState = selectCursorStateForScheduledQpc(scheduledQpc, g_LastFrame, "repeat");
            }
            bool succeeded = false;
            if (useScreenGrab && !config.video.useVFR && MediaEngine_RepeatLastFrameWithTimeline) {
                succeeded = MediaEngine_RepeatLastFrameWithTimeline(
                    scheduledQpc, computeLiveTimelineElapsedUs(scheduledQpc), &cursorState);
            } else {
                succeeded = MediaEngine_RepeatLastFrame && MediaEngine_RepeatLastFrame(scheduledQpc, &cursorState);
            }
            if (succeeded && useScreenGrab && privacyRuntime.IsEnabled()) {
                privacyRuntime.CommitRepeatOutput();
            }
            return succeeded;
        };
        auto recoverScheduledFreshEncodeFailure = [&](bool scheduledCfrTick, bool freshEncodeSucceeded,
                                                      bool freshEncodeDeferred, int64_t scheduledQpc,
                                                      const QueuedFrame* failedFrame, const char* context) {
            const bool repeatCacheAvailable = MediaEngine_CanRepeatLastFrame && MediaEngine_CanRepeatLastFrame();
            if (!ce::capture_policy::ShouldRepeatAfterScheduledFreshEncodeFailure(
                    scheduledCfrTick, freshEncodeSucceeded, freshEncodeDeferred, hasRepeatLastFramePath,
                    repeatCacheAvailable)) {
                return false;
            }

            // The failed WGC attempt may have changed cursor suppression to
            // match pixels that were never emitted. Restore the metadata that
            // belongs to the cached successful source frame before repeating.
            if (failedFrame && !failedFrame->isInjectMode && hasSuccessfulWgcCursorMetadata) {
                SyncDuplicationCursorSuppression(lastSuccessfulWgcCursorEmbedded);
            }

            const bool repeatSucceeded = repeatLastFrameForScheduledQpc(scheduledQpc);
            const bool repeatDeferred = MediaEngine_WasLastFrameDeferred && MediaEngine_WasLastFrameDeferred();
            if (!repeatSucceeded || repeatDeferred) {
                LogWarn(
                    "[EncoderThread] CFR fresh encode failed and cached-repeat recovery also failed: context=%s "
                    "scheduledQpc=%lld repeatSucceeded=%d repeatDeferred=%d",
                    context ? context : "unknown", static_cast<long long>(scheduledQpc), repeatSucceeded ? 1 : 0,
                    repeatDeferred ? 1 : 0);
                return false;
            }

            static uint64_t s_freshEncodeRecoveryCount = 0;
            ++s_freshEncodeRecoveryCount;
            if (s_freshEncodeRecoveryCount <= 5 || (s_freshEncodeRecoveryCount % 120ull) == 0ull) {
                LogWarn(
                    "[EncoderThread] CFR fresh encode failure recovered with cached duplicate: context=%s "
                    "scheduledQpc=%lld recoveryCount=%llu",
                    context ? context : "unknown", static_cast<long long>(scheduledQpc),
                    static_cast<unsigned long long>(s_freshEncodeRecoveryCount));
            }
            return true;
        };
        auto releaseWgcLeaseAfterMediaEngineCopy = [&](QueuedFrame& encodedFrame, const char* context) {
            if (encodedFrame.isInjectMode || !encodedFrame.wgcPoolLease.IsValid()) {
                return;
            }
            if (!hasRepeatLastFramePath) {
                static bool s_loggedHeldForFallback = false;
                if (!s_loggedHeldForFallback) {
                    LogWarn(
                        "[WGC] Holding encoded pool slot lease because media-engine repeat cache is unavailable "
                        "(slot=%u generation=%llu context=%s). Pool pressure can rise on fallback duplicate paths.",
                        encodedFrame.wgcPoolSlot, static_cast<unsigned long long>(encodedFrame.wgcPoolGeneration),
                        context);
                    s_loggedHeldForFallback = true;
                }
                return;
            }

            const uint32_t slot = encodedFrame.wgcPoolSlot;
            const uint64_t generation = encodedFrame.wgcPoolGeneration;
            encodedFrame.wgcPoolLease.Reset();
            encodedFrame.wgcPoolSlot = std::numeric_limits<uint32_t>::max();
            encodedFrame.wgcPoolGeneration = 0;

            static uint64_t s_releaseLogCount = 0;
            ++s_releaseLogCount;
            if (s_releaseLogCount <= 5 || (s_releaseLogCount % 1000ull) == 0ull) {
                LogInfo(
                    "[WGC] Pool slot lease released after media-engine copy: slot=%u generation=%llu context=%s "
                    "releaseCount=%llu",
                    slot, static_cast<unsigned long long>(generation), context,
                    static_cast<unsigned long long>(s_releaseLogCount));
            }
        };
        const bool warmupCaptureModeChanged = ce::capture_policy::ResetWarmupOnCaptureModeChange(
            recordingOutputLive, useScreenGrab, GetTickCount64(), warmupState);
        if (warmupCaptureModeChanged || !useScreenGrab) {
            ResetWarmupWgcFreshness();
            wgcLowSourceModeActive = false;
            wgcLowSourceStateChangeTick = 0;
            wgcLiveRecoveryModeActive = false;
            wgcLiveRecoveryStateChangeTick = 0;
            wgcSourceStarvedCurrent = false;
            wgcSchedulerLimitedCurrent = false;
            wgcEncoderRecoveryLimitedCurrent = false;
        }
        startupWarmupStartTick = warmupState.startupWarmupStartTick;
        hiddenStartupFrames = warmupState.hiddenStartupFrames;
        const size_t injectReserveFrames = (!useScreenGrab)
                                               ? ce::capture_policy::GetInjectReserveFrames(
                                                     config.video.useVFR, smoothedInjectFenceMs, frameIntervalMs)
                                               : 0;
        if (!recordingOutputLive && !pendingLiveActivation && g_Recording && g_EncoderRunning) {
            const uint64_t warmupElapsedMs64 = GetTickCount64() - startupWarmupStartTick;
            const DWORD warmupElapsedMs =
                warmupElapsedMs64 > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<DWORD>(warmupElapsedMs64);
            const bool warmupReady = useScreenGrab
                                         ? ce::capture_policy::ShouldCommitWgcWarmup(
                                               popped, bufferedWgcFrames.size(), warmupElapsedMs,
                                               smoothedInputPerTick * static_cast<double>(config.video.fps),
                                               static_cast<uint32_t>(config.video.fps))
                                         : ce::capture_policy::ShouldCommitRecordingWarmup(
                                               useScreenGrab, config.video.useVFR, popped, !bufferedWgcFrames.empty(),
                                               bufferedInjectFrames.size(), injectReserveFrames, warmupElapsedMs);
            const bool warmupFreshEnough =
                !useScreenGrab || wgcFreshWarmupFrameCount >= ce::capture_policy::kWgcWarmupFreshFrames;
            if (warmupReady && warmupFreshEnough) {
                const bool wgcCfrStartupSync = ce::capture_policy::ShouldUseWgcCfrStartupSyncBarrier(
                    useScreenGrab, config.video.useVFR, targetIntervalTicks);
                if (wgcCfrStartupSync) {
                    if (!wgcStartupPreLiveDelayComplete) {
                        if (popped) {
                            TrackWarmupWgcFreshFrame(frame);
                            ++hiddenStartupFrames;
                            ++wgcStartupPreLiveDelayDroppedFrames;
                            warmupState.hiddenStartupFrames = hiddenStartupFrames;
                            DiscardQueuedFrame(frame);
                        }

                        const uint32_t smoothnessStartupDesiredFrames = getWgcSmoothnessDesiredFramesForConfig();
                        const uint32_t smoothnessStartupRetainedFrames = getWgcSmoothnessRetainedFramesBudget();
                        const bool smoothnessStartupAttempt = shouldAttemptWgcStartupSmoothnessBufferNow();
                        const int64_t delayTicks =
                            ce::capture_policy::GetWgcCfrStartupPreLiveDelayTicks(targetIntervalTicks);
                        updateWgcIngressPressure("startup-pre-live-delay");
                        if (hTimer && delayTicks > 0 && qpcFreq.QuadPart > 0) {
                            const int64_t delay100ns = (delayTicks * 10000000) / qpcFreq.QuadPart;
                            LARGE_INTEGER dueTime;
                            dueTime.QuadPart = -delay100ns;
                            if (SetWaitableTimer(hTimer, &dueTime, 0, NULL, NULL, FALSE)) {
                                WaitForSingleObject(hTimer, INFINITE);
                            }
                        }

                        QueuedFrame qf;
                        size_t queueFlushed = 0;
                        while (g_FrameQueue.Pop(qf, 0)) {
                            if (qf.isInjectMode)
                                DiscardQueuedFrame(qf);
                            else if (qf.texture)
                                ReleaseQueuedFrameTexture(qf);
                            queueFlushed++;
                        }
                        size_t bufferedFlushed = 0;
                        if (!bufferedWgcFrames.empty()) {
                            bufferedFlushed = bufferedWgcFrames.size();
                            ClearBufferedWgcFrames();
                        }
                        updateWgcIngressPressure("startup-post-delay-flush");

                        LARGE_INTEGER barrierNow;
                        QueryPerformanceCounter(&barrierNow);
                        wgcStartupBarrierQpc =
                            ce::capture_policy::GetWgcStartupBarrierQpc(barrierNow.QuadPart, targetIntervalTicks);
                        wgcStartupBarrierDroppedFrames = 0;
                        wgcStartupPreLiveDelayComplete = true;
                        const uint64_t warmupElapsedWithDelayMs64 = GetTickCount64() - startupWarmupStartTick;
                        LogInfo(
                            "[EncoderThread] WGC startup pre-live delay complete: anchorQpc=%lld now=%lld "
                            "oneFrame=%lld delayTicks=%lld hiddenFrames=%u discarded=%u queueFlushed=%zu "
                            "bufferedFlushed=%zu smoothAttempt=%d smoothDesiredFrames=%u "
                            "smoothnessRetainedFrames=%u smoothPreLiveDelayTicks=0 smoothReason=%s warmupMs=%llu",
                            static_cast<long long>(wgcStartupBarrierQpc), static_cast<long long>(barrierNow.QuadPart),
                            static_cast<long long>(targetIntervalTicks), static_cast<long long>(delayTicks),
                            hiddenStartupFrames, wgcStartupPreLiveDelayDroppedFrames, queueFlushed, bufferedFlushed,
                            smoothnessStartupAttempt ? 1 : 0, smoothnessStartupDesiredFrames,
                            smoothnessStartupRetainedFrames, getWgcSmoothnessBufferReason(),
                            static_cast<unsigned long long>(warmupElapsedWithDelayMs64));
                        continue;
                    }

                    if (wgcStartupBarrierQpc <= 0) {
                        LARGE_INTEGER barrierNow;
                        QueryPerformanceCounter(&barrierNow);
                        wgcStartupBarrierQpc =
                            ce::capture_policy::GetWgcStartupBarrierQpc(barrierNow.QuadPart, targetIntervalTicks);
                        LogInfo(
                            "[EncoderThread] WGC startup sync post-delay barrier armed: anchorQpc=%lld now=%lld "
                            "oneFrame=%lld",
                            static_cast<long long>(wgcStartupBarrierQpc), static_cast<long long>(barrierNow.QuadPart),
                            static_cast<long long>(targetIntervalTicks));
                    }

                    if (!popped || frame.isInjectMode ||
                        !ce::capture_policy::IsWgcFramePastStartupBarrier(frame.timestamp, wgcStartupBarrierQpc)) {
                        if (popped) {
                            TrackWarmupWgcFreshFrame(frame);
                            ++hiddenStartupFrames;
                            ++wgcStartupBarrierDroppedFrames;
                            warmupState.hiddenStartupFrames = hiddenStartupFrames;
                            DiscardQueuedFrame(frame);
                        }
                        continue;
                    }

                    // Initialize codec/device/mux once WGC/DXGI format is known, then
                    // discard prewarm-era pool contents and arm the real barrier.
                    if (!wgcEncoderPrewarmAttempted) {
                        wgcEncoderPrewarmAttempted = true;
                        LARGE_INTEGER prewarmStartQpc = {};
                        LARGE_INTEGER prewarmEndQpc = {};
                        QueryPerformanceCounter(&prewarmStartQpc);
                        const bool privacyTextureReady = privacyRuntime.PrepareTexture(frame.texture);
                        wgcEncoderPrewarmSucceeded =
                            privacyTextureReady && MediaEngine_PrepareFrameD3D11 &&
                            MediaEngine_PrepareFrameD3D11(frame.texture, frame.width, frame.height, frame.isHDR);
                        QueryPerformanceCounter(&prewarmEndQpc);
                        wgcEncoderPrewarmElapsedUs = qpcToUs(prewarmEndQpc.QuadPart - prewarmStartQpc.QuadPart);
                        LogInfo(
                            "[EncoderThread] WGC transactional video prewarm %s: elapsed=%lldus frameQpc=%lld "
                            "dimensions=%ux%u hdr=%d queuedAfter=%zu bufferedAfter=%zu; timeline remains uncommitted",
                            wgcEncoderPrewarmSucceeded ? "complete" : "FAILED",
                            static_cast<long long>(wgcEncoderPrewarmElapsedUs), static_cast<long long>(frame.timestamp),
                            frame.width, frame.height, frame.isHDR ? 1 : 0, g_FrameQueue.Size(),
