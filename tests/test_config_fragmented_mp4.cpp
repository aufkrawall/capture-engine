#include "test_config_shared.h"

// [Output] fragmented_mp4 (libcengine 11, part 3): off by default, because a fragmented file cannot state its exact
// end; on writes mp4/mov in fragments of about a second so an interrupted recording stays repairable.
TEST_F(ConfigTest, FragmentedMp4IsOffByDefault) {
    WriteConfig("[Output]\ncontainer=mp4\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config);
    EXPECT_FALSE(config.video.fragmentedMp4);
}

TEST_F(ConfigTest, FragmentedMp4ParsesAsABool) {
    AppConfig config;
    WriteConfig("[Output]\nfragmented_mp4=true\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_TRUE(config.video.fragmentedMp4);
    WriteConfig("[Output]\nfragmented_mp4=off\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_FALSE(config.video.fragmentedMp4);
}
