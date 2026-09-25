#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>

#include "../common/capture_pipeline_policy.h"
#include "../common/cfr_rational_grid.h"
#include "../common/frame_timing_utils.h"
#include "source_fragment_reader.h"

namespace grid = ce::cfr_grid;
namespace policy = ce::capture_policy;

namespace {

constexpr int64_t kQpc10MHz = 10000000;

int64_t SlotOffset(uint64_t ticks, int64_t frequency, int fps) {
    int64_t offset = -1;
    EXPECT_TRUE(grid::TryGetSlotOffsetQpc(ticks, frequency, fps, &offset));
    return offset;
}

}  // namespace

TEST(CfrRationalGridTest, SlotOffsetIsFloorOfExactRationalPosition) {
    EXPECT_EQ(SlotOffset(0, kQpc10MHz, 120), 0);
    EXPECT_EQ(SlotOffset(1, kQpc10MHz, 120), 83333);
    EXPECT_EQ(SlotOffset(2, kQpc10MHz, 120), 166666);
    EXPECT_EQ(SlotOffset(3, kQpc10MHz, 120), 250000);
    EXPECT_EQ(SlotOffset(120, kQpc10MHz, 120), kQpc10MHz);
    EXPECT_EQ(SlotOffset(1, kQpc10MHz, 144), 69444);
    EXPECT_EQ(SlotOffset(144, kQpc10MHz, 144), kQpc10MHz);
}

// The regression: slot n was n * floor(qpcFrequency / fps), which runs the real-time
// grid fast against the exact n/fps packet timestamps and QPC-placed audio.
TEST(CfrRationalGridTest, WholeSecondsStayExactForHoursAtEveryCommonRate) {
    for (const int fps : {24, 30, 50, 60, 90, 100, 120, 144, 165, 240, 360}) {
        for (uint64_t seconds : {1ull, 60ull, 3600ull, 3ull * 3600ull, 24ull * 3600ull}) {
            EXPECT_EQ(SlotOffset(seconds * static_cast<uint64_t>(fps), kQpc10MHz, fps),
                      static_cast<int64_t>(seconds) * kQpc10MHz)
                << "fps=" << fps << " seconds=" << seconds;
        }
    }
    // What the truncated stride produced after one hour at 240 fps: 57.6 ms early.
    const int64_t truncatedHour240 = static_cast<int64_t>(240ull * 3600ull) * (kQpc10MHz / 240);
    EXPECT_EQ(3600ll * kQpc10MHz - truncatedHour240, 576000);
}

TEST(CfrRationalGridTest, SlotIndexIsTheExactInverseOfSlotOffset) {
    for (const int64_t frequency : {kQpc10MHz, int64_t{3579545}, int64_t{2929687}, int64_t{24000000}}) {
        for (const int fps : {30, 60, 120, 144, 240}) {
            for (uint64_t n = 0; n < 2000; ++n) {
                const int64_t offset = SlotOffset(n, frequency, fps);
                EXPECT_EQ(grid::GetSlotIndexAtOrBefore(offset, frequency, fps), n);
                if (n > 0) {
                    EXPECT_EQ(grid::GetSlotIndexAtOrBefore(offset - 1, frequency, fps), n - 1);
                }
            }
        }
    }
    EXPECT_EQ(grid::GetSlotIndexAtOrBefore(-5, kQpc10MHz, 120), 0u);
    EXPECT_EQ(grid::GetSlotIndexAtOrBefore(3600ll * kQpc10MHz, kQpc10MHz, 120), 120ull * 3600ull);
    EXPECT_EQ(grid::GetSlotIndexAtOrBefore(3600ll * kQpc10MHz - 1, kQpc10MHz, 120), 120ull * 3600ull - 1);
}

TEST(CfrRationalGridTest, IncrementalWakeIntervalsSumToTheExactGrid) {
    for (const int fps : {60, 120, 144, 240, 360}) {
        int64_t remainder = 0;
        int64_t position = 0;
        for (uint64_t n = 1; n <= static_cast<uint64_t>(fps) * 600ull; ++n) {
            const int64_t interval = grid::NextSlotIntervalQpc(kQpc10MHz, fps, remainder);
            EXPECT_GE(interval, kQpc10MHz / fps);
            EXPECT_LE(interval, kQpc10MHz / fps + 1);
            position += interval;
            if (n % 997 == 0 || n % static_cast<uint64_t>(fps) == 0) {
                ASSERT_EQ(position, SlotOffset(n, kQpc10MHz, fps)) << "fps=" << fps << " n=" << n;
            }
        }
    }
}

TEST(CfrRationalGridTest, InvalidInputAndOverflowFailClosed) {
    int64_t offset = 0;
    EXPECT_FALSE(grid::TryGetSlotOffsetQpc(1, 0, 120, &offset));
    EXPECT_FALSE(grid::TryGetSlotOffsetQpc(1, kQpc10MHz, 0, &offset));
    EXPECT_FALSE(grid::TryGetSlotOffsetQpc(1, kQpc10MHz, 120, nullptr));
    EXPECT_FALSE(grid::TryGetSlotOffsetQpc(std::numeric_limits<uint64_t>::max(), kQpc10MHz, 1, &offset));
    EXPECT_EQ(grid::GetSlotQpc(std::numeric_limits<int64_t>::max() - 5, 1, 10, 1, 77), 77);
    int64_t remainder = 3;
    EXPECT_EQ(grid::NextSlotIntervalQpc(kQpc10MHz, 0, remainder), 0);
    EXPECT_EQ(remainder, 0);
}

TEST(CfrRationalGridTest, OutputAndSelectionGridsHaveNoLongRunDrift) {
    constexpr int64_t kStart = 5000000;
    EXPECT_EQ(policy::GetNextCfrOutputQpc(kStart, 120ull * 3600ull, kQpc10MHz, 120, 0), kStart + 3600ll * kQpc10MHz);
    EXPECT_EQ(policy::GetNextInjectCfrOutputQpc(kStart, 3ull * 240ull * 3600ull, kQpc10MHz, 240, 0),
              kStart + 3ll * 3600ll * kQpc10MHz);
    // Selection grid: tick 1 at the grid start, tick k at (k - 1) exact slots after it.
    EXPECT_EQ(ComputeIdealOutputQpcOnRationalGrid(kStart, 1, kQpc10MHz, 120), kStart);
    EXPECT_EQ(ComputeIdealOutputQpcOnRationalGrid(kStart, 4, kQpc10MHz, 120), kStart + 250000);
    EXPECT_EQ(ComputeIdealOutputQpcOnRationalGrid(kStart, 1 + 144ll * 3600ll, kQpc10MHz, 144),
              kStart + 3600ll * kQpc10MHz);
    EXPECT_EQ(ComputeIdealOutputQpcOnRationalGrid(0, 4, kQpc10MHz, 120), 0);
    EXPECT_EQ(ComputeIdealOutputQpcOnRationalGrid(kStart, 0, kQpc10MHz, 120), kStart);
}

// Every absolute CFR slot position, wake deadline and elapsed-slot count in the
// encoder session must use the exact grid; the truncated interval is a duration only.
TEST(CfrRationalGridTest, EncoderSessionNeverStridesTheTruncatedInterval) {
    const std::string source = ce::test_source::ReadLogicalSource(std::filesystem::current_path() /
                                                                   "captureengine" / "media_main.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_EQ(source.find("nextSampleTime.QuadPart += targetIntervalTicks"), std::string::npos);
    EXPECT_EQ(source.find("nextSampleTime.QuadPart = now.QuadPart + targetIntervalTicks"), std::string::npos);
    EXPECT_EQ(source.find("nextSampleTime.QuadPart = liveStartQpc.QuadPart + targetIntervalTicks"), std::string::npos);
    EXPECT_EQ(source.find("static_cast<int64_t>(liveTicksOutput) * targetIntervalTicks"), std::string::npos);
    EXPECT_EQ(source.find("ComputeIdealOutputQpc(encoderGridStartQpc"), std::string::npos);
    EXPECT_EQ(source.find("liveTicksOutput, targetIntervalTicks, scheduledSampleQpc"), std::string::npos);
    EXPECT_NE(source.find("ce::cfr_grid::NextSlotIntervalQpc(qpcFreq.QuadPart, config.video.fps"), std::string::npos);
    EXPECT_NE(source.find("ce::cfr_grid::GetSlotIndexAtOrBefore(scheduledUntilQpc - liveStartQpc.QuadPart"),
              std::string::npos);
    EXPECT_NE(source.find("ComputeIdealOutputQpcOnRationalGrid(encoderGridStartQpc"), std::string::npos);
}
