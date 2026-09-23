#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <thread>

#include "../common/crash_first_chance.h"

namespace {

constexpr DWORD kTestFaultCode = 0xC0DE0001UL;  // error severity, no customer bit
constexpr DWORD kTestRaiseCode = 0xE0DE0002UL;

std::atomic<bool> g_SawDispatchInsideHandler{false};
std::atomic<int> g_HandlerMode{0};  // 0 = inert, 1 = observe dispatch, 2 = record and resume

LONG CALLBACK TestVectoredHandler(PEXCEPTION_POINTERS pointers) {
    if (!pointers || !pointers->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = pointers->ExceptionRecord->ExceptionCode;
    if (code != kTestFaultCode && code != kTestRaiseCode)
        return EXCEPTION_CONTINUE_SEARCH;
    if (g_HandlerMode.load() == 1)
        g_SawDispatchInsideHandler.store(ce::crash_first_chance::IsCurrentThreadInsideExceptionDispatch());
    if (g_HandlerMode.load() == 2)
        ce::crash_first_chance::RecordFault(pointers);
    // RaiseException is continuable: resuming returns from RaiseException.
    return EXCEPTION_CONTINUE_EXECUTION;
}

EXCEPTION_POINTERS MakeFault(EXCEPTION_RECORD& record, CONTEXT& context) {
    record = {};
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.ExceptionAddress = reinterpret_cast<void*>(0x1234);
    RtlCaptureContext(&context);
    EXCEPTION_POINTERS pointers = {};
    pointers.ExceptionRecord = &record;
    pointers.ContextRecord = &context;
    return pointers;
}

}  // namespace

TEST(CrashFirstChanceTest, RecordedFaultIsThreadLocalAndClearable) {
    ce::crash_first_chance::ResetForTesting();
    EXCEPTION_RECORD record;
    CONTEXT context;
    EXCEPTION_POINTERS pointers = MakeFault(record, context);
    ce::crash_first_chance::RecordFault(&pointers);

    EXCEPTION_RECORD copied = {};
    CONTEXT copiedContext = {};
    ASSERT_TRUE(ce::crash_first_chance::CopyFaultForCurrentThread(&copied, &copiedContext));
    EXPECT_EQ(copied.ExceptionCode, static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION));
    EXPECT_EQ(copied.ExceptionAddress, reinterpret_cast<void*>(0x1234));
    EXPECT_EQ(copied.ExceptionRecord, nullptr) << "the nested chain points into the faulting stack";

    bool otherThreadSeesIt = true;
    std::thread other(
        [&]() { otherThreadSeesIt = ce::crash_first_chance::CopyFaultForCurrentThread(nullptr, nullptr); });
    other.join();
    EXPECT_FALSE(otherThreadSeesIt);

    ce::crash_first_chance::ClearFaultForCurrentThread();
    EXPECT_FALSE(ce::crash_first_chance::CopyFaultForCurrentThread(nullptr, nullptr));
}

// A thread id reused after its thread exited must not inherit that thread's
// fault: the recorded stack pointer is outside the new thread's stack.
TEST(CrashFirstChanceTest, RecordFromAnotherStackIsNotThisThreadsFault) {
    ce::crash_first_chance::ResetForTesting();
    EXCEPTION_RECORD record;
    CONTEXT context;
    EXCEPTION_POINTERS pointers = MakeFault(record, context);
#ifdef _WIN64
    context.Rsp = 0x10;
#else
    context.Esp = 0x10;
#endif
    ce::crash_first_chance::RecordFault(&pointers);
    EXPECT_FALSE(ce::crash_first_chance::CopyFaultForCurrentThread(nullptr, nullptr));
    ce::crash_first_chance::ResetForTesting();
}

TEST(CrashFirstChanceTest, DispatchIsDetectedOnlyWhileAnExceptionIsBeingDispatched) {
    EXPECT_FALSE(ce::crash_first_chance::IsCurrentThreadInsideExceptionDispatch());

    PVOID handler = AddVectoredExceptionHandler(1, TestVectoredHandler);
    ASSERT_NE(handler, nullptr);
    g_HandlerMode.store(1);
    g_SawDispatchInsideHandler.store(false);
    RaiseException(kTestRaiseCode, 0, 0, nullptr);
    g_HandlerMode.store(0);
    RemoveVectoredExceptionHandler(handler);

    EXPECT_TRUE(g_SawDispatchInsideHandler.load()) << "a vectored handler runs inside RtlRaiseException";
    EXPECT_FALSE(ce::crash_first_chance::IsCurrentThreadInsideExceptionDispatch());
}

// Mono's and the JVM's vectored handlers resolve their faults by resuming
// execution; the continue handler must then forget the recorded fault so a
// later non-zero exit is not blamed on it.
TEST(CrashFirstChanceTest, ResumingExecutionClearsTheRecordedFault) {
    ce::crash_first_chance::ResetForTesting();
    ce::crash_first_chance::Install();
    PVOID handler = AddVectoredExceptionHandler(1, TestVectoredHandler);
    ASSERT_NE(handler, nullptr);
    g_HandlerMode.store(2);
    const auto before = ce::crash_first_chance::GetStatistics();
    RaiseException(kTestFaultCode, 0, 0, nullptr);
    g_HandlerMode.store(0);
    RemoveVectoredExceptionHandler(handler);

    const auto after = ce::crash_first_chance::GetStatistics();
    EXPECT_EQ(after.recorded, before.recorded + 1);
    EXPECT_EQ(after.resolvedByContinue, before.resolvedByContinue + 1);
    EXPECT_FALSE(ce::crash_first_chance::CopyFaultForCurrentThread(nullptr, nullptr));
}
