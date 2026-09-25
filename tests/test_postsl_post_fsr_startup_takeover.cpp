#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../hook/common/dx12_overlay_policy.h"

#include "source_fragment_reader.h"

namespace {

namespace policy = ce::dx12_overlay_policy;

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

// Session 20260925_045043 (switch spam): after FSR, each DLSS-G startup carried the overlay only
// through the official UI tag (generated frames). Real frames lacked it until PostSL activated, and
// activation waited for ProcessFrame to go dormant - which the eager present-time call kept from
// happening - so a short DLSS phase ended on a real frame without the overlay.
TEST(PostSLPostFSRStartupTakeoverTest, ProofRequiresEveryExplicitPostFSRCondition) {
    EXPECT_TRUE(policy::HasExplicitPostFSRSafeBootstrapStartupProof(true, true, true, true, true));
    // No FSR history: the pure-DLSS cold-start proof owns that case.
    EXPECT_FALSE(policy::HasExplicitPostFSRSafeBootstrapStartupProof(false, true, true, true, true));
    // GetState-only activation (the GTA startup-churn family) never qualifies.
    EXPECT_FALSE(policy::HasExplicitPostFSRSafeBootstrapStartupProof(true, false, true, true, true));
    // No proven safe post-FSR bootstrap path.
    EXPECT_FALSE(policy::HasExplicitPostFSRSafeBootstrapStartupProof(true, true, false, true, true));
    // The callback is not proven to present the retained startup handoff chain.
    EXPECT_FALSE(policy::HasExplicitPostFSRSafeBootstrapStartupProof(true, true, true, false, true));
    EXPECT_FALSE(policy::HasExplicitPostFSRSafeBootstrapStartupProof(true, true, true, true, false));
}

TEST(PostSLPostFSRStartupTakeoverTest, ProofAdvancesStartupWithoutProcessFrameDormancy) {
    // processFrameRecentlySeen=true is the switch-spam state that used to block activation.
    EXPECT_FALSE(policy::ShouldSyntheticPostSLAdvanceDormantStartup(true, true, false, true, false, false, false));
    EXPECT_TRUE(policy::ShouldSyntheticPostSLAdvanceDormantStartup(true, true, false, true, false, false, true));
    // The proof never activates without a pending startup, a running FG signal, or once already active.
    EXPECT_FALSE(policy::ShouldSyntheticPostSLAdvanceDormantStartup(false, true, false, true, false, false, true));
    EXPECT_FALSE(policy::ShouldSyntheticPostSLAdvanceDormantStartup(true, false, false, true, false, false, true));
    EXPECT_FALSE(policy::ShouldSyntheticPostSLAdvanceDormantStartup(true, true, true, true, false, false, true));
}

// The post-activation gates must already admit the explicit post-FSR path, or the takeover would
// activate PostSL and then skip the very outputs it was meant to draw.
TEST(PostSLPostFSRStartupTakeoverTest, SafePostFSRPathSkipsCountdownAndWarmup) {
    EXPECT_FALSE(policy::ShouldDelaySyntheticPostSLActivationBehindRepeatedCallbacks(true, true));
    EXPECT_FALSE(policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(true, false, false, false, true));
    EXPECT_TRUE(policy::ShouldBypassPostSLReactivationWarmup(true, false, true));
}

TEST(PostSLPostFSRStartupTakeoverTest, EntryPassesTheProofAndKeepsMakeBeforeBreak) {
    const std::string entry = ReadSource("hook/apis/dx12_hook_postsl_render_entry.cpp");
    ASSERT_FALSE(entry.empty());

    const size_t proof = entry.find("ce::dx12_overlay_policy::HasExplicitPostFSRSafeBootstrapStartupProof(");
    ASSERT_NE(proof, std::string::npos);
    EXPECT_NE(entry.find("HookHasExplicitStreamlineSetOptionsActivation(), safePostFSRBootstrapPathForPostSL", proof),
              std::string::npos);

    const size_t advance = entry.find("ShouldSyntheticPostSLAdvanceDormantStartup(");
    ASSERT_NE(advance, std::string::npos);
    const size_t advanceArgs = entry.find("explicitPostFSRSafeBootstrapStartupProof)", advance);
    ASSERT_NE(advanceArgs, std::string::npos);
    EXPECT_LT(proof, advance);

    // A normal-route draw already pending for this present keeps the first PostSL draw on the next one.
    EXPECT_NE(entry.find("(immediateSameQueueStartupTakeover || immediatePostFSRExplicitStartupTakeover) && "
                         "normalRouteDrawPendingAtEntry"),
              std::string::npos);
    EXPECT_NE(entry.find("PostSL post-FSR explicit startup takeover"), std::string::npos);
}

}  // namespace
