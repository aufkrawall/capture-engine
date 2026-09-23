#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

#include "../common/vulkan_layer_target_list.h"
#include "source_fragment_reader.h"

namespace targets = ce::vulkan_layer_targets;

namespace {

std::string FunctionBody(const std::string& source, const std::string& signature) {
    const size_t begin = source.find(signature);
    const size_t end = begin == std::string::npos ? begin : source.find("\n}\n", begin);
    if (end == std::string::npos)
        return {};
    return source.substr(begin, end - begin);
}

}  // namespace

TEST(VulkanLayerTargetListTest, SerializedListMatchesCaseInsensitivelyAndExactly) {
    const std::wstring list = targets::SerializeTargetList({L"DOOMEternalx64vk.exe", L"Talos2.exe", L"talos2.EXE", L""});
    EXPECT_TRUE(targets::IsProcessNameListed(list, L"doometernalx64vk.exe"));
    EXPECT_TRUE(targets::IsProcessNameListed(list, L"TALOS2.exe"));
    EXPECT_FALSE(targets::IsProcessNameListed(list, L"Talos2"));
    EXPECT_FALSE(targets::IsProcessNameListed(list, L"chrome.exe"));
    EXPECT_FALSE(targets::IsProcessNameListed(list, L""));
    // Duplicates collapse; empty names are dropped instead of ending the list early.
    EXPECT_EQ(list, std::wstring(L"doometernalx64vk.exe\0talos2.exe\0\0", 33));
}

TEST(VulkanLayerTargetListTest, ListEndsAtTheFirstEmptyStringOrTheEndOfTheData) {
    EXPECT_FALSE(targets::IsProcessNameListed(std::wstring(L"a.exe\0\0b.exe\0\0", 14), L"b.exe"));
    EXPECT_TRUE(targets::IsProcessNameListed(std::wstring(L"a.exe\0b.exe", 11), L"b.exe"));
    EXPECT_FALSE(targets::IsProcessNameListed(std::wstring(), L"a.exe"));
    EXPECT_FALSE(targets::IsProcessNameListed(targets::SerializeTargetList({}), L"a.exe"));
}

// Regression: the injector wrote the list beside captureengine.exe while the
// layer read it beside its STAGED copy (%LOCALAPPDATA%\CaptureEngine\
// vulkan_layers\b<build>), so no Vulkan title started before CaptureEngine was
// ever admitted. Both sides now go through the same registry value; this
// round-trips it under a scratch value name so the real list is untouched.
TEST(VulkanLayerTargetListTest, RegistryRoundTripKeepsEveryName) {
    const wchar_t* scratchValue = L"VulkanLayerTargets_UnitTest";
    const std::wstring written = targets::SerializeTargetList({L"Game.exe", L"Spielä.exe"});
    ASSERT_EQ(targets::WritePersistedTargetList(written, scratchValue), ERROR_SUCCESS);
    std::wstring read;
    const bool readOk = targets::ReadPersistedTargetList(&read, scratchValue);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, targets::kRegistryKey, scratchValue);
    ASSERT_TRUE(readOk);
    EXPECT_TRUE(targets::IsProcessNameListed(read, L"GAME.EXE"));
    EXPECT_TRUE(targets::IsProcessNameListed(read, L"spielä.exe"));
    EXPECT_FALSE(targets::IsProcessNameListed(read, L"other.exe"));

    std::wstring absent = L"stale";
    EXPECT_FALSE(targets::ReadPersistedTargetList(&absent, scratchValue));
    EXPECT_TRUE(absent.empty());
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
}

// The writer and the reader must name the list the same way. A location derived
// from either module's own path cannot: the layer runs from a versioned staging
// copy that neither the injector nor a newer host shares.
TEST(VulkanLayerTargetListSourceTest, WriterAndReaderShareOneLocationIndependentOfModulePaths) {
    namespace fs = std::filesystem;
    const std::string layer =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "vulkan_layer" / "layer_main.cpp");
    const std::string publication =
        ce::test_source::ReadFile(fs::current_path() / "captureengine" / "inject_config_publication.cpp");
    ASSERT_FALSE(layer.empty());
    ASSERT_FALSE(publication.empty());

    const std::string reader = FunctionBody(layer, "static bool IsListedAsResidentTarget() {");
    const std::string writer =
        FunctionBody(publication, "static void PersistVulkanLayerTargetList(const std::vector<std::string>& names) {");
    ASSERT_FALSE(reader.empty());
    ASSERT_FALSE(writer.empty());
    EXPECT_NE(reader.find("ce::vulkan_layer_targets::ReadPersistedTargetList(&list)"), std::string::npos);
    EXPECT_NE(writer.find("ce::vulkan_layer_targets::WritePersistedTargetList(contents)"), std::string::npos);
    for (const std::string* body : {&reader, &writer}) {
        EXPECT_EQ(body->find("parent_path()"), std::string::npos) << "the list must not live beside a module";
        EXPECT_EQ(body->find("GetModuleHandleExW"), std::string::npos);
    }
}
