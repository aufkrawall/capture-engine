#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../captureengine/pawnio_setup.h"

namespace {

std::filesystem::path CreateTempTestFile(const std::string& content) {
    const std::filesystem::path tempPath =
        std::filesystem::temp_directory_path() / ("ce_test_pawnio_" + std::to_string(GetTickCount64()) + ".bin");
    std::ofstream out(tempPath, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return tempPath;
}

}  // namespace

TEST(PawnIoSetupTest, CommandAndRegistryConstantsArePreserved) {
    EXPECT_STREQ(ce::pawnio::kInstallCommand, L"--install-pawnio");
    EXPECT_STREQ(ce::pawnio::kUninstallCommand, L"--uninstall-pawnio");
    EXPECT_STREQ(ce::pawnio::kDriverServiceKey, L"SYSTEM\\CurrentControlSet\\Services\\PawnIO");
    EXPECT_STREQ(ce::pawnio::kUninstallKey, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO");
    EXPECT_STREQ(ce::pawnio::kSuppressionKey, L"Software\\CaptureEngine");
    EXPECT_STREQ(ce::pawnio::kSuppressionValue, L"PawnIoPromptSuppressed");
    EXPECT_STREQ(ce::pawnio::kPawnIoExpectedSha256,
                 L"1f519a22e47187f70a1379a48ca604981c4fcf694f4e65b734aaa74a9fba3032");
}

TEST(PawnIoSetupTest, ComputeFileSha256MatchesEmptyAndKnownFiles) {
    const std::filesystem::path emptyFile = CreateTempTestFile("");
    const std::wstring emptyHash = ce::pawnio::ComputeFileSha256(emptyFile);
    std::filesystem::remove(emptyFile);
    EXPECT_EQ(emptyHash, L"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    const std::filesystem::path nonExistent = "non_existent_ce_pawnio_path_12345.bin";
    EXPECT_TRUE(ce::pawnio::ComputeFileSha256(nonExistent).empty());
}

TEST(PawnIoSetupTest, RejectsMissingOrCorruptedBinary) {
    EXPECT_FALSE(ce::pawnio::VerifyPawnIoSetupBinary("non_existent_file.exe"));

    const std::filesystem::path smallFile = CreateTempTestFile("MZsmall");
    EXPECT_FALSE(ce::pawnio::VerifyPawnIoSetupBinary(smallFile));
    std::filesystem::remove(smallFile);

    // File without valid Authenticode signature
    std::string fakeExe(1000000, '\0');
    fakeExe[0] = 'M';
    fakeExe[1] = 'Z';
    const std::filesystem::path unsignedExe = CreateTempTestFile(fakeExe);
    EXPECT_FALSE(ce::pawnio::VerifyPawnIoSetupBinary(unsignedExe));
    std::filesystem::remove(unsignedExe);
}

TEST(PawnIoSetupTest, ValidatesStagedPawnIoSetupBinaryIfPresent) {
    const std::filesystem::path repoPawnIo =
        std::filesystem::current_path() / "plugins" / "LibreHardwareMonitor" / "PawnIO_setup.exe";
    if (std::filesystem::exists(repoPawnIo)) {
        EXPECT_TRUE(ce::pawnio::VerifyPawnIoSetupBinary(repoPawnIo));
        EXPECT_EQ(ce::pawnio::ComputeFileSha256(repoPawnIo), ce::pawnio::kPawnIoExpectedSha256);
    }
}
