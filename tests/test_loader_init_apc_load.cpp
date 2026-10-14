#include "common/platform/loader_init_apc_load.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using namespace ce::loader_init_apc;

struct Child {
    PROCESS_INFORMATION info{};
    Child() {
        wchar_t system[MAX_PATH]{};
        GetSystemDirectoryW(system, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(system) + L"\\cmd.exe\" /c exit 7";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        EXPECT_TRUE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                                   CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info));
    }
    ~Child() {
        if (info.hProcess) {
            TerminateProcess(info.hProcess, 1);
            WaitForSingleObject(info.hProcess, 10000);
            CloseHandle(info.hProcess);
        }
        if (info.hThread)
            CloseHandle(info.hThread);
    }
    void ResumeAndHoldAfterLoad() {
        // This second APC suspends its own thread after the load acknowledgement,
        // before cmd's entry point can exit. No sleeps or scheduling assumptions.
        const auto suspend = reinterpret_cast<PAPCFUNC>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SuspendThread"));
        ASSERT_TRUE(QueueUserAPC(suspend, info.hThread, reinterpret_cast<ULONG_PTR>(GetCurrentThread())));
        ASSERT_NE(ResumeThread(info.hThread), static_cast<DWORD>(-1));
    }
};

void* LoadLibraryAddress() {
    return reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
}

TEST(LoaderInitApcLoad, ReportsLoadedOnlyAfterRemoteLoadCompletes) {
    Child child;
    ASSERT_NE(child.info.hProcess, nullptr);
    LoadRequest request;
    const auto queued = request.Queue(child.info.hProcess, child.info.dwProcessId, L"kernel32.dll", LoadLibraryAddress());
    ASSERT_TRUE(queued.queued) << queued.error;
    EXPECT_EQ(queued.phase, Phase::NotStarted);
    EXPECT_EQ(request.ReadResult().state, LoadState::Pending);
    child.ResumeAndHoldAfterLoad();
    ASSERT_EQ(WaitForSingleObject(request.CompletionEvent(), 10000), WAIT_OBJECT_0);
    const auto result = request.ReadResult();
    EXPECT_EQ(result.state, LoadState::Loaded) << result.error;
    EXPECT_NE(result.module, 0u);
    EXPECT_EQ(result.error, ERROR_SUCCESS);
    // Releasing the acknowledged executable allocation must not crash the child.
}

TEST(LoaderInitApcLoad, MissingDllReportsFailureAndWindowsError) {
    Child child;
    ASSERT_NE(child.info.hProcess, nullptr);
    LoadRequest request;
    ASSERT_TRUE(request.Queue(child.info.hProcess, child.info.dwProcessId,
                               L"CE_missing_apc_regression.dll", LoadLibraryAddress()).queued);
    child.ResumeAndHoldAfterLoad();
    ASSERT_EQ(WaitForSingleObject(request.CompletionEvent(), 10000), WAIT_OBJECT_0);
    const auto result = request.ReadResult();
    EXPECT_EQ(result.state, LoadState::Failed);
    EXPECT_EQ(result.module, 0u);
    EXPECT_EQ(result.error, ERROR_MOD_NOT_FOUND);
}

TEST(LoaderInitApcLoad, AbandoningQueuedRequestDoesNotFreeFutureInstructions) {
    Child child;
    ASSERT_NE(child.info.hProcess, nullptr);
    {
        LoadRequest request;
        ASSERT_TRUE(request.Queue(child.info.hProcess, child.info.dwProcessId, L"kernel32.dll", LoadLibraryAddress()).queued);
        EXPECT_EQ(request.ReadResult().state, LoadState::Pending);
    }
    // Queue an exit behind the abandoned load. Its exit code proves the load
    // returned through valid storage even after the host discarded its request.
    const auto exit = reinterpret_cast<PAPCFUNC>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ExitProcess"));
    ASSERT_TRUE(QueueUserAPC(exit, child.info.hThread, 42));
    ASSERT_NE(ResumeThread(child.info.hThread), static_cast<DWORD>(-1));
    ASSERT_EQ(WaitForSingleObject(child.info.hProcess, 10000), WAIT_OBJECT_0);
    DWORD code = 0;
    ASSERT_TRUE(GetExitCodeProcess(child.info.hProcess, &code));
    EXPECT_EQ(code, 42u);
}

TEST(LoaderInitApcLoad, RejectsCallingProcessWithoutAllocating) {
    LoadRequest request;
    EXPECT_FALSE(request.Queue(GetCurrentProcess(), GetCurrentProcessId(), L"kernel32.dll", LoadLibraryAddress()).queued);
    EXPECT_EQ(request.CompletionEvent(), nullptr);
    EXPECT_EQ(request.ReadResult().state, LoadState::Failed);
}

TEST(LoaderInitApcLoad, ExitedTargetCannotRemainPendingForever) {
    Child child;
    ASSERT_NE(child.info.hProcess, nullptr);
    LoadRequest request;
    ASSERT_TRUE(request.Queue(child.info.hProcess, child.info.dwProcessId, L"kernel32.dll", LoadLibraryAddress()).queued);
    ASSERT_TRUE(TerminateProcess(child.info.hProcess, 1));
    ASSERT_EQ(WaitForSingleObject(child.info.hProcess, 10000), WAIT_OBJECT_0);
    EXPECT_EQ(request.ReadResult().state, LoadState::Failed);
    EXPECT_EQ(request.ReadResult().error, ERROR_PROCESS_ABORTED);
}

}  // namespace
