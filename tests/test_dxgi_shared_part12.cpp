#include "test_dxgi_shared_shared.h"

#include "../hook/common/dxgi_shared_detail/steam_null_callback.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using DXGIShared::detail::kSteamNullCallbackMaxSlots;
using DXGIShared::detail::FindSteamNullCallbackSlotCandidates;

// FSR-FG -> all-FG-off (session 20260813_153118): the game-created recovery swapchain ended the
// runtime-owned native-FSR teardown and the first Present on it warm-reinited the overlay, then the
// late [outer] SL-FG-OFF observer force-cleared it + armed a 60-frame cooldown (missed=60 / 453 ms).
// Pin that the recovery-reinit proof feeds the keep-live decision and vetoes that teardown.
TEST(DXGISharedSourceTest, NativeFSRGameSwapchainRecoveryReinitKeepsOverlayLiveAcrossLateOuterOff) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    ASSERT_TRUE(fs::exists(source));
    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());

    const size_t processFrame = text.find("void ProcessFrame(");
    ASSERT_NE(processFrame, std::string::npos);
    const size_t recoveryDecision =
        text.find("ShouldReinitOverlayImmediatelyAfterGameSwapchainRecoveryFromNativeFSROff(", processFrame);
    const size_t proofLatch =
        text.find("nativeFSRGameSwapchainRecoveryReinitializedThisPresent", recoveryDecision);
    const size_t keepLiveDecision =
        text.find("ShouldKeepOverlayLiveAcrossNativeFSRGameSwapchainRecovery(", processFrame);
    const size_t forceReinitGuard =
        text.find("!keepOverlayLiveAcrossNativeFSRGameSwapchainRecovery", processFrame);
    ASSERT_NE(recoveryDecision, std::string::npos);
    ASSERT_NE(proofLatch, std::string::npos);
    ASSERT_NE(keepLiveDecision, std::string::npos);
    ASSERT_NE(forceReinitGuard, std::string::npos);
    EXPECT_LT(recoveryDecision, keepLiveDecision);
    EXPECT_LT(keepLiveDecision, forceReinitGuard)
        << "the FSR->off game-swapchain recovery reinit must veto the late [outer] SL-FG-OFF teardown";
}

// FSR FG -> all-FG-off with DLSS FG in between (session 20260813_155313): the game-created recovery
// swapchain REUSED the previous swapchain's COM pointer address, so the plain pointer-change detection
// never processed the replacement, the recovery reinit never ran, and the outer SL-FG-OFF teardown
// blanked the overlay for 60 presents / 484 ms again. Pin the exact one-shot lifetime proof that makes
// the ABA-reused game-created recovery swapchain detectable as a new lifetime.
TEST(DXGISharedSourceTest, ExactGameSwapchainRecoveryLifetimeProofArmsFeedsAndIsConsumedOnce) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    ASSERT_TRUE(fs::exists(source));
    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());

    // The teardown-end arms the exact one-shot lifetime proof together with the recovery queue.
    const size_t recoveryQueueArm =
        text.find("dx12_hook_g_PostNativeFSROffGameSwapchainRecoveryQueue.store(pQueue");
    ASSERT_NE(recoveryQueueArm, std::string::npos);
    const size_t proofArm = text.find("dx12_hook_g_ExactGameSwapchainRecoverySwapchain.store(associatedSwapchain",
                                      recoveryQueueArm);
    ASSERT_NE(proofArm, std::string::npos);
    EXPECT_LT(proofArm - recoveryQueueArm, static_cast<size_t>(400));

    // Phase1 feeds the proof into the logical-replacement decision so an ABA-equal pointer still counts
    // as a new swapchain lifetime.
    const size_t processFrame = text.find("void ProcessFrame(");
    ASSERT_NE(processFrame, std::string::npos);
    const size_t proofLeg = text.find("exactGameSwapchainRecoverySwapchainProof", processFrame);
    const size_t replacementDecision = text.find("ShouldProcessLogicalSwapchainReplacement(", processFrame);
    ASSERT_NE(proofLeg, std::string::npos);
    ASSERT_NE(replacementDecision, std::string::npos);
    EXPECT_LT(proofLeg, replacementDecision);

    // Phase2 consumes the proof exactly once at the top of the replacement handler, before any
    // preserve-path evaluation can return without touching the proof (a stale armed proof would
    // otherwise reprocess the replacement on every subsequent ABA-equal Present).
    const size_t phase2 = text.find("FrameProcessSession::Phase2()");
    ASSERT_NE(phase2, std::string::npos);
    const size_t consume = text.find("dx12_hook_g_ExactGameSwapchainRecoverySwapchain.compare_exchange_strong(",
                                     phase2);
    const size_t preserveEval =
        text.find("ShouldSuppressFreshRuntimeOwnedStreamlineNoFGSeparateOverlayWork(", phase2);
    ASSERT_NE(consume, std::string::npos);
    ASSERT_NE(preserveEval, std::string::npos);
    EXPECT_LT(consume, preserveEval);
}

// Every recovery-queue reset site must also clear the one-shot lifetime proof, so evidence armed by a
// teardown end that never reaches a Present cannot later turn a live swapchain into a spurious
// replacement.
TEST(DXGISharedSourceTest, ExactGameSwapchainRecoveryLifetimeProofClearedAtRecoveryQueueResetSites) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    ASSERT_TRUE(fs::exists(source));
    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());

    size_t cursor = 0;
    int sites = 0;
    while ((cursor = text.find("dx12_hook_g_PostNativeFSROffGameSwapchainRecoveryQueue.store(nullptr", cursor)) !=
           std::string::npos) {
        const size_t proofClear =
            text.find("dx12_hook_g_ExactGameSwapchainRecoverySwapchain.store(nullptr", cursor);
        ASSERT_NE(proofClear, std::string::npos) << "recovery-queue reset site without lifetime-proof clear";
        EXPECT_LT(proofClear - cursor, static_cast<size_t>(200));
        cursor = proofClear + 1;
        ++sites;
    }
    EXPECT_GE(sites, 3);
}

TEST(DXGISharedTest, KeepsOverlayLiveAcrossPrewarmedPostSLHandoffPreserve) {
    using ce::dx12_overlay_policy::ShouldKeepOverlayLiveAcrossPrewarmedPostSLHandoffPreserve;

    // Session 20260813_162959 (FSR FG -> DLSS FG after prior switch sequences): the first Present on
    // the fresh Streamline proxy preserved the exact prewarmed PostSL backend (RTVs already rebuilt
    // for the new swapchain lifetime), then the stale [outer] SL-FG-OFF observer force-cleared it +
    // armed a 60-frame cooldown; PostSL had to rebuild after a 203 ms dormancy -> visible blank.
    // The preserved backend is current by construction, so the outer teardown must keep it live.
    EXPECT_TRUE(ShouldKeepOverlayLiveAcrossPrewarmedPostSLHandoffPreserve(
        /*streamlineTurnedOff=*/true, /*exactPrewarmedBackendPreservedThisPresent=*/true,
        /*fsrFGApiActive=*/false, /*overlayInit=*/true, /*syncInit=*/true, /*deviceRemoved=*/false));

    // Not an OFF edge -> not this path.
    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossPrewarmedPostSLHandoffPreserve(false, true, false, true, true, false));
    // No prewarmed-handoff preservation proof this Present -> the generic teardown owns it.
    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossPrewarmedPostSLHandoffPreserve(true, false, false, true, true, false));
    // FSR still API-active -> a live FSR takeover keeps the protective teardown.
    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossPrewarmedPostSLHandoffPreserve(true, true, true, true, true, false));
    // Backend not init/sync -> nothing live to keep (let the reinit run).
    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossPrewarmedPostSLHandoffPreserve(true, true, false, false, true, false));
    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossPrewarmedPostSLHandoffPreserve(true, true, false, true, false, false));
    // Device removed -> don't keep rendering into a removed device.
    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossPrewarmedPostSLHandoffPreserve(true, true, false, true, true, true));
}

// The phase2 prewarmed-handoff preserve must latch a per-Present proof that the outer teardown
// consults, so the freshly preserved backend can never be torn down by the late outer SL-FG-OFF
// observer (session 20260813_162959: 203 ms PostSL dormancy after the outer teardown).
TEST(DXGISharedSourceTest, PrewarmedPostSLHandoffPreserveKeepsOverlayLiveAcrossLateOuterOff) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    ASSERT_TRUE(fs::exists(source));
    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());

    const size_t processFrame = text.find("void ProcessFrame(");
    ASSERT_NE(processFrame, std::string::npos);
    const size_t preserveDecision =
        text.find("ShouldPreserveExactPrewarmedPostSLHandoffBackendOnFirstPresent(", processFrame);
    const size_t proofLatch =
        text.find("exactPrewarmedPostSLHandoffBackendPreservedThisPresent = true", preserveDecision);
    const size_t keepLiveDecision =
        text.find("ShouldKeepOverlayLiveAcrossPrewarmedPostSLHandoffPreserve(", processFrame);
    const size_t drainSkipGuard = text.find("!keepOverlayLiveAcrossPrewarmedPostSLHandoffPreserve", processFrame);
    const size_t forceReinitGuard =
        text.find("!keepOverlayLiveAcrossPrewarmedPostSLHandoffPreserve", drainSkipGuard + 1);
    ASSERT_NE(preserveDecision, std::string::npos);
    ASSERT_NE(proofLatch, std::string::npos);
    ASSERT_NE(keepLiveDecision, std::string::npos);
    ASSERT_NE(drainSkipGuard, std::string::npos);
    ASSERT_NE(forceReinitGuard, std::string::npos);
    EXPECT_LT(preserveDecision, keepLiveDecision);
    EXPECT_LT(keepLiveDecision, forceReinitGuard)
        << "the FSR->DLSS prewarmed-handoff preserve must veto the late [outer] SL-FG-OFF teardown";
}

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

// Talos + DLSS FG + RTSS (session 20260812_022607): CE must never speculatively write into
// Steam's Present-shaped callback slots. Those slots are Steam's own hook-install outputs
// (gameoverlayrenderer64+0x8da00 takes &slot; every call site tests `cmpq $0, slot`
// afterwards), so pre-filling them makes Steam skip its own install and chain to a raw
// dxgi!Present copy that skips every hook BELOW Steam. Frame 1 ran CE -> Steam -> RTSS with
// all three overlays drawing; from frame 2 every guarded invoke reported the callback as
// CE's own bypass value and RTSS never submitted again. Slot discovery stays - CE reads the
// slots to decide whether invoking Steam is safe - but the write must not come back.
TEST(DXGISharedSourceTest, CENeverWritesIntoSteamCallbackSlots) {
    namespace fs = std::filesystem;
    const fs::path steamSource = fs::current_path() / "hook" / "common" / "dxgi_shared_steam.cpp";
    const fs::path originalSource = fs::current_path() / "hook" / "common" / "dxgi_shared_original.cpp";
    ASSERT_TRUE(fs::exists(steamSource));
    ASSERT_TRUE(fs::exists(originalSource));
    const std::string steam = ce::test_source::ReadFile(steamSource);
    const std::string original = ce::test_source::ReadFile(originalSource);
    ASSERT_FALSE(steam.empty());
    ASSERT_FALSE(original.empty());

    EXPECT_EQ(steam.find("EnsureSteamNullCallbacksPatched"), std::string::npos);
    EXPECT_EQ(original.find("EnsureSteamNullCallbacksPatched"), std::string::npos);

    // The read-only inspection that feeds the callback-state gate must remain, and the
    // guarded invoke must still consult it before entering Steam.
    const size_t guardedEntry = steam.find("bool TryInvokeGuardedExternalSteamOverlayPresent(");
    ASSERT_NE(guardedEntry, std::string::npos);
    const size_t threadGate = steam.find("if (!synchronousPresentThreadAllowed)", guardedEntry);
    const size_t callbackRead = steam.find("TryReadSteamOverlayNullCallbackSlot(", threadGate);
    const size_t invoke = steam.find("const HRESULT hr = externalPresent", callbackRead);
    ASSERT_NE(threadGate, std::string::npos);
    ASSERT_NE(callbackRead, std::string::npos);
    ASSERT_NE(invoke, std::string::npos);
    EXPECT_LT(threadGate, callbackRead);
    EXPECT_LT(callbackRead, invoke);

    const size_t dynamicRead = steam.find("DiscoverSteamNullCallbackSlots(", steam.find("bool TryReadSteamOverlayNullCallbackSlot("));
    EXPECT_NE(dynamicRead, std::string::npos);

    // The crash-time recovery stays: it resolves the EXACT faulting slot from the fault
    // context instead of writing into slots Steam has not dispatched through.
    const fs::path vehSource = fs::current_path() / "hook" / "common" / "dxgi_shared_steam_veh.cpp";
    ASSERT_TRUE(fs::exists(vehSource));
    const std::string veh = ce::test_source::ReadFile(vehSource);
    ASSERT_FALSE(veh.empty());
    EXPECT_NE(veh.find("ResolveSteamNullCallbackSlotFromFault(returnAddress"), std::string::npos);
}

// RoboCop session 20260809_143910: the Streamline runtime created the real
// swapchain before CE captured the original game queue, so the create-time
// retention never ran and PostSL startup activation stayed half-armed forever
// ("No startup activation swapchain available ... weakLast=<live>"). The live
// swapchain fallback must fire exactly in that late-handoff window.
TEST(DXGISharedSourceTest, LateHandoffLiveSwapchainFallbackPolicy) {
    using ce::dx12_overlay_policy::ShouldFallbackRetainLiveSwapchainForPostSLStartupActivation;

    // The RoboCop case: no retained swapchain, live swapchain usable, pure-DLSS
    // startup activation pending, no native FSR path, Streamline FG running.
    EXPECT_TRUE(ShouldFallbackRetainLiveSwapchainForPostSLStartupActivation(
        /*retainedSwapchainAvailable=*/false,
        /*liveSwapchainUsable=*/true,
        /*startupActivationPending=*/true,
        /*nativeFSRPresentPathActive=*/false,
        /*fsrApiActive=*/false,
        /*streamlineFGRunning=*/true));

    // Never when a retained swapchain already exists (Talos topology).
    EXPECT_FALSE(ShouldFallbackRetainLiveSwapchainForPostSLStartupActivation(
        true, true, true, false, false, true));
    // Never when the live swapchain is unusable.
    EXPECT_FALSE(ShouldFallbackRetainLiveSwapchainForPostSLStartupActivation(
        false, false, true, false, false, true));
    // Never outside the pending activation window.
    EXPECT_FALSE(ShouldFallbackRetainLiveSwapchainForPostSLStartupActivation(
        false, true, false, false, false, true));
    // Never on a native-FSR present path or FSR API activity.
    EXPECT_FALSE(ShouldFallbackRetainLiveSwapchainForPostSLStartupActivation(
        false, true, true, true, false, true));
    EXPECT_FALSE(ShouldFallbackRetainLiveSwapchainForPostSLStartupActivation(
        false, true, true, false, true, true));
    // Never when Streamline FG is not actually running.
    EXPECT_FALSE(ShouldFallbackRetainLiveSwapchainForPostSLStartupActivation(
        false, true, true, false, false, false));
}

TEST(DXGISharedSourceTest, LateHandoffLiveSwapchainFallbackPrecedesMissingLog) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook_fg_startup.cpp";
    ASSERT_TRUE(fs::exists(source));
    const std::string text = ce::test_source::ReadFile(source);
    ASSERT_FALSE(text.empty());

    const size_t fallback = text.find("ShouldFallbackRetainLiveSwapchainForPostSLStartupActivation(");
    const size_t retain = text.find("DX12_RetainStreamlineStartupActivationSwapchain(liveSwapchain", fallback);
    const size_t missingLog = text.find("No startup activation swapchain available", retain);
    ASSERT_NE(fallback, std::string::npos);
    ASSERT_NE(retain, std::string::npos);
    ASSERT_NE(missingLog, std::string::npos);
    EXPECT_LT(fallback, retain);
    EXPECT_LT(retain, missingLog);
}

// RoboCop session 20260809_144640: NVIDIA Streamline loads its runtime DLLs
// under obfuscated hashed names (1B0_E658703.dll) that contain none of the
// sl.* path tokens, so callerFromStreamlineModule stayed false and the PostSL
// routing never classified the runtime presents as Streamline-originated.
// Recognition must fall back to the Streamline plugin API exports.
TEST(DXGISharedSourceTest, StreamlineModuleRecognitionFallsBackToPluginExports) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "common" / "dxgi_shared_steam.cpp";
    ASSERT_TRUE(fs::exists(source));
    const std::string text = ce::test_source::ReadFile(source);
    ASSERT_FALSE(text.empty());

    const size_t resolver = text.find("bool ResolveIsStreamlineModuleHandle(HMODULE moduleHandle)");
    const size_t nameTokens = text.find("sl.dlss_g", resolver);
    const size_t exportFallback = text.find("GetProcAddress(moduleHandle, \"slGetPluginFunction\")", resolver);
    const size_t featureExport = text.find("GetProcAddress(moduleHandle, \"slGetFeatureFunction\")", resolver);
    const size_t function = text.find("bool IsStreamlineModuleHandle(HMODULE moduleHandle)");
    ASSERT_NE(resolver, std::string::npos);
    ASSERT_NE(nameTokens, std::string::npos);
    ASSERT_NE(exportFallback, std::string::npos);
    ASSERT_NE(featureExport, std::string::npos);
    ASSERT_NE(function, std::string::npos);
    EXPECT_LT(resolver, nameTokens);
    EXPECT_LT(nameTokens, exportFallback);
    EXPECT_LT(exportFallback, featureExport);
    EXPECT_LT(featureExport, function);
}

// The late-handoff fallback must consume the one-shot top-level bootstrap so
// the confirmed-startup settling window is covered by the normal keep-startup
// route; otherwise the overlay starves after the first confirmed frame.
TEST(DXGISharedSourceTest, LateHandoffFallbackConsumesTopLevelBootstrap) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook_fg_startup.cpp";
    ASSERT_TRUE(fs::exists(source));
    const std::string text = ce::test_source::ReadFile(source);
    ASSERT_FALSE(text.empty());

    const size_t retain = text.find("DX12_RetainStreamlineStartupActivationSwapchain(liveSwapchain");
    const size_t consume =
        text.find("streamlineStartupTopLevelPresentConsumed.exchange", retain);
    const size_t reacquire = text.find("AcquireRetainedStreamlineStartupActivationSwapchain()", consume);
    ASSERT_NE(retain, std::string::npos);
    ASSERT_NE(consume, std::string::npos);
    ASSERT_NE(reacquire, std::string::npos);
    EXPECT_LT(retain, consume);
    EXPECT_LT(consume, reacquire);
}

} // namespace
