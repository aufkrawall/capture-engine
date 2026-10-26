#include <gtest/gtest.h>
#include <windows.h>

#include <winternl.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hook/runtime/module_replacement_policy.h"

namespace {

using namespace ce::module_replacement;

TEST(ModuleReplacementPolicyTest, RequesterFollowsTheModuleLocation) {
    EXPECT_EQ(ClassifyRequesterPath(L"D:\\GTAVEnhanced\\GTA5_Enhanced.exe"), Requester::kApplication);
    EXPECT_EQ(ClassifyRequesterPath(L"C:\\Users\\u\\npi\\sl\\sl.interposer.dll"), Requester::kApplication);
    EXPECT_EQ(ClassifyRequesterPath(L"C:\\Users\\u\\npi\\3rdparty\\vkd3d\\d3d12core.dll"), Requester::kApplication);
    EXPECT_EQ(ClassifyRequesterPath(L"C:\\WINDOWS\\SYSTEM32\\d3d11.dll"), Requester::kSystem);
    EXPECT_EQ(ClassifyRequesterPath(L"C:\\Windows\\SysWOW64\\mfplat.dll"), Requester::kSystem);
    EXPECT_EQ(ClassifyRequesterPath(L"C:\\Windows\\WinSxS\\amd64_microsoft.windows.common-controls\\comctl32.dll"),
              Requester::kSystem);
    // The driver lives in the DriverStore under System32.
    EXPECT_EQ(ClassifyRequesterPath(
                  L"C:\\WINDOWS\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64\\nvwgf2umx.dll"),
              Requester::kSystem);
    EXPECT_EQ(ClassifyRequesterPath("C:/Windows/System32/mfplat.dll"), Requester::kSystem);
    EXPECT_EQ(ClassifyRequesterPath(L"C:\\Program Files\\Capture Engine\\capture_hook_x64.dll"),
              Requester::kCaptureEngine);
    EXPECT_EQ(ClassifyRequesterPath(L"C:\\Users\\u\\AppData\\Local\\CaptureEngine\\VK_LAYER_CE_overlay.dll"),
              Requester::kCaptureEngine);
    EXPECT_EQ(ClassifyRequesterPath(L"C:\\Program Files\\Capture Engine\\d3d12_wrappers.dll"),
              Requester::kCaptureEngine);
    // Only a whole directory named system32 counts.
    EXPECT_EQ(ClassifyRequesterPath(L"D:\\Games\\system32mods\\game.exe"), Requester::kApplication);
    EXPECT_EQ(ClassifyRequesterPath(L"D:\\system32.dll"), Requester::kApplication);
    // Generated code resolves to no module.
    EXPECT_EQ(ClassifyRequesterPath(static_cast<const wchar_t*>(nullptr)), Requester::kApplication);
    EXPECT_EQ(ClassifyRequesterPath(L""), Requester::kApplication);
}

TEST(ModuleReplacementPolicyTest, ScopeKeepsProxiesAndCaptureEngineOnTheImagesTheyAskFor) {
    EXPECT_EQ(ScopeFor(Requester::kSystem, false), RequesterScope::kNone);
    EXPECT_EQ(ScopeFor(Requester::kSystem, true), RequesterScope::kNone);
    EXPECT_EQ(ScopeFor(Requester::kCaptureEngine, false), RequesterScope::kBareNames);
    EXPECT_EQ(ScopeFor(Requester::kApplication, true), RequesterScope::kBareNames);
    EXPECT_EQ(ScopeFor(Requester::kApplication, false), RequesterScope::kFull);
}

TEST(ModuleReplacementPolicyTest, BareNamesFollowLoaderNameRules) {
    EXPECT_TRUE(IsBareModuleName(L"dxgi.dll"));
    EXPECT_TRUE(IsBareModuleName("d3d12"));
    EXPECT_FALSE(IsBareModuleName(L"C:\\Windows\\System32\\dxgi.dll"));
    EXPECT_FALSE(IsBareModuleName(L".\\dxgi.dll"));
    EXPECT_FALSE(IsBareModuleName(L"C:dxgi.dll"));
    EXPECT_FALSE(IsBareModuleName(L""));

    EXPECT_TRUE(BareNameMatchesBaseName(L"dxgi.dll", "dxgi.dll"));
    EXPECT_TRUE(BareNameMatchesBaseName(L"DXGI.DLL", "dxgi.dll"));
    EXPECT_TRUE(BareNameMatchesBaseName("DXGI", "dxgi.dll"));
    EXPECT_TRUE(BareNameMatchesBaseName(L"d3d12", "d3d12.dll"));
    // A trailing dot means "exactly this name": no default extension.
    EXPECT_TRUE(BareNameMatchesBaseName(L"dxgi.dll.", "dxgi.dll"));
    EXPECT_FALSE(BareNameMatchesBaseName(L"dxgi.", "dxgi.dll"));
    EXPECT_FALSE(BareNameMatchesBaseName(L"dxgi.d", "dxgi.dll"));
    EXPECT_FALSE(BareNameMatchesBaseName(L"d3d12core.dll", "d3d12.dll"));
    EXPECT_FALSE(BareNameMatchesBaseName(L"d3d1", "d3d12.dll"));
    EXPECT_FALSE(BareNameMatchesBaseName(L"dxgi.dll", "d3d12.dll"));
    EXPECT_FALSE(BareNameMatchesBaseName(L"C:\\Windows\\System32\\dxgi.dll", "dxgi.dll"));
    EXPECT_EQ(PathBaseName(L"C:\\Windows\\System32\\dxgi.dll"), std::wstring(L"dxgi.dll"));
    EXPECT_EQ(PathBaseName("C:/x/d3d12.dll"), std::string("d3d12.dll"));
}

TEST(ModuleReplacementPolicyTest, CachedNativeEntryCallsForwardOnlyApplicationCallers) {
    EXPECT_TRUE(ShouldForwardEntryCall(ScopeFor(Requester::kApplication, false)));
    EXPECT_FALSE(ShouldForwardEntryCall(ScopeFor(Requester::kApplication, true)));
    EXPECT_FALSE(ShouldForwardEntryCall(ScopeFor(Requester::kCaptureEngine, false)));
    EXPECT_FALSE(ShouldForwardEntryCall(ScopeFor(Requester::kSystem, false)));
    // Native Agility components can live next to the executable. Their explicit native calls
    // must not enter a translation runtime; a configured vkd3d core still has its proxy scope.
    EXPECT_FALSE(ShouldForwardEntryCall(ScopeFor(Requester::kApplication, false, true)));
    EXPECT_EQ(ScopeFor(Requester::kApplication, true, true), RequesterScope::kBareNames);
}

TEST(ModuleReplacementPolicyTest, AgilityComponentsBelongToTheReplacedD3D12) {
    EXPECT_TRUE(IsRuntimeComponentOf("d3d12.dll", L"D3D12Core.dll"));
    EXPECT_TRUE(IsRuntimeComponentOf("D3D12.DLL", L"d3d12SDKLayers.dll"));
    EXPECT_FALSE(IsRuntimeComponentOf("dxgi.dll", L"D3D12Core.dll"));
    EXPECT_FALSE(IsRuntimeComponentOf("d3d12.dll", L"dstoragecore.dll"));
}

TEST(ModuleReplacementPolicyTest, OnlyApplicationImportersMoveAndCustomDllsAlwaysDo) {
    ImporterFacts game;
    EXPECT_TRUE(ShouldRebindImporter(game));

    ImporterFacts system;
    system.requester = Requester::kSystem;
    EXPECT_FALSE(ShouldRebindImporter(system));

    ImporterFacts captureEngine;
    captureEngine.requester = Requester::kCaptureEngine;
    EXPECT_FALSE(ShouldRebindImporter(captureEngine));

    ImporterFacts superseded;  // a game-folder proxy that was resident under the replaced name
    superseded.isSuperseded = true;
    EXPECT_FALSE(ShouldRebindImporter(superseded));

    // The Agility SDK's D3D12Core.dll in the game folder serves System32's d3d12.dll (CE's own native use).
    ImporterFacts agilityCore;
    agilityCore.belongsToReplacedRuntime = true;
    EXPECT_FALSE(ShouldRebindImporter(agilityCore));

    // vkd3d-proton's d3d12core.dll is configured too and must reach DXVK's dxgi.dll.
    ImporterFacts vkd3dCore;
    vkd3dCore.belongsToReplacedRuntime = true;
    vkd3dCore.isConfiguredCustomDll = true;
    EXPECT_TRUE(ShouldRebindImporter(vkd3dCore));
}

// An import directory laid out in a flat buffer: ForEachImportOf walks raw RVAs, so no PE headers are needed.
class ImportImage {
public:
    static constexpr DWORD kSize = 0x2000;
    static constexpr DWORD kDirectoryRva = 0x100;

    struct Slot {
        std::string name;  // empty: ordinal import
        uintptr_t bound;   // the value the loader (or a hook) left in the IAT slot
    };

    ImportImage() : bytes_(kSize, 0) {}

    void Set(const std::vector<std::pair<std::string, std::vector<Slot>>>& modules) {
        directorySize_ = static_cast<DWORD>((modules.size() + 1) * sizeof(IMAGE_IMPORT_DESCRIPTOR));
        DWORD cursor = 0x400;
        for (size_t m = 0; m < modules.size(); ++m) {
            auto* descriptor = At<IMAGE_IMPORT_DESCRIPTOR>(kDirectoryRva + m * sizeof(IMAGE_IMPORT_DESCRIPTOR));
            descriptor->Name = cursor;
            cursor = Put(cursor, modules[m].first);
            const DWORD lookup = Align(cursor);
            const DWORD iat = lookup + static_cast<DWORD>((modules[m].second.size() + 1) * sizeof(IMAGE_THUNK_DATA));
            cursor = iat + static_cast<DWORD>((modules[m].second.size() + 1) * sizeof(IMAGE_THUNK_DATA));
            descriptor->OriginalFirstThunk = lookup;
            descriptor->FirstThunk = iat;
            for (size_t i = 0; i < modules[m].second.size(); ++i) {
                const Slot& slot = modules[m].second[i];
                ULONG_PTR lookupValue = IMAGE_ORDINAL_FLAG | (i + 1);
                if (!slot.name.empty()) {
                    lookupValue = cursor;
                    cursor = Align(Put(cursor + 2, slot.name));
                }
                At<IMAGE_THUNK_DATA>(lookup + i * sizeof(IMAGE_THUNK_DATA))->u1.AddressOfData = lookupValue;
                At<IMAGE_THUNK_DATA>(iat + i * sizeof(IMAGE_THUNK_DATA))->u1.Function =
                    slot.bound ? slot.bound : lookupValue;  // 0: not yet bound, the slot still holds its lookup
            }
        }
    }

    BYTE* Base() { return bytes_.data(); }
    DWORD DirectorySize() const { return directorySize_; }
    ULONG_PTR SlotValue(const SlotChange& change) const { return *reinterpret_cast<ULONG_PTR*>(change.entry); }

private:
    template <typename T>
    T* At(size_t offset) {
        return reinterpret_cast<T*>(bytes_.data() + offset);
    }
    DWORD Put(DWORD offset, const std::string& text) {
        std::memcpy(bytes_.data() + offset, text.c_str(), text.size() + 1);
        return offset + static_cast<DWORD>(text.size() + 1);
    }
    static DWORD Align(DWORD offset) { return (offset + 7u) & ~7u; }

    std::vector<BYTE> bytes_;
    DWORD directorySize_ = 0;
};

TEST(ModuleReplacementPolicyTest, RebindPlanMovesOnlySlotsStillBoundIntoTheSupersededImage) {
    const ImageRange superseded{0x10000000, 0x100000};
    const ImageRange replacement{0x20000000, 0x100000};
    constexpr uintptr_t kForeignHook = 0x30000040;
    ImportImage image;
    image.Set({
        {"KERNEL32.dll", {{"GetModuleHandleW", 0x40000010}}},
        {"DXGI.dll",
         {{"CreateDXGIFactory2", superseded.base + 0x100},
          {"CreateDXGIFactory1", replacement.base + 0x200},  // already moved (an earlier pass)
          {"CreateDXGIFactory", kForeignHook},               // CE's wrapper or another tool owns the slot
          {"DXGIDeclareAdapterRemovalSupport", superseded.base + 0x300},  // the replacement lacks it
          {"DXGIGetDebugInterface1", 0},                     // the loader has not bound it yet
          {"", superseded.base + 0x400}}},                   // by ordinal: no name to resolve
        {"d3d12.dll", {{"D3D12CreateDevice", superseded.base + 0x500}}},
    });
    std::vector<std::string> resolved;
    const auto resolve = [&](const char* name) -> void* {
        resolved.emplace_back(name);
        if (std::strcmp(name, "CreateDXGIFactory2") == 0)
            return reinterpret_cast<void*>(replacement.base + 0x110);
        return nullptr;
    };
    const ImportRebindPlan plan =
        PlanImportRebind(image.Base(), ImportImage::kSize, ImportImage::kDirectoryRva, image.DirectorySize(),
                         "dxgi.dll", superseded, replacement, resolve, nullptr);

    EXPECT_EQ(plan.issue, IATHook::detail::ImportTableIssue::None);
    ASSERT_EQ(plan.changes.size(), 1u);
    EXPECT_EQ(plan.changes[0].expected, reinterpret_cast<void*>(superseded.base + 0x100));
    EXPECT_EQ(plan.changes[0].replacement, reinterpret_cast<void*>(replacement.base + 0x110));
    EXPECT_EQ(image.SlotValue(plan.changes[0]), superseded.base + 0x100) << "planning must not write";
    EXPECT_EQ(plan.alreadyReplaced, 1u);
    EXPECT_EQ(plan.keptForeign, 1u);
    EXPECT_EQ(plan.missingExport, 1u);
    EXPECT_STREQ(plan.firstMissing, "DXGIDeclareAdapterRemovalSupport");
    EXPECT_EQ(plan.notYetBound, 1u);
    EXPECT_EQ(plan.unnamed, 1u);
    // Only names that still point into the superseded image are looked up, and only in dxgi.dll's descriptor.
    EXPECT_EQ(resolved, (std::vector<std::string>{"CreateDXGIFactory2", "DXGIDeclareAdapterRemovalSupport"}));
}

// Whether Windows' loader notification sees a new module's imports already bound decides whether the
// replacement rebinds a late module before its DllMain runs; module_replacement.cpp queues the module
// for the post-load pass when they are not.
struct NotificationData {
    ULONG flags;
    const UNICODE_STRING* fullDllName;
    const UNICODE_STRING* baseDllName;
    void* dllBase;
    ULONG sizeOfImage;
};

struct NotificationProbe {
    const wchar_t* baseName = nullptr;
    int loads = 0;
    int boundAtNotification = 0;
    int unboundAtNotification = 0;
};

VOID CALLBACK ObserveLoad(ULONG reason, const NotificationData* data, PVOID context) {
    auto* probe = static_cast<NotificationProbe*>(context);
    if (reason != 1 || !data || !data->baseDllName || !data->baseDllName->Buffer ||
        std::wstring(data->baseDllName->Buffer, data->baseDllName->Length / sizeof(wchar_t)) != probe->baseName) {
        return;
    }
    ++probe->loads;
    auto* base = static_cast<BYTE*>(data->dllBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; directory.VirtualAddress && descriptor->Name; ++descriptor) {
        const ULONG_PTR slot = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk)->u1.Function;
        const bool bound = (slot & IMAGE_ORDINAL_FLAG) == 0 && slot >= data->sizeOfImage;
        ++(bound ? probe->boundAtNotification : probe->unboundAtNotification);
    }
}

TEST(ModuleReplacementLoaderTest, LoaderNotificationSeesBoundImports) {
    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto registerNotification = reinterpret_cast<LONG(NTAPI*)(ULONG, PVOID, PVOID, PVOID*)>(
        GetProcAddress(ntdll, "LdrRegisterDllNotification"));
    const auto unregisterNotification =
        reinterpret_cast<LONG(NTAPI*)(PVOID)>(GetProcAddress(ntdll, "LdrUnregisterDllNotification"));
    ASSERT_NE(registerNotification, nullptr);
    ASSERT_NE(unregisterNotification, nullptr);
    NotificationProbe probe;
    for (const wchar_t* candidate : {L"msimg32.dll", L"dciman32.dll", L"wtsapi32.dll", L"pdh.dll"}) {
        if (!GetModuleHandleW(candidate)) {
            probe.baseName = candidate;
            break;
        }
    }
    if (!probe.baseName) {
        GTEST_SKIP() << "every candidate module is already loaded";
    }
    PVOID cookie = nullptr;
    ASSERT_EQ(registerNotification(0, reinterpret_cast<PVOID>(&ObserveLoad), &probe, &cookie), 0);
    const HMODULE module = LoadLibraryExW(probe.baseName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    unregisterNotification(cookie);
    ASSERT_NE(module, nullptr);
    FreeLibrary(module);
    ASSERT_EQ(probe.loads, 1);
    std::printf("[  INFO    ] %ls at its load notification: %d descriptor(s) bound, %d unbound\n", probe.baseName,
                probe.boundAtNotification, probe.unboundAtNotification);
    // Windows 11 26300: every slot is bound when the notification arrives, ahead of the module's DllMain.
    EXPECT_GT(probe.boundAtNotification, 0);
    EXPECT_EQ(probe.unboundAtNotification, 0);
}

TEST(ModuleReplacementPolicyTest, RebindPlanIgnoresModulesThatDoNotImportTheName) {
    ImportImage image;
    image.Set({{"KERNEL32.dll", {{"Sleep", 0x40000020}}}});
    const ImportRebindPlan plan = PlanImportRebind(
        image.Base(), ImportImage::kSize, ImportImage::kDirectoryRva, image.DirectorySize(), "dxgi.dll",
        ImageRange{0x10000000, 0x1000}, ImageRange{0x20000000, 0x1000},
        [](const char*) -> void* { return nullptr; }, nullptr);
    EXPECT_EQ(plan.issue, IATHook::detail::ImportTableIssue::None);
    EXPECT_TRUE(plan.changes.empty());
    EXPECT_EQ(plan.alreadyReplaced + plan.keptForeign + plan.missingExport + plan.notYetBound + plan.unnamed, 0u);
}

}  // namespace
