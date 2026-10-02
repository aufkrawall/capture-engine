#include <gtest/gtest.h>
#include "captureengine/sensors/pawnio_workers.h"
#include "common/ipc/elevation_windows.h"
#include <atomic>

TEST(PawnioShutdownTest, WaitsForOwnedWorkerFilesAndRefusesNewSetupDuringTeardown) {
    wchar_t directory[MAX_PATH]{};
    wchar_t original[MAX_PATH]{};
    ASSERT_NE(GetTempPathW(MAX_PATH, directory), 0);
    ASSERT_NE(GetTempFileNameW(directory, L"CEW", 0, original), 0);
    const std::wstring renamed = std::wstring(original) + L".released";
    ce::elevation::Handle ready(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ce::elevation::Handle finish(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    std::atomic<bool> opened{false};
    ce::pawnio::LaunchSetupWorker([&] {
        ce::elevation::Handle file(CreateFileW(original, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                               FILE_ATTRIBUTE_NORMAL, nullptr));
        opened.store(static_cast<bool>(file));
        SetEvent(ready.Get());
        WaitForSingleObject(finish.Get(), INFINITE);
    });
    const DWORD started = WaitForSingleObject(ready.Get(), 5000);
    if (started == WAIT_OBJECT_0)
        EXPECT_FALSE(MoveFileW(original, renamed.c_str()));
    SetEvent(finish.Get());
    ce::pawnio::ShutdownSetupWorkers();
    EXPECT_EQ(started, WAIT_OBJECT_0);
    EXPECT_TRUE(opened.load());
    EXPECT_TRUE(MoveFileW(original, renamed.c_str()));
    bool launchedAfterClose = false;
    ce::pawnio::LaunchSetupWorker([&] { launchedAfterClose = true; });
    ce::pawnio::ShutdownSetupWorkers();
    EXPECT_FALSE(launchedAfterClose);
    DeleteFileW(original);
    DeleteFileW(renamed.c_str());
}
