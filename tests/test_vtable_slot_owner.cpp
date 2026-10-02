#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

#include "hook/hooking/vtable_slot_owner.h"
#include "source_fragment_reader.h"

namespace owner = ce::vtable_slot_owner;

namespace {

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

}  // namespace

// A slot can name DXGI's own function and still hand every call elsewhere through a
// jump at the function's entry (CE's ResizeBuffers body hook is one). The refused-resize
// diagnostics resolve both shapes an inline hook leaves there.
TEST(VTableSlotOwnerTest, ResolvesRelativeAndIndirectEntryJumps) {
    uint8_t relative[16] = {0xE9};
    const int32_t displacement = 0x20;
    std::memcpy(relative + 1, &displacement, sizeof(displacement));
    EXPECT_EQ(owner::EntryJumpTarget(relative), relative + 5 + displacement);
    EXPECT_EQ(owner::IndirectJumpSlot(relative), nullptr);

    alignas(8) uint8_t indirect[32] = {0xFF, 0x25};
    const int32_t slotDisplacement = 2;  // pointer stored 2 bytes after the 6-byte instruction
    std::memcpy(indirect + 2, &slotDisplacement, sizeof(slotDisplacement));
    const void* target = reinterpret_cast<const void*>(static_cast<uintptr_t>(0x7FF612345678ull));
    std::memcpy(indirect + 8, static_cast<const void*>(&target), sizeof(target));
    EXPECT_EQ(owner::IndirectJumpSlot(indirect), indirect + 8);
    EXPECT_EQ(owner::EntryJumpTarget(indirect), target);

    const uint8_t prolog[8] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83};
    EXPECT_EQ(owner::EntryJumpTarget(prolog), nullptr);
    EXPECT_EQ(owner::EntryJumpTarget(nullptr), nullptr);
}

TEST(VTableSlotOwnerTest, CoversTheSlotsAnOverlayHooksToDrawAndFollowResizes) {
    bool resize = false;
    bool present = false;
    for (const auto& slot : owner::kOverlayHookedSwapchainSlots) {
        resize |= slot.index == 13;
        present |= slot.index == 8;
    }
    EXPECT_TRUE(resize);
    EXPECT_TRUE(present);
}

// Logged at every refused resize, and for the first resizes as the baseline.
TEST(VTableSlotOwnerTest, RefusedResizeLogsTheSwapchainSlotOwners) {
    const std::string resize = ReadSource("hook/present/dxgi_shared_resize.cpp");
    const size_t begin = resize.find("void EndD3D12ResizeDiagnostics(");
    ASSERT_NE(begin, std::string::npos);
    const size_t owners = resize.find("LogSwapchainVTableSlotOwners(pSwapChain, source, FAILED(hr)", begin);
    ASSERT_NE(owners, std::string::npos);
    EXPECT_NE(resize.find("if (FAILED(hr) || s_slotOwnerBaselines", begin), std::string::npos);
}
