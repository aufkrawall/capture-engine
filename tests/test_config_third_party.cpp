#include "../common/config.h"
#include "test_config_override_fixture.h"

TEST_F(ConfigOverrideTest, ThirdPartyPathsDefaultEmpty) {
    WriteConfig("[ThirdParty]\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config, "test.exe");

    EXPECT_TRUE(config.thirdParty.reshadeDllPath.empty());
    EXPECT_TRUE(config.thirdParty.optiscalerDllPath.empty());
    EXPECT_TRUE(config.thirdParty.specialkDllPath.empty());
}

TEST_F(ConfigOverrideTest, ThirdPartyGlobalSectionParsing) {
    std::string iniContent =
        "[ThirdParty]\n"
        "reshade_dll_path=C:\\tools\\reshade\n"
        "optiscaler_dll_path=C:\\tools\\OptiScaler.dll\n"
        "specialk_dll_path=C:\\tools\\specialk\\\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config, "test.exe");

    EXPECT_EQ(config.thirdParty.reshadeDllPath, "C:\\tools\\reshade");
    EXPECT_EQ(config.thirdParty.optiscalerDllPath, "C:\\tools\\OptiScaler.dll");
    // INI values are literal: a trailing backslash survives parsing and the
    // load-policy resolver handles it without doubling the separator.
    EXPECT_EQ(config.thirdParty.specialkDllPath, "C:\\tools\\specialk\\");
}

TEST_F(ConfigOverrideTest, PerAppThirdPartyPathOverrideWins) {
    std::string iniContent =
        "[ThirdParty]\n"
        "reshade_dll_path=C:\\tools\\global\\reshade\n"
        "[Profile.one]\n"
        "Process=reshade.exe\n"
        "ThirdParty.reshade_dll_path=C:\\tools\\per-app\\reshade\n";

    WriteConfig(iniContent);

    AppConfig matched;
    LoadConfig(tempConfigFile, matched, "reshade.exe");
    EXPECT_EQ(matched.thirdParty.reshadeDllPath, "C:\\tools\\per-app\\reshade");

    AppConfig unmatched;
    LoadConfig(tempConfigFile, unmatched, "other.exe");
    EXPECT_EQ(unmatched.thirdParty.reshadeDllPath, "C:\\tools\\global\\reshade");
}

TEST_F(ConfigOverrideTest, PerAppThirdPartyPathOverrideLeavesUnlistedToolsGlobal) {
    std::string iniContent =
        "[ThirdParty]\n"
        "reshade_dll_path=C:\\tools\\global\\reshade\n"
        "specialk_dll_path=C:\\tools\\global\\specialk\n"
        "[Profile.one]\n"
        "Process=opti.exe\n"
        "ThirdParty.optiscaler_dll_path=C:\\tools\\per-app\\OptiScaler.dll\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config, "opti.exe");
    EXPECT_EQ(config.thirdParty.reshadeDllPath, "C:\\tools\\global\\reshade");
    EXPECT_EQ(config.thirdParty.optiscalerDllPath, "C:\\tools\\per-app\\OptiScaler.dll");
    EXPECT_EQ(config.thirdParty.specialkDllPath, "C:\\tools\\global\\specialk");
}
