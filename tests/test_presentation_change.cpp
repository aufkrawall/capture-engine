#include <gtest/gtest.h>

#include <string>

#include "hook/present/presentation_change.h"
#include "source_fragment_reader.h"

using ce::present_stall::FormatSlowOverlaySubmit;
using ce::present_stall::PresentationChangeWatch;

// A sampled call suspends the game's thread for a moment, so sampling must stop on its own soon after a change.
TEST(PresentationChangeWatchTest, ACallIsSampledOnlyInTheWindowAfterAChange) {
    PresentationChangeWatch watch;
    EXPECT_FALSE(watch.TakeCall()) << "no change yet";
    EXPECT_EQ(watch.Why(), nullptr);

    watch.Note("swapchain handoff", 1234);
    EXPECT_STREQ(watch.Why(), "swapchain handoff");
    EXPECT_EQ(watch.AtTicks(), 1234);
    for (uint32_t call = 0; call < PresentationChangeWatch::kCalls; ++call) {
        EXPECT_TRUE(watch.TakeCall()) << call;
    }
    EXPECT_FALSE(watch.TakeCall()) << "the window ended";

    watch.Note("foreground regained", 5678);
    EXPECT_TRUE(watch.TakeCall()) << "a new change opens a new window";
    EXPECT_STREQ(watch.Why(), "foreground regained");
}

TEST(PresentationChangeWatchTest, ASlowSubmitNamesItselfAndTheChangeBeforeIt) {
    char line[256];
    FormatSlowOverlaySubmit(21.884, "normal overlay submit", true, "swapchain handoff", 4.25, line, sizeof(line));
    EXPECT_STREQ(line,
                 "[OVERLAY SUBMIT] slow 21.88ms reason=normal overlay submit stackArmed=1 "
                 "lastPresentationChange=swapchain handoff 4.2ms earlier");
    FormatSlowOverlaySubmit(3.0, nullptr, false, nullptr, 0.0, line, sizeof(line));
    EXPECT_STREQ(line, "[OVERLAY SUBMIT] slow 3.00ms reason=? stackArmed=0 lastPresentationChange=none");
}

// The stall needs a real driver and a real flip, which neither flow scenarios nor unit tests have, so the wiring is a
// source contract: every CE submit is timed, both kinds of change open the window, and the down-call shares it, because
// the stall also hits DXGI's own submit inside Present while CE submits nothing (20261104_210842).
TEST(PresentationChangeWatchTest, EveryOverlaySubmitAndBothChangesAreWired) {
    const std::string types = ce::test_source::ReadFile(ce::test_source::FindSource("hook", "dx12_hook_types_impl.cpp"));
    EXPECT_NE(types.find("submitProbe_(reason)"), std::string::npos);
    const std::string coverage =
        ce::test_source::ReadFile(ce::test_source::FindSource("hook", "dx12_hook_overlay_coverage.cpp"));
    const size_t handoff = coverage.find("if (event.handoff) {");
    ASSERT_NE(handoff, std::string::npos);
    EXPECT_NE(coverage.find("NotePresentationChange(\"swapchain handoff\");", handoff), std::string::npos);
    const std::string present =
        ce::test_source::ReadFile(ce::test_source::FindSource("hook", "dx12_hook_overlay_present.cpp"));
    EXPECT_NE(present.find("NotePresentationChange(fgState ? \"foreground regained\" : \"foreground lost\");"),
              std::string::npos);
    const std::string stall =
        ce::test_source::ReadFile(ce::test_source::FindSource("hook", "present_stall_attribution.cpp"));
    const size_t probe = stall.find("DownCallProbe::DownCallProbe(bool d3d12)");
    ASSERT_NE(probe, std::string::npos);
    EXPECT_NE(stall.find("if (entry_.valid || PresentationChange().TakeCall()) {", probe), std::string::npos);
}
