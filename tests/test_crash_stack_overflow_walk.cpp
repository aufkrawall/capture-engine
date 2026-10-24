#include <gtest/gtest.h>

#include <windows.h>

#include <cstring>
#include <string>

#include "common/crash/crash_stack_overflow_summary.h"
#include "common/crash/crash_stack_overflow_walk.h"

namespace {

using ce::crash_stack::Frame;
using ce::crash_stack::OverflowFrameSummary;

constexpr uintptr_t kForeignBase = 0x7FF800000000ull;
constexpr uintptr_t kOwnBase = 0x7FF900000000ull;

Frame Foreign(uintptr_t offset) { return Frame{kForeignBase + offset, kForeignBase}; }
Frame Own(uintptr_t offset) { return Frame{kOwnBase + offset, kOwnBase}; }

// Session 20261024_131043: OptiScaler's detour called its second detour of the same function, 75k frames of
// one return site, then its own thread's start. CE's crash.log could only name the faulting module.
TEST(CrashStackOverflowSummaryTest, ForeignSelfRecursionIsOneFrameCycleWithoutCeFrames) {
    OverflowFrameSummary summary;
    for (int i = 0; i < 75000; ++i) {
        summary.Add(Foreign(0x86C67), false);
    }
    summary.Add(Foreign(0x30B58), false);
    summary.Add(Frame{0x7FFA00002DDA7ull, 0x7FFA00000000ull}, false);
    summary.Finish();

    EXPECT_EQ(summary.Frames(), 75002u);
    EXPECT_EQ(summary.CyclePeriod(), 1u);
    EXPECT_EQ(summary.CycleFrames(), 75000u);
    EXPECT_EQ(summary.CycleFrame(0).returnAddress, kForeignBase + 0x86C67);
    EXPECT_EQ(summary.OwnFrames(), 0u);
    EXPECT_EQ(summary.FirstOwnDepth(), OverflowFrameSummary::kNoDepth);
    ASSERT_EQ(summary.TailCount(), 4u);
    EXPECT_EQ(summary.Tail(0).returnAddress, 0x7FFA00002DDA7ull);  // outermost first
    EXPECT_EQ(summary.Tail(1).returnAddress, kForeignBase + 0x30B58);
}

// Session 20261023_173151 had the opposite verdict: CE's hook sat inside OptiScaler's loop. A cycle that runs
// through CE counts CE's frames and names the first one.
TEST(CrashStackOverflowSummaryTest, LoopThroughCeCountsCeFramesInEveryCycle) {
    OverflowFrameSummary summary;
    summary.Add(Foreign(0x10), false);  // the faulting function's caller before the loop settles
    for (int i = 0; i < 300; ++i) {
        summary.Add(Foreign(0x20), false);
        summary.Add(Own(0x30), true);
        summary.Add(Foreign(0x40), false);
    }
    summary.Finish();

    EXPECT_EQ(summary.OwnFrames(), 300u);
    EXPECT_EQ(summary.FirstOwnDepth(), 2u);
    EXPECT_EQ(summary.CyclePeriod(), 3u);
    EXPECT_EQ(summary.CycleStart(), 1u);
    EXPECT_EQ(summary.CycleFrames(), 900u);
    EXPECT_EQ(summary.CycleFrame(1).returnAddress, kOwnBase + 0x30);
}

TEST(CrashStackOverflowSummaryTest, MultiFrameCycleAndItsBreakAreMeasured) {
    OverflowFrameSummary summary;
    for (int i = 0; i < 100; ++i) {
        summary.Add(Foreign(0x20), false);
        summary.Add(Own(0x30), true);
        summary.Add(Foreign(0x40), false);
    }
    summary.Add(Foreign(0x99), false);
    summary.Add(Foreign(0x20), false);  // a matching frame after the break does not extend the cycle
    summary.Finish();

    EXPECT_EQ(summary.CyclePeriod(), 3u);
    EXPECT_EQ(summary.CycleStart(), 0u);
    EXPECT_EQ(summary.CycleFrames(), 300u);
    EXPECT_EQ(summary.CycleFrame(1).returnAddress, kOwnBase + 0x30);
}

TEST(CrashStackOverflowSummaryTest, ShortAndNonRepeatingWalksReportNoCycle) {
    OverflowFrameSummary distinct;
    for (uintptr_t i = 0; i < 200; ++i) {
        distinct.Add(Foreign((i + 1) * 8), false);
    }
    distinct.Finish();
    EXPECT_EQ(distinct.CyclePeriod(), 0u);

    OverflowFrameSummary shortCycle;
    for (int i = 0; i < 10; ++i) {
        shortCycle.Add(Foreign(0x50), false);
    }
    shortCycle.Finish();
    EXPECT_EQ(shortCycle.CyclePeriod(), 1u);
    EXPECT_EQ(shortCycle.CycleFrames(), 10u);

    OverflowFrameSummary single;
    single.Add(Foreign(0x50), false);
    single.Finish();
    EXPECT_EQ(single.CyclePeriod(), 0u);
    EXPECT_EQ(single.TailCount(), 1u);
}

TEST(CrashStackOverflowSummaryTest, AttributionLineNamesTheCycleAndStaysInsideItsBuffer) {
    OverflowFrameSummary summary;
    for (int i = 0; i < 500; ++i) {
        summary.Add(Foreign(0x86C67), false);
    }
    summary.Finish();
    char line[1000];
    ce::crash_stack::FormatOverflowAttribution(summary, true, line, sizeof(line));
    const std::string text = line;
    EXPECT_NE(text.find("500 frames unwound (complete=1)"), std::string::npos) << text;
    EXPECT_NE(text.find("cycle of 1 frame(s) from depth 0 repeats over 500 frames"), std::string::npos) << text;
    EXPECT_NE(text.find("CE frames=0;"), std::string::npos) << text;

    char tiny[24];
    std::memset(tiny, 'x', sizeof(tiny));
    ce::crash_stack::FormatOverflowAttribution(summary, true, tiny, sizeof(tiny));
    EXPECT_LT(std::strlen(tiny), sizeof(tiny));
}

#ifdef _WIN64
constexpr int kRecursionDepth = 200;

struct WalkResult {
    bool walked = false;
    bool complete = false;
    OverflowFrameSummary summary;
};

__attribute__((noinline)) int RecurseThenWalk(int depth, uintptr_t ownBase, WalkResult* result) {
    if (depth == 0) {
        CONTEXT context{};
        RtlCaptureContext(&context);
        result->walked = ce::crash_stack::WalkStack(context, ce::crash_stack::kMaxOverflowWalkFrames, ownBase,
                                                    &result->summary, &result->complete);
        return 1;
    }
    volatile int keep = RecurseThenWalk(depth - 1, ownBase, result);
    return keep + 1;  // not a tail call: every level keeps a frame and the same return site
}

// The real unwinder on this thread's own stack: a known recursion must come out as a one-frame cycle in this
// image, and the walk must reach the thread's start.
TEST(CrashStackOverflowWalkTest, UnwindsAKnownRecursionToTheThreadStart) {
    PVOID ownBase = nullptr;
    RtlPcToFileHeader(reinterpret_cast<PVOID>(&RecurseThenWalk), &ownBase);
    ASSERT_NE(ownBase, nullptr);
    WalkResult result;
    EXPECT_EQ(RecurseThenWalk(kRecursionDepth, reinterpret_cast<uintptr_t>(ownBase), &result), kRecursionDepth + 1);
    ASSERT_TRUE(result.walked);
    EXPECT_TRUE(result.complete);
    EXPECT_GT(result.summary.Frames(), static_cast<size_t>(kRecursionDepth));
    EXPECT_EQ(result.summary.CyclePeriod(), 1u);
    EXPECT_GE(result.summary.CycleFrames(), static_cast<size_t>(kRecursionDepth - 1));
    EXPECT_GE(result.summary.OwnFrames(), static_cast<size_t>(kRecursionDepth));
    EXPECT_EQ(result.summary.FirstOwnDepth(), 0u);
    EXPECT_EQ(result.summary.CycleFrame(0).imageBase, reinterpret_cast<uintptr_t>(ownBase));
}

TEST(CrashStackOverflowWalkTest, ACapOrAnUnmappedStackEndsTheWalkIncomplete) {
    PVOID ownBase = nullptr;
    RtlPcToFileHeader(reinterpret_cast<PVOID>(&RecurseThenWalk), &ownBase);
    CONTEXT context{};
    RtlCaptureContext(&context);
    OverflowFrameSummary capped;
    bool complete = true;
    ASSERT_TRUE(ce::crash_stack::WalkStack(context, 2, reinterpret_cast<uintptr_t>(ownBase), &capped, &complete));
    EXPECT_FALSE(complete);
    EXPECT_LE(capped.Frames(), 2u);

    CONTEXT unmapped = context;
    unmapped.Rsp = 0x10;  // below every mapping
    OverflowFrameSummary none;
    EXPECT_FALSE(ce::crash_stack::WalkStack(unmapped, 16, 0, &none, &complete));
    EXPECT_FALSE(complete);
}
#endif

}  // namespace
