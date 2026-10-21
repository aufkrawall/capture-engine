#include <gtest/gtest.h>

#include "common/platform/thread_label.h"

#include <atomic>
#include <string>
#include <thread>

namespace {

// Runs `name` on a fresh thread, then reads its description back from this thread by id while the named thread is
// still alive (the crash path reads the faulting thread from the dump worker, never from itself).
std::string NameAndDescribe(const char* name, size_t capacity = 64) {
    std::atomic<DWORD> threadId{0};
    HANDLE named = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread worker([&] {
        ce::platform::NameCurrentThread(name);
        threadId.store(GetCurrentThreadId());
        SetEvent(named);
        WaitForSingleObject(release, INFINITE);
    });
    WaitForSingleObject(named, INFINITE);
    std::string described(capacity, '\0');
    const size_t length = ce::platform::DescribeThread(threadId.load(), described.data(), capacity);
    described.resize(length);
    SetEvent(release);
    worker.join();
    CloseHandle(named);
    CloseHandle(release);
    return described;
}

TEST(ThreadLabelTest, ANamedThreadIsDescribedFromAnotherThread) {
    EXPECT_EQ(NameAndDescribe("CE media r0001 encoder"), "CE media r0001 encoder");
}

TEST(ThreadLabelTest, TheDescriptionIsCutToTheCallersBuffer) {
    EXPECT_EQ(NameAndDescribe("CE media r0001 encoder", 9), "CE media");
}

TEST(ThreadLabelTest, NamesLongerThanTheLimitAreCut) {
    const std::string longName(100, 'x');
    EXPECT_EQ(NameAndDescribe(longName.c_str(), 128), std::string(63, 'x'));
}

TEST(ThreadLabelTest, AnUnknownThreadIsEmpty) {
    char out[16] = "unchanged";
    // Thread id 0 is never a valid thread.
    EXPECT_EQ(ce::platform::DescribeThread(0, out, sizeof(out)), 0u);
    EXPECT_STREQ(out, "");
}

TEST(ThreadLabelTest, ANullNameLeavesTheThreadAlone) {
    ce::platform::NameCurrentThread(nullptr);
    SUCCEED();
}

}  // namespace
