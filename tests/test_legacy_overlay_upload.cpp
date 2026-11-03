#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "hook/overlay/legacy_overlay_cache.h"

// How the D3D9 overlay uploads a frame's geometry: vertices appended behind
// the previous frame's (D3DLOCK_NOOVERWRITE) and the buffer discarded only when
// they no longer fit; indices re-uploaded only when they change, which a frame
// that only moved the frame-time graph never does.
namespace {

using CustomOverlay::LegacyFrameLayout;
using CustomOverlay::LegacyGeometryUploadState;
using CustomOverlay::LegacyIndexUploadState;
using CustomOverlay::LegacyVertexRing;

constexpr size_t kCapacity = 1000;

}  // namespace

TEST(LegacyVertexRingTest, TheFirstFrameDiscardsAndLaterFramesAppend) {
    LegacyVertexRing ring;
    const auto first = ring.Next(300, kCapacity);
    EXPECT_TRUE(first.discard);
    EXPECT_EQ(first.offset, 0u);
    const auto second = ring.Next(300, kCapacity);
    EXPECT_FALSE(second.discard);
    EXPECT_EQ(second.offset, 300u);
    const auto third = ring.Next(300, kCapacity);
    EXPECT_FALSE(third.discard);
    EXPECT_EQ(third.offset, 600u);
}

TEST(LegacyVertexRingTest, AFrameThatNoLongerFitsDiscardsAndStartsOver) {
    LegacyVertexRing ring;
    ring.Next(400, kCapacity);
    ring.Next(400, kCapacity);
    const auto wrapped = ring.Next(400, kCapacity);
    EXPECT_TRUE(wrapped.discard);
    EXPECT_EQ(wrapped.offset, 0u);
    EXPECT_EQ(ring.Next(400, kCapacity).offset, 400u) << "appending resumes behind the wrapped frame";
}

TEST(LegacyVertexRingTest, AFrameThatExactlyFillsTheBufferIsAppended) {
    LegacyVertexRing ring;
    ring.Next(500, kCapacity);
    const auto last = ring.Next(500, kCapacity);
    EXPECT_FALSE(last.discard);
    EXPECT_EQ(last.offset, 500u);
    EXPECT_TRUE(ring.Next(1, kCapacity).discard);
}

TEST(LegacyVertexRingTest, ARecreatedBufferOrAFailedLockDiscards) {
    LegacyVertexRing ring;
    ring.Next(100, kCapacity);
    ring.Invalidate();
    const auto next = ring.Next(100, kCapacity);
    EXPECT_TRUE(next.discard);
    EXPECT_EQ(next.offset, 0u);
}

TEST(LegacyVertexRingTest, AGrownBufferIsUsedFromItsNewSize) {
    LegacyVertexRing ring;
    ring.Next(900, kCapacity);
    ring.Invalidate();  // ResizeVertexBuffer
    EXPECT_TRUE(ring.Next(1500, 2 * kCapacity).discard);
    EXPECT_FALSE(ring.Next(400, 2 * kCapacity).discard);
}

// A frame that only moved the graph writes the graph: the rest stays where the last whole frame put it.
TEST(LegacyVertexRingTest, AFrameKeepsWhatTheLastWholeFrameWroteAndAppendsTheRest) {
    LegacyVertexRing ring;
    ring.Next(100, kCapacity);
    const auto whole = ring.Next(100, kCapacity);
    ASSERT_EQ(whole.offset, 100u);

    const auto tail = ring.Next(100, kCapacity, 60);
    EXPECT_FALSE(tail.discard);
    EXPECT_EQ(tail.offset, 200u) << "behind everything written so far";
    EXPECT_EQ(tail.keptBytes, 60u);
    EXPECT_EQ(tail.keptOffset, 100u);

    const auto next = ring.Next(120, kCapacity, 60);
    EXPECT_EQ(next.offset, 240u);
    EXPECT_EQ(next.keptOffset, 100u) << "still the last whole frame, not the last frame";
}

TEST(LegacyVertexRingTest, AFrameThatWouldDiscardIsWrittenWhole) {
    LegacyVertexRing ring;
    ring.Next(400, kCapacity);
    ASSERT_EQ(ring.Next(400, kCapacity, 100).keptBytes, 100u);
    ASSERT_EQ(ring.Next(400, kCapacity, 100).keptBytes, 100u);  // up to 1000

    const auto wrapped = ring.Next(400, kCapacity, 100);
    EXPECT_TRUE(wrapped.discard) << "a discard loses what the frame would keep";
    EXPECT_EQ(wrapped.offset, 0u);
    EXPECT_EQ(wrapped.keptBytes, 0u);

    const auto after = ring.Next(400, kCapacity, 100);
    EXPECT_EQ(after.keptBytes, 100u);
    EXPECT_EQ(after.keptOffset, 0u) << "kept from the frame the discard wrote";
    EXPECT_EQ(after.offset, 400u);
}

TEST(LegacyVertexRingTest, NothingIsKeptWhenNoFrameHoldsThatMuchOrTheFrameAddsNothing) {
    LegacyVertexRing ring;
    ring.Next(100, kCapacity);
    EXPECT_EQ(ring.Next(300, kCapacity, 200).keptBytes, 0u) << "more than the last whole frame wrote";
    EXPECT_EQ(ring.Next(100, kCapacity, 100).keptBytes, 0u) << "a lock of zero bytes would lock the whole buffer";
    EXPECT_EQ(ring.Next(100, kCapacity, 0).keptBytes, 0u);
}

TEST(LegacyVertexRingTest, ARecreatedBufferOrAFailedLockKeepsNothing) {
    LegacyVertexRing ring;
    ring.Next(100, kCapacity);
    ring.Invalidate();
    const auto next = ring.Next(100, kCapacity, 50);
    EXPECT_TRUE(next.discard);
    EXPECT_EQ(next.keptBytes, 0u);
}

// The renderer's "unchanged" is relative to the frame it last built, which may not have reached the
// buffers. Keeping then would draw the text of an older frame.
TEST(LegacyGeometryUploadStateTest, UnchangedOnlyCountsAfterTheFrameBeforeItWasUploaded) {
    LegacyGeometryUploadState state;
    state.MarkTailChanged(10, 20);
    EXPECT_TRUE(state.NeedsUpload());
    EXPECT_EQ(state.UnchangedVertices(), 0u) << "nothing was ever uploaded";

    state.MarkUploadSucceeded();
    state.MarkTailChanged(10, 20);
    EXPECT_TRUE(state.NeedsUpload());
    EXPECT_EQ(state.UnchangedVertices(), 10u);
    EXPECT_EQ(state.UnchangedIndices(), 20u);

    state.MarkUploadSucceeded();
    EXPECT_FALSE(state.NeedsUpload());
    EXPECT_EQ(state.UnchangedVertices(), 0u) << "reported for one upload only";

    state.MarkDrawDataChanged();
    state.MarkTailChanged(10, 20);
    EXPECT_EQ(state.UnchangedVertices(), 0u) << "a whole new frame is pending";

    state.MarkUploadSucceeded();
    state.MarkDrawDataChanged();
    state.MarkUploadFailed();
    state.MarkTailChanged(10, 20);
    EXPECT_EQ(state.UnchangedVertices(), 0u) << "the rebuilt frame never reached the buffers";

    state.MarkUploadSucceeded();
    state.MarkTailChanged(10, 20);
    state.MarkBufferRecreated();
    EXPECT_EQ(state.UnchangedVertices(), 0u) << "the buffer was replaced";
    EXPECT_EQ(state.UnchangedIndices(), 0u);
}

namespace {

struct DrawRun {
    uint32_t base, firstVertex, vertexCount, firstIndex, indexCount;
    bool operator==(const DrawRun& other) const {
        return base == other.base && firstVertex == other.firstVertex && vertexCount == other.vertexCount &&
               firstIndex == other.firstIndex && indexCount == other.indexCount;
    }
};

std::vector<DrawRun> RunsOf(const LegacyFrameLayout& layout, uint32_t indexOffset, uint32_t indexCount,
                        uint32_t vertexCount) {
    std::vector<DrawRun> runs;
    layout.ForEachRun(indexOffset, indexCount, vertexCount,
                      [&](uint32_t base, uint32_t firstVertex, uint32_t count, uint32_t firstIndex, uint32_t indices) {
                          runs.push_back({base, firstVertex, count, firstIndex, indices});
                      });
    return runs;
}

constexpr size_t kStride = 10;

}  // namespace

TEST(LegacyFrameLayoutTest, AWholeFrameIsOneRunFromItsSlot) {
    LegacyVertexRing::Slot slot;
    slot.offset = 500;
    const auto layout = LegacyFrameLayout::For(slot, kStride, 40, 60, 0);
    const auto runs = RunsOf(layout, 6, 54, 40);
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs[0], (DrawRun{50, 0, 40, 6, 54}));
}

TEST(LegacyFrameLayoutTest, ACommandAcrossTheKeptPartAndTheTailTakesADrawForEach) {
    LegacyVertexRing::Slot slot;
    slot.offset = 900;  // vertex 90: where the tail was written
    slot.discard = false;
    slot.keptBytes = 100;  // the frame's first 10 vertices...
    slot.keptOffset = 300;  // ...are at vertex 30
    const auto layout = LegacyFrameLayout::For(slot, kStride, 25, 45, 15);

    const auto both = RunsOf(layout, 0, 45, 25);
    ASSERT_EQ(both.size(), 2u);
    EXPECT_EQ(both[0], (DrawRun{30, 0, 10, 0, 15}));
    // Frame vertex 10, the tail's first, is index base 80 + 10 = 90: where it was written.
    EXPECT_EQ(both[1], (DrawRun{80, 10, 15, 15, 30}));

    const auto keptOnly = RunsOf(layout, 6, 9, 25);
    ASSERT_EQ(keptOnly.size(), 1u);
    EXPECT_EQ(keptOnly[0], (DrawRun{30, 0, 10, 6, 9}));

    const auto tailOnly = RunsOf(layout, 15, 12, 25);
    ASSERT_EQ(tailOnly.size(), 1u);
    EXPECT_EQ(tailOnly[0], (DrawRun{80, 10, 15, 15, 12}));
}

// The smallest legal tail position: right behind the vertices it keeps.
TEST(LegacyFrameLayoutTest, TheTailBaseCannotUnderflow) {
    LegacyVertexRing ring;
    ring.Next(100, kCapacity);
    const auto slot = ring.Next(130, kCapacity, 100);
    const auto layout = LegacyFrameLayout::For(slot, kStride, 13, 20, 12);
    EXPECT_EQ(layout.headBaseVertex, 0u);
    EXPECT_EQ(layout.tailBaseVertex, 0u);
    EXPECT_EQ(layout.tailFirstVertex, 10u);
}

TEST(LegacyIndexUploadStateTest, UnchangedIndicesAreNotUploadedAgain) {
    LegacyIndexUploadState state;
    const std::vector<uint16_t> text = {0, 1, 2, 0, 2, 3};
    EXPECT_TRUE(state.NeedsUpload(text));
    state.MarkUploaded(text);
    EXPECT_FALSE(state.NeedsUpload(text)) << "a graph-only frame keeps the buffer's indices";

    std::vector<uint16_t> longer = text;
    longer.push_back(4);
    EXPECT_TRUE(state.NeedsUpload(longer)) << "new text, new indices";
    state.MarkUploaded(longer);

    std::vector<uint16_t> changed = text;
    changed[1] = 7;
    EXPECT_TRUE(state.NeedsUpload(changed)) << "same length or shorter, but different";
}

// The graph line's section count moves by a few as its joins change between miter and bevel, and its
// index pattern depends only on where it starts: a shorter line is the same list ended earlier.
TEST(LegacyIndexUploadStateTest, AListThatStartsWhatTheBufferHoldsIsNotUploadedAgain) {
    LegacyIndexUploadState state;
    const std::vector<uint16_t> longest = {0, 1, 2, 0, 2, 3, 4, 5, 6};
    state.MarkUploaded(longest);
    const std::vector<uint16_t> shorter(longest.begin(), longest.begin() + 6);
    EXPECT_FALSE(state.NeedsUpload(shorter));
    EXPECT_FALSE(state.NeedsUpload(longest)) << "the longer list is still what the buffer holds";
    EXPECT_FALSE(state.NeedsUpload({}));
}

TEST(LegacyIndexUploadStateTest, ARecreatedBufferOrAFailedUploadUploadsAgain) {
    LegacyIndexUploadState state;
    const std::vector<uint16_t> indices = {0, 1, 2};
    state.MarkUploaded(indices);
    state.Invalidate();
    EXPECT_TRUE(state.NeedsUpload(indices));
}
