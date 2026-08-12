#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../hook/common/dx12_factory_slot_policy.h"

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
    const std::string source = ReadSource("hook/apis/dx12_hook_hook_install.cpp");
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
    const std::string source = ReadSource("hook/apis/dx12_hook_hook_install.cpp");
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

}  // namespace
