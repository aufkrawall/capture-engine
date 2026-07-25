#include <gtest/gtest.h>

#include "../common/config.h"
#include "../hook/common/dx12_overlay_policy.h"
#include "../hook/common/streamline_runtime_policy.h"

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
