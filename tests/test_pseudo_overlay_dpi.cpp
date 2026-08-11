#include <gtest/gtest.h>

#include "../common/pseudo_overlay_dpi_policy.h"

namespace podp = ce::pseudo_overlay;

// Regression: the pseudo-overlay's font/circle scale must follow the anchor monitor's
// effective DPI, never the anchor window's awareness-dependent GetDpiForWindow() value.
// GetDpiForWindow() reports 96 for DPI-unaware apps and the system DPI for
// system-DPI-aware apps even when they sit on a 150% monitor, which previously made the
// overlay text change size on the same monitor whenever the foreground app switched.
TEST(PseudoOverlayDpiPolicyTest, MonitorDpiAlwaysWins) {
    // 150% monitor, unrelated system DPI: the monitor value is the physical truth.
    EXPECT_EQ(podp::ResolveOverlayDpi(/*monitor*/ 144, /*system*/ 96), 144u);
    // Mixed-DPI desktop: secondary monitor at 100% while the primary (system) runs 150%.
    EXPECT_EQ(podp::ResolveOverlayDpi(/*monitor*/ 96, /*system*/ 144), 96u);
    // 200% monitor.
    EXPECT_EQ(podp::ResolveOverlayDpi(/*monitor*/ 192, /*system*/ 96), 192u);
}

TEST(PseudoOverlayDpiPolicyTest, FallsBackToSystemDpiWithoutMonitorDpi) {
    // Transiently failing monitor query (destroyed monitor, API unavailable).
    EXPECT_EQ(podp::ResolveOverlayDpi(/*monitor*/ 0, /*system*/ 120), 120u);
    EXPECT_EQ(podp::ResolveOverlayDpi(/*monitor*/ 0, /*system*/ 96), 96u);
}

TEST(PseudoOverlayDpiPolicyTest, NeverResolvesToZero) {
    // Both sources unavailable -> classic 100% scale.
    EXPECT_EQ(podp::ResolveOverlayDpi(/*monitor*/ 0, /*system*/ 0), 96u);
}
