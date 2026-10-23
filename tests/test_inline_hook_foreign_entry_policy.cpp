#include <gtest/gtest.h>

#include "hook/hooking/inline_hook_policy.h"

namespace {

using ce::inline_hook_policy::ClassifyEntryJumpOwner;
using ce::inline_hook_policy::EntryJumpOwner;
using ce::inline_hook_policy::ForeignEntryPolicy;
using ce::inline_hook_policy::ShouldLeaveEntryToForeignOwner;

// sl.common!slGetPluginFunction as shipped (push rbx; sub rsp,20h; lea rdx,...), and the same entry
// after OptiScaler's Detours patch (E9 to its relay, int3 filling the sixth stolen byte). Both from
// the Talos crash dump of session 20261023_173151.
constexpr unsigned char kSlCommonPristine[8] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8D};
constexpr unsigned char kSlCommonOptiScalerPatched[8] = {0xE9, 0xB3, 0x0E, 0xC1, 0x2F, 0xCC, 0x48, 0x8D};

TEST(InlineHookForeignEntryPolicy, AJumpThatIsNotTheImagesOwnCodeIsForeign) {
    EXPECT_EQ(ClassifyEntryJumpOwner(kSlCommonOptiScalerPatched, kSlCommonPristine, true), EntryJumpOwner::kForeign);
    EXPECT_EQ(ClassifyEntryJumpOwner(kSlCommonOptiScalerPatched, kSlCommonPristine, false), EntryJumpOwner::kForeign);
}

TEST(InlineHookForeignEntryPolicy, AModulesOwnEntryJumpIsNotMistakenForAPatch) {
    // A tail-jump thunk the module shipped: the live bytes are the image's bytes.
    constexpr unsigned char tailJump[8] = {0xE9, 0x10, 0x20, 0x00, 0x00, 0xCC, 0xCC, 0xCC};
    EXPECT_EQ(ClassifyEntryJumpOwner(tailJump, tailJump, true), EntryJumpOwner::kModule);

    constexpr unsigned char importThunk[8] = {0xFF, 0x25, 0x12, 0x34, 0x00, 0x00, 0x90, 0x90};
    EXPECT_EQ(ClassifyEntryJumpOwner(importThunk, importThunk, true), EntryJumpOwner::kModule);

    // Only the jump itself has to match; what follows it is not part of the ownership question.
    constexpr unsigned char laterBytesDiffer[8] = {0xE9, 0x10, 0x20, 0x00, 0x00, 0x90, 0x90, 0x90};
    EXPECT_EQ(ClassifyEntryJumpOwner(laterBytesDiffer, tailJump, true), EntryJumpOwner::kModule);
}

TEST(InlineHookForeignEntryPolicy, AnIndirectJumpPatchIsComparedOverItsFullLength) {
    constexpr unsigned char pristine[8] = {0xFF, 0x25, 0x12, 0x34, 0x00, 0x00, 0x90, 0x90};
    constexpr unsigned char retargeted[8] = {0xFF, 0x25, 0x12, 0x34, 0x00, 0x01, 0x90, 0x90};
    EXPECT_EQ(ClassifyEntryJumpOwner(retargeted, pristine, true), EntryJumpOwner::kForeign);
    // x86 has no RIP-relative FF 25 form CE chains to.
    EXPECT_EQ(ClassifyEntryJumpOwner(retargeted, pristine, false), EntryJumpOwner::kNoJump);
}

TEST(InlineHookForeignEntryPolicy, UnreadablePristineBytesCannotProveTheModuleOwnsTheJump) {
    EXPECT_EQ(ClassifyEntryJumpOwner(kSlCommonOptiScalerPatched, nullptr, true), EntryJumpOwner::kForeign);
}

TEST(InlineHookForeignEntryPolicy, AnEntryWithoutAChainableJumpHasNoOwnerQuestion) {
    EXPECT_EQ(ClassifyEntryJumpOwner(kSlCommonPristine, kSlCommonPristine, true), EntryJumpOwner::kNoJump);
    EXPECT_EQ(ClassifyEntryJumpOwner(kSlCommonPristine, nullptr, true), EntryJumpOwner::kNoJump);
    constexpr unsigned char callEntry[8] = {0xE8, 0x00, 0x10, 0x00, 0x00, 0x90, 0x90, 0x90};
    EXPECT_EQ(ClassifyEntryJumpOwner(callEntry, nullptr, true), EntryJumpOwner::kNoJump);
    EXPECT_EQ(ClassifyEntryJumpOwner(nullptr, kSlCommonPristine, true), EntryJumpOwner::kNoJump);
}

TEST(InlineHookForeignEntryPolicy, OnlyTheLeavePolicyLeavesAForeignEntry) {
    // The default keeps prepending: every hook family that did not opt in is unchanged.
    EXPECT_FALSE(ShouldLeaveEntryToForeignOwner(ForeignEntryPolicy::kPrepend, EntryJumpOwner::kForeign));
    EXPECT_FALSE(ShouldLeaveEntryToForeignOwner(ForeignEntryPolicy::kPrepend, EntryJumpOwner::kModule));
    EXPECT_FALSE(ShouldLeaveEntryToForeignOwner(ForeignEntryPolicy::kPrepend, EntryJumpOwner::kNoJump));

    EXPECT_TRUE(ShouldLeaveEntryToForeignOwner(ForeignEntryPolicy::kLeaveToOwner, EntryJumpOwner::kForeign));
    // The module's own jump and a plain prologue are CE's to hook as before.
    EXPECT_FALSE(ShouldLeaveEntryToForeignOwner(ForeignEntryPolicy::kLeaveToOwner, EntryJumpOwner::kModule));
    EXPECT_FALSE(ShouldLeaveEntryToForeignOwner(ForeignEntryPolicy::kLeaveToOwner, EntryJumpOwner::kNoJump));
}

TEST(InlineHookForeignEntryPolicy, TheTalosOptiScalerEntryIsLeftUntouched) {
    // The decision the crash needed: CE's 5-byte prepend rewrote OptiScaler's jump displacement, its
    // 8-byte fingerprint stopped matching, and it detoured again on top of CE.
    const auto owner = ClassifyEntryJumpOwner(kSlCommonOptiScalerPatched, kSlCommonPristine, true);
    EXPECT_TRUE(ShouldLeaveEntryToForeignOwner(ForeignEntryPolicy::kLeaveToOwner, owner));
}

}  // namespace
