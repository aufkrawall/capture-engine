#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "hook/hooking/deep_hook_x86_policy.h"

namespace {
using ce::deep_hook_x86::BuildPatch;
using ce::deep_hook_x86::PatchSize;
using ce::deep_hook_x86::PrologUndo;
using ce::deep_hook_x86::TryAnalyzeProlog;

// Prologues read from SysWOW64\dxgi.dll 10.0.26100.9711 (session 20261017_000225).
constexpr unsigned char kPresent[] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8,
                                      0x83, 0xEC, 0x38, 0xA1, 0xC0, 0x96, 0x0D, 0x10};
constexpr unsigned char kResizeBuffers[] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8,
                                            0x81, 0xEC, 0xEC, 0x00, 0x00, 0x00, 0xA1, 0xC0};
constexpr unsigned char kSetColorSpace1[] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08, 0x6A, 0x01};

uint32_t ReadRel(const unsigned char* bytes) {
    uint32_t rel = 0;
    std::memcpy(&rel, bytes, sizeof(rel));
    return rel;
}

TEST(DeepHookX86PolicyTest, HotPatchEntryResumesWithPopEbpOnly) {
    // Steam's x86 entry patch covers exactly these five bytes; resuming at +5 leaves esp == ebp.
    PrologUndo undo;
    ASSERT_TRUE(TryAnalyzeProlog(kPresent, 5, &undo));
    EXPECT_TRUE(undo.framePointerOnly);
    EXPECT_EQ(PatchSize(undo), 6);
}

TEST(DeepHookX86PolicyTest, ReservedSixByteSpanUndoesThroughTheFramePointer) {
    // The reserved 6-byte span ends after `and esp,-8`: only `mov esp,ebp; pop ebp` restores the entry.
    PrologUndo undo;
    ASSERT_TRUE(TryAnalyzeProlog(kPresent, 8, &undo));
    EXPECT_FALSE(undo.framePointerOnly);
    EXPECT_EQ(PatchSize(undo), 8);
    ASSERT_TRUE(TryAnalyzeProlog(kResizeBuffers, 14, &undo));
    EXPECT_FALSE(undo.framePointerOnly);
}

TEST(DeepHookX86PolicyTest, RefusesPrologsThatChangeRegistersOrLackAFramePointer) {
    PrologUndo undo;
    // mov ecx,[ebp+8] is not stack bookkeeping: undoing past it would hide an argument load.
    EXPECT_FALSE(TryAnalyzeProlog(kSetColorSpace1, 8, &undo));
    EXPECT_TRUE(TryAnalyzeProlog(kSetColorSpace1, 5, &undo));
    const unsigned char framePointerOmitted[] = {0x83, 0xEC, 0x10, 0x56, 0x57};
    EXPECT_FALSE(TryAnalyzeProlog(framePointerOmitted, 5, &undo));
    const unsigned char noMovEbp[] = {0x8B, 0xFF, 0x55, 0x56, 0x57};
    EXPECT_FALSE(TryAnalyzeProlog(noMovEbp, 5, &undo));
    // An instruction cut by the resume offset is never accepted.
    EXPECT_FALSE(TryAnalyzeProlog(kPresent, 7, &undo));
    EXPECT_FALSE(TryAnalyzeProlog(kPresent, 4, &undo));
}

TEST(DeepHookX86PolicyTest, AcceptsPushesAndStackReservationAfterTheFrame) {
    const unsigned char prolog[] = {0x55, 0x89, 0xE5, 0x51, 0x6A, 0xFE, 0x68, 0x11, 0x22, 0x33, 0x44,
                                    0x81, 0xEC, 0x00, 0x01, 0x00, 0x00, 0x83, 0xE4, 0xF0};
    PrologUndo undo;
    ASSERT_TRUE(TryAnalyzeProlog(prolog, static_cast<int>(sizeof(prolog)), &undo));
    EXPECT_FALSE(undo.framePointerOnly);
}

TEST(DeepHookX86PolicyTest, PatchUndoesThenJumpsToTheDetourAndPadsWithNop) {
    unsigned char patch[16] = {};
    PrologUndo framePointerOnly;
    framePointerOnly.framePointerOnly = true;
    ASSERT_TRUE(BuildPatch(framePointerOnly, 0x60731175u, 0x1BB01340u, 6, patch, sizeof(patch)));
    EXPECT_EQ(patch[0], 0x5D);
    EXPECT_EQ(patch[1], 0xE9);
    EXPECT_EQ(ReadRel(patch + 2), 0x1BB01340u - (0x60731175u + 6u));

    PrologUndo fullUndo;
    std::memset(patch, 0, sizeof(patch));
    ASSERT_TRUE(BuildPatch(fullUndo, 0x60731178u, 0x1BB01340u, 11, patch, sizeof(patch)));
    EXPECT_EQ(patch[0], 0x8B);
    EXPECT_EQ(patch[1], 0xE5);
    EXPECT_EQ(patch[2], 0x5D);
    EXPECT_EQ(patch[3], 0xE9);
    EXPECT_EQ(ReadRel(patch + 4), 0x1BB01340u - (0x60731178u + 8u));
    EXPECT_EQ(patch[8], 0x90);
    EXPECT_EQ(patch[9], 0x90);
    EXPECT_EQ(patch[10], 0x90);
}

TEST(DeepHookX86PolicyTest, PatchRefusesTooFewDisplacedBytesAndWrapsRelativeJumps) {
    unsigned char patch[16] = {};
    PrologUndo fullUndo;
    EXPECT_FALSE(BuildPatch(fullUndo, 0x1000u, 0x2000u, 7, patch, sizeof(patch)));
    EXPECT_FALSE(BuildPatch(fullUndo, 0x1000u, 0x2000u, 20, patch, sizeof(patch)));
    // rel32 is modular on x86: a detour below a high patch address is reached by wrapping.
    ASSERT_TRUE(BuildPatch(fullUndo, 0xFFFFFF00u, 0x00000100u, 8, patch, sizeof(patch)));
    EXPECT_EQ(0xFFFFFF00u + 8u + ReadRel(patch + 4), 0x00000100u);
}

}  // namespace
