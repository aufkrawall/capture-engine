#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "hook/overlay/legacy_overlay_cache.h"

// How the D3D9 overlay uploads a frame's geometry: vertices appended behind
// the previous frame's (D3DLOCK_NOOVERWRITE) and the buffer discarded only when
// they no longer fit; indices re-uploaded only when they change, which a frame
// that only moved the frame-time graph never does.
namespace {

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

TEST(LegacyIndexUploadStateTest, UnchangedIndicesAreNotUploadedAgain) {
    LegacyIndexUploadState state;
    const std::vector<uint16_t> text = {0, 1, 2, 0, 2, 3};
    EXPECT_TRUE(state.NeedsUpload(text));
    state.MarkUploaded(text);
    EXPECT_FALSE(state.NeedsUpload(text)) << "a graph-only frame keeps the buffer's indices";

    std::vector<uint16_t> changed = text;
    changed.push_back(4);
    EXPECT_TRUE(state.NeedsUpload(changed)) << "new text, new indices";
    state.MarkUploaded(changed);
    EXPECT_TRUE(state.NeedsUpload(text));
}

TEST(LegacyIndexUploadStateTest, ARecreatedBufferOrAFailedUploadUploadsAgain) {
    LegacyIndexUploadState state;
    const std::vector<uint16_t> indices = {0, 1, 2};
    state.MarkUploaded(indices);
    state.Invalidate();
    EXPECT_TRUE(state.NeedsUpload(indices));
}
