#include <gtest/gtest.h>

#include <filesystem>
#include <utility>

#include "common/logging/log_meter.h"
#include "mediaengine/audio/audio_sync_utils.h"
#include "source_fragment_reader.h"

TEST(AppAudioLatencyWarningTest, SessionBacklogWithinIntentionalDelayDoesNotWarn) {
    // Actual session observations in samples at 48 kHz: legacy absolute 250 ms
    // warnings fired for all of these, including zero excess and no underruns.
    constexpr int64_t rate = 48;
    for (const auto [delayMs, targetMs] : {std::pair{371, 430}, {335, 385}, {328, 374}, {399, 450}}) {
        EXPECT_FALSE(ce::audio::ShouldWarnAppAudioLatency(delayMs * rate, targetMs * rate, 50 * rate));
    }
}

TEST(AppAudioLatencyWarningTest, WarnsAtExcessBoundaryEvenWithShortAbsoluteDelay) {
    EXPECT_FALSE(ce::audio::ShouldWarnAppAudioLatency(4799, 2400, 2400));
    EXPECT_TRUE(ce::audio::ShouldWarnAppAudioLatency(4800, 2400, 2400));
    EXPECT_TRUE(ce::audio::ShouldWarnAppAudioLatency(24000, 16000, 2400));
    EXPECT_FALSE(ce::audio::ShouldWarnAppAudioLatency(0, 0, 2400));
    EXPECT_FALSE(ce::audio::ShouldWarnAppAudioLatency(4800, 2400, 0));
}

TEST(AppAudioLatencyWarningTest, WarningHeartbeatCountsRepeatsAndRecoveryResetsIt) {
    ce::log_meter::ChangeGate gate(5000);
    EXPECT_TRUE(gate.Observe(1, 10000).log);
    EXPECT_FALSE(gate.Observe(1, 10001).log);
    EXPECT_FALSE(gate.Observe(1, 14999).log);
    const auto heartbeat = gate.Observe(1, 15000);
    EXPECT_TRUE(heartbeat.log);
    EXPECT_EQ(heartbeat.suppressed, 2u);
    gate.Reset();
    EXPECT_TRUE(gate.Observe(1, 15001).log);
}

TEST(AppAudioLatencyWarningTest, ConsumePathUsesTargetRelativePolicyAndMetersRealWarnings) {
    const auto source = ce::test_source::ReadLogicalSource(
        std::filesystem::current_path() / "mediaengine/engine/mediaengine_audio_pull_mix_source.cpp");
    EXPECT_NE(source.find("ce::audio::ShouldWarnAppAudioLatency("), std::string::npos);
    EXPECT_EQ(source.find("appDelayMs >= kAppLatencyWarnMs"), std::string::npos);
    EXPECT_NE(source.find("src.appLatencyWarningLog->Observe("), std::string::npos);
    EXPECT_NE(source.find("warningLog.suppressed"), std::string::npos);
}
