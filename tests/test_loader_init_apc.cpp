// Regression for the Witcher 3 Streamline takeover race (session 20261014_190537):
// a remote thread started during the target's loader initialization raced the
// image's own slInit call. These tests prove the APC route runs before the image
// entry point in both phases it accepts, using real child processes held at
// deterministic points (created suspended; stopped at a loader debug event).
#include "common/platform/loader_init_apc.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using ce::loader_init_apc::ClassifyPhase;
using ce::loader_init_apc::Phase;
using ce::loader_init_apc::PhaseName;
using ce::loader_init_apc::QueueBeforeEntryPoint;
using ce::loader_init_apc::ReadPhase;
using ce::loader_init_apc::RunsBeforeEntryPoint;

// cmd.exe exits 7 on its own; an APC exit code proves the APC ran first.
constexpr DWORD kImageExitCode = 7;
constexpr DWORD kChildDeadlineMs = 10000;

PAPCFUNC ExitProcessApc() {
    return reinterpret_cast<PAPCFUNC>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ExitProcess"));
}

std::wstring ChildCommandLine() {
    wchar_t system[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    return L"\"" + std::wstring(system) + L"\\cmd.exe\" /c exit " + std::to_wstring(kImageExitCode);
}

struct Child {
    PROCESS_INFORMATION info{};
    ~Child() {
        if (info.hProcess) {
            if (WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT) {
                TerminateProcess(info.hProcess, 1);
                WaitForSingleObject(info.hProcess, kChildDeadlineMs);
            }
            CloseHandle(info.hProcess);
        }
        if (info.hThread) {
            CloseHandle(info.hThread);
        }
    }
};

bool StartChild(DWORD flags, Child& child) {
    std::wstring command = ChildCommandLine();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    return !command.empty() && CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                                              flags | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child.info);
}

DWORD ExitCodeOf(HANDLE process) {
    DWORD code = 0;
    EXPECT_EQ(WaitForSingleObject(process, kChildDeadlineMs), WAIT_OBJECT_0);
    EXPECT_TRUE(GetExitCodeProcess(process, &code));
    return code;
}

TEST(LoaderInitApc, ClassifiesPebState) {
    EXPECT_EQ(ClassifyPhase(0, 0x2), Phase::NotStarted);
    EXPECT_EQ(ClassifyPhase(0, 0), Phase::NotStarted);
    EXPECT_EQ(ClassifyPhase(0x7FF000001000ull, 0x3), Phase::Initializing);
    EXPECT_EQ(ClassifyPhase(0x7FF000001000ull, 0x2), Phase::Initializing);
    // ProcessInJob alone (bit 0): initialization finished.
    EXPECT_EQ(ClassifyPhase(0x7FF000001000ull, 0x1), Phase::Complete);
    EXPECT_EQ(ClassifyPhase(0x7FF000001000ull, 0), Phase::Complete);
}

TEST(LoaderInitApc, OnlyPreEntryPhasesQueue) {
    EXPECT_TRUE(RunsBeforeEntryPoint(Phase::NotStarted));
    EXPECT_TRUE(RunsBeforeEntryPoint(Phase::Initializing));
    EXPECT_FALSE(RunsBeforeEntryPoint(Phase::Complete));
    EXPECT_FALSE(RunsBeforeEntryPoint(Phase::Unsupported));
    EXPECT_FALSE(RunsBeforeEntryPoint(Phase::Unreadable));
    EXPECT_STREQ(PhaseName(Phase::Initializing), "initializing");
}

TEST(LoaderInitApc, ReadsOwnProcessAsComplete) {
    DWORD error = 0;
    EXPECT_EQ(ReadPhase(GetCurrentProcess(), &error), Phase::Complete) << error;
}

TEST(LoaderInitApc, RefusesTheCallingProcess) {
    const auto result = QueueBeforeEntryPoint(GetCurrentProcess(), GetCurrentProcessId(), ExitProcessApc(), 1);
    EXPECT_FALSE(result.queued);
    EXPECT_EQ(result.phase, Phase::Unsupported);
}

TEST(LoaderInitApc, NotStartedTargetRunsApcBeforeEntryPoint) {
    Child child;
    ASSERT_TRUE(StartChild(CREATE_SUSPENDED, child)) << GetLastError();
    const auto result = QueueBeforeEntryPoint(child.info.hProcess, child.info.dwProcessId, ExitProcessApc(), 42);
    EXPECT_EQ(result.phase, Phase::NotStarted) << PhaseName(result.phase) << " error=" << result.error;
    EXPECT_EQ(result.initialThreadId, child.info.dwThreadId);
    ASSERT_TRUE(result.queued) << result.error;
    ASSERT_NE(ResumeThread(child.info.hThread), static_cast<DWORD>(-1));
    EXPECT_EQ(ExitCodeOf(child.info.hProcess), 42u);
}

// The poll-discovered case: the target is inside LdrpInitializeProcess. A loader
// debug event holds it there deterministically (every thread is frozen until
// ContinueDebugEvent).
TEST(LoaderInitApc, InitializingTargetRunsApcBeforeEntryPoint) {
    Child child;
    ASSERT_TRUE(StartChild(DEBUG_ONLY_THIS_PROCESS, child)) << GetLastError();
    ce::loader_init_apc::Result queuedResult;
    DWORD exitCode = 0;
    bool exited = false;
    while (!exited) {
        DEBUG_EVENT event{};
        ASSERT_TRUE(WaitForDebugEvent(&event, kChildDeadlineMs)) << GetLastError();
        DWORD status = DBG_CONTINUE;
        switch (event.dwDebugEventCode) {
            case CREATE_PROCESS_DEBUG_EVENT:
                if (event.u.CreateProcessInfo.hFile) {
                    CloseHandle(event.u.CreateProcessInfo.hFile);
                }
                break;
            case LOAD_DLL_DEBUG_EVENT:
                if (event.u.LoadDll.hFile) {
                    CloseHandle(event.u.LoadDll.hFile);
                }
                if (!queuedResult.queued && ReadPhase(child.info.hProcess) == Phase::Initializing) {
                    queuedResult =
                        QueueBeforeEntryPoint(child.info.hProcess, child.info.dwProcessId, ExitProcessApc(), 43);
                }
                break;
            case EXCEPTION_DEBUG_EVENT:
                if (event.u.Exception.ExceptionRecord.ExceptionCode != EXCEPTION_BREAKPOINT) {
                    status = DBG_EXCEPTION_NOT_HANDLED;
                }
                break;
            case EXIT_PROCESS_DEBUG_EVENT:
                exitCode = event.u.ExitProcess.dwExitCode;
                exited = true;
                break;
            default:
                break;
        }
        ASSERT_TRUE(ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status));
    }
    EXPECT_EQ(queuedResult.phase, Phase::Initializing) << PhaseName(queuedResult.phase);
    EXPECT_EQ(queuedResult.initialThreadId, child.info.dwThreadId);
    EXPECT_TRUE(queuedResult.queued) << queuedResult.error;
    EXPECT_EQ(exitCode, 43u) << "the image entry point ran before the APC (cmd exits " << kImageExitCode << ")";
}

}  // namespace
