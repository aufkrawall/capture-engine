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
    const std::wstring written = targets::SerializeTargetList({L"Game.exe", L"Spiel\u00e4.exe"});
    ASSERT_EQ(targets::WritePersistedTargetList(written, scratchValue), ERROR_SUCCESS);
    std::wstring read;
    const bool readOk = targets::ReadPersistedTargetList(&read, scratchValue);
    EXPECT_EQ(targets::DeletePersistedTargetList(scratchValue), ERROR_SUCCESS);
    ASSERT_TRUE(readOk);
    EXPECT_TRUE(targets::IsProcessNameListed(read, L"GAME.EXE"));
    EXPECT_TRUE(targets::IsProcessNameListed(read, L"spiel\u00e4.exe"));
    EXPECT_FALSE(targets::IsProcessNameListed(read, L"other.exe"));

    std::wstring absent = L"stale";
    EXPECT_FALSE(targets::ReadPersistedTargetList(&absent, scratchValue));
    EXPECT_TRUE(absent.empty());
    EXPECT_EQ(targets::DeletePersistedTargetList(scratchValue), ERROR_SUCCESS) << "an absent list counts as removed";
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

    const std::string participation =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "vulkan_layer" / "layer_participation.cpp");
    const std::string registrar =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "vulkan_layer" / "layer_register.cpp");
    const std::string reader = FunctionBody(participation, "bool IsListedAsResidentTarget() {");
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
    // Both the gate and the full layer read it through that one function.
    EXPECT_NE(layer.find("ce::vulkan_layer_participation::IsListedAsResidentTarget()"), std::string::npos);
    // An unregistered layer leaves no list behind.
    EXPECT_NE(registrar.find("ce::vulkan_layer_targets::DeletePersistedTargetList()"), std::string::npos);
}

// The gate is what every Vulkan process on the machine maps. It must decide with
// the same unit as the full layer, load the full layer only once admitted and
// only from its own directory, release it again when the full layer declines,
// and never export the proc-address entry points a loader could chain.
TEST(VulkanLayerTargetListSourceTest, GateLoadsTheFullLayerOnlyForAnAdmittedProcess) {
    namespace fs = std::filesystem;
    const std::string gate =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "vulkan_layer" / "layer_gate.cpp");
    ASSERT_FALSE(gate.empty());

    const std::string negotiate = FunctionBody(
        gate, "vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {");
    ASSERT_FALSE(negotiate.empty());
    const size_t decide = negotiate.find("ce::vulkan_layer_participation::DecideParticipation(processName)");
    const size_t decline = negotiate.find("if (!decision.participate) {");
    const size_t load = negotiate.find("LoadLayerBesideGate(&error)");
    const size_t release = negotiate.find("FreeLibrary(layer);");
    ASSERT_NE(decide, std::string::npos);
    ASSERT_NE(decline, std::string::npos);
    ASSERT_NE(load, std::string::npos);
    ASSERT_NE(release, std::string::npos);
    EXPECT_LT(decide, decline);
    EXPECT_LT(decline, load) << "a declined process must never map the full layer";
    EXPECT_NE(gate.find("LOAD_WITH_ALTERED_SEARCH_PATH"), std::string::npos);
    EXPECT_NE(gate.find("wcsrchr(path, L'\\\\')"), std::string::npos) << "resolve beside the gate, never search";

    EXPECT_EQ(gate.find("vkGetInstanceProcAddr("), std::string::npos);
    EXPECT_EQ(gate.find("vkGetDeviceProcAddr("), std::string::npos);
    // Nothing of the full layer's runtime: that is the whole point of the gate.
    EXPECT_EQ(gate.find("layer_main.h"), std::string::npos);
    EXPECT_EQ(gate.find("ipc_client.h"), std::string::npos);
}
