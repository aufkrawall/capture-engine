#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "common/platform/raii_helpers.h"
#include "common/platform/restricted_child_process.h"
#include "source_fragment_reader.h"

namespace {

std::wstring CommandInterpreter() {
    wchar_t directory[MAX_PATH]{};
    const UINT count = GetSystemDirectoryW(directory, MAX_PATH);
    EXPECT_GT(count, 0u);
    EXPECT_LT(count, static_cast<UINT>(MAX_PATH));
    return std::wstring(directory) + L"\\cmd.exe";
}

ce::HandleGuard CreateOwnerJob() {
    ce::HandleGuard job(CreateJobObjectW(nullptr, nullptr));
    EXPECT_TRUE(job);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    EXPECT_TRUE(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)));
    return job;
}

}  // namespace

TEST(ChildProcessOwnerJobTest, JobMembershipExistsBeforeSuspendedChildRunsAndClosingOwnerReapsIt) {
    const std::wstring executable = CommandInterpreter();
    for (bool inheritEvent : {false, true}) {
        auto job = CreateOwnerJob();
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        ce::HandleGuard event(CreateEventW(&attributes, TRUE, FALSE, nullptr));
        ASSERT_TRUE(event);
        const std::vector<HANDLE> inherited = inheritEvent ? std::vector<HANDLE>{event.get()} : std::vector<HANDLE>{};
        ce::process::RestrictedChildProcess child;
        DWORD error = ERROR_SUCCESS;
        ASSERT_TRUE(ce::process::LaunchRestrictedChildProcess(
            executable, L"\"" + executable + L"\" /c exit 7", L"", inherited,
            CREATE_NO_WINDOW | CREATE_SUSPENDED, child, error, job.get())) << error;
        BOOL inJob = FALSE;
        EXPECT_TRUE(IsProcessInJob(child.processHandle, job.get(), &inJob));
        EXPECT_TRUE(inJob);
        EXPECT_EQ(WaitForSingleObject(child.processHandle, 0), WAIT_TIMEOUT);
        DWORD flags = HANDLE_FLAG_INHERIT;
        EXPECT_TRUE(GetHandleInformation(job.get(), &flags));
        EXPECT_EQ(flags & HANDLE_FLAG_INHERIT, 0u);

        // Deterministic owner loss: no sleeps, no executing child and no system-wide state.
        job.reset();
        const DWORD stopped = WaitForSingleObject(child.processHandle, 5000);
        EXPECT_EQ(stopped, WAIT_OBJECT_0);
        if (stopped != WAIT_OBJECT_0) {
            TerminateProcess(child.processHandle, ERROR_PROCESS_ABORTED);
            EXPECT_EQ(WaitForSingleObject(child.processHandle, 5000), WAIT_OBJECT_0);
        }
    }
}

TEST(ChildProcessOwnerJobTest, OwnedChildCanExitNormallyAndUnownedLaunchIsCompatible) {
    const std::wstring executable = CommandInterpreter();
    for (bool owned : {false, true}) {
        auto job = CreateOwnerJob();
        ce::process::RestrictedChildProcess child;
        DWORD error = ERROR_SUCCESS;
        ASSERT_TRUE(ce::process::LaunchRestrictedChildProcess(
            executable, L"\"" + executable + L"\" /c exit 7", L"", {}, CREATE_NO_WINDOW,
            child, error, owned ? job.get() : nullptr)) << error;
        const DWORD stopped = WaitForSingleObject(child.processHandle, 5000);
        EXPECT_EQ(stopped, WAIT_OBJECT_0);
        if (stopped != WAIT_OBJECT_0) {
            TerminateProcess(child.processHandle, ERROR_PROCESS_ABORTED);
            EXPECT_EQ(WaitForSingleObject(child.processHandle, 5000), WAIT_OBJECT_0);
        }
        DWORD exitCode = STILL_ACTIVE;
        EXPECT_TRUE(GetExitCodeProcess(child.processHandle, &exitCode));
        EXPECT_EQ(exitCode, 7u);
    }
}

TEST(ChildProcessOwnerJobTest, InvalidJobNeverLeavesAChildRunning) {
    const std::wstring executable = CommandInterpreter();
    ce::process::RestrictedChildProcess child;
    DWORD error = ERROR_SUCCESS;
    const bool launched = ce::process::LaunchRestrictedChildProcess(
        executable, L"\"" + executable + L"\" /c exit 7", L"", {}, CREATE_NO_WINDOW,
        child, error, INVALID_HANDLE_VALUE);
    EXPECT_FALSE(launched);
    EXPECT_EQ(child.processHandle, nullptr);
    EXPECT_NE(error, ERROR_SUCCESS);
    if (launched) {
        TerminateProcess(child.processHandle, ERROR_PROCESS_ABORTED);
        EXPECT_EQ(WaitForSingleObject(child.processHandle, 5000), WAIT_OBJECT_0);
    }
}

TEST(ChildProcessOwnerJobTest, AudioWorkerOwnsTheNonInheritedKillOnCloseJob) {
    const std::string source = ce::test_source::ReadLogicalSource(
        std::filesystem::current_path() / "mediaengine" / "audio" / "process_loopback_capture.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE"), std::string::npos);
    EXPECT_NE(source.find("CloseHandle(ownershipJob)"), std::string::npos);
    EXPECT_NE(source.find("worker->ownershipJob))"), std::string::npos);
    EXPECT_NE(source.find("ownerLifetime=job"), std::string::npos);
}
