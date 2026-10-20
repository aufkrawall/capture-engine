#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "captureengine/media/media_role_context.h"

namespace {

HANDLE ManualResetEvent(bool signaled) {
    return CreateEventW(nullptr, TRUE, signaled ? TRUE : FALSE, nullptr);
}

}  // namespace

TEST(MediaRoleContextTest, ConfigAndCacheFollowThePackageDirectoryWhenNoPathWasGiven) {
    const ce::media_role::RolePaths paths =
        ce::media_role::DeriveRolePaths(L"C:\\Games\\CaptureEngine\\bin", std::string());

    EXPECT_EQ(paths.configPath, "C:\\Games\\CaptureEngine\\bin\\config.ini");
    EXPECT_EQ(paths.mediaCacheDir, "C:\\Games\\CaptureEngine\\bin");
}

TEST(MediaRoleContextTest, AGivenConfigPathIsKeptAndItsDirectoryIsTheCacheDirectory) {
    const ce::media_role::RolePaths paths =
        ce::media_role::DeriveRolePaths(L"C:\\Games\\CaptureEngine\\bin", "D:\\Settings\\capture.ini");

    EXPECT_EQ(paths.configPath, "D:\\Settings\\capture.ini");
    EXPECT_EQ(paths.mediaCacheDir, "D:\\Settings");
}

TEST(MediaRoleContextTest, ForwardSlashesSeparateTheCacheDirectoryToo) {
    const ce::media_role::RolePaths paths = ce::media_role::DeriveRolePaths(L"C:\\pkg", "C:/pkg/settings/config.ini");

    EXPECT_EQ(paths.configPath, "C:/pkg/settings/config.ini");
    EXPECT_EQ(paths.mediaCacheDir, "C:/pkg/settings");
}

TEST(MediaRoleContextTest, WaitEndsTheLoopOnlyWhenTheStopEventIsSignaled) {
    HANDLE unsignaled = ManualResetEvent(false);
    ASSERT_NE(unsignaled, nullptr);
    // A zero timeout observes the event's state without waiting.
    EXPECT_FALSE(ce::media_role::WaitForPollOrStop(unsignaled, 0));

    ASSERT_NE(SetEvent(unsignaled), FALSE);
    EXPECT_TRUE(ce::media_role::WaitForPollOrStop(unsignaled, 5000));
    EXPECT_TRUE(ce::media_role::WaitForPollOrStop(unsignaled, 0));
    CloseHandle(unsignaled);
}

TEST(MediaRoleContextTest, ProcessTuningIsAppliedOnlyWhenTheContextCarriesIt) {
    std::atomic<bool> loggedRefusal{false};
    EXPECT_TRUE(ce::media_role::AllowsProcessTuning(true, loggedRefusal));
    EXPECT_FALSE(loggedRefusal.load());

    // A client's process keeps its priority class and GPU scheduling; the refusal is recorded for its one log line.
    EXPECT_FALSE(ce::media_role::AllowsProcessTuning(false, loggedRefusal));
    EXPECT_TRUE(loggedRefusal.load());
    EXPECT_FALSE(ce::media_role::AllowsProcessTuning(false, loggedRefusal));
}

TEST(MediaRoleContextTest, WaitWithoutAStopEventJustPolls) {
    EXPECT_FALSE(ce::media_role::WaitForPollOrStop(nullptr, 0));
}

TEST(MediaRoleContextTest, WaitOnAnUnusableHandleDoesNotEndTheLoop) {
    const HANDLE notAHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x5E5E));
    EXPECT_FALSE(ce::media_role::WaitForPollOrStop(notAHandle, 0));
}
