#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "../common/vulkan_layer_target_list.h"
#include "source_fragment_reader.h"

namespace targets = ce::vulkan_layer_targets;

TEST(VulkanLayerTargetListTest, SerializedListMatchesCaseInsensitivelyAndExactly) {
    const std::string contents = targets::SerializeTargetList({"DOOMEternalx64vk.exe", "Talos2.exe", "talos2.EXE", ""});
    EXPECT_TRUE(targets::IsProcessNameListed(contents, "doometernalx64vk.exe"));
    EXPECT_TRUE(targets::IsProcessNameListed(contents, "TALOS2.exe"));
    EXPECT_FALSE(targets::IsProcessNameListed(contents, "Talos2"));
    EXPECT_FALSE(targets::IsProcessNameListed(contents, "chrome.exe"));
    EXPECT_FALSE(targets::IsProcessNameListed(contents, ""));
    // The header is a comment, never a match; duplicates collapse.
    EXPECT_FALSE(targets::IsProcessNameListed(contents, "# CaptureEngine: executables the Vulkan layer may enter "
                                                        "while CaptureEngine is not running."));
    EXPECT_EQ(contents.find("talos2.exe"), contents.rfind("talos2.exe"));
}

TEST(VulkanLayerTargetListTest, ToleratesCrLfAndSurroundingWhitespace) {
    EXPECT_TRUE(targets::IsProcessNameListed("# header\r\n  game.exe \r\nother.exe", "GAME.EXE"));
    EXPECT_TRUE(targets::IsProcessNameListed("# header\r\n  game.exe \r\nother.exe", "other.exe"));
    EXPECT_FALSE(targets::IsProcessNameListed("", "game.exe"));
}

// Regression: the implicit layer entered EVERY Vulkan process on the machine and
// stayed in its call chain as "passthrough", including titles whose profile says
// dll_injection=never and anti-cheat-protected games. It now participates only
// where CE may inject: by the running host's whitelist, or - with no host - by
// the whitelist the injector persisted.
TEST(VulkanLayerTargetListTest, ParticipationFollowsTheHostOrThePersistedWhitelist) {
    EXPECT_TRUE(targets::ShouldLayerParticipate(true, true, false));
    EXPECT_FALSE(targets::ShouldLayerParticipate(true, false, true)) << "a running host is authoritative";
    EXPECT_TRUE(targets::ShouldLayerParticipate(false, false, true)) << "late injection for listed targets";
    EXPECT_FALSE(targets::ShouldLayerParticipate(false, false, false));
}

TEST(VulkanLayerTargetListSourceTest, NonTargetsAreDeclinedAtNegotiationAndNeverPinned) {
    namespace fs = std::filesystem;
    const std::string layer =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "vulkan_layer" / "layer_main.cpp");
    const std::string publication =
        ce::test_source::ReadFile(fs::current_path() / "captureengine" / "inject_config_publication.cpp");
    ASSERT_FALSE(layer.empty());
    ASSERT_FALSE(publication.empty());

    const size_t dllMain = layer.find("BOOL WINAPI DllMain(");
    const size_t dllMainEnd = layer.find("// Global states", dllMain);
    ASSERT_NE(dllMain, std::string::npos);
    ASSERT_NE(dllMainEnd, std::string::npos);
    EXPECT_EQ(layer.substr(dllMain, dllMainEnd - dllMain).find("GET_MODULE_HANDLE_EX_FLAG_PIN"), std::string::npos)
        << "a pinned module could not be unloaded after declining";

    const size_t negotiate =
        layer.find("vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {");
    const size_t decision = layer.find("if (!ShouldParticipateInThisProcess()) {", negotiate);
    const size_t decline = layer.find("return VK_ERROR_INITIALIZATION_FAILED;", decision);
    const size_t pin = layer.find("GET_MODULE_HANDLE_EX_FLAG_PIN", negotiate);
    const size_t watcher = layer.find("LayerIPC_StartHostLifecycleWatcher();", negotiate);
    ASSERT_NE(decision, std::string::npos);
    ASSERT_NE(decline, std::string::npos);
    ASSERT_NE(pin, std::string::npos);
    EXPECT_LT(decision, pin);
    EXPECT_LT(decline, watcher) << "a declined process must not get CE's host watcher thread";

    EXPECT_NE(publication.find("PersistVulkanLayerTargetList(persistedNames);"), std::string::npos);
    EXPECT_NE(publication.find("MOVEFILE_REPLACE_EXISTING"), std::string::npos);
}
