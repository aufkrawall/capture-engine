#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../captureengine/encoder_loop_stage_cost.h"
#include "source_fragment_reader.h"

namespace cost = ce::encoder_loop_cost;

namespace {

constexpr int64_t kFrameInterval = 83'333;  // 120 fps at a 10 MHz QPC

std::string ReadSessionSource() {
    return ce::test_source::ReadFile(std::filesystem::current_path() / "captureengine" /
                                     "media_main_encoder_00_session.cpp");
}

std::string FunctionBody(const std::string& source, const std::string& signature) {
    const size_t start = source.find(signature);
    if (start == std::string::npos) {
        return {};
    }
    const size_t open = source.find('{', start);
    int depth = 0;
    for (size_t index = open; index < source.size(); ++index) {
        if (source[index] == '{') {
            ++depth;
        } else if (source[index] == '}' && --depth == 0) {
            return source.substr(open, index - open + 1);
        }
    }
    return {};
}

}  // namespace

TEST(EncoderLoopStageCostTest, TimerWaitIsMovedOutOfTheContainingPhase) {
    cost::IterationCost iteration;
    const int64_t waitBefore = iteration.TimerWaitQpc();
    iteration.Charge(cost::Phase::kTimerWait, 70'000);  // recorded inside LoopCatchup
    iteration.ChargeCall(cost::Phase::kCatchup, 75'000, waitBefore);

    EXPECT_EQ(iteration.PhaseQpc(cost::Phase::kTimerWait), 70'000);
    EXPECT_EQ(iteration.PhaseQpc(cost::Phase::kCatchup), 5'000);
    EXPECT_EQ(iteration.WorkQpc(), 5'000);
}

TEST(EncoderLoopStageCostTest, PhasesAndWaitSumToIterationWallTime) {
    cost::IterationCost iteration;
    iteration.ChargeCall(cost::Phase::kStart, 1'000, iteration.TimerWaitQpc());
    const int64_t waitBefore = iteration.TimerWaitQpc();
    iteration.Charge(cost::Phase::kTimerWait, 60'000);
    iteration.ChargeCall(cost::Phase::kCatchup, 62'000, waitBefore);
    iteration.ChargeCall(cost::Phase::kEmit, 9'000, iteration.TimerWaitQpc());

    EXPECT_EQ(iteration.WorkQpc() + iteration.TimerWaitQpc(), 1'000 + 62'000 + 9'000);
}

TEST(EncoderLoopStageCostTest, LongTimerWaitAloneIsNotASlowIteration) {
    // An idle loop that sleeps a whole interval must never be reported.
    cost::IterationCost iteration;
    iteration.Charge(cost::Phase::kTimerWait, kFrameInterval * 10);
    iteration.Charge(cost::Phase::kEmit, 1'000);
    EXPECT_FALSE(iteration.IsSlow(kFrameInterval));
}

TEST(EncoderLoopStageCostTest, SessionStallShapeIsSlowAndNamesTheHoldingPhase) {
    // 20260926_012955 r0004: "Catchup budget exceeded at extraTick=1 (elapsed=174.60ms)".
    cost::IterationCost iteration;
    iteration.Charge(cost::Phase::kWgcSelect, 1'500);
    iteration.Charge(cost::Phase::kEmit, 1'746'000);
    iteration.Charge(cost::Phase::kHealth, 800);

    EXPECT_TRUE(iteration.IsSlow(kFrameInterval));
    EXPECT_EQ(iteration.DominantWorkPhase(), cost::Phase::kEmit);
    EXPECT_STREQ(cost::PhaseName(iteration.DominantWorkPhase()), "emit");
}

TEST(EncoderLoopStageCostTest, ThresholdScalesWithTheOutputRate) {
    EXPECT_EQ(cost::SlowIterationThresholdQpc(kFrameInterval), kFrameInterval * cost::kSlowIterationFrameIntervals);
    EXPECT_EQ(cost::SlowIterationThresholdQpc(0), 0);

    cost::IterationCost iteration;
    iteration.Charge(cost::Phase::kEncode, cost::SlowIterationThresholdQpc(kFrameInterval) - 1);
    EXPECT_FALSE(iteration.IsSlow(kFrameInterval));
    iteration.Charge(cost::Phase::kEncode, 1);
    EXPECT_TRUE(iteration.IsSlow(kFrameInterval));
    EXPECT_FALSE(iteration.IsSlow(0));  // unknown rate: never guess
}

TEST(EncoderLoopStageCostTest, TimerWaitNeverWinsDominance) {
    cost::IterationCost iteration;
    iteration.Charge(cost::Phase::kTimerWait, 900'000);
    iteration.Charge(cost::Phase::kHealth, 10);
    EXPECT_EQ(iteration.DominantWorkPhase(), cost::Phase::kHealth);
}

TEST(EncoderLoopStageCostTest, ResetClearsPhasesAndWakeLateness) {
    cost::IterationCost iteration;
    iteration.Charge(cost::Phase::kEmit, 5);
    iteration.NoteWakeLate(1'600'000);
    iteration.Reset();
    EXPECT_EQ(iteration.WorkQpc(), 0);
    EXPECT_EQ(iteration.WakeLateQpc(), 0);
    iteration.NoteWakeLate(-4);  // woke early
    EXPECT_EQ(iteration.WakeLateQpc(), 0);
}

TEST(EncoderLoopStageCostTest, LogGateBurstsThenSummarizesSuppressedIterations) {
    cost::SlowIterationLogGate gate;
    for (uint64_t index = 0; index < cost::SlowIterationLogGate::kBurstLines; ++index) {
        EXPECT_TRUE(gate.Observe(100, 1'000).log);
    }
    EXPECT_FALSE(gate.Observe(200, 2'000).log);
    EXPECT_FALSE(gate.Observe(300, 7'000).log);
    EXPECT_FALSE(gate.Observe(100 + cost::SlowIterationLogGate::kIntervalMs - 1, 3'000).log);

    const auto decision = gate.Observe(100 + cost::SlowIterationLogGate::kIntervalMs, 4'000);
    EXPECT_TRUE(decision.log);
    EXPECT_EQ(decision.total, cost::SlowIterationLogGate::kBurstLines + 4);
    EXPECT_EQ(decision.suppressed, 3u);
    EXPECT_EQ(decision.suppressedWorstQpc, 7'000);

    const auto next = gate.Observe(100 + 2 * cost::SlowIterationLogGate::kIntervalMs, 1);
    EXPECT_TRUE(next.log);
    EXPECT_EQ(next.suppressed, 0u);
    EXPECT_EQ(next.suppressedWorstQpc, 0);
}

// The encoder thread's MMCSS registration was a local of MediaEncoderSession::Init(), so it
// was reverted as soon as Init() returned and the recording loop ran at ordinary priority.
TEST(EncoderLoopStageCostTest, EncoderThreadQosSpansTheWholeSession) {
    const std::string source = ReadSessionSource();
    ASSERT_FALSE(source.empty());

    const std::string threadFunc = FunctionBody(source, "void EncoderThreadFunc(const AppConfig& config)");
    ASSERT_FALSE(threadFunc.empty());
    const size_t qos = threadFunc.find("ScopedMmcssTask ");
    const size_t run = threadFunc.find("session.Run()");
    ASSERT_NE(qos, std::string::npos);
    ASSERT_NE(run, std::string::npos);
    EXPECT_LT(qos, run);

    const std::string init = FunctionBody(source, "bool MediaEncoderSession::Init()");
    ASSERT_FALSE(init.empty());
    EXPECT_EQ(init.find("ScopedMmcssTask"), std::string::npos);
}

TEST(EncoderLoopStageCostTest, EveryLoopPhaseIsTimed) {
    const std::string run = FunctionBody(ReadSessionSource(), "void MediaEncoderSession::Run()");
    ASSERT_FALSE(run.empty());
    for (const char* step : {"LoopStart", "LoopPressure", "LoopCatchup", "LoopWgcTarget", "LoopWgcSelect",
                             "LoopStartup", "LoopEmit", "LoopEncode", "LoopHealth"}) {
        EXPECT_NE(run.find(std::string("&MediaEncoderSession::") + step), std::string::npos) << step;
    }
    EXPECT_NE(run.find("ChargeCall("), std::string::npos);
}

// Pre-live passes are one-time startup work (the 20260926_030958 recording logged two 72-200 ms
// "startup" passes before live output): INFO evidence only, WARN once the output is live.
TEST(EncoderLoopStageCostTest, SlowIterationWarnsOnlyWhileLive) {
    const std::string report = FunctionBody(ReadSessionSource(), "void ReportSlowEncoderIteration(");
    ASSERT_FALSE(report.empty());
    const size_t live = report.find("if (context.live) {");
    ASSERT_NE(live, std::string::npos);
    const size_t warn = report.find("LogWarn(", live);
    const size_t info = report.find("LogInfo(", live);
    ASSERT_NE(warn, std::string::npos);
    ASSERT_NE(info, std::string::npos);
    EXPECT_LT(warn, info);
}
