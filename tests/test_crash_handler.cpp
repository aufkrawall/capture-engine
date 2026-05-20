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

EXCEPTION_POINTERS MakeSyntheticExceptionPointers(EXCEPTION_RECORD& record, CONTEXT& context, ULONG_PTR accessType,
                                                  ULONG_PTR faultAddr) {
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
    EXCEPTION_POINTERS pointers = MakeSyntheticExceptionPointers(record, context, 8, kSyntheticExecuteFault);

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
    EXCEPTION_POINTERS pointers = MakeSyntheticExceptionPointers(record, context, 0, kSyntheticExecuteFault);

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
    EXPECT_NE(contents.find("Extended resume offset past patched fill bytes"), std::string::npos);
    EXPECT_NE(contents.find("Guarded Steam Present hook installed Steam null-callback VEH recovery"),
              std::string::npos);
    EXPECT_NE(contents.find("Streamline startup-handoff normal-route bypass"), std::string::npos);
    EXPECT_NE(contents.find("Streamline startup normal-route transport allowed"), std::string::npos);
    EXPECT_NE(contents.find("Suppressing slDLSSGSetOptions(OFF) during PostSL warmup proof"), std::string::npos);
    EXPECT_NE(contents.find("Startup-protected OFF churn quiet proof reached"), std::string::npos);
    EXPECT_NE(contents.find("Fresh authoritative Streamline handoff invalidated stale PostSL confirmation"),
              std::string::npos);
    EXPECT_NE(contents.find("Retained Streamline startup activation swapchain"), std::string::npos);
    EXPECT_NE(contents.find("Skipping retained-swapchain PostSL startup activation callback"), std::string::npos);
    EXPECT_NE(contents.find("startupActivationEntered"), std::string::npos);
    EXPECT_NE(contents.find("activated-but-unconfirmed Streamline startup normal route"), std::string::npos);
    EXPECT_NE(contents.find("Accepting Streamline OFF during activated-but-unconfirmed startup resume"),
              std::string::npos);
    EXPECT_NE(contents.find("Late Reflex feature hook retry during DLSSG runtime activity"), std::string::npos);
    EXPECT_NE(contents.find("stale runtime-owned Streamline no-FG cleanup"), std::string::npos);
    EXPECT_NE(contents.find("Shutting down adapter-owned DescFree backend"), std::string::npos);
    EXPECT_NE(contents.find("inline CreateSwapChainForHwnd hook already handled forwarded swapchain side-effects"),
              std::string::npos);
    EXPECT_NE(contents.find("External dump storm threshold reached"), std::string::npos);
    EXPECT_NE(contents.find("Explicit native FSR OFF plus origGame swapchain return ending runtime-owned native-FG"),
              std::string::npos);
    EXPECT_NE(contents.find("Native FSR configure disabled; not installing DX12 present-callback bridge"),
              std::string::npos);
    EXPECT_NE(
        contents.find("Native FSR disabled startup-arming configure forwarded without CE present-callback bridge"),
        std::string::npos);
    EXPECT_NE(contents.find("Native FSR disabled configure used for startup arming"), std::string::npos);
    EXPECT_NE(contents.find("Native FSR startup configure arming"), std::string::npos);
    EXPECT_NE(contents.find("Official FFX takeover side-effects staged until enabled ffxConfigure"), std::string::npos);
    EXPECT_NE(contents.find("Finalizing staged official FFX takeover after enabled ffxConfigure"), std::string::npos);
    EXPECT_NE(contents.find("Protected official FFX startup swapchain pass-through"), std::string::npos);
    EXPECT_NE(contents.find("Protected official FFX startup pending - passing ExecuteCommandLists through"),
              std::string::npos);
    EXPECT_NE(contents.find("Protected official FFX startup pending - skipping ProcessFrame overlay/capture/FFX retry"),
              std::string::npos);
    EXPECT_NE(contents.find("Preserving swapchain descriptor for authoritative FG runtime create"), std::string::npos);
    EXPECT_NE(contents.find("Finalizing protected official FFX startup pass-through after enabled ffxConfigure"),
              std::string::npos);
    EXPECT_NE(contents.find("Finalizing protected official FFX startup pass-through after sustained frame progress"),
              std::string::npos);
    EXPECT_NE(contents.find("progress-resolved official FFX runtime-owned Present path assumption"),
              std::string::npos);
    EXPECT_NE(contents.find("normal overlay fallback is unsafe for progress-resolved official FFX startup"),
              std::string::npos);
    EXPECT_NE(contents.find("ECL startup activation swapchain probe suppressed"), std::string::npos);
    EXPECT_NE(contents.find("Rejecting startup activation swapchain"), std::string::npos);
    EXPECT_NE(contents.find("FatalExitDump: Installed pre-termination dump hooks"), std::string::npos);
    EXPECT_NE(contents.find("FatalExitDump: Installed inline pre-termination hook"), std::string::npos);
    EXPECT_NE(contents.find("FatalExitDump: Capturing pre-termination dump before crash-like process exit"),
              std::string::npos);
    EXPECT_NE(contents.find("FatalExitDump: _purecall caller stack before pre-termination dump"), std::string::npos);
    EXPECT_NE(contents.find("FatalExitDump: Using minimal-first pre-termination dump attempt"), std::string::npos);
    EXPECT_NE(contents.find("FatalExitDump: Launching external pre-termination dump helper"), std::string::npos);
    EXPECT_NE(contents.find("RtlExitUserProcess"), std::string::npos);
    EXPECT_NE(contents.find("NtTerminateProcess"), std::string::npos);
    EXPECT_NE(contents.find("_invoke_watson"), std::string::npos);
    EXPECT_NE(contents.find("NtRaiseException"), std::string::npos);
    EXPECT_NE(contents.find("ZwRaiseException"), std::string::npos);
    EXPECT_NE(contents.find("ZwTerminateProcess"), std::string::npos);
    EXPECT_NE(contents.find("Registered module-filtered dynamic hooks for FFX exports"), std::string::npos);
    EXPECT_NE(contents.find("Using GetProcAddress-only hooks for protected official FFX module"), std::string::npos);
    EXPECT_NE(contents.find("IAT import patching skipped to avoid startup fail-fast"), std::string::npos);
}
