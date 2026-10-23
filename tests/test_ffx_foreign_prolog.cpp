#include <gtest/gtest.h>

#include "hook/hooking/inline_hook_policy.h"

namespace {

using ce::inline_hook_policy::DeepHookPrologUndo;
using ce::inline_hook_policy::TryAnalyzeDeepHookProlog;

// AMD 4.0.1.2740, the native runtime OptiScaler calls through its Detours originals. A five-byte entry
// patch steals seven configure bytes and eleven destroy bytes, including unused volatile-register writes.
TEST(FFXForeignPrologTest, ConfigurePreservesBothArgumentsBelowTheForeignEntry) {
    constexpr unsigned char configure[] = {0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8B, 0xC9};
    DeepHookPrologUndo undo;
    ASSERT_TRUE(TryAnalyzeDeepHookProlog(configure, sizeof(configure), &undo, 2));
    EXPECT_EQ(undo.stackDelta, 0x28);
    EXPECT_FALSE(undo.restoreRbp);
    EXPECT_FALSE(TryAnalyzeDeepHookProlog(configure, sizeof(configure), &undo));
    EXPECT_FALSE(TryAnalyzeDeepHookProlog(configure, sizeof(configure) - 1, &undo, 2));
}

TEST(FFXForeignPrologTest, DestroyCanReplayTheCookieLoadIntoTheReturnRegister) {
    constexpr unsigned char destroy[] = {0x48, 0x83, 0xEC, 0x38, 0x48, 0x8B, 0x05, 0x95, 0x36, 0x50, 0x02};
    DeepHookPrologUndo undo;
    ASSERT_TRUE(TryAnalyzeDeepHookProlog(destroy, sizeof(destroy), &undo, 2));
    EXPECT_EQ(undo.stackDelta, 0x38);
    EXPECT_FALSE(TryAnalyzeDeepHookProlog(destroy, sizeof(destroy), &undo));
    EXPECT_FALSE(TryAnalyzeDeepHookProlog(destroy, sizeof(destroy) - 1, &undo, 2));
}

TEST(FFXForeignPrologTest, NeverLosesAnArgumentOrANonvolatileRegister) {
    constexpr unsigned char losesRdx[] = {0x48, 0x8B, 0xD1};
    constexpr unsigned char losesR8[] = {0x4C, 0x8B, 0xC1};
    constexpr unsigned char losesRbx[] = {0x48, 0x8B, 0xD9};
    DeepHookPrologUndo undo;
    EXPECT_FALSE(TryAnalyzeDeepHookProlog(losesRdx, sizeof(losesRdx), &undo, 2));
    EXPECT_FALSE(TryAnalyzeDeepHookProlog(losesR8, sizeof(losesR8), &undo, 3));
    EXPECT_FALSE(TryAnalyzeDeepHookProlog(losesRbx, sizeof(losesRbx), &undo, 2));
}

TEST(FFXForeignPrologTest, ScratchWritesInvalidateTheHomeSpaceBaseProof) {
    constexpr unsigned char prolog[] = {0x48, 0x8B, 0xC4, 0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x48, 0x89, 0x48, 8};
    DeepHookPrologUndo undo;
    EXPECT_FALSE(TryAnalyzeDeepHookProlog(prolog, sizeof(prolog), &undo, 2));
}

TEST(FFXForeignPrologTest, CetLandingInstructionPreservesTheCallState) {
    constexpr unsigned char prolog[] = {0xF3, 0x0F, 0x1E, 0xFA, 0x41, 0x57};
    DeepHookPrologUndo undo;
    ASSERT_TRUE(TryAnalyzeDeepHookProlog(prolog, sizeof(prolog), &undo));
    EXPECT_EQ(undo.stackDelta, 8);
    EXPECT_FALSE(TryAnalyzeDeepHookProlog(prolog, 3, &undo));
}

}  // namespace
