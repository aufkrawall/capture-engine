#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <vector>

#include "hook/overlay/upload_slot_geometry.h"

// What the D3D12 and Vulkan overlay backends write into an upload slot. The backends only keep what the
// slot's memory already holds, so the one thing that must never happen is a draw reading another frame's
// geometry: the simulation at the end checks that against plain memory for thousands of random frames.
namespace {

using CustomOverlay::UploadSlotGeometry;

constexpr size_t kVertices = 100;
const std::vector<uint16_t>& QuadIndices() {
    static const std::vector<uint16_t> indices = {0, 1, 2, 0, 2, 3};
    return indices;
}

}  // namespace

TEST(UploadSlotGeometryTest, AFreshSlotGetsEverything) {
    UploadSlotGeometry geometry(4);
    geometry.NoteWholeFrame();
    geometry.IndicesToHold(QuadIndices());
    const auto plan = geometry.PlanSlot(0, kVertices);
    EXPECT_TRUE(plan.writeVertices);
    EXPECT_EQ(plan.firstVertex, 0u);
    EXPECT_TRUE(plan.writeIndices);
}

TEST(UploadSlotGeometryTest, ASlotWithTheCurrentRowsGetsOnlyTheGraph) {
    UploadSlotGeometry geometry(4);
    geometry.NoteWholeFrame();
    geometry.IndicesToHold(QuadIndices());
    geometry.MarkWritten(0);

    geometry.NoteTailFrame(40, 4);
    geometry.IndicesToHold(QuadIndices());
    const auto kept = geometry.PlanSlot(0, kVertices);
    EXPECT_TRUE(kept.writeVertices);
    EXPECT_EQ(kept.firstVertex, 40u);
    EXPECT_FALSE(kept.writeIndices) << "the same list";

    const auto other = geometry.PlanSlot(1, kVertices);
    EXPECT_TRUE(other.writeVertices);
    EXPECT_EQ(other.firstVertex, 0u) << "that slot never held these rows";
    EXPECT_TRUE(other.writeIndices);
}

TEST(UploadSlotGeometryTest, ACachedFrameWritesNothingToASlotThatHoldsIt) {
    UploadSlotGeometry geometry(4);
    geometry.NoteWholeFrame();
    geometry.IndicesToHold(QuadIndices());
    geometry.MarkWritten(0);

    geometry.IndicesToHold(QuadIndices());
    const auto same = geometry.PlanSlot(0, kVertices);
    EXPECT_FALSE(same.writeVertices);
    EXPECT_FALSE(same.writeIndices);
    EXPECT_TRUE(geometry.PlanSlot(1, kVertices).writeVertices) << "another slot still needs it";
}

TEST(UploadSlotGeometryTest, NewRowsInvalidateWhatEverySlotKept) {
    UploadSlotGeometry geometry(4);
    geometry.NoteWholeFrame();
    geometry.IndicesToHold(QuadIndices());
    geometry.MarkWritten(0);

    geometry.NoteWholeFrame();
    geometry.IndicesToHold(QuadIndices());
    const auto plan = geometry.PlanSlot(0, kVertices);
    EXPECT_TRUE(plan.writeVertices);
    EXPECT_EQ(plan.firstVertex, 0u);
    EXPECT_FALSE(plan.writeIndices) << "equal indices stay";
}

TEST(UploadSlotGeometryTest, ReplacedMemoryHoldsNothing) {
    UploadSlotGeometry geometry(4);
    geometry.NoteWholeFrame();
    geometry.IndicesToHold(QuadIndices());
    geometry.MarkWritten(0);
    geometry.MarkWritten(1);

    geometry.InvalidateSlot(0);
    EXPECT_TRUE(geometry.PlanSlot(0, kVertices).writeVertices);
    EXPECT_TRUE(geometry.PlanSlot(0, kVertices).writeIndices);
    EXPECT_FALSE(geometry.PlanSlot(1, kVertices).writeVertices);

    geometry.InvalidateAll();
    EXPECT_TRUE(geometry.PlanSlot(1, kVertices).writeVertices);
    EXPECT_EQ(geometry.PlanSlot(1, kVertices).firstVertex, 0u);
}

TEST(UploadSlotGeometryTest, AShorterGraphLineKeepsTheLongerListInTheBuffer) {
    UploadSlotGeometry geometry(4);
    const std::vector<uint16_t> longer = {0, 1, 2, 0, 2, 3, 4, 5, 6};
    const std::vector<uint16_t> shorter(longer.begin(), longer.begin() + 6);
    geometry.NoteWholeFrame();
    EXPECT_EQ(geometry.IndicesToHold(longer), longer);
    geometry.MarkWritten(0);

    geometry.NoteTailFrame(10, 3);
    EXPECT_EQ(geometry.IndicesToHold(shorter), longer) << "what a slot must hold, to serve the longer one again";
    EXPECT_FALSE(geometry.PlanSlot(0, kVertices).writeIndices);

    geometry.NoteTailFrame(10, 3);
    EXPECT_EQ(geometry.IndicesToHold(longer), longer);
    EXPECT_FALSE(geometry.PlanSlot(0, kVertices).writeIndices);

    std::vector<uint16_t> grown = longer;
    grown.push_back(7);
    geometry.NoteTailFrame(10, 3);
    EXPECT_EQ(geometry.IndicesToHold(grown), grown);
    EXPECT_TRUE(geometry.PlanSlot(0, kVertices).writeIndices);
}

// A whole frame no draw has seen (its slot was still in flight) must not lose its compare.
TEST(UploadSlotGeometryTest, ATailAfterAnUnseenWholeFrameComparesItsWholeList) {
    UploadSlotGeometry geometry(4);
    geometry.NoteWholeFrame();
    geometry.IndicesToHold(QuadIndices());

    const std::vector<uint16_t> newText = {9, 8, 7, 0, 2, 3};
    geometry.NoteWholeFrame();
    geometry.NoteTailFrame(10, 3);  // says the first three are unchanged, relative to the unseen frame
    EXPECT_EQ(geometry.IndicesToHold(newText), newText);
}

TEST(UploadSlotGeometryTest, BytesAreReportedOncePerWindow) {
    UploadSlotGeometry geometry(1);
    UploadSlotGeometry::Totals report;
    for (uint32_t frame = 1; frame < UploadSlotGeometry::kReportFrames; ++frame)
        EXPECT_FALSE(geometry.NoteBytes(10, 2, 100, &report));
    ASSERT_TRUE(geometry.NoteBytes(10, 2, 100, &report));
    EXPECT_EQ(report.frames, UploadSlotGeometry::kReportFrames);
    EXPECT_EQ(report.vertexBytes, 10u * UploadSlotGeometry::kReportFrames);
    EXPECT_EQ(report.indexBytes, 2u * UploadSlotGeometry::kReportFrames);
    EXPECT_EQ(report.frameBytes, 100u * UploadSlotGeometry::kReportFrames);
    EXPECT_FALSE(geometry.NoteBytes(10, 2, 100, &report)) << "a new window";
}

TEST(UploadSlotGeometryTest, TheReportCountsTheSlotsThatDrewInItsWindow) {
    UploadSlotGeometry geometry(8);
    UploadSlotGeometry::Totals report;
    geometry.NoteWholeFrame();
    geometry.IndicesToHold(QuadIndices());
    for (uint32_t frame = 0; frame < UploadSlotGeometry::kReportFrames - 1; ++frame) {
        geometry.MarkWritten(frame % 2);
        ASSERT_FALSE(geometry.NoteBytes(10, 2, 100, &report));
    }
    geometry.MarkWritten(5);
    ASSERT_TRUE(geometry.NoteBytes(10, 2, 100, &report));
    EXPECT_EQ(report.slotsUsed, 3u);

    // The next window starts from nothing, and keeping what a slot holds is not touched by it.
    EXPECT_EQ(geometry.SlotsUsedInWindow(), 0u);
    EXPECT_FALSE(geometry.PlanSlot(5, kVertices).writeVertices);
}

// The contract, end to end against plain memory: after every plan a slot's memory reads as the current frame.
// Frames are random: new rows, a moved graph of varying length, cached redraws, slots skipped because they were
// in flight, and whole-frame draws lost to the same.
TEST(UploadSlotGeometryTest, EverySlotReadsAsTheCurrentFrameAfterItsPlanWhateverTheHistory) {
    constexpr size_t kSlots = 16;
    constexpr size_t kMaxVertices = 400;
    constexpr size_t kMaxIndices = 600;
    // Fixed seed keeps the test sequence reproducible.
    // NOLINTNEXTLINE(bugprone-random-generator-seed)
    std::mt19937 random(0xCE12);
    UploadSlotGeometry geometry(kSlots);
    std::vector<std::vector<uint32_t>> vertexMemory(kSlots, std::vector<uint32_t>(kMaxVertices, 0xDEADu));
    std::vector<std::vector<uint16_t>> indexMemory(kSlots, std::vector<uint16_t>(kMaxIndices, 0xDEADu));

    std::vector<uint32_t> vertices;
    std::vector<uint16_t> indices;
    uint32_t headVertices = 0;
    uint32_t headIndices = 0;
    uint32_t nextValue = 1;
    size_t wholeWrites = 0;
    size_t tailWrites = 0;
    size_t skipped = 0;

    const auto setTail = [&]() {
        const size_t tailVertices = 20 + random() % 40;
        vertices.resize(headVertices);
        for (size_t i = 0; i < tailVertices; ++i)
            vertices.push_back(nextValue++);
        // The graph's index pattern depends only on where it starts, so its length changes only how far it runs.
        indices.resize(headIndices);
        const size_t tailIndices = tailVertices * 2;
        for (size_t i = 0; i < tailIndices; ++i)
            indices.push_back(static_cast<uint16_t>(headVertices + i));
    };

    for (int frame = 0; frame < 20000; ++frame) {
        const uint32_t kind = random() % 100;
        if (frame == 0 || kind < 3) {
            headVertices = 50 + random() % 100;
            headIndices = headVertices * 3 / 2;
            vertices.clear();
            for (uint32_t i = 0; i < headVertices; ++i)
                vertices.push_back(nextValue++);
            indices.clear();
            for (uint32_t i = 0; i < headIndices; ++i)
                indices.push_back(static_cast<uint16_t>((i + nextValue) % headVertices));
            setTail();
            geometry.NoteWholeFrame();
        } else if (kind < 90) {
            setTail();
            geometry.NoteTailFrame(headVertices, headIndices);
        }  // else: a cached redraw of the same frame

        if (random() % 100 < 5) {
            ++skipped;
            continue;  // the slot was in flight: no draw, nothing written
        }
        const size_t slot = random() % kSlots;
        const std::vector<uint16_t>& held = geometry.IndicesToHold(indices);
        ASSERT_LE(held.size(), kMaxIndices);
        const auto plan = geometry.PlanSlot(slot, vertices.size());
        if (plan.writeVertices) {
            std::copy(vertices.begin() + static_cast<std::ptrdiff_t>(plan.firstVertex), vertices.end(),
                      vertexMemory[slot].begin() + static_cast<std::ptrdiff_t>(plan.firstVertex));
            ++(plan.firstVertex == 0 ? wholeWrites : tailWrites);
        }
        if (plan.writeIndices)
            std::copy(held.begin(), held.end(), indexMemory[slot].begin());
        geometry.MarkWritten(slot);

        ASSERT_TRUE(std::equal(vertices.begin(), vertices.end(), vertexMemory[slot].begin())) << "frame " << frame;
        ASSERT_TRUE(std::equal(indices.begin(), indices.end(), indexMemory[slot].begin())) << "frame " << frame;
    }
    EXPECT_GT(tailWrites, wholeWrites) << "the point of it";
    EXPECT_GT(skipped, 0u);
}
