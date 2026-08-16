#include <gtest/gtest.h>

#include <cwchar>

#include "../common/config.h"
#include "../common/inject_overlay_policy.h"
#include "../common/recording_lifecycle.h"
#include "../common/recording_indicator_policy.h"
#include "../common/shared_defs.h"

TEST(CaptureStateTest, RuntimeFlagsRoundTrip) {
    CaptureState state;

    EXPECT_FALSE(state.HasRuntimeFlag(kCaptureRuntimeFlagVulkanOverlayActive));

    state.SetRuntimeFlag(kCaptureRuntimeFlagVulkanOverlayActive, true);
    EXPECT_TRUE(state.HasRuntimeFlag(kCaptureRuntimeFlagVulkanOverlayActive));

    state.SetRuntimeFlag(kCaptureRuntimeFlagVulkanOverlayActive, false);
    EXPECT_FALSE(state.HasRuntimeFlag(kCaptureRuntimeFlagVulkanOverlayActive));
}

TEST(CaptureStateTest, RecordingStartIntentUpdatesAtomicallyAndPreservesUnrelatedFlags) {
    CaptureState state;
    state.SetRuntimeFlag(kCaptureRuntimeFlagInjectOverlayActive, true);

    EXPECT_EQ(state.GetRecordingStartIntent(), RecordingStartIntent::Idle);
    state.SetRecordingStartIntent(RecordingStartIntent::Video);
    EXPECT_EQ(state.GetRecordingStartIntent(), RecordingStartIntent::Video);
    EXPECT_TRUE(state.HasRuntimeFlag(kCaptureRuntimeFlagInjectOverlayActive));

    state.SetRecordingStartIntent(RecordingStartIntent::AudioOnly);
    EXPECT_EQ(state.GetRecordingStartIntent(), RecordingStartIntent::AudioOnly);
    EXPECT_TRUE(state.HasRuntimeFlag(kCaptureRuntimeFlagRecordingStartPending));
    EXPECT_TRUE(state.HasRuntimeFlag(kCaptureRuntimeFlagRecordingStartAudioOnly));
    EXPECT_TRUE(state.HasRuntimeFlag(kCaptureRuntimeFlagInjectOverlayActive));

    state.SetRecordingStartIntent(RecordingStartIntent::Idle);
    EXPECT_EQ(state.GetRecordingStartIntent(), RecordingStartIntent::Idle);
    EXPECT_FALSE(state.HasRuntimeFlag(kCaptureRuntimeFlagRecordingStartPending));
    EXPECT_FALSE(state.HasRuntimeFlag(kCaptureRuntimeFlagRecordingStartAudioOnly));
    EXPECT_TRUE(state.HasRuntimeFlag(kCaptureRuntimeFlagInjectOverlayActive));
}

TEST(CaptureStateTest, ScreenGrabTargetSnapshotIsSeparateAndClearsAtomically) {
    CaptureState state;
    ScreenGrabTargetSnapshot snapshot;

    ASSERT_TRUE(state.ReadScreenGrabTarget(snapshot));
    EXPECT_FALSE(snapshot.active);
    EXPECT_EQ(snapshot.processId, 0u);

    state.PublishScreenGrabTarget(4242, static_cast<int32_t>(0x89ABCDEFu), 0x01234567, true);
    ASSERT_TRUE(state.ReadScreenGrabTarget(snapshot));
    EXPECT_TRUE(snapshot.active);
    EXPECT_EQ(snapshot.processId, 4242u);
    EXPECT_EQ(static_cast<uint32_t>(snapshot.adapterLuidLow), 0x89ABCDEFu);
    EXPECT_EQ(snapshot.adapterLuidHigh, 0x01234567);

    state.PublishScreenGrabTarget(0, 0, 0, false);
    ASSERT_TRUE(state.ReadScreenGrabTarget(snapshot));
    EXPECT_FALSE(snapshot.active);
    EXPECT_EQ(snapshot.processId, 0u);
    EXPECT_EQ(snapshot.adapterLuidLow, 0);
    EXPECT_EQ(snapshot.adapterLuidHigh, 0);
}

TEST(CaptureStateTest, ScreenGrabTargetPublicationCannotMasqueradeAsAHookSource) {
    SharedMemoryLayout sharedMemory;
    sharedMemory.SetSourcePid(0);
    sharedMemory.SetLuidSourcePid(0);

    sharedMemory.runtimeState.PublishScreenGrabTarget(4242, 0x11223344, 0x55667788, true);

    EXPECT_EQ(sharedMemory.GetSourcePid(), 0u);
    EXPECT_EQ(sharedMemory.GetLuidSourcePid(), 0u);
    ScreenGrabTargetSnapshot snapshot;
    ASSERT_TRUE(sharedMemory.runtimeState.ReadScreenGrabTarget(snapshot));
    EXPECT_TRUE(snapshot.active);
    EXPECT_EQ(snapshot.processId, 4242u);
}

TEST(RecordingIndicatorPolicyTest, LiveRecordingTakesPrecedenceOverStaleStartIntent) {
    using ce::recording_indicator::SelectState;
    using ce::recording_indicator::State;

    EXPECT_EQ(SelectState(false, false, RecordingStartIntent::Idle), State::Idle);
    EXPECT_EQ(SelectState(false, false, RecordingStartIntent::Video), State::StartingVideo);
    EXPECT_EQ(SelectState(false, false, RecordingStartIntent::AudioOnly), State::StartingAudio);
    EXPECT_EQ(SelectState(true, false, RecordingStartIntent::AudioOnly), State::RecordingVideo);
    EXPECT_EQ(SelectState(true, true, RecordingStartIntent::Video), State::RecordingAudio);
    EXPECT_STREQ(ce::recording_indicator::GetStartingText(State::StartingVideo), "STARTING RECORDING...");
    EXPECT_STREQ(ce::recording_indicator::GetStartingText(State::StartingAudio), "STARTING AUDIO...");
}

TEST(CaptureStateTest, CaptureRequestAndRecordingVisibilityAreIndependent) {
    CaptureState state;

    EXPECT_FALSE(state.captureRequested.load(std::memory_order_relaxed));
    EXPECT_FALSE(state.isRecording.load(std::memory_order_relaxed));

    state.captureRequested.store(true, std::memory_order_relaxed);
    EXPECT_TRUE(state.captureRequested.load(std::memory_order_relaxed));
    EXPECT_FALSE(state.isRecording.load(std::memory_order_relaxed));

    state.isRecording.store(true, std::memory_order_relaxed);
    EXPECT_TRUE(state.captureRequested.load(std::memory_order_relaxed));
    EXPECT_TRUE(state.isRecording.load(std::memory_order_relaxed));

    state.captureRequested.store(false, std::memory_order_relaxed);
    EXPECT_FALSE(state.captureRequested.load(std::memory_order_relaxed));
    EXPECT_TRUE(state.isRecording.load(std::memory_order_relaxed));
}

TEST(CaptureStateTest, InjectVideoRequestRequiresSessionAndActiveInjectPath) {
    CaptureState state;

    EXPECT_FALSE(state.IsInjectVideoCaptureRequested());

    state.captureRequested.store(true, std::memory_order_release);
    EXPECT_FALSE(state.IsInjectVideoCaptureRequested());

    state.SetRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested, true);
    EXPECT_TRUE(state.IsInjectVideoCaptureRequested());

    state.captureRequested.store(false, std::memory_order_release);
    EXPECT_FALSE(state.IsInjectVideoCaptureRequested());
    EXPECT_TRUE(state.HasRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested));
}

TEST(CaptureStateTest, WgcDiagnosticsFieldsDefaultToZero) {
    CaptureState state;

    EXPECT_EQ(state.wgcDeliveredFramesPerSec.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcDeliveredMin250Fps.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcDeliveredMin500Fps.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcInputMin250Fps.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcInputMin500Fps.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcSelectionErrorAvgUs.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcSelectionErrorMaxUs.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcSelectionErrorSignedAvgUs.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(state.wgcSelectionEarlyMaxUs.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcSelectionLateMaxUs.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcQueueEmptyTickPermille.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcBufferedAtTickAvgPermille.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcBufferedAtTickMin.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcStarvedTickCount.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcSingleFrameTickCount.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.recordingHealthFlags.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.recordingTimelineDebtMs.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.recordingPeakTimelineDebtMs.load(std::memory_order_relaxed), 0u);
}

TEST(CaptureStateTest, WgcTelemetryFieldsRepresentFreshnessAndReserveCounters) {
    CaptureState state;

    state.wgcQueueEmptyTickPermille.store(375, std::memory_order_relaxed);
    state.wgcStarvedTickCount.store(9, std::memory_order_relaxed);
    state.wgcSingleFrameTickCount.store(15, std::memory_order_relaxed);
    state.wgcSelectionErrorAvgUs.store(2200, std::memory_order_relaxed);
    state.wgcSelectionErrorMaxUs.store(8100, std::memory_order_relaxed);
    state.wgcSelectionErrorSignedAvgUs.store(-1300, std::memory_order_relaxed);
    state.wgcSelectionEarlyMaxUs.store(2700, std::memory_order_relaxed);
    state.wgcSelectionLateMaxUs.store(4500, std::memory_order_relaxed);

    EXPECT_EQ(state.wgcQueueEmptyTickPermille.load(std::memory_order_relaxed), 375u);
    EXPECT_EQ(state.wgcStarvedTickCount.load(std::memory_order_relaxed), 9u);
    EXPECT_EQ(state.wgcSingleFrameTickCount.load(std::memory_order_relaxed), 15u);
    EXPECT_EQ(state.wgcSelectionErrorAvgUs.load(std::memory_order_relaxed), 2200u);
    EXPECT_EQ(state.wgcSelectionErrorMaxUs.load(std::memory_order_relaxed), 8100u);
    EXPECT_EQ(state.wgcSelectionErrorSignedAvgUs.load(std::memory_order_relaxed), -1300);
    EXPECT_EQ(state.wgcSelectionEarlyMaxUs.load(std::memory_order_relaxed), 2700u);
    EXPECT_EQ(state.wgcSelectionLateMaxUs.load(std::memory_order_relaxed), 4500u);
}

TEST(SharedDefsTest, DlssFrameGenerationHelpersClampToSupportedRange) {
    EXPECT_EQ(NormalizeDLSSFGFactor(1), 0);
    EXPECT_EQ(NormalizeDLSSFGFactor(2), 2);
    EXPECT_EQ(NormalizeDLSSFGFactor(4), 4);
    EXPECT_EQ(NormalizeDLSSFGFactor(5), 0);

    EXPECT_EQ(DLSSFGMultiplierToGeneratedFrames(0), 0u);
    EXPECT_EQ(DLSSFGMultiplierToGeneratedFrames(2), 1u);
    EXPECT_EQ(DLSSFGMultiplierToGeneratedFrames(4), 3u);

    EXPECT_EQ(StreamlineGeneratedFramesToDLSSFGMultiplier(0), 0);
    EXPECT_EQ(StreamlineGeneratedFramesToDLSSFGMultiplier(1), 2);
    EXPECT_EQ(StreamlineGeneratedFramesToDLSSFGMultiplier(3), 4);
    EXPECT_EQ(StreamlineGeneratedFramesToDLSSFGMultiplier(4), 0);
}

TEST(SharedDefsTest, NameGeneratorsIncludeExpectedPidFormatting) {
    wchar_t sharedMemName[64]{};
    wchar_t shutdownEventName[64]{};
    wchar_t shmemName[64]{};
    wchar_t hostStoppingEventName[64]{};
    wchar_t injectReactivateEventName[64]{};
    wchar_t vulkanReactivateEventName[64]{};
    wchar_t injectDormantEventName[64]{};
    wchar_t vulkanDormantEventName[64]{};

    GenerateSharedMemName(sharedMemName, std::size(sharedMemName), 0x1234ABCDu);
    GenerateShutdownEventName(shutdownEventName, std::size(shutdownEventName), 0x89ABCDEFu);
    GenerateShmemName(shmemName, std::size(shmemName), 0x00ABCDEFu);
    GenerateInjectHostStoppingEventName(hostStoppingEventName, std::size(hostStoppingEventName));
    GenerateInjectReactivateEventName(injectReactivateEventName, std::size(injectReactivateEventName), 0x1234ABCDu);
    GenerateVulkanReactivateEventName(vulkanReactivateEventName, std::size(vulkanReactivateEventName), 0x1234ABCDu);
    GenerateInjectDormantEventName(injectDormantEventName, std::size(injectDormantEventName), 0x1234ABCDu);
    GenerateVulkanDormantEventName(vulkanDormantEventName, std::size(vulkanDormantEventName), 0x1234ABCDu);

    EXPECT_EQ(std::wcscmp(sharedMemName, L"Local\\CE_SM_42_1234ABCD"), 0);
    EXPECT_EQ(std::wcscmp(SHARED_MEM_DISCOVERY, L"Local\\CE_Disc_42"), 0);
    EXPECT_EQ(std::wcscmp(shutdownEventName, L"Local\\CE_Shutdown_89ABCDEF"), 0);
    EXPECT_EQ(std::wcscmp(shmemName, L"Local\\CE_SHM_00ABCDEF"), 0);
    EXPECT_EQ(std::wcscmp(hostStoppingEventName, L"Local\\CE_InjectHostStopping_42"), 0);
    EXPECT_EQ(std::wcscmp(injectReactivateEventName, L"Local\\CE_InjectReactivate_42_1234ABCD"), 0);
    EXPECT_EQ(std::wcscmp(vulkanReactivateEventName, L"Local\\CE_VulkanReactivate_42_1234ABCD"), 0);
    EXPECT_EQ(std::wcscmp(injectDormantEventName, L"Local\\CE_InjectDormant_42_1234ABCD"), 0);
    EXPECT_EQ(std::wcscmp(vulkanDormantEventName, L"Local\\CE_VulkanDormant_42_1234ABCD"), 0);
}

TEST(SharedDefsTest, DiscoveryRequiresExactPublishedBuildIdentity) {
    DiscoveryInfo discovery;
    EXPECT_FALSE(ValidateDiscoveryInfo(&discovery));

    discovery.SetInjectPid(1234);
    discovery.SetMagic(DISCOVERY_MAGIC);
    EXPECT_TRUE(ValidateDiscoveryInfo(&discovery));

    discovery.SetBuildNumber(GetCurrentBuildNumber() ^ 1u);
    EXPECT_FALSE(ValidateDiscoveryInfo(&discovery));
    discovery.SetBuildNumber(GetCurrentBuildNumber());
    EXPECT_TRUE(ValidateDiscoveryInfo(&discovery));

    discovery.SetMagic(0);
    EXPECT_FALSE(ValidateDiscoveryInfo(&discovery));
}

TEST(SharedDefsTest, CapturePipelinePhaseStringCoversKnownAndUnknownValues) {
    EXPECT_STREQ(CapturePipelinePhaseToString(CapturePipelinePhase::kIdle), "idle");
    EXPECT_STREQ(CapturePipelinePhaseToString(CapturePipelinePhase::kWarmup), "warmup");
    EXPECT_STREQ(CapturePipelinePhaseToString(CapturePipelinePhase::kLive), "live");
    EXPECT_STREQ(CapturePipelinePhaseToString(CapturePipelinePhase::kDrain), "drain");
    EXPECT_STREQ(CapturePipelinePhaseToString(CapturePipelinePhase::kStopping), "stopping");
    EXPECT_STREQ(CapturePipelinePhaseToString(CapturePipelinePhase::kCancelling), "cancelling");
    EXPECT_STREQ(CapturePipelinePhaseToString(999u), "unknown");
}

TEST(SharedDefsTest, RecordingStopCancelsOnlyBeforeLiveOutputExists) {
    using ce::recording_lifecycle::SelectStopTransition;

    EXPECT_EQ(SelectStopTransition(CapturePipelinePhase::kIdle, 0), CapturePipelinePhase::kCancelling);
    EXPECT_EQ(SelectStopTransition(CapturePipelinePhase::kWarmup, 0), CapturePipelinePhase::kCancelling);
    EXPECT_EQ(SelectStopTransition(CapturePipelinePhase::kLive, 0), CapturePipelinePhase::kStopping);
    EXPECT_EQ(SelectStopTransition(CapturePipelinePhase::kWarmup, 1), CapturePipelinePhase::kStopping);
    EXPECT_EQ(SelectStopTransition(CapturePipelinePhase::kDrain, 0), CapturePipelinePhase::kStopping);
    EXPECT_EQ(SelectStopTransition(CapturePipelinePhase::kCancelling, 0), CapturePipelinePhase::kCancelling);
    EXPECT_EQ(SelectStopTransition(CapturePipelinePhase::kStopping, 0), CapturePipelinePhase::kStopping);
}

TEST(SharedDefsTest, StopAndLiveCommitHaveOneAtomicWinner) {
    std::atomic<uint32_t> phase{static_cast<uint32_t>(CapturePipelinePhase::kIdle)};
    std::atomic<bool> requested{true};

    ASSERT_TRUE(ce::recording_lifecycle::TryArmWarmup(phase, requested));
    requested.store(false, std::memory_order_release);
    EXPECT_EQ(ce::recording_lifecycle::BeginStop(phase, 0), CapturePipelinePhase::kCancelling);
    EXPECT_FALSE(ce::recording_lifecycle::TryCommitLive(phase, requested));
    EXPECT_EQ(static_cast<CapturePipelinePhase>(phase.load()), CapturePipelinePhase::kCancelling);

    phase.store(static_cast<uint32_t>(CapturePipelinePhase::kIdle));
    requested.store(true, std::memory_order_release);
    ASSERT_TRUE(ce::recording_lifecycle::TryArmWarmup(phase, requested));
    ASSERT_TRUE(ce::recording_lifecycle::TryCommitLive(phase, requested));
    requested.store(false, std::memory_order_release);
    EXPECT_EQ(ce::recording_lifecycle::BeginStop(phase, 0), CapturePipelinePhase::kStopping);
}

TEST(SharedDefsTest, OverlayConfigSeqlockPublishesStableSnapshot) {
    SharedMemoryLayout sharedMemory;
    EXPECT_EQ(sharedMemory.overlayConfigSeq.load(std::memory_order_relaxed), 0u);

    sharedMemory.BeginWriteOverlayConfig();
    const uint32_t lockedSeq = sharedMemory.overlayConfigSeq.load(std::memory_order_relaxed);
    EXPECT_EQ(lockedSeq & 1u, 1u);
    sharedMemory.overlayConfig.showOverlay = true;
    sharedMemory.overlayConfig.observerOnly = true;
    sharedMemory.overlayConfig.observerPolicyOnly = true;
    sharedMemory.overlayConfig.observerStartupPresentOnly = true;
    sharedMemory.overlayConfig.padding = 18;
    sharedMemory.overlayConfig.fontSize = 22.5f;
    sharedMemory.EndWriteOverlayConfig();

    const uint32_t publishedSeq = sharedMemory.overlayConfigSeq.load(std::memory_order_relaxed);
    EXPECT_EQ(publishedSeq & 1u, 0u);

    const OverlayConfig snapshot = sharedMemory.ReadOverlayConfig();
    EXPECT_TRUE(snapshot.showOverlay);
    EXPECT_TRUE(snapshot.observerOnly);
    EXPECT_TRUE(snapshot.observerPolicyOnly);
    EXPECT_TRUE(snapshot.observerStartupPresentOnly);
    EXPECT_EQ(snapshot.padding, 18);
    EXPECT_FLOAT_EQ(snapshot.fontSize, 22.5f);
}

TEST(SharedDefsTest, ValidateSharedMemoryRequiresExplicitAbiPublication) {
    SharedMemoryLayout sharedMemory;
    EXPECT_FALSE(ValidateSharedMemory(&sharedMemory));

    sharedMemory.structSize.store(sizeof(SharedMemoryLayout), std::memory_order_relaxed);
    sharedMemory.abiSignature.store(SHARED_MEMORY_ABI_SIGNATURE, std::memory_order_relaxed);
    sharedMemory.SetMagic(SHARED_MEMORY_MAGIC);

    EXPECT_TRUE(ValidateSharedMemory(&sharedMemory));

    sharedMemory.SetMagic(0);
    EXPECT_FALSE(ValidateSharedMemory(&sharedMemory));
    sharedMemory.SetMagic(SHARED_MEMORY_MAGIC);

    sharedMemory.SetVersion(SHARED_MEMORY_VERSION + 1);
    EXPECT_FALSE(ValidateSharedMemory(&sharedMemory));
    sharedMemory.SetVersion(SHARED_MEMORY_VERSION);

    sharedMemory.structSize.store(sizeof(SharedMemoryLayout) - 1, std::memory_order_relaxed);
    EXPECT_FALSE(ValidateSharedMemory(&sharedMemory));
    sharedMemory.structSize.store(sizeof(SharedMemoryLayout), std::memory_order_relaxed);

    sharedMemory.abiSignature.store(SHARED_MEMORY_ABI_SIGNATURE ^ 1u, std::memory_order_relaxed);
    EXPECT_FALSE(ValidateSharedMemory(&sharedMemory));
}

TEST(SharedDefsTest, FrameRingWindowValidationHandlesWrapAndRejectsCorruption) {
    EXPECT_TRUE(IsFrameRingWindowValid(10, 10));
    EXPECT_TRUE(IsFrameRingWindowValid(20, 10));
    EXPECT_TRUE(IsFrameRingWindowValid(5, UINT32_MAX - 4));
    EXPECT_FALSE(IsFrameRingWindowValid(1000, 10));
    EXPECT_FALSE(IsFrameRingWindowValid(0x6C75765Bu, 0x6579616Cu));
}

TEST(InjectOverlayPolicyTest, WgcKeepsFullInjectionWhitelistIndependentOfVideoMethod) {
    AppConfig config;
    config.captureMethod = "wgc";
    config.gameWhitelist.push_back({.pattern = "game.exe"});

    const InjectorConfigState state = BuildInjectorConfigState(config);

    EXPECT_TRUE(state.allowInjection);
    ASSERT_EQ(state.config.gameWhitelist.size(), 1u);
    EXPECT_EQ(state.config.gameWhitelist[0].pattern, "game.exe");
    EXPECT_TRUE(state.config.overlayWhitelist.empty());
}

TEST(InjectOverlayPolicyTest, WgcKeepsFullAndOverlayOnlyTargetsSeparate) {
    AppConfig config;
    config.captureMethod = "wgc";
    config.gameWhitelist.push_back({.pattern = "game.exe"});
    config.overlayWhitelist.push_back({.pattern = "overlay-only.exe"});

    const InjectorConfigState state = BuildInjectorConfigState(config);

    EXPECT_TRUE(state.allowInjection);
    ASSERT_EQ(state.config.gameWhitelist.size(), 1u);
    EXPECT_EQ(state.config.gameWhitelist[0].pattern, "game.exe");
    ASSERT_EQ(state.config.overlayWhitelist.size(), 1u);
    EXPECT_EQ(state.config.overlayWhitelist[0].pattern, "overlay-only.exe");
}

TEST(InjectOverlayPolicyTest, DxgiDupKeepsFullInjectionWhitelistIndependentOfVideoMethod) {
    AppConfig config;
    config.captureMethod = "dxgi_dup";
    config.gameWhitelist.push_back({.pattern = "game.exe"});

    const InjectorConfigState state = BuildInjectorConfigState(config);

    EXPECT_TRUE(state.allowInjection);
    ASSERT_EQ(state.config.gameWhitelist.size(), 1u);
    EXPECT_EQ(state.config.gameWhitelist[0].pattern, "game.exe");
    EXPECT_TRUE(state.config.overlayWhitelist.empty());
}

TEST(InjectOverlayPolicyTest, AutoOverlayOnlyInjectionKeepsOverlayTargetForHooking) {
    AppConfig config;
    config.captureMethod = "auto";
    config.overlayWhitelist.push_back({.pattern = "overlay-only.exe"});

    const InjectorConfigState state = BuildInjectorConfigState(config);

    EXPECT_TRUE(state.allowInjection);
    EXPECT_TRUE(state.config.gameWhitelist.empty());
    ASSERT_EQ(state.config.overlayWhitelist.size(), 1u);
    EXPECT_EQ(state.config.overlayWhitelist[0].pattern, "overlay-only.exe");
}

TEST(InjectOverlayPolicyTest, AutoInjectionKeepsGameAndOverlayTargetsSeparate) {
    AppConfig config;
    config.captureMethod = "auto";
    config.gameWhitelist.push_back({.pattern = "capture-game.exe"});
    config.overlayWhitelist.push_back({.pattern = "overlay-only.exe"});

    const InjectorConfigState state = BuildInjectorConfigState(config);

    EXPECT_TRUE(state.allowInjection);
    ASSERT_EQ(state.config.gameWhitelist.size(), 1u);
    ASSERT_EQ(state.config.overlayWhitelist.size(), 1u);
    EXPECT_EQ(state.config.gameWhitelist[0].pattern, "capture-game.exe");
    EXPECT_EQ(state.config.overlayWhitelist[0].pattern, "overlay-only.exe");
}

TEST(InjectOverlayPolicyTest, CaptureMethodChangesDoNotChangeInjectionPolicyOrAutoTargets) {
    AppConfig autoConfig;
    autoConfig.captureMethod = "auto";
    autoConfig.gameWhitelist.push_back({.pattern = "capture-game.exe"});
    autoConfig.overlayWhitelist.push_back({.pattern = "overlay-only.exe"});

    AppConfig screenGrabConfig = autoConfig;
    screenGrabConfig.captureMethod = "wgc";

    const InjectorConfigState autoState = BuildInjectorConfigState(autoConfig);
    const InjectorConfigState screenGrabState = BuildInjectorConfigState(screenGrabConfig);

    EXPECT_TRUE(autoState.allowInjection);
    EXPECT_TRUE(screenGrabState.allowInjection);
    EXPECT_EQ(autoState.config.gameWhitelist, screenGrabState.config.gameWhitelist);
    EXPECT_EQ(autoState.config.overlayWhitelist, screenGrabState.config.overlayWhitelist);
    EXPECT_FALSE(ShouldRescanForConfigChange(autoConfig, autoState, screenGrabConfig, screenGrabState));
}

TEST(InjectOverlayPolicyTest, StandardInjectionKeepsGameWhitelist) {
    AppConfig config;
    config.captureMethod = "inject";
    config.gameWhitelist.push_back({.pattern = "game.exe"});

    const InjectorConfigState state = BuildInjectorConfigState(config);

    EXPECT_TRUE(state.allowInjection);
    ASSERT_EQ(state.config.gameWhitelist.size(), 1u);
    EXPECT_EQ(state.config.gameWhitelist[0].pattern, "game.exe");
}

TEST(InjectOverlayPolicyTest, RescanTracksWhitelistAndLoggingChanges) {
    AppConfig oldConfig;
    oldConfig.captureMethod = "inject";
    oldConfig.logLevel = LogLevel::Off;
    oldConfig.debugLogging = false;
    oldConfig.gameWhitelist.push_back({.pattern = "game.exe"});

    AppConfig sameConfig = oldConfig;
    EXPECT_FALSE(ShouldRescanForConfigChange(oldConfig, BuildInjectorConfigState(oldConfig), sameConfig,
                                             BuildInjectorConfigState(sameConfig)));

    AppConfig newConfig = oldConfig;
    newConfig.logLevel = LogLevel::Debug;
    newConfig.debugLogging = true;
    EXPECT_TRUE(ShouldRescanForConfigChange(oldConfig, BuildInjectorConfigState(oldConfig), newConfig,
                                            BuildInjectorConfigState(newConfig)));

    newConfig = oldConfig;
    newConfig.overlayWhitelist.push_back({.pattern = "overlay.dll"});
    EXPECT_TRUE(ShouldRescanForConfigChange(oldConfig, BuildInjectorConfigState(oldConfig), newConfig,
                                            BuildInjectorConfigState(newConfig)));
}

TEST(InjectOverlayPolicyTest, PseudoOverlaySuppressionMatchesPendingAndActiveFlags) {
    EXPECT_FALSE(ShouldSuppressPseudoOverlayForInjectOverlayHandoff(false, false));
    EXPECT_TRUE(ShouldSuppressPseudoOverlayForInjectOverlayHandoff(true, false));
    EXPECT_TRUE(ShouldSuppressPseudoOverlayForInjectOverlayHandoff(false, true));
    EXPECT_TRUE(ShouldSuppressPseudoOverlayForInjectOverlayHandoff(true, true));
}
