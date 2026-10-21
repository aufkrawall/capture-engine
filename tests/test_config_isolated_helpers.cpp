#include "test_config_shared.h"

#include "common/config/config_diagnostics.h"

#include <string>
#include <vector>

// [Runtime] isolated_helpers (DR-45, DR-49): auto by default (the client decides), on/off and the bool spellings force
// processes or threads, an unknown value falls back to auto with a diagnostic, and the key is never reported as
// unknown.
namespace {
bool UnknownRuntimeKeys(const ConfigDiagnostics& diagnostics) {
    for (const ConfigDiagnostic& entry : diagnostics.entries) {
        if (entry.kind == ConfigDiagnosticKind::kUnknownKey && entry.section == "Runtime")
            return true;
        if (entry.kind == ConfigDiagnosticKind::kUnknownSection && entry.section == "Runtime")
            return true;
    }
    return false;
}
}  // namespace

TEST_F(ConfigTest, IsolatedHelpersIsAutoWhenTheSectionIsAbsent) {
    WriteConfig("[Capture]\nblack_when_no_fullscreen_focus=false\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.helperIsolation, HelperIsolation::kAuto);
}

TEST_F(ConfigTest, IsolatedHelpersParsesAutoOnAndOff) {
    AppConfig config;
    WriteConfig("[Runtime]\nisolated_helpers=auto\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.helperIsolation, HelperIsolation::kAuto);
    WriteConfig("[Runtime]\nisolated_helpers=on\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.helperIsolation, HelperIsolation::kOn);
    WriteConfig("[Runtime]\nisolated_helpers=off\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.helperIsolation, HelperIsolation::kOff);
}

TEST_F(ConfigTest, IsolatedHelpersAcceptsTheBoolSpellings) {
    AppConfig config;
    for (const char* spelling : {"true", "TRUE", "1", "yes"}) {
        WriteConfig(std::string("[Runtime]\nisolated_helpers=") + spelling + "\n");
        LoadConfig(tempConfigFile, config);
        EXPECT_EQ(config.helperIsolation, HelperIsolation::kOn) << spelling;
    }
    for (const char* spelling : {"false", "0", "No"}) {
        WriteConfig(std::string("[Runtime]\nisolated_helpers=") + spelling + "\n");
        LoadConfig(tempConfigFile, config);
        EXPECT_EQ(config.helperIsolation, HelperIsolation::kOff) << spelling;
    }
}

TEST_F(ConfigTest, AnUnknownIsolatedHelpersValueIsAutoAndReported) {
    WriteConfig("[Runtime]\nisolated_helpers=sometimes\n");
    AppConfig config;
    ConfigDiagnostics diagnostics;
    LoadConfig(tempConfigFile, config, "", &diagnostics);
    EXPECT_EQ(config.helperIsolation, HelperIsolation::kAuto);
    bool reported = false;
    for (const ConfigDiagnostic& entry : diagnostics.entries)
        reported = reported || (entry.kind == ConfigDiagnosticKind::kInvalidValue && entry.section == "Runtime" &&
                                entry.key == "isolated_helpers");
    EXPECT_TRUE(reported);
}

TEST_F(ConfigTest, IsolatedHelpersIsKnownToTheUnknownKeyTracker) {
    WriteConfig("[Runtime]\nisolated_helpers=true\n");
    AppConfig config;
    ConfigDiagnostics diagnostics;
    LoadConfig(tempConfigFile, config, "", &diagnostics);
    EXPECT_EQ(config.helperIsolation, HelperIsolation::kOn);
    EXPECT_FALSE(UnknownRuntimeKeys(diagnostics));
}

TEST_F(ConfigTest, AMisspelledRuntimeKeyIsStillReported) {
    WriteConfig("[Runtime]\nisolated_helper=true\n");
    AppConfig config;
    ConfigDiagnostics diagnostics;
    LoadConfig(tempConfigFile, config, "", &diagnostics);
    EXPECT_EQ(config.helperIsolation, HelperIsolation::kAuto);
    EXPECT_TRUE(UnknownRuntimeKeys(diagnostics));
}
