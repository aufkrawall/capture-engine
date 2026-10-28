#include "hook/present/present_flip_timing.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace ce::present_stall;

namespace {

std::unique_ptr<SharedDisplayTiming> ActiveStream() {
    auto timing = std::make_unique<SharedDisplayTiming>();
    timing->Reset(1, 0, DisplayTimingStatus::Active);
    return timing;
}

FlipDownCall Call(int64_t entryUs, int64_t exitUs, bool captured, uint64_t slowOrdinal = 0) {
    FlipDownCall call;
    call.entryUs = entryUs;
    call.exitUs = exitUs;
    call.captured = captured;
    call.slow = slowOrdinal != 0;
    call.logged = slowOrdinal != 0;
    call.slowOrdinal = slowOrdinal;
    return call;
}

void PublishFlip(SharedDisplayTiming& timing, int64_t startUs, int64_t submitUs, int64_t readyUs,
                 int64_t programmedUs, int64_t screenUs) {
    timing.Publish(screenUs, 0, startUs, true, 0, false, {submitUs, readyUs, programmedUs});
}

}  // namespace

// What released a blocked call is the latest milestone right before its return.
TEST(PresentFlipTimingTest, TheCallReturnedAfterTheLatestMilestoneJustBeforeItsReturn) {
    FlipTiming timing;
    timing.exitUs = 3000;
    timing.submitUs = 3010;  // a driver thread submitted after the return
    timing.readyUs = 3050;
    timing.screenUs = 6000;
    timing.previousScreenUs = 2900;
    EXPECT_EQ(ClassifyFlipWait(timing), FlipWait::kPreviousOnScreen);

    timing.programmedUs = 2950;
    EXPECT_EQ(ClassifyFlipWait(timing), FlipWait::kProgrammed);

    timing.programmedUs = kFlipNotObserved;
    timing.previousScreenUs = 2000;  // 1 ms before the return: not what released it
    EXPECT_EQ(ClassifyFlipWait(timing), FlipWait::kNone);

    timing.submitUs = 100;
    timing.exitUs = 200;
    EXPECT_EQ(ClassifyFlipWait(timing), FlipWait::kSubmit);
}

// Each published transition lands on the down-call whose span holds its
// runtime PresentStart; a frame a later one passed is counted, not invented.
TEST(PresentFlipTimingTest, TransitionsArePlacedOnTheirDownCallsFromCallEntry) {
    auto timing = ActiveStream();
    timing->Publish(500, 0, 400);  // published before the timer synchronized
    FlipTimer timer;
    FlipTiming out[8];
    EXPECT_EQ(timer.Harvest(*timing, out, std::size(out)), 0u);

    timer.NoteDownCall(Call(10'000, 10'300, true));
    timer.NoteDownCall(Call(17'000, 20'200, true, 3));
    timer.NoteDownCall(Call(24'000, 24'200, false));  // its frame never flips
    timer.NoteDownCall(Call(31'000, 31'250, false));
    PublishFlip(*timing, 10'050, 10'120, 10'200, 10'210, 10'400);
    PublishFlip(*timing, 17'040, 17'100, 17'150, 19'900, 20'150);
    PublishFlip(*timing, 31'010, 31'020, 31'100, 31'120, 31'500);
    timing->Publish(40'000, 0, 39'000);  // no down-call covers it

    ASSERT_EQ(timer.Harvest(*timing, out, std::size(out)), 3u);
    EXPECT_TRUE(out[0].captured);
    EXPECT_EQ(out[0].exitUs, 300);
    EXPECT_EQ(out[0].presentStartUs, 50);
    EXPECT_EQ(out[0].submitUs, 120);
    EXPECT_EQ(out[0].readyUs, 200);
    EXPECT_EQ(out[0].programmedUs, 210);
    EXPECT_EQ(out[0].screenUs, 400);
    EXPECT_EQ(out[0].previousScreenUs, kFlipNotObserved);
    EXPECT_EQ(out[0].wait, FlipWait::kProgrammed);

    EXPECT_TRUE(out[1].slow);
    EXPECT_EQ(out[1].slowOrdinal, 3u);
    EXPECT_EQ(out[1].exitUs, 3200);
    EXPECT_EQ(out[1].programmedUs, 2900);
    EXPECT_EQ(out[1].screenUs, 3150);
    EXPECT_EQ(out[1].previousScreenUs, 10'400 - 17'000);
    EXPECT_EQ(out[1].wait, FlipWait::kOnScreen);

    EXPECT_FALSE(out[2].captured);
    EXPECT_EQ(out[2].screenUs, 500);
    EXPECT_EQ(out[2].previousScreenUs, kFlipNotObserved);  // the call before it never flipped
    EXPECT_EQ(timer.unplaced(), 1u);
    EXPECT_EQ(timer.unmatched(), 1u);
}

TEST(PresentFlipTimingTest, AFullHarvestLeavesTheRestForTheNextOne) {
    auto timing = ActiveStream();
    FlipTimer timer;
    FlipTiming out[2];
    EXPECT_EQ(timer.Harvest(*timing, out, std::size(out)), 0u);
    for (int64_t i = 0; i < 3; ++i) {
        const int64_t entry = 10'000 + i * 7'000;
        timer.NoteDownCall(Call(entry, entry + 200, false));
        PublishFlip(*timing, entry + 20, entry + 30, entry + 100, entry + 110, entry + 400);
    }
    EXPECT_EQ(timer.Harvest(*timing, out, std::size(out)), 2u);
    ASSERT_EQ(timer.Harvest(*timing, out, std::size(out)), 1u);
    EXPECT_EQ(out[0].presentStartUs, 20);
    EXPECT_EQ(timer.Harvest(*timing, out, std::size(out)), 0u);
}

// A stream reset starts a new publication history: what it already holds
// belongs to no noted call, and later transitions are placed again.
TEST(PresentFlipTimingTest, AStreamResetResynchronizes) {
    auto timing = ActiveStream();
    FlipTimer timer;
    FlipTiming out[4];
    EXPECT_EQ(timer.Harvest(*timing, out, std::size(out)), 0u);
    timer.NoteDownCall(Call(10'000, 10'200, false));
    timing->Reset(1, 0, DisplayTimingStatus::Active);
    PublishFlip(*timing, 10'020, 10'030, 10'100, 10'110, 10'400);
    EXPECT_EQ(timer.Harvest(*timing, out, std::size(out)), 0u);

    timer.NoteDownCall(Call(17'000, 17'200, false));
    PublishFlip(*timing, 17'020, 17'030, 17'100, 17'110, 17'400);
    ASSERT_EQ(timer.Harvest(*timing, out, std::size(out)), 1u);
    EXPECT_EQ(out[0].screenUs, 400);
}

TEST(PresentFlipTimingTest, TheWindowSplitsCopiedFramesFromTheRestAndCountsWhatSlowCallsWaitedOn) {
    FlipTiming copied;
    copied.captured = true;
    copied.slow = true;
    copied.exitUs = 3200;
    copied.submitUs = 100;
    copied.readyUs = 150;
    copied.programmedUs = 2900;
    copied.screenUs = 3150;
    copied.wait = FlipWait::kOnScreen;
    FlipTiming plain;
    plain.exitUs = 200;
    plain.submitUs = 100;
    plain.readyUs = 150;
    plain.programmedUs = 160;
    plain.screenUs = 400;
    plain.wait = FlipWait::kProgrammed;
    FlipWindow window;
    window.Observe(copied);
    window.Observe(plain);
    window.NoteLosses(2, 1);

    const FlipSummary summary = window.Drain();
    EXPECT_EQ(summary.placed, 2u);
    EXPECT_EQ(summary.placedCaptured, 1u);
    EXPECT_EQ(summary.unplaced, 2u);
    EXPECT_EQ(summary.unmatched, 1u);
    EXPECT_DOUBLE_EQ(summary.toScreen[0].MeanUs(), 3150.0);
    EXPECT_DOUBLE_EQ(summary.flipQueue[0].MeanUs(), 2750.0);
    EXPECT_DOUBLE_EQ(summary.scanout[0].MeanUs(), 250.0);
    EXPECT_DOUBLE_EQ(summary.toScreen[1].MeanUs(), 400.0);
    EXPECT_DOUBLE_EQ(summary.gpuQueue[1].MeanUs(), 50.0);
    EXPECT_EQ(summary.slow, 1u);
    EXPECT_EQ(summary.slowWaits[static_cast<size_t>(FlipWait::kOnScreen)], 1u);

    char line[1024];
    FormatFlipSummary(summary, 10'000, line, sizeof(line));
    const std::string text = line;
    EXPECT_NE(text.find("[PRESENT STALL] flip window=10000ms placed=2 captured=1 unplaced=2 unmatched=1"),
              std::string::npos)
        << text;
    EXPECT_NE(text.find("captured toScreen=3150/"), std::string::npos) << text;
    EXPECT_NE(text.find("flipQueue=2750/"), std::string::npos) << text;
    EXPECT_NE(text.find("returnedAfter(submit/ready/programmed/onScreen/previousOnScreen/none)=0/0/0/1/0/0"),
              std::string::npos)
        << text;
    EXPECT_EQ(window.Drain().placed, 0u);
}

TEST(PresentFlipTimingTest, TheSlowCallLineMarksWhatTheStreamDidNotReport) {
    FlipTiming timing;
    timing.slowOrdinal = 7;
    timing.captured = true;
    timing.exitUs = 3200;
    timing.presentStartUs = 40;
    timing.submitUs = 100;
    timing.screenUs = 3150;
    timing.wait = FlipWait::kOnScreen;
    char line[384];
    FormatSlowCallFlip(timing, line, sizeof(line));
    EXPECT_STREQ(line,
                 "[PRESENT STALL] slow down-call #7 captured=1 flip: start=+40us submit=+100us ready=? programmed=? "
                 "screen=+3150us returned=+3200us previousScreen=? (from call entry) -> returned after its flip on "
                 "screen");
}
