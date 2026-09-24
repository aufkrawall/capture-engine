#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../mediaengine/audio_recovery_policy.h"
#include "source_fragment_reader.h"

namespace {

// AUDCLNT_E_* HRESULTs are stored as negative HRESULTs (0x8889xxxx). Build them as
// signed long the same way the capture loop receives them from WASAPI.
constexpr long Hr(unsigned long code) {
    return static_cast<long>(code);
}

}  // namespace

// --- Fatal stream-error classification ---

TEST(AudioRecoveryPolicyTest, FatalErrorsAreDeviceInvalidatedFamilyOnly) {
    // Recoverable: the client is gone, re-activate from scratch.
    EXPECT_TRUE(ce::audio::IsFatalWasapiStreamError(Hr(0x88890004)));  // AUDCLNT_E_DEVICE_INVALIDATED
    EXPECT_TRUE(ce::audio::IsFatalWasapiStreamError(Hr(0x88890010)));  // AUDCLNT_E_SERVICE_NOT_RUNNING
    EXPECT_TRUE(ce::audio::IsFatalWasapiStreamError(Hr(0x88890001)));  // AUDCLNT_E_NOT_INITIALIZED
    EXPECT_TRUE(ce::audio::IsFatalWasapiStreamError(Hr(0x8889000f)));  // AUDCLNT_E_ENDPOINT_CREATE_FAILED
    EXPECT_TRUE(ce::audio::IsFatalWasapiStreamError(Hr(0x88890026)));  // AUDCLNT_E_RESOURCES_INVALIDATED
}

TEST(AudioRecoveryPolicyTest, NonFatalAndSuccessAreNotTreatedAsFatal) {
    EXPECT_FALSE(ce::audio::IsFatalWasapiStreamError(0));               // S_OK
    EXPECT_FALSE(ce::audio::IsFatalWasapiStreamError(1));               // S_FALSE
    EXPECT_FALSE(ce::audio::IsFatalWasapiStreamError(Hr(0x88890006)));  // AUDCLNT_E_BUFFER_TOO_LARGE (transient)
    EXPECT_FALSE(
        ce::audio::IsFatalWasapiStreamError(Hr(0x8889000a)));  // AUDCLNT_E_DEVICE_IN_USE (not auto-recoverable)
    EXPECT_FALSE(ce::audio::IsFatalWasapiStreamError(Hr(0x80004005)));  // E_FAIL (generic)
}

TEST(AudioRecoveryPolicyTest, UnqualifiedEventCaptureFallsBackAtDeadlineOnly) {
    ce::audio::StreamRecoveryConfig cfg;
    cfg.firstPacketEventFallbackMs = 1000;

    EXPECT_FALSE(ce::audio::ShouldFallbackUnqualifiedEventCapture(
        /*eventDriven=*/true, /*activationQualified=*/false, /*activationElapsedMs=*/999, cfg));
    EXPECT_TRUE(ce::audio::ShouldFallbackUnqualifiedEventCapture(
        /*eventDriven=*/true, /*activationQualified=*/false, /*activationElapsedMs=*/1000, cfg));
    EXPECT_FALSE(ce::audio::ShouldFallbackUnqualifiedEventCapture(
        /*eventDriven=*/false, /*activationQualified=*/false, /*activationElapsedMs=*/10000, cfg));
    EXPECT_FALSE(ce::audio::ShouldFallbackUnqualifiedEventCapture(
        /*eventDriven=*/true, /*activationQualified=*/true, /*activationElapsedMs=*/10000, cfg));
}

// --- Exponential backoff progression ---

TEST(AudioRecoveryPolicyTest, BackoffStartsAtBaseThenDoublesToCap) {
    ce::audio::StreamRecoveryConfig cfg;
    cfg.baseBackoffMs = 1000;
    cfg.maxBackoffMs = 30000;

    EXPECT_EQ(ce::audio::NextRecoveryBackoffMs(0, cfg), 1000u);
    EXPECT_EQ(ce::audio::NextRecoveryBackoffMs(1000, cfg), 2000u);
    EXPECT_EQ(ce::audio::NextRecoveryBackoffMs(2000, cfg), 4000u);
    EXPECT_EQ(ce::audio::NextRecoveryBackoffMs(8000, cfg), 16000u);
    EXPECT_EQ(ce::audio::NextRecoveryBackoffMs(16000, cfg), 30000u);  // 32000 clamped to cap
    EXPECT_EQ(ce::audio::NextRecoveryBackoffMs(30000, cfg), 30000u);  // stays at cap
}

// --- Backoff gate ---

TEST(AudioRecoveryPolicyTest, BackoffElapsedAllowsFirstAttemptImmediately) {
    // Never attempted before -> always allowed regardless of backoff.
    EXPECT_TRUE(ce::audio::RecoveryBackoffElapsed(/*now*/ 0, /*last*/ 0, /*backoff*/ 30000));
    EXPECT_TRUE(ce::audio::RecoveryBackoffElapsed(/*now*/ 5000, /*last*/ 0, /*backoff*/ 1000));
}

TEST(AudioRecoveryPolicyTest, BackoffElapsedRespectsWindowAndBackwardsClock) {
    // Within the backoff window -> not yet.
    EXPECT_FALSE(ce::audio::RecoveryBackoffElapsed(/*now*/ 10500, /*last*/ 10000, /*backoff*/ 1000));
    // At/after the window -> allowed.
    EXPECT_TRUE(ce::audio::RecoveryBackoffElapsed(/*now*/ 11000, /*last*/ 10000, /*backoff*/ 1000));
    EXPECT_TRUE(ce::audio::RecoveryBackoffElapsed(/*now*/ 12000, /*last*/ 10000, /*backoff*/ 1000));
    // Clock ran backwards -> defer rather than fire early.
    EXPECT_FALSE(ce::audio::RecoveryBackoffElapsed(/*now*/ 9000, /*last*/ 10000, /*backoff*/ 1000));
}

// --- Silent-stall watchdog gate ---

TEST(AudioRecoveryPolicyTest, SilentStallNeverFiresBeforeAnyPacketSeen) {
    ce::audio::StreamRecoveryConfig cfg;  // defaults: 10 s window
    // The current activation has produced nothing -> never churn, even after an
    // arbitrarily long initial or post-recovery silence.
    EXPECT_FALSE(ce::audio::ShouldReactivateForSilentStall(/*activationQualified*/ false, /*now*/ 1000000,
                                                           /*lastPacket*/ 0, /*lastReact*/ 0,
                                                           /*backoff*/ 0, cfg));
}

TEST(AudioRecoveryPolicyTest, SilentStallRequiresFullWindowThenFires) {
    ce::audio::StreamRecoveryConfig cfg;
    cfg.silentStallReactivateMs = 10000;

    // Stream delivered audio before; gone silent for less than the window -> wait.
    EXPECT_FALSE(ce::audio::ShouldReactivateForSilentStall(/*activationQualified*/ true, /*now*/ 109000,
                                                           /*lastPacket*/ 100000, /*lastReact*/ 0,
                                                           /*backoff*/ 0, cfg));
    // Silent for the full window with no prior attempt -> fire.
    EXPECT_TRUE(ce::audio::ShouldReactivateForSilentStall(/*activationQualified*/ true, /*now*/ 110000,
                                                          /*lastPacket*/ 100000, /*lastReact*/ 0,
                                                          /*backoff*/ 0, cfg));
}

TEST(AudioRecoveryPolicyTest, SilentStallStillHonorsBackoffBetweenAttempts) {
    ce::audio::StreamRecoveryConfig cfg;
    cfg.silentStallReactivateMs = 10000;

    // Window satisfied (15 s since last packet) but only 500 ms since the last
    // re-activation attempt and backoff is 1 s -> hold off.
    EXPECT_FALSE(ce::audio::ShouldReactivateForSilentStall(/*activationQualified*/ true, /*now*/ 115000,
                                                           /*lastPacket*/ 100000, /*lastReact*/ 114500,
                                                           /*backoff*/ 1000, cfg));
    // Once backoff has elapsed, fire.
    EXPECT_TRUE(ce::audio::ShouldReactivateForSilentStall(/*activationQualified*/ true, /*now*/ 116000,
                                                          /*lastPacket*/ 100000, /*lastReact*/ 114500,
                                                          /*backoff*/ 1000, cfg));
}

TEST(AudioRecoveryPolicyTest, SilentStallIgnoresBackwardsPacketClock) {
    ce::audio::StreamRecoveryConfig cfg;
    // now < lastPacketTick (clock skew) -> treat as not stalled.
    EXPECT_FALSE(ce::audio::ShouldReactivateForSilentStall(/*activationQualified*/ true, /*now*/ 90000,
                                                           /*lastPacket*/ 100000, /*lastReact*/ 0,
                                                           /*backoff*/ 0, cfg));
}

// A default-following source stayed on the old endpoint after Windows switched
// the default output (volume flyout, a headset that became the default) and
// recorded silence for the rest of the recording.
TEST(AudioDefaultDeviceFollowTest, OnlyTheSourcesOwnFlowAndConsoleRoleAreFollowed) {
    using ce::audio::IsFollowedDefaultDeviceChange;
    constexpr int kRender = 0, kCapture = 1, kConsole = 0, kMultimedia = 1, kCommunications = 2;
    EXPECT_TRUE(IsFollowedDefaultDeviceChange(true, true, kRender, kConsole));
    EXPECT_TRUE(IsFollowedDefaultDeviceChange(true, false, kCapture, kConsole));
    EXPECT_FALSE(IsFollowedDefaultDeviceChange(true, true, kCapture, kConsole));
    EXPECT_FALSE(IsFollowedDefaultDeviceChange(true, false, kRender, kConsole));
    EXPECT_FALSE(IsFollowedDefaultDeviceChange(true, true, kRender, kMultimedia));
    EXPECT_FALSE(IsFollowedDefaultDeviceChange(true, true, kRender, kCommunications));
    // A source configured with an explicit device never follows the default.
    EXPECT_FALSE(IsFollowedDefaultDeviceChange(false, true, kRender, kConsole));
}

TEST(AudioDefaultDeviceFollowTest, SwitchesOnlyOntoADifferentEndpoint) {
    using ce::audio::ShouldSwitchToNewDefaultEndpoint;
    EXPECT_TRUE(ShouldSwitchToNewDefaultEndpoint(true, L"{0.0.0}.{speakers}", L"{0.0.0}.{headset}"));
    EXPECT_FALSE(ShouldSwitchToNewDefaultEndpoint(true, L"{0.0.0}.{speakers}", L"{0.0.0}.{speakers}"));
    // Waiting for a device: any default is worth taking.
    EXPECT_TRUE(ShouldSwitchToNewDefaultEndpoint(false, nullptr, L"{0.0.0}.{headset}"));
    // No default at all: keep what is running.
    EXPECT_FALSE(ShouldSwitchToNewDefaultEndpoint(true, L"{0.0.0}.{speakers}", nullptr));
    EXPECT_FALSE(ShouldSwitchToNewDefaultEndpoint(false, nullptr, L""));
    // Prefix ids are different endpoints.
    EXPECT_TRUE(ShouldSwitchToNewDefaultEndpoint(true, L"{a}", L"{a}x"));
}

// A missing/refused endpoint at start (unplugged, microphone privacy denial)
// dropped the source for the whole recording with only a log line, and the
// completion said "saved".
TEST(AudioDefaultDeviceFollowTest, CaptureWorkerKeepsRunningWithoutADeviceAndReportsIt) {
    const auto root = std::filesystem::current_path();
    const std::string loop = ce::test_source::ReadLogicalSource(root / "mediaengine/audio_capture_loop.cpp");
    const std::string endpoints = ce::test_source::ReadLogicalSource(root / "mediaengine/audio_capture_endpoints.cpp");
    const std::string capture = ce::test_source::ReadLogicalSource(root / "mediaengine/audio_capture.cpp");
    const std::string stop = ce::test_source::ReadLogicalSource(root / "mediaengine/mediaengine_recording_stop.cpp");
    const std::string config = ce::test_source::ReadLogicalSource(root / "mediaengine/mediaengine_config.cpp");
    ASSERT_FALSE(loop.empty());
    ASSERT_FALSE(endpoints.empty());
    EXPECT_NE(loop.find("startWithoutDevice = true;"), std::string::npos);
    EXPECT_NE(loop.find("RegisterEndpointListenerOnWorker();"), std::string::npos);
    EXPECT_NE(loop.find("attemptReactivate(\"default_device_changed\", 0);"), std::string::npos);
    EXPECT_NE(endpoints.find("RegisterEndpointNotificationCallback(&endpointListener_)"), std::string::npos);
    // Unregistered before the enumerator goes away.
    const size_t unregister = capture.find("UnregisterEndpointListenerOnWorker();");
    const size_t enumeratorRelease = capture.find("pEnumerator->Release();", unregister);
    ASSERT_NE(unregister, std::string::npos);
    EXPECT_NE(enumeratorRelease, std::string::npos);
    EXPECT_NE(stop.find("AudioSourcesLostTheirDevice()"), std::string::npos);
    EXPECT_NE(config.find("AudioSourcesLostTheirDevice()"), std::string::npos);
}
