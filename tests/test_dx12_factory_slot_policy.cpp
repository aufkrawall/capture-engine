#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "hook/d3d12/dx12_factory_slot_policy.h"

#include "source_fragment_reader.h"

namespace {

using ce::dx12_factory_slot::HasForeignEntryJump;
using ce::dx12_factory_slot::ShouldInvokeSavedCreateSwapChainForHwndSlot;

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

TEST(Dx12FactorySlotPolicyTest, SavedSlotMayOnlyRunOnObjectsOfItsOwnVtable) {
    void* savedVtable = reinterpret_cast<void*>(0x1000);
    void* foreignVtable = reinterpret_cast<void*>(0x2000);
    void* factoryObject[1] = {savedVtable};

    EXPECT_TRUE(ShouldInvokeSavedCreateSwapChainForHwndSlot(savedVtable, static_cast<const void*>(factoryObject)));
    factoryObject[0] = foreignVtable;
    EXPECT_FALSE(ShouldInvokeSavedCreateSwapChainForHwndSlot(savedVtable, static_cast<const void*>(factoryObject)));
    EXPECT_FALSE(ShouldInvokeSavedCreateSwapChainForHwndSlot(nullptr, static_cast<const void*>(factoryObject)));
    EXPECT_FALSE(ShouldInvokeSavedCreateSwapChainForHwndSlot(savedVtable, nullptr));
    EXPECT_FALSE(ShouldInvokeSavedCreateSwapChainForHwndSlot(nullptr, nullptr));
}

TEST(Dx12FactorySlotPolicyTest, ForeignEntryJumpShapesAreRecognizedOnlyAtEntry) {
    const uint8_t relativeJump[] = {0xE9, 0x00, 0x00, 0x00, 0x00};
    const uint8_t indirectJump[] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    const uint8_t plainProlog[] = {0x48, 0x83, 0xEC, 0x28, 0xE8};

    EXPECT_TRUE(HasForeignEntryJump(relativeJump));
    EXPECT_TRUE(HasForeignEntryJump(indirectJump));
    EXPECT_FALSE(HasForeignEntryJump(plainProlog));
    EXPECT_FALSE(HasForeignEntryJump(nullptr));
}

TEST(Dx12FactorySlotPolicyTest, HookInstallerCapturesSavedSlotVtableWithTheSlotValue) {
    const std::string source = ReadSource("hook/d3d12/dx12_hook_hook_install.cpp");
    ASSERT_FALSE(source.empty());

    const size_t slotSave = source.find("dx12_hook_s_realCreateSCForHwndAddr = realCreateSCForHwndAddr;");
    const size_t vtableSave = source.find("dx12_hook_s_savedCreateSwapChainForHwndVtable = vtable;");
    const size_t slotPatch = source.find("Hooked global CreateSwapChainForHwnd at vtable[15]");
    ASSERT_NE(slotSave, std::string::npos);
    ASSERT_NE(vtableSave, std::string::npos);
    ASSERT_NE(slotPatch, std::string::npos);
    EXPECT_LT(slotSave, vtableSave);
    EXPECT_LT(vtableSave, slotPatch);
}

TEST(Dx12FactorySlotPolicyTest, TempSwapchainBypassesFactoryExportPatchAndGuardsTheRawSlotCall) {
    const std::string source = ReadSource("hook/d3d12/dx12_hook_hook_install.cpp");
    ASSERT_FALSE(source.empty());

    const size_t exportBypass = source.find(
        "Bypassing foreign entry patch on CreateDXGIFactory1 at %p");
    const size_t factoryCreate =
        source.find("pCreateFactory(IID_PPV_ARGS(&pFactory))", exportBypass);
    const size_t vtableGuard = source.find("ShouldInvokeSavedCreateSwapChainForHwndSlot(", factoryCreate);
    const size_t rawCall = source.find(
        "dx12_hook_oCreateSwapChainForHwndGlobal(pFactory, pQueue, hwnd");
    ASSERT_NE(exportBypass, std::string::npos);
    ASSERT_NE(factoryCreate, std::string::npos);
    ASSERT_NE(vtableGuard, std::string::npos);
    ASSERT_NE(rawCall, std::string::npos);
    EXPECT_LT(exportBypass, factoryCreate);
    EXPECT_LT(factoryCreate, vtableGuard);
    EXPECT_LT(vtableGuard, rawCall);
}

// Session 20261001_042335: Steam's overlay patched the CreateSwapChainForHwnd entry while CE was
// installing 0.5 s after process start. CE prepended over it and kept Steam's jump as its
// original; Steam abandoned that hook (its saved original for that relay stub is null while the
// neighbours are set) and the first real create ran CE -> Steam's handler -> 0x0.
TEST(Dx12FactorySlotPolicyTest, ForeignEntryPatchIsLeftIntactWhenCeHoldsTheBelowChainView) {
    using ce::dx12_factory_slot::ShouldPrependCreateSwapChainForHwndEntry;
    EXPECT_FALSE(ShouldPrependCreateSwapChainForHwndEntry(/*foreign=*/true, /*belowChain=*/true));
    // No below-chain view: the prepend is the only coverage left, so it stays the fallback.
    EXPECT_TRUE(ShouldPrependCreateSwapChainForHwndEntry(/*foreign=*/true, /*belowChain=*/false));
    // CE first: the entry is CE's to patch, as before.
    EXPECT_TRUE(ShouldPrependCreateSwapChainForHwndEntry(/*foreign=*/false, /*belowChain=*/false));
    EXPECT_TRUE(ShouldPrependCreateSwapChainForHwndEntry(/*foreign=*/false, /*belowChain=*/true));
}

TEST(Dx12FactorySlotPolicyTest, BelowChainHookCarriesTheEntryHandlingOnlyForCeForwardedCreates) {
    using ce::dx12_factory_slot::ShouldBelowChainHookRunEntrySemantics;
    EXPECT_TRUE(ShouldBelowChainHookRunEntrySemantics(/*forwarded=*/true, /*accessDeniedRetry=*/false,
                                                      /*entryPrepend=*/false));
    // CE owns the entry: its entry detour already handled the call.
    EXPECT_FALSE(ShouldBelowChainHookRunEntrySemantics(true, false, /*entryPrepend=*/true));
    // The nested access-denied retry must stay a plain pass-through.
    EXPECT_FALSE(ShouldBelowChainHookRunEntrySemantics(true, /*accessDeniedRetry=*/true, false));
    // A create that did not come through CE's vtable detour keeps the deep hook's own handling.
    EXPECT_FALSE(ShouldBelowChainHookRunEntrySemantics(/*forwarded=*/false, false, false));
}

TEST(Dx12FactorySlotPolicyTest, InstallerSamplesTheEntryBeforePatchingAndPlacesTheDeepHookFirst) {
    const std::string source = ReadSource("hook/d3d12/dx12_hook_hook_install.cpp");
    ASSERT_FALSE(source.empty());
    const size_t sample = source.find("ce::dx12_factory_slot::HasForeignEntryJump(realCreateSCForHwndAddr)");
    const size_t deep = source.find("(void*)DeepHookCreateSwapChainForHwnd,");
    const size_t decide = source.find("ShouldPrependCreateSwapChainForHwndEntry(createSCForHwndEntryForeignOwned");
    const size_t prepend = source.find("InlineHook::InstallPublished(realCreateSCForHwndAddr");
    ASSERT_NE(sample, std::string::npos);
    ASSERT_NE(deep, std::string::npos);
    ASSERT_NE(decide, std::string::npos);
    ASSERT_NE(prepend, std::string::npos);
    EXPECT_LT(sample, deep);
    EXPECT_LT(deep, decide);
    EXPECT_LT(decide, prepend);

    const std::string deepHook = ReadSource("hook/d3d12/dx12_hook_swapchain_tracking.cpp");
    ASSERT_FALSE(deepHook.empty());
    EXPECT_NE(deepHook.find("return RunCreateSwapChainForHwndEntrySemantics(dx12_hook_s_deepHookTrampoline"),
              std::string::npos);
}

// 20261001_044939: Steam was loaded but had not patched the entry yet when CE installed; CE's
// prepend made Steam skip the function and its overlay never drew. 20261001_045157: Steam won the
// race and worked. A loaded overlay must own the entry regardless of who patches first.
TEST(Dx12FactorySlotPolicyTest, LoadedOverlayOwnsTheEntryBeforeItHasPatchedIt) {
    using ce::dx12_factory_slot::CreateSwapChainForHwndBelowChainPatchSpan;
    using ce::dx12_factory_slot::IsCreateSwapChainForHwndEntryForeignOwned;
    using ce::dx12_factory_slot::ShouldPrependCreateSwapChainForHwndEntry;
    EXPECT_TRUE(IsCreateSwapChainForHwndEntryForeignOwned(/*visible=*/false, /*loadedOverlays=*/1));
    EXPECT_TRUE(IsCreateSwapChainForHwndEntryForeignOwned(true, 0));
    EXPECT_FALSE(IsCreateSwapChainForHwndEntryForeignOwned(false, 0));

    // Unpatched owner: the body hook clears the widest entry form; a visible patch measures itself.
    EXPECT_EQ(CreateSwapChainForHwndBelowChainPatchSpan(false, 1), 14);
    EXPECT_EQ(CreateSwapChainForHwndBelowChainPatchSpan(true, 1), 0);
    EXPECT_EQ(CreateSwapChainForHwndBelowChainPatchSpan(false, 0), 0);

    // The 044939 state: overlay loaded, entry clean, body hook placed -> no prepend.
    EXPECT_FALSE(ShouldPrependCreateSwapChainForHwndEntry(IsCreateSwapChainForHwndEntryForeignOwned(false, 1),
                                                          /*belowChain=*/true));
    // No overlay at all: CE owns the entry as before.
    EXPECT_TRUE(ShouldPrependCreateSwapChainForHwndEntry(IsCreateSwapChainForHwndEntryForeignOwned(false, 0),
                                                         /*belowChain=*/false));
}

TEST(Dx12FactorySlotPolicyTest, InstallerPassesTheOwnersSpanToTheBelowChainHook) {
    const std::string source = ReadSource("hook/d3d12/dx12_hook_hook_install.cpp");
    ASSERT_FALSE(source.empty());
    const size_t owned = source.find("IsCreateSwapChainForHwndEntryForeignOwned(foreignCreateSCForHwndEntry");
    const size_t deep = source.find("(void*)DeepHookCreateSwapChainForHwnd,");
    const size_t span = source.find("CreateSwapChainForHwndBelowChainPatchSpan(foreignCreateSCForHwndEntry", deep);
    const size_t decide = source.find("ShouldPrependCreateSwapChainForHwndEntry(createSCForHwndEntryForeignOwned");
    ASSERT_NE(owned, std::string::npos);
    ASSERT_NE(deep, std::string::npos);
    ASSERT_NE(span, std::string::npos);
    ASSERT_NE(decide, std::string::npos);
    EXPECT_LT(owned, deep);
    EXPECT_LT(span, decide);
}

// 20261001_045954 (0.1.6877): with the CSFH entry already left to Steam, Steam's own log still read
// `DXGIFactory2_CreateSwapChain points to another module, skipping hooks` for every factory - it
// decides from the vtable SLOTS, which held CE's detours. A loaded overlay owns the slots too.
TEST(Dx12FactorySlotPolicyTest, LoadedOverlayOwnsTheFactorySlotsWhileABodyHookCoversTheCreate) {
    using ce::dx12_factory_slot::ShouldHookFactoryCreateSwapchainSlot;
    EXPECT_FALSE(ShouldHookFactoryCreateSwapchainSlot(/*loadedOverlays=*/1, /*belowChain=*/true));
    // No body hook: the slot detour is CE's only create view, so it stays the fallback.
    EXPECT_TRUE(ShouldHookFactoryCreateSwapchainSlot(1, /*belowChain=*/false));
    // No overlay: unchanged, CE takes the slot.
    EXPECT_TRUE(ShouldHookFactoryCreateSwapchainSlot(0, false));
    EXPECT_TRUE(ShouldHookFactoryCreateSwapchainSlot(0, true));
}

TEST(Dx12FactorySlotPolicyTest, BodyHookRunsEntryHandlingForEveryTopLevelCreateWhenTheSlotIsTheOverlays) {
    using ce::dx12_factory_slot::ShouldBelowChainHookRunEntrySemantics;
    EXPECT_TRUE(ShouldBelowChainHookRunEntrySemantics(/*forwarded=*/false, false, false, /*slotLeft=*/true));
    EXPECT_FALSE(ShouldBelowChainHookRunEntrySemantics(false, /*accessDeniedRetry=*/true, false, true));
    EXPECT_FALSE(ShouldBelowChainHookRunEntrySemantics(false, false, /*entryPrepend=*/true, true));
    EXPECT_FALSE(ShouldBelowChainHookRunEntrySemantics(false, false, false, /*slotLeft=*/false));
}

TEST(Dx12FactorySlotPolicyTest, BelowChainOriginatorSkipsCeTheOverlayChainDxgiAndRelayPages) {
    using ce::dx12_factory_slot::CreateSwapchainStackFrameKind;
    using ce::dx12_factory_slot::SelectCreateSwapchainOriginatorFrame;
    using Kind = CreateSwapchainStackFrameKind;
    // CE wrapper -> Steam handler -> Steam relay page -> sl.dlss_g (the real caller) -> game
    const Kind steamForwarded[] = {Kind::kCaptureEngine, Kind::kThirdPartyOverlay, Kind::kNoImage, Kind::kOther,
                                   Kind::kOther};
    EXPECT_EQ(SelectCreateSwapchainOriginatorFrame(steamForwarded, 5), 3);
    const Kind viaDxgi[] = {Kind::kCaptureEngine, Kind::kSystemDxgi, Kind::kOther};
    EXPECT_EQ(SelectCreateSwapchainOriginatorFrame(viaDxgi, 3), 2);
    const Kind nothing[] = {Kind::kCaptureEngine, Kind::kThirdPartyOverlay};
    EXPECT_EQ(SelectCreateSwapchainOriginatorFrame(nothing, 2), -1);
    EXPECT_EQ(SelectCreateSwapchainOriginatorFrame(nullptr, 3), -1);
}

TEST(Dx12FactorySlotPolicyTest, SlotsAreDecidedAfterTheBodyHooksAndLeftSlotsAreNeverRestored) {
    const std::string install = ReadSource("hook/d3d12/dx12_hook_hook_install.cpp");
    ASSERT_FALSE(install.empty());
    const size_t body = install.find("void InstallGlobalVTableHooks() {");
    ASSERT_NE(body, std::string::npos);
    const size_t deepHwnd = install.find("(void*)DeepHookCreateSwapChainForHwnd,", body);
    const size_t deepCreate = install.find("InstallCreateSwapChainBelowChainHook(vtable[10]");
    const size_t decide10 = install.find("ShouldHookFactoryCreateSwapchainSlot(loadedOverlayCount, createSCBelowChain");
    const size_t decide15 = install.find("ShouldHookFactoryCreateSwapchainSlot(loadedOverlayCount, belowChainHook");
    const size_t patch10 = install.find("(LPVOID)DetourCreateSwapChainGlobal,", body);
    const size_t patch15 = install.find("(LPVOID)DetourCreateSwapChainForHwndGlobal,", body);
    ASSERT_NE(deepHwnd, std::string::npos);
    ASSERT_NE(deepCreate, std::string::npos);
    ASSERT_NE(decide10, std::string::npos);
    ASSERT_NE(decide15, std::string::npos);
    ASSERT_NE(patch10, std::string::npos);
    ASSERT_NE(patch15, std::string::npos);
    EXPECT_LT(deepHwnd, decide15);
    EXPECT_LT(deepCreate, decide10);
    EXPECT_LT(decide10, patch10);
    EXPECT_LT(decide15, patch15);
    // A version-specific vtable (IDXGIFactory4/6) gets the same per-slot decision.
    EXPECT_NE(install.find("if (hookCreateSCForHwndSlot) {"), std::string::npos);

    const std::string removal = ReadSource("hook/d3d12/dx12_hook_swapchain.cpp");
    EXPECT_NE(removal.find("dx12_hook_s_createSCSlotHooked.exchange(false)"), std::string::npos);
    EXPECT_NE(removal.find("dx12_hook_s_createSCForHwndSlotHooked.exchange(false)"), std::string::npos);
    EXPECT_NE(removal.find("RemoveCreateSwapChainBelowChainHook()"), std::string::npos);
}

TEST(Dx12FactorySlotPolicyTest, BelowChainCreatesResolveTheOriginatorFromTheStack) {
    const std::string tracking = ReadSource("hook/d3d12/dx12_hook_swapchain_tracking.cpp");
    const size_t decide = tracking.find("ShouldBelowChainHookRunEntrySemantics(");
    const size_t scope = tracking.find("ScopedBelowForeignChainSwapchainCreate belowChain;", decide);
    const size_t run = tracking.find("RunCreateSwapChainForHwndEntrySemantics(dx12_hook_s_deepHookTrampoline", decide);
    ASSERT_NE(decide, std::string::npos);
    ASSERT_NE(scope, std::string::npos);
    ASSERT_NE(run, std::string::npos);
    EXPECT_LT(scope, run);
    EXPECT_NE(tracking.find("slotLeftToOverlay)", decide), std::string::npos);

    const std::string belowChain = ReadSource("hook/d3d12/dx12_hook_swapchain_create_below_chain.cpp");
    const size_t resolver =
        belowChain.find("CreateSwapchainForHwndCallerContext ResolveCreateSwapchainForHwndCallerContext() {");
    ASSERT_NE(resolver, std::string::npos);
    EXPECT_NE(belowChain.find("DX12_IsSwapchainCreateBelowForeignChain()", resolver), std::string::npos);
    // The legacy CreateSwapChain body hook runs the slot detour's handling with a stack-derived caller.
    const size_t deep = belowChain.find("HRESULT STDMETHODCALLTYPE DeepHookCreateSwapChain(");
    ASSERT_NE(deep, std::string::npos);
    const size_t caller = belowChain.find("ResolveCreateSwapchainCallerBelowForeignChain();", deep);
    const size_t semantics = belowChain.find("RunCreateSwapChainGlobalSemantics(original, caller", deep);
    ASSERT_NE(caller, std::string::npos);
    ASSERT_NE(semantics, std::string::npos);
    EXPECT_LT(caller, semantics);
    // Never an overlay by inference below the chain.
    EXPECT_NE(belowChain.find("context.callerFromThirdPartyOverlay = false;"), std::string::npos);
}

TEST(Dx12FactorySlotPolicyTest, FactoryVtableDiscoveryNeverEntersAnOverlayFactoryHandler) {
    // 20261001_044010: CE's discovery factory ran Steam's CreateDXGIFactory1 handler on CE's hook
    // thread while the game initialized; Steam hooked CreateSwapChainForHwnd twice and the game's
    // first swapchain create recursed in Steam's handler until the stack overflowed.
    const std::string source = ReadSource("hook/d3d12/dx12_hook_hook_install.cpp");
    ASSERT_FALSE(source.empty());
    const size_t install = source.find("void InstallGlobalVTableHooks() {");
    const size_t genuine = source.find("GenuineCreateDXGIFactory1ForDiscovery(", install);
    const size_t firstCreate = source.find("pCreateFactory(IID_PPV_ARGS(&pFactory))", install);
    ASSERT_NE(install, std::string::npos);
    ASSERT_NE(genuine, std::string::npos);
    ASSERT_NE(firstCreate, std::string::npos);
    EXPECT_LT(genuine, firstCreate);
    const size_t helper = source.find("PFN_CreateDXGIFactory1Export GenuineCreateDXGIFactory1ForDiscovery(");
    ASSERT_NE(helper, std::string::npos);
    EXPECT_NE(source.find("InlineHook::CreateBypassTrampoline(reinterpret_cast<void*>(exported))", helper),
              std::string::npos);
}

}  // namespace
