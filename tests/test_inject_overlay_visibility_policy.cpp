#include <gtest/gtest.h>
#include <windows.h>

#include <string>

#include "../common/config.h"
#include "../common/inject_overlay_policy.h"

namespace {

// The inject process publishes one resolved config for the active injected
// target. These tests pin the rule the overlay-toggle hotkey used to break:
// every publication has to resolve the target's [Profile.*] section, and the
// toggle is an override carried beside that config rather than an edit of it.
class InjectOverlayVisibilityTest : public ::testing::Test {
protected:
    std::string configPath;

    void SetUp() override {
        char buffer[MAX_PATH] = {};
        const std::string name = "test_inject_overlay_visibility." + std::to_string(GetCurrentProcessId()) + ".ini";
        const DWORD length = GetFullPathNameA(name.c_str(), MAX_PATH, buffer, nullptr);
        ASSERT_GT(length, 0u);
        ASSERT_LT(length, static_cast<DWORD>(MAX_PATH));
        configPath.assign(buffer, length);
        remove(configPath.c_str());
    }

    void TearDown() override {
        remove(configPath.c_str());
    }

    void WriteConfig(const std::string& content) {
        HANDLE file =
            CreateFileA(configPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(file, INVALID_HANDLE_VALUE);
        DWORD written = 0;
        ASSERT_TRUE(WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr));
        CloseHandle(file);
        ASSERT_EQ(written, content.size());
    }

    // A global section that leaves every override alone, plus a profile that
    // turns the UE5/DLSS bundle on — the shape the reported regression used.
    void WriteProfileConfig() {
        WriteConfig(
            "[Overlay]\n"
            "enabled=true\n"
            "[Graphics]\n"
            "vsync_mode=default\n"
            "cpu_prerender_limit=-1\n"
            "[DLSS]\n"
            "dlss_sr_preset=default\n"
            "dlss_fg_factor=default\n"
            "[Profile.testgame]\n"
            "process=testgame.exe\n"
            "vsync_mode=fifo\n"
            "cpu_prerender_limit=1\n"
            "dlss_sr_preset=m\n"
            "dlss_fg_factor=4x\n"
            "ue5.force_ray_reconstruction=on\n"
            "ue5.ray_reconstruction_optimal_settings=on\n"
            "ue5.disable_post_processing_effects=on\n"
            "ue5.tonemapper_sharpen=0.6\n"
            "ue5.internal_anisotropic_filtering=16x\n"
            "ue5.internal_texture_mip_bias=-2\n");
    }

    void ExpectProfileOverridesPresent(const AppConfig& config) {
        EXPECT_TRUE(config.graphics.forceRayReconstruction);
        EXPECT_TRUE(config.graphics.rayReconstructionOptimalSettings);
        EXPECT_TRUE(config.graphics.disablePostProcessingEffects);
        EXPECT_FLOAT_EQ(config.graphics.tonemapperSharpen, 0.6f);
        EXPECT_EQ(config.graphics.internalAnisotropicFiltering, 16);
        EXPECT_FLOAT_EQ(config.graphics.internalTextureMipBias, -2.0f);
        EXPECT_EQ(config.graphics.vsyncMode, "fifo");
        EXPECT_FLOAT_EQ(config.graphics.cpuPrerenderLimit, 1.0f);
    }
};

}  // namespace

TEST_F(InjectOverlayVisibilityTest, ToggleKeepsTheActiveTargetsProfileOverrides) {
    WriteProfileConfig();

    AppConfig baseConfig;
    LoadConfig(configPath, baseConfig, "no-such-process.exe");
    EXPECT_FALSE(baseConfig.graphics.forceRayReconstruction) << "the profile must not leak into the base config";

    AppConfig published = ResolveTargetConfig(configPath, baseConfig, "testgame.exe");
    ExpectProfileOverridesPresent(published);
    EXPECT_TRUE(published.overlay.showOverlay);

    // One hotkey press. Before the fix this republished the base config, which
    // dropped every override above and made the hook restore its installed CVar
    // shadows as "configuration disabled" mid-session.
    OverlayVisibilityOverride visibility;
    visibility = ToggleOverlayVisibility(visibility, published.overlay.showOverlay);
    AppConfig afterToggle = ResolveTargetConfig(configPath, baseConfig, "testgame.exe");
    ApplyOverlayVisibility(visibility, afterToggle);

    EXPECT_FALSE(afterToggle.overlay.showOverlay);
    ExpectProfileOverridesPresent(afterToggle);

    // And back on, still with the profile applied.
    visibility = ToggleOverlayVisibility(visibility, afterToggle.overlay.showOverlay);
    AppConfig afterSecondToggle = ResolveTargetConfig(configPath, baseConfig, "testgame.exe");
    ApplyOverlayVisibility(visibility, afterSecondToggle);

    EXPECT_TRUE(afterSecondToggle.overlay.showOverlay);
    ExpectProfileOverridesPresent(afterSecondToggle);
}

TEST_F(InjectOverlayVisibilityTest, ToggleFlipsTheProfileVisibilityNotTheGlobalOne) {
    WriteConfig(
        "[Overlay]\n"
        "enabled=true\n"
        "[Profile.testgame]\n"
        "process=testgame.exe\n"
        "enabled=false\n");

    AppConfig baseConfig;
    LoadConfig(configPath, baseConfig, "no-such-process.exe");
    EXPECT_TRUE(baseConfig.overlay.showOverlay);

    AppConfig resolved = ResolveTargetConfig(configPath, baseConfig, "testgame.exe");
    ASSERT_FALSE(resolved.overlay.showOverlay) << "the profile hides the overlay for this target";

    // Toggling against the resolved value shows the overlay. Toggling against
    // the global value would have produced "hidden" again — a dead first press.
    const OverlayVisibilityOverride visibility = ToggleOverlayVisibility({}, resolved.overlay.showOverlay);
    ApplyOverlayVisibility(visibility, resolved);
    EXPECT_TRUE(visibility.active);
    EXPECT_TRUE(resolved.overlay.showOverlay);
}

TEST_F(InjectOverlayVisibilityTest, ToggleSurvivesRepublicationForTheSameTarget) {
    WriteProfileConfig();

    AppConfig baseConfig;
    LoadConfig(configPath, baseConfig, "no-such-process.exe");

    AppConfig resolved = ResolveTargetConfig(configPath, baseConfig, "testgame.exe");
    const OverlayVisibilityOverride visibility = ToggleOverlayVisibility({}, resolved.overlay.showOverlay);

    // A hook-source detection or injection callback republishes for the same
    // target; the runtime toggle must not silently snap back.
    AppConfig republished = ResolveTargetConfig(configPath, baseConfig, "testgame.exe");
    ApplyOverlayVisibility(visibility, republished);
    EXPECT_FALSE(republished.overlay.showOverlay);
    ExpectProfileOverridesPresent(republished);
}

TEST_F(InjectOverlayVisibilityTest, EmptyTargetPublishesTheBaseConfigUnchanged) {
    WriteProfileConfig();

    AppConfig baseConfig;
    LoadConfig(configPath, baseConfig, "no-such-process.exe");

    // No injected target identified yet. Resolving must not fall back to this
    // process' own image name, which would pull in an unrelated profile.
    const AppConfig resolved = ResolveTargetConfig(configPath, baseConfig, std::string());
    EXPECT_FALSE(resolved.graphics.forceRayReconstruction);
    EXPECT_EQ(resolved.graphics.vsyncMode, baseConfig.graphics.vsyncMode);
    EXPECT_EQ(resolved.overlay.showOverlay, baseConfig.overlay.showOverlay);
}

TEST(InjectOverlayVisibilityPolicyTest, VisibilityFollowsTheConfigUntilTheHotkeyArmsAnOverride) {
    const OverlayVisibilityOverride unarmed;
    EXPECT_FALSE(unarmed.active);
    EXPECT_TRUE(ResolveOverlayVisibility(unarmed, true));
    EXPECT_FALSE(ResolveOverlayVisibility(unarmed, false));

    OverlayVisibilityOverride armed;
    armed.active = true;
    armed.showOverlay = false;
    EXPECT_FALSE(ResolveOverlayVisibility(armed, true)) << "an armed override outranks the configured value";

    armed.showOverlay = true;
    EXPECT_TRUE(ResolveOverlayVisibility(armed, false));
}

TEST(InjectOverlayVisibilityPolicyTest, TogglingAlwaysArmsAndInvertsTheEffectiveVisibility) {
    OverlayVisibilityOverride visibility = ToggleOverlayVisibility({}, true);
    EXPECT_TRUE(visibility.active);
    EXPECT_FALSE(visibility.showOverlay);

    visibility = ToggleOverlayVisibility(visibility, true);
    EXPECT_TRUE(visibility.active);
    EXPECT_TRUE(visibility.showOverlay) << "the second press inverts the override, not the configured value";

    visibility = ToggleOverlayVisibility({}, false);
    EXPECT_TRUE(visibility.active);
    EXPECT_TRUE(visibility.showOverlay);
}

TEST(InjectOverlayVisibilityPolicyTest, TheInjectorsTargetOutranksTheHookSourceIdentity) {
    EXPECT_EQ(ResolveActiveTargetProcessName("newgame.exe", "oldgame.exe"), "newgame.exe");
    EXPECT_EQ(ResolveActiveTargetProcessName(std::string(), "oldgame.exe"), "oldgame.exe")
        << "a hook that connected without an observed injection still decides the profile";
    EXPECT_TRUE(ResolveActiveTargetProcessName(std::string(), std::string()).empty());
}
