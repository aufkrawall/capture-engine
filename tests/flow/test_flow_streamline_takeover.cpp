// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "tests/flow/flow_test_support.h"

#include <filesystem>
#include <string>

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

TEST(FlowStreamlineTakeover, EveryStaticImportMovesToTheConfiguredInterposer) {
    VendorGame vendor("takeover_game.dll", "sl.interposer.dll");
    ASSERT_NE(vendor.module, nullptr) << "LoadLibraryEx error " << GetLastError();
    const HMODULE resident = GetModuleHandleW(L"sl.interposer.dll");
    ASSERT_NE(resident, nullptr);
    ASSERT_TRUE(fs::equivalent(VendorGame::PathOf(resident), vendor.vendor / "sl.interposer.dll"));

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
