// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "tests/flow/flow_test_support.h"

#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#include "sl.h"

// The in-process sl.interposer.dll takeover, end to end: a game module that
// statically imports the fake interposer from its own (vendor) folder is
// mapped before CE, then CE's real hook installation arms the takeover with
// the flow build directory as streamline_dll_path, and the game's own calls
// decide what happens.
namespace {

namespace fs = std::filesystem;

using TakeoverGameInit_t = int (*)();
using TakeoverGameInterposer_t = HMODULE (*)();

struct VendorGame {
    fs::path directory;
    fs::path vendor;
    HMODULE module = nullptr;

    VendorGame(const char* gameModule, const fs::path& residentInterposer) {
        char executable[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, executable, MAX_PATH);
        directory = fs::path(executable).parent_path();
        vendor = directory / "logs" / ce::flow::CurrentTestName() / "vendor";
        fs::create_directories(vendor);
        fs::copy_file(directory / residentInterposer, vendor / "sl.interposer.dll",
                      fs::copy_options::overwrite_existing);
        fs::copy_file(directory / gameModule, vendor / gameModule, fs::copy_options::overwrite_existing);
        // The game's folder first, as for an executable that imports its own interposer.
        module = LoadLibraryExW((vendor / gameModule).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }

    template <typename T>
    T Export(const char* name) const {
        return reinterpret_cast<T>(GetProcAddress(module, name));
    }

    static fs::path PathOf(HMODULE module) {
        wchar_t path[MAX_PATH] = {};
        return GetModuleFileNameW(module, path, MAX_PATH) ? fs::path(path) : fs::path();
    }
};

using Identity = HMODULE (*)();
Identity previousIdentity = nullptr;
HMODULE ForeignIdentity() { return previousIdentity(); }

void** IdentityImport(HMODULE module) {
    auto* base = reinterpret_cast<BYTE*>(module);
    const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; imports->Name; ++imports) {
        if (_stricmp(reinterpret_cast<const char*>(base + imports->Name), "sl.interposer.dll") != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imports->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imports->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(name->Name), "flowInterposerIdentity") == 0)
                return reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    return nullptr;
}

void ForeignBindingKeepsOneResidentRuntime(bool afterArming, bool disableNgxOta = false) {
    VendorGame vendor("takeover_game.dll", "sl.interposer.dll");
    ASSERT_NE(vendor.module, nullptr);
    const HMODULE resident = GetModuleHandleW(L"sl.interposer.dll");
    ASSERT_NE(resident, nullptr);
    const auto residentFlags = reinterpret_cast<uint64_t (*)()>(GetProcAddress(resident, "flowInterposerInitFlags"));
    ASSERT_NE(residentFlags, nullptr);
    const std::string overrideDirectory = vendor.directory.string();
    CEFlowEarlyLoaderProbe probe{nullptr, overrideDirectory.c_str(), nullptr, disableNgxOta};
    std::unique_ptr<ce::flow::FlowGame> game;
    if (afterArming)
        game = std::make_unique<ce::flow::FlowGame>(ce::flow::CurrentTestName(), &probe);
    void** slot = IdentityImport(vendor.module);
    ASSERT_NE(slot, nullptr);
    previousIdentity = reinterpret_cast<Identity>(*slot);
    DWORD protection = 0;
    ASSERT_TRUE(VirtualProtect(static_cast<void*>(slot), sizeof(void*), PAGE_READWRITE, &protection));
    *slot = reinterpret_cast<void*>(&ForeignIdentity);
    DWORD ignored = 0;
    ASSERT_TRUE(VirtualProtect(static_cast<void*>(slot), sizeof(void*), protection, &ignored));
    struct RestoreImport {
        void** slot;
        ~RestoreImport() {
            DWORD protection = 0;
            if (VirtualProtect(static_cast<void*>(slot), sizeof(void*), PAGE_READWRITE, &protection)) {
                InterlockedCompareExchangePointer(slot, reinterpret_cast<void*>(previousIdentity),
                                                   reinterpret_cast<void*>(&ForeignIdentity));
                DWORD ignored = 0;
                VirtualProtect(static_cast<void*>(slot), sizeof(void*), protection, &ignored);
            }
        }
    } restore{slot};
    if (!game)
        game = std::make_unique<ce::flow::FlowGame>(ce::flow::CurrentTestName(), &probe);
    ASSERT_TRUE(game->Error().empty()) << game->Error();
    const auto init = vendor.Export<TakeoverGameInit_t>("TakeoverGameInit");
    const auto identity = vendor.Export<TakeoverGameInterposer_t>("TakeoverGameInterposer");
    ASSERT_NE(init, nullptr);
    ASSERT_NE(identity, nullptr);
    init(); // Resident has no plugins; only provider identity is relevant.
    EXPECT_EQ(identity(), resident) << "logs: " << game->LogDirectory();
    EXPECT_EQ(GetModuleHandleW((vendor.directory / "sl.interposer.dll").c_str()), nullptr)
        << "a preserved foreign predecessor must prevent initializing the override";
    if (afterArming && disableNgxOta) {
        game->ServiceHookThread();
        const auto initialize = vendor.Export<int (*)(const void*, unsigned long long)>(
            "TakeoverGameInitWithPreferences");
        ASSERT_NE(initialize, nullptr);
        sl::Preferences preferences{};
        preferences.flags = sl::PreferenceFlags::eAllowOTA | sl::PreferenceFlags::eLoadDownloadedPlugins;
        const auto originalFlags = preferences.flags;
        initialize(&preferences, sl::kSDKVersion);
        EXPECT_EQ(residentFlags(), 0u) << "resident routing must keep OTA control after worker rollback";
        EXPECT_EQ(preferences.flags, originalFlags);
    }
}

TEST(FlowStreamlineTakeover, ForeignImportOwnerBeforeArmingKeepsTheResidentRuntime) {
    ForeignBindingKeepsOneResidentRuntime(false);
}

TEST(FlowStreamlineTakeover, ForeignImportOwnerAfterArmingKeepsTheResidentRuntime) {
    ForeignBindingKeepsOneResidentRuntime(true);
}

TEST(FlowStreamlineTakeover, OtaControlStillPreservesForeignImportOwnerBeforeArming) {
    ForeignBindingKeepsOneResidentRuntime(false, true);
}

TEST(FlowStreamlineTakeover, OtaControlStillPreservesForeignImportOwnerAfterArming) {
    ForeignBindingKeepsOneResidentRuntime(true, true);
}

TEST(FlowStreamlineTakeover, EveryStaticImportMovesToTheConfiguredInterposer) {
    VendorGame vendor("takeover_game.dll", "sl.interposer.dll");
    ASSERT_NE(vendor.module, nullptr) << "LoadLibraryEx error " << GetLastError();
    const HMODULE resident = GetModuleHandleW(L"sl.interposer.dll");
    ASSERT_NE(resident, nullptr);
    ASSERT_TRUE(fs::equivalent(VendorGame::PathOf(resident), vendor.vendor / "sl.interposer.dll"));
    using Calls = uint32_t (*)();
    // Resolved before CE redirects module queries on the dormant copy.
    const auto residentCalls = reinterpret_cast<Calls>(GetProcAddress(resident, "flowInterposerSetDeviceCalls"));
    ASSERT_NE(residentCalls, nullptr);

    const std::string overrideDirectory = vendor.directory.string();
    CEFlowEarlyLoaderProbe probe{nullptr, overrideDirectory.c_str(), nullptr};
    ce::flow::FlowGame game(ce::flow::CurrentTestName(), &probe);
    ASSERT_TRUE(game.Error().empty()) << game.Error();

    const auto init = vendor.Export<TakeoverGameInit_t>("TakeoverGameInit");
    const auto interposer = vendor.Export<TakeoverGameInterposer_t>("TakeoverGameInterposer");
    ASSERT_NE(init, nullptr);
    ASSERT_NE(interposer, nullptr);

    // slInit is the entry thunk: it activates the takeover, and the configured
    // copy initializes (it finds its plugins beside it, the vendor copy has none).
    EXPECT_EQ(init(), 0) << "logs: " << game.LogDirectory();
    const HMODULE answered = interposer();
    ASSERT_NE(answered, nullptr);
    // No thunk gates flowInterposerIdentity: before the fix it stayed bound to
    // the dormant vendor copy while slInit ran in the configured one.
    EXPECT_NE(answered, resident) << "logs: " << game.LogDirectory();
    EXPECT_TRUE(fs::equivalent(VendorGame::PathOf(answered), vendor.directory / "sl.interposer.dll"))
        << VendorGame::PathOf(answered) << "; logs: " << game.LogDirectory();

    // CE's Streamline hook used to patch the game's slSetD3DDevice/slGetFeatureFunction/
    // slSetTagForFrame/slEvaluateFeature slots while the override loaded, replacing the
    // takeover's gates; publication then declined (session 20261014_203446). The call
    // must reach the configured copy, never the dormant one.
    using SetDevice = int (*)(void*);
    const auto setDevice = vendor.Export<SetDevice>("TakeoverGameSetDevice");
    const auto customCalls = reinterpret_cast<Calls>(GetProcAddress(answered, "flowInterposerSetDeviceCalls"));
    ASSERT_NE(setDevice, nullptr);
    ASSERT_NE(customCalls, nullptr);
    EXPECT_EQ(setDevice(nullptr), 0) << "logs: " << game.LogDirectory();
    EXPECT_EQ(customCalls(), 1u) << "logs: " << game.LogDirectory();
    EXPECT_EQ(residentCalls(), 0u) << "logs: " << game.LogDirectory();
}

void OtaControlPreservesTakeoverGate(bool dynamic) {
    VendorGame vendor("takeover_game.dll", "sl.interposer.dll");
    ASSERT_NE(vendor.module, nullptr);
    const HMODULE resident = GetModuleHandleW(L"sl.interposer.dll");
    ASSERT_NE(resident, nullptr);
    using Calls = uint32_t (*)();
    using Flags = uint64_t (*)();
    // Save this before CE redirects module queries to the published override.
    const auto residentCalls = reinterpret_cast<Calls>(GetProcAddress(resident, "flowInterposerInitCalls"));
    ASSERT_NE(residentCalls, nullptr);
    const std::string overrideDirectory = vendor.directory.string();
    CEFlowEarlyLoaderProbe probe{nullptr, overrideDirectory.c_str(), nullptr, true};
    ce::flow::FlowGame game(ce::flow::CurrentTestName(), &probe);
    ASSERT_TRUE(game.Error().empty()) << game.Error();
    using Init = int (*)(const void*, unsigned long long);
    const auto resolve = vendor.Export<FARPROC (*)()>("TakeoverGameResolveInit");
    ASSERT_NE(resolve, nullptr);
    const auto init = dynamic ? reinterpret_cast<Init>(resolve())
                             : vendor.Export<Init>("TakeoverGameInitWithPreferences");
    const auto identity = vendor.Export<TakeoverGameInterposer_t>("TakeoverGameInterposer");
    ASSERT_NE(init, nullptr);
    ASSERT_NE(identity, nullptr);
    // A dynamic query must leave the override unmapped until its entry is called.
    EXPECT_EQ(GetModuleHandleW((vendor.directory / "sl.interposer.dll").c_str()), nullptr);
    sl::Preferences preferences{};
    preferences.flags = sl::PreferenceFlags::eAllowOTA | sl::PreferenceFlags::eLoadDownloadedPlugins |
                        sl::PreferenceFlags::eDisableCLStateTracking;
    const uint64_t retainedFlags = static_cast<uint64_t>(sl::PreferenceFlags::eDisableCLStateTracking);
    const auto originalFlags = preferences.flags;
    EXPECT_EQ(init(&preferences, sl::kSDKVersion), 0) << "logs: " << game.LogDirectory();
    const HMODULE custom = identity();
    ASSERT_NE(custom, resident) << "logs: " << game.LogDirectory();
    EXPECT_TRUE(fs::equivalent(VendorGame::PathOf(custom), vendor.directory / "sl.interposer.dll"));
    const auto customCalls = reinterpret_cast<Calls>(GetProcAddress(custom, "flowInterposerInitCalls"));
    const auto customFlags = reinterpret_cast<Flags>(GetProcAddress(custom, "flowInterposerInitFlags"));
    ASSERT_NE(customCalls, nullptr);
    ASSERT_NE(customFlags, nullptr);
    EXPECT_EQ(residentCalls(), 0u);
    EXPECT_EQ(customCalls(), 1u);
    EXPECT_EQ(customFlags(), retainedFlags);
    EXPECT_EQ(preferences.flags, originalFlags);
    // Both cached gates and fresh lookups must keep the selected provider and OTA policy.
    const auto again = dynamic ? reinterpret_cast<Init>(resolve()) : init;
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again(&preferences, sl::kSDKVersion), 0);
    EXPECT_EQ(customCalls(), 2u);
    EXPECT_EQ(residentCalls(), 0u);
    EXPECT_EQ(customFlags(), retainedFlags);
    EXPECT_EQ(preferences.flags, originalFlags);
    // An unknown layout passes through unchanged, even on the selected runtime.
    preferences.structVersion = 0;
    EXPECT_EQ(again(&preferences, sl::kSDKVersion), 0);
    EXPECT_EQ(customCalls(), 3u);
    EXPECT_EQ(residentCalls(), 0u);
    EXPECT_EQ(customFlags(), static_cast<uint64_t>(originalFlags));
    EXPECT_EQ(preferences.flags, originalFlags);
    EXPECT_TRUE(fs::equivalent(VendorGame::PathOf(GetModuleHandleW(L"sl.common.dll")),
                               vendor.directory / "sl.common.dll"));
}

TEST(FlowStreamlineTakeover, OtaControlPreservesTheStaticTakeoverGate) {
    OtaControlPreservesTakeoverGate(false);
}

TEST(FlowStreamlineTakeover, OtaControlPreservesTheDynamicTakeoverGate) {
    OtaControlPreservesTakeoverGate(true);
}

TEST(FlowStreamlineTakeover, ImportTheConfiguredCopyLacksKeepsTheGameInterposer) {
    VendorGame vendor("takeover_game_unsupported.dll", fs::path("takeover_resident") / "sl.interposer.dll");
    ASSERT_NE(vendor.module, nullptr) << "LoadLibraryEx error " << GetLastError();
    const HMODULE resident = GetModuleHandleW(L"sl.interposer.dll");
    ASSERT_NE(resident, nullptr);

    const std::string overrideDirectory = vendor.directory.string();
    CEFlowEarlyLoaderProbe probe{nullptr, overrideDirectory.c_str(), nullptr};
    ce::flow::FlowGame game(ce::flow::CurrentTestName(), &probe);
    ASSERT_TRUE(game.Error().empty()) << game.Error();

    const auto init = vendor.Export<TakeoverGameInit_t>("TakeoverGameInit");
    const auto interposer = vendor.Export<TakeoverGameInterposer_t>("TakeoverGameInterposer");
    ASSERT_NE(init, nullptr);
    ASSERT_NE(interposer, nullptr);
    init();  // The vendor copy has no plugins beside it; only the routing matters here.
    // Declined at arming: every call, thunked or not, stays on one interposer.
    EXPECT_EQ(interposer(), resident) << "logs: " << game.LogDirectory();
    EXPECT_EQ(GetModuleHandleW((vendor.directory / "sl.interposer.dll").c_str()), nullptr)
        << "the configured copy must not be mapped beside the game's own; logs: " << game.LogDirectory();
}

}  // namespace
