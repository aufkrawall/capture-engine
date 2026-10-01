#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../hook/common/dxgi_color_space_hook_policy.h"
#include "../hook/wrappers/inline_hook_policy.h"
#include "source_fragment_reader.h"

namespace {

namespace policy = ce::dxgi_color_space_hook;

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

TEST(DxgiColorSpaceHookPolicyTest, NoOverlayKeepsTheEntryHook) {
    EXPECT_FALSE(policy::IsSetColorSpace1EntryForeignOwned(/*visible=*/false, /*loadedOverlays=*/0));
    EXPECT_EQ(policy::SetColorSpace1BelowChainPatchSpan(false, 0), 0);
    EXPECT_TRUE(policy::ShouldPrependSetColorSpace1Entry(/*foreignOwned=*/false, /*belowChain=*/false));
    EXPECT_TRUE(policy::ShouldPrependSetColorSpace1Entry(false, true));
}

TEST(DxgiColorSpaceHookPolicyTest, LoadedOverlayOwnsTheCleanEntryAndGetsTheWidestSpan) {
    for (size_t loadedOverlays : {1u, 2u}) {
        const bool owned = policy::IsSetColorSpace1EntryForeignOwned(/*visible=*/false, loadedOverlays);
        EXPECT_TRUE(owned);
        EXPECT_EQ(policy::SetColorSpace1BelowChainPatchSpan(false, loadedOverlays), 14);
        EXPECT_FALSE(policy::ShouldPrependSetColorSpace1Entry(owned, /*belowChain=*/true));
        // Body refusal (including x86): retain color-space tracking through the entry fallback.
        EXPECT_TRUE(policy::ShouldPrependSetColorSpace1Entry(owned, /*belowChain=*/false));
    }
}

TEST(DxgiColorSpaceHookPolicyTest, VisibleForeignPatchOwnsTheEntryWithoutATrackedOverlay) {
    for (size_t loadedOverlays : {0u, 1u, 2u}) {
        const bool owned = policy::IsSetColorSpace1EntryForeignOwned(/*visible=*/true, loadedOverlays);
        EXPECT_TRUE(owned);
        // InstallDeepHookPublished measures the visible E9 / FF 25 patch itself.
        EXPECT_EQ(policy::SetColorSpace1BelowChainPatchSpan(true, loadedOverlays), 0);
        EXPECT_FALSE(policy::ShouldPrependSetColorSpace1Entry(owned, /*belowChain=*/true));
        EXPECT_TRUE(policy::ShouldPrependSetColorSpace1Entry(owned, /*belowChain=*/false));
    }
}

TEST(DxgiColorSpaceHookPolicyTest, SystemDxgiPrologCanBeUndoneBeyondTheFourteenByteEntrySpan) {
    // cdb + Microsoft symbols, dxgi 10.0.26100.9549, CDXGISwapChain::SetColorSpace1:
    // mov [rsp+18h],rbx; push rbp; push rsi; push rdi; sub rsp,80h. Resume at +0xF.
    const unsigned char prolog[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57,
                                   0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00};
    ce::inline_hook_policy::DeepHookPrologUndo undo;
    ASSERT_TRUE(ce::inline_hook_policy::TryAnalyzeDeepHookProlog(prolog, static_cast<int>(sizeof(prolog)), &undo));
    EXPECT_GE(sizeof(prolog), static_cast<size_t>(policy::SetColorSpace1BelowChainPatchSpan(false, 1)));
    EXPECT_EQ(undo.stackDelta, 3 * 8 + 0x80);
    EXPECT_FALSE(undo.restoreRbp);
    // A five-byte foreign jump resumes after the shadow save, before any stack change.
    ASSERT_TRUE(ce::inline_hook_policy::TryAnalyzeDeepHookProlog(prolog, 5, &undo));
    EXPECT_EQ(undo.stackDelta, 0);
    // Never split the sub rsp instruction merely to fit exactly fourteen bytes.
    EXPECT_FALSE(ce::inline_hook_policy::TryAnalyzeDeepHookProlog(prolog, 14, &undo));
}

TEST(DxgiColorSpaceHookPolicyTest, InstallerSamplesOwnershipAndPublishesTheBodyHookBeforeAnyEntryFallback) {
    const std::string source = ReadSource("hook/common/dxgi_shared_hooks.cpp");
    ASSERT_FALSE(source.empty());
    const size_t install = source.find("bool InstallSetColorSpace1InlineHook(IDXGISwapChain* pSwapChain,");
    ASSERT_NE(install, std::string::npos);
    const size_t sample = source.find("HasExternalEntryHook(colorSpaceAddress)", install);
    const size_t count = source.find("CountLoadedTrackedOverlayModules(", install);
    const size_t subset = source.find("TrackedOverlaySubset::kOverlay", count);
    const size_t owned =
        source.find("IsSetColorSpace1EntryForeignOwned(foreignEntryJumpVisible, loadedOverlayCount)", install);
    const size_t deep = source.find("InlineHook::InstallDeepHookPublished(", install);
    const size_t span =
        source.find("SetColorSpace1BelowChainPatchSpan(foreignEntryJumpVisible, loadedOverlayCount)", install);
    const size_t decide =
        source.find("ShouldPrependSetColorSpace1Entry(entryForeignOwned, colorSpaceTrampoline != nullptr)", install);
    const size_t prepend = source.find("InlineHook::InstallPublished(colorSpaceAddress", install);
    ASSERT_NE(sample, std::string::npos);
    ASSERT_NE(count, std::string::npos);
    ASSERT_NE(subset, std::string::npos);
    ASSERT_NE(owned, std::string::npos);
    ASSERT_NE(deep, std::string::npos);
    ASSERT_NE(span, std::string::npos);
    ASSERT_NE(decide, std::string::npos);
    ASSERT_NE(prepend, std::string::npos);
    EXPECT_LT(sample, owned);
    EXPECT_LT(count, owned);
    EXPECT_LT(subset, owned);
    EXPECT_LT(owned, deep);
    EXPECT_LT(deep, span);
    EXPECT_LT(span, decide);
    EXPECT_LT(decide, prepend);
    const size_t bodyReturn = source.find("return true;", decide);
    ASSERT_NE(bodyReturn, std::string::npos);
    EXPECT_LT(bodyReturn, prepend);
    // Both sites publish the same callable original before exposing the detour.
    const size_t bodyPublish = source.find("PublishSetColorSpace1Trampoline, nullptr", deep);
    const size_t entryPublish = source.find("PublishSetColorSpace1Trampoline, nullptr", prepend);
    ASSERT_NE(bodyPublish, std::string::npos);
    ASSERT_NE(entryPublish, std::string::npos);
    EXPECT_LT(bodyPublish, decide);
}

TEST(DxgiColorSpaceHookPolicyTest, BodyHookUsesTheExistingColorTrackingAndRemovalPaths) {
    const std::string source = ReadSource("hook/common/dxgi_shared_hooks.cpp");
    const size_t deep = source.find("InlineHook::InstallDeepHookPublished(");
    ASSERT_NE(deep, std::string::npos);
    EXPECT_NE(source.find("reinterpret_cast<void*>(DetourSetColorSpace1)", deep), std::string::npos);
    const size_t detour = source.find("HRESULT STDMETHODCALLTYPE DetourSetColorSpace1(");
    ASSERT_NE(detour, std::string::npos);
    EXPECT_NE(source.find("dxgi_shared_oSetColorSpace1Trampoline.load(std::memory_order_acquire)", detour),
              std::string::npos);
    EXPECT_NE(source.find("ShouldRecordDetouredColorSpaceChange(dxgi_shared_s_wrapperSetColorSpaceForwardDepth)",
                          detour),
              std::string::npos);

    const std::string teardown = ReadSource("hook/common/dxgi_shared_hooks_present_vtable.cpp");
    for (const char* function : {"void RemovePresentHooks() {", "void RemoveSwapchainVTableHooks() {"}) {
        const size_t remove = teardown.find(function);
        ASSERT_NE(remove, std::string::npos);
        const size_t all = teardown.find("InlineHook::RemoveAll();", remove);
        const size_t clear = teardown.find("dxgi_shared_oSetColorSpace1Trampoline.store(nullptr", remove);
        ASSERT_NE(all, std::string::npos);
        ASSERT_NE(clear, std::string::npos);
        EXPECT_LT(all, clear);
    }
    const std::string inlineHook = ReadSource("hook/wrappers/inline_hook.cpp");
    const size_t removeAll = inlineHook.find("void RemoveAll() {");
    ASSERT_NE(removeAll, std::string::npos);
    EXPECT_NE(inlineHook.find("RemoveAllDeepHooksLocked();", removeAll), std::string::npos);
}

}  // namespace
