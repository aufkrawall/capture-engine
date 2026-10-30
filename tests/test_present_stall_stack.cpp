#include "hook/present/present_stall_stack.h"
#include "hook/present/present_stall_sampler_internal.h"

#include <gtest/gtest.h>

#include <string>

using namespace ce::present_stall;

namespace {

struct ParkedThread {
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = nullptr;
    // Written after each call returns, so neither call can become a tail call
    // that leaves no frame of the test on the parked stack.
    DWORD waitResult = 0;
    bool exited = false;
};

__attribute__((noinline)) DWORD ParkInKernelWait(ParkedThread& parked) {
    // Signals and enters the wait in one system call: once `entered` is seen,
    // the thread is blocked inside the kernel, as a stalled Present would be.
    parked.waitResult = SignalObjectAndWait(parked.entered, parked.release, INFINITE, FALSE);
    return parked.waitResult;
}

DWORD WINAPI ParkedThreadMain(void* parameter) {
    auto& parked = *static_cast<ParkedThread*>(parameter);
    const DWORD result = ParkInKernelWait(parked);
    parked.exited = true;
    return result;
}

bool StartParked(ParkedThread& parked) {
    parked.thread = CreateThread(nullptr, 0, ParkedThreadMain, &parked, 0, nullptr);
    return parked.thread != nullptr && WaitForSingleObject(parked.entered, INFINITE) == WAIT_OBJECT_0;
}

void ReleaseParked(ParkedThread& parked) {
    SetEvent(parked.release);
    WaitForSingleObject(parked.thread, INFINITE);
    CloseHandle(parked.thread);
    CloseHandle(parked.entered);
    CloseHandle(parked.release);
}

bool InModule(uint64_t address, HMODULE module) {
    HMODULE holder = nullptr;
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<const char*>(static_cast<uintptr_t>(address)), &holder) &&
           holder == module;
}

struct FailedSamplerStartup {
    HANDLE timer = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(1));
    HANDLE closed = nullptr;
    int threadAttempts = 0;
    int closeCalls = 0;

    static HANDLE CreateTimer(void* context) {
        return static_cast<FailedSamplerStartup*>(context)->timer;
    }
    static HANDLE CreateThread(void* context, LPTHREAD_START_ROUTINE, void*) {
        ++static_cast<FailedSamplerStartup*>(context)->threadAttempts;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    static void CloseHandle(void* context, HANDLE handle) {
        auto& startup = *static_cast<FailedSamplerStartup*>(context);
        startup.closed = handle;
        ++startup.closeCalls;
    }
    detail::SamplerStartupOps Ops() {
        return {this, CreateTimer, CreateThread, CloseHandle};
    }
};

}  // namespace

TEST(PresentStallStackTest, FailedSamplerThreadReleasesTheTimer) {
    FailedSamplerStartup startup;
    EXPECT_EQ(detail::StartSampler(startup.Ops()), nullptr);
    EXPECT_EQ(startup.threadAttempts, 1);
    EXPECT_EQ(startup.closeCalls, 1);
    EXPECT_EQ(startup.closed, startup.timer);
}

TEST(PresentStallStackTest, MissingSamplerTimerNeverStartsAThread) {
    FailedSamplerStartup startup;
    startup.timer = nullptr;
    EXPECT_EQ(detail::StartSampler(startup.Ops()), nullptr);
    EXPECT_EQ(startup.threadAttempts, 0);
    EXPECT_EQ(startup.closeCalls, 0);
}

TEST(PresentStallStackTest, TheBudgetAllowsABurstThenOneSamplePerInterval) {
    StackSampleBudget budget;
    for (uint32_t i = 0; i < StackSampleBudget::kBurst; ++i) {
        ASSERT_TRUE(budget.Allows(1'000));
        budget.Taken(1'000);
    }
    EXPECT_FALSE(budget.Allows(1'000 + StackSampleBudget::kIntervalMs - 1));
    EXPECT_TRUE(budget.Allows(1'000 + StackSampleBudget::kIntervalMs));
    budget.Taken(1'000 + StackSampleBudget::kIntervalMs);
    EXPECT_FALSE(budget.Allows(1'000 + StackSampleBudget::kIntervalMs + 1));
    EXPECT_EQ(budget.taken(), StackSampleBudget::kBurst + 1);
}

// The copy outlives the sampled thread: the walk reads only the copy, so it
// still names the wait and the function that entered it after the thread's
// own stack is gone.
TEST(PresentStallStackTest, ACopiedStackUnwindsAfterTheThreadMovedOn) {
    StackCopy copy;
    ASSERT_TRUE(copy.valid());
    ParkedThread parked;
    ASSERT_TRUE(StartParked(parked));
    const bool copied = copy.CopySuspended(parked.thread, nullptr, nullptr);
    ReleaseParked(parked);
    ASSERT_TRUE(copied);
    EXPECT_GT(copy.bytes(), 0u);
    EXPECT_LE(copy.bytes(), StackCopy::kBytes);

    uint64_t frames[kStackSampleFrames] = {};
    const size_t count = UnwindStackCopy(copy, frames, std::size(frames));
    ASSERT_GE(count, 3u);
    EXPECT_TRUE(InModule(frames[0], GetModuleHandleW(L"ntdll.dll"))) << std::hex << frames[0];
    const HMODULE testModule = GetModuleHandleW(nullptr);
    bool reachedTheParkingFunction = false;
    for (size_t i = 0; i < count; ++i) {
        reachedTheParkingFunction |= InModule(frames[i], testModule);
    }
    char text[1536];
    FormatStackFrames(frames, count, text, sizeof(text));
    EXPECT_TRUE(reachedTheParkingFunction) << text;
    EXPECT_EQ(std::string(text).rfind(" ntdll.dll+0x", 0), 0u) << text;
}

TEST(PresentStallStackTest, ACallThatEndedWhileSuspendingIsNotCopied) {
    StackCopy copy;
    ASSERT_TRUE(copy.valid());
    ParkedThread parked;
    ASSERT_TRUE(StartParked(parked));
    bool asked = false;
    const bool copied = copy.CopySuspended(
        parked.thread,
        [](void* user) {
            *static_cast<bool*>(user) = true;
            return false;
        },
        &asked);
    // The thread was resumed: it can still be released and exit.
    ReleaseParked(parked);
    EXPECT_TRUE(asked);
    EXPECT_FALSE(copied);
    EXPECT_EQ(copy.bytes(), 0u);
    uint64_t frames[4] = {};
    EXPECT_EQ(UnwindStackCopy(copy, frames, std::size(frames)), 0u);
}

TEST(PresentStallStackTest, FramesOutsideAnyModuleKeepTheirAddress) {
    const uint64_t frames[] = {0x10};
    char text[64];
    FormatStackFrames(frames, std::size(frames), text, sizeof(text));
    EXPECT_STREQ(text, " 0x10");
}
