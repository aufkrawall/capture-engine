#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string ReadCoordinatorSource() {
    const std::filesystem::path source = std::filesystem::current_path() / "captureengine" / "media_main.cpp";
    std::ifstream file(source, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::string ReadWgcCaptureSource() {
    const std::filesystem::path source = std::filesystem::current_path() / "captureengine" / "wgc_capture.cpp";
    std::ifstream file(source, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::string ReadVideoEncoderSource() {
    const std::filesystem::path source = std::filesystem::current_path() / "mediaengine" / "video_encoder.cpp";
    std::ifstream file(source, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::string ReadPrivacyRuntimeSource() {
    const std::filesystem::path source =
        std::filesystem::current_path() / "captureengine" / "screen_grab_privacy_runtime.cpp";
    std::ifstream file(source, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::string ReadPrivacyPolicySource() {
    const std::filesystem::path source = std::filesystem::current_path() / "common" / "screen_grab_privacy.cpp";
    std::ifstream file(source, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

}  // namespace

TEST(CaptureCoordinatorSourceTest, WgcRetargetUsesAtomicallyPublishedSharedOwnership) {
    const std::string source = ReadCoordinatorSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("AtomicSharedOwner<WGCCapture> g_WgcCap"), std::string::npos);
    EXPECT_NE(source.find("PublishWgcCapture(std::move(capture), \"window retarget\")"), std::string::npos);
    EXPECT_NE(source.find("auto retired = g_WgcCap.Exchange"), std::string::npos);
    EXPECT_NE(source.find("retired.reset()"), std::string::npos);
    EXPECT_NE(source.find("g_WgcSourceEpoch.fetch_add"), std::string::npos);
    EXPECT_NE(source.find("g_WgcCap.LockExclusive()"), std::string::npos);
    EXPECT_NE(source.find("DiscardWgcEpochNotEqual"), std::string::npos);
    EXPECT_EQ(source.find("g_WgcCap.reset()"), std::string::npos);
    EXPECT_EQ(source.find("std::make_unique<WGCCapture>()"), std::string::npos);
}

TEST(CaptureCoordinatorSourceTest, DuplicationCursorSuppressionIsExplicitlyReset) {
    const std::string source = ReadCoordinatorSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("ResetDuplicationCursorSuppression(\"WGC pipeline stop\")"), std::string::npos);
    EXPECT_NE(source.find("ResetDuplicationCursorSuppression(\"WGC recording start\")"), std::string::npos);
    EXPECT_NE(source.find("MediaEngine_SetCursorCompositionSuppressed(false)"), std::string::npos);
}

TEST(CaptureCoordinatorSourceTest, ExplicitTenBitWgcCannotUseCompactBgraIntermediate) {
    const std::string source = ReadWgcCaptureSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("allowLossyBgra8Pool_ &&"), std::string::npos);
    EXPECT_NE(source.find("ShouldAllowBgra8WgcFallback(requireHighPrecisionCapture_, captureIsHDR_)"),
              std::string::npos);
}

TEST(CaptureCoordinatorSourceTest, SplitDeviceKeyedMutexLifecycleCoversDiscardedAndRepeatedFrames) {
    const std::string capture = ReadWgcCaptureSource();
    const std::string encoder = ReadVideoEncoderSource();
    ASSERT_FALSE(capture.empty());
    ASSERT_FALSE(encoder.empty());

    // A frame discarded by CFR never reaches the encoder to return key 1 to
    // key 0. Once its lease is free, the producer must reclaim key 1 instead
    // of permanently poisoning that pool slot.
    EXPECT_NE(capture.find("writeMutex->AcquireSync(1, 0)"), std::string::npos);
    EXPECT_NE(capture.find("keyedMutexAbandonedReclaimCount_"), std::string::npos);
    EXPECT_NE(capture.find("const uint64_t releaseKey = copySucceeded ? 1 : 0"), std::string::npos);

    // Cursor-aware CFR repeats make a second source read after fresh-frame
    // conversion returned the shared surface to key 0. That cache copy must
    // reacquire key 0 or the repeat texture can be black/stale.
    const size_t cacheBegin = encoder.find("bool VideoEncoder::CacheRepeatSourceFrameTexture");
    const size_t cacheEnd = encoder.find("bool VideoEncoder::PopulateD3D11FrameFromRepeatSource", cacheBegin);
    ASSERT_NE(cacheBegin, std::string::npos);
    ASSERT_NE(cacheEnd, std::string::npos);
    const std::string cacheFunction = encoder.substr(cacheBegin, cacheEnd - cacheBegin);
    EXPECT_NE(cacheFunction.find("keyedSourceGuard.mutex->AcquireSync(0, 0)"), std::string::npos);
    EXPECT_NE(cacheFunction.find("d3d11Context->CopyResource(repeatSourceFrameTexture, sourceTexture)"),
              std::string::npos);
    EXPECT_NE(cacheFunction.find("d3d11Context->Flush()"), std::string::npos);
}

TEST(CaptureCoordinatorSourceTest, FreshAndRepeatedCfrFramesShareScheduledCursorSelection) {
    const std::string source = ReadCoordinatorSource();
    ASSERT_FALSE(source.empty());

    const size_t resolver = source.find("auto selectCursorStateForScheduledQpc");
    const size_t repeat = source.find("auto repeatLastFrameForScheduledQpc", resolver);
    const size_t recovery = source.find("auto recoverScheduledFreshEncodeFailure", repeat);
    ASSERT_NE(resolver, std::string::npos);
    ASSERT_NE(repeat, std::string::npos);
    ASSERT_NE(recovery, std::string::npos);

    const std::string resolverBody = source.substr(resolver, repeat - resolver);
    EXPECT_NE(resolverBody.find("timeline.SelectAtOrBefore(cursorTargetQpc, &cursorState)"), std::string::npos);
    EXPECT_NE(resolverBody.find("captureWidth, captureHeight, false)"), std::string::npos);
    EXPECT_NE(resolverBody.find("cursorState.SetSourceEmbedded(cursorEmbedded)"), std::string::npos);
    EXPECT_EQ(resolverBody.find("captureWidth, captureHeight, cursorEmbedded)"), std::string::npos);

    const std::string repeatBody = source.substr(repeat, recovery - repeat);
    EXPECT_NE(repeatBody.find("selectCursorStateForScheduledQpc(scheduledQpc, g_LastFrame, \"repeat\")"),
              std::string::npos);

    EXPECT_NE(source.find("selectCursorStateForScheduledQpc(scheduledOutputQpc, *frameToProcess, \"fresh\")"),
              std::string::npos);
    EXPECT_NE(source.find("selectCursorStateForScheduledQpc(repeatScheduledQpc, catchupFrame, \"fresh-catchup\")"),
              std::string::npos);
}

TEST(CaptureCoordinatorSourceTest, CfrRecoverySeparatesOutputGridFromWakeDeadlineForEveryBackend) {
    const std::string source = ReadCoordinatorSource();
    ASSERT_FALSE(source.empty());

    const size_t wgcOutputGrid = source.find("scheduledOutputQpc = ce::capture_policy::GetNextCfrOutputQpc");
    const size_t wgcSelection = source.find("auto computeWgcSelectionTargetForTick", wgcOutputGrid);
    ASSERT_NE(wgcOutputGrid, std::string::npos);
    ASSERT_NE(wgcSelection, std::string::npos);
    EXPECT_LT(wgcOutputGrid, wgcSelection);
    EXPECT_NE(source.find("computeWgcSelectionTargetForTick(scheduledOutputQpc", wgcSelection), std::string::npos);
    EXPECT_NE(source.find("scheduledOutputQpc > 0 ? scheduledOutputQpc", wgcSelection), std::string::npos);
    EXPECT_EQ(source.find("wgcLiveRecoveryModeActive && !wgcAudioLeadCatchupPressure"), std::string::npos);
    EXPECT_EQ(source.find("if (extraTick > 1)"), std::string::npos);
    EXPECT_NE(source.find("scheduledOutputQpc = ce::capture_policy::GetNextInjectCfrOutputQpc"),
              std::string::npos);
    EXPECT_NE(source.find("ShouldAdvanceWakeDeadlineForCfrCatchupTick(useScreenGrab,"), std::string::npos);
    EXPECT_NE(source.find("scheduledOutputQpc + static_cast<int64_t>(extraTick) * targetIntervalTicks"),
              std::string::npos);
    EXPECT_EQ(source.find("scheduledSampleQpc + static_cast<int64_t>(extraTick) * targetIntervalTicks"),
              std::string::npos);
    EXPECT_NE(source.find("GetInjectCfrServiceMsPerOutputTick(cycleMs, outputTicksThisCycle)"),
              std::string::npos);
    EXPECT_NE(source.find("std::max(smoothedEncodeMs, smoothedInjectServiceMs)"), std::string::npos);
    EXPECT_EQ(source.find("std::max(smoothedEncodeMs, smoothedEncCycleMs)"), std::string::npos);
}

TEST(CaptureCoordinatorSourceTest, WgcCatchupRecoveryChangesPixelsOnlyOnTheImmutableCfrGrid) {
    const std::string source = ReadCoordinatorSource();
    ASSERT_FALSE(source.empty());

    const size_t recoveryBegin = source.find("const size_t freshCatchupReserveFrames");
    const size_t recoveryEnd = source.find("if (!hasRepeatLastFramePath)", recoveryBegin);
    ASSERT_NE(recoveryBegin, std::string::npos);
    ASSERT_NE(recoveryEnd, std::string::npos);
    const std::string recovery = source.substr(recoveryBegin, recoveryEnd - recoveryBegin);

    EXPECT_NE(recovery.find("GetWgcFreshCatchupBudgetThisLoop"), std::string::npos);
    EXPECT_NE(recovery.find("getWgcDelayReservoirLowWaterFrames"), std::string::npos);
    EXPECT_NE(recovery.find("isWgcEffectiveContentDelayActive"), std::string::npos);
    EXPECT_NE(recovery.find("smoothedWgcFreshServiceMs"), std::string::npos);
    EXPECT_NE(recovery.find("UpdateWgcServiceTimeEma("), std::string::npos);
    EXPECT_NE(recovery.find("currentEncodeMs, pureEncodeMs, kEncodeEmaAlpha"), std::string::npos);
    EXPECT_NE(recovery.find("ShouldUseFreshWgcCatchupFrame"), std::string::npos);
    EXPECT_NE(recovery.find("ApplyCfrCaptureSyncPhaseLock"), std::string::npos);
    EXPECT_NE(recovery.find("computeWgcSelectionTargetForTick"), std::string::npos);
    EXPECT_NE(recovery.find("computeLiveTimelineElapsedUs(repeatScheduledQpc)"), std::string::npos);
    EXPECT_NE(recovery.find("++liveTicksOutput"), std::string::npos);
    EXPECT_NE(recovery.find("++encoderGridTickCount"), std::string::npos);
    EXPECT_EQ(recovery.find("clampWgcSelectionTargetQpc"), std::string::npos);
    EXPECT_EQ(source.find("[EncoderThread] CFR Catchup applied using fresh frame"), std::string::npos);
    EXPECT_NE(source.find("PtsGrid=immutable AudioTimeline=unchanged"), std::string::npos);
}

TEST(CaptureCoordinatorSourceTest, WgcDeepGridDebtNeverMovesTheAudioAlignedSelectionTarget) {
    const std::string source = ReadCoordinatorSource();
    ASSERT_FALSE(source.empty());

    const size_t uniformStart = source.find("int64_t playoutTargetQpc =");
    const size_t uniformEnd = source.find("const uint32_t uniformActiveDelaySoftLateTargetUs", uniformStart);
    ASSERT_NE(uniformStart, std::string::npos);
    ASSERT_NE(uniformEnd, std::string::npos);
    const std::string uniformSelection = source.substr(uniformStart, uniformEnd - uniformStart);

    EXPECT_NE(uniformSelection.find("IsWgcFrameTooNewForCfrSlot"), std::string::npos);
    EXPECT_NE(uniformSelection.find("Uniform playout grid-debt sync hold"), std::string::npos);
    EXPECT_EQ(uniformSelection.find("ApplyWgcUniformPlayoutAntiFreezeFloor"), std::string::npos);
    EXPECT_EQ(source.find("IsWgcUniformPlayoutAntiFreezeFloorSyncSafe"), std::string::npos);
    EXPECT_NE(source.find("GridDebtSyncHolds="), std::string::npos);
    EXPECT_NE(source.find("held-repeat catch-up advances the "), std::string::npos);
}

TEST(CaptureCoordinatorSourceTest, ScreenGrabOverloadPacingRepeatsWithoutMovingTheCfrOrAudioGrid) {
    const std::string source = ReadCoordinatorSource();
    ASSERT_FALSE(source.empty());

    const size_t telemetryTick = source.find("const bool scheduledWgcTelemetryTick");
    const size_t pacerCall = source.find("UpdateWgcOverloadRepeatPacer(", telemetryTick);
    const size_t repeatBranch = source.find("if (wgcProactiveOverloadRepeatThisTick)", pacerCall);
    const size_t ordinarySelection = source.find("else if (!g_EncoderRunning && !bufferedWgcFrames.empty())",
                                                 repeatBranch);
    ASSERT_NE(telemetryTick, std::string::npos);
    ASSERT_NE(pacerCall, std::string::npos);
    ASSERT_NE(repeatBranch, std::string::npos);
    ASSERT_NE(ordinarySelection, std::string::npos);
    EXPECT_LT(telemetryTick, pacerCall);
    EXPECT_LT(pacerCall, repeatBranch);
    EXPECT_LT(repeatBranch, ordinarySelection);

    const std::string pacerIntegration = source.substr(telemetryTick, ordinarySelection - telemetryTick);
    EXPECT_NE(pacerIntegration.find("IsWgcIngressSourceAtOrAboveCfrTarget"), std::string::npos);
    EXPECT_NE(pacerIntegration.find("wgcFreshAvailableAtTickStart"), std::string::npos);
    EXPECT_NE(pacerIntegration.find("smoothedWgcFreshServiceMs"), std::string::npos);
    EXPECT_NE(pacerIntegration.find("smoothedWgcRepeatServiceMs"), std::string::npos);
    EXPECT_NE(pacerIntegration.find("CFR PTS and audio timeline unchanged"), std::string::npos);
    EXPECT_EQ(pacerIntegration.find("++liveTicksOutput"), std::string::npos);
    EXPECT_EQ(pacerIntegration.find("scheduledOutputQpc ="), std::string::npos);
    EXPECT_EQ(source.find("UpdateWgcOverloadRepeatPacer(", pacerCall + 1), std::string::npos);
    EXPECT_NE(source.find("++wgcOverloadRepeatPacer.emittedRepeats"), std::string::npos);
    EXPECT_NE(source.find("else if (!duplicateFromCapacityPacerReason)"), std::string::npos);
    EXPECT_NE(source.find("wgcProactiveOverloadRepeatThisTick);"), std::string::npos);
}

TEST(CaptureCoordinatorSourceTest, EncoderCapacityWarningUsesStartupGuard) {
    const std::string source = ReadCoordinatorSource();
    ASSERT_FALSE(source.empty());

    const size_t warningDecision = source.find("ShouldWarnEncoderApproachingCapacity(");
    const size_t warningLog = source.find("LogWarn(\"Encoder approaching capacity", warningDecision);
    ASSERT_NE(warningDecision, std::string::npos);
    ASSERT_NE(warningLog, std::string::npos);

    const std::string warningBlock = source.substr(warningDecision, warningLog - warningDecision);
    EXPECT_NE(warningBlock.find("encoderStartupWindowActive"), std::string::npos);
    EXPECT_EQ(source.find("smoothedEncodeMs > frameIntervalMs * 0.85"), std::string::npos);
    EXPECT_EQ(source.find("LogInfo(\"[WARN] Encoder approaching capacity"), std::string::npos);
}

TEST(CaptureCoordinatorSourceTest, AutoFallbackProvesWgcBeforeStoppingInject) {
    const std::string source = ReadCoordinatorSource();
    ASSERT_FALSE(source.empty());

    const size_t fallbackStart = source.find("StartWgcRecordingCapture(config)");
    const size_t firstFrameProof = source.find("autoWgcHandoff.OnWgcFirstFrame()");
    ASSERT_NE(fallbackStart, std::string::npos);
    ASSERT_NE(firstFrameProof, std::string::npos);
    const size_t activate = source.find("SetActiveScreenGrab(true)", firstFrameProof);
    const size_t stopInjectPublication = source.find(
        "SetInjectVideoCaptureRequestedState(false, \"auto inject-to-WGC handoff committed\")", firstFrameProof);
    const size_t stopInject = source.find("StopInjectCapturePipeline()", firstFrameProof);
    ASSERT_NE(activate, std::string::npos);
    ASSERT_NE(stopInjectPublication, std::string::npos);
    ASSERT_NE(stopInject, std::string::npos);
    EXPECT_LT(activate, stopInjectPublication);
    EXPECT_LT(stopInjectPublication, stopInject);
    EXPECT_LT(activate, stopInject);

    EXPECT_NE(source.find("WGC fallback failed to start; inject capture remains active"), std::string::npos);
    EXPECT_NE(source.find("g_AutoWgcFallbackArmed.store(fallbackReady"), std::string::npos);
    EXPECT_NE(source.find("inject remains active pending first-frame "), std::string::npos);
    EXPECT_NE(source.find("proof"), std::string::npos);
}

TEST(CaptureCoordinatorSourceTest, AutoInjectFallbackNeverDefaultsToAnUnresolvedPrimaryMonitor) {
    const std::string source = ReadCoordinatorSource();
    ASSERT_FALSE(source.empty());

    const size_t fallbackBegin = source.find("if (isAutoCaptureConfig() && injectWhitelisted)");
    const size_t fallbackEnd = source.find("if (!config.wgcWindowTitles.empty())", fallbackBegin);
    ASSERT_NE(fallbackBegin, std::string::npos);
    ASSERT_NE(fallbackEnd, std::string::npos);
    const std::string fallback = source.substr(fallbackBegin, fallbackEnd - fallbackBegin);

    EXPECT_NE(fallback.find("candidate.pid == sourcePid"), std::string::npos);
    EXPECT_NE(fallback.find("fallbackMonitor && WGCCapture::IsSupported()"), std::string::npos);
    EXPECT_NE(fallback.find("leaving fallback unarmed instead of capturing an unrelated primary monitor"),
              std::string::npos);
}

TEST(CaptureCoordinatorSourceTest, MonitorSelectionIsStableAndNeverFallsBackAcrossDisplays) {
    const std::string source = ReadCoordinatorSource();
    ASSERT_FALSE(source.empty());

    const size_t monitorPrime = source.find("auto primeWgcMonitorTarget");
    const size_t configuredPrime = source.find("auto primeConfiguredMonitorTarget", monitorPrime);
    ASSERT_NE(monitorPrime, std::string::npos);
    ASSERT_NE(configuredPrime, std::string::npos);
    const std::string monitorPrimeBody = source.substr(monitorPrime, configuredPrime - monitorPrime);
    EXPECT_NE(monitorPrimeBody.find("capture->InitForMonitor(d3dDevice, targetMonitor)"), std::string::npos);
    EXPECT_EQ(source.find("capture->Init(d3dDevice)"), std::string::npos);
    EXPECT_NE(monitorPrimeBody.find("refusing cross-monitor fallback"), std::string::npos);

    EXPECT_NE(source.find("currentCapturedMonitorStableId"), std::string::npos);
    EXPECT_NE(source.find("\"id:\" + currentCapturedMonitorStableId"), std::string::npos);
    EXPECT_NE(source.find("discardCurrentWgcTarget(\"explicit monitor unavailable\")"), std::string::npos);
    EXPECT_NE(source.find("refusing fallback to "), std::string::npos);
    EXPECT_NE(source.find("another display"), std::string::npos);
}

TEST(CaptureCoordinatorSourceTest, WorkerTimeoutNeverDetachesAcrossLiveResources) {
    const std::string source = ReadCoordinatorSource();
    ASSERT_FALSE(source.empty());

    EXPECT_EQ(source.find("thread.detach()"), std::string::npos);
    EXPECT_NE(source.find("cleanup while the worker is live would race released capture/encoder resources"),
              std::string::npos);
    EXPECT_NE(source.find("thread.join();"), std::string::npos);
}

TEST(CaptureCoordinatorSourceTest, WgcFramesCarryProducerEpochInsteadOfSamplingCoordinatorEpoch) {
    const std::string coordinator = ReadCoordinatorSource();
    const std::string capture = ReadWgcCaptureSource();
    ASSERT_FALSE(coordinator.empty());
    ASSERT_FALSE(capture.empty());

    EXPECT_NE(coordinator.find("replacement->SetSourceEpoch(epoch)"), std::string::npos);
    EXPECT_EQ(coordinator.find("AdvanceActiveWgcSourceEpoch"), std::string::npos);
    EXPECT_NE(coordinator.find("Keep the standby capture's publication epoch"), std::string::npos);
    EXPECT_NE(coordinator.find("qf.wgcSourceEpoch = sourceEpoch"), std::string::npos);
    EXPECT_NE(coordinator.find("qf.wgcSourceEpoch = frame.sourceEpoch"), std::string::npos);
    EXPECT_EQ(coordinator.find("qf.wgcSourceEpoch = g_WgcSourceEpoch.load"), std::string::npos);
    EXPECT_NE(capture.find("outputFrame->sourceEpoch = sourceEpoch"), std::string::npos);
    EXPECT_NE(capture.find("sourceEpoch_.load(std::memory_order_acquire)"), std::string::npos);
}

TEST(CaptureCoordinatorSourceTest, SourceEpochInvalidatesMediaEngineRepeatPixels) {
    const std::string source = ReadCoordinatorSource();
    const std::string privacyRuntime = ReadPrivacyRuntimeSource();
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(privacyRuntime.empty());

    EXPECT_NE(source.find("privacyRuntime.ResetSource()"), std::string::npos);
    EXPECT_NE(privacyRuntime.find("MediaEngine_ResetRepeatFrameCache()"), std::string::npos);
    EXPECT_NE(source.find("ShouldRepeatAfterScheduledFreshEncodeFailure"), std::string::npos);
    EXPECT_NE(source.find("CFR fresh encode failure recovered with cached duplicate"), std::string::npos);
}

TEST(CaptureCoordinatorSourceTest, FreshFrameMetadataCommitsOnlyAfterSuccessfulEncode) {
    const std::string source = ReadCoordinatorSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("frameToProcess = &frame;"), std::string::npos);
    EXPECT_NE(source.find("const bool attemptedFreshCandidate = popped && frameToProcess == &frame"),
              std::string::npos);
    EXPECT_NE(source.find("g_LastFrame = std::move(frame);"), std::string::npos);
    EXPECT_NE(source.find("preserve g_LastFrame unchanged"), std::string::npos);
    EXPECT_NE(source.find("frame.injectRingLease.Reset();"), std::string::npos);
    EXPECT_NE(source.find("Deferred candidates never enter this branch"), std::string::npos);
}

TEST(CaptureCoordinatorSourceTest, StopBeforeFirstLiveFrameDisarmsCfrDrainBeforeQueuePolling) {
    const std::string source = ReadCoordinatorSource();
    ASSERT_FALSE(source.empty());

    const size_t drainAbort = source.find("ShouldAbortCfrStopDrainBeforeOutputIsLive");
    const size_t drainClear = source.find("g_DrainOutstandingCfrTicks.store(false", drainAbort);
    const size_t stopDrain =
        source.find("if (!recordingActive && recordingOutputLive && drainOutstandingCfrTicks)", drainClear);
    ASSERT_NE(drainAbort, std::string::npos);
    ASSERT_NE(drainClear, std::string::npos);
    ASSERT_NE(stopDrain, std::string::npos);
    EXPECT_LT(drainAbort, drainClear);
    EXPECT_LT(drainClear, stopDrain);
}

TEST(CaptureCoordinatorSourceTest, WgcAndDuplicationStartupPrewarmsBeforeTransactionalContractCommit) {
    const std::string source = ReadCoordinatorSource();
    const std::string encoder = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(encoder.empty());

    const size_t startupBarrier = source.find("IsWgcFramePastStartupBarrier");
    const size_t prewarm = source.find("MediaEngine_PrepareFrameD3D11", startupBarrier);
    const size_t refreshedBarrier =
        source.find("WGC startup barrier refreshed after transactional prewarm", prewarm);
    const size_t contractSelection = source.find("WGC CFR start contract selected", refreshedBarrier);
    const size_t firstEncodeCommit =
        source.find("WGC CFR start contract committed after first successful encode", contractSelection);
    ASSERT_NE(startupBarrier, std::string::npos);
    ASSERT_NE(prewarm, std::string::npos);
    ASSERT_NE(refreshedBarrier, std::string::npos);
    ASSERT_NE(contractSelection, std::string::npos);
    ASSERT_NE(firstEncodeCommit, std::string::npos);
    EXPECT_LT(startupBarrier, prewarm);
    EXPECT_LT(prewarm, refreshedBarrier);
    EXPECT_LT(refreshedBarrier, contractSelection);
    EXPECT_LT(contractSelection, firstEncodeCommit);

    EXPECT_NE(source.find("BuildWallAnchoredCfrTimelineStartContract", refreshedBarrier), std::string::npos);
    EXPECT_NE(source.find("firstTransactionalWgcFrame ? pendingWgcStartContract.videoOriginQpc"),
              std::string::npos);
    EXPECT_EQ(source.find("RebaseCfrTimelineStartContract(", contractSelection), std::string::npos);
    EXPECT_NE(source.find("Preserved transactional WGC startup reserve at live handoff"), std::string::npos);
    EXPECT_EQ(source.find("Flushed %zu warmup WGC frames before live handoff"), std::string::npos);

    const size_t prepareBegin = encoder.find("bool VideoEncoder::PrepareFrameD3D11");
    const size_t encodeBegin = encoder.find("bool VideoEncoder::EncodeFrameD3D11", prepareBegin);
    ASSERT_NE(prepareBegin, std::string::npos);
    ASSERT_NE(encodeBegin, std::string::npos);
    EXPECT_EQ(encoder.substr(prepareBegin, encodeBegin - prepareBegin).find("inputFrameCount++"), std::string::npos);
    EXPECT_NE(encoder.find("inputFrameCount++", encodeBegin), std::string::npos);
}

TEST(CaptureCoordinatorSourceTest, WgcStartupSmoothnessHistoryIsProtectedBeforeLiveDebtPruning) {
    const std::string source = ReadCoordinatorSource();
    ASSERT_FALSE(source.empty());

    const size_t pruneHelper = source.find("auto pruneStaleWgcVisualDebt");
    const size_t startupProtection = source.find("ShouldProtectWgcStartupSmoothnessHistory", pruneHelper);
    const size_t liveDebtFloor = source.find("GetWgcLiveVisualDebtFloorQpcForMode", startupProtection);
    const size_t gridPrune = source.find("ShouldPruneWgcVisualDebtFrameForGrid", liveDebtFloor);
    const size_t selectionPrune =
        source.find("pruneStaleWgcVisualDebt(liveNowQpc, \"selection\"", gridPrune);
    ASSERT_NE(pruneHelper, std::string::npos);
    ASSERT_NE(startupProtection, std::string::npos);
    ASSERT_NE(liveDebtFloor, std::string::npos);
    ASSERT_NE(gridPrune, std::string::npos);
    ASSERT_NE(selectionPrune, std::string::npos);
    EXPECT_LT(pruneHelper, startupProtection);
    EXPECT_LT(startupProtection, liveDebtFloor);
    EXPECT_LT(liveDebtFloor, gridPrune);
    EXPECT_NE(source.find("outputShortfallTicks > 0 && immutableSelectionTargetQpc <= 0", pruneHelper),
              std::string::npos);
    EXPECT_NE(source.substr(selectionPrune, 300).find("selectionTargetQpc"), std::string::npos);
    EXPECT_NE(source.find("WGC startup history protected from the shallower live-debt window", startupProtection),
              std::string::npos);
}

TEST(CaptureCoordinatorSourceTest, ProvenStandbyFrameAndInjectRepeatSurviveHandoffBoundary) {
    const std::string source = ReadCoordinatorSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("Keep the standby capture's publication epoch"), std::string::npos);
    EXPECT_NE(source.find("canPreserveLastFrameAcrossPathHandoff"), std::string::npos);
    EXPECT_NE(source.find("Observed standby WGC source epoch"), std::string::npos);
    EXPECT_EQ(source.find("AdvanceActiveWgcSourceEpoch"), std::string::npos);
    EXPECT_NE(source.find("capture->GetCallbackFrameCount()"), std::string::npos);
    EXPECT_NE(source.find("config.video.useVFR ? HasStandbyWgcHandoffFrame()"), std::string::npos);
    EXPECT_NE(source.find("TakeStandbyWgcHandoffFrame(retainedVfrFrame)"), std::string::npos);
    EXPECT_NE(source.find("SubmitWgcQueuedFrame(std::move(retainedVfrFrame))"), std::string::npos);

    const size_t storeHelper = source.find("static bool StoreStandbyWgcHandoffFrame");
    const size_t storeRecheck = source.find("!g_RetainStandbyWgcFrameForHandoff.load", storeHelper);
    ASSERT_NE(storeHelper, std::string::npos);
    ASSERT_NE(storeRecheck, std::string::npos);

    const size_t firstFrameProof = source.find("autoWgcHandoff.OnWgcFirstFrame()");
    const size_t disarmRetention = source.find("g_RetainStandbyWgcFrameForHandoff.store(false", firstFrameProof);
    const size_t takeRetained = source.find("TakeStandbyWgcHandoffFrame(retainedVfrFrame)", firstFrameProof);
    ASSERT_NE(disarmRetention, std::string::npos);
    ASSERT_NE(takeRetained, std::string::npos);
    EXPECT_LT(disarmRetention, takeRetained);
}

TEST(CaptureCoordinatorSourceTest, ScreenGrabPrivacyGatesEveryVideoSubmissionShape) {
    const std::string source = ReadCoordinatorSource();
    ASSERT_FALSE(source.empty());

    const size_t repeatBegin = source.find("auto repeatLastFrameForScheduledQpc");
    const size_t repeatEnd = source.find("auto recoverScheduledFreshEncodeFailure", repeatBegin);
    ASSERT_NE(repeatBegin, std::string::npos);
    ASSERT_NE(repeatEnd, std::string::npos);
    const std::string repeat = source.substr(repeatBegin, repeatEnd - repeatBegin);
    EXPECT_NE(repeat.find("evaluateScreenGrabPrivacy(nullptr)"), std::string::npos);
    EXPECT_NE(repeat.find("submitPrivacyBlackFrame(g_LastFrame"), std::string::npos);
    EXPECT_NE(repeat.find("privacyRuntime.RepeatCacheIsBlack()"), std::string::npos);

    const size_t catchupBegin = source.find("const auto privacyDecision = evaluateScreenGrabPrivacy(&catchupFrame)");
    const size_t catchupEnd = source.find("recoveredCatchupEncodeFailure", catchupBegin);
    ASSERT_NE(catchupBegin, std::string::npos);
    ASSERT_NE(catchupEnd, std::string::npos);
    EXPECT_NE(source.substr(catchupBegin, catchupEnd - catchupBegin).find("submitPrivacyBlackFrame(catchupFrame"),
              std::string::npos);

    const size_t vfrBegin = source.find("if (!frameToProcess && useScreenGrab && config.video.useVFR");
    const size_t vfrEnd = source.find("if ((!frameToProcess || wantsTrueRepeatLastFrame)", vfrBegin);
    ASSERT_NE(vfrBegin, std::string::npos);
    ASSERT_NE(vfrEnd, std::string::npos);
    const std::string vfr = source.substr(vfrBegin, vfrEnd - vfrBegin);
    EXPECT_NE(vfr.find("evaluateScreenGrabPrivacy(nullptr)"), std::string::npos);
    EXPECT_NE(vfr.find("submitPrivacyBlackFrame(g_LastFrame"), std::string::npos);

    const size_t freshBegin = source.find("const auto privacyDecision = evaluateScreenGrabPrivacy(frameToProcess)");
    const size_t freshEnd = source.find("encodeDeferred = false", freshBegin);
    ASSERT_NE(freshBegin, std::string::npos);
    ASSERT_NE(freshEnd, std::string::npos);
    EXPECT_NE(source.substr(freshBegin, freshEnd - freshBegin).find("submitPrivacyBlackFrame(*frameToProcess"),
              std::string::npos);

    EXPECT_NE(source.find("privacyRuntime.ResetSource()"), std::string::npos);
    EXPECT_NE(source.find("privacyRuntime.PrepareTexture(frame.texture)"), std::string::npos);
}

TEST(CaptureCoordinatorSourceTest, ScreenGrabPrivacyBlackFailureCannotRevealCachedPixelsOrCursor) {
    const std::string runtime = ReadPrivacyRuntimeSource();
    ASSERT_FALSE(runtime.empty());

    EXPECT_NE(runtime.find("repeatCacheIsBlack_ && gate_.LastOutputWasBlack()"), std::string::npos);
    EXPECT_NE(runtime.find("ResetMediaRepeatCache();"), std::string::npos);
    EXPECT_NE(runtime.find("const ce::cursor::CaptureState hiddenCursor"), std::string::npos);
    EXPECT_NE(runtime.find("&hiddenCursor"), std::string::npos);
    EXPECT_NE(runtime.find("MediaEngine_ProcessFrameD3D11(blackTexture_.Get()"), std::string::npos);
}

TEST(CaptureCoordinatorSourceTest, ScreenGrabPrivacyUsesOnlyPassiveWindowStateQueries) {
    const std::string policy = ReadPrivacyPolicySource();
    const std::string runtime = ReadPrivacyRuntimeSource();
    ASSERT_FALSE(policy.empty());
    ASSERT_FALSE(runtime.empty());
    const std::string combined = policy + runtime;

    EXPECT_NE(policy.find("GetForegroundWindow()"), std::string::npos);
    EXPECT_NE(policy.find("GetWindowRect("), std::string::npos);
    EXPECT_NE(policy.find("GetMonitorInfo("), std::string::npos);
    for (const char* forbidden :
         {"OpenProcess(", "ReadProcessMemory(", "WriteProcessMemory(", "SetWindowsHookEx(", "SetWinEventHook(",
          "SendMessage(", "PostMessage(", "CreateRemoteThread(", "SendInput("}) {
        EXPECT_EQ(combined.find(forbidden), std::string::npos) << forbidden;
    }
}
