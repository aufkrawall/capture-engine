#include <gtest/gtest.h>

#include "reserved_capture_output.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

class ReservedCaptureOutputTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory =
            std::filesystem::temp_directory_path() / (L"capture_output_test_" + std::to_wstring(GetCurrentProcessId()) +
                                                      L"_" + std::to_wstring(GetTickCount64()));
        ASSERT_TRUE(std::filesystem::create_directories(directory));
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    std::filesystem::path directory;
};

}  // namespace

class ReservedCaptureOutputCollisionTest : public ReservedCaptureOutputTest,
                                           public ::testing::WithParamInterface<const wchar_t*> {};

TEST_P(ReservedCaptureOutputCollisionTest, IdenticalSeedsRetryWithoutChangingExistingFile) {
    const ce::capture_output::OutputNameSeed seed{13380163200123ull, 42, 7};
    auto first = ce::capture_output::ReservedCaptureOutput::ReserveForTesting(directory, L"capture", GetParam(), seed);
    ASSERT_TRUE(first);
    const std::filesystem::path sentinelPath = first.Path();
    ASSERT_TRUE(first.ReleaseToWriter());
    {
        std::ofstream sentinel(sentinelPath, std::ios::binary | std::ios::trunc);
        sentinel << "sentinel-bytes";
    }
    first.Publish();

    auto second = ce::capture_output::ReservedCaptureOutput::ReserveForTesting(directory, L"capture", GetParam(), seed);
    ASSERT_TRUE(second);
    EXPECT_NE(second.Path(), sentinelPath);

    std::ifstream sentinel(sentinelPath, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(sentinel)), std::istreambuf_iterator<char>());
    EXPECT_EQ(bytes, "sentinel-bytes");
}

INSTANTIATE_TEST_SUITE_P(VideoAudioAndScreenshots, ReservedCaptureOutputCollisionTest,
                         ::testing::Values(L"mkv", L"mka", L"png", L"avif"));

TEST_F(ReservedCaptureOutputTest, ReservationPreventsReplacementUntilPublication) {
    const ce::capture_output::OutputNameSeed seed{13380163200456ull, 43, 8};
    auto output = ce::capture_output::ReservedCaptureOutput::ReserveForTesting(directory, L"capture", L"mka", seed);
    ASSERT_TRUE(output);
    const std::filesystem::path path = output.Path();
    ASSERT_TRUE(output.ReleaseToWriter());
    const BOOL deletedWhileReserved = DeleteFileW(path.c_str());
    const DWORD deleteError = GetLastError();
    EXPECT_FALSE(deletedWhileReserved);
    EXPECT_EQ(deleteError, ERROR_SHARING_VIOLATION);

    output.Publish();
    ASSERT_TRUE(DeleteFileW(path.c_str()));
    {
        std::ofstream replacement(path, std::ios::binary);
        replacement << "replacement";
    }

    EXPECT_TRUE(std::filesystem::exists(path));
}

TEST_F(ReservedCaptureOutputTest, AtomicCommitPublishesOnlyOwnedStagingReservation) {
    const ce::capture_output::OutputNameSeed finalSeed{13380163200456ull, 43, 9};
    const ce::capture_output::OutputNameSeed stagingSeed{13380163200456ull, 43, 10};
    auto output =
        ce::capture_output::ReservedCaptureOutput::ReserveForTesting(directory, L"capture", L"avif", finalSeed);
    auto staging =
        ce::capture_output::ReservedCaptureOutput::ReserveForTesting(directory, L"capture_stage", L"part", stagingSeed);
    ASSERT_TRUE(output);
    ASSERT_TRUE(staging);
    const std::filesystem::path outputPath = output.Path();
    const std::filesystem::path stagingPath = staging.Path();
    ASSERT_TRUE(staging.ReleaseToWriter());
    {
        std::ofstream writer(stagingPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(writer);
        writer << "complete-staging-payload";
    }

    ASSERT_TRUE(output.CommitStagingFile(staging));
    EXPECT_FALSE(std::filesystem::exists(stagingPath));
    std::ifstream published(outputPath, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(published)), std::istreambuf_iterator<char>());
    EXPECT_EQ(bytes, "complete-staging-payload");
}

TEST_F(ReservedCaptureOutputTest, RejectsFilenameComponentsThatCouldEscapeTheCaptureDirectory) {
    const ce::capture_output::OutputNameSeed seed{13380163200456ull, 43, 11};
    EXPECT_FALSE(ce::capture_output::ReservedCaptureOutput::ReserveForTesting(directory, L"../capture", L"mkv", seed));
    EXPECT_FALSE(ce::capture_output::ReservedCaptureOutput::ReserveForTesting(directory, L"capture", L"../mkv", seed));
    EXPECT_TRUE(std::filesystem::is_empty(directory));
}
