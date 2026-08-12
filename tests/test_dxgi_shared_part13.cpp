#include "test_dxgi_shared_shared.h"

#include "../hook/common/dxgi_shared_internal.h"

namespace {

// The DXGI swapchain class vftable lives in read-only image data
// (dxgi.dll .rdata). CE makes it writable only transiently inside
// VirtualProtect regions, so every observation of a slot must be a plain
// volatile read - a `lock cmpxchg` used as a read faults on the read-only
// page (regression: 20260811_192706, all three dumps crash in
// RepairVTableHooksIfNeeded::<lambda0> at the first CAS).

HRESULT STDMETHODCALLTYPE DummyPresent(IDXGISwapChain*, UINT, UINT) {
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DummyPresent1(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*) {
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DummyResizeBuffers(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT) {
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DummyResizeBuffers1(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*,
                                              IUnknown* const*) {
    return S_OK;
}

void** AllocateWritableVTablePage() {
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const size_t pageSize = systemInfo.dwPageSize;
    void* page = VirtualAlloc(nullptr, pageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return page ? static_cast<void**>(page) : nullptr;
}

bool MakePageReadOnly(void* page) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(page, &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    DWORD oldProtect = 0;
    return VirtualProtect(mbi.BaseAddress, mbi.RegionSize, PAGE_READONLY, &oldProtect) != 0;
}

void AssertPageStillReadOnly(const void* page) {
    MEMORY_BASIC_INFORMATION mbi{};
    ASSERT_NE(VirtualQuery(page, &mbi, sizeof(mbi)), 0u);
    EXPECT_EQ(mbi.Protect & 0xFF, PAGE_READONLY);
}

struct ScopedVTableStateGuard {
    ~ScopedVTableStateGuard() {
        DXGIShared::dxgi_shared_s_hookedVTable = nullptr;
        DXGIShared::dxgi_shared_oPresent = nullptr;
        DXGIShared::dxgi_shared_oPresent1 = nullptr;
        DXGIShared::dxgi_shared_oResizeBuffers = nullptr;
        DXGIShared::dxgi_shared_oResizeBuffers1 = nullptr;
    }
};

void ReleaseVTablePage(void* vtable) {
    MEMORY_BASIC_INFORMATION cleanupInfo{};
    if (VirtualQuery(vtable, &cleanupInfo, sizeof(cleanupInfo)) != 0) {
        DWORD oldProtect = 0;
        VirtualProtect(cleanupInfo.BaseAddress, cleanupInfo.RegionSize, PAGE_READWRITE, &oldProtect);
    }
    VirtualFree(vtable, 0, MEM_RELEASE);
}

}  // namespace

TEST(DXGISharedVTableRepairTest, RepairReclaimsRestoredSlotsOnReadOnlyClassVftable) {
    void** vtable = AllocateWritableVTablePage();
    ASSERT_NE(vtable, nullptr);

    // Fill the slots while the page is writable (production installs hooks
    // inside a VirtualProtect region, then restores the read-only image
    // protection), and only then lock the page down like the real class
    // vftable in dxgi.dll.
    vtable[8] = (void*)DummyPresent;
    vtable[22] = (void*)DummyPresent1;
    ASSERT_TRUE(MakePageReadOnly(static_cast<void*>(vtable)));

    DXGIShared::dxgi_shared_s_hookedVTable = vtable;
    DXGIShared::dxgi_shared_oPresent = &DummyPresent;
    DXGIShared::dxgi_shared_oPresent1 = &DummyPresent1;
    ScopedVTableStateGuard stateGuard;

    // Pre-fix this crashed with an access violation: the first slot
    // observation used InterlockedCompareExchangePointer, i.e. `lock
    // cmpxchg`, which requires write access on the read-only vtable page.
    EXPECT_NO_FATAL_FAILURE(DXGIShared::RepairVTableHooksIfNeeded());

    EXPECT_EQ(vtable[8], (void*)DXGIShared::DetourPresent);
    EXPECT_EQ(vtable[22], (void*)DXGIShared::DetourPresent1);
    AssertPageStillReadOnly(static_cast<const void*>(vtable));

    ReleaseVTablePage(static_cast<void*>(vtable));
}

TEST(DXGISharedVTableRepairTest, DetachRestoresOwnedSlotsOnReadOnlyClassVftable) {
    void** vtable = AllocateWritableVTablePage();
    ASSERT_NE(vtable, nullptr);

    vtable[8] = (void*)DXGIShared::DetourPresent;
    vtable[22] = (void*)DXGIShared::DetourPresent1;
    vtable[13] = (void*)DXGIShared::DetourResizeBuffers;
    vtable[39] = (void*)DXGIShared::DetourResizeBuffers1;
    ASSERT_TRUE(MakePageReadOnly(static_cast<void*>(vtable)));

    DXGIShared::dxgi_shared_s_hookedVTable = vtable;
    DXGIShared::dxgi_shared_oPresent = &DummyPresent;
    DXGIShared::dxgi_shared_oPresent1 = &DummyPresent1;
    DXGIShared::dxgi_shared_oResizeBuffers = &DummyResizeBuffers;
    DXGIShared::dxgi_shared_oResizeBuffers1 = &DummyResizeBuffers1;
    ScopedVTableStateGuard stateGuard;

    // Same read-only class-vftable constraint: DetachOwnedVTableSlot must not
    // run a locked operation on the page before VirtualProtect.
    EXPECT_NO_FATAL_FAILURE(DXGIShared::RemoveSwapchainVTableHooks());

    EXPECT_EQ(vtable[8], (void*)DummyPresent);
    EXPECT_EQ(vtable[22], (void*)DummyPresent1);
    EXPECT_EQ(vtable[13], (void*)DummyResizeBuffers);
    EXPECT_EQ(vtable[39], (void*)DummyResizeBuffers1);
    EXPECT_EQ(DXGIShared::dxgi_shared_s_hookedVTable, nullptr);
    AssertPageStillReadOnly(static_cast<const void*>(vtable));

    ReleaseVTablePage(static_cast<void*>(vtable));
}
namespace {

// Regression 20260811_195131: CE's inline-hook trampoline re-issues the
// foreign entry jump when it was prepended over an external overlay's
// E9/FF25. The trampoline chain detector must recognize both jump forms and
// only accept chains that match the preserved external hook target (or, in
// generic mode, targets outside dxgi.dll).

void WriteFF25Jump(void* page, void* target) {
    auto* bytes = static_cast<uint8_t*>(page);
    bytes[0] = 0xFF;
    bytes[1] = 0x25;
    bytes[2] = 0x00;
    bytes[3] = 0x00;
    bytes[4] = 0x00;
    bytes[5] = 0x00;
    memcpy(bytes + 6, static_cast<const void*>(&target), sizeof(target));
}

void WriteE9Jump(void* page, void* target) {
    const int64_t displacement =
        reinterpret_cast<int64_t>(target) - (reinterpret_cast<int64_t>(page) + 5);
    ASSERT_GE(displacement, INT32_MIN);
    ASSERT_LE(displacement, INT32_MAX);
    auto* bytes = static_cast<uint8_t*>(page);
    bytes[0] = 0xE9;
    const int32_t displacement32 = static_cast<int32_t>(displacement);
    memcpy(bytes + 1, &displacement32, sizeof(displacement32));
}

}  // namespace

TEST(DXGISharedSteamTrampolineChainTest, FF25TrampolineMatchesPreservedExternalHook) {
    void* page = static_cast<void*>(AllocateWritableVTablePage());
    ASSERT_NE(page, nullptr);
    // The resolved jump target itself is only compared (install-time rule),
    // so a nearby unmapped address inside the same allocation works.
    const auto target = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(page) + 0x200);
    WriteFF25Jump(page, target);

    EXPECT_TRUE(DXGIShared::TrampolineChainsToExternalOverlay(page, target));
    EXPECT_FALSE(DXGIShared::TrampolineChainsToExternalOverlay(
        page, reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(target) + 0x10)));
    VirtualFree(page, 0, MEM_RELEASE);
}

TEST(DXGISharedSteamTrampolineChainTest, E9TrampolineMatchesPreservedExternalHook) {
    void* page = static_cast<void*>(AllocateWritableVTablePage());
    ASSERT_NE(page, nullptr);
    const auto target = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(page) + 0x200);
    WriteE9Jump(page, target);

    EXPECT_TRUE(DXGIShared::TrampolineChainsToExternalOverlay(page, target));
    VirtualFree(page, 0, MEM_RELEASE);
}

TEST(DXGISharedSteamTrampolineChainTest, GenericModeDetectsForeignChainTargets) {
    void* page = static_cast<void*>(AllocateWritableVTablePage());
    ASSERT_NE(page, nullptr);
    const auto foreignTarget = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(page) + 0x200);
    WriteFF25Jump(page, foreignTarget);

    // No preserved hook target: a chain outside dxgi.dll counts as foreign.
    EXPECT_TRUE(DXGIShared::TrampolineChainsToExternalOverlay(page, nullptr));

    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    ASSERT_NE(hDXGI, nullptr);
    WriteFF25Jump(page, reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hDXGI) + 0x1000));
    EXPECT_FALSE(DXGIShared::TrampolineChainsToExternalOverlay(page, nullptr));
    VirtualFree(page, 0, MEM_RELEASE);
}

TEST(DXGISharedSteamTrampolineChainTest, RejectsCleanTrampolineAndNullArguments) {
    void* page = static_cast<void*>(AllocateWritableVTablePage());
    ASSERT_NE(page, nullptr);
    // Clean trampoline bytes (real dxgi!Present prolog): no entry jump.
    auto* bytes = static_cast<uint8_t*>(page);
    bytes[0] = 0x48;  // mov [rsp+8],rbx
    bytes[1] = 0x89;
    bytes[2] = 0x5C;
    bytes[3] = 0x24;
    bytes[4] = 0x10;
    EXPECT_FALSE(DXGIShared::TrampolineChainsToExternalOverlay(page, nullptr));
    EXPECT_FALSE(DXGIShared::TrampolineChainsToExternalOverlay(nullptr, nullptr));
    VirtualFree(page, 0, MEM_RELEASE);
}

// Talos + DLSS FG + Steam + RTSS crash (session 20260812_024730): five milliseconds after a
// foreign re-hook took the Present entry from CE, CE jumped through its saved hook thunk whose
// FF25 payload now read 0x295C8999101 - a heap address - and died with a DEP execute violation
// inside TryInvokeGuardedExternalSteamOverlayPresent. A saved foreign handler is only valid
// while the overlay that owns it keeps its runtime-allocated thunk alive, so every transfer
// must first prove the entry AND the address it forwards to are still executable.
TEST(DXGISharedForeignHandlerValidityTest, RejectsThunksForwardingToNonExecutableMemory) {
    void* thunk = static_cast<void*>(AllocateWritableVTablePage());
    ASSERT_NE(thunk, nullptr);
    void* dataTarget = static_cast<void*>(AllocateWritableVTablePage());
    ASSERT_NE(dataTarget, nullptr);

    DWORD executableProtect = 0;
    ASSERT_NE(VirtualProtect(thunk, sizeof(void*) * 2, PAGE_EXECUTE_READWRITE, &executableProtect), 0);

    // The exact crash shape: an executable thunk whose payload points at plain data.
    WriteFF25Jump(thunk, dataTarget);
    EXPECT_FALSE(DXGIShared::IsCallableForeignPresentHandler(thunk));

    // Same thunk, now forwarding to real executable code, stays callable.
    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    ASSERT_NE(hDXGI, nullptr);
    void* codeTarget = reinterpret_cast<void*>(GetProcAddress(hDXGI, "CreateDXGIFactory"));
    ASSERT_NE(codeTarget, nullptr);
    WriteFF25Jump(thunk, codeTarget);
    EXPECT_TRUE(DXGIShared::IsCallableForeignPresentHandler(thunk));

    VirtualFree(thunk, 0, MEM_RELEASE);
    VirtualFree(dataTarget, 0, MEM_RELEASE);
}

TEST(DXGISharedForeignHandlerValidityTest, RejectsNullAndNonExecutableHandlers) {
    EXPECT_FALSE(DXGIShared::IsCallableForeignPresentHandler(nullptr));

    void* dataPage = static_cast<void*>(AllocateWritableVTablePage());
    ASSERT_NE(dataPage, nullptr);
    EXPECT_FALSE(DXGIShared::IsCallableForeignPresentHandler(dataPage));
    VirtualFree(dataPage, 0, MEM_RELEASE);

    // A freed thunk address is not callable either.
    void* freed = static_cast<void*>(AllocateWritableVTablePage());
    ASSERT_NE(freed, nullptr);
    DWORD executableProtect = 0;
    ASSERT_NE(VirtualProtect(freed, sizeof(void*) * 2, PAGE_EXECUTE_READWRITE, &executableProtect), 0);
    VirtualFree(freed, 0, MEM_RELEASE);
    EXPECT_FALSE(DXGIShared::IsCallableForeignPresentHandler(freed));
}

// Session 20260812_140930 (dx12_fg_switch_test via Steam, all FG off, Steam overlay + RTSS
// both loaded): CE injected after the game had already created its D3D12 device and
// swapchain, so no CWrapDXGISwapChain could ever exist for it; the two foreign overlays then
// put CE into the leave-the-entry mode, whose entire premise is "intercept through the
// swapchain wrapper". The result was zero Present interception for the whole session
// (`Postponed temp swapchain also failed`, game submitting ~720 ECL/s, no overlay).
//
// The fix is a deep hook in the dxgi!Present BODY, past the five entry bytes Steam and RTSS
// keep restoring and re-patching around each other: CE runs below the whole foreign chain,
// owns no entry bytes, and sees presents on swapchains it never created.
TEST(DXGISharedSourceTest, ForeignChainModeTakesADeepBodyViewSoPreExistingSwapchainsAreCovered) {
    namespace fs = std::filesystem;
    const fs::path installSource = fs::current_path() / "hook" / "common" / "dxgi_shared_hooks_present.cpp";
    ASSERT_TRUE(fs::exists(installSource));
    const std::string install = ce::test_source::ReadFile(installSource);
    ASSERT_FALSE(install.empty());

    const size_t leaveEntry = install.find("ShouldLeavePresentEntryToForeignOverlayChain(");
    ASSERT_NE(leaveEntry, std::string::npos);
    // The entry bytes are volatile (RTSS restores/re-patches them around every call), so the
    // decision must rest on the loaded-overlay count, not on this instant's sample, and it must
    // run before the bypass machinery that does need a visible foreign jump.
    EXPECT_NE(install.find("externalJmpDetected || loadedOverlayCount >= 2", leaveEntry), std::string::npos);
    const size_t bypassBlock = install.find("if (externalJmpDetected) {");
    ASSERT_NE(bypassBlock, std::string::npos);
    EXPECT_LT(leaveEntry, bypassBlock);
    // The body view is taken inside the leave-entry branch, and only a view that was actually
    // obtained may latch the install — otherwise a refused body patch blinds the session with
    // no retry, because InstallPresentInlineHooks early-returns on the latch.
    const size_t deepInstall = install.find(
        "InstallPresentBodyHooksBelowForeignChain(presentAddr, present1Addr, observedEntryPatchSize)", leaveEntry);
    ASSERT_NE(deepInstall, std::string::npos);
    // The very first latch after the decision is that conditional one, never a bare `= true`.
    const size_t firstLatchAfterDecision = install.find("s_inlineHooksInstalled =", leaveEntry);
    ASSERT_NE(firstLatchAfterDecision, std::string::npos);
    EXPECT_LT(firstLatchAfterDecision, deepInstall);
    const std::string decisionBlock = install.substr(leaveEntry, deepInstall - leaveEntry);
    EXPECT_EQ(decisionBlock.find("s_inlineHooksInstalled = true;"), std::string::npos);

    // The Present view below the chain is a deep body hook, never an entry patch.
    const size_t helper = install.find("bool InstallPresentBodyHooksBelowForeignChain(");
    ASSERT_NE(helper, std::string::npos);
    EXPECT_NE(install.find("InlineHook::InstallDeepHookPublished(presentAddr, (void*)DetourPresent", helper),
              std::string::npos);
    // A Present1 entry a foreign overlay owns gets the same deep treatment; an unclaimed one
    // has no chain to damage, so the ordinary prepend is correct there.
    EXPECT_NE(install.find("InlineHook::InstallDeepHookPublished(present1Addr, (void*)DetourPresent1", helper),
              std::string::npos);
    // Both entries carry the observed patch span, so a momentarily restored entry cannot make
    // the body hook refuse (session 20260812_150918: Present refused on byte=0x48 milliseconds
    // after the caller logged the E9, and Present is the entry the game actually uses).
    EXPECT_NE(install.find("observedPresentEntryPatchSize", helper), std::string::npos);
    EXPECT_EQ(install.find("present1EntryIsForeign", helper), std::string::npos);
    // Losing the body view leaves the overlay invisible — that must be an important log line,
    // not a silent fallback.
    EXPECT_NE(install.find("Present view at all unless it wraps the presenting swapchain", helper),
              std::string::npos);

    // The DXGI bypass built moments earlier resumes at exactly the offset the deep hook takes
    // over, so leaving it in place would route every "skip the foreign entry" consumer back
    // into CE's own detour. It must be republished as the deep trampoline.
    const size_t bypassRepublish = install.find("dxgi_shared_oPresentBypass = dxgi_shared_oPresentDeepBody", helper);
    ASSERT_NE(bypassRepublish, std::string::npos);
    EXPECT_NE(install.find("dxgi_shared_oPresent1Bypass = dxgi_shared_oPresent1DeepBody", helper), std::string::npos);
}

// The deep hook is entered BELOW the foreign chain, so CE's own forward must run the
// remaining real body. Forwarding through the live entry there re-runs Steam and RTSS and
// re-enters the deep hook without end.
TEST(DXGISharedSourceTest, DeepBodyForwardIsCheckedBeforeEveryForeignChainEntryForward) {
    namespace fs = std::filesystem;
    const fs::path presentSource = fs::current_path() / "hook" / "common" / "dxgi_shared_original.cpp";
    const fs::path present1Source = fs::current_path() / "hook" / "common" / "dxgi_shared_original_present1.cpp";
    ASSERT_TRUE(fs::exists(presentSource));
    ASSERT_TRUE(fs::exists(present1Source));
    const std::string present = ce::test_source::ReadFile(presentSource);
    const std::string present1 = ce::test_source::ReadFile(present1Source);
    ASSERT_FALSE(present.empty());
    ASSERT_FALSE(present1.empty());

    // Shutdown path and steady-state path, both in Present and Present1.
    for (const std::string* source : {&present, &present1}) {
        const bool isPresent1 = source == &present1;
        const char* deepGlobal = isPresent1 ? "dxgi_shared_oPresent1DeepBody" : "dxgi_shared_oPresentDeepBody";

        size_t searchFrom = 0;
        int deepChecksBeforeEntryForwards = 0;
        for (int i = 0; i < 2; ++i) {
            const size_t deep = source->find(std::string("if (") + deepGlobal + ")", searchFrom);
            ASSERT_NE(deep, std::string::npos);
            const size_t entryForward = source->find("IsPresentEntryLeftToForeignChain()", deep);
            ASSERT_NE(entryForward, std::string::npos);
            EXPECT_LT(deep, entryForward);
            ++deepChecksBeforeEntryForwards;
            searchFrom = deep + 1;
        }
        EXPECT_EQ(deepChecksBeforeEntryForwards, 2);
    }

    // Both forwards are logged rate-limited so a live session shows which view CE is on.
    EXPECT_NE(present.find("foreign-chain deep body forward"), std::string::npos);
    EXPECT_NE(present1.find("foreign-chain deep body forward"), std::string::npos);
}

// A deep body hook is a full CE Present view: DX12Hook must stop reporting "no Present
// hooks" (which is what drove FindAndWrapPreExistingSwapchains to give up), and the
// swapchain wrapper must delegate instead of running a second Present path over the same
// frames. Both decisions read these two predicates.
TEST(DXGISharedSourceTest, DeepBodyHookCountsAsAPresentViewButNotAsAnOwnedEntryPatch) {
    namespace fs = std::filesystem;
    const fs::path hooksSource = fs::current_path() / "hook" / "common" / "dxgi_shared_hooks.cpp";
    ASSERT_TRUE(fs::exists(hooksSource));
    const std::string hooks = ce::test_source::ReadFile(hooksSource);
    ASSERT_FALSE(hooks.empty());

    const size_t inlineHooks = hooks.find("bool HasPresentInlineHooks()");
    const size_t detourHooks = hooks.find("bool HasPresentDetourHooks()");
    const size_t prepended = hooks.find("bool HasPrependedPresentEntryHook()");
    ASSERT_NE(inlineHooks, std::string::npos);
    ASSERT_NE(detourHooks, std::string::npos);
    ASSERT_NE(prepended, std::string::npos);
    EXPECT_NE(hooks.find("dxgi_shared_oPresentDeepBody", inlineHooks), std::string::npos);
    EXPECT_NE(hooks.find("dxgi_shared_oPresentDeepBody", detourHooks), std::string::npos);
    // The owned-entry predicate must be false in leave-entry mode: CE holds no entry bytes
    // there, so the "second overlay joined an entry CE prepended over" warning does not apply.
    EXPECT_NE(hooks.find("IsPresentEntryLeftToForeignChain()", prepended), std::string::npos);

    const fs::path detectSource = fs::current_path() / "hook" / "main_overlay_detect.cpp";
    ASSERT_TRUE(fs::exists(detectSource));
    const std::string detect = ce::test_source::ReadFile(detectSource);
    ASSERT_FALSE(detect.empty());
    EXPECT_NE(detect.find("DXGIShared::HasPrependedPresentEntryHook()"), std::string::npos);
    EXPECT_EQ(detect.find("DXGIShared::HasPresentInlineHooks()"), std::string::npos);
}

// Session 20260812_144425 (build 0.1.5953, the deep body view's first real run): CE's overlay
// rendered until the deep hook took over, then vanished. Every present logged
// `Bypassing DX12 ProcessFrame for third-party overlay swapchain <game swapchain>
// (caller=RTSSHooks64.dll)` — one per frame, for the game's own swapchain.
//
// Cause: "a third-party overlay made this call" is an inference from the IMMEDIATE caller,
// which only holds while CE sits at the top of the Present chain. Below the chain the caller
// of dxgi!Present is always the last foreign overlay in it, for every swapchain. Present
// provenance must therefore be resolved from the stack in that mode — negatively for the
// overlay classification (swapchain identity stays authoritative), positively for the FG
// interposers, whose frames are simply further out.
TEST(DXGISharedSourceTest, PresentProvenanceIsNotTakenFromTheImmediateCallerBelowAForeignChain) {
    namespace fs = std::filesystem;
    const fs::path presentSource = fs::current_path() / "hook" / "common" / "dxgi_shared_present.cpp";
    const fs::path present1Source = fs::current_path() / "hook" / "common" / "dxgi_shared_present1.cpp";
    ASSERT_TRUE(fs::exists(presentSource));
    ASSERT_TRUE(fs::exists(present1Source));
    const std::string present = ce::test_source::ReadFile(presentSource);
    const std::string present1 = ce::test_source::ReadFile(present1Source);
    ASSERT_FALSE(present.empty());
    ASSERT_FALSE(present1.empty());

    for (const std::string* source : {&present, &present1}) {
        // The third-party-overlay classification is suppressed below the chain.
        const size_t overlayClassification = source->find("callerFromThirdPartyOverlay =");
        ASSERT_NE(overlayClassification, std::string::npos);
        const size_t overlaySuppression =
            source->find("!IsPresentInterceptedBelowForeignChain()", overlayClassification);
        ASSERT_NE(overlaySuppression, std::string::npos);

        // FG provenance is recovered from the originating frame instead of dropped, through
        // the single bounded walk (not one full-stack scan per module).
        const size_t resolve = source->find("ResolvePresentOriginatorBelowForeignChain(");
        ASSERT_NE(resolve, std::string::npos);
        ASSERT_GE(resolve, 200u);
        EXPECT_NE(source->find("IsPresentInterceptedBelowForeignChain()", resolve - 200), std::string::npos);
        const size_t streamline = source->find("callerFromStreamlineModule =", resolve);
        ASSERT_NE(streamline, std::string::npos);
        EXPECT_NE(source->find("originatorFromStreamline", streamline), std::string::npos);
        const size_t ffx = source->find("callerFromFFXFrameGenerationModule =", resolve);
        ASSERT_NE(ffx, std::string::npos);
        EXPECT_NE(source->find("originatorFromFFXFrameGeneration", ffx), std::string::npos);
    }

    // The originator walk must stop at the first real originator instead of scanning every
    // frame for every module: address->module resolution takes the loader lock and this runs
    // on the Present hot path.
    const fs::path presentImpl = fs::current_path() / "hook" / "common" / "dxgi_shared_present.cpp";
    const std::string presentImplText = ce::test_source::ReadFile(presentImpl);
    ASSERT_FALSE(presentImplText.empty());
    const size_t resolver = presentImplText.find("void ResolvePresentOriginatorBelowForeignChain(");
    ASSERT_NE(resolver, std::string::npos);
    EXPECT_NE(presentImplText.find("IsThirdPartyOverlayModulePath(framePath)", resolver), std::string::npos);
    EXPECT_NE(presentImplText.find("IsGraphicsDispatchModulePath(framePath)", resolver), std::string::npos);
    EXPECT_NE(presentImplText.find("frameModule == CaptureEngineModuleHandle()", resolver), std::string::npos);

    // The predicate itself is derived from the deep-body trampolines, not from a separate flag
    // that could drift out of sync with them.
    const fs::path sharedSource = fs::current_path() / "hook" / "common" / "dxgi_shared.cpp";
    ASSERT_TRUE(fs::exists(sharedSource));
    const std::string shared = ce::test_source::ReadFile(sharedSource);
    ASSERT_FALSE(shared.empty());
    const size_t predicate = shared.find("bool IsPresentInterceptedBelowForeignChain()");
    ASSERT_NE(predicate, std::string::npos);
    EXPECT_NE(shared.find("dxgi_shared_oPresentDeepBody != nullptr", predicate), std::string::npos);
    EXPECT_NE(shared.find("dxgi_shared_oPresent1DeepBody != nullptr", predicate), std::string::npos);
}

// Session 20260812_145524 (build 0.1.5954, DLSS FG on): presents took ~5 s each
// (`DetourPresent TOTAL SLOW 5014.9ms`, ~0.2 fps). The dump caught the exact loop —
// sl_dlss_g -> CWrapDXGISwapChain::Present -> Steam -> RTSS -> DetourPresent (CE's deep body
// hook) -> TryInvokeGuardedExternalSteamOverlayPresent -> Steam -> RTSS -> GetTickCount, i.e.
// CE re-invited a chain that had already run above it, and RTSS's reentrancy guard spun.
//
// Below the chain every foreign overlay has drawn before CE is entered, so there is never
// anything to service by invoking one of them.
TEST(DXGISharedSourceTest, NoForeignOverlayHandlerIsInvokedWhileCEInterceptsBelowTheChain) {
    namespace fs = std::filesystem;
    const fs::path steamSource = fs::current_path() / "hook" / "common" / "dxgi_shared_steam.cpp";
    ASSERT_TRUE(fs::exists(steamSource));
    const std::string steam = ce::test_source::ReadFile(steamSource);
    ASSERT_FALSE(steam.empty());

    const size_t entry = steam.find("bool TryInvokeGuardedExternalSteamOverlayPresent(");
    ASSERT_NE(entry, std::string::npos);
    const size_t belowChainGuard = steam.find("if (IsPresentInterceptedBelowForeignChain())", entry);
    ASSERT_NE(belowChainGuard, std::string::npos);

    // It must fail closed BEFORE the handler is resolved, not merely before the call: resolving
    // and validating a foreign thunk is itself work CE has no reason to do in this mode.
    const size_t handlerResolution = steam.find("GetCallableExternalOverlayPresentHook()", entry);
    ASSERT_NE(handlerResolution, std::string::npos);
    EXPECT_LT(belowChainGuard, handlerResolution);

    const size_t declineReturn = steam.find("return false;", belowChainGuard);
    ASSERT_NE(declineReturn, std::string::npos);
    EXPECT_LT(declineReturn, handlerResolution);
    EXPECT_NE(steam.find("already drew above this call", belowChainGuard), std::string::npos);
}
