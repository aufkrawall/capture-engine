#include <gtest/gtest.h>

#include <cstring>

#include "hook/vulkan_layer/vulkan_presentation_path_policy.h"

namespace {

using namespace ce::vulkan_presentation_path;

// A borderless window covering a 4K monitor at 150 % scaling, as DXVK creates
// it for a fullscreen D3D9 title (Mirror's Edge, session 20261018_155920).
WindowFacts BorderlessFullscreen() {
    WindowFacts facts;
    facts.windowKnown = true;
    facts.window = {0, 0, 3840, 2160};
    facts.monitor = {0, 0, 3840, 2160};
    facts.clientWidth = 3840;
    facts.clientHeight = 2160;
    facts.swapchainWidth = 3840;
    facts.swapchainHeight = 2160;
    facts.windowDpi = 144;
    facts.monitorDpi = 144;
    facts.awareness = DpiAwareness::kPerMonitorAware;
    return facts;
}

std::string Describe(const WindowFacts& facts) {
    char text[128] = {};
    DescribeBlockers(IndependentFlipBlockers(facts), text, sizeof(text));
    return text;
}

}  // namespace

TEST(VulkanPresentationPathPolicyTest, CoveringPerMonitorAwareWindowHasNoBlocker) {
    EXPECT_EQ(IndependentFlipBlockers(BorderlessFullscreen()), static_cast<uint32_t>(kBlockerNone));
    EXPECT_EQ(Describe(BorderlessFullscreen()), "none");
}

TEST(VulkanPresentationPathPolicyTest, UnknownWindowSaysSoInsteadOfGuessing) {
    WindowFacts facts;
    EXPECT_EQ(IndependentFlipBlockers(facts), static_cast<uint32_t>(kBlockerWindowUnknown));
    EXPECT_EQ(Describe(facts), "window-unknown");
}

TEST(VulkanPresentationPathPolicyTest, DpiVirtualizedWindowIsStretchedByTheCompositor) {
    WindowFacts facts = BorderlessFullscreen();
    facts.awareness = DpiAwareness::kUnaware;
    facts.windowDpi = 96;
    EXPECT_EQ(IndependentFlipBlockers(facts), static_cast<uint32_t>(kBlockerDpiVirtualized));
    // The same unaware window on a 100 % monitor is not virtualized.
    facts.monitorDpi = 96;
    EXPECT_EQ(IndependentFlipBlockers(facts), static_cast<uint32_t>(kBlockerNone));
    // An unknown monitor DPI is no evidence either way.
    facts.monitorDpi = 0;
    EXPECT_EQ(IndependentFlipBlockers(facts), static_cast<uint32_t>(kBlockerNone));
}

TEST(VulkanPresentationPathPolicyTest, GeometryAndStyleBlockersAreAllNamed) {
    WindowFacts facts = BorderlessFullscreen();
    facts.window = {0, 0, 2560, 1440};
    facts.clientWidth = 2560;
    facts.clientHeight = 1440;
    facts.layered = true;
    facts.minimized = true;
    EXPECT_EQ(IndependentFlipBlockers(facts), static_cast<uint32_t>(kBlockerMinimized | kBlockerNotCoveringMonitor |
                                                                     kBlockerExtentMismatch | kBlockerLayeredWindow));
    EXPECT_EQ(Describe(facts), "minimized|not-covering-monitor|extent!=client|layered-window");
}

TEST(VulkanPresentationPathPolicyTest, DescriptionNeverOverrunsItsBuffer) {
    WindowFacts facts = BorderlessFullscreen();
    facts.window = {0, 0, 1, 1};
    facts.layered = true;
    char text[12];
    std::memset(text, 'x', sizeof(text));
    const size_t written = DescribeBlockers(IndependentFlipBlockers(facts), text, sizeof(text));
    EXPECT_LT(written, sizeof(text));
    EXPECT_EQ(std::strlen(text), written);
    char tiny[3];
    DescribeBlockers(kBlockerNone, tiny, sizeof(tiny));
    EXPECT_EQ(std::strlen(tiny), 2u);
    EXPECT_EQ(DescribeBlockers(kBlockerNone, nullptr, 8), 0u);
}

TEST(VulkanPresentationPathPolicyTest, SignatureChangesWithEveryJudgedFact) {
    const WindowFacts base = BorderlessFullscreen();
    const uint32_t baseSignature = FactsSignature(base);
    EXPECT_NE(baseSignature, 0u);
    EXPECT_EQ(FactsSignature(BorderlessFullscreen()), baseSignature);

    WindowFacts moved = base;
    moved.window.left = 1;
    EXPECT_NE(FactsSignature(moved), baseSignature);
    WindowFacts resized = base;
    resized.swapchainWidth = 2560;
    EXPECT_NE(FactsSignature(resized), baseSignature);
    WindowFacts layered = base;
    layered.layered = true;
    EXPECT_NE(FactsSignature(layered), baseSignature);
    WindowFacts unaware = base;
    unaware.awareness = DpiAwareness::kUnaware;
    EXPECT_NE(FactsSignature(unaware), baseSignature);
    EXPECT_NE(FactsSignature(WindowFacts{}), 0u);
}
