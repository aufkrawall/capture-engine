// Audit 4, item 6: the logger child waited only on the controller's shutdown
// event. A hard controller exit (crash, TerminateProcess) never sets it, so the
// logger was orphaned and competed with the next controller's logger for the
// shared log rings. The logger now waits on the controller process too, through
// the same helper the tests drive here. A thread handle stands in for the
// controller process: both are waitable and signal when they end.

#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <string>

#include "../captureengine/service_lifetime_wait.h"
#include "source_fragment_reader.h"

namespace lifetime = ce::service_lifetime;

namespace {

DWORD WINAPI WaitForReleaseThread(LPVOID parameter) {
    WaitForSingleObject(static_cast<HANDLE>(parameter), INFINITE);
    return 0;
}

}  // namespace

TEST(ServiceLifetimeWaitTest, HardControllerExitEndsTheServiceAndARestartIsWatchedAgain) {
    HANDLE shutdown = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(shutdown, nullptr);
    ASSERT_NE(release, nullptr);

    // First controller: alive -> the service keeps running.
    HANDLE controller = CreateThread(nullptr, 0, WaitForReleaseThread, release, 0, nullptr);
    ASSERT_NE(controller, nullptr);
    EXPECT_EQ(lifetime::WaitForServiceLifetime(shutdown, controller, 0), lifetime::WaitOutcome::kTimeout);

    // Hard exit: no shutdown signal, the controller simply ends.
    SetEvent(release);
    ASSERT_EQ(WaitForSingleObject(controller, INFINITE), WAIT_OBJECT_0);
    EXPECT_EQ(lifetime::WaitForServiceLifetime(shutdown, controller, 0), lifetime::WaitOutcome::kControllerExited);
    CloseHandle(controller);

    // Restart: a new controller is watched by its own logger; an orderly stop wins.
    ResetEvent(release);
    HANDLE restarted = CreateThread(nullptr, 0, WaitForReleaseThread, release, 0, nullptr);
    ASSERT_NE(restarted, nullptr);
    EXPECT_EQ(lifetime::WaitForServiceLifetime(shutdown, restarted, 0), lifetime::WaitOutcome::kTimeout);
    SetEvent(shutdown);
    EXPECT_EQ(lifetime::WaitForServiceLifetime(shutdown, restarted, 0), lifetime::WaitOutcome::kShutdownSignaled);
    SetEvent(release);
    WaitForSingleObject(restarted, INFINITE);
    CloseHandle(restarted);

    // No handles: a plain bounded wait.
    EXPECT_EQ(lifetime::WaitForServiceLifetime(nullptr, nullptr, 0), lifetime::WaitOutcome::kTimeout);
    CloseHandle(release);
    CloseHandle(shutdown);
}

TEST(ServiceLifetimeWaitTest, LoggerWatchesTheControllerProcess) {
    const std::string logger =
        ce::test_source::ReadFile(std::filesystem::current_path() / "captureengine" / "logger_service.cpp");
    ASSERT_FALSE(logger.empty());
    EXPECT_NE(logger.find("OpenProcess(SYNCHRONIZE, FALSE, controllerPid)"), std::string::npos);
    EXPECT_NE(logger.find("WaitForServiceLifetime(hShutdownEvent, hControllerProcess, waitMs)"), std::string::npos);
    EXPECT_NE(logger.find("WaitOutcome::kControllerExited"), std::string::npos);
    EXPECT_EQ(logger.find("WaitForSingleObject(hShutdownEvent"), std::string::npos);
}
