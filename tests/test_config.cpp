#include "../common/config.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>


class ConfigTest : public ::testing::Test {
protected:
  // Use absolute path because GetPrivateProfileString requires it or checks
  // C:\Windows
  std::string tempConfigFile;

  void SetUp() override {
    // Get absolute path
    tempConfigFile =
        (std::filesystem::current_path() / "test_config.ini").string();

    // Clean up before test
    remove(tempConfigFile.c_str());
  }

  void TearDown() override {
    // Clean up after test
    remove(tempConfigFile.c_str());
  }

  void WriteConfig(const std::string &content) {
    std::ofstream out(tempConfigFile);
    out << content;
    out.close();
  }
};

TEST_F(ConfigTest, LoadDefaultsWhenFileMissing) {
  AppConfig config;
  // Use a non-existent absolute path to force default creation logic if
  // applicable Or just ensure it doesn't find a file. LoadConfig creates a
  // default file if missing, so we should check THAT file or the loaded values.

  std::string missingFile =
      (std::filesystem::current_path() / "nonexistent.ini").string();
  remove(missingFile.c_str());

  LoadConfig(missingFile, config);

  // Default is TRUE in config.cpp
  EXPECT_TRUE(config.debugLogging);

  // Clean up the created default file
  remove(missingFile.c_str());
}

TEST_F(ConfigTest, ParseValues) {
  std::string iniContent = "[General]\n"
                           "debug_logging=true\n"
                           "capture_method=inject\n"
                           "\n"
                           "[Video]\n"
                           "encoder=av1_nvenc\n"
                           "fps=60\n"
                           "bitrate=50Mbps\n";

  WriteConfig(iniContent);

  AppConfig config;
  LoadConfig(tempConfigFile, config);

  EXPECT_TRUE(config.debugLogging);
  EXPECT_EQ(config.captureMethod, "inject");
  EXPECT_EQ(config.video.encoder, "av1_nvenc");
  EXPECT_EQ(config.video.fps, 60);
  EXPECT_EQ(config.video.bitrate, "50Mbps");
}

TEST_F(ConfigTest, WhitelistParsing) {
  std::string iniContent = "[Injection]\n"
                           "whitelist=(\n"
                           "game1.exe,game2.exe\n"
                           "game3.exe\n"
                           "\"Game With Spaces.exe\"\n"
                           ")\n"
                           "\n"
                           "[Overlay]\n"
                           "enabled=true\n";

  WriteConfig(iniContent);

  AppConfig config;
  LoadConfig(tempConfigFile, config);

  ASSERT_EQ(config.gameWhitelist.size(), 4);
  EXPECT_EQ(config.gameWhitelist[0], "game1.exe");
  EXPECT_EQ(config.gameWhitelist[1], "game2.exe");
  EXPECT_EQ(config.gameWhitelist[2], "game3.exe");
  EXPECT_EQ(config.gameWhitelist[3], "Game With Spaces.exe");
}

TEST_F(ConfigTest, InvalidValuesFallBack) {
  // Test robustness if needed
}
