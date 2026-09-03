#include "test_config_shared.h"

TEST_F(ConfigTest, BenchmarkHotkeyParsesDefaultFromTemplate) {
    AppConfig config;
    LoadConfig(DefaultTemplatePath(), config);

    EXPECT_EQ(config.hotkeyBenchmark.vkey, '7');
    EXPECT_TRUE(config.hotkeyBenchmark.ctrl);
    EXPECT_FALSE(config.hotkeyBenchmark.shift);
    EXPECT_FALSE(config.hotkeyBenchmark.alt);
    EXPECT_FALSE(config.hotkeyBenchmark.win);
}

TEST_F(ConfigTest, BenchmarkSectionParsesFromConfig) {
    std::string iniContent =
        "[Hotkeys]\n"
        "benchmark=CTRL+SHIFT+B\n"
        "[Benchmark]\n"
        "start_delay_seconds=5\n"
        "duration_seconds=60\n"
        "output_dir=C:\\Benchmarks\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.hotkeyBenchmark.vkey, 'B');
    EXPECT_TRUE(config.hotkeyBenchmark.ctrl);
    EXPECT_TRUE(config.hotkeyBenchmark.shift);
    EXPECT_FALSE(config.hotkeyBenchmark.alt);

    EXPECT_EQ(config.benchmark.startDelaySeconds, 5u);
    EXPECT_EQ(config.benchmark.durationSeconds, 60u);
    EXPECT_EQ(config.benchmark.outputDir, "C:\\Benchmarks");
}

TEST_F(ConfigTest, BenchmarkResetsOutOfBoundsValuesToDefault) {
    std::string iniContent =
        "[Benchmark]\n"
        "start_delay_seconds=-10\n"
        "duration_seconds=999999\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.benchmark.startDelaySeconds, 0u);
    EXPECT_EQ(config.benchmark.durationSeconds, 0u);
}
