#include <gtest/gtest.h>

#include <windows.h>

#include "hook/runtime/process_thread_walk.h"
#include "hook/runtime/tool_thread_suspension.h"
#include "tests/exact_start_thread_support.h"

namespace {

using ce::third_party_load::ToolThreadSuspension;

DWORD WINAPI ToolThreadMain(LPVOID event) {
    WaitForSingleObject(static_cast<HANDLE>(event), INFINITE);
    return 0;
}

// Differs from ToolThreadMain so identical-code folding cannot merge the two start addresses.
DWORD WINAPI GameThreadMain(LPVOID event) {
    WaitForSingleObject(static_cast<HANDLE>(event), INFINITE);
    return 1;
}

// A thread parked on an event; joined on destruction.
class ParkedThread {
public:
    explicit ParkedThread(LPTHREAD_START_ROUTINE start) : release_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        // Not CreateThread: ToolThreadSuspension matches the kernel's start address, which ASan's thread thunk
        // would replace (see exact_start_thread_support.h).
        thread_ = ce::test_support::CreateThreadWithExactStart(start, release_);
        if (!thread_)
            ADD_FAILURE() << "CreateThreadWithExactStart failed";
    }
    ~ParkedThread() {
        SetEvent(release_);
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        CloseHandle(release_);
    }
    HANDLE handle() const {
        return thread_;
    }

private:
    HANDLE release_ = nullptr;
    HANDLE thread_ = nullptr;
};

DWORD SuspendCountOf(HANDLE thread) {
    DWORD count = 0;
    EXPECT_TRUE(ce::process_threads::QueryThreadSuspendCount(thread, &count));
    return count;
}

// The "loaded tool module": exactly the tool thread's start routine.
ToolThreadSuspension::ModuleRanges ToolModule() {
    return {{reinterpret_cast<uintptr_t>(&ToolThreadMain), 1}};
}

}  // namespace

// Only the tool's thread is suspended. The preload retries Suspend() after ResumeAll() when a tool thread was
// caught inside the loader; ResumeAll() kept the processed count, so every retry re-found the same threads and
// suspended none of them.
TEST(ToolThreadSuspensionTest, RetryAfterResumeAllSuspendsAgain) {
    ParkedThread tool(&ToolThreadMain);
    ParkedThread game(&GameThreadMain);
    ToolThreadSuspension suspension(ToolModule());
    ASSERT_TRUE(suspension.Suspend());
    EXPECT_EQ(suspension.SuspendedThreads(), 1u);
    EXPECT_EQ(SuspendCountOf(tool.handle()), 1u);
    EXPECT_EQ(SuspendCountOf(game.handle()), 0u);
    suspension.ResumeAll();
    EXPECT_EQ(SuspendCountOf(tool.handle()), 0u);
    ASSERT_TRUE(suspension.Suspend());
    EXPECT_EQ(SuspendCountOf(tool.handle()), 1u);
    EXPECT_EQ(SuspendCountOf(game.handle()), 0u);
    suspension.ResumeAll();
    EXPECT_EQ(SuspendCountOf(tool.handle()), 0u);
}

// A tool thread another owner holds at MAXIMUM_SUSPEND_COUNT (as Heroes of the Storm does with one of its own
// threads) is stopped: it must neither fail the suspension nor lose one of its owner's suspensions on resume.
TEST(ToolThreadSuspensionTest, ToolThreadHeldByAnotherOwnerIsLeftAlone) {
    ParkedThread tool(&ToolThreadMain);
    DWORD held = 0;
    while (SuspendThread(tool.handle()) != static_cast<DWORD>(-1))
        ++held;
    ASSERT_EQ(held, static_cast<DWORD>(MAXIMUM_SUSPEND_COUNT));
    {
        ToolThreadSuspension suspension(ToolModule());
        EXPECT_TRUE(suspension.Suspend());
        EXPECT_EQ(suspension.HeldThreads(), 1u);
        suspension.ResumeAll();
        EXPECT_EQ(SuspendCountOf(tool.handle()), held);
        EXPECT_TRUE(suspension.Suspend());
    }
    EXPECT_EQ(SuspendCountOf(tool.handle()), held);
    while (held > 0 && ResumeThread(tool.handle()) != static_cast<DWORD>(-1))
        --held;
    EXPECT_EQ(SuspendCountOf(tool.handle()), 0u);
}
