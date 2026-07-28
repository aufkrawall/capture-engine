                            bufferedWgcFrames.size());
                        if (!privacyTextureReady) {
                            LogError(
                                "[PrivacyBlackout] Failed to create the GPU opaque-black texture for %ux%u; "
                                "recording start is rejected rather than exposing captured pixels",
                                frame.width, frame.height);
                        }
                        if (!wgcEncoderPrewarmSucceeded) {
                            PublishRecordingStartFailure(RecordingFailureCode::RecordingStartFailed,
                                                         "WGC deferred encoder/mux initialization");
                            g_EncoderRunning = false;
                            DiscardQueuedFrame(frame);
                            break;
                        }

                        // Restart after a long prewarm so live begins with fresh pool history.
                        size_t prewarmEraDiscarded = 1;
                        DiscardQueuedFrame(frame);
                        frame = {};
                        QueuedFrame prewarmEraFrame;
                        while (g_FrameQueue.Pop(prewarmEraFrame, 0)) {
                            DiscardQueuedFrame(prewarmEraFrame);
                            prewarmEraFrame = {};
                            ++prewarmEraDiscarded;
                        }
                        prewarmEraDiscarded += bufferedWgcFrames.size();
                        ClearBufferedWgcFrames();
                        wgcStartupBarrierDroppedFrames += SaturatingToUint32(prewarmEraDiscarded);
                        wgcStartupReserveWaitStartQpc = 0;
                        wgcStartupReserveWaitCount = 0;
                        wgcStartupReserveWaitInitialSpanUs = 0;
                        wgcStartupReserveWaitFreshenedMax = 0;
                        LARGE_INTEGER postPrewarmNow = {};
                        QueryPerformanceCounter(&postPrewarmNow);
                        wgcStartupBarrierQpc = ce::capture_policy::GetWgcStartupBarrierQpc(
                            postPrewarmNow.QuadPart, targetIntervalTicks);
                        updateWgcIngressPressure("startup-post-prewarm-refresh");
                        LogInfo(
                            "[EncoderThread] WGC startup barrier refreshed after transactional prewarm: "
                            "anchorQpc=%lld now=%lld discardedPrewarmEra=%zu elapsed=%lldus",
                            static_cast<long long>(wgcStartupBarrierQpc),
                            static_cast<long long>(postPrewarmNow.QuadPart), prewarmEraDiscarded,
                            static_cast<long long>(wgcEncoderPrewarmElapsedUs));
                        continue;
                    }

                    size_t startupBufferedExamined = 0;
                    size_t startupQueueExamined = 0;
                    size_t startupFreshened = 0;
                    size_t startupDiscardedOlder = 0;
                    size_t startupDiscardedBeforeBarrier = 0;
                    size_t startupDiscardedPathMismatch = 0;
                    struct StartupWgcCandidate {
                        QueuedFrame frame;
                        size_t sequence = 0;
                    };
                    std::vector<StartupWgcCandidate> startupCandidates;
                    startupCandidates.reserve(1 + bufferedWgcFrames.size() + 8);
                    size_t startupCandidateSequence = 0;
                    const int64_t initialStartupSelectionQpc = GetFrameSelectionTimestamp(frame);
                    auto considerStartupWgcCandidate = [&](QueuedFrame candidate, bool fromQueue,
                                                           bool initialCandidate = false) {
                        if (fromQueue) {
                            ++startupQueueExamined;
                        } else if (!initialCandidate) {
                            ++startupBufferedExamined;
                        }

                        if (candidate.isInjectMode) {
                            ++startupDiscardedPathMismatch;
                            DiscardQueuedFrame(candidate);
                            return;
                        }

                        if (!ce::capture_policy::IsWgcFramePastStartupBarrier(candidate.timestamp,
                                                                              wgcStartupBarrierQpc)) {
                            ++startupDiscardedBeforeBarrier;
                            ++wgcStartupBarrierDroppedFrames;
                            ReleaseQueuedFrameTexture(candidate);
                            return;
                        }

                        StartupWgcCandidate startupCandidate;
                        startupCandidate.frame = std::move(candidate);
                        startupCandidate.sequence = startupCandidateSequence++;
                        startupCandidates.push_back(std::move(startupCandidate));
                    };

                    considerStartupWgcCandidate(std::move(frame), false, true);

                    while (!bufferedWgcFrames.empty()) {
                        QueuedFrame candidate = std::move(bufferedWgcFrames.front());
                        bufferedWgcFrames.pop_front();
                        considerStartupWgcCandidate(std::move(candidate), false);
                    }

                    QueuedFrame queuedStartupCandidate;
                    while (g_FrameQueue.Pop(queuedStartupCandidate, 0)) {
                        considerStartupWgcCandidate(std::move(queuedStartupCandidate), true);
                        queuedStartupCandidate = QueuedFrame{};
                    }

                    std::stable_sort(startupCandidates.begin(), startupCandidates.end(),
                                     [&](const StartupWgcCandidate& lhs, const StartupWgcCandidate& rhs) {
                                         const int64_t lhsSelectionQpc = GetFrameSelectionTimestamp(lhs.frame);
                                         const int64_t rhsSelectionQpc = GetFrameSelectionTimestamp(rhs.frame);
                                         if (lhsSelectionQpc != rhsSelectionQpc) {
                                             return lhsSelectionQpc < rhsSelectionQpc;
                                         }
                                         if (lhs.frame.timestamp != rhs.frame.timestamp) {
                                             return lhs.frame.timestamp < rhs.frame.timestamp;
                                         }
                                         return lhs.sequence < rhs.sequence;
                                     });

                    std::vector<int64_t> startupSelectionQpcs;
                    startupSelectionQpcs.reserve(startupCandidates.size());
                    for (const auto& candidate : startupCandidates) {
                        startupSelectionQpcs.push_back(GetFrameSelectionTimestamp(candidate.frame));
                        if (GetFrameSelectionTimestamp(candidate.frame) > initialStartupSelectionQpc) {
                            ++startupFreshened;
                        }
                    }

                    const uint32_t smoothnessDesiredFrames = getWgcSmoothnessDesiredFramesForConfig();
                    const uint32_t smoothnessRetainedFrames =
                        (g_WgcCap && smoothnessDesiredFrames > 0) ? g_WgcCap->GetSmoothnessRetainedFrameCount() : 0u;
                    const bool smoothnessStartupAttempted = ce::capture_policy::ShouldAttemptWgcStartupSmoothnessBuffer(
                        config.wgcSmoothnessBufferEnabled, config.video.useVFR, wgcSmoothnessDelayDesired,
                        targetIntervalTicks, smoothnessRetainedFrames);
                    const uint32_t smoothnessPoolSlots =
                        g_WgcCap ? g_WgcCap->GetTexturePoolSlotCount()
                                 : ce::capture_policy::GetWgcSmoothnessPoolFrameCount(smoothnessRetainedFrames);
                    const uint32_t smoothnessRetainedFrameCap =
                        g_WgcCap ? g_WgcCap->GetSmoothnessRetainedFrameCap() : smoothnessPoolSlots;
                    const uint32_t smoothnessReservedFreeSlots =
                        g_WgcCap ? g_WgcCap->GetSmoothnessReservedFreeSlotCount()
                                 : ce::capture_policy::kWgcSmoothnessBufferPoolSafetyFrames;
                    const uint64_t smoothnessEstimatedVramBytes =
                        g_WgcCap ? g_WgcCap->GetSmoothnessEstimatedVramBytes() : 0ull;
                    const bool smoothnessCapLimited =
                        smoothnessDesiredFrames > 0 && smoothnessRetainedFrames < smoothnessDesiredFrames;
                    // Full buildable reservoir target (audio-latency path uses this unchanged).
                    const int64_t smoothnessReservoirTargetDelayQpc =
                        getWgcStartupSmoothnessTargetDelayQpc(smoothnessStartupAttempted);
                    // Resolve and lock the jitter/config floor once; the audio-latency
                    // reservoir already dominates it when active.
                    if (wgcSmoothnessFloorConfigured && smoothnessStartupAttempted &&
                        smoothnessReservoirTargetDelayQpc > 0) {
                        if (g_WgcCap) {
                            wgcSmoothnessFloorJitter.deliveryGapAvgUs =
                                SaturatingToUint32(g_WgcCap->GetCallbackGapAvgUs());
                            wgcSmoothnessFloorJitter.deliveryGapMaxUs =
                                SaturatingToUint32(g_WgcCap->GetCallbackGapMaxUs());
                            wgcSmoothnessFloorJitter.sourceJitterAvgUs =
                                SaturatingToUint32(g_WgcCap->GetSourceJitterAvgUs());
                            wgcSmoothnessFloorJitter.sourceJitterMaxUs =
                                SaturatingToUint32(g_WgcCap->GetSourceJitterMaxUs());
                        }
                        if (config.wgcSmoothnessFloorAuto) {
                            wgcSmoothnessFloorSource = "auto";
                            wgcSmoothnessFloorRequestedQpc = ce::capture_policy::DeriveWgcSmoothnessFloorDelayQpc(
                                wgcSmoothnessFloorJitter, targetIntervalTicks, qpcFreq.QuadPart,
                                config.wgcSmoothnessBufferMaxMs, smoothnessRetainedFrames);
                            wgcSmoothnessFloorDelayQpc = wgcSmoothnessFloorRequestedQpc;
                        } else {
                            wgcSmoothnessFloorSource = "config";
                            wgcSmoothnessFloorRequestedQpc =
                                qpcFreq.QuadPart > 0
                                    ? (qpcFreq.QuadPart * static_cast<int64_t>(config.wgcSmoothnessFloorMs)) / 1000
                                    : 0;
                            wgcSmoothnessFloorDelayQpc = ce::capture_policy::ClampWgcSmoothnessFloorDelayQpc(
                                wgcSmoothnessFloorRequestedQpc, targetIntervalTicks, qpcFreq.QuadPart,
                                config.wgcSmoothnessBufferMaxMs, smoothnessRetainedFrames);
                        }
                        const int64_t floorCapQpc = ce::capture_policy::GetWgcSmoothnessFloorCapQpc(
                            targetIntervalTicks, qpcFreq.QuadPart, config.wgcSmoothnessBufferMaxMs,
                            smoothnessRetainedFrames);
                        const int64_t floorMinQpc =
                            targetIntervalTicks *
                            static_cast<int64_t>(ce::capture_policy::kWgcSmoothnessFloorMinFrames);
                        if (wgcSmoothnessFloorRequestedQpc >= floorCapQpc && floorCapQpc > 0) {
                            const int64_t maxMsQpc =
                                config.wgcSmoothnessBufferMaxMs > 0 && qpcFreq.QuadPart > 0
                                    ? (qpcFreq.QuadPart * static_cast<int64_t>(config.wgcSmoothnessBufferMaxMs)) / 1000
                                    : INT64_MAX;
                            wgcSmoothnessFloorClampedBy = (maxMsQpc <= floorCapQpc) ? "max_ms" : "reservoir";
                        } else if (wgcSmoothnessFloorRequestedQpc < floorMinQpc) {
                            wgcSmoothnessFloorClampedBy = "min";
                        } else {
                            wgcSmoothnessFloorClampedBy = "none";
                        }
                    }
                    // Without audio latency, use only the smaller jitter-sized floor.
                    const int64_t smoothnessTargetDelayQpc =
                        avContentDelayActive ? smoothnessReservoirTargetDelayQpc
                                             : std::min(wgcSmoothnessFloorDelayQpc, smoothnessReservoirTargetDelayQpc);
                    const int64_t startupContentDelayTargetQpc =
                        avContentDelayQpc + std::max<int64_t>(0, smoothnessTargetDelayQpc);
                    wgcSmoothnessDesiredFrames = smoothnessDesiredFrames;
                    wgcSmoothnessRetainedFrames = smoothnessRetainedFrames;
                    wgcSmoothnessPoolSlots = smoothnessPoolSlots;
                    wgcSmoothnessRetainedFrameCap = smoothnessRetainedFrameCap;
                    wgcSmoothnessReservedFreeSlots = smoothnessReservedFreeSlots;
                    wgcSmoothnessEstimatedVramBytes = smoothnessEstimatedVramBytes;
                    wgcSmoothnessCapLimited = smoothnessCapLimited;
                    wgcSmoothnessBufferReason = getWgcSmoothnessBufferReason();
                    if (smoothnessDesiredFrames > 0 && smoothnessRetainedFrames == 0) {
                        wgcSmoothnessBufferReason = "vram_budget_exhausted";
                    } else if (smoothnessCapLimited) {
                        wgcSmoothnessBufferReason = "vram_cap_limited";
                    }

                    // Log the smoothness-floor inputs, clamp, effective target, and
                    // with-audio no-op once so the startup decision stays auditable.
                    if (wgcSmoothnessFloorConfigured && !wgcSmoothnessFloorLogged) {
                        // This runs on each reserve re-evaluation, but the result is stable.
                        wgcSmoothnessFloorLogged = true;
                        const char* floorNote = avContentDelayActive
                                                    ? "no-op: audio-latency reservoir target dominates"
                                                    : (smoothnessReservoirTargetDelayQpc > 0
                                                           ? "active: video-only/low-confidence jitter buffer"
                                                           : "inactive: no reservoir capacity");
                        LogInfo(
                            "[AVSyncApply] wgc_smoothness_floor: source=%s auto=%d configuredMs=%u "
                            "deliveryGapUs(avg/max)=%u/%u sourceJitterUs(avg/max)=%u/%u requestedUs=%lld "
                            "derivedUs=%lld clampedBy=%s reservoirTargetUs=%lld effectiveTargetUs=%lld "
                            "avContentDelayUs=%lld note=\"%s\"",
                            wgcSmoothnessFloorSource, config.wgcSmoothnessFloorAuto ? 1 : 0,
                            config.wgcSmoothnessFloorMs, wgcSmoothnessFloorJitter.deliveryGapAvgUs,
                            wgcSmoothnessFloorJitter.deliveryGapMaxUs, wgcSmoothnessFloorJitter.sourceJitterAvgUs,
                            wgcSmoothnessFloorJitter.sourceJitterMaxUs,
                            static_cast<long long>(qpcToUs(wgcSmoothnessFloorRequestedQpc)),
                            static_cast<long long>(qpcToUs(wgcSmoothnessFloorDelayQpc)), wgcSmoothnessFloorClampedBy,
                            static_cast<long long>(qpcToUs(smoothnessReservoirTargetDelayQpc)),
                            static_cast<long long>(qpcToUs(smoothnessTargetDelayQpc)),
                            static_cast<long long>(qpcToUs(avContentDelayQpc)), floorNote);
                    }

                    const int64_t startupReserveToleranceQpc =
                        qpcFreq.QuadPart > 0
                            ? std::min<int64_t>(targetIntervalTicks > 0 ? (targetIntervalTicks / 2) : 0,
                                                qpcFreq.QuadPart / 200)
                            : 0;
                    const auto startupReserveSelection = ce::capture_policy::SelectWgcStartupReserveCandidate(
                        startupSelectionQpcs.empty() ? nullptr : startupSelectionQpcs.data(),
                        startupSelectionQpcs.size(),
                        startupContentDelayTargetQpc > 0 ? startupContentDelayTargetQpc : 0,
                        startupReserveToleranceQpc);
                    size_t selectedStartupIndex = startupReserveSelection.selectedIndex;
                    if (selectedStartupIndex >= startupCandidates.size()) {
                        selectedStartupIndex = startupCandidates.empty() ? 0 : (startupCandidates.size() - 1);
                    }

                    const auto qpcDeltaToUs = [&](int64_t qpcDelta) -> int64_t {
                        return qpcFreq.QuadPart > 0 ? (qpcDelta * 1000000) / qpcFreq.QuadPart : 0;
                    };
                    wgcStartupReserveFrames = SaturatingToUint32(startupCandidates.size());
                    wgcStartupReserveSpanUs = qpcDeltaToUs(startupReserveSelection.reserveSpanQpc);
                    wgcStartupDelayTargetUs =
                        qpcDeltaToUs(startupContentDelayTargetQpc > 0 ? startupContentDelayTargetQpc : 0);
                    wgcStartupSelectedByDelayReserve = startupReserveSelection.usedDelayReserve;
                    if (startupContentDelayTargetQpc <= 0) {
                        wgcStartupReserveReason = "inactive";
                    } else if (startupCandidates.size() < 2) {
                        wgcStartupReserveReason = "insufficient_frames";
                    } else if (startupReserveSelection.usedDelayReserve) {
                        wgcStartupReserveReason = "selected";
                    } else {
                        wgcStartupReserveReason = "insufficient_span";
                    }

                    const size_t newerStartupReserveFrames = selectedStartupIndex < startupCandidates.size()
                                                                 ? (startupCandidates.size() - selectedStartupIndex - 1)
                                                                 : 0;
                    const bool startupReserveBelowLowWater =
                        startupContentDelayTargetQpc > 0 && startupReserveSelection.usedDelayReserve &&
                        newerStartupReserveFrames <
                            getWgcDelayReservoirLowWaterFramesForDelay(startupContentDelayTargetQpc);
                    const bool startupReserveMissing =
                        startupContentDelayTargetQpc > 0 &&
                        (!startupReserveSelection.usedDelayReserve || startupReserveBelowLowWater);
                    if (startupReserveMissing && targetIntervalTicks > 0 && qpcFreq.QuadPart > 0) {
                        LARGE_INTEGER waitNow;
                        QueryPerformanceCounter(&waitNow);
                        if (wgcStartupReserveWaitStartQpc <= 0) {
                            wgcStartupReserveWaitStartQpc = waitNow.QuadPart;
                            wgcStartupReserveWaitInitialSpanUs = wgcStartupReserveSpanUs;
                        }
                        wgcStartupReserveWaitFreshenedMax =
                            std::max<uint32_t>(wgcStartupReserveWaitFreshenedMax, SaturatingToUint32(startupFreshened));
                        const int64_t waitBudgetQpc = ce::capture_policy::GetWgcStartupReserveWaitBudgetQpc(
                            startupContentDelayTargetQpc, targetIntervalTicks, smoothnessTargetDelayQpc,
                            smoothnessStartupAttempted, qpcFreq.QuadPart);
                        const bool waitBudgetRemaining =
                            waitNow.QuadPart - wgcStartupReserveWaitStartQpc < waitBudgetQpc;
                        if (waitBudgetRemaining) {
                            ++wgcStartupReserveWaitCount;
                            for (auto& candidate : startupCandidates) {
                                if (candidate.frame.texture || candidate.frame.sharedHandle ||
                                    candidate.frame.timestamp > 0) {
                                    bufferedWgcFrames.push_back(std::move(candidate.frame));
                                }
                            }
                            trimBufferedWgcStartupWaitToRetainedCap("startup-wait");
                            wgcStartupReserveReason =
                                startupReserveBelowLowWater ? "waiting_low_water" : "waiting_span";
                            if (wgcStartupReserveWaitCount <= 3 || (wgcStartupReserveWaitCount % 30u) == 0u) {
                                LogInfo(
                                    "[EncoderThread] WGC startup delay-reserve wait: reason=%s candidates=%zu "
                                    "newer=%zu lowWater=%u target=%u span=%lldus initialSpan=%lldus "
                                    "freshened=%u waited=%lldus budget=%lldus smoothAttempt=%d "
                                    "smoothFrames=%u/%u capLimited=%d",
                                    wgcStartupReserveReason.c_str(), startupCandidates.size(),
                                    newerStartupReserveFrames,
                                    getWgcDelayReservoirLowWaterFramesForDelay(startupContentDelayTargetQpc),
                                    getWgcDelayReservoirTargetFramesForDelay(startupContentDelayTargetQpc),
                                    static_cast<long long>(wgcStartupReserveSpanUs),
                                    static_cast<long long>(wgcStartupReserveWaitInitialSpanUs),
                                    wgcStartupReserveWaitFreshenedMax,
                                    static_cast<long long>(qpcToUs(waitNow.QuadPart - wgcStartupReserveWaitStartQpc)),
                                    static_cast<long long>(qpcToUs(waitBudgetQpc)), smoothnessStartupAttempted ? 1 : 0,
                                    smoothnessRetainedFrames, smoothnessDesiredFrames, smoothnessCapLimited ? 1 : 0);
                            }
                            continue;
                        }
                        const bool noStartupSpanGrowth = wgcStartupReserveSpanUs <= 0 &&
                                                         wgcStartupReserveSpanUs <= wgcStartupReserveWaitInitialSpanUs;
                        wgcStartupReserveReason =
                            noStartupSpanGrowth
                                ? "source_startup_underfeed"
                                : (startupReserveBelowLowWater ? "low_water_timeout" : "reserve_timeout");
                    }

                    bool startupPartialReserveFallback = false;
                    if (ce::capture_policy::ShouldPreserveWgcStartupPartialReserve(
                            startupCandidates.size(), startupReserveSelection.reserveSpanQpc,
                            startupContentDelayTargetQpc > 0, startupReserveMissing)) {
                        selectedStartupIndex = 0;
                        startupPartialReserveFallback = true;
                        if (wgcStartupReserveReason != "source_startup_underfeed") {
                            wgcStartupReserveReason = "partial_span_timeout";
                        }
                    }

                    const int64_t latestStartupSelectionQpc =
                        startupSelectionQpcs.empty() ? 0 : startupSelectionQpcs.back();
                    int64_t selectedStartupSelectionQpc =
                        selectedStartupIndex < startupCandidates.size()
                            ? GetFrameSelectionTimestamp(startupCandidates[selectedStartupIndex].frame)
                            : 0;
                    int64_t actualStartupDelayQpc = latestStartupSelectionQpc > selectedStartupSelectionQpc
                                                        ? latestStartupSelectionQpc - selectedStartupSelectionQpc
                                                        : 0;
                    const int64_t pileupSmoothnessActiveDelayQpc =
                        ce::capture_policy::SelectWgcStartupSmoothnessExtraDelayQpc(
                            actualStartupDelayQpc, avContentDelayQpc, smoothnessTargetDelayQpc);
                    // Avoid locking healthy-source startup pile-up as permanent delay.
                    const bool startupMinWindowSourceAtOrAboveCfr =
                        g_WgcCap && ce::capture_policy::IsWgcIngressSourceAtOrAboveCfrTarget(
                                        std::max<uint32_t>(1u, static_cast<uint32_t>(config.video.fps)),
                                        g_WgcCap->GetInputMin250Fps(), g_WgcCap->GetInputMin500Fps());
                    const bool startupCandidateCadenceAtOrAboveCfr =
                        ce::capture_policy::IsWgcStartupCandidateCadenceAtOrAboveCfrTarget(
                            startupCandidates.size(), startupReserveSelection.reserveSpanQpc, targetIntervalTicks);
                    const bool startupSourceAtOrAboveCfr =
                        startupMinWindowSourceAtOrAboveCfr || startupCandidateCadenceAtOrAboveCfr;
                    wgcSmoothnessActiveDelayQpc = ce::capture_policy::ResolveWgcStartupSmoothnessActiveDelayQpc(
                        pileupSmoothnessActiveDelayQpc, wgcSmoothnessFloorDelayQpc, startupPartialReserveFallback,
                        startupSourceAtOrAboveCfr);
                    if (startupPartialReserveFallback && !startupSelectionQpcs.empty() &&
                        latestStartupSelectionQpc > 0) {
                        const size_t fallbackIndexBeforeContractRecalculation = selectedStartupIndex;
                        const int64_t recalculatedContentDelayQpc =
                            avContentDelayQpc + std::max<int64_t>(0, wgcSmoothnessActiveDelayQpc);
                        const int64_t recalculatedTargetQpc =
                            latestStartupSelectionQpc > recalculatedContentDelayQpc
                                ? latestStartupSelectionQpc - recalculatedContentDelayQpc
                                : latestStartupSelectionQpc;
                        selectedStartupIndex = ce::capture_policy::SelectNearestMonotonicTimestampIndex(
                            startupSelectionQpcs.data(), startupSelectionQpcs.size(), recalculatedTargetQpc);
                        selectedStartupSelectionQpc = startupSelectionQpcs[selectedStartupIndex];
                        actualStartupDelayQpc = latestStartupSelectionQpc > selectedStartupSelectionQpc
                                                    ? latestStartupSelectionQpc - selectedStartupSelectionQpc
                                                    : 0;
                        wgcSmoothnessActiveDelayQpc = ce::capture_policy::SelectWgcStartupSmoothnessExtraDelayQpc(
                            actualStartupDelayQpc, avContentDelayQpc, smoothnessTargetDelayQpc);
                        LogInfo(
                            "[EncoderThread] WGC partial reservoir contract recalculated: oldIndex=%zu newIndex=%zu "
                            "latestQpc=%lld targetQpc=%lld selectedQpc=%lld realizedContentDelayUs=%lld "
                            "renderDelayUs=%lld smoothReserveUs=%lld (frame selection and delay changed together)",
                            fallbackIndexBeforeContractRecalculation, selectedStartupIndex,
                            static_cast<long long>(latestStartupSelectionQpc),
                            static_cast<long long>(recalculatedTargetQpc),
                            static_cast<long long>(selectedStartupSelectionQpc),
                            static_cast<long long>(qpcDeltaToUs(actualStartupDelayQpc)),
                            static_cast<long long>(qpcDeltaToUs(avContentDelayQpc)),
                            static_cast<long long>(qpcDeltaToUs(wgcSmoothnessActiveDelayQpc)));
                    }
                    if (wgcSmoothnessActiveDelayQpc < pileupSmoothnessActiveDelayQpc) {
                        LogInfo(
                            "[EncoderThread] WGC startup underfed active-delay capped to measured jitter floor: "
                            "pileupUs=%lld cappedUs=%lld floorUs=%lld minWindowProof=%d candidateProof=%d "
                            "candidates=%zu candidateSpanUs=%lld reason=%s (avoids startup-timing-dependent deep-lock "
                            "repeat clustering; sync-neutral)",
                            static_cast<long long>(qpcDeltaToUs(pileupSmoothnessActiveDelayQpc)),
                            static_cast<long long>(qpcDeltaToUs(wgcSmoothnessActiveDelayQpc)),
                            static_cast<long long>(qpcDeltaToUs(wgcSmoothnessFloorDelayQpc)),
                            startupMinWindowSourceAtOrAboveCfr ? 1 : 0, startupCandidateCadenceAtOrAboveCfr ? 1 : 0,
                            startupCandidates.size(),
                            static_cast<long long>(qpcDeltaToUs(startupReserveSelection.reserveSpanQpc)),
                            wgcStartupReserveReason.c_str());
                    } else if (startupPartialReserveFallback) {
                        // The fortistutter session showed this decision silently NOT engaging because the
                        // barrier-time min-window input rate was polluted by pre-live settling gaps
                        // (MinIn250=104 for a healthy 140 fps source). Log the gate inputs so a dormant
                        // cap is diagnosable instead of invisible.
                        LogInfo(
                            "[EncoderThread] WGC startup underfed active-delay cap NOT engaged: pileupUs=%lld "
                            "floorUs=%lld sourceAtOrAboveCfr=%d minWindowProof=%d candidateProof=%d "
                            "candidates=%zu candidateSpanUs=%lld inputMin250=%u inputMin500=%u outputFps=%u "
                            "reason=%s (deep pile-up lock retained for lull absorption)",
                            static_cast<long long>(qpcDeltaToUs(pileupSmoothnessActiveDelayQpc)),
                            static_cast<long long>(qpcDeltaToUs(wgcSmoothnessFloorDelayQpc)),
                            startupSourceAtOrAboveCfr ? 1 : 0, startupMinWindowSourceAtOrAboveCfr ? 1 : 0,
                            startupCandidateCadenceAtOrAboveCfr ? 1 : 0, startupCandidates.size(),
                            static_cast<long long>(qpcDeltaToUs(startupReserveSelection.reserveSpanQpc)),
                            g_WgcCap ? g_WgcCap->GetInputMin250Fps() : 0u,
                            g_WgcCap ? g_WgcCap->GetInputMin500Fps() : 0u, static_cast<uint32_t>(config.video.fps),
                            wgcStartupReserveReason.c_str());
                    }
                    wgcSmoothnessActualFrames =
                        targetIntervalTicks > 0
                            ? SaturatingToUint32(static_cast<uint64_t>(
                                  (wgcSmoothnessActiveDelayQpc + targetIntervalTicks / 2) / targetIntervalTicks))
                            : 0u;

                    uint32_t startupRetainedCapTrimmed = 0;
                    size_t startupKeptReserveFrames = 0;
                    for (size_t i = 0; i < startupCandidates.size(); ++i) {
                        if (i < selectedStartupIndex) {
                            ReleaseQueuedFrameTexture(startupCandidates[i].frame);
                            ++startupDiscardedOlder;
                        } else if (i == selectedStartupIndex) {
                            frame = std::move(startupCandidates[i].frame);
                        } else if (startupReserveSelection.usedDelayReserve || startupPartialReserveFallback) {
                            if (smoothnessRetainedFrameCap == 0 ||
                                startupKeptReserveFrames < smoothnessRetainedFrameCap) {
                                bufferedWgcFrames.push_back(std::move(startupCandidates[i].frame));
                                ++startupKeptReserveFrames;
                            } else {
                                ReleaseQueuedFrameTexture(startupCandidates[i].frame);
                                ++startupRetainedCapTrimmed;
                            }
                        } else {
                            ReleaseQueuedFrameTexture(startupCandidates[i].frame);
                            ++startupDiscardedOlder;
                        }
                    }
                    if (startupRetainedCapTrimmed > 0) {
                        wgcRetainedCapTrimTotal += startupRetainedCapTrimmed;
                        wgcRetainedCapTrimWindow += startupRetainedCapTrimmed;
                    }

                    LARGE_INTEGER anchorNow;
                    QueryPerformanceCounter(&anchorNow);
                    ++pendingWgcStartContractGeneration;
                    const int64_t selectedContentDelayQpc = getWgcEffectiveContentDelayQpc();
                    if (frame.timestamp > 0) {
                        pendingWgcStartContract = ce::capture_policy::BuildWallAnchoredCfrTimelineStartContract(
                            anchorNow.QuadPart, selectedContentDelayQpc, avContentDelayQpc);
                    } else {
                        pendingWgcStartContract = {};
                    }
                    if (pendingWgcStartContract.valid) {
                        LogInfo(
                            "[EncoderThread] WGC CFR start contract selected: generation=%llu videoQpc=%lld "
                            "sourceQpc=%lld selectionQpc=%lld liveQpc=%lld contentDelayUs=%lld renderDelayUs=%lld "
                            "smoothReserveUs=%lld retainedNewer=%zu prewarm=%s/%lldus",
                            static_cast<unsigned long long>(pendingWgcStartContractGeneration),
                            static_cast<long long>(pendingWgcStartContract.videoOriginQpc),
                            static_cast<long long>(frame.timestamp),
                            static_cast<long long>(GetFrameSelectionTimestamp(frame)),
                            static_cast<long long>(pendingWgcStartContract.liveQpc),
                            static_cast<long long>(qpcDeltaToUs(pendingWgcStartContract.contentDelayQpc)),
                            static_cast<long long>(qpcDeltaToUs(pendingWgcStartContract.renderLoopbackLatencyQpc)),
                            static_cast<long long>(qpcDeltaToUs(pendingWgcStartContract.smoothnessReserveQpc)),
                            bufferedWgcFrames.size(), wgcEncoderPrewarmSucceeded ? "ok" : "failed",
                            static_cast<long long>(wgcEncoderPrewarmElapsedUs));
                    } else {
                        LogWarn(
                            "[EncoderThread] ERROR: WGC CFR start contract selection failed: generation=%llu "
                            "videoQpc=%lld contentDelayUs=%lld renderDelayUs=%lld prewarm=%s/%lldus",
                            static_cast<unsigned long long>(pendingWgcStartContractGeneration),
                            static_cast<long long>(frame.timestamp),
                            static_cast<long long>(qpcDeltaToUs(selectedContentDelayQpc)),
                            static_cast<long long>(qpcDeltaToUs(avContentDelayQpc)),
                            wgcEncoderPrewarmSucceeded ? "ok" : "failed",
                            static_cast<long long>(wgcEncoderPrewarmElapsedUs));
                    }
                    updateWgcIngressPressure("startup-selected");

                    const int64_t startupReserveWaitedUs =
                        wgcStartupReserveWaitStartQpc > 0 ? qpcToUs(anchorNow.QuadPart - wgcStartupReserveWaitStartQpc)
                                                          : 0;
                    const int64_t startupReserveSpanGrowthUs =
                        wgcStartupReserveWaitStartQpc > 0
                            ? std::max<int64_t>(0, wgcStartupReserveSpanUs - wgcStartupReserveWaitInitialSpanUs)
                            : 0;
                    const int64_t startupSelectedDelayUs = qpcDeltaToUs(actualStartupDelayQpc);
                    const bool startupReserveFallback =
                        startupContentDelayTargetQpc > 0 && !startupReserveSelection.usedDelayReserve;
                    const int64_t startDeltaUs =
                        ((frame.timestamp - wgcStartupBarrierQpc) * 1000000) / qpcFreq.QuadPart;
                    const int64_t frameAgeUs =
                        anchorNow.QuadPart >= frame.timestamp
                            ? ((anchorNow.QuadPart - frame.timestamp) * 1000000) / qpcFreq.QuadPart
                            : 0;
                    const bool startupSmoothnessUnderfed =
                        smoothnessStartupAttempted && wgcSmoothnessActiveDelayQpc < smoothnessTargetDelayQpc;
                    LogInfo(
                        "[EncoderThread] WGC startup sync post-delay barrier satisfied: anchorQpc=%lld "
                        "firstFrameQpc=%lld delta=%lldus frameAge=%lldus droppedPostDelay=%u "
                        "discardedBeforeDelay=%u freshened=%zu bufferedExamined=%zu queueExamined=%zu "
                        "discardedOlder=%zu discardedBeforeBarrier=%zu pathMismatch=%zu startupReserveFrames=%u "
                        "startupReserveSpanUs=%lld startupDelayTargetUs=%lld startupSelectedByDelayReserve=%d "
                        "startupReserveReason=%s keptReserveFrames=%zu startupReserveWaits=%u "
                        "startupReserveWaitedUs=%lld startupReserveInitialSpanUs=%lld "
                        "startupReserveSpanGrowthUs=%lld startupReserveWaitFreshened=%u "
                        "startupSelectedIndex=%zu startupSelectedDelayUs=%lld startupFallback=%d "
                        "startupPartialReserveFallback=%d smoothAttempt=%d smoothDesiredFrames=%u "
                        "smoothRetainedFrames=%u smoothActualFrames=%u smoothTargetDelayUs=%lld "
                        "smoothDelayUs=%lld smoothStartupUnderfed=%d smoothPoolSlots=%u retainedCap=%u "
                        "reservedFreeSlots=%u retainedCapTrimmed=%u smoothCapLimited=%d "
                        "startupEffectiveDelayUs=%lld smoothReason=%s",
                        static_cast<long long>(wgcStartupBarrierQpc), static_cast<long long>(frame.timestamp),
                        static_cast<long long>(startDeltaUs), static_cast<long long>(frameAgeUs),
                        wgcStartupBarrierDroppedFrames, wgcStartupPreLiveDelayDroppedFrames, startupFreshened,
                        startupBufferedExamined, startupQueueExamined, startupDiscardedOlder,
                        startupDiscardedBeforeBarrier, startupDiscardedPathMismatch, wgcStartupReserveFrames,
                        static_cast<long long>(wgcStartupReserveSpanUs),
                        static_cast<long long>(wgcStartupDelayTargetUs), wgcStartupSelectedByDelayReserve ? 1 : 0,
                        wgcStartupReserveReason.c_str(), bufferedWgcFrames.size(), wgcStartupReserveWaitCount,
                        static_cast<long long>(startupReserveWaitedUs),
                        static_cast<long long>(wgcStartupReserveWaitInitialSpanUs),
                        static_cast<long long>(startupReserveSpanGrowthUs), wgcStartupReserveWaitFreshenedMax,
                        selectedStartupIndex, static_cast<long long>(startupSelectedDelayUs),
                        startupReserveFallback ? 1 : 0, startupPartialReserveFallback ? 1 : 0,
                        smoothnessStartupAttempted ? 1 : 0, smoothnessDesiredFrames, smoothnessRetainedFrames,
                        wgcSmoothnessActualFrames, static_cast<long long>(qpcDeltaToUs(smoothnessTargetDelayQpc)),
                        static_cast<long long>(qpcDeltaToUs(wgcSmoothnessActiveDelayQpc)),
                        startupSmoothnessUnderfed ? 1 : 0, smoothnessPoolSlots, smoothnessRetainedFrameCap,
                        smoothnessReservedFreeSlots, startupRetainedCapTrimmed, smoothnessCapLimited ? 1 : 0,
                        static_cast<long long>(qpcDeltaToUs(getWgcEffectiveContentDelayQpc())),
                        wgcSmoothnessBufferReason.c_str());
                }

                pendingLiveActivation = true;
                // Reset Bresenham credit for a clean start; keep smoothedInputPerTick
                // so the EMA calibration from warmup carries over.
                frameCreditAccumulator = 0.0;
                selectionLogCounter = 0;
                pacingInputThisWindow = 0;
                pacingTicksThisWindow = 0;
                encoderGridStartQpc = 0;
                encoderGridTickCount = 0;
                liveTicksOutput = 0;
                liveTicksScheduled = 0;
                liveTicksDiscardedByTimerRebase = 0;
                wgcVisualDebtMaxExcessTicks = 0;
                wgcStopDrainHeldFrameLogged = false;
                liveStartQpc = {};
                wgcInputPredictor.Reset();
                wgcCfrPhaseLock.Reset();
                smoothedEncCycleMs = 0.0;
                smoothedInjectServiceMs = 0.0;
                smoothedWgcFreshServiceMs = 0.0;
                smoothedWgcRepeatServiceMs = 0.0;
                wgcFreshServiceSamples = wgcRepeatServiceSamples = 0;
                wgcOverloadRepeatPacer = {};
                wgcRepeatCatchupTotal = 0;
                wgcFreshCatchupTotal = 0;
                injectServiceMaxUs = 0;
                injectCfrRecoveryActive = false;
                injectEncoderServiceTooSlowCurrent = false;
                injectCfrRecoveryStartTick = 0;
                injectCfrRecoveryStartDebt = 0;
                injectCfrRecoveryBestDebt = 0;
                injectCfrRecoveryStartFreshCatchup = injectFreshCatchupTotal;
                injectCfrRecoveryStartRepeatCatchup = injectRepeatCatchupTotal;
                injectCfrRecoveryLastProgressLogTick = 0;
                encCycleMaxMs = 0;
                dupTimestampCount = 0;
                lastWgcDuplicateTimestampSkipCountForCadence =
                    g_WgcCap ? g_WgcCap->GetDuplicateTimestampSkipCount() : 0u;
                wgcRecentDeliveredFps = 0;
                wgcRecentDeliveredMin250Fps = 0;
                wgcRecentDeliveredMin500Fps = 0;
                wgcRecentInputMin250Fps = 0;
                wgcRecentInputMin500Fps = 0;
                wgcNoFreshTickCount = 0;
                encodeSpikeCountThisSecond = 0;
                wgcQueueTickSampleCount = 0;
                wgcNoFreshTickPermille = 0;
                wgcBufferedAtTickSum = 0;
                wgcBufferedAtTickMin = UINT32_MAX;
                wgcNoReserveTickCount = 0;
                wgcAncientSelectionCount = 0;
                wgcFreshSelectionMissCount = 0;
                wgcStaleUniqueFallbackCount = 0;
                wgcRepeatNoFreshCount = 0;
                wgcRepeatPolicyHoldCount = 0;
                wgcRepeatPolicyHoldTotal = 0;
                wgcSyncDelayHoldCount = 0;
                wgcSyncDelayHoldTotal = 0;
                wgcSyncDelaySourceLimitedHoldCount = 0;
                wgcSyncDelaySourceLimitedHoldTotal = 0;
                wgcSyncDelayPolicyHoldCount = 0;
                wgcSyncDelayPolicyHoldTotal = 0;
                wgcTooNewLeadMaxUs = 0;
                wgcTooNewLeadSessionMaxUs = 0;
                wgcDelaySoftLateRejectedTotal = 0;
                wgcDelaySoftLateRejectedWindow = 0;
                wgcDelaySoftLateAcceptedTotal = 0;
                wgcDelaySoftLateAcceptedWindow = 0;
                wgcDelayNearCapAcceptedTotal = 0;
                wgcDelayNearCapAcceptedWindow = 0;
                wgcDelayUniformCadenceTotal = 0;
                wgcDelayUniformCadenceWindow = 0;
                wgcDelayUniformHoldTotal = 0;
                wgcDelayUniformHoldWindow = 0;
                wgcDelayPaceCapTrimTotal = 0;
                wgcDelayPaceCapTrimWindow = 0;
                wgcRetainedCapTrimTotal = 0;
                wgcRetainedCapTrimWindow = 0;
                wgcPoolPressureTrimTotal = 0;
                wgcPoolPressureTrimWindow = 0;
                wgcDelayOlderFrameAvoidedRepeatTotal = 0;
                wgcDelayOlderFrameAvoidedRepeatWindow = 0;
                wgcDelaySourceLimitedRepeatTotal = 0;
                wgcDelaySourceLimitedRepeatWindow = 0;
                wgcDelayRepeatRescueAttemptTotal = 0;
                wgcDelayRepeatRescueAttemptWindow = 0;
                wgcDelayRepeatRescueSuccessTotal = 0;
                wgcDelayRepeatRescueSuccessWindow = 0;
                wgcDelayRepeatRescueRejectedSyncTotal = 0;
                wgcDelayRepeatRescueRejectedSyncWindow = 0;
                wgcDelayRepeatRescueRejectedHeadroomTotal = 0;
                wgcDelayRepeatRescueRejectedHeadroomWindow = 0;
                wgcDelayRepeatRescueRejectedCostTotal = 0;
