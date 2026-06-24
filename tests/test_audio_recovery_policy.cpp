#include <gtest/gtest.h>

#include "../mediaengine/audio_recovery_policy.h"

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
    EXPECT_TRUE(ce::audio::IsFatalWasapiStreamError(Hr(0x88890004)));   // AUDCLNT_E_DEVICE_INVALIDATED
    EXPECT_TRUE(ce::audio::IsFatalWasapiStreamError(Hr(0x88890010)));   // AUDCLNT_E_SERVICE_NOT_RUNNING
    EXPECT_TRUE(ce::audio::IsFatalWasapiStreamError(Hr(0x88890001)));   // AUDCLNT_E_NOT_INITIALIZED
    EXPECT_TRUE(ce::audio::IsFatalWasapiStreamError(Hr(0x8889000f)));   // AUDCLNT_E_ENDPOINT_CREATE_FAILED
}

TEST(AudioRecoveryPolicyTest, NonFatalAndSuccessAreNotTreatedAsFatal) {
    EXPECT_FALSE(ce::audio::IsFatalWasapiStreamError(0));               // S_OK
    EXPECT_FALSE(ce::audio::IsFatalWasapiStreamError(1));               // S_FALSE
    EXPECT_FALSE(ce::audio::IsFatalWasapiStreamError(Hr(0x88890006)));  // AUDCLNT_E_BUFFER_TOO_LARGE (transient)
    EXPECT_FALSE(ce::audio::IsFatalWasapiStreamError(Hr(0x8889000a)));  // AUDCLNT_E_DEVICE_IN_USE (not auto-recoverable)
    EXPECT_FALSE(ce::audio::IsFatalWasapiStreamError(Hr(0x80004005)));  // E_FAIL (generic)
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
    // Process has produced nothing yet (sawAnyPacket=false) -> never churn, even
    // after an arbitrarily long initial silence.
    EXPECT_FALSE(ce::audio::ShouldReactivateForSilentStall(/*sawAnyPacket*/ false, /*now*/ 1000000,
                                                           /*lastPacket*/ 0, /*lastReact*/ 0,
                                                           /*backoff*/ 0, cfg));
}

TEST(AudioRecoveryPolicyTest, SilentStallRequiresFullWindowThenFires) {
    ce::audio::StreamRecoveryConfig cfg;
    cfg.silentStallReactivateMs = 10000;

    // Stream delivered audio before; gone silent for less than the window -> wait.
    EXPECT_FALSE(ce::audio::ShouldReactivateForSilentStall(/*sawAnyPacket*/ true, /*now*/ 109000,
                                                           /*lastPacket*/ 100000, /*lastReact*/ 0,
                                                           /*backoff*/ 0, cfg));
    // Silent for the full window with no prior attempt -> fire.
    EXPECT_TRUE(ce::audio::ShouldReactivateForSilentStall(/*sawAnyPacket*/ true, /*now*/ 110000,
                                                          /*lastPacket*/ 100000, /*lastReact*/ 0,
                                                          /*backoff*/ 0, cfg));
}

TEST(AudioRecoveryPolicyTest, SilentStallStillHonorsBackoffBetweenAttempts) {
    ce::audio::StreamRecoveryConfig cfg;
    cfg.silentStallReactivateMs = 10000;

    // Window satisfied (15 s since last packet) but only 500 ms since the last
    // re-activation attempt and backoff is 1 s -> hold off.
    EXPECT_FALSE(ce::audio::ShouldReactivateForSilentStall(/*sawAnyPacket*/ true, /*now*/ 115000,
                                                           /*lastPacket*/ 100000, /*lastReact*/ 114500,
                                                           /*backoff*/ 1000, cfg));
    // Once backoff has elapsed, fire.
    EXPECT_TRUE(ce::audio::ShouldReactivateForSilentStall(/*sawAnyPacket*/ true, /*now*/ 116000,
                                                          /*lastPacket*/ 100000, /*lastReact*/ 114500,
                                                          /*backoff*/ 1000, cfg));
}

TEST(AudioRecoveryPolicyTest, SilentStallIgnoresBackwardsPacketClock) {
    ce::audio::StreamRecoveryConfig cfg;
    // now < lastPacketTick (clock skew) -> treat as not stalled.
    EXPECT_FALSE(ce::audio::ShouldReactivateForSilentStall(/*sawAnyPacket*/ true, /*now*/ 90000,
                                                           /*lastPacket*/ 100000, /*lastReact*/ 0,
                                                           /*backoff*/ 0, cfg));
}
