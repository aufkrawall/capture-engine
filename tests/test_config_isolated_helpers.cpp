#include "test_config_shared.h"

#include "common/config/config_diagnostics.h"

#include <string>
#include <vector>

// [Runtime] isolated_helpers (DR-45): off by default, a plain bool, and a key the loader reads, so it is never
// reported as unknown.
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

TEST_F(ConfigTest, IsolatedHelpersIsOffWhenTheSectionIsAbsent) {
    WriteConfig("[Capture]\nblack_when_no_fullscreen_focus=false\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config);
    EXPECT_FALSE(config.isolatedHelpers);
}

TEST_F(ConfigTest, IsolatedHelpersParsesAsABool) {
    AppConfig config;
    WriteConfig("[Runtime]\nisolated_helpers=true\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_TRUE(config.isolatedHelpers);

    WriteConfig("[Runtime]\nisolated_helpers=off\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_FALSE(config.isolatedHelpers);

    WriteConfig("[Runtime]\nisolated_helpers=false\n");
    LoadConfig(tempConfigFile, config);
    EXPECT_FALSE(config.isolatedHelpers);
}

TEST_F(ConfigTest, IsolatedHelpersIsKnownToTheUnknownKeyTracker) {
    WriteConfig("[Runtime]\nisolated_helpers=true\n");
    AppConfig config;
    ConfigDiagnostics diagnostics;
    LoadConfig(tempConfigFile, config, "", &diagnostics);
    EXPECT_TRUE(config.isolatedHelpers);
    EXPECT_FALSE(UnknownRuntimeKeys(diagnostics));
}

TEST_F(ConfigTest, AMisspelledRuntimeKeyIsStillReported) {
    WriteConfig("[Runtime]\nisolated_helper=true\n");
    AppConfig config;
    ConfigDiagnostics diagnostics;
    LoadConfig(tempConfigFile, config, "", &diagnostics);
    EXPECT_FALSE(config.isolatedHelpers);
    EXPECT_TRUE(UnknownRuntimeKeys(diagnostics));
}
