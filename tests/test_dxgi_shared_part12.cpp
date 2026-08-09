#include "test_dxgi_shared_shared.h"

#include "../hook/common/dxgi_shared_detail/steam_null_callback.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using DXGIShared::detail::kSteamNullCallbackMaxSlots;
using DXGIShared::detail::FindSteamNullCallbackSlotCandidates;

// RoboCop: Rogue City session 20260809_141705 crashed in
// gameoverlayrenderer64!OverlayHookD3D3: the handler loads its Present-shaped
// rendering callback with `mov rax,[rip+disp]` and calls it with `call rax`;
// on that Steam build (2026-08-03) the slot is at RVA 0x167340 (not the legacy
// 0x1621d8), and it was still NULL on the real swapchain. The proactive slot
// discovery must find the slot from the code pattern alone.
TEST(DXGISharedSourceTest, SteamNullCallbackScannerFindsRoboCopX64Pattern) {
    constexpr uintptr_t kModuleStart = 0x1000;
    constexpr uintptr_t kModuleEnd = 0x2000;
    constexpr uintptr_t kSlot = 0x1500;

    std::vector<uint8_t> code(0x200, 0x90);
    const size_t movOffset = 0x100;  // 48 8B 05 <disp32>
    const int32_t disp = static_cast<int32_t>(kSlot - (kModuleStart + movOffset + 7));
    code[movOffset] = 0x48;
    code[movOffset + 1] = 0x8B;
    code[movOffset + 2] = 0x05;
    std::memcpy(code.data() + movOffset + 3, &disp, sizeof(disp));
    code[movOffset + 0x0F] = 0xFF;  // call rax at lead distance 8
    code[movOffset + 0x10] = 0xD0;

    uintptr_t slots[kSteamNullCallbackMaxSlots] = {};
    const size_t found = FindSteamNullCallbackSlotCandidates(
        code.data(), code.size(), kModuleStart, kModuleEnd, slots, kSteamNullCallbackMaxSlots, true);
    ASSERT_EQ(found, 1u);
    EXPECT_EQ(slots[0], kSlot);
}

TEST(DXGISharedSourceTest, SteamNullCallbackScannerRejectsOutOfModuleSlot) {
    constexpr uintptr_t kModuleStart = 0x1000;
    constexpr uintptr_t kModuleEnd = 0x2000;
    constexpr uintptr_t kSlotOutside = 0x3000;

    std::vector<uint8_t> code(0x200, 0x90);
    const size_t movOffset = 0x100;
    const int32_t disp = static_cast<int32_t>(kSlotOutside - (kModuleStart + movOffset + 7));
    code[movOffset] = 0x48;
    code[movOffset + 1] = 0x8B;
    code[movOffset + 2] = 0x05;
    std::memcpy(code.data() + movOffset + 3, &disp, sizeof(disp));
    code[movOffset + 0x0F] = 0xFF;
    code[movOffset + 0x10] = 0xD0;

    uintptr_t slots[kSteamNullCallbackMaxSlots] = {};
    const size_t found = FindSteamNullCallbackSlotCandidates(
        code.data(), code.size(), kModuleStart, kModuleEnd, slots, kSteamNullCallbackMaxSlots, true);
    EXPECT_EQ(found, 0u);
}

TEST(DXGISharedSourceTest, SteamNullCallbackScannerRejectsMissingCallRax) {
    constexpr uintptr_t kModuleStart = 0x1000;
    constexpr uintptr_t kModuleEnd = 0x2000;
    constexpr uintptr_t kSlot = 0x1500;

    std::vector<uint8_t> code(0x200, 0x90);
    const size_t movOffset = 0x100;
    const int32_t disp = static_cast<int32_t>(kSlot - (kModuleStart + movOffset + 7));
    code[movOffset] = 0x48;
    code[movOffset + 1] = 0x8B;
    code[movOffset + 2] = 0x05;
    std::memcpy(code.data() + movOffset + 3, &disp, sizeof(disp));
    // No FF D0 within the lead window (all NOPs).

    uintptr_t slots[kSteamNullCallbackMaxSlots] = {};
    const size_t found = FindSteamNullCallbackSlotCandidates(
        code.data(), code.size(), kModuleStart, kModuleEnd, slots, kSteamNullCallbackMaxSlots, true);
    EXPECT_EQ(found, 0u);
}

TEST(DXGISharedSourceTest, SteamNullCallbackScannerRespectsLeadWindowAndCap) {
    constexpr uintptr_t kModuleStart = 0x1000;
    constexpr uintptr_t kModuleEnd = 0x4000;
    constexpr uintptr_t kSlot = 0x1500;

    std::vector<uint8_t> code(0x400, 0x90);
    for (size_t candidate = 0; candidate < 3; ++candidate) {
        const size_t movOffset = 0x100 + candidate * 0x100;
        const int32_t disp = static_cast<int32_t>(kSlot + candidate * 0x10 - (kModuleStart + movOffset + 7));
        code[movOffset] = 0x48;
        code[movOffset + 1] = 0x8B;
        code[movOffset + 2] = 0x05;
        std::memcpy(code.data() + movOffset + 3, &disp, sizeof(disp));
        code[movOffset + 0x0F] = 0xFF;
        code[movOffset + 0x10] = 0xD0;
    }

    uintptr_t slots[kSteamNullCallbackMaxSlots] = {};
    const size_t found = FindSteamNullCallbackSlotCandidates(
        code.data(), code.size(), kModuleStart, kModuleEnd, slots, kSteamNullCallbackMaxSlots, true);
    ASSERT_EQ(found, 3u);

    // MaxSlots caps the result.
    uintptr_t capped[kSteamNullCallbackMaxSlots] = {};
    const size_t cappedFound = FindSteamNullCallbackSlotCandidates(
        code.data(), code.size(), kModuleStart, kModuleEnd, capped, 2, true);
    EXPECT_EQ(cappedFound, 2u);
}

TEST(DXGISharedSourceTest, SteamNullCallbackScannerHandlesX86Patterns) {
    constexpr uintptr_t kModuleStart = 0x1000;
    constexpr uintptr_t kModuleEnd = 0x2000;
    constexpr uintptr_t kSlot = 0x1500;

    // mov eax, [abs32] (A1) followed by call eax.
    std::vector<uint8_t> codeA1(0x100, 0x90);
    codeA1[0x40] = 0xA1;
    std::memcpy(codeA1.data() + 0x41, &kSlot, sizeof(uint32_t));
    codeA1[0x48] = 0xFF;
    codeA1[0x49] = 0xD0;

    uintptr_t slots[kSteamNullCallbackMaxSlots] = {};
    const size_t foundA1 = FindSteamNullCallbackSlotCandidates(
        codeA1.data(), codeA1.size(), kModuleStart, kModuleEnd, slots, kSteamNullCallbackMaxSlots, false);
    ASSERT_EQ(foundA1, 1u);
    EXPECT_EQ(slots[0], kSlot);

    // mov eax, [abs32] (8B 05) followed by call eax.
    std::vector<uint8_t> code8B(0x100, 0x90);
    code8B[0x40] = 0x8B;
    code8B[0x41] = 0x05;
    std::memcpy(code8B.data() + 0x42, &kSlot, sizeof(uint32_t));
    code8B[0x4A] = 0xFF;
    code8B[0x4B] = 0xD0;

    const size_t found8B = FindSteamNullCallbackSlotCandidates(
        code8B.data(), code8B.size(), kModuleStart, kModuleEnd, slots, kSteamNullCallbackMaxSlots, false);
    ASSERT_EQ(found8B, 1u);
    EXPECT_EQ(slots[0], kSlot);
}

// Regression for session 20260809_143040: an 8-slot cap stopped the module
// scan before the actual faulting slot (steam+0x167340) was reached, because
// an earlier cluster of six NULL slots (steam+0x1668B0..0x166990) consumed
// the budget. The production cap must be large enough to cover every candidate
// in a real module, and a full-size scan must find the late site too.
TEST(DXGISharedSourceTest, SteamNullCallbackScannerDoesNotTruncateAtSmallCap) {
    EXPECT_GE(kSteamNullCallbackMaxSlots, 64u);

    constexpr uintptr_t kModuleStart = 0x1000;
    constexpr uintptr_t kModuleEnd = 0x20000;
    std::vector<uint8_t> code(0x19000, 0x90);

    size_t expected = 0;
    constexpr uintptr_t kClusterBaseSlot = 0x8000;
    for (size_t candidate = 0; candidate < 6; ++candidate) {
        const size_t movOffset = 0x100 + candidate * 0x40;
        const uintptr_t slot = kClusterBaseSlot + candidate * 0x10;
        const int32_t disp = static_cast<int32_t>(slot - (kModuleStart + movOffset + 7));
        code[movOffset] = 0x48;
        code[movOffset + 1] = 0x8B;
        code[movOffset + 2] = 0x05;
        std::memcpy(code.data() + movOffset + 3, &disp, sizeof(disp));
        code[movOffset + 0x0F] = 0xFF;
        code[movOffset + 0x10] = 0xD0;
        ++expected;
    }

    // The late crash-site candidate (steam+0x167340 analog), far beyond the
    // earlier cluster.
    const size_t lateOffset = 0x1000;
    constexpr uintptr_t kLateSlot = 0x9000;
    const int32_t lateDisp = static_cast<int32_t>(kLateSlot - (kModuleStart + lateOffset + 7));
    code[lateOffset] = 0x48;
    code[lateOffset + 1] = 0x8B;
    code[lateOffset + 2] = 0x05;
    std::memcpy(code.data() + lateOffset + 3, &lateDisp, sizeof(lateDisp));
    code[lateOffset + 0x0F] = 0xFF;
    code[lateOffset + 0x10] = 0xD0;
    ++expected;

    uintptr_t slots[kSteamNullCallbackMaxSlots] = {};
    const size_t found = FindSteamNullCallbackSlotCandidates(
        code.data(), code.size(), kModuleStart, kModuleEnd, slots, kSteamNullCallbackMaxSlots, true);
    EXPECT_EQ(found, expected);

    bool foundLate = false;
    for (size_t i = 0; i < found; ++i) {
        if (slots[i] == kLateSlot) {
            foundLate = true;
        }
    }
    EXPECT_TRUE(foundLate);
}

TEST(DXGISharedSourceTest, SteamNullCallbackProactivePatchIsWiredBeforeEverySteamInvoke) {
    namespace fs = std::filesystem;
    const fs::path steamSource = fs::current_path() / "hook" / "common" / "dxgi_shared_steam.cpp";
    ASSERT_TRUE(fs::exists(steamSource));
    const std::string steam = ce::test_source::ReadFile(steamSource);
    ASSERT_FALSE(steam.empty());

    const size_t guardedEntry = steam.find("bool TryInvokeGuardedExternalSteamOverlayPresent(");
    const size_t threadGate = steam.find("if (!synchronousPresentThreadAllowed)", guardedEntry);
    const size_t proactivePatch = steam.find("EnsureSteamNullCallbacksPatched(presentBypass)", threadGate);
    const size_t callbackRead = steam.find("TryReadSteamOverlayNullCallbackSlot(", proactivePatch);
    const size_t invoke = steam.find("const HRESULT hr = externalPresent", callbackRead);
    ASSERT_NE(guardedEntry, std::string::npos);
    ASSERT_NE(threadGate, std::string::npos);
    ASSERT_NE(proactivePatch, std::string::npos);
    ASSERT_NE(callbackRead, std::string::npos);
    ASSERT_NE(invoke, std::string::npos);
    EXPECT_LT(threadGate, proactivePatch);
    EXPECT_LT(proactivePatch, callbackRead);
    EXPECT_LT(callbackRead, invoke);

    const size_t dynamicRead = steam.find("DiscoverSteamNullCallbackSlots(", steam.find("bool TryReadSteamOverlayNullCallbackSlot("));
    EXPECT_NE(dynamicRead, std::string::npos);
}

TEST(DXGISharedSourceTest, SlFastPathProactivePatchPrecedesTheSteamE9Return) {
    namespace fs = std::filesystem;
    const fs::path originalSource = fs::current_path() / "hook" / "common" / "dxgi_shared_original.cpp";
    ASSERT_TRUE(fs::exists(originalSource));
    const std::string original = ce::test_source::ReadFile(originalSource);
    ASSERT_FALSE(original.empty());

    const size_t fastPath = original.find("if (slLoaded && presentOriginal && presentOriginal != DetourPresent)");
    ASSERT_NE(fastPath, std::string::npos);
    const size_t proactivePatch = original.find("EnsureSteamNullCallbacksPatched(presentBypass)", fastPath);
    const size_t fastPathReturn = original.find("return presentOriginal(pSwapChain, SyncInterval, Flags);", fastPath);
    ASSERT_NE(proactivePatch, std::string::npos);
    ASSERT_NE(fastPathReturn, std::string::npos);
    EXPECT_LT(proactivePatch, fastPathReturn);
}

} // namespace
