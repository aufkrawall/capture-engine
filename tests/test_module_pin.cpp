// Regression coverage for the module-lifetime rule behind the Witcher 3 startup
// crash: CE resolved, inline-patched and called into d3d11.dll while holding
// only a non-owning GetModuleHandle handle. A transient probe load released the
// module about a second later, which faulted CE first on the unmapped export
// entry (DX11Hook::Init reading D3D11CreateDeviceAndSwapChain's first byte) and
// then inside D3D11CreateDeviceAndSwapChain itself, where d3d11's detach had
// already torn the NVIDIA UMD adapter cache down underneath CE's thread.
//
// See hook/common/module_pin.h.

#include <gtest/gtest.h>

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../hook/common/module_pin.h"
#include "../hook/common/module_pin_policy.h"
#include "source_fragment_reader.h"

namespace {

using ce::module_pin_policy::IsQueriedRangeReadableCode;
using ce::module_pin_policy::kProtectExecuteRead;
using ce::module_pin_policy::kProtectExecuteReadWrite;
using ce::module_pin_policy::kProtectExecuteWriteCopy;
using ce::module_pin_policy::kProtectGuard;
using ce::module_pin_policy::kProtectNoAccess;
using ce::module_pin_policy::kStateCommit;

constexpr uintptr_t kRegionBase = 0x7ff000000000ull;
constexpr size_t kRegionSize = 0x2000;

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

std::string FunctionBody(const std::string& source, const std::string& signature, const std::string& nextSignature) {
    const size_t begin = source.find(signature);
    if (begin == std::string::npos)
        return {};
    const size_t end = source.find(nextSignature, begin + signature.size());
    return source.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

// Something with a stable code address to probe. Marked noinline so the linker
// cannot fold it away entirely.
__attribute__((noinline)) int ProbeTargetFunction(int value) {
    return value * 3 + 1;
}

}  // namespace

TEST(ModulePinPolicyTest, AcceptsCommittedReadableCodeInsideOneRegion) {
    EXPECT_TRUE(IsQueriedRangeReadableCode(true, kRegionBase, kRegionSize, kStateCommit, kProtectExecuteRead,
                                           kRegionBase, 2));
    EXPECT_TRUE(IsQueriedRangeReadableCode(true, kRegionBase, kRegionSize, kStateCommit, kProtectExecuteReadWrite,
                                           kRegionBase + 0x100, 14));
    EXPECT_TRUE(IsQueriedRangeReadableCode(true, kRegionBase, kRegionSize, kStateCommit, kProtectExecuteWriteCopy,
                                           kRegionBase, 1));
}

TEST(ModulePinPolicyTest, RejectsMemoryThatIsNotCommittedReadableCode) {
    // The exact shape of the Witcher 3 fault: the query fails outright because
    // the image behind the cached export address has been unmapped.
    EXPECT_FALSE(IsQueriedRangeReadableCode(false, 0, 0, 0, 0, kRegionBase, 2));

    // Reserved-but-not-committed, and free, both fault on read.
    EXPECT_FALSE(IsQueriedRangeReadableCode(true, kRegionBase, kRegionSize, 0x2000 /*MEM_RESERVE*/,
                                            kProtectExecuteRead, kRegionBase, 2));
    EXPECT_FALSE(
        IsQueriedRangeReadableCode(true, kRegionBase, kRegionSize, 0x10000 /*MEM_FREE*/, 0, kRegionBase, 2));

    // No-access and guard pages fault even though the region is committed.
    EXPECT_FALSE(
        IsQueriedRangeReadableCode(true, kRegionBase, kRegionSize, kStateCommit, kProtectNoAccess, kRegionBase, 2));
    EXPECT_FALSE(IsQueriedRangeReadableCode(true, kRegionBase, kRegionSize, kStateCommit,
                                            kProtectExecuteRead | kProtectGuard, kRegionBase, 2));

    // Data memory is not a code entry, and execute-without-read is not provably
    // readable.
    EXPECT_FALSE(IsQueriedRangeReadableCode(true, kRegionBase, kRegionSize, kStateCommit, PAGE_READWRITE, kRegionBase,
                                            2));
    EXPECT_FALSE(
        IsQueriedRangeReadableCode(true, kRegionBase, kRegionSize, kStateCommit, PAGE_EXECUTE, kRegionBase, 2));

    // Zero-length and null probes are always refused.
    EXPECT_FALSE(
        IsQueriedRangeReadableCode(true, kRegionBase, kRegionSize, kStateCommit, kProtectExecuteRead, kRegionBase, 0));
    EXPECT_FALSE(IsQueriedRangeReadableCode(true, 0, kRegionSize, kStateCommit, kProtectExecuteRead, 0, 2));
}

TEST(ModulePinPolicyTest, RefusesRangesThatLeaveTheQueriedRegion) {
    // The next region can be unmapped, so a probe that straddles the boundary
    // still faults on the byte after the split.
    EXPECT_TRUE(IsQueriedRangeReadableCode(true, kRegionBase, kRegionSize, kStateCommit, kProtectExecuteRead,
                                           kRegionBase + kRegionSize - 14, 14));
    EXPECT_FALSE(IsQueriedRangeReadableCode(true, kRegionBase, kRegionSize, kStateCommit, kProtectExecuteRead,
                                            kRegionBase + kRegionSize - 13, 14));
    EXPECT_FALSE(IsQueriedRangeReadableCode(true, kRegionBase, kRegionSize, kStateCommit, kProtectExecuteRead,
                                            kRegionBase + kRegionSize, 1));
    EXPECT_FALSE(IsQueriedRangeReadableCode(true, kRegionBase, kRegionSize, kStateCommit, kProtectExecuteRead,
                                            kRegionBase - 1, 2));
}

TEST(ModulePinTest, IsReadableCodeSeparatesLiveCodeFromDataAndUnmappedMemory) {
    EXPECT_TRUE(ce::module_pin::IsReadableCode(reinterpret_cast<const void*>(&ProbeTargetFunction), 14));

    std::vector<uint8_t> data(64, 0x90);
    EXPECT_FALSE(ce::module_pin::IsReadableCode(data.data(), 2));

    EXPECT_FALSE(ce::module_pin::IsReadableCode(nullptr, 2));
    EXPECT_FALSE(ce::module_pin::IsReadableCode(reinterpret_cast<const void*>(static_cast<uintptr_t>(0x10)), 2));
}

TEST(ModulePinTest, IsReadableCodeSpansAdjacentReadableRegions) {
    // A module's .text splits into several query regions as soon as anyone
    // leaves a page at a different protection. A hook target that merely sits
    // near such a split is still readable, and refusing it would silently drop
    // the hook - so the probe must walk regions, not demand a single one.
    SYSTEM_INFO systemInfo = {};
    GetSystemInfo(&systemInfo);
    const size_t pageSize = systemInfo.dwPageSize;
    auto* pages = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, pageSize * 2, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(pages, nullptr);
    uint8_t* straddle = pages + pageSize - 4;

    EXPECT_TRUE(ce::module_pin::IsReadableCode(straddle, 14));

    DWORD previousProtect = 0;
    ASSERT_TRUE(VirtualProtect(pages + pageSize, pageSize, PAGE_EXECUTE_READ, &previousProtect));
    EXPECT_TRUE(ce::module_pin::IsReadableCode(straddle, 14));

    // The second page becoming unreadable-as-code must be caught, not skipped.
    ASSERT_TRUE(VirtualProtect(pages + pageSize, pageSize, PAGE_READWRITE, &previousProtect));
    EXPECT_FALSE(ce::module_pin::IsReadableCode(straddle, 14));
    EXPECT_TRUE(ce::module_pin::IsReadableCode(straddle, 4));

    ASSERT_TRUE(VirtualProtect(pages + pageSize, pageSize, PAGE_NOACCESS, &previousProtect));
    EXPECT_FALSE(ce::module_pin::IsReadableCode(straddle, 14));

    VirtualFree(pages, 0, MEM_RELEASE);
    EXPECT_FALSE(ce::module_pin::IsReadableCode(straddle, 14));
}

TEST(ModulePinTest, PinByNameResolvesLoadedModulesOnlyAndKeepsTheHandleStable) {
    const HMODULE pinned = ce::module_pin::PinByName("kernel32.dll");
    ASSERT_NE(pinned, nullptr);
    EXPECT_EQ(pinned, GetModuleHandleA("kernel32.dll"));
    // Pinning is idempotent: the same image, the same handle.
    EXPECT_EQ(ce::module_pin::PinByName("kernel32.dll"), pinned);

    EXPECT_EQ(ce::module_pin::PinByName("ce-module-that-is-not-loaded.dll"), nullptr);
    EXPECT_EQ(ce::module_pin::PinByName(nullptr), nullptr);
    EXPECT_EQ(ce::module_pin::PinByName(""), nullptr);
}

TEST(ModulePinTest, PinOwnerOfAddressResolvesImageBackedCodeOnly) {
    HMODULE self = nullptr;
    ASSERT_TRUE(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(&ProbeTargetFunction), &self));
    EXPECT_EQ(ce::module_pin::PinOwnerOfAddress(reinterpret_cast<const void*>(&ProbeTargetFunction)), self);

    // Allocated thunks and trampolines are not modules and cannot be pinned.
    void* allocated = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    ASSERT_NE(allocated, nullptr);
    EXPECT_EQ(ce::module_pin::PinOwnerOfAddress(allocated), nullptr);
    EXPECT_TRUE(ce::module_pin::IsReadableCode(allocated, 14));
    VirtualFree(allocated, 0, MEM_RELEASE);

    EXPECT_EQ(ce::module_pin::PinOwnerOfAddress(nullptr), nullptr);
}

TEST(ModulePinSourceTest, GraphicsRuntimeExportCaptureResolvesThroughAPin) {
    const std::string source = ReadSource("hook/wrappers/iat_hook_init.cpp");
    ASSERT_FALSE(source.empty());

    // Each of these caches export addresses in long-lived globals and hands them
    // to game code through RegisterDynamicHook, so the image behind them must be
    // pinned, never resolved with a non-owning handle.
    for (const char* module : {"dxgi.dll", "d3d12.dll", "d3d11.dll", "d3d10.dll", "d3d10_1.dll", "d3d9.dll",
                               "ddraw.dll"}) {
        const std::string pinned = std::string("ce::module_pin::PinByName(\"") + module + "\")";
        const std::string unpinned = std::string("GetModuleHandleA(\"") + module + "\")";
        EXPECT_NE(source.find(pinned), std::string::npos) << module;
        EXPECT_EQ(source.find(unpinned), std::string::npos) << module;
    }
}

TEST(ModulePinSourceTest, ExportHookPinsTheModuleItSavesAnOriginalFrom) {
    const std::string source = ReadSource("hook/wrappers/custom_hook.cpp");
    ASSERT_FALSE(source.empty());
    const std::string body = FunctionBody(source, "Status HookExport(const char* moduleName", "Status HookExportW(");
    ASSERT_FALSE(body.empty());

    EXPECT_NE(body.find("ce::module_pin::PinByName(moduleName)"), std::string::npos);
    EXPECT_EQ(body.find("GetModuleHandleA(moduleName)"), std::string::npos);
}

TEST(ModulePinSourceTest, DX11InstallCommitsOnlyAfterPinningWhatItPatches) {
    const std::string source = ReadSource("hook/main_install.cpp");
    ASSERT_FALSE(source.empty());
    const std::string body = FunctionBody(source, "if (dx11CondAll) {", "// For other APIs, skip if D3D12");
    ASSERT_FALSE(body.empty());

    const size_t pinD3D11 = body.find("ce::module_pin::PinByName(\"d3d11.dll\")");
    const size_t pinD3D10 = body.find("ce::module_pin::PinByName(\"d3d10.dll\")");
    const size_t pinD3D10_1 = body.find("ce::module_pin::PinByName(\"d3d10_1.dll\")");
    const size_t construct = body.find("g_DX11Hook = new DX11Hook()");
    ASSERT_NE(pinD3D11, std::string::npos);
    ASSERT_NE(pinD3D10, std::string::npos);
    ASSERT_NE(pinD3D10_1, std::string::npos);
    ASSERT_NE(construct, std::string::npos);
    EXPECT_LT(pinD3D11, construct);
    EXPECT_LT(pinD3D10, construct);
    EXPECT_LT(pinD3D10_1, construct);

    // A module that vanished between the presence check and the commit must not
    // latch g_DX11Hook, or the install can never be retried.
    EXPECT_NE(body.find("if (!pinnedD3D11 && !pinnedD3D10 && !pinnedD3D10_1)"), std::string::npos);
}

TEST(ModulePinSourceTest, DX11InitPinsItsModulesAndGuardsEveryEntryProbe) {
    const std::string source = ReadSource("hook/apis/dx11_hook.cpp");
    ASSERT_FALSE(source.empty());
    const std::string body = FunctionBody(source, "void DX11Hook::Init() {", "void DX11Hook::Shutdown() {");
    ASSERT_FALSE(body.empty());

    EXPECT_NE(body.find("ce::module_pin::PinByName(\"d3d11.dll\")"), std::string::npos);
    EXPECT_NE(body.find("ce::module_pin::PinByName(\"d3d10.dll\")"), std::string::npos);
    EXPECT_EQ(body.find("GetModuleHandleA(\"d3d11.dll\")"), std::string::npos);
    EXPECT_EQ(body.find("GetModuleHandleA(\"d3d10.dll\")"), std::string::npos);

    // Both create-entry probes must go through the guarded reader. Indexing a
    // raw `const uint8_t*` view of the entry is what faulted.
    EXPECT_NE(body.find("TryReadCreateEntryBytes(reinterpret_cast<const void*>(pD3D10CD)"), std::string::npos);
    EXPECT_NE(body.find("TryReadCreateEntryBytes(reinterpret_cast<const void*>(pTempCreate)"), std::string::npos);
    EXPECT_EQ(body.find("const uint8_t* d3d10Entry"), std::string::npos);
    EXPECT_EQ(body.find("const uint8_t* tempCreateEntry"), std::string::npos);

    const std::string helper = FunctionBody(source, "bool TryReadCreateEntryBytes(", "}  // namespace");
    ASSERT_FALSE(helper.empty());
    EXPECT_NE(helper.find("ce::module_pin::PinOwnerOfAddress(entry)"), std::string::npos);
    EXPECT_NE(helper.find("ce::module_pin::IsReadableCode(entry, 2)"), std::string::npos);
}

TEST(ModulePinSourceTest, InlinePatchingValidatesItsTargetWithoutPinningForeignPlugins) {
    const std::string installSource = ReadSource("hook/wrappers/inline_hook.cpp");
    ASSERT_FALSE(installSource.empty());
    const size_t guard = installSource.find("ce::module_pin::IsReadableCode(target, PATCH_SIZE)");
    const size_t firstByteRead = installSource.find("code[i]");
    ASSERT_NE(guard, std::string::npos);
    ASSERT_NE(firstByteRead, std::string::npos);
    EXPECT_LT(guard, firstByteRead);
    // Streamline/NGX/FFX plugins genuinely unload while FG switches modes, and
    // CE answers that with scoped holds and teardown latches. A blanket pin in
    // the generic installer would silently take that lifecycle away.
    EXPECT_EQ(installSource.find("ce::module_pin::PinOwnerOfAddress(target)"), std::string::npos);
    EXPECT_EQ(installSource.find("ce::module_pin::PinByName"), std::string::npos);

    const std::string deepSource = ReadSource("hook/wrappers/inline_hook_deep.cpp");
    ASSERT_FALSE(deepSource.empty());
    const std::string bypass = FunctionBody(deepSource, "void* CreateBypassTrampoline(void* target) {",
                                            "\n}  // namespace InlineHook");
    ASSERT_FALSE(bypass.empty());
    const size_t bypassGuard = bypass.find("ce::module_pin::IsReadableCode(target, 2)");
    const size_t bypassRead = bypass.find("if (code[0] == 0xE9)");
    ASSERT_NE(bypassGuard, std::string::npos);
    ASSERT_NE(bypassRead, std::string::npos);
    EXPECT_LT(bypassGuard, bypassRead);
    EXPECT_EQ(deepSource.find("ce::module_pin::PinOwnerOfAddress(target)"), std::string::npos);
}
