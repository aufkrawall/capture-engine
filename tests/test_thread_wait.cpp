#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "../common/thread_wait.h"

namespace {

// std::thread::native_handle() only yields a waitable Win32 HANDLE under the
// Win32 threading model. A winpthreads build returns a pthread_t, and the old
// reinterpret_cast<HANDLE> of that value made WaitForSingleObject fail with
// ERROR_INVALID_HANDLE - so every bounded join in a cross-built binary silently
// took its timeout/failure path. These cover both directions of that contract.

TEST(ThreadWait, HandleOfAFinishedThreadSignals) {
    std::thread worker([] {});
    const HANDLE handle = ce::Win32ThreadHandle(worker);

    ASSERT_NE(handle, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    EXPECT_EQ(WaitForSingleObject(handle, 10000), static_cast<DWORD>(WAIT_OBJECT_0));

    worker.join();
}

TEST(ThreadWait, HandleOfARunningThreadTimesOutInsteadOfFailing) {
    std::atomic<bool> release{false};
    std::atomic<bool> started{false};
    std::thread worker([&] {
        started.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    const HANDLE handle = ce::Win32ThreadHandle(worker);
    // A rejected handle reports WAIT_FAILED rather than WAIT_TIMEOUT, which is
    // exactly how the old conversion misbehaved: callers cannot distinguish
    // "still running" from "unusable handle".
    const DWORD waitResult = WaitForSingleObject(handle, 0);
    EXPECT_EQ(waitResult, static_cast<DWORD>(WAIT_TIMEOUT));
    EXPECT_NE(waitResult, static_cast<DWORD>(WAIT_FAILED));

    release.store(true, std::memory_order_release);
    EXPECT_EQ(WaitForSingleObject(handle, 10000), static_cast<DWORD>(WAIT_OBJECT_0));
    worker.join();
}

TEST(ThreadWait, HandleStaysStableAcrossQueries) {
    std::thread worker([] { std::this_thread::yield(); });

    const HANDLE first = ce::Win32ThreadHandle(worker);
    const HANDLE second = ce::Win32ThreadHandle(worker);
    EXPECT_EQ(first, second);

    worker.join();
}

}  // namespace
