#include <gtest/gtest.h>

#include <array>
#include <filesystem>

#include "../hook/common/dx12_overlay_policy/inline_upload_slots.h"
#include "source_fragment_reader.h"

namespace {
using ce::dx12_overlay_policy::InlineUploadSlots;

TEST(DX12InlineUploadSlotsTest, LatestCommittedObservationDoesNotChangeReuseSafety) {
    InlineUploadSlots<4> slots;
    EXPECT_EQ(slots.LastCommitted(), 4u);
    const auto pending = [](std::size_t, uint32_t guard) { return guard == 0; };
    ASSERT_EQ(slots.FindReusable(pending), 0);
    EXPECT_EQ(slots.LastCommitted(), 4u);
    slots.Commit(0);
    EXPECT_EQ(slots.LastCommitted(), 0u);
    ASSERT_EQ(slots.FindReusable(pending), 1);
    EXPECT_EQ(slots.LastCommitted(), 0u);
    slots.Commit(1);
    EXPECT_EQ(slots.LastCommitted(), 1u);
    EXPECT_EQ(slots.FindReusable([](std::size_t, uint32_t) { return true; }), 0);
    slots.Commit(0);
    EXPECT_EQ(slots.LastCommitted(), 0u);
    EXPECT_EQ(slots.Guard(0), 2u);
    EXPECT_EQ(slots.FindReusable(pending), 2);
    slots = {};
    EXPECT_EQ(slots.LastCommitted(), 4u);
    EXPECT_EQ(slots.Count(), 0u);
}

TEST(DX12InlineUploadSlotsTest, DelayedGpuGrowsPastTheOldBlindSixteenFrameRing) {
    InlineUploadSlots<128> slots;
    std::array<uint32_t, 128> completed{};
    const auto ready = [&](std::size_t slot, uint32_t guard) {
        return guard == 0 || completed[slot] == guard;
    };
    // Record 40 callbacks before allowing any GPU completion. Every upload
    // must have different storage, including two renders in one callback.
    for (int frame = 0; frame < 40; ++frame) {
        const int slot = slots.FindReusable(ready);
        ASSERT_EQ(slot, frame);
        EXPECT_EQ(slots.Commit(static_cast<std::size_t>(slot)), 1u);
    }
    EXPECT_EQ(slots.Count(), 40u);
    completed[7] = 1;
    EXPECT_EQ(slots.FindReusable(ready), 7);
    EXPECT_EQ(slots.Commit(7), 2u);
    // A stale completion from the prior use cannot retire this draw.
    EXPECT_EQ(slots.FindReusable(ready), 40);
}

TEST(DX12InlineUploadSlotsTest, ReusesOnlyCompletedSlotsInAnyCompletionOrder) {
    InlineUploadSlots<4> slots;
    std::array<uint32_t, 4> completed{};
    const auto ready = [&](std::size_t slot, uint32_t guard) {
        return guard == 0 || completed[slot] == guard;
    };
    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(slots.FindReusable(ready), i);
        slots.Commit(static_cast<std::size_t>(i));
    }
    EXPECT_EQ(slots.FindReusable(ready), -1);
    completed[3] = 1;
    EXPECT_EQ(slots.FindReusable(ready), 3);
    slots.Commit(3);
    EXPECT_EQ(slots.FindReusable(ready), -1);
    completed[1] = 1;
    EXPECT_EQ(slots.FindReusable(ready), 1);
}

TEST(DX12InlineUploadSlotsTest, AllocationFailureWithoutARecordedDrawDoesNotStrandASlot) {
    InlineUploadSlots<4> slots;
    const auto ready = [](std::size_t, uint32_t guard) { return guard == 0; };
    EXPECT_EQ(slots.FindReusable(ready), 0);
    // The allocation failed before any command referenced it: no Commit.
    EXPECT_EQ(slots.FindReusable(ready), 0);
    EXPECT_EQ(slots.Count(), 1u);
}

TEST(DX12InlineUploadSlotsTest, DoesNotAdoptAnExternalFenceSlotBeforeThatFenceCompletes) {
    InlineUploadSlots<32> slots;
    EXPECT_EQ(slots.FindReusable([](std::size_t slot, uint32_t) { return slot >= 16; }), 16);
    EXPECT_EQ(slots.Commit(16), 1u);
    EXPECT_EQ(slots.FindReusable([](std::size_t slot, uint32_t guard) {
        return slot >= 16 && guard == 0;
    }), 17);
}

TEST(DX12InlineUploadSlotsTest, SteadyCompletedDrawsReuseStorageWithoutFurtherGrowth) {
    InlineUploadSlots<128> slots;
    uint32_t completed = 0;
    for (int frame = 0; frame < 1000; ++frame) {
        ASSERT_EQ(slots.FindReusable([&](std::size_t, uint32_t guard) { return guard == completed; }), 0);
        completed = slots.Commit(0);
    }
    EXPECT_EQ(slots.Count(), 1u);
}

TEST(DX12InlineUploadSlotsTest, CallbackCompletionUsesTheOriginalListAndServiceThreadRetirement) {
    const auto read = [](const char* path) {
        return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / path);
    };
    const auto backend = read("hook/common/custom_overlay_dx12.cpp");
    EXPECT_NE(backend.find("AcquireInlineUploadSlot()"), std::string::npos);
    EXPECT_NE(backend.find("D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT"), std::string::npos);
    EXPECT_NE(backend.find("MarkInlineUploadComplete(inlineList.Get(), slot)"), std::string::npos);
    const auto marker = read("hook/common/custom_overlay_dx12_inline_upload.cpp");
    EXPECT_EQ(marker.find("->Signal("), std::string::npos);
    EXPECT_EQ(marker.find("->ExecuteCommandLists("), std::string::npos);
    EXPECT_EQ(marker.find("WaitForSingleObject("), std::string::npos);
    const auto adapter = read("hook/common/overlay_adapter.cpp");
    EXPECT_NE(adapter.find("retiringDX12->HasInlineUploadsInFlight()"), std::string::npos);
    EXPECT_NE(adapter.find("CustomOverlay::RetireDX12Backend(retiringDX12)"), std::string::npos);
    const auto worker = read("hook/main_hookthread.cpp");
    EXPECT_NE(worker.find("CustomOverlay::CollectRetiredDX12Backends()"), std::string::npos);
    const auto callback = read("hook/apis/dx12_hook_ffx_overlay_adapter.cpp");
    EXPECT_LT(callback.find("CanReuseWarmDX12OverlayBackend"), callback.find("queueLock(g_CommandQueueMutex)"));
}

}  // namespace
