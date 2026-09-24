#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../common/vulkan_layer_registration.h"
#include "../common/vulkan_layer_target_list.h"
#include "../hook/vulkan_layer/vulkan_present_thread_policy.h"
#include "source_fragment_reader.h"

namespace targets = ce::vulkan_layer_targets;
namespace registration = ce::vulkan_layer;
namespace present_thread = ce::vulkan_present_boundary;

namespace {

std::string ReadLayerSource(const char* name) {
    return ce::test_source::ReadFile(std::filesystem::current_path() / "hook" / "vulkan_layer" / name);
}

std::string FunctionBody(const std::string& source, const std::string& signature) {
    const size_t begin = source.find(signature);
    const size_t end = begin == std::string::npos ? begin : source.find("\n}\n", begin);
    if (end == std::string::npos)
        return {};
    return source.substr(begin, end - begin);
}

}  // namespace

// Regression: with a compatible host published, a host whitelist miss
// completely masked the persisted registry target list - and the gate decides
// once per process (vkNegotiateLoaderLayerInterfaceVersion; a decline unloads
// the gate and nothing re-enters). A profile edit mid-session, a
// CaptureEngine/game start-order race, or a name encoding mismatch therefore
// locked a real Vulkan game out of overlay, capture and sharpen for the rest of
// its life. Either signal now admits; the worst case is a pre-gate dormant
// entry, never a lockout.
TEST(VulkanLayerGateRegression, HostPublishedAndListedButNotYetEligibleAdmits) {
    EXPECT_TRUE(targets::ShouldLayerParticipate(true, false, true));
    EXPECT_TRUE(targets::ShouldLayerParticipate(true, true, true));
    EXPECT_TRUE(targets::ShouldLayerParticipate(true, true, false));
    EXPECT_FALSE(targets::ShouldLayerParticipate(true, false, false)) << "an unrelated process stays out";
    EXPECT_FALSE(targets::ShouldLayerParticipate(false, false, false));
}

// Regression: the host path matched a GetModuleFileNameA (CP_ACP) name against
// the UTF-8 whitelist the host publishes (captureengine/
// inject_config_publication.cpp) with _stricmp, while the no-host path matched
// GetModuleFileNameW against the UTF-16 registry names. A non-ASCII executable
// name got the inverse of sane behavior: the layer worked with CaptureEngine
// closed and disappeared whenever CaptureEngine ran. Both paths now match in
// UTF-16 off one name source, through the one conversion used for the
// published UTF-8 entries.
TEST(VulkanLayerGateRegression, NonAsciiNameMatchesBothWhitelistsThroughOneConversion) {
    std::string published = "doometernalx64vk.exe";
    published.push_back('\0');
    published += "spiel\xc3\xa4.exe";  // "spiel\u00e4.exe" as the host publishes it: UTF-8
    published.push_back('\0');

    EXPECT_TRUE(targets::IsProcessNameListedUtf8(published, L"spiel\u00e4.exe"));
    EXPECT_TRUE(targets::IsProcessNameListedUtf8(published, L"Spiel\u00e4.EXE"))
        << "ASCII case folding, exact non-ASCII";
    EXPECT_FALSE(targets::IsProcessNameListedUtf8(published, L"spiel.exe"));
    EXPECT_FALSE(targets::IsProcessNameListedUtf8(published, L""));

    const std::wstring persisted = targets::SerializeTargetList({L"spiel\u00e4.exe"});
    EXPECT_TRUE(targets::IsProcessNameListed(persisted, L"spiel\u00e4.exe"));
    EXPECT_TRUE(targets::IsProcessNameListed(persisted, L"Spiel\u00e4.exe"));
}

// The conversion exists only for the published UTF-8 entries; the name itself
// is UTF-16 from GetModuleFileNameW end to end. The old ANSI fetch and its
// locale-dependent compare must not come back.
TEST(VulkanLayerGateRegression, NameMatchingIsWideEndToEndWithOneConversion) {
    const std::string targetList = ce::test_source::ReadFile(
        std::filesystem::current_path() / "common" / "vulkan_layer_target_list.h");
    ASSERT_FALSE(targetList.empty());
    EXPECT_NE(targetList.find("MultiByteToWideChar(CP_UTF8"), std::string::npos)
        << "the one conversion: UTF-8 published entries into the UTF-16 match";

    const std::string participation = ReadLayerSource("layer_participation.cpp");
    ASSERT_FALSE(participation.empty());
    EXPECT_NE(participation.find("GetModuleFileNameW"), std::string::npos);
    EXPECT_EQ(participation.find("GetModuleFileNameA"), std::string::npos)
        << "the ANSI fetch is what mangled non-ASCII names";
    EXPECT_EQ(participation.find("_stricmp"), std::string::npos) << "byte compares never see UTF-8 as equal to UTF-16";
    EXPECT_NE(participation.find("IsProcessNameListedUtf8"), std::string::npos);
    EXPECT_NE(participation.find("GetCurrentProcessBaseNameWide"), std::string::npos);
}

// The persisted list must be consulted for every process the host has not (yet)
// admitted - gating that read on `hostPublished` is exactly the mask that
// locked real targets out.
TEST(VulkanLayerGateRegression, ThePersistedListIsNeverMaskedByAPublishedHost) {
    const std::string participation = ReadLayerSource("layer_participation.cpp");
    const std::string main = ReadLayerSource("layer_main.cpp");
    ASSERT_FALSE(participation.empty());
    ASSERT_FALSE(main.empty());

    const std::string decide =
        FunctionBody(participation, "Decision DecideParticipation(const wchar_t* processName) {");
    ASSERT_FALSE(decide.empty());
    const size_t listedGate = decide.find("if (!decision.eligibleByHost)");
    const size_t listedRead = decide.find("decision.listedTarget = IsListedAsResidentTarget();");
    ASSERT_NE(listedGate, std::string::npos);
    ASSERT_NE(listedRead, std::string::npos);
    EXPECT_LT(listedGate, listedRead);
    EXPECT_EQ(decide.find("if (!decision.hostPublished)"), std::string::npos)
        << "host-published + listed-but-not-yet-eligible must still admit";

    EXPECT_NE(main.find("const bool listed = !eligibleByHost && ce::vulkan_layer_participation::"
                        "IsListedAsResidentTarget();"),
              std::string::npos)
        << "the full layer's own re-decision follows the same rule as the gate";
}

// Regression: the gate manifest was generated only when the layer artifacts
// were staged to the versioned directory. With stagingDir == baseDir - the
// documented env-root fallback - generation was skipped entirely and whatever
// manifest sat there got registered: the checked-in one names the FULL layer
// with vkGetInstanceProcAddr, so the 1.5 MB layer and its imports were mapped
// into every Vulkan process on the machine again. The fallback must generate
// the same gate manifest the staging path does.
TEST(VulkanLayerGateRegression, EnvRootFallbackRegistersAGateManifestNotTheFullLayer) {
    const std::filesystem::path testRoot = std::filesystem::current_path() / "vk_gate_manifest_fallback_test";
    const std::filesystem::path baseDir = testRoot / "installed";
    std::filesystem::create_directories(baseDir);

    const std::filesystem::path manifestPath = baseDir / L"VK_LAYER_CE_overlay.json";
    {
        // The stale shape: the full layer named as the manifest's library.
        std::ofstream stale(registration::PathToUtf8ForLogging(manifestPath), std::ios::binary | std::ios::trunc);
        stale << "{ \"layer\": { \"library_path\": \".\\\\VK_LAYER_CE_overlay.dll\",\n"
                 "  \"functions\": { \"vkGetInstanceProcAddr\": \"vkGetInstanceProcAddr\" } } }\n";
        std::ofstream layer(registration::PathToUtf8ForLogging(baseDir / L"VK_LAYER_CE_overlay.dll"),
                            std::ios::binary);
        layer << '\n';
        std::ofstream gate(registration::PathToUtf8ForLogging(baseDir / L"VK_LAYER_CE_gate.dll"), std::ios::binary);
        gate << '\n';
    }

    // An explicit staging directory equal to baseDir is the env-root fallback
    // shape (BuildRegistrationPlan falls back to the source directory when the
    // environment root cannot be resolved).
    const auto plan = registration::BuildRegistrationPlan(baseDir, registration::RegistrationMode::CurrentUser, false,
                                                          baseDir);
    ASSERT_EQ(plan.stagingDir, baseDir);
    ASSERT_EQ(plan.installTargets.size(), 1u);
    ASSERT_TRUE(registration::ApplyRegistrationPlan(plan, true));

    const std::string generated = ce::test_source::ReadFile(manifestPath);
    ASSERT_FALSE(generated.empty());
    EXPECT_NE(generated.find("VK_LAYER_CE_gate.dll"), std::string::npos)
        << "the manifest must name the negotiation gate";
    EXPECT_EQ(generated.find("VK_LAYER_CE_overlay.dll"), std::string::npos)
        << "the full layer must never be the manifest's library_path";
    EXPECT_EQ(generated.find("vkGetInstanceProcAddr"), std::string::npos)
        << "the gate exports negotiation only";

    EXPECT_TRUE(registration::ApplyRegistrationPlan(plan, false));
    std::filesystem::remove_all(testRoot);
}

// Regression: async-present detection latched forever off a device-wide,
// un-windowed submit-thread heuristic. One background-worker submit minutes
// earlier forced limiter pacing plus the reserved-queue overlay route for the
// whole session (`20260831_054801` latched 7.5 s in). The submit route now
// requires the same recency window the acquire route has always had.
TEST(VulkanLayerGateRegression, AsyncPresentSubmitRouteRequiresARecentMismatch) {
    using present_thread::AsyncRoute;
    using present_thread::DetectSubmitThreadMismatch;

    // The present thread submitted recently itself: synchronous presentation.
    EXPECT_EQ(DetectSubmitThreadMismatch(7, 100000, 7, 100500), AsyncRoute::kNone);
    // A fresh submit from another thread is the async-present shape.
    EXPECT_EQ(DetectSubmitThreadMismatch(7, 100000, 9, 100500), AsyncRoute::kSubmitThreadMismatch);
    // ... and stays one while that thread keeps submitting.
    EXPECT_EQ(DetectSubmitThreadMismatch(7, 101000, 9, 101200), AsyncRoute::kSubmitThreadMismatch);
    // The window's edges: fresh at 1999 ms, stale at the window itself.
    EXPECT_EQ(DetectSubmitThreadMismatch(7, 100000, 9, 101999), AsyncRoute::kSubmitThreadMismatch);
    EXPECT_EQ(DetectSubmitThreadMismatch(7, 100000, 9, 102000), AsyncRoute::kNone);
    // A background-worker submit from minutes ago is stale evidence.
    EXPECT_EQ(DetectSubmitThreadMismatch(7, 100000, 9, 400000), AsyncRoute::kNone);
    // Never submitted / an unknown thread says nothing.
    EXPECT_EQ(DetectSubmitThreadMismatch(0, 100000, 9, 100500), AsyncRoute::kNone);
    EXPECT_EQ(DetectSubmitThreadMismatch(7, 0, 9, 100500), AsyncRoute::kNone);
}

// The acquire route's window, pinned beside the submit route's: both routes
// move the same limiter boundary and must not drift apart.
TEST(VulkanLayerGateRegression, AsyncPresentAcquireRouteUsesTheSameRecencyWindow) {
    using present_thread::AsyncRoute;
    using present_thread::DetectAcquireThreadMismatch;

    EXPECT_EQ(DetectAcquireThreadMismatch(7, 100000, 9, 100500), AsyncRoute::kAcquireThreadMismatch);
    EXPECT_EQ(DetectAcquireThreadMismatch(7, 100000, 9, 101999), AsyncRoute::kAcquireThreadMismatch);
    EXPECT_EQ(DetectAcquireThreadMismatch(7, 100000, 9, 102000), AsyncRoute::kNone);
    EXPECT_EQ(DetectAcquireThreadMismatch(7, 100000, 9, 400000), AsyncRoute::kNone);
    EXPECT_EQ(DetectAcquireThreadMismatch(7, 100000, 7, 100500), AsyncRoute::kNone);
    EXPECT_EQ(DetectAcquireThreadMismatch(0, 100000, 9, 100500), AsyncRoute::kNone);
    EXPECT_EQ(present_thread::kThreadRecencyWindowMs, 2000ULL);
}

// The present hook must ask the windowed policy for the submit route and feed
// it the submission's timestamp; a bare thread-id compare is what latched.
TEST(VulkanLayerGateRegression, SubmitRouteDetectionGoesThroughTheWindowedPolicy) {
    const std::string boundary = ce::test_source::ReadFile(
        std::filesystem::current_path() / "hook" / "vulkan_layer" / "vulkan_present_boundary.h");
    ASSERT_FALSE(boundary.empty());
    EXPECT_NE(boundary.find("GetLastSubmitTickMs(queueDevice)"), std::string::npos);
    EXPECT_NE(boundary.find("DetectSubmitThreadMismatch(lastSubmitThreadId, lastSubmitTickMs"), std::string::npos);
}

// The checked-in reference manifests named the full layer; registered anywhere,
// they would map it (and its imports) into every Vulkan process again.
TEST(VulkanLayerGateRegression, CheckedInManifestsNameTheGateNotTheFullLayer) {
    for (const auto& [name, gate] : {std::pair{"VK_LAYER_CE_overlay.json", "VK_LAYER_CE_gate.dll"},
                                     std::pair{"VK_LAYER_CE_overlay_x86.json", "VK_LAYER_CE_gate_x86.dll"}}) {
        SCOPED_TRACE(name);
        const std::string manifest = ReadLayerSource(name);
        ASSERT_FALSE(manifest.empty());
        EXPECT_NE(manifest.find(gate), std::string::npos);
        EXPECT_EQ(manifest.find("VK_LAYER_CE_overlay.dll"), std::string::npos);
        EXPECT_EQ(manifest.find("VK_LAYER_CE_overlay_x86.dll"), std::string::npos);
        EXPECT_EQ(manifest.find("vkGetInstanceProcAddr"), std::string::npos);
    }
}
