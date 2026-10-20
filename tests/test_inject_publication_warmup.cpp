#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "captureengine/injection/inject_config_publication.h"
#include "common/config/config.h"

// The inject role's profile prewarm across runs of the role in one process (DR-38): a thread-hosted inject role
// can be restarted, or the runtime recreated, inside the same client. The stop of one run must not keep the next
// run from prewarming (StopPublicationWarmup used to leave its stop flag set for the rest of the process).

TEST(InjectPublicationWarmupTest, AStoppedPrewarmStartsAgainForTheNextRunOfTheRole) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("ce_publication_warmup_" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    const std::filesystem::path configPath = dir / "config.ini";
    { std::ofstream(configPath) << "[General]\n"; }
    AppConfig config;
    config.gameWhitelist.push_back({.pattern = "game.exe", .windowName = {}});

    StopPublicationWarmup();  // whatever an earlier test left running
    SetPublicationBaseConfig(configPath.string(), config, dir.string());
    EXPECT_TRUE(PublicationWarmupRunningForTesting());
    StopPublicationWarmup();
    EXPECT_FALSE(PublicationWarmupRunningForTesting());

    // The next run of the role publishes again and must get its own prewarm.
    SetPublicationBaseConfig(configPath.string(), config, dir.string());
    EXPECT_TRUE(PublicationWarmupRunningForTesting());
    StopPublicationWarmup();
    EXPECT_FALSE(PublicationWarmupRunningForTesting());

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}
