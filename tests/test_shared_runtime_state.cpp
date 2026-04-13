#include <gtest/gtest.h>

#include <cwchar>

#include "../common/config.h"
#include "../common/inject_overlay_policy.h"
#include "../common/shared_defs.h"

TEST(CaptureStateTest, RuntimeFlagsRoundTrip) {
    CaptureState state;

    EXPECT_FALSE(state.HasRuntimeFlag(kCaptureRuntimeFlagVulkanOverlayActive));

    state.SetRuntimeFlag(kCaptureRuntimeFlagVulkanOverlayActive, true);
    EXPECT_TRUE(state.HasRuntimeFlag(kCaptureRuntimeFlagVulkanOverlayActive));

    state.SetRuntimeFlag(kCaptureRuntimeFlagVulkanOverlayActive, false);
    EXPECT_FALSE(state.HasRuntimeFlag(kCaptureRuntimeFlagVulkanOverlayActive));
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

TEST(CaptureStateTest, WgcDiagnosticsFieldsDefaultToZero) {
    CaptureState state;

    EXPECT_EQ(state.wgcDeliveredFramesPerSec.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcDeliveredMin250Fps.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcDeliveredMin500Fps.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcInputMin250Fps.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcInputMin500Fps.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcQueueEmptyTickPermille.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcBufferedAtTickAvgPermille.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcBufferedAtTickMin.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcStarvedTickCount.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcSingleFrameTickCount.load(std::memory_order_relaxed), 0u);
}

TEST(CaptureStateTest, WgcTelemetryFieldsRepresentFreshnessAndReserveCounters) {
    CaptureState state;

    state.wgcQueueEmptyTickPermille.store(375, std::memory_order_relaxed);
    state.wgcStarvedTickCount.store(9, std::memory_order_relaxed);
    state.wgcSingleFrameTickCount.store(15, std::memory_order_relaxed);

    EXPECT_EQ(state.wgcQueueEmptyTickPermille.load(std::memory_order_relaxed), 375u);
    EXPECT_EQ(state.wgcStarvedTickCount.load(std::memory_order_relaxed), 9u);
    EXPECT_EQ(state.wgcSingleFrameTickCount.load(std::memory_order_relaxed), 15u);
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

    GenerateSharedMemName(sharedMemName, std::size(sharedMemName), 0x1234ABCDu);
    GenerateShutdownEventName(shutdownEventName, std::size(shutdownEventName), 0x89ABCDEFu);
    GenerateShmemName(shmemName, std::size(shmemName), 0x00ABCDEFu);

    EXPECT_EQ(std::wcscmp(sharedMemName, L"Local\\CE_SM_1234ABCD"), 0);
    EXPECT_EQ(std::wcscmp(shutdownEventName, L"Local\\CE_Shutdown_89ABCDEF"), 0);
    EXPECT_EQ(std::wcscmp(shmemName, L"Local\\CE_SHM_00ABCDEF"), 0);
}

TEST(SharedDefsTest, CapturePipelinePhaseStringCoversKnownAndUnknownValues) {
    EXPECT_STREQ(CapturePipelinePhaseToString(CapturePipelinePhase::kIdle), "idle");
    EXPECT_STREQ(CapturePipelinePhaseToString(CapturePipelinePhase::kWarmup), "warmup");
    EXPECT_STREQ(CapturePipelinePhaseToString(CapturePipelinePhase::kLive), "live");
    EXPECT_STREQ(CapturePipelinePhaseToString(CapturePipelinePhase::kDrain), "drain");
    EXPECT_STREQ(CapturePipelinePhaseToString(CapturePipelinePhase::kStopping), "stopping");
    EXPECT_STREQ(CapturePipelinePhaseToString(999u), "unknown");
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

TEST(SharedDefsTest, ValidateSharedMemoryRejectsBadHeaderAndAcceptsDefaultLayout) {
    SharedMemoryLayout sharedMemory;
    sharedMemory.structSize.store(sizeof(SharedMemoryLayout), std::memory_order_relaxed);

    EXPECT_TRUE(ValidateSharedMemory(&sharedMemory));

    sharedMemory.SetMagic(0);
    EXPECT_FALSE(ValidateSharedMemory(&sharedMemory));
    sharedMemory.SetMagic(SHARED_MEMORY_MAGIC);

    sharedMemory.SetVersion(SHARED_MEMORY_VERSION + 1);
    EXPECT_FALSE(ValidateSharedMemory(&sharedMemory));
    sharedMemory.SetVersion(SHARED_MEMORY_VERSION);

    sharedMemory.structSize.store(sizeof(SharedMemoryLayout) - 1, std::memory_order_relaxed);
    EXPECT_FALSE(ValidateSharedMemory(&sharedMemory));
}

TEST(InjectOverlayPolicyTest, WgcWithoutOverlayTargetsDisablesInjection) {
    AppConfig config;
    config.captureMethod = "wgc";
    config.gameWhitelist.push_back({.pattern = "game.exe"});

    const InjectorConfigState state = BuildInjectorConfigState(config);

    EXPECT_FALSE(state.allowInjection);
    EXPECT_TRUE(state.config.gameWhitelist.empty());
    EXPECT_TRUE(state.config.overlayWhitelist.empty());
}

TEST(InjectOverlayPolicyTest, WgcOverlayOnlyInjectionKeepsOverlayTargetsOnly) {
    AppConfig config;
    config.captureMethod = "wgc";
    config.gameWhitelist.push_back({.pattern = "game.exe"});
    config.overlayWhitelist.push_back({.pattern = "SocialClubD3D12Renderer.dll"});

    const InjectorConfigState state = BuildInjectorConfigState(config);

    EXPECT_TRUE(state.allowInjection);
    EXPECT_TRUE(state.config.gameWhitelist.empty());
    ASSERT_EQ(state.config.overlayWhitelist.size(), 1u);
    EXPECT_EQ(state.config.overlayWhitelist[0].pattern, "SocialClubD3D12Renderer.dll");
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
