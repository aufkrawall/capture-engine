#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "../common/crash_handler.h"

namespace {

constexpr ULONG_PTR kSyntheticExecuteFault = 0x12345000;
int g_HandlerCallCount = 0;
ULONG_PTR g_LastAccessType = 0;
ULONG_PTR g_LastFaultAddr = 0;

LONG RecoverSyntheticExecuteFault(EXCEPTION_POINTERS* exceptionPointers, ULONG_PTR accessType, ULONG_PTR faultAddr) {
    ++g_HandlerCallCount;
    g_LastAccessType = accessType;
    g_LastFaultAddr = faultAddr;
    if (accessType != 8 || faultAddr != kSyntheticExecuteFault || !exceptionPointers ||
        !exceptionPointers->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

#ifdef _WIN64
    exceptionPointers->ContextRecord->Rip = faultAddr;
#else
    exceptionPointers->ContextRecord->Eip = static_cast<DWORD>(faultAddr);
#endif
    return EXCEPTION_CONTINUE_EXECUTION;
}

EXCEPTION_POINTERS MakeSyntheticExceptionPointers(EXCEPTION_RECORD& record, CONTEXT& context,
                                                  ULONG_PTR accessType, ULONG_PTR faultAddr) {
    record = {};
    context = {};
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.NumberParameters = 2;
    record.ExceptionInformation[0] = accessType;
    record.ExceptionInformation[1] = faultAddr;
    record.ExceptionAddress = reinterpret_cast<void*>(faultAddr);
#ifdef _WIN64
    context.Rip = 0x11111111;
#else
    context.Eip = 0x11111111;
#endif
    return EXCEPTION_POINTERS{&record, &context};
}

std::string ReadBinaryFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(CrashHandlerTest, RegisteredExecutionFaultHandlerCanRecoverSyntheticDepFault) {
    g_HandlerCallCount = 0;
    g_LastAccessType = 0;
    g_LastFaultAddr = 0;
    RegisterCrashExecutionFaultHandler(RecoverSyntheticExecuteFault);

    EXCEPTION_RECORD record = {};
    CONTEXT context = {};
    EXCEPTION_POINTERS pointers =
        MakeSyntheticExceptionPointers(record, context, 8, kSyntheticExecuteFault);

    const LONG result = DispatchCrashExecutionFaultHandlerForTesting(&pointers);

    RegisterCrashExecutionFaultHandler(nullptr);
    EXPECT_EQ(result, EXCEPTION_CONTINUE_EXECUTION);
    EXPECT_EQ(g_HandlerCallCount, 1);
    EXPECT_EQ(g_LastAccessType, static_cast<ULONG_PTR>(8));
    EXPECT_EQ(g_LastFaultAddr, kSyntheticExecuteFault);
#ifdef _WIN64
    EXPECT_EQ(context.Rip, kSyntheticExecuteFault);
#else
    EXPECT_EQ(context.Eip, static_cast<DWORD>(kSyntheticExecuteFault));
#endif
}

TEST(CrashHandlerTest, RegisteredExecutionFaultHandlerIgnoresReadWriteAccessViolations) {
    g_HandlerCallCount = 0;
    RegisterCrashExecutionFaultHandler(RecoverSyntheticExecuteFault);

    EXCEPTION_RECORD record = {};
    CONTEXT context = {};
    EXCEPTION_POINTERS pointers =
        MakeSyntheticExceptionPointers(record, context, 0, kSyntheticExecuteFault);

    const LONG result = DispatchCrashExecutionFaultHandlerForTesting(&pointers);

    RegisterCrashExecutionFaultHandler(nullptr);
    EXPECT_EQ(result, EXCEPTION_CONTINUE_SEARCH);
    EXPECT_EQ(g_HandlerCallCount, 0);
}

TEST(CrashHandlerBinaryTest, HookDllContainsLazyExecRegressionStrings) {
    const std::filesystem::path hookDll =
        std::filesystem::current_path() / "installed" / "captureengine" / "capture_hook_x64.dll";
    if (!std::filesystem::exists(hookDll)) {
        GTEST_SKIP() << "capture_hook_x64.dll has not been built yet";
    }

    const std::string contents = ReadBinaryFile(hookDll);
    ASSERT_FALSE(contents.empty());
    EXPECT_NE(contents.find("LazyExec: Recovered trampoline DEP fault"), std::string::npos);
    EXPECT_NE(contents.find("BypassTrampoline: Created lazy-exec trampoline"), std::string::npos);
    EXPECT_NE(contents.find("Guarded Steam Present hook installed Steam null-callback VEH recovery"),
              std::string::npos);
    EXPECT_NE(contents.find("Streamline startup-handoff normal-route bypass"), std::string::npos);
    EXPECT_NE(contents.find("Fresh authoritative Streamline handoff invalidated stale PostSL confirmation"),
              std::string::npos);
    EXPECT_NE(contents.find("Retained Streamline startup activation swapchain"), std::string::npos);
    EXPECT_NE(contents.find("External dump storm threshold reached"), std::string::npos);
}
