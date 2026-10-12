#include "common/config/config.h"
#include "test_config_override_fixture.h"

TEST_F(ConfigOverrideTest, ThirdPartyPathsDefaultEmpty) {
    WriteConfig("[ThirdParty]\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config, "test.exe");

    EXPECT_TRUE(config.thirdParty.reshadeDllPath.empty());
    EXPECT_TRUE(config.thirdParty.optiscalerDllPath.empty());
    EXPECT_TRUE(config.thirdParty.specialkDllPath.empty());
    EXPECT_TRUE(config.thirdParty.customDllPaths.empty());
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

TEST_F(ConfigOverrideTest, CustomDllPathsParsesQuotedAndUnquotedPathsWithSpaces) {
    {
        WriteConfig(
            "[ThirdParty]\n"
            "custom_dll_paths=\"'C:\\path \\ to\\1.dll','C:\\path\\to\\dxgi.dll'\"\n");
        AppConfig config;
        LoadConfig(tempConfigFile, config, "test.exe");
        const std::vector<std::string> expected = {
            R"(C:\path \ to\1.dll)",
            R"(C:\path\to\dxgi.dll)",
        };
        EXPECT_EQ(config.thirdParty.customDllPaths, expected);
    }
    {
        // Without outer double quotes, GetPrivateProfileStringA / LookupIniValue
        // strips the first and last single quote; the parser restores them so
        // spaces and commas inside quoted paths still work.
        WriteConfig(
            "[ThirdParty]\n"
            "custom_dll_paths='C:\\Program Files (x86)\\Mod A\\first mod.dll', 'D:\\Games\\Mod, B\\dxgi.dll'\n");
        AppConfig config;
        LoadConfig(tempConfigFile, config, "test.exe");
        const std::vector<std::string> expected = {
            R"(C:\Program Files (x86)\Mod A\first mod.dll)",
            R"(D:\Games\Mod, B\dxgi.dll)",
        };
        EXPECT_EQ(config.thirdParty.customDllPaths, expected);
    }
    {
        WriteConfig(
            "[ThirdParty]\n"
            "custom_dll_paths=\"C:\\Program Files\\Mod 1\\a b.dll\", \"C:\\Mods\\Mod, 2\\c d.dll\"\n");
        AppConfig config;
        LoadConfig(tempConfigFile, config, "test.exe");
        const std::vector<std::string> expected = {
            R"(C:\Program Files\Mod 1\a b.dll)",
            R"(C:\Mods\Mod, 2\c d.dll)",
        };
        EXPECT_EQ(config.thirdParty.customDllPaths, expected);
    }
    {
        WriteConfig(
            "[ThirdParty]\n"
            "custom_dll_paths=C:\\Program Files (x86)\\My Mod\\1.dll, C:\\Mods\\Owner's PC\\dxgi.dll\n");
        AppConfig config;
        LoadConfig(tempConfigFile, config, "test.exe");
        const std::vector<std::string> expected = {
            R"(C:\Program Files (x86)\My Mod\1.dll)",
            R"(C:\Mods\Owner's PC\dxgi.dll)",
        };
        EXPECT_EQ(config.thirdParty.customDllPaths, expected);
    }
}

TEST_F(ConfigOverrideTest, CustomDllPathsDeduplicatesAndSupportsProfileOverrideAndOff) {
    std::string iniContent =
        "[ThirdParty]\n"
        "custom_dll_paths='C:\\Mods\\First Mod.dll', 'c:/mods/first mod.dll', 'C:\\Mods\\Second Mod.dll'\n"
        "[Profile.custom]\n"
        "Process=custom.exe\n"
        "ThirdParty.custom_dll_paths='D:\\Per App\\only one.dll'\n"
        "[Profile.disabled]\n"
        "Process=vanilla.exe\n"
        "ThirdParty.custom_dll_paths=off\n";

    WriteConfig(iniContent);

    AppConfig globalCfg;
    LoadConfig(tempConfigFile, globalCfg, "other.exe");
    const std::vector<std::string> expectedGlobal = {
        R"(C:\Mods\First Mod.dll)",
        R"(C:\Mods\Second Mod.dll)",
    };
    EXPECT_EQ(globalCfg.thirdParty.customDllPaths, expectedGlobal);

    AppConfig customCfg;
    LoadConfig(tempConfigFile, customCfg, "custom.exe");
    const std::vector<std::string> expectedCustom = {
        R"(D:\Per App\only one.dll)",
    };
    EXPECT_EQ(customCfg.thirdParty.customDllPaths, expectedCustom);

    AppConfig disabledCfg;
    LoadConfig(tempConfigFile, disabledCfg, "vanilla.exe");
    EXPECT_TRUE(disabledCfg.thirdParty.customDllPaths.empty());
}

TEST_F(ConfigOverrideTest, SingleQuotedCommaPathSurvivesGlobalAliasAndProfileLookup) {
    for (const char* key : {"custom_dll_paths", "custom_dll_path"}) {
        for (const char quote : {'\'', '"'}) {
            const std::string path = R"(D:\Mods\Mod, B\dxgi.dll)";
            WriteConfig(std::string("[ThirdParty]\n") + key + "=" + quote + path + quote + "\n" +
                        "[Profile.custom]\nProcess=custom.exe\nThirdParty." + key + "=" + quote +
                        R"(D:\Mods\Other, C\proxy.dll)" + quote + "\n");
            AppConfig global, profile;
            LoadConfig(tempConfigFile, global, "other.exe");
            LoadConfig(tempConfigFile, profile, "custom.exe");
            EXPECT_EQ(global.thirdParty.customDllPaths, std::vector<std::string>{path});
            EXPECT_EQ(profile.thirdParty.customDllPaths,
                      std::vector<std::string>{R"(D:\Mods\Other, C\proxy.dll)"});
        }
    }
}
