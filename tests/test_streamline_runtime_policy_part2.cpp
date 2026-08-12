#include <gtest/gtest.h>

#include <filesystem>

#include "../common/config.h"
#include "../hook/common/dx12_overlay_policy.h"
#include "../hook/common/streamline_runtime_policy.h"
#include "source_fragment_reader.h"

namespace {

TEST(StreamlineRuntimePolicyTest, StreamlineModuleUnloadDispatchesHookInvalidation) {
    // Crash 20260612_003407: the game unloads/reloads the whole SL stack when
    // toggling DLSS FG; every sl.* unload must invalidate stale hook slots.
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload("sl.interposer.dll"));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload("sl.common.dll"));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload("sl.dlss_g.dll"));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload("SL.COMMON.DLL"));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload("slang.dll"));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload("common.dll"));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload("dxgi.dll"));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload(nullptr));
}

// Regression (session 20260811_230524): late injection into an already-running
// DLSS-FG game never hooks slDLSSGSetOptions because the game resolved the
// feature function before injection and never re-resolves it. The loaded-module
// scan must proactively resolve the DLSS-G/Reflex feature functions through
// the interposer after hooking the loaded modules, so the game's cached
// slDLSSGSetOptions pointer flows through CE (and the FG multiplier / Reflex
// state signals become observable). Without this, Talos's 4x MFG
// (slDLSSGSetOptions numFramesToGenerate=3) stays invisible and the overlay
// reports DLSS 2x.
TEST(StreamlineRuntimePolicyTest, LoadedModuleScanResolvesFeatureHooksAfterHookingModules) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "streamline_hook_install.cpp";
    const fs::path headerSource = fs::current_path() / "hook" / "apis" / "streamline_hook_internal.h";
    ASSERT_TRUE(fs::exists(source));
    ASSERT_TRUE(fs::exists(headerSource));

    const std::string text = ce::test_source::ReadLogicalSource(source);
    const std::string headerText = ce::test_source::ReadLogicalSource(headerSource);
    ASSERT_FALSE(text.empty());
    ASSERT_FALSE(headerText.empty());

    const size_t scanStart = text.find("bool ScanLoadedStreamlineModules(bool pinFeatureResolution)");
    ASSERT_NE(scanStart, std::string::npos);
    const size_t snapshotClose = text.find("CloseHandle(snapshot);", scanStart);
    ASSERT_NE(snapshotClose, std::string::npos);
    const size_t dlssgResolve = text.find("TryResolveDLSSGFeatureHooks(pinFeatureResolution)", snapshotClose);
    const size_t reflexResolve = text.find("TryResolveReflexFeatureHooks(pinFeatureResolution)", snapshotClose);
    ASSERT_NE(dlssgResolve, std::string::npos);
    ASSERT_NE(reflexResolve, std::string::npos);
    // Resolution must run after the module snapshot is released (runtime
    // stable, no loader lock) and inside the scan so late inject and the
    // runtime retry path both benefit.
    EXPECT_LT(snapshotClose, dlssgResolve);
    EXPECT_LT(snapshotClose, reflexResolve);
    EXPECT_NE(text.find("Resolved feature hooks after loaded-module scan", scanStart), std::string::npos);
    // Reflex SetConstants can be genuinely absent from a sl.reflex build; the
    // retry loop must bound the failed queries instead of re-scanning forever
    // (session 20260811_231851 logged endless 2.5s rescans).
    EXPECT_NE(headerText.find("kReflexSetConstantsUnavailableQueryLimit"), std::string::npos);
    EXPECT_NE(headerText.find("streamline_hook_g_ReflexSetConstantsUnavailableQueries"), std::string::npos);
}

// Crash 20260812_042259 (dx12_fg_switch_test via Steam + RTSS): switching DLSS FG -> FSR FG
// unloads sl.dlss_g / sl.reflex BEFORE sl.interposer, and the HookThread's proactive feature
// resolution called sl.interposer!slGetFeatureFunction which dispatched into the already-unmapped
// sl.dlss_g -> DEP execute violation. The proactive scan must pin the queried modules and fail
// closed when a tracked sl.* unload is in flight; runtime-internal callers (which may run under
// the loader lock during SL DllMain) must not load libraries and only skip when the feature
// module is already gone.
TEST(StreamlineRuntimePolicyTest, FeatureResolutionSkipsStreamlineTeardownRace) {
    namespace fs = std::filesystem;
    const fs::path installSource = fs::current_path() / "hook" / "apis" / "streamline_hook_install.cpp";
    const fs::path hookSource = fs::current_path() / "hook" / "apis" / "streamline_hook.cpp";
    const fs::path resolveSource = fs::current_path() / "hook" / "apis" / "streamline_hook_resolve.cpp";
    const fs::path headerSource = fs::current_path() / "hook" / "apis" / "streamline_hook_internal.h";
    ASSERT_TRUE(fs::exists(installSource));
    ASSERT_TRUE(fs::exists(hookSource));
    ASSERT_TRUE(fs::exists(resolveSource));
    ASSERT_TRUE(fs::exists(headerSource));

    const std::string install = ce::test_source::ReadLogicalSource(installSource);
    const std::string hook = ce::test_source::ReadLogicalSource(hookSource);
    const std::string resolve = ce::test_source::ReadLogicalSource(resolveSource);
    const std::string header = ce::test_source::ReadLogicalSource(headerSource);
    ASSERT_FALSE(install.empty());
    ASSERT_FALSE(hook.empty());
    ASSERT_FALSE(resolve.empty());
    ASSERT_FALSE(header.empty());

    // Every tracked sl.* unload bumps a teardown generation that the resolve path snapshots.
    EXPECT_NE(header.find("streamline_hook_g_StreamlineModuleUnloadGeneration"), std::string::npos);
    EXPECT_NE(hook.find("streamline_hook_g_StreamlineModuleUnloadGeneration.fetch_add(1"), std::string::npos);

    // The HookThread's Init scan pins the queried modules; the runtime-activity retry path must
    // not (it can run under the loader lock where LoadLibrary is forbidden).
    EXPECT_NE(hook.find("ScanLoadedStreamlineModules(/*pinFeatureResolution=*/true)"), std::string::npos);
    EXPECT_NE(install.find("const bool foundModule = ScanLoadedStreamlineModules();"), std::string::npos);

    // The query guard pins both the feature plugin and the interposer via their full paths and
    // rejects the query when the generation changed between the liveness check and the pins.
    EXPECT_NE(resolve.find("class ScopedStreamlineFeatureQueryGuard"), std::string::npos);
    EXPECT_NE(resolve.find("PinLoadedStreamlineModule"), std::string::npos);
    EXPECT_NE(resolve.find("LoadLibraryA(modulePath)"), std::string::npos);
    EXPECT_NE(resolve.find("GetModuleHandleA(\"sl.interposer.dll\")"), std::string::npos);
    EXPECT_NE(resolve.find("GetModuleHandleA(\"sl.dlss_g.dll\")"), std::string::npos);
    EXPECT_NE(resolve.find("GetModuleHandleA(\"sl.reflex.dll\")"), std::string::npos);
    EXPECT_NE(resolve.find("streamline_hook_g_StreamlineModuleUnloadGeneration.load("), std::string::npos);

    // Non-proactive callers (runtime-internal, DllMain-safe) only check module presence.
    EXPECT_NE(resolve.find("bool TryResolveDLSSGFeatureHooks(bool proactiveScan)"), std::string::npos);
    EXPECT_NE(resolve.find("bool TryResolveReflexFeatureHooks(bool proactiveScan)"), std::string::npos);
    EXPECT_NE(resolve.find("} else if (!GetModuleHandleA(\"sl.dlss_g.dll\"))"), std::string::npos);
    EXPECT_NE(resolve.find("} else if (!GetModuleHandleA(\"sl.reflex.dll\"))"), std::string::npos);
}

TEST(StreamlineRuntimePolicyTest, HookSlotInvalidationMatchesUnloadedImageRange) {
    alignas(16) static unsigned char image[0x100];
    alignas(16) static unsigned char outsideImage[0x100];
    const void* base = image;
    const size_t size = sizeof(image);
    void* inRange = image + 0x40;
    void* pastEnd = image + sizeof(image);
    void* outside = outsideImage + 0x40;

    // Patched target inside the departing image.
    EXPECT_TRUE(
        ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(inRange, nullptr, base, size));
    // Saved original/export inside the departing image (import-fallback slots).
    EXPECT_TRUE(
        ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(nullptr, inRange, base, size));

    EXPECT_FALSE(
        ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(outside, pastEnd, base, size));
    EXPECT_FALSE(
        ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(nullptr, nullptr, base, size));
    EXPECT_FALSE(
        ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(inRange, inRange, nullptr, size));
    EXPECT_FALSE(
        ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(inRange, inRange, base, 0));
}

TEST(StreamlineRuntimePolicyTest, ReloadedCoreModuleMaskIsStaleWhenNoTargetBelongsToArrivingInstance) {
    EXPECT_TRUE(ce::streamline_runtime_policy::IsInstalledStreamlineModuleMaskStaleForReloadedModule(true, false));

    EXPECT_FALSE(ce::streamline_runtime_policy::IsInstalledStreamlineModuleMaskStaleForReloadedModule(true, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::IsInstalledStreamlineModuleMaskStaleForReloadedModule(false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::IsInstalledStreamlineModuleMaskStaleForReloadedModule(false, true));
}

// ---------------------------------------------------------------------------
// DLSSG activation-health monitor (session 20260702_094955): GTA cold-start DLSS FG reported ON
// (optionsMode=on, updateActive=1) but presents stayed at base rate all session — the health monitor must
// warn deterministically (sample streaks, not wall-clock) and only for ON-request samples so an OFF phase
// never extends or misattributes a streak.
// ---------------------------------------------------------------------------
TEST(StreamlineRuntimePolicyTest, DLSSGHealthTracksOnlySuccessfulOnRequestSamples) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldTrackDLSSGActivationHealthSample(true, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldTrackDLSSGActivationHealthSample(true, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldTrackDLSSGActivationHealthSample(false, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldTrackDLSSGActivationHealthSample(false, false));
}

TEST(StreamlineRuntimePolicyTest, DLSSGInterpolationEvidenceRequiresGeneratedFramePresented) {
    // ==1: only the real frame reached presentation (the failing GTA session's steady value).
    EXPECT_FALSE(ce::streamline_runtime_policy::IsDLSSGInterpolationPresentEvidence(0));
    EXPECT_FALSE(ce::streamline_runtime_policy::IsDLSSGInterpolationPresentEvidence(1));
    // >=2: real + generated frames presented (2x FG); MFG presents more.
    EXPECT_TRUE(ce::streamline_runtime_policy::IsDLSSGInterpolationPresentEvidence(2));
    EXPECT_TRUE(ce::streamline_runtime_policy::IsDLSSGInterpolationPresentEvidence(4));
}

TEST(StreamlineRuntimePolicyTest, DLSSGHealthWarnsAtStreakThenRepeatsSparsely) {
    using ce::streamline_runtime_policy::ShouldWarnDLSSGActiveButNotInterpolating;
    // Below the streak threshold: silent.
    EXPECT_FALSE(ShouldWarnDLSSGActiveButNotInterpolating(0, 8, 512));
    EXPECT_FALSE(ShouldWarnDLSSGActiveButNotInterpolating(7, 8, 512));
    // First warning exactly at the threshold.
    EXPECT_TRUE(ShouldWarnDLSSGActiveButNotInterpolating(8, 8, 512));
    // Then sparse repeats at the repeat cadence, silent in between.
    EXPECT_FALSE(ShouldWarnDLSSGActiveButNotInterpolating(9, 8, 512));
    EXPECT_FALSE(ShouldWarnDLSSGActiveButNotInterpolating(519, 8, 512));
    EXPECT_TRUE(ShouldWarnDLSSGActiveButNotInterpolating(520, 8, 512));
    EXPECT_TRUE(ShouldWarnDLSSGActiveButNotInterpolating(1032, 8, 512));
    // Degenerate configs never warn / never divide by zero.
    EXPECT_FALSE(ShouldWarnDLSSGActiveButNotInterpolating(100, 0, 512));
    EXPECT_FALSE(ShouldWarnDLSSGActiveButNotInterpolating(100, 8, 0));
    EXPECT_TRUE(ShouldWarnDLSSGActiveButNotInterpolating(8, 8, 0));
}


}  // namespace
