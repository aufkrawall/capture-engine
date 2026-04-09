#include <gtest/gtest.h>

#include "../hook/common/ffx_api_parsing.h"

namespace {

TEST(FFXApiParsingTest, RecognizesEnabledFrameGenerationConfigure) {
    ce::ffx_api::ConfigureDescFrameGeneration desc{};
    desc.header.type = ce::ffx_api::kConfigureDescTypeFrameGeneration;
    desc.frameGenerationEnabled = true;
    desc.frameID = 42;

    const auto parsed = ce::ffx_api::ParseFrameGenerationConfigureState(&desc.header);
    EXPECT_TRUE(parsed.recognized);
    EXPECT_TRUE(parsed.enabled);
    EXPECT_EQ(parsed.frameId, 42u);
}

TEST(FFXApiParsingTest, RecognizesDisabledFrameGenerationConfigure) {
    ce::ffx_api::ConfigureDescFrameGeneration desc{};
    desc.header.type = ce::ffx_api::kConfigureDescTypeFrameGeneration;
    desc.frameGenerationEnabled = false;
    desc.frameID = 77;

    const auto parsed = ce::ffx_api::ParseFrameGenerationConfigureState(&desc.header);
    EXPECT_TRUE(parsed.recognized);
    EXPECT_FALSE(parsed.enabled);
    EXPECT_EQ(parsed.frameId, 77u);
}

TEST(FFXApiParsingTest, IgnoresNonFrameGenerationConfigure) {
    ce::ffx_api::ApiHeader desc{};
    desc.type = ce::ffx_api::MakeEffectSubId(ce::ffx_api::kEffectIdFrameGenerationSwapchain, 0x08u);

    const auto parsed = ce::ffx_api::ParseFrameGenerationConfigureState(&desc);
    EXPECT_FALSE(parsed.recognized);
    EXPECT_FALSE(parsed.enabled);
    EXPECT_EQ(parsed.frameId, 0u);
}

}  // namespace
