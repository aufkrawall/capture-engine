#include <gtest/gtest.h>

#include "common/platform/runtime_package_paths.h"
#include "common/platform/ansi_path.h"
#include "common/ipc/process_ipc.h"
#include "captureengine/app/runtime_configuration.h"
#include "source_fragment_reader.h"

#include <filesystem>
#include <fstream>
#include <optional>

// A bounded helper role in the actual test executable. It exercises the same
// startup path selection and settings owner as WinMain without recording/UI.
std::optional<int> RunRuntimePackagePathProbe(int argc, char** argv) {
    if (ParseProcessMode(argc, argv) != ProcessMode::Logger)
        return std::nullopt;
    ce::runtime::PackagePathError error = ce::runtime::PackagePathError::None;
    const auto paths =
        ce::runtime::RuntimePackagePaths::FromModule(nullptr, ce::runtime::ReadProcessConfigurationArgument(), error);
    if (!paths)
        return 2;
    ScopedQuietConfigLog quiet;
    ce::runtime::RuntimeConfigurationSession settings(paths->Configuration());
    return settings.IsReady() && ce::runtime::RuntimeConfiguration().video.outputDir == "runtime-path-probe-value" ? 0
                                                                                                                   : 3;
}

namespace {
using namespace ce::runtime;

TEST(RuntimePackagePathsTest, DefaultConfigurationFollowsTheRuntimeNotTheEmbeddingClient) {
    PackagePathError error = PackagePathError::None;
    auto paths = RuntimePackagePaths::FromExecutable(L"C:\\runtime package\\captureengine.exe", {}, error);
    ASSERT_TRUE(paths.has_value());
    if (!paths)
        return;
    EXPECT_EQ(error, PackagePathError::None);
    EXPECT_EQ(paths->Executable(), L"C:\\runtime package\\captureengine.exe");
    EXPECT_EQ(paths->ConfigurationWide(), L"C:\\runtime package\\config.ini");
    EXPECT_EQ(paths->Configuration(), "C:\\runtime package\\config.ini");
}

TEST(RuntimePackagePathsTest, ExecutableKeepsItsOwnNameAndDllPackageChoosesTheWorkerBesideIt) {
    PackagePathError error = PackagePathError::None;
    auto process = RuntimePackagePaths::FromModule(nullptr, {}, error);
    ASSERT_TRUE(process.has_value());
    if (!process)
        return;
    EXPECT_EQ(process->Executable(), ce::ansi_path::ModulePathW(nullptr));
    const auto module = GetModuleHandleW(L"kernel32.dll");
    ASSERT_NE(module, nullptr);
    auto library = RuntimePackagePaths::FromModule(module, {}, error);
    ASSERT_TRUE(library.has_value());
    if (!library)
        return;
    EXPECT_EQ(
        library->Executable(),
        (std::filesystem::path(ce::ansi_path::ModulePathW(module)).parent_path() / L"captureengine.exe").wstring());
    EXPECT_NE(library->Executable(), process->Executable());
}

TEST(RuntimePackagePathsTest, ExplicitConfigurationIsPreservedInsteadOfReturningToTheWorkerDefault) {
    PackagePathError error = PackagePathError::None;
    auto paths = RuntimePackagePaths::FromExecutable(
        L"C:\\runtime\\worker.exe", {ConfigurationArgumentStatus::Supplied, L"C:\\client settings\\recording.ini"},
        error);
    ASSERT_TRUE(paths.has_value());
    if (!paths)
        return;
    EXPECT_EQ(paths->ConfigurationWide(), L"C:\\client settings\\recording.ini");
    EXPECT_EQ(paths->Configuration(), "C:\\client settings\\recording.ini");
    EXPECT_EQ(paths->Directory(), "C:\\runtime");
}

TEST(RuntimePackagePathsTest, RelativeConfigurationBecomesAnAbsolutePathBeforeItCrossesProcesses) {
    PackagePathError error = PackagePathError::None;
    auto paths = RuntimePackagePaths::FromExecutable(
        L"C:\\runtime\\worker.exe", {ConfigurationArgumentStatus::Supplied, L"settings fixture.ini"}, error);
    ASSERT_TRUE(paths.has_value());
    if (!paths)
        return;
    EXPECT_EQ(paths->ConfigurationWide(), std::filesystem::absolute(L"settings fixture.ini").wstring());
    EXPECT_TRUE(std::filesystem::path(paths->ConfigurationWide()).is_absolute());
}

TEST(RuntimePackagePathsTest, InvalidExecutableAndExplicitInvalidConfigurationDoNotSelectDefaults) {
    PackagePathError error = PackagePathError::None;
    EXPECT_FALSE(RuntimePackagePaths::FromExecutable(L"worker.exe", {}, error));
    EXPECT_EQ(error, PackagePathError::InvalidExecutable);
    EXPECT_FALSE(RuntimePackagePaths::FromExecutable(L"C:\\runtime\\worker.exe",
                                                     {ConfigurationArgumentStatus::Invalid, {}}, error));
    EXPECT_EQ(error, PackagePathError::InvalidConfiguration);
    EXPECT_FALSE(RuntimePackagePaths::FromExecutable(L"C:\\runtime\\worker.exe",
                                                     {ConfigurationArgumentStatus::Supplied, {}}, error));
    const std::wstring embeddedNull(L"C:\\runtime\\worker.exe\0suffix", 28);
    EXPECT_FALSE(RuntimePackagePaths::FromExecutable(embeddedNull, {}, error));
}

TEST(RuntimePackagePathsTest, ExactArgumentsPreserveUnicodeAndSpacesWithoutSubstringMatches) {
    const auto supplied = ReadConfigurationArgument(
        {L"client.exe", L"--mode=media", L"--config=C:\\settings folder\\\x30b2\x30fc\x30e0.ini"});
    EXPECT_EQ(supplied.status, ConfigurationArgumentStatus::Supplied);
    EXPECT_EQ(supplied.path, L"C:\\settings folder\\\x30b2\x30fc\x30e0.ini");
    EXPECT_EQ(ReadConfigurationArgument({L"client.exe", L"--launch=game --config=wrong.ini"}).status,
              ConfigurationArgumentStatus::Missing);
    EXPECT_EQ(ReadConfigurationArgument({L"--config=ignored.exe"}).status, ConfigurationArgumentStatus::Missing);
}

TEST(RuntimePackagePathsTest, EmptyBareDuplicateAndEmbeddedNullOptionsAreRejected) {
    EXPECT_EQ(ReadConfigurationArgument({L"client.exe", L"--config="}).status, ConfigurationArgumentStatus::Invalid);
    EXPECT_EQ(ReadConfigurationArgument({L"client.exe", L"--config"}).status, ConfigurationArgumentStatus::Invalid);
    EXPECT_EQ(ReadConfigurationArgument({L"client.exe", L"--config=a.ini", L"--config=b.ini"}).status,
              ConfigurationArgumentStatus::Invalid);
    const std::wstring option(L"--config=a.ini\0suffix", 20);
    EXPECT_EQ(ReadConfigurationArgument({L"client.exe", option}).status, ConfigurationArgumentStatus::Invalid);
}

TEST(RuntimePackagePathsTest, DelegatedLaunchArgumentsCannotSelectOrInvalidateEngineConfiguration) {
    EXPECT_EQ(ReadConfigurationArgument({L"client.exe", L"--launch", L"fixture.exe", L"--config=game.ini"}).status,
              ConfigurationArgumentStatus::Missing);
    const auto selected = ReadConfigurationArgument(
        {L"client.exe", L"--config=engine.ini", L"--launch=fixture.exe", L"--config=game.ini", L"--config="});
    EXPECT_EQ(selected.status, ConfigurationArgumentStatus::Supplied);
    EXPECT_EQ(selected.path, L"engine.ini");
}

TEST(RuntimePackagePathsTest, ActiveCodePagePathsRoundTripThroughTheWideLaunchBoundary) {
    const std::wstring path = L"C:\\settings\\\x00fc.ini";
    std::string narrow;
    if (!ce::ansi_path::TryNarrowAcpExactly(path, &narrow))
        GTEST_SKIP() << "test glyph is unavailable in the active code page";
    EXPECT_EQ(ActiveCodePagePathToWide(narrow), path);
    EXPECT_TRUE(ActiveCodePagePathToWide(std::string("a\0b", 3)).empty());
}

TEST(RuntimePackagePathsTest, RelativeWorkerExecutableIsRejectedBeforeAProcessCanStart) {
    EXPECT_EQ(SpawnChildProcess(ProcessMode::Logger, "fixture.ini", nullptr, L"worker.exe"), nullptr);
}

TEST(RuntimePackagePathsTest, ActualRenamedHelperOutsideClientDirectoryLoadsTheRequestedIni) {
    const auto directory =
        std::filesystem::temp_directory_path() / ("runtime package fixture " + std::to_string(GetCurrentProcessId()));
    const auto worker = directory / L"runtime-helper-fixture.exe";
    const auto config =
        std::filesystem::absolute("runtime-config-probe." + std::to_string(GetCurrentProcessId()) + ".ini");
    struct Cleanup {
        std::filesystem::path worker;
        std::filesystem::path config;
        std::filesystem::path directory;
        HANDLE process = nullptr;
        // NOLINTNEXTLINE(bugprone-exception-escape) - only test failure reporting allocates; reporting OOM is fatal to this isolated probe.
        ~Cleanup() {
            if (process) {
                if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0) {
                    const bool stopped = TerminateProcess(process, ERROR_PROCESS_ABORTED) != FALSE;
                    EXPECT_TRUE(stopped);
                    if (stopped)
                        EXPECT_EQ(WaitForSingleObject(process, 10000), WAIT_OBJECT_0);
                }
                CloseHandle(process);
            }
            EXPECT_TRUE(DeleteFileW(worker.c_str()));
            EXPECT_TRUE(DeleteFileW(config.c_str()));
            EXPECT_TRUE(RemoveDirectoryW(directory.c_str()));
        }
    } cleanup{worker, config, directory};
    ASSERT_TRUE(CreateDirectoryW(directory.c_str(), nullptr));
    const auto image = ce::ansi_path::ModulePathW(nullptr);
    if (!CreateHardLinkW(worker.c_str(), image.c_str(), nullptr)) {
        ASSERT_EQ(GetLastError(), static_cast<DWORD>(ERROR_NOT_SAME_DEVICE));
        ASSERT_TRUE(CopyFileW(image.c_str(), worker.c_str(), TRUE));
    }
    {
        std::ofstream file(config, std::ios::binary);
        ASSERT_TRUE(file);
        file << "[Output]\r\noutput_dir=runtime-path-probe-value\r\n";
    }
    std::string configAcp;
    ASSERT_TRUE(ce::ansi_path::TryNarrowAcpExactly(config.wstring(), &configAcp));
    cleanup.process = SpawnChildProcess(ProcessMode::Logger, configAcp.c_str(), nullptr, worker.c_str());
    ASSERT_NE(cleanup.process, nullptr);
    ASSERT_EQ(WaitForSingleObject(cleanup.process, 15000), WAIT_OBJECT_0);
    DWORD exitCode = ERROR_PROCESS_ABORTED;
    ASSERT_TRUE(GetExitCodeProcess(cleanup.process, &exitCode));
    EXPECT_EQ(exitCode, 0u) << "helper startup must use the supplied INI rather than its own package default";
}

TEST(RuntimePackagePathsTest, ShippingEntryAndChildOwnerUseResolvedPathsAndHonorTheConfigArgument) {
    const auto root = std::filesystem::current_path();
    const auto entry = ce::test_source::ReadLogicalSource(root / "captureengine/app/main_entry.cpp");
    const auto owner = ce::test_source::ReadLogicalSource(root / "captureengine/app/host_children.cpp");
    const auto launcher = ce::test_source::ReadLogicalSource(root / "common/ipc/process_ipc_client.cpp");
    EXPECT_NE(entry.find("ReadProcessConfigurationArgument()"), std::string::npos);
    EXPECT_NE(entry.find("main_g_ConfigPath = paths->Configuration()"), std::string::npos);
    EXPECT_NE(entry.find("paths.Executable().c_str()"), std::string::npos);
    EXPECT_NE(owner.find("executable_.c_str()"), std::string::npos);
    EXPECT_NE(launcher.find("ActiveCodePagePathToWide(configPath)"), std::string::npos);
    EXPECT_EQ(launcher.find("Utf8ToWide(configPath)"), std::string::npos);
}
}  // namespace
