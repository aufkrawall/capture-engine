// FG-composed overlay coverage inheritance (hook/d3d12/dx12_overlay_policy/coverage_inheritance.h).
//
// Talos + OptiScaler XeFG (session 20261023_183119): two physical presents per game frame on one swapchain, CE
// drew on the first (PostSL) and skipped the second (`Routing state ... skip=1`), yet the ledger counted both
// covered (`[OVERLAY HANDOFF] ... inheritIfNoDraw=1 covered=1`): the Streamline FG signal made every undrawn
// present inherit the overlay, whoever issued it.

#include <gtest/gtest.h>

#include "hook/d3d12/dx12_overlay_policy/coverage_inheritance.h"
#include "hook/d3d12/dx12_overlay_policy/protected_ffx_startup.h"

namespace {

using ce::dx12_overlay_policy::OverlayPresentCoverageTracker;
using ce::dx12_overlay_policy::ShouldInheritFGComposedCoverage;
using ce::present_originator::Originator;

TEST(OverlayCoverageInheritanceTest, OnlyTheComposingRuntimesOwnPresentsInherit) {
    EXPECT_TRUE(ShouldInheritFGComposedCoverage(Originator::kStreamline));
    EXPECT_TRUE(ShouldInheritFGComposedCoverage(Originator::kFFXFrameGeneration));
    EXPECT_FALSE(ShouldInheritFGComposedCoverage(Originator::kApplication));
    EXPECT_FALSE(ShouldInheritFGComposedCoverage(Originator::kXeFG));
    EXPECT_FALSE(ShouldInheritFGComposedCoverage(Originator::kPresentInterposer));
    // No provenance at all keeps the runtime's own inheritance rule.
    EXPECT_TRUE(ShouldInheritFGComposedCoverage(Originator::kUnresolved));
}

// Two presents per frame, one drawn and one not: exactly one uncovered present per frame, whatever FG signal
// asked for inheritance. Before: no uncovered present at all.
TEST(OverlayCoverageInheritanceTest, TwoPresentsPerFrameWithOneDrawnCountOneUncoveredPerFrame) {
    struct Present {
        Originator originator;
        bool drawn;
    };
    // The session's pair before CE knew XeFG (Streamline-looking game-thread present, application-looking XeFG
    // thread present), and the same pair once both read as XeFG's.
    const Present pairs[2][2] = {
        {{Originator::kStreamline, true}, {Originator::kApplication, false}},
        {{Originator::kXeFG, true}, {Originator::kXeFG, false}},
    };
    for (const auto& pair : pairs) {
        OverlayPresentCoverageTracker tracker;
        OverlayPresentCoverageTracker legacy;
        constexpr int kFrames = 72;
        for (int frame = 0; frame < kFrames; ++frame) {
            for (const Present& present : pair) {
                const bool requested = true;  // the Streamline FG signal and an installed PostSL callback
                const bool inherit = requested && (present.drawn || ShouldInheritFGComposedCoverage(present.originator));
                tracker.NotePresent(present.drawn, inherit);
                legacy.NotePresent(present.drawn, requested);
            }
        }
        EXPECT_EQ(tracker.TotalPresents(), 2u * kFrames);
        EXPECT_EQ(tracker.UncoveredPresents(), static_cast<uint64_t>(kFrames));
        EXPECT_EQ(tracker.LongestUncoveredStreak(), 1u);
        EXPECT_EQ(legacy.UncoveredPresents(), 0u) << "the old rule hid every undrawn present";
    }
}

// DLSS-G presents frames it built from the proxy back buffer CE drew into on the game's own Present (and its
// generated outputs between PostSL draws): they keep inheriting, so no false uncovered presents.
TEST(OverlayCoverageInheritanceTest, StreamlineOutputsOfAnApplicationDrawnFrameStayCovered) {
    OverlayPresentCoverageTracker tracker;
    for (int frame = 0; frame < 100; ++frame) {
        tracker.NotePresent(true, false);  // the game's Present into the proxy, drawn on the normal route
        tracker.NotePresent(false, ShouldInheritFGComposedCoverage(Originator::kStreamline));
        tracker.NotePresent(false, ShouldInheritFGComposedCoverage(Originator::kStreamline));
    }
    EXPECT_EQ(tracker.UncoveredPresents(), 0u);
}

}  // namespace
