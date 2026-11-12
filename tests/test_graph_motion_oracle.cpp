// The pixel oracle the FG flow scenarios judge graph motion with (tests/flow/graph_motion_oracle.h): it must find the
// true shift of a scrolling line and flag a graph that holds, catches up or steps back, from pixels alone.

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "tests/flow/graph_motion_oracle.h"

namespace {

using namespace ce::flow::graph_motion;

constexpr size_t kColumns = 400;

// The height of sample `slot`: a flat line with a three-sample spike every 120 samples.
double SampleHeight(long long slot) {
    return slot % 120 < 3 ? 40.0 : 100.0 + 2.0 * std::sin(static_cast<double>(slot) * 0.1);
}

// The graph as the renderer draws it, a polyline through the samples, scrolled `offset` pixels to the left, in quarter
// pixels.
std::vector<uint16_t> Profile(double offset, double pixelsPerSlot = 2.6, double scale = 1.0) {
    std::vector<uint16_t> profile(kColumns);
    for (size_t x = 0; x < kColumns; ++x) {
        const double slot = (static_cast<double>(x) + offset) / pixelsPerSlot;
        const auto before = static_cast<long long>(std::floor(slot));
        const double t = slot - static_cast<double>(before);
        const double height = SampleHeight(before) * (1.0 - t) + SampleHeight(before + 1) * t;
        profile[x] = static_cast<uint16_t>(std::lround(height * scale * 4.0));
    }
    return profile;
}

std::vector<Shift> Steps(const std::vector<double>& offsets) {
    std::vector<std::vector<uint16_t>> profiles;
    for (double offset : offsets)
        profiles.push_back(Profile(offset));
    std::vector<Shift> steps;
    for (size_t i = 1; i < profiles.size(); ++i)
        steps.push_back(EstimateShift(profiles[i - 1].data(), profiles[i].data(), kColumns, 16));
    return steps;
}

TEST(GraphMotionOracleTest, FindsTheTrueSubPixelShift) {
    const auto previous = Profile(0.0);
    for (double shift : {0.0, 1.0, 2.6, 5.2, 7.5}) {
        SCOPED_TRACE(shift);
        const auto current = Profile(shift);
        const Shift measured = EstimateShift(previous.data(), current.data(), kColumns, 16);
        ASSERT_TRUE(measured.measured);
        EXPECT_NEAR(measured.pixels, shift, 0.35);
    }
}

TEST(GraphMotionOracleTest, AFlatLineOrAScaleChangeIsNoMeasurement) {
    const std::vector<uint16_t> flat(kColumns, 400);
    EXPECT_FALSE(EstimateShift(flat.data(), flat.data(), kColumns, 16).measured);
    const auto previous = Profile(0.0);
    const auto rescaled = Profile(2.6, 2.6, 0.7);
    const Shift shift = EstimateShift(previous.data(), rescaled.data(), kColumns, 16);
    EXPECT_FALSE(shift.measured);
    EXPECT_TRUE(shift.rescaled);
}

TEST(GraphMotionOracleTest, UniformScrollingPasses) {
    std::vector<double> offsets;
    for (int i = 0; i < 100; ++i)
        offsets.push_back(i * 2.6);
    const Verdict verdict = JudgeSteps(Steps(offsets), {}, 0.35);
    EXPECT_EQ(verdict.measured, 99u);
    EXPECT_EQ(verdict.holds + verdict.jumps + verdict.backwards, 0u) << verdict.firstViolation;
    EXPECT_NEAR(verdict.medianPixels, 2.6, 0.2);
}

// GTA's menu symptom as the screen shows it: one output repeats the graph, the next catches up two slots.
TEST(GraphMotionOracleTest, AHoldAndCatchUpIsFlagged) {
    std::vector<double> offsets;
    double offset = 0.0;
    for (int i = 0; i < 100; ++i) {
        offsets.push_back(offset);
        offset += i % 10 == 4 ? 0.0 : (i % 10 == 5 ? 5.2 : 2.6);
    }
    const Verdict verdict = JudgeSteps(Steps(offsets), {}, 0.35);
    EXPECT_EQ(verdict.holds, 10u);
    EXPECT_EQ(verdict.jumps, 10u);
    ASSERT_FALSE(verdict.violations.empty());
    EXPECT_EQ(verdict.violations.front(), 4u);
}

TEST(GraphMotionOracleTest, BackwardMotionIsFlagged) {
    // Rewinding is a negative shift, which the search (0..16 px) never explains: the step is unmeasured or a hold,
    // never a pass.
    std::vector<double> offsets;
    for (int i = 0; i < 40; ++i)
        offsets.push_back(i == 20 ? 19 * 2.6 - 2.6 : i * 2.6);
    const Verdict verdict = JudgeSteps(Steps(offsets), {}, 0.35);
    EXPECT_GT(verdict.holds + verdict.jumps + verdict.backwards + verdict.rescaled, 0u);
}

// Steps of outputs that stand for different amounts of time are judged against that time.
TEST(GraphMotionOracleTest, ExpectedRatiosNormalizeLongOutputs) {
    std::vector<double> offsets;
    std::vector<double> ratios;
    double offset = 0.0;
    for (int i = 0; i < 60; ++i) {
        offsets.push_back(offset);
        const double ratio = i % 15 == 7 ? 2.0 : 1.0;
        ratios.push_back(ratio);
        offset += 2.6 * ratio;
    }
    ratios.pop_back();
    EXPECT_EQ(JudgeSteps(Steps(offsets), ratios, 0.35).jumps, 0u);
    EXPECT_GT(JudgeSteps(Steps(offsets), {}, 0.35).jumps, 0u);
}

}  // namespace
