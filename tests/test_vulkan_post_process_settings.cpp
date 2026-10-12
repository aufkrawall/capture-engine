#include <gtest/gtest.h>
#include "hook/vulkan_layer/vulkan_post_process_settings.h"
#include "source_fragment_reader.h"

TEST(VulkanPostProcessSettingsTest, GammaOnlyLiveReloadAndVerifiedNativeSourceReachTheRequest) {
    VulkanPostProcessSettings settings;
    EXPECT_FALSE(ce::gamma::Requested(settings.Request().gamma));
    settings.Store(0, 0, 0, 0, 2, 1, false);
    auto request = settings.Request();
    EXPECT_TRUE(ce::gamma::Requested(request.gamma));
    EXPECT_EQ(request.mode, ce::sharpen::Mode::Off);
    EXPECT_FLOAT_EQ(request.gamma.source, 2.2f);
    EXPECT_FLOAT_EQ(request.gamma.destination, 2.4f);
    ce::gamma::PublishVerifiedNativeCurve(0.0f);
    settings.Store(0, 0, 0, 0, 2, 1, true);
    EXPECT_FLOAT_EQ(settings.Request().gamma.source, 0.0f);
    settings.Store(0, 0, 0, 0, 0, 3, false);
    EXPECT_FALSE(ce::gamma::Requested(settings.Request().gamma));
    ce::gamma::PublishVerifiedNativeCurve(-1.0f);
}

TEST(VulkanPostProcessSettingsTest, ConnectAndEveryPresentPublishBothCurvesAndNativeOverrideIntent) {
    for (const char* file : {"vulkan_layer_state.cpp", "layer_sharpen.cpp"}) {
        const auto source = ce::test_source::ReadFile(ce::test_source::FindSource("hook", file));
        EXPECT_NE(source.find("cfg.postProcessDisplayGamma"), std::string::npos);
        EXPECT_NE(source.find("cfg.postProcessGammaSource"), std::string::npos);
        EXPECT_NE(source.find("cfg.displayGamma"), std::string::npos);
    }
}
