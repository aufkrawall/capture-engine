#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "hook/present/present_callback_association.h"

using namespace ce::present_association;

namespace {
// One presented frame as the runtime produces it: the callback finishes, then
// the Present it belongs to enters CE's detour on the same thread.
void Frame(int64_t callbackEndUs, int64_t presentEntryUs, bool generated) {
    NoteCallbackEnd(callbackEndUs, generated);
    NotePresentEntry(presentEntryUs);
}
}  // namespace

class PresentCallbackAssociationTest : public ::testing::Test {
protected:
    void SetUp() override { Reset(); }
    void TearDown() override { Reset(); }
};

TEST_F(PresentCallbackAssociationTest, ResolvesTheProducingCallbackForAHostPresentStart) {
    Frame(1'000'000, 1'003'000, true);
    Frame(1'011'000, 1'014'000, false);
    Association found;
    // The host's PresentStart and CE's Present entry are two clocks on the same
    // call, a few hundred microseconds apart.
    ASSERT_TRUE(Find(1'003'120, found));
    EXPECT_EQ(found.callbackEndUs, 1'000'000);
    EXPECT_TRUE(found.generated);
    ASSERT_TRUE(Find(1'014'090, found));
    EXPECT_EQ(found.callbackEndUs, 1'011'000);
    EXPECT_FALSE(found.generated);
}

TEST_F(PresentCallbackAssociationTest, NeverBindsBeyondToleranceOrToTheNeighbouringPresent) {
    Frame(1'000'000, 1'003'000, true);
    Frame(1'011'000, 1'014'000, false);
    Association found;
    EXPECT_FALSE(Find(1'003'000 + kAssociationToleranceUs + 1, found));
    // Exactly between the two presents: the nearer one wins, and it is the only
    // candidate inside the tolerance.
    ASSERT_TRUE(Find(1'004'000, found));
    EXPECT_EQ(found.callbackEndUs, 1'000'000);
}

TEST_F(PresentCallbackAssociationTest, APresentWithoutAStagedCallbackCommitsNothing) {
    // A foreign or non-callback Present must not claim the previous frame's
    // callback: the stage is consumed exactly once.
    Frame(1'000'000, 1'003'000, true);
    NotePresentEntry(1'014'000);
    Association found;
    EXPECT_FALSE(Find(1'014'000, found));
    ASSERT_TRUE(Find(1'003'000, found));
    EXPECT_EQ(found.callbackEndUs, 1'000'000);
}

TEST_F(PresentCallbackAssociationTest, RejectsAPresentThatPrecedesItsCallback) {
    NoteCallbackEnd(1'005'000, true);
    NotePresentEntry(1'004'000);
    Association found;
    EXPECT_FALSE(Find(1'004'000, found));
}

TEST_F(PresentCallbackAssociationTest, ResetDropsStagedAndCommittedStateAcrossATransition) {
    Frame(1'000'000, 1'003'000, true);
    NoteCallbackEnd(1'011'000, false);
    Reset();
    // The staged callback belonged to the previous epoch's topology.
    NotePresentEntry(1'014'000);
    Association found;
    EXPECT_FALSE(Find(1'003'000, found));
    EXPECT_FALSE(Find(1'014'000, found));
    Frame(1'020'000, 1'023'000, false);
    ASSERT_TRUE(Find(1'023'000, found));
    EXPECT_EQ(found.callbackEndUs, 1'020'000);
}

TEST_F(PresentCallbackAssociationTest, OldestEntriesAgeOutWithoutReportingStaleAssociations) {
    for (int i = 0; i < 4000; ++i)
        Frame(1'000'000 + i * 8'000, 1'003'000 + i * 8'000, (i % 2) == 0);
    Association found;
    EXPECT_FALSE(Find(1'003'000, found));
    const int64_t newest = 1'003'000 + 3999 * 8'000;
    ASSERT_TRUE(Find(newest, found));
    EXPECT_EQ(found.callbackEndUs, newest - 3'000);
}

TEST_F(PresentCallbackAssociationTest, ConcurrentProducersAndReaderNeverYieldTornPairs) {
    // Two presenter threads can exist across a frame-generation transition. A
    // reader must see a whole association or none, never a mixed one.
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};
    auto produce = [&](int64_t base) {
        for (int i = 0; !stop.load(std::memory_order_relaxed) && i < 200'000; ++i)
            Frame(base + i * 8'000, base + i * 8'000 + 3'000, (i % 2) == 0);
    };
    std::thread a(produce, 1'000'000);
    std::thread b(produce, 500'000'000);
    for (int i = 0; i < 200'000; ++i) {
        Association found;
        if (Find(1'003'000 + (i % 1000) * 8'000, found) && found.presentEntryUs - found.callbackEndUs != 3'000)
            torn.fetch_add(1, std::memory_order_relaxed);
    }
    stop.store(true, std::memory_order_relaxed);
    a.join();
    b.join();
    EXPECT_EQ(torn.load(), 0);
}

TEST_F(PresentCallbackAssociationTest, ThePresentAfterACallbackCarriesItsVerdictExactlyOnce) {
    Frame(1'000'000, 1'003'000, false);
    PresentFrameVerdict verdict = ConsumePresentFrameVerdict();
    EXPECT_TRUE(verdict.known);
    EXPECT_FALSE(verdict.generated);
    // Consumed: nothing left for a later Present to inherit.
    EXPECT_FALSE(ConsumePresentFrameVerdict().known);

    Frame(1'011'000, 1'014'000, true);
    verdict = ConsumePresentFrameVerdict();
    EXPECT_TRUE(verdict.known);
    EXPECT_TRUE(verdict.generated);
}

// The DXGI detours read the verdict at Present entry for the FPS limiter's call
// site, which runs after ProcessFrame consumed it; the read must leave it there.
TEST_F(PresentCallbackAssociationTest, PeekingAtPresentEntryLeavesTheVerdictForProcessFrame) {
    Frame(1'000'000, 1'003'000, true);
    PresentFrameVerdict peeked = PeekPresentFrameVerdict();
    EXPECT_TRUE(peeked.known);
    EXPECT_TRUE(peeked.generated);
    peeked = PeekPresentFrameVerdict();
    EXPECT_TRUE(peeked.known);
    const PresentFrameVerdict consumed = ConsumePresentFrameVerdict();
    EXPECT_TRUE(consumed.known);
    EXPECT_TRUE(consumed.generated);
    EXPECT_FALSE(PeekPresentFrameVerdict().known);
}

TEST_F(PresentCallbackAssociationTest, PeekSeesNothingWithoutACallbackOrAfterReset) {
    NotePresentEntry(1'003'000);
    EXPECT_FALSE(PeekPresentFrameVerdict().known);
    Frame(1'011'000, 1'014'000, false);
    Reset();
    EXPECT_FALSE(PeekPresentFrameVerdict().known);
}

TEST_F(PresentCallbackAssociationTest, APresentWithoutACallbackHasNoVerdict) {
    NotePresentEntry(1'003'000);
    EXPECT_FALSE(ConsumePresentFrameVerdict().known);
}

TEST_F(PresentCallbackAssociationTest, ANestedPresentEntryKeepsTheOuterPresentsVerdict) {
    // A Present re-entering CE's detour inside the runtime's Present stages no
    // callback of its own and must not erase the frame's verdict.
    Frame(1'000'000, 1'003'000, false);
    NotePresentEntry(1'003'200);
    const PresentFrameVerdict verdict = ConsumePresentFrameVerdict();
    EXPECT_TRUE(verdict.known);
    EXPECT_FALSE(verdict.generated);
}

TEST_F(PresentCallbackAssociationTest, AnUnconsumedVerdictNeverOutlivesTheNextCallback) {
    // A Present that skipped ProcessFrame leaves its verdict behind; the next
    // frame's callback retires it before its own Present commits a new one.
    Frame(1'000'000, 1'003'000, false);
    NoteCallbackEnd(1'011'000, true);
    EXPECT_FALSE(ConsumePresentFrameVerdict().known);
    NotePresentEntry(1'014'000);
    const PresentFrameVerdict verdict = ConsumePresentFrameVerdict();
    EXPECT_TRUE(verdict.known);
    EXPECT_TRUE(verdict.generated);
}

TEST_F(PresentCallbackAssociationTest, ResetInvalidatesACommittedVerdict) {
    Frame(1'000'000, 1'003'000, false);
    Reset();
    EXPECT_FALSE(ConsumePresentFrameVerdict().known);
}

TEST_F(PresentCallbackAssociationTest, AVerdictBelongsOnlyToThePresentingThread) {
    Frame(1'000'000, 1'003'000, false);
    bool otherThreadSawVerdict = true;
    std::thread other([&]() { otherThreadSawVerdict = ConsumePresentFrameVerdict().known; });
    other.join();
    EXPECT_FALSE(otherThreadSawVerdict);
    EXPECT_TRUE(ConsumePresentFrameVerdict().known);
}
