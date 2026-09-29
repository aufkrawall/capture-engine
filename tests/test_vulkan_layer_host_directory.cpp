#include <gtest/gtest.h>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "source_fragment_reader.h"

#include "../common/vulkan_layer_host_directory.h"
#include "../common/vulkan_layer_registration.h"

namespace host_dir = ce::vulkan_layer_host_directory;

namespace {

using ce::vulkan_layer::BuildRegistrationPlan;
using ce::vulkan_layer::RegistrationMode;

void TouchFile(const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary);
    out << '\n';
}

std::string ReadBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string FunctionBody(const std::string& source, const std::string& signature) {
    const size_t begin = source.find(signature);
    const size_t end = begin == std::string::npos ? begin : source.find("\n}\n", begin);
    if (end == std::string::npos)
        return {};
    return source.substr(begin, end - begin);
}

// A layout with the layer and its gate present, so the plan is registrable.
void CreateInstallLayout(const std::filesystem::path& baseDir) {
    std::filesystem::create_directories(baseDir);
    TouchFile(baseDir / L"VK_LAYER_CE_overlay.json");
    TouchFile(baseDir / L"VK_LAYER_CE_overlay.dll");
    TouchFile(baseDir / L"VK_LAYER_CE_gate.dll");
}

}  // namespace

TEST(VulkanLayerHostDirectoryTest, ShapeAcceptsOnlyFullyQualifiedDirectories) {
    EXPECT_TRUE(host_dir::IsUsableHostDirectory(L"C:\\Users\\Someone\\Programme\\captureengine"));
    EXPECT_TRUE(host_dir::IsUsableHostDirectory(L"D:/games/captureengine"));
    EXPECT_TRUE(host_dir::IsUsableHostDirectory(L"\\\\server\\share\\captureengine"));
    EXPECT_TRUE(host_dir::IsUsableHostDirectory(L"\\\\?\\C:\\captureengine"));
    EXPECT_TRUE(host_dir::IsUsableHostDirectory(L"C:\\"));

    // Everything below would be resolved against the process's current directory
    // or names something that is not a directory a DLL can sit in.
    EXPECT_FALSE(host_dir::IsUsableHostDirectory(L""));
    EXPECT_FALSE(host_dir::IsUsableHostDirectory(L"captureengine"));
    EXPECT_FALSE(host_dir::IsUsableHostDirectory(L".\\captureengine"));
    EXPECT_FALSE(host_dir::IsUsableHostDirectory(L"\\captureengine"));
    EXPECT_FALSE(host_dir::IsUsableHostDirectory(L"C:captureengine"));
    EXPECT_FALSE(host_dir::IsUsableHostDirectory(L"C:"));
    EXPECT_FALSE(host_dir::IsUsableHostDirectory(L"\\\\"));
    EXPECT_FALSE(host_dir::IsUsableHostDirectory(L"\\\\.\\C:"));
    EXPECT_FALSE(host_dir::IsUsableHostDirectory(L"C:\\a\\..\\b"));
    EXPECT_FALSE(host_dir::IsUsableHostDirectory(L"C:\\a\\..") );
    EXPECT_FALSE(host_dir::IsUsableHostDirectory(std::wstring(L"C:\\a\0b", 6)));
    EXPECT_FALSE(host_dir::IsUsableHostDirectory(L"C:\\a\nb"));
    // A segment merely containing dots is an ordinary name.
    EXPECT_TRUE(host_dir::IsUsableHostDirectory(L"C:\\a..b\\...\\captureengine"));
}

TEST(VulkanLayerHostDirectoryTest, SerializeAndParseRoundTripIncludingNonAsciiFolders) {
    const std::wstring directory = L"C:\\Users\\J\u00fclian\\Programme\\captureengine";
    const std::string bytes = host_dir::Serialize(directory);
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes, "C:\\Users\\J\xC3\xBClian\\Programme\\captureengine")
        << "the file is UTF-8, so a folder outside the ANSI code page survives";
    EXPECT_EQ(host_dir::Parse(bytes), directory);

    // A trailing separator is not part of the identity.
    EXPECT_EQ(host_dir::Serialize(L"C:\\captureengine\\"), host_dir::Serialize(L"C:\\captureengine"));
    // A drive root keeps the separator that makes it a root.
    EXPECT_EQ(host_dir::Parse(host_dir::Serialize(L"C:\\")), L"C:\\");
}

TEST(VulkanLayerHostDirectoryTest, SerializeRefusesWhatTheLayerWouldReject) {
    EXPECT_TRUE(host_dir::Serialize(L"").empty());
    EXPECT_TRUE(host_dir::Serialize(L"captureengine").empty());
    EXPECT_TRUE(host_dir::Serialize(L"C:\\a\\..\\b").empty());
    const std::wstring tooLong = L"C:\\" + std::wstring(host_dir::kMaxPointerFileBytes, L'a');
    EXPECT_TRUE(host_dir::Serialize(tooLong).empty()) << "an oversized path cannot be read back within the bound";
}

TEST(VulkanLayerHostDirectoryTest, ParseToleratesEditorArtifactsAndRefusesEverythingElse) {
    const std::wstring expected = L"C:\\captureengine";
    EXPECT_EQ(host_dir::Parse("C:\\captureengine"), expected);
    EXPECT_EQ(host_dir::Parse("C:\\captureengine\r\n"), expected);
    EXPECT_EQ(host_dir::Parse("C:\\captureengine\n"), expected);
    EXPECT_EQ(host_dir::Parse("C:\\captureengine \t"), expected);
    EXPECT_EQ(host_dir::Parse("\xEF\xBB\xBF" "C:\\captureengine\r\n"), expected) << "Notepad writes a BOM";
    EXPECT_EQ(host_dir::Parse(std::string("C:\\captureengine\0\0", 18)), expected) << "zero padding after the path";

    EXPECT_TRUE(host_dir::Parse("").empty());
    EXPECT_TRUE(host_dir::Parse("\r\n").empty());
    EXPECT_TRUE(host_dir::Parse("captureengine").empty());
    EXPECT_TRUE(host_dir::Parse("C:\\captureengine\r\nD:\\other").empty()) << "a second line is not a pointer";
    EXPECT_TRUE(host_dir::Parse(std::string("C:\\capture\0engine", 17)).empty()) << "an embedded NUL is a partial write";
    EXPECT_TRUE(host_dir::Parse("C:\\a\\..\\b").empty());
    EXPECT_TRUE(host_dir::Parse("C:\\caf\xC3").empty()) << "a truncated UTF-8 sequence";
    EXPECT_TRUE(host_dir::Parse("C:\\bad\xFF\xFE").empty()) << "invalid UTF-8";
    EXPECT_TRUE(host_dir::Parse(std::string(host_dir::kMaxPointerFileBytes + 1, 'a')).empty());
}

// The failure this pins: the layer is a staged copy, so "beside the layer" never
// holds the hook. The installed hook - named by the pointer - has to come first.
TEST(VulkanLayerHostDirectoryTest, InstalledHookIsTriedBeforeTheStagedLayerDirectory) {
    const std::wstring staged = L"C:\\Users\\Someone\\AppData\\Local\\CaptureEngine\\vulkan_layers\\b6850";
    const std::wstring installed = L"C:\\Users\\Someone\\Programme\\captureengine";

    const std::vector<std::wstring> candidates =
        host_dir::HookLoadCandidates(installed, staged, L"capture_hook_x64.dll");
    ASSERT_EQ(candidates.size(), 2u);
    EXPECT_EQ(candidates[0], installed + L"\\capture_hook_x64.dll");
    EXPECT_EQ(candidates[1], staged + L"\\capture_hook_x64.dll");
}

TEST(VulkanLayerHostDirectoryTest, WithoutAPointerTheLayerDirectoryIsTheOnlyCandidate) {
    // Development layout: nothing staged, the layer sits beside the hook.
    const std::vector<std::wstring> candidates =
        host_dir::HookLoadCandidates(L"", L"D:\\build\\installed\\captureengine", L"capture_hook_x86.dll");
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates[0], L"D:\\build\\installed\\captureengine\\capture_hook_x86.dll");
}

TEST(VulkanLayerHostDirectoryTest, CandidatesDropDuplicatesUnusableDirectoriesAndBadNames) {
    // Same directory spelled differently (case, trailing separator) is one attempt.
    const auto duplicate = host_dir::HookLoadCandidates(L"C:\\CaptureEngine\\", L"c:\\captureengine",
                                                        L"capture_hook_x64.dll");
    ASSERT_EQ(duplicate.size(), 1u);
    EXPECT_EQ(duplicate[0], L"C:\\CaptureEngine\\capture_hook_x64.dll");

    // Nothing relative may ever become a load path.
    EXPECT_TRUE(host_dir::HookLoadCandidates(L"captureengine", L"", L"capture_hook_x64.dll").empty());
    EXPECT_TRUE(host_dir::HookLoadCandidates(L"", L"", L"capture_hook_x64.dll").empty());

    // The name is a file name, never a path: it cannot escape the directory.
    EXPECT_TRUE(host_dir::HookLoadCandidates(L"C:\\ce", L"C:\\layer", L"..\\evil.dll").empty());
    EXPECT_TRUE(host_dir::HookLoadCandidates(L"C:\\ce", L"C:\\layer", L"D:evil.dll").empty());
    EXPECT_TRUE(host_dir::HookLoadCandidates(L"C:\\ce", L"C:\\layer", L"").empty());

    // A drive root gains no doubled separator.
    const auto root = host_dir::HookLoadCandidates(L"C:\\", L"", L"capture_hook_x64.dll");
    ASSERT_EQ(root.size(), 1u);
    EXPECT_EQ(root[0], L"C:\\capture_hook_x64.dll");
}

// Staging must record where the installed hook lives, or a split renderer's
// layer has nowhere to load it from (Portal RTX 20260929_033904).
TEST(VulkanLayerHostDirectoryTest, StagingRecordsTheInstallDirectoryBesideTheStagedLayer) {
    const std::filesystem::path testRoot = std::filesystem::current_path() / "vk_host_dir_staging_test";
    std::filesystem::remove_all(testRoot);
    const std::filesystem::path baseDir = testRoot / "installed";
    const std::filesystem::path stagingDir = testRoot / "staging";
    CreateInstallLayout(baseDir);

    const auto plan = BuildRegistrationPlan(baseDir, RegistrationMode::CurrentUser, false, stagingDir);
    ASSERT_EQ(plan.installTargets.size(), 1u);
    ASSERT_TRUE(ce::vulkan_layer::ApplyRegistrationPlan(plan, true));

    const std::filesystem::path pointer = stagingDir / host_dir::kPointerFileName;
    ASSERT_TRUE(std::filesystem::exists(pointer));
    const std::wstring recorded = host_dir::Parse(ReadBytes(pointer));
    ASSERT_FALSE(recorded.empty());
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::equivalent(std::filesystem::path(recorded), baseDir, ec))
        << "the pointer must name the install directory the layer was staged from";
    EXPECT_FALSE(std::filesystem::exists(stagingDir / (std::wstring(host_dir::kPointerFileName) + L".tmp")))
        << "the atomic replace must not leave its temporary file behind";

    // Staging again from the same install is a no-op on content.
    const std::string first = ReadBytes(pointer);
    ASSERT_TRUE(ce::vulkan_layer::ApplyRegistrationPlan(plan, true));
    EXPECT_EQ(ReadBytes(pointer), first);

    ce::vulkan_layer::ApplyRegistrationPlan(plan, false);
    std::filesystem::remove_all(testRoot);
}

// The same staged directory is reused when CaptureEngine starts from another
// install of the same build number; the pointer must follow the host that staged
// last, not keep naming a directory that may no longer exist.
TEST(VulkanLayerHostDirectoryTest, RestagingFromAnotherInstallDirectoryUpdatesThePointer) {
    const std::filesystem::path testRoot = std::filesystem::current_path() / "vk_host_dir_restage_test";
    std::filesystem::remove_all(testRoot);
    const std::filesystem::path installA = testRoot / "installA";
    const std::filesystem::path installB = testRoot / "installB";
    const std::filesystem::path stagingDir = testRoot / "staging";
    CreateInstallLayout(installA);
    CreateInstallLayout(installB);

    const auto planA = BuildRegistrationPlan(installA, RegistrationMode::CurrentUser, false, stagingDir);
    const auto planB = BuildRegistrationPlan(installB, RegistrationMode::CurrentUser, false, stagingDir);
    ASSERT_TRUE(ce::vulkan_layer::ApplyRegistrationPlan(planA, true));
    const std::string pointerA = ReadBytes(stagingDir / host_dir::kPointerFileName);
    ASSERT_TRUE(ce::vulkan_layer::ApplyRegistrationPlan(planB, true));
    const std::string pointerB = ReadBytes(stagingDir / host_dir::kPointerFileName);

    EXPECT_NE(pointerA, pointerB);
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::equivalent(std::filesystem::path(host_dir::Parse(pointerB)), installB, ec));

    ce::vulkan_layer::ApplyRegistrationPlan(planB, false);
    std::filesystem::remove_all(testRoot);
}

// With nothing staged (staging directory == install directory) the layer already
// sits beside the hook; a pointer file there would only be clutter in the
// install directory.
TEST(VulkanLayerHostDirectoryTest, NoPointerIsWrittenWhenTheLayerIsNotStaged) {
    const std::filesystem::path testRoot = std::filesystem::current_path() / "vk_host_dir_unstaged_test";
    std::filesystem::remove_all(testRoot);
    const std::filesystem::path baseDir = testRoot / "installed";
    CreateInstallLayout(baseDir);

    const auto plan = BuildRegistrationPlan(baseDir, RegistrationMode::CurrentUser, false, baseDir);
    ASSERT_EQ(plan.stagingDir, plan.baseDir);
    ASSERT_TRUE(ce::vulkan_layer::ApplyRegistrationPlan(plan, true));
    EXPECT_FALSE(std::filesystem::exists(baseDir / host_dir::kPointerFileName));

    ce::vulkan_layer::ApplyRegistrationPlan(plan, false);
    std::filesystem::remove_all(testRoot);
}

TEST(VulkanLayerHostDirectorySourceTest, BootstrapLoadsTheHookFromTheRecordedHostDirectory) {
    namespace fs = std::filesystem;
    const fs::path root = fs::current_path();
    const std::string bootstrap =
        ce::test_source::ReadFile(root / "hook" / "vulkan_layer" / "layer_renderer_bootstrap.cpp");
    const std::string ipc = ce::test_source::ReadFile(root / "hook" / "vulkan_layer" / "layer_ipc.cpp");
    const std::string header = ce::test_source::ReadFile(root / "hook" / "vulkan_layer" / "layer_main.h");
    const std::string registration = ce::test_source::ReadFile(root / "common" / "vulkan_layer_registration.cpp");
    ASSERT_FALSE(bootstrap.empty());
    ASSERT_FALSE(ipc.empty());
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(registration.empty());

    const std::string body = FunctionBody(bootstrap, "InheritedRendererBootstrap LayerBootstrapInheritedRendererHook() {");
    ASSERT_FALSE(body.empty());
    // The hook is resolved through the recorded directory, never as
    // `<layer directory>\capture_hook_x64.dll` alone: the layer is a staged copy.
    EXPECT_NE(body.find("ReadStagedHostDirectory(directory"), std::string::npos);
    EXPECT_NE(body.find("HookLoadCandidates("), std::string::npos);
    EXPECT_EQ(body.find("directory / kHookName"), std::string::npos)
        << "the staged layer directory does not hold the hook (error 126 in session 20260929_033904)";
    EXPECT_NE(bootstrap.find("LoadLibraryFromSecurePath"), std::string::npos);

    // Staging writes the same file name the layer reads, from the same header.
    EXPECT_NE(bootstrap.find("kPointerFileName"), std::string::npos);
    EXPECT_NE(registration.find("kPointerFileName"), std::string::npos);
    const size_t stage = registration.find("static bool StagePlanArtifacts(");
    const size_t stageEnd = registration.find("void LogRegistrationPlan(", stage);
    ASSERT_NE(stage, std::string::npos);
    ASSERT_NE(stageEnd, std::string::npos);
    EXPECT_NE(registration.substr(stage, stageEnd - stage).find("WriteHostDirectoryPointer(plan)"),
              std::string::npos);

    // A layer that could not load the hook must not keep the claim that tells
    // the client to stand down.
    const size_t bootstrapCall = ipc.find("LayerBootstrapInheritedRendererHook()");
    ASSERT_NE(bootstrapCall, std::string::npos);
    const size_t unavailable = ipc.find("InheritedRendererBootstrap::HookUnavailable", bootstrapCall);
    const size_t release = ipc.find("ReleaseInheritedRendererClaim(sharedMemory)", unavailable);
    EXPECT_NE(unavailable, std::string::npos);
    EXPECT_NE(release, std::string::npos);
    EXPECT_NE(header.find("HookUnavailable"), std::string::npos);
    EXPECT_NE(header.find("NotReady"), std::string::npos);

    // Both failure modes say what the user loses, so the next log names it.
    EXPECT_NE(bootstrap.find("dlss_*_dll_path"), std::string::npos);
    EXPECT_NE(bootstrap.find("will NOT apply in this renderer"), std::string::npos);
}
