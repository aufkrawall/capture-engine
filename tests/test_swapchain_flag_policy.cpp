#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "hook/present/swapchain_flag_policy.h"

#include "source_fragment_reader.h"

namespace {

using ce::swapchain_flag_policy::BufferCountAction;
using ce::swapchain_flag_policy::DecideBackbufferCountOverride;
using ce::swapchain_flag_policy::IsFlipSwapEffect;
using ce::swapchain_flag_policy::kCeOwnedCreationFlags;
using ce::swapchain_flag_policy::ReconcileApplicationResizeFlags;
using ce::swapchain_flag_policy::WouldDXGIRejectResizeFlags;

// Strange Brigade (DX12) session `20260921_173511`: a 3840x2160 FLIP_DISCARD
// chain created with ALLOW_TEARING | ALLOW_MODE_SWITCH.
constexpr UINT kStrangeBrigadeFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
constexpr UINT kStrangeBrigadeBufferCount = 3;
constexpr int32_t kConfiguredBackbufferCount = 2;

TEST(SwapchainFlagPolicyTest, WaitableObjectIsWithheldWithoutResizeReconciliation) {
    const auto decision =
        DecideBackbufferCountOverride(kStrangeBrigadeBufferCount, kStrangeBrigadeFlags, DXGI_SWAP_EFFECT_FLIP_DISCARD,
                                      kConfiguredBackbufferCount, /*ceReconcilesApplicationResizeFlags=*/false);

    // The exact regression: CE used to hand DXGI 0x842 here while the game kept
    // remembering 0x802, and the game's own ResizeBuffers then failed
    // E_INVALIDARG before a single frame was presented.
    EXPECT_EQ(decision.flags, kStrangeBrigadeFlags);
    EXPECT_EQ(decision.flags & kCeOwnedCreationFlags, 0u);
    EXPECT_TRUE(decision.waitableObjectWithheld);
    EXPECT_FALSE(decision.waitableObjectRequested);
    // Declining the flag must not turn into a buffer-count change the
    // application did not ask for either.
    EXPECT_EQ(decision.bufferCount, kStrangeBrigadeBufferCount);
    EXPECT_EQ(decision.bufferCountAction, BufferCountAction::PacedInsteadOfShrunk);
}

TEST(SwapchainFlagPolicyTest, WaitableObjectIsAddedWhenResizeReconciliationExists) {
    const auto decision =
        DecideBackbufferCountOverride(kStrangeBrigadeBufferCount, kStrangeBrigadeFlags, DXGI_SWAP_EFFECT_FLIP_DISCARD,
                                      kConfiguredBackbufferCount, /*ceReconcilesApplicationResizeFlags=*/true);

    EXPECT_EQ(decision.flags, kStrangeBrigadeFlags | kCeOwnedCreationFlags);
    EXPECT_TRUE(decision.waitableObjectRequested);
    EXPECT_FALSE(decision.waitableObjectWithheld);
    EXPECT_EQ(decision.bufferCount, kStrangeBrigadeBufferCount);
}

TEST(SwapchainFlagPolicyTest, ApplicationsOwnCreationFlagsAreNeverDisturbed) {
    const auto decision = DecideBackbufferCountOverride(kStrangeBrigadeBufferCount, kStrangeBrigadeFlags,
                                                       DXGI_SWAP_EFFECT_FLIP_DISCARD, kConfiguredBackbufferCount, true);
    EXPECT_EQ(decision.flags & ~kCeOwnedCreationFlags, kStrangeBrigadeFlags);
}

TEST(SwapchainFlagPolicyTest, BlitModelChainsNeverGetTheWaitableObject) {
    for (bool reconciles : {false, true}) {
        const auto decision = DecideBackbufferCountOverride(1, 0, DXGI_SWAP_EFFECT_DISCARD, kConfiguredBackbufferCount,
                                                            reconciles);
        EXPECT_EQ(decision.flags & kCeOwnedCreationFlags, 0u);
        EXPECT_FALSE(decision.waitableObjectRequested);
        EXPECT_FALSE(decision.waitableObjectWithheld);
        // A blit chain has no flip queue, so the depth is BufferCount alone.
        EXPECT_EQ(decision.bufferCount, 2u);
        EXPECT_EQ(decision.bufferCountAction, BufferCountAction::Applied);
    }
}

TEST(SwapchainFlagPolicyTest, ConfiguredDepthAboveTheApplicationRaisesBufferCount) {
    const auto decision = DecideBackbufferCountOverride(2, 0, DXGI_SWAP_EFFECT_FLIP_DISCARD, 4, true);
    EXPECT_EQ(decision.bufferCount, 4u);
    EXPECT_EQ(decision.bufferCountAction, BufferCountAction::Applied);
    EXPECT_EQ(decision.flags & kCeOwnedCreationFlags, kCeOwnedCreationFlags);
}

TEST(SwapchainFlagPolicyTest, NoOverrideLeavesTheDescriptorUntouched) {
    for (int32_t configured : {-1, 0, 1, 7}) {
        const auto decision = DecideBackbufferCountOverride(kStrangeBrigadeBufferCount, kStrangeBrigadeFlags,
                                                            DXGI_SWAP_EFFECT_FLIP_DISCARD, configured, true);
        EXPECT_EQ(decision.bufferCount, kStrangeBrigadeBufferCount) << configured;
        EXPECT_EQ(decision.flags, kStrangeBrigadeFlags) << configured;
        EXPECT_EQ(decision.bufferCountAction, BufferCountAction::None) << configured;
    }
}

TEST(SwapchainFlagPolicyTest, ResizeFlagsGainTheBitTheChainWasCreatedWith) {
    const UINT creationFlags = kStrangeBrigadeFlags | kCeOwnedCreationFlags;
    EXPECT_TRUE(WouldDXGIRejectResizeFlags(kStrangeBrigadeFlags, creationFlags));

    const UINT reconciled = ReconcileApplicationResizeFlags(kStrangeBrigadeFlags, creationFlags);
    EXPECT_EQ(reconciled, creationFlags);
    EXPECT_FALSE(WouldDXGIRejectResizeFlags(reconciled, creationFlags));
}

TEST(SwapchainFlagPolicyTest, ResizeFlagsLoseTheBitTheChainWasNotCreatedWith) {
    // The opposite direction is just as fatal, and it is what a stale
    // "the config says backbuffer_count, so set the bit" rewrite produced on a
    // chain created before the override existed.
    const UINT creationFlags = kStrangeBrigadeFlags;
    const UINT callerFlags = kStrangeBrigadeFlags | kCeOwnedCreationFlags;
    EXPECT_TRUE(WouldDXGIRejectResizeFlags(callerFlags, creationFlags));

    const UINT reconciled = ReconcileApplicationResizeFlags(callerFlags, creationFlags);
    EXPECT_EQ(reconciled, creationFlags);
    EXPECT_FALSE(WouldDXGIRejectResizeFlags(reconciled, creationFlags));
}

TEST(SwapchainFlagPolicyTest, ReconciliationTouchesNoOtherFlagBit) {
    const UINT callerFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_NONPREROTATED |
                             DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
    const UINT creationFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | kCeOwnedCreationFlags;
    const UINT reconciled = ReconcileApplicationResizeFlags(callerFlags, creationFlags);

    EXPECT_EQ(reconciled & ~kCeOwnedCreationFlags, callerFlags);
    EXPECT_EQ(reconciled & kCeOwnedCreationFlags, kCeOwnedCreationFlags);
}

TEST(SwapchainFlagPolicyTest, ReconciliationIsIdempotent) {
    const UINT creationFlags = kStrangeBrigadeFlags | kCeOwnedCreationFlags;
    const UINT once = ReconcileApplicationResizeFlags(kStrangeBrigadeFlags, creationFlags);
    EXPECT_EQ(ReconcileApplicationResizeFlags(once, creationFlags), once);
}

TEST(SwapchainFlagPolicyTest, CeOwnsOnlyTheFrameLatencyWaitableBit) {
    // Guards the blast radius of the rewrite: widening this constant would let
    // CE start silently editing flags that belong to the application.
    EXPECT_EQ(kCeOwnedCreationFlags, static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT));
    EXPECT_EQ(kCeOwnedCreationFlags, 0x40u);
}

TEST(SwapchainFlagPolicyTest, FlipSwapEffectClassification) {
    EXPECT_TRUE(IsFlipSwapEffect(DXGI_SWAP_EFFECT_FLIP_DISCARD));
    EXPECT_TRUE(IsFlipSwapEffect(DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL));
    EXPECT_FALSE(IsFlipSwapEffect(DXGI_SWAP_EFFECT_DISCARD));
    EXPECT_FALSE(IsFlipSwapEffect(DXGI_SWAP_EFFECT_SEQUENTIAL));
}


std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

// The bug was a duplicated rule: six creation paths each added the waitable
// object on their own, so the one that mattered kept doing it after the
// reconciliation that hides it again had stopped existing. Keep the write in a
// single place.
TEST(SwapchainFlagPolicySourceTest, OnlyThePolicyWritesTheWaitableCreationFlag) {
    const char* creationSources[] = {
        "hook/d3d12/dx12_hook_swapchain_create.cpp", "hook/d3d12/dx12_hook_swapchain_tracking.cpp",
        "hook/wrappers/dxgi_factory_wrap.cpp",      "hook/wrappers/wrapper_hooks.cpp",
        "hook/wrappers/dxgi_swapchain_wrap_modern.cpp",
    };
    for (const char* relativePath : creationSources) {
        const std::string source = ReadSource(relativePath);
        ASSERT_FALSE(source.empty()) << relativePath;
        EXPECT_EQ(source.find("Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT"), std::string::npos)
            << relativePath << " must request the waitable object through ce::swapchain_flag_policy";
    }
}

TEST(SwapchainFlagPolicySourceTest, CreationPathsGoThroughTheSharedPolicy) {
    const char* creationSources[] = {
        "hook/d3d12/dx12_hook_swapchain_create.cpp",
        "hook/d3d12/dx12_hook_swapchain_tracking.cpp",
        "hook/wrappers/dxgi_factory_wrap.cpp",
        "hook/wrappers/wrapper_hooks.cpp",
    };
    for (const char* relativePath : creationSources) {
        const std::string source = ReadSource(relativePath);
        ASSERT_FALSE(source.empty()) << relativePath;
        EXPECT_NE(source.find("ApplyBackbufferCountOverrideToDesc"), std::string::npos) << relativePath;
    }
}

// The flag is only safe to add because the application's own resize calls are
// rewritten to match. Every path that can see such a call must do that, and it
// must read the live descriptor rather than re-derive intent from the config.
TEST(SwapchainFlagPolicySourceTest, EveryResizePathReconcilesAgainstTheLiveDescriptor) {
    const char* resizeSources[] = {
        "hook/present/dxgi_shared_resize.cpp",
        "hook/wrappers/dxgi_swapchain_wrap_modern.cpp",
        "hook/d3d11/dx11_hook_present.cpp",
    };
    for (const char* relativePath : resizeSources) {
        const std::string source = ReadSource(relativePath);
        ASSERT_FALSE(source.empty()) << relativePath;
        EXPECT_NE(source.find("ReconcileApplicationResizeFlags"), std::string::npos) << relativePath;
        EXPECT_NE(source.find("GetDesc("), std::string::npos) << relativePath;
    }
}

// Session `20260921_173511`: the compensation lived only in paths CE had
// declined to install, so the claim has to happen where the swapchain is handed
// to the application untouched, and before the Present ownership question.
TEST(SwapchainFlagPolicySourceTest, ResizeClaimIsIndependentOfPresentOwnership) {
    const std::string hooks = ReadSource("hook/present/dxgi_shared_hooks.cpp");
    ASSERT_FALSE(hooks.empty());

    const size_t claim = hooks.find("InstallResizeReconciliationHooks(pSwapChain, \"InstallHooks\")");
    const size_t foreignChainReturn = hooks.find("Multi-overlay foreign Present chain owns the entry");
    ASSERT_NE(claim, std::string::npos);
    ASSERT_NE(foreignChainReturn, std::string::npos);
    EXPECT_LT(claim, foreignChainReturn);

    const std::string create = ReadSource("hook/d3d12/dx12_hook_swapchain_create.cpp");
    ASSERT_FALSE(create.empty());
    EXPECT_NE(create.find("InstallResizeReconciliationHooks"), std::string::npos);

    const std::string wrap = ReadSource("hook/wrappers/dxgi_factory_wrap.cpp");
    ASSERT_FALSE(wrap.empty());
    EXPECT_NE(wrap.find("InstallResizeReconciliationHooks"), std::string::npos);
}

// DXGI reads BufferCount == 0 as "keep the existing count"; substituting the
// configured depth there silently reallocates a chain the application meant to
// leave alone.
TEST(SwapchainFlagPolicySourceTest, ResizePathsPreserveAnImplicitBufferCount) {
    const std::string shared = ReadSource("hook/present/dxgi_shared_resize.cpp");
    ASSERT_FALSE(shared.empty());
    EXPECT_NE(shared.find("if (BufferCount == 0)"), std::string::npos);

    const std::string wrapper = ReadSource("hook/wrappers/dxgi_swapchain_wrap_modern.cpp");
    ASSERT_FALSE(wrapper.empty());
    EXPECT_NE(wrapper.find("if (BufferCount == 0)"), std::string::npos);
}

// The Steam overlay hooks a swapchain by patching the entry of each function its
// vtable points to, and skips a function whose entry already jumps into another
// module ("points to another module, skipping hooks"). It holds one reference on
// each back buffer it draws on and returns them only in its ResizeBuffers hook, so
// a CE detour in slot 13 (logs/20260926_192858) and a CE jump at ResizeBuffers'
// entry (logs/20260927_031545: Present/Present1 jumped into Steam's relay page,
// ResizeBuffers into capture_hook) both cost Steam its resize handling, and every
// resize of a chain it drew on failed with DXGI_ERROR_INVALID_CALL. The
// reconciliation hooks below the entry, like Present, and never takes the entry
// while a third-party overlay is loaded.
TEST(SwapchainFlagPolicySourceTest, ResizeReconciliationLeavesSlotsAndEntriesToSlotHookingOverlays) {
    const std::string hooks = ReadSource("hook/present/dxgi_shared_hooks.cpp");
    const size_t begin = hooks.find("bool InstallResizeReconciliationHooks(IDXGISwapChain* pSwapChain");
    ASSERT_NE(begin, std::string::npos);
    const std::string body = hooks.substr(begin, hooks.find("\n}\n", begin) - begin);
    EXPECT_EQ(body.find("ClaimSwapchainVTableSlot("), std::string::npos)
        << "a CE detour in the slot hides ResizeBuffers from Steam's hook";
    EXPECT_EQ(body.find("VirtualProtect("), std::string::npos) << "the slots are only read";

    const size_t deep = body.find("InlineHook::InstallDeepHookPublished(target, detour, publish, nullptr,");
    const size_t fallback = body.find("ChooseSiteAfterBodyHookRefused(overlayLoaded)");
    const size_t entry = body.find("fallback == Site::kEntry && InlineHook::InstallPublished(target");
    ASSERT_NE(deep, std::string::npos);
    ASSERT_NE(fallback, std::string::npos);
    ASSERT_NE(entry, std::string::npos);
    EXPECT_LT(deep, fallback) << "the body below the entry is always tried first";
    EXPECT_LT(fallback, entry) << "the entry is only a fallback the policy allows";
    EXPECT_EQ(body.find("InlineHook::InstallPublished(resizeTarget"), std::string::npos);
    EXPECT_EQ(body.find("InlineHook::InstallPublished(resize1Target"), std::string::npos);

    // Only dxgi's own function is patched; a slot another component already
    // owns is left alone and reconciliation reports itself unavailable.
    const size_t guard = body.find("if (!inDxgi(resizeTarget))");
    ASSERT_NE(guard, std::string::npos);
    EXPECT_LT(guard, deep);
    // Both methods must hide the flag before CE may add it.
    EXPECT_NE(body.find("(!hasResize1 || resize1Site != ce::resize_reconcile_hook::Site::kNone)"),
              std::string::npos);
}

// A "call the original through the vtable" shortcut in a resize detour would
// re-enter the chain it is part of; the detours forward to their saved
// predecessors only.
TEST(SwapchainFlagPolicySourceTest, ResizeDetoursNeverReenterTheirOwnVTableSlot) {
    const std::string shared = ReadSource("hook/present/dxgi_shared_resize.cpp");
    ASSERT_FALSE(shared.empty());
    // The cast form is the call itself; the slot numbers still appear in the
    // comments that explain why the call must not be made that way.
    EXPECT_EQ(shared.find(")vtable[13]"), std::string::npos);
    EXPECT_EQ(shared.find(")vtable[39]"), std::string::npos);
}

// The device the game presents through is discovered from its command queue,
// which does not depend on CE having seen D3D12CreateDevice. That is the only
// discovery that survives an injection later than the game's graphics init.
TEST(SwapchainFlagPolicySourceTest, SamplerOverridesHookEveryDiscoveredD3D12Device) {
    const std::string helpers = ReadSource("hook/d3d12/dx12_hook_helpers.cpp");
    ASSERT_FALSE(helpers.empty());

    const size_t publish = helpers.find("void DX12_PublishNativeLimiterDevice(");
    ASSERT_NE(publish, std::string::npos);
    EXPECT_NE(helpers.find("DX12_HookDeviceVTable(device)", publish), std::string::npos);
    EXPECT_NE(helpers.find("MarkD3D12DeviceCreated()", publish), std::string::npos);
}

}  // namespace
