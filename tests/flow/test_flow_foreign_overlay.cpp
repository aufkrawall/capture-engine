// A third-party overlay that loads into a scenario process changes the Present path the scenarios check, so the run is
// not a result about CE. The harness must name the overlay at the scenario's end, not let the numbers pass or fail
// unexplained. The stand-in is a module named like RivaTuner's hook (fakes/overlay/third_party_overlay_stand_in.cpp).

#include <gtest/gtest-spi.h>

#include <filesystem>

#include "common/platform/raii_helpers.h"
#include "tests/flow/flow_test_support.h"

namespace {

using ce::flow::CurrentTestName;

// The stand-in loads after the hook's loader notification is registered (FlowGame's CEFlow_Init), as an injected
// overlay does, and it is unloaded before the scenario ends: the report must survive the unload.
void RunScenarioWithAStandInOverlay() {
    ce::flow::FlowGame game(CurrentTestName());
    const HMODULE overlay = LoadLibraryA("third_party\\RTSSHooks64.dll");
    ASSERT_NE(overlay, nullptr) << "the stand-in overlay is built next to fg_flow_tests.exe";
    FreeLibrary(overlay);
}

// A second copy of the CaptureEngine hook loads from another folder, as a live CaptureEngine's injection would.
void RunScenarioWithAForeignCaptureHook() {
    ce::flow::FlowGame game(CurrentTestName());
    const HMODULE hook = LoadLibraryA("third_party\\capture_hook_x64.dll");
    ASSERT_NE(hook, nullptr) << "the stand-in hook is built next to fg_flow_tests.exe";
    FreeLibrary(hook);
}

// A stand-in the scenario copies into its own log folder is part of the scenario (the late-overlay scenarios play an
// overlay that way), so the scenario's end reports nothing: the destructor's report would fail this test.
void RunScenarioWithAStandInInItsOwnFolder() {
    ce::flow::FlowGame game(CurrentTestName());
    const auto path = std::filesystem::path(game.LogDirectory()) / "RTSSHooks64.dll";
    ASSERT_TRUE(std::filesystem::copy_file("third_party\\RTSSHooks64.dll", path));
    const auto removeFile = ce::make_scope_guard([&] { std::filesystem::remove(path); });
    const HMODULE overlay = LoadLibraryW(path.c_str());
    ASSERT_NE(overlay, nullptr);
    FreeLibrary(overlay);
}

TEST(FlowForeignOverlay, AThirdPartyOverlayLoadedDuringAScenarioIsReportedByItsName) {
    EXPECT_NONFATAL_FAILURE(RunScenarioWithAStandInOverlay(), "RTSSHooks64.dll");
}

TEST(FlowForeignOverlay, AForeignCaptureEngineHookIsReportedByItsName) {
    EXPECT_NONFATAL_FAILURE(RunScenarioWithAForeignCaptureHook(), "capture_hook_x64.dll");
}

TEST(FlowForeignOverlay, AStandInInTheScenarioFolderIsNotAForeignOverlay) {
    RunScenarioWithAStandInInItsOwnFolder();
}

}  // namespace
