#include <gtest/gtest.h>

#include "runtime/children/process_residue.h"

#include <windows.h>

#include <string>

namespace {
using ce::runtime::DescribeResidue;
using ce::runtime::ProcessResidueSample;
using ce::runtime::ResidueMeter;

// A probe the test advances by hand: each call returns the current value and counts the calls.
struct FakeProbe {
    ProcessResidueSample current;
    int calls = 0;
    ProcessResidueSample operator()() {
        ++calls;
        return current;
    }
};

ProcessResidueSample Sample(uint32_t handles, uint32_t threads, uint64_t privateMiB, uint64_t vramMiB) {
    ProcessResidueSample sample;
    sample.handles = handles;
    sample.threads = threads;
    sample.gdiObjects = 10;
    sample.userObjects = 20;
    sample.privateBytes = privateMiB * 1024 * 1024;
    sample.videoMemoryBytes = vramMiB * 1024 * 1024;
    sample.videoMemoryKnown = true;
    return sample;
}

TEST(ProcessResidueTest, TheDescriptionNamesEverySignedDifference) {
    EXPECT_EQ(DescribeResidue(Sample(100, 10, 50, 200), Sample(103, 9, 52, 200)),
              "handles=+3 threads=-1 gdi=+0 user=+0 private=+2.0MiB vram=+0.0MiB");
}

TEST(ProcessResidueTest, UnknownVideoMemoryIsNotAvailable) {
    ProcessResidueSample after = Sample(100, 10, 50, 0);
    after.videoMemoryKnown = false;
    EXPECT_EQ(DescribeResidue(Sample(100, 10, 50, 200), after),
              "handles=+0 threads=+0 gdi=+0 user=+0 private=+0.0MiB vram=n/a");
}

TEST(ProcessResidueTest, OneSessionReportsAtItsEnd) {
    FakeProbe probe;
    probe.current = Sample(100, 10, 50, 200);
    ResidueMeter meter([&probe] { return probe(); });
    meter.Starting();
    probe.current = Sample(140, 18, 90, 600);  // the live session's own resources do not matter
    probe.current = Sample(101, 10, 50, 200);
    const std::optional<std::string> line = meter.Ended();
    ASSERT_TRUE(line.has_value());
    if (!line)
        return;
    EXPECT_EQ(*line, "sessions=1 handles=+1 threads=+0 gdi=+0 user=+0 private=+0.0MiB vram=+0.0MiB");
    EXPECT_EQ(probe.calls, 2);
}

// A stop followed by an immediate start: the retired recording still finalizes while the next one runs. One line,
// at the idle transition, against the baseline from before the first.
TEST(ProcessResidueTest, OverlappingSessionsReportOnceWhenTheLastEnds) {
    FakeProbe probe;
    probe.current = Sample(100, 10, 50, 200);
    ResidueMeter meter([&probe] { return probe(); });
    meter.Starting();
    meter.Starting();
    EXPECT_EQ(meter.Live(), 2u);
    EXPECT_FALSE(meter.Ended().has_value());
    meter.Starting();
    EXPECT_FALSE(meter.Ended().has_value());
    probe.current = Sample(100, 10, 50, 200);
    const std::optional<std::string> line = meter.Ended();
    ASSERT_TRUE(line.has_value());
    if (!line)
        return;
    EXPECT_EQ(line->rfind("sessions=3 handles=+0", 0), 0u);
    EXPECT_EQ(probe.calls, 2);  // one baseline, one report
}

TEST(ProcessResidueTest, AnAbandonedStartNeitherReportsNorCounts) {
    FakeProbe probe;
    probe.current = Sample(100, 10, 50, 200);
    ResidueMeter meter([&probe] { return probe(); });
    meter.Starting();
    meter.Abandoned();
    EXPECT_EQ(meter.Live(), 0u);
    EXPECT_FALSE(meter.Ended().has_value());  // nothing live: no report
    meter.Starting();
    const std::optional<std::string> line = meter.Ended();
    ASSERT_TRUE(line.has_value());
    if (!line)
        return;
    EXPECT_EQ(line->rfind("sessions=1 ", 0), 0u);
}

TEST(ProcessResidueTest, EachIdleTransitionTakesAFreshBaseline) {
    FakeProbe probe;
    probe.current = Sample(100, 10, 50, 200);
    ResidueMeter meter([&probe] { return probe(); });
    meter.Starting();
    probe.current = Sample(105, 10, 50, 200);
    ASSERT_TRUE(meter.Ended().has_value());
    meter.Starting();  // baseline 105
    probe.current = Sample(105, 10, 50, 200);
    const std::optional<std::string> second = meter.Ended();
    ASSERT_TRUE(second.has_value());
    if (!second)
        return;
    EXPECT_EQ(second->rfind("sessions=1 handles=+0", 0), 0u);
}

TEST(ProcessResidueTest, TheSystemProbeReadsThisProcess) {
    const ProcessResidueSample sample = ce::runtime::SampleThisProcess();
    EXPECT_GT(sample.handles, 0u);
    EXPECT_GT(sample.threads, 0u);
    EXPECT_EQ(sample.threads, sample.threadIds.size());
    EXPECT_GT(sample.privateBytes, 0u);
}

// The threads a session left behind are named by owner, most first, so the residue line says where to look.
TEST(ProcessResidueTest, NewThreadsAreGroupedByOwnerMostFirst) {
    ProcessResidueSample before;
    before.threadIds = {10, 20, 30};
    ProcessResidueSample after;
    after.threadIds = {10, 20, 30, 41, 42, 43, 44, 50};
    const auto attribute = [](uint32_t id) { return std::string(id < 50 ? "nvwgf2umx.dll" : "'CE media encoder'"); };
    EXPECT_EQ(ce::runtime::DescribeNewThreads(before, after, attribute), "nvwgf2umx.dll x4, 'CE media encoder' x1");
    EXPECT_EQ(ce::runtime::DescribeNewThreads(before, before, attribute), "") << "no new threads, nothing to name";
    EXPECT_EQ(ce::runtime::DescribeNewThreads(before, after, attribute, 1), "nvwgf2umx.dll x4 (+1 groups)");
}

// A thread this test starts is named by its description, or else by the module its start address lies in.
TEST(ProcessResidueTest, ALiveNewThreadIsAttributedFromTheSystem) {
    const ProcessResidueSample before = ce::runtime::SampleThisProcess();
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(stop, nullptr);
    HANDLE thread = CreateThread(
        nullptr, 0, [](void* event) -> DWORD { return WaitForSingleObject(static_cast<HANDLE>(event), INFINITE); },
        stop, 0, nullptr);
    ASSERT_NE(thread, nullptr);
    const ProcessResidueSample after = ce::runtime::SampleThisProcess();
    const std::string described = ce::runtime::DescribeNewThreads(before, after);
    SetEvent(stop);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    CloseHandle(stop);
    const bool matched = (described.find("unit_tests.exe x") != std::string::npos) ||
                         (described.find("libclang_rt.asan") != std::string::npos);
    EXPECT_TRUE(matched) << described;
}

}  // namespace
