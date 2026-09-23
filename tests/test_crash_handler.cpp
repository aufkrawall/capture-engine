#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "../common/crash_dump_policy.h"
#include "../common/crash_handler.h"
#include "../common/crash_handler_internal.h"
#include "../hook/common/freeze_watchdog.h"
#include "source_fragment_reader.h"

namespace {

constexpr ULONG_PTR kSyntheticExecuteFault = 0x12345000;
int g_HandlerCallCount = 0;
ULONG_PTR g_LastAccessType = 0;
ULONG_PTR g_LastFaultAddr = 0;

int g_ExternalCaptureCallCount = 0;
std::string g_LastExternalCaptureHint;
bool g_LastExternalCaptureStackOnly = false;

bool RecordExternalCapture(const char* dumpFileNameHint, bool stackOnly, const ExternalDumpException*) {
    ++g_ExternalCaptureCallCount;
    g_LastExternalCaptureHint = dumpFileNameHint ? dumpFileNameHint : "";
    g_LastExternalCaptureStackOnly = stackOnly;
    return true;
}

bool ForeignOverlayLoadedStub() {
    return true;
}

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

std::string ReadSourceFile(const std::filesystem::path& path) {
    return ce::test_source::ReadLogicalSource(path);
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

TEST(CrashHandlerTest, CrashDumpEnvironmentHooksAreOptionalAndAnswerConservatively) {
    // Nothing registered (captureengine's own processes): the dump worker keeps
    // its plain in-process path and must never believe an overlay is loaded or a
    // helper is available.
    RegisterCrashDumpEnvironmentHooks(CrashDumpEnvironmentHooks{});
    EXPECT_FALSE(HasExternalCrashDumpCapture());
    EXPECT_FALSE(IsForeignOverlayLoadedForCrashDump());
    EXPECT_FALSE(CaptureCrashDumpWithExternalHelper("crash_test.dmp"));

    g_ExternalCaptureCallCount = 0;
    g_LastExternalCaptureHint.clear();
    CrashDumpEnvironmentHooks hooks;
    hooks.captureWithExternalHelper = &RecordExternalCapture;
    hooks.foreignOverlayLoaded = &ForeignOverlayLoadedStub;
    RegisterCrashDumpEnvironmentHooks(hooks);

    EXPECT_TRUE(HasExternalCrashDumpCapture());
    EXPECT_TRUE(IsForeignOverlayLoadedForCrashDump());
    EXPECT_TRUE(CaptureCrashDumpWithExternalHelper("crash_test.dmp"));
    EXPECT_EQ(g_ExternalCaptureCallCount, 1);
    EXPECT_EQ(g_LastExternalCaptureHint, "crash_test.dmp");
    // The default is the dump every caller got before the scope existed.
    EXPECT_FALSE(g_LastExternalCaptureStackOnly);

    EXPECT_TRUE(CaptureCrashDumpWithExternalHelper("freeze_test.dmp", /*stackOnly=*/true));
    EXPECT_EQ(g_ExternalCaptureCallCount, 2);
    EXPECT_TRUE(g_LastExternalCaptureStackOnly);

    // An empty hint would make the helper write an unnamed artifact; refuse it
    // instead of launching the helper.
    EXPECT_FALSE(CaptureCrashDumpWithExternalHelper(""));
    EXPECT_FALSE(CaptureCrashDumpWithExternalHelper(nullptr));
    EXPECT_EQ(g_ExternalCaptureCallCount, 2);

    RegisterCrashDumpEnvironmentHooks(CrashDumpEnvironmentHooks{});
}

TEST(CrashHandlerBinaryTest, HookDllContainsCfgSealedTrampolineRegressionStrings) {
    const std::filesystem::path hookDll =
        std::filesystem::current_path() / "installed" / "captureengine" / "capture_hook_x64.dll";
    if (!std::filesystem::exists(hookDll)) {
        GTEST_SKIP() << "capture_hook_x64.dll has not been built yet";
    }

    const std::string contents = ReadBinaryFile(hookDll);
    ASSERT_FALSE(contents.empty());
    EXPECT_EQ(contents.find("LazyExec: Recovered trampoline DEP fault"), std::string::npos);
    EXPECT_NE(contents.find("BypassTrampoline: Created RX/CFG trampoline"), std::string::npos);
    EXPECT_NE(contents.find("SetProcessValidCallTargets failed"), std::string::npos);
    EXPECT_NE(contents.find("Extended resume offset past patched fill bytes"), std::string::npos);
    EXPECT_NE(contents.find("Guarded Steam Present hook armed Steam null-callback VEH recovery"),
              std::string::npos);
    EXPECT_NE(contents.find("Registered the process-lifetime Steam null-callback recovery handler"),
              std::string::npos);
    EXPECT_NE(contents.find("Streamline startup-handoff normal-route bypass"), std::string::npos);
    EXPECT_NE(contents.find("Streamline startup normal-route transport allowed"), std::string::npos);
    EXPECT_NE(contents.find("Suppressing slDLSSGSetOptions(OFF) during PostSL warmup proof"), std::string::npos);
    EXPECT_NE(contents.find("Startup-protected OFF churn quiet proof reached"), std::string::npos);
    EXPECT_NE(contents.find("Fresh authoritative Streamline handoff invalidated stale PostSL confirmation"),
              std::string::npos);
    EXPECT_NE(contents.find("Retained Streamline startup activation swapchain"), std::string::npos);
    EXPECT_NE(contents.find("releasing retained Streamline activation swapchain before DXGI CreateSwapChainForHwnd"),
              std::string::npos);
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
    EXPECT_NE(contents.find("Native FSR configure without DX12 present-callback bridge"), std::string::npos);
    EXPECT_NE(
        contents.find("Native FSR disabled startup-arming configure forwarded without CE present-callback bridge"),
        std::string::npos);
    EXPECT_NE(contents.find("Native FSR enabled with no app present callback"), std::string::npos);
    EXPECT_NE(contents.find("Native FSR contexts destroyed; cleared callback routing"), std::string::npos);
    EXPECT_NE(contents.find("Native FSR disabled configure used for startup arming"), std::string::npos);
    EXPECT_NE(contents.find("Native FSR startup configure arming"), std::string::npos);
    EXPECT_NE(contents.find("Official FFX takeover side-effects staged until enabled ffxConfigure"), std::string::npos);
    EXPECT_NE(contents.find("Finalizing staged official FFX takeover after enabled ffxConfigure"), std::string::npos);
    EXPECT_NE(contents.find("Protected official FFX startup swapchain pass-through"), std::string::npos);
    EXPECT_NE(contents.find("Protected official FFX startup pending - passing ExecuteCommandLists through"),
              std::string::npos);
    EXPECT_NE(contents.find("Protected official FFX startup pending - keeping ProcessFrame tracking-only"),
              std::string::npos);
    EXPECT_NE(contents.find("Protected official FFX startup keeping nested real-swapchain work tracking-only"),
              std::string::npos);
    EXPECT_NE(contents.find("protected-startup-backbuffer"), std::string::npos);
    EXPECT_NE(contents.find("Preserving overlay backend across protected official FFX startup swapchain change"),
              std::string::npos);
    EXPECT_NE(contents.find("Preserving swapchain descriptor for authoritative FG runtime create"), std::string::npos);
    EXPECT_NE(contents.find("Finalizing protected official FFX startup pass-through after enabled ffxConfigure"),
              std::string::npos);
    EXPECT_NE(contents.find("Protected official FFX startup has sustained frame progress but remains quiesced"),
              std::string::npos);
    EXPECT_NE(contents.find("progress-resolved official FFX runtime-owned Present path assumption"), std::string::npos);
    EXPECT_NE(contents.find("normal overlay fallback is unsafe for this native FSR handoff"), std::string::npos);
    EXPECT_NE(contents.find("direct ffxConfigure/present-callback proof"), std::string::npos);
    EXPECT_NE(contents.find("Protected official FFX startup immediately quiesced Streamline/PostSL"),
              std::string::npos);
    EXPECT_NE(contents.find("Continuing DX12 overlay submissions while startup-overlay compatibility window is active"),
              std::string::npos);
    EXPECT_NE(contents.find("FFX Hook: Using IAT/dynamic hooks for protected official FFX module"), std::string::npos);
    EXPECT_NE(contents.find("Native FSR fallback proof allows normal overlay rendering"), std::string::npos);
    EXPECT_NE(contents.find("ECL startup activation swapchain probe suppressed"), std::string::npos);
    EXPECT_NE(contents.find("Rejecting startup activation swapchain"), std::string::npos);
    EXPECT_NE(contents.find("FatalExitDump: Installed pre-termination dump hooks"), std::string::npos);
    EXPECT_NE(contents.find("FatalExitDump: Installed inline pre-termination hook"), std::string::npos);
    EXPECT_NE(contents.find("FatalExitDump: Capturing pre-termination dump before crash-like process exit"),
              std::string::npos);
    EXPECT_NE(contents.find("FatalExitDump: _purecall caller stack before pre-termination dump"), std::string::npos);
    EXPECT_NE(contents.find("FatalExitDump: Using minimal-first pre-termination dump attempt"), std::string::npos);
    EXPECT_NE(contents.find("FatalExitDump: Launching external pre-termination dump helper"), std::string::npos);
    EXPECT_NE(contents.find("CrashHandler: safe pre-dump diagnostics complete"), std::string::npos);
    EXPECT_NE(contents.find("RtlExitUserProcess"), std::string::npos);
    EXPECT_NE(contents.find("NtTerminateProcess"), std::string::npos);
    EXPECT_NE(contents.find("_invoke_watson"), std::string::npos);
    EXPECT_NE(contents.find("NtRaiseException"), std::string::npos);
    EXPECT_NE(contents.find("ZwRaiseException"), std::string::npos);
    EXPECT_NE(contents.find("ZwTerminateProcess"), std::string::npos);
    EXPECT_NE(contents.find("CrashHandler: using minimal-first crash dump attempts"), std::string::npos);
    EXPECT_NE(contents.find("minimal-primary"), std::string::npos);
    EXPECT_NE(contents.find("Registered module-filtered dynamic hooks for FFX exports"), std::string::npos);
    EXPECT_NE(contents.find("Using GetProcAddress-only hooks for protected official FFX module"), std::string::npos);
    EXPECT_NE(contents.find("code bytes left unmodified"), std::string::npos);
    EXPECT_NE(contents.find("waiting for a real ffxConfigure call to arm the native FSR present-callback bridge"),
              std::string::npos);
    EXPECT_NE(contents.find("GetProcAddress: Intercepted FFX API"), std::string::npos);
    EXPECT_NE(contents.find("initializing FFX hooks immediately for native FSR callback bridge"), std::string::npos);
    EXPECT_NE(contents.find("Installed LdrLoadDll hook for module-load observation"), std::string::npos);
    EXPECT_NE(contents.find("IAT import patching skipped to avoid startup fail-fast"), std::string::npos);
    EXPECT_NE(contents.find("DX12 focus-loss sync policy=v13 draw-every-frame + x86 solid-span text"),
              std::string::npos);
    EXPECT_NE(
        contents.find("overlay STILL RENDERING (not held; x86 solid-span text; upload-slot fence paces slot reuse)"),
        std::string::npos);
    // DescFree UPLOAD-ring per-slot GPU-completion guard (x86 Alt+Tab DEVICE_HUNG fix).
    EXPECT_NE(contents.find("GPU-completion wait"), std::string::npos);
    EXPECT_NE(contents.find("DescFree: font structured buffer ready"), std::string::npos);
    EXPECT_NE(contents.find("DescFree: font upload recorded to default buffer"), std::string::npos);
    EXPECT_NE(contents.find("DX12 Overlay: slot"), std::string::npos);
    EXPECT_NE(contents.find("textured SDR"), std::string::npos);
    EXPECT_NE(contents.find("Focus-change edge ("), std::string::npos);
    EXPECT_NE(contents.find("Focus-loss same-frame overlay fence wait result"), std::string::npos);
    EXPECT_NE(contents.find("Requesting immediate freeze dump for focus-loss same-frame overlay fence wait"),
              std::string::npos);
    EXPECT_NE(contents.find("Holding overlay/capture backbuffer work while swapchain is NOT presentable"),
              std::string::npos);
    EXPECT_NE(contents.find("Resuming overlay/capture backbuffer work"), std::string::npos);
    EXPECT_NE(contents.find("Swapchain presentability changed ->"), std::string::npos);
    EXPECT_NE(contents.find("Requesting immediate freeze dump for focus-loss device removal"), std::string::npos);
    // DRED GPU-fault diagnostics (device-hung breadcrumbs + page-fault).
    EXPECT_NE(contents.find("DX12 DRED: armed "), std::string::npos);
    // Low-perturbation page-fault-only arming mode (steady-state DEVICE_HUNG repro).
    EXPECT_NE(contents.find("page-fault only"), std::string::npos);
    EXPECT_NE(contents.find("DX12 DRED: ===== device-removed extended data"), std::string::npos);
    // Device-removed ECL forward guard (avoids nvwgf2um AV after a DEVICE_HUNG TDR).
    EXPECT_NE(contents.find("Skipping app ExecuteCommandLists forward"), std::string::npos);
}

TEST(CrashHandlerSourceTest, ExternalDumpHelperSuppressesGuiLaunchFeedback) {
    const std::filesystem::path source = std::filesystem::current_path() / "hook" / "main.cpp";
    const std::string contents = ReadSourceFile(source);
    ASSERT_FALSE(contents.empty());
    EXPECT_NE(contents.find("si.dwFlags = STARTF_USESHOWWINDOW | STARTF_FORCEOFFFEEDBACK;"), std::string::npos);
}

// The vectored filter runs for every exception the host raises - thousands per
// second under a JIT or managed runtime. Until an exception is classified as a
// dump, the filter must not allocate, lock or write a file, and a first-chance
// fault is recorded rather than dumped.
TEST(CrashHandlerSourceTest, FirstChanceFilterRecordsFaultsWithoutIoOrAllocation) {
    const std::string contents =
        ReadSourceFile(std::filesystem::current_path() / "common" / "crash_dump_writer.cpp");
    ASSERT_FALSE(contents.empty());
    const size_t filter = contents.find("LONG WINAPI CrashHandlerExceptionFilter(");
    const size_t classify = contents.find("ClassifyFirstChanceException(", filter);
    const size_t record = contents.find("ce::crash_first_chance::RecordFault(pExceptionPointers);", filter);
    const size_t firstTrace = contents.find("TraceCrash(", filter);
    ASSERT_NE(filter, std::string::npos);
    ASSERT_NE(classify, std::string::npos);
    ASSERT_NE(record, std::string::npos);
    const std::string preClassification = contents.substr(filter, classify - filter);
    EXPECT_EQ(preClassification.find("std::string"), std::string::npos);
    EXPECT_EQ(preClassification.find("TraceCrash("), std::string::npos);
    EXPECT_EQ(preClassification.find("unique_lock"), std::string::npos);
    EXPECT_EQ(preClassification.find("lock_guard"), std::string::npos);
    EXPECT_EQ(preClassification.find("ExceptionSafeLock"), std::string::npos);
    EXPECT_LT(record, firstTrace) << "recording a fault must return before any crash.log write";

    // One registration: the last-position duplicate ran every exception twice.
    EXPECT_EQ(contents.find("AddVectoredExceptionHandler(0, CrashHandlerExceptionFilter)"), std::string::npos);
    EXPECT_NE(contents.find("ce::crash_first_chance::Install();"), std::string::npos);
}

TEST(CrashHandlerSourceTest, DumpWorkerOwnsItsExceptionStateAndTheAttemptFlag) {
    // The dump worker can outlive the filter that spawned it: the filter waits 5 s, and this
    // file's own notes record a 61.6 s in-process dump with the Steam overlay loaded. It must
    // therefore never hold a pointer into the crashing thread's stack - EXCEPTION_POINTERS,
    // EXCEPTION_RECORD and CONTEXT all live there, and that stack is reused as soon as some
    // SEH frame handles the exception and execution continues.
    const std::filesystem::path source =
        std::filesystem::current_path() / "common" / "crash_dump_writer.cpp";
    const std::string contents = ReadSourceFile(source);
    ASSERT_FALSE(contents.empty());

    // Deep copies, not the caller's pointers.
    EXPECT_NE(contents.find("EXCEPTION_RECORD record{};"), std::string::npos);
    EXPECT_NE(contents.find("CONTEXT context{};"), std::string::npos);
    EXPECT_NE(contents.find("pointers.ExceptionRecord = &record;"), std::string::npos);
    EXPECT_NE(contents.find("pointers.ContextRecord = &context;"), std::string::npos);
    EXPECT_NE(contents.find("mdei.ExceptionPointers = &params->pointers;"), std::string::npos)
        << "the dump must be written from the copies, not the crashed stack";
    EXPECT_EQ(contents.find("mdei.ExceptionPointers = params->pExceptionPointers;"), std::string::npos);

    // No heap allocation on the crash path: the crash may have happened while the heap lock
    // was held, so `new` here can deadlock.
    EXPECT_EQ(contents.find("new DumpParams"), std::string::npos)
        << "allocating inside a crash filter can deadlock on a held heap lock";
    EXPECT_NE(contents.find("static DumpParams g_DumpParamsSlot;"), std::string::npos);

    // The worker releases the attempt flag, so a second crash cannot start a second worker
    // while the first is still writing.
    EXPECT_NE(contents.find("~AttemptReleaser() { g_DumpAttemptInProgress.store(false, std::memory_order_release); }"),
              std::string::npos);
}

TEST(CrashHandlerSourceTest, TrampolinePagesPreserveAnInitiallyInvalidCfgBitmap) {
    const std::filesystem::path source =
        std::filesystem::current_path() / "hook" / "wrappers" / "inline_hook_trampoline.cpp";
    const std::string contents = ReadSourceFile(source);
    ASSERT_FALSE(contents.empty());
    EXPECT_NE(contents.find("PAGE_EXECUTE_READ | PAGE_TARGETS_INVALID"), std::string::npos);
    EXPECT_NE(contents.find("PAGE_EXECUTE_READ |"), std::string::npos);
    EXPECT_NE(contents.find("PAGE_TARGETS_NO_UPDATE"), std::string::npos);
    EXPECT_NE(contents.find("target.Flags = CFG_CALL_TARGET_VALID"), std::string::npos);
    EXPECT_NE(contents.find("__declspec(guard(nocf))"), std::string::npos);
    EXPECT_NE(contents.find("CallCfgRegistrationBootstrap(setProcessValidCallTargets"), std::string::npos);
    EXPECT_NE(contents.find("GetProcAddress(module, \"SetProcessValidCallTargets\")"), std::string::npos);
    const size_t registrationCall = contents.find("CallCfgRegistrationBootstrap(setProcessValidCallTargets");
    ASSERT_NE(registrationCall, std::string::npos);
    const size_t registrationCallEnd = contents.find(");", registrationCall);
    ASSERT_NE(registrationCallEnd, std::string::npos);
    const std::string registrationArguments = contents.substr(registrationCall, registrationCallEnd - registrationCall);
    EXPECT_NE(registrationArguments.find("allocationBase"), std::string::npos);
    EXPECT_NE(registrationArguments.find("allocationSize"), std::string::npos);
    EXPECT_NE(registrationArguments.find("1, &target"), std::string::npos);

    const size_t allocatorStart = contents.find("uint8_t* AllocateWritableTrampolinePage(void* preferredAddress) {");
    ASSERT_NE(allocatorStart, std::string::npos);
    // The allocator body runs to the first column-0 closing brace after it.
    const size_t allocatorEnd = contents.find("\n}\n", allocatorStart);
    ASSERT_NE(allocatorEnd, std::string::npos);
    EXPECT_EQ(contents.substr(allocatorStart, allocatorEnd - allocatorStart).find("PAGE_EXECUTE_READWRITE"),
              std::string::npos);
}

// The frame-generation fallback may only be suppressed on evidence the policy
// can trust, and that evidence has to be gathered without touching the loader
// on the termination path: whatever is tearing the process down may already
// hold the loader lock, and NtTerminateProcess is reached from
// RtlExitUserProcess with that lock held by the terminating thread itself. The
// bounds of every module the path has to tell apart - the executable, CE's own
// image, and the layers a request is forwarded through - are therefore cached
// while the hooks are installed, and every decision is a plain range check.
TEST(CrashHandlerSourceTest, TerminationOriginIsCachedAtInstallAndResolvedWithoutTheLoader) {
    const std::filesystem::path source = std::filesystem::current_path() / "hook" / "main.cpp";
    const std::string contents = ReadSourceFile(source);
    ASSERT_FALSE(contents.empty());

    const size_t bootstrap = contents.find("void TryInstallFatalTerminationDumpHooks()");
    ASSERT_NE(bootstrap, std::string::npos);
    const size_t cacheCall = contents.find("CacheTerminationOriginModuleBounds();", bootstrap);
    ASSERT_NE(cacheCall, std::string::npos);

    const size_t classifier =
        contents.find("ce::crash_dump_policy::TerminationFrameKind ClassifyTerminationFrame(const void* address) {");
    ASSERT_NE(classifier, std::string::npos);
    const size_t classifierEnd = contents.find("\n}\n", classifier);
    ASSERT_NE(classifierEnd, std::string::npos);
    const std::string classifierBody = contents.substr(classifier, classifierEnd - classifier);
    EXPECT_NE(classifierBody.find("g_PrimaryModuleBase"), std::string::npos);
    EXPECT_NE(classifierBody.find("g_PrimaryModuleSize"), std::string::npos);
    EXPECT_NE(classifierBody.find("g_CaptureEngineModuleRange"), std::string::npos);
    EXPECT_NE(classifierBody.find("g_TerminationPlumbingRanges"), std::string::npos);

    // Anchor on the definition, not on the declaration that precedes it in the
    // logical unit's internal header.
    const size_t resolver = contents.find("const void** requesterAddress) {");
    ASSERT_NE(resolver, std::string::npos);
    const size_t resolverEnd = contents.find("\n}\n", resolver);
    ASSERT_NE(resolverEnd, std::string::npos);
    const std::string resolverBody = contents.substr(resolver, resolverEnd - resolver);
    EXPECT_NE(resolverBody.find("g_PrimaryModuleBase.load"), std::string::npos);
    EXPECT_NE(resolverBody.find("g_PrimaryModuleSize.load"), std::string::npos);

    for (const std::string& body : {classifierBody, resolverBody}) {
        EXPECT_EQ(body.find("GetModuleHandle"), std::string::npos);
        EXPECT_EQ(body.find("LoadLibrary"), std::string::npos);
        EXPECT_EQ(body.find("EnumProcessModules"), std::string::npos);
    }
}

// A termination request passes through several hooked layers on one thread, and
// only the outermost is called by the module that made it. Portal RTX session
// 20260914_130052: the TerminateProcess hook suppressed the dump for
// NvRemixBridge.exe terminating itself, then the NtTerminateProcess hook - whose
// caller is KERNELBASE - captured the 183 MB dump the first hook had refused. So
// when the immediate caller only carries the request, the resolver walks the
// stack for the frame that actually made it.
TEST(CrashHandlerSourceTest, LayeredTerminationRequestIsAttributedByWalkingTheStack) {
    const std::filesystem::path source = std::filesystem::current_path() / "hook" / "main.cpp";
    const std::string contents = ReadSourceFile(source);
    ASSERT_FALSE(contents.empty());

    // Anchor on the definition, not on the declaration that precedes it in the
    // logical unit's internal header.
    const size_t resolver = contents.find("const void** requesterAddress) {");
    ASSERT_NE(resolver, std::string::npos);
    const size_t resolverEnd = contents.find("\n}\n", resolver);
    ASSERT_NE(resolverEnd, std::string::npos);
    const std::string resolverBody = contents.substr(resolver, resolverEnd - resolver);

    // The plumbing layers are the only ones that trigger the walk; a caller the
    // classifier can attribute directly is answered without one.
    EXPECT_NE(resolverBody.find("kTerminationPlumbing"), std::string::npos);
    EXPECT_NE(resolverBody.find("RtlCaptureStackBackTrace"), std::string::npos);
    EXPECT_NE(resolverBody.find("ce::crash_dump_policy::ResolveTerminationOriginFromFrames"), std::string::npos);

    const size_t directAnswer = resolverBody.find("case Kind::kPrimaryModule:");
    const size_t walk = resolverBody.find("RtlCaptureStackBackTrace");
    ASSERT_NE(directAnswer, std::string::npos);
    EXPECT_LT(directAnswer, walk);
}

// An unresolved origin must never suppress a dump, and a suppressed one must
// never be silent - a lost artifact that leaves no trace is worse than a large
// one, so the decision is logged with the caller that made it.
TEST(CrashHandlerSourceTest, PreTerminationDumpResolvesOriginBeforeDecidingAndLogsASuppression) {
    const std::filesystem::path source = std::filesystem::current_path() / "hook" / "main.cpp";
    const std::string contents = ReadSourceFile(source);
    ASSERT_FALSE(contents.empty());

    const size_t entry = contents.find("bool CapturePreTerminationDumpIfNeeded(const char* source, DWORD exitCode,");
    ASSERT_NE(entry, std::string::npos);
    const size_t originResolved =
        contents.find("ResolveTerminationOrigin(callerAddress, &terminationRequester)", entry);
    const size_t policyCall = contents.find("ce::crash_dump_policy::ShouldCapturePreTerminationDump(", entry);
    const size_t suppressionLog = contents.find(
        "LogSuppressedPreTerminationDump(source, exitCode, callerAddress, terminationRequester)", entry);
    ASSERT_NE(originResolved, std::string::npos);
    ASSERT_NE(policyCall, std::string::npos);
    ASSERT_NE(suppressionLog, std::string::npos);
    EXPECT_LT(originResolved, policyCall);
    EXPECT_LT(policyCall, suppressionLog);

    // The caller address is established before it is classified, so a hook that
    // passes none still resolves to a real module rather than to kUnknown.
    const size_t callerFallback = contents.find("callerAddress = __builtin_return_address(0);", entry);
    ASSERT_NE(callerFallback, std::string::npos);
    EXPECT_LT(callerFallback, originResolved);
}

TEST(CrashHandlerSourceTest, FatalHookBootstrapPublishesTrampolinesBeforeIatRouting) {
    const std::filesystem::path source = std::filesystem::current_path() / "hook" / "main.cpp";
    const std::string contents = ReadSourceFile(source);
    ASSERT_FALSE(contents.empty());

    const size_t bootstrap = contents.find("void TryInstallFatalTerminationDumpHooks()");
    const size_t inlineHooks = contents.find("std::vector<void*> inlineHookTargets", bootstrap);
    const size_t iatRouting = contents.find("patchRaise(\"kernel32.dll\")", inlineHooks);
    ASSERT_NE(bootstrap, std::string::npos);
    ASSERT_NE(inlineHooks, std::string::npos);
    ASSERT_NE(iatRouting, std::string::npos);
    EXPECT_LT(inlineHooks, iatRouting);

    const std::string bootstrapBody = contents.substr(bootstrap, iatRouting - bootstrap);
    EXPECT_EQ(bootstrapBody.find("ResolveNtdllExport(\"RtlRaiseException\")"), std::string::npos);
    EXPECT_EQ(bootstrapBody.find("installInlineHook(\"ntdll.dll\", \"RtlRaiseException\""), std::string::npos);
    EXPECT_NE(contents.find("::RtlRaiseException(ExceptionRecord);"), std::string::npos);
    EXPECT_NE(contents.find("::NtRaiseException(ExceptionRecord, ContextRecord, FirstChance)"), std::string::npos);
    EXPECT_NE(contents.find("::RtlExitUserProcess(ExitStatus);"), std::string::npos);
    EXPECT_EQ(contents.find("ExitProcess(static_cast<UINT>(ExitStatus));"), std::string::npos);
    EXPECT_NE(bootstrapBody.find("PatchIATAllModulesFiltered"), std::string::npos);

    const std::filesystem::path inlineSource =
        std::filesystem::current_path() / "hook" / "wrappers" / "inline_hook.cpp";
    const std::string inlineContents = ReadSourceFile(inlineSource);
    ASSERT_FALSE(inlineContents.empty());
    const size_t livePatch = inlineContents.find("if (!WriteOwnedEntryPatch(target, detour");
    const size_t publish = inlineContents.rfind("publisher(trampoline, publisherContext);", livePatch);
    ASSERT_NE(publish, std::string::npos);
    ASSERT_NE(livePatch, std::string::npos);
    EXPECT_LT(publish, livePatch);
}

// Gothic II 20260916_005504: the watchdog found a visible `#32770` window in the
// process and wrote a 29 MB "blocking dialog" dump for it 5.1 s later - while
// the game was presenting ~270 times a second into that very window. Gothic's
// own render window is registered with the dialog class, and CE always knows
// which window it composites into.
TEST(FreezeWatchdogPolicyTest, ThePresentationWindowIsNeverABlockingDialog) {
    HWND renderWindow = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x320ae0));
    HWND otherDialog = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x123456));

    EXPECT_FALSE(ce::freeze_watchdog_policy::DialogWindowCanBlockPresentation(renderWindow, renderWindow));
    EXPECT_TRUE(ce::freeze_watchdog_policy::DialogWindowCanBlockPresentation(otherDialog, renderWindow));

    // Before any presentation window is known - early startup - every dialog
    // still counts, which is what preserves the startup-crash dumps.
    EXPECT_TRUE(ce::freeze_watchdog_policy::DialogWindowCanBlockPresentation(renderWindow, nullptr));
    EXPECT_FALSE(ce::freeze_watchdog_policy::DialogWindowCanBlockPresentation(nullptr, nullptr));
}

// A freeze claim is a timeout the process may still recover from (a very long
// load on the render thread). An in-process dump suspends every thread for its
// whole duration, so the helper is preferred whenever it is registered - not
// only when a foreign overlay makes the in-process walk slow.
TEST(FreezeWatchdogPolicyTest, FreezeDumpsPreferTheExternalHelperWheneverAvailable) {
    EXPECT_TRUE(ce::freeze_watchdog_policy::ShouldPreferExternalFreezeDumpHelper(true));
    EXPECT_FALSE(ce::freeze_watchdog_policy::ShouldPreferExternalFreezeDumpHelper(false));
    const std::string dump =
        ReadSourceFile(std::filesystem::current_path() / "hook" / "common" / "freeze_watchdog_dump.cpp");
    ASSERT_FALSE(dump.empty());
    EXPECT_NE(dump.find("ShouldPreferExternalFreezeDumpHelper(HasExternalCrashDumpCapture())"), std::string::npos);
}

TEST(FreezeWatchdogPolicyTest, BackgroundFreezeSuppressionKeepsRuntimePresentationMonitored) {
    EXPECT_TRUE(ce::freeze_watchdog_policy::ShouldSuppressFreezeCheckForBackgroundProcess(false, false, false, false));
    EXPECT_FALSE(ce::freeze_watchdog_policy::ShouldSuppressFreezeCheckForBackgroundProcess(false, false, true, false));
    EXPECT_FALSE(ce::freeze_watchdog_policy::ShouldSuppressFreezeCheckForBackgroundProcess(false, true, false, false));
    EXPECT_FALSE(ce::freeze_watchdog_policy::ShouldSuppressFreezeCheckForBackgroundProcess(false, false, false, true));
    EXPECT_FALSE(ce::freeze_watchdog_policy::ShouldSuppressFreezeCheckForBackgroundProcess(true, false, false, false));
}

// Strange Brigade Vulkan 20260818_190149: the watchdog was armed on the DX12
// hook-install worker thread, the game presented through the CE Vulkan layer,
// and the heartbeat therefore never moved. Thirty seconds later it declared
// "Render thread frozen" while the game was still rendering at 144 FPS.
TEST(FreezeWatchdogPolicyTest, NeverAssertsAFreezeBeforeAnyPresentWasObserved) {
    EXPECT_FALSE(ce::freeze_watchdog_policy::ShouldAssertRenderThreadFreeze(false, false, false, false));
    EXPECT_TRUE(ce::freeze_watchdog_policy::ShouldAssertRenderThreadFreeze(true, false, false, false));
}

TEST(FreezeWatchdogPolicyTest, PresentingEvidenceWithoutHeartbeatStillAllowsFreezeAssertions) {
    // A Present stuck inside CE's own hook, a removed device, and an FG runtime
    // that owns presentation each prove a D3D render loop exists, so a hang
    // that starts before the first heartbeat still has to be dumpable.
    EXPECT_TRUE(ce::freeze_watchdog_policy::ShouldAssertRenderThreadFreeze(false, true, false, false));
    EXPECT_TRUE(ce::freeze_watchdog_policy::ShouldAssertRenderThreadFreeze(false, false, true, false));
    EXPECT_TRUE(ce::freeze_watchdog_policy::ShouldAssertRenderThreadFreeze(false, false, false, true));
}

TEST(FreezeWatchdogPolicyTest, CrossApiPresentLivenessSuppressesOnlyWhileFresh) {
    constexpr uint64_t kMaxAgeMs = 30000;
    EXPECT_TRUE(ce::freeze_watchdog_policy::IsObservedPresentRecent(100000, 100000, kMaxAgeMs));
    EXPECT_TRUE(ce::freeze_watchdog_policy::IsObservedPresentRecent(100000, 130000, kMaxAgeMs));
    EXPECT_FALSE(ce::freeze_watchdog_policy::IsObservedPresentRecent(100000, 130001, kMaxAgeMs));
    // Never published (dormant or absent layer) is not liveness evidence, and a
    // tick from the future must not be read as an enormous age either.
    EXPECT_FALSE(ce::freeze_watchdog_policy::IsObservedPresentRecent(0, 130000, kMaxAgeMs));
    EXPECT_FALSE(ce::freeze_watchdog_policy::IsObservedPresentRecent(100000, 99999, kMaxAgeMs));
}

TEST(FreezeWatchdogPolicyTest, PersistentDialogRequiresAStaleRenderHeartbeat) {
    constexpr double kFreezeTimeoutSeconds = 30.0;
    EXPECT_FALSE(ce::freeze_watchdog_policy::ShouldCapturePersistentDialogDump(
        /*criticalDialog=*/false, /*renderLoopObserved=*/true, 0.0, kFreezeTimeoutSeconds));
    EXPECT_FALSE(ce::freeze_watchdog_policy::ShouldCapturePersistentDialogDump(
        /*criticalDialog=*/false, /*renderLoopObserved=*/true, 29.999, kFreezeTimeoutSeconds));
    EXPECT_TRUE(ce::freeze_watchdog_policy::ShouldCapturePersistentDialogDump(
        /*criticalDialog=*/false, /*renderLoopObserved=*/true, kFreezeTimeoutSeconds, kFreezeTimeoutSeconds));
}

TEST(FreezeWatchdogPolicyTest, StartupAndCriticalDialogsRemainDumpable) {
    constexpr double kFreezeTimeoutSeconds = 30.0;
    EXPECT_TRUE(ce::freeze_watchdog_policy::ShouldCapturePersistentDialogDump(
        /*criticalDialog=*/false, /*renderLoopObserved=*/false, 0.0, kFreezeTimeoutSeconds));
    EXPECT_TRUE(ce::freeze_watchdog_policy::ShouldCapturePersistentDialogDump(
        /*criticalDialog=*/true, /*renderLoopObserved=*/true, 0.0, kFreezeTimeoutSeconds));
}

// DOOM Eternal may rotate Vulkan presents across an idTech worker pool while
// its WSI transport also traverses CE-observed D3D12/DXGI helper paths. Once a
// Vulkan call returns, neither the last worker nor historical helper activity
// proves a freeze. A call that remains published is exact evidence and target.
TEST(FreezeWatchdogPolicyTest, VulkanLayerEvidenceRequiresAPresentStillInFlight) {
    EXPECT_TRUE(ce::freeze_watchdog_policy::HasLiveRenderLoopEvidence(false, true, true));
    EXPECT_FALSE(ce::freeze_watchdog_policy::HasLiveRenderLoopEvidence(false, true, false));
    EXPECT_FALSE(ce::freeze_watchdog_policy::HasLiveRenderLoopEvidence(true, true, false));
    EXPECT_TRUE(ce::freeze_watchdog_policy::HasLiveRenderLoopEvidence(true, false, false));
    EXPECT_FALSE(ce::freeze_watchdog_policy::HasLiveRenderLoopEvidence(false, false, true));
}

TEST(FreezeWatchdogPolicyTest, VulkanWorkerPoolThreadSwitchLoggingIsRateLimited) {
    for (uint64_t count = 1; count <= 8; ++count) {
        EXPECT_TRUE(ce::freeze_watchdog_policy::ShouldLogVulkanPresentThreadSwitch(count));
    }
    EXPECT_FALSE(ce::freeze_watchdog_policy::ShouldLogVulkanPresentThreadSwitch(9));
    EXPECT_TRUE(ce::freeze_watchdog_policy::ShouldLogVulkanPresentThreadSwitch(16));
    EXPECT_FALSE(ce::freeze_watchdog_policy::ShouldLogVulkanPresentThreadSwitch(17));
    EXPECT_TRUE(ce::freeze_watchdog_policy::ShouldLogVulkanPresentThreadSwitch(1024));
}

// A freeze dump that names no thread is a freeze dump that hides its own cause.
// DOOM Eternal `20260819_020933` had no monitored render thread (the DXGI
// heartbeat came from a helper-thread path that deliberately does not adopt),
// so the dump targeted tid 0 and !analyze -hang reported the idle main thread
// while the actual deadlock sat in CE's flip-queue pacing wait on the Vulkan
// runtime's presenter thread.
TEST(FreezeWatchdogPolicyTest, DumpTargetsTheThreadStuckInsideCePresentHook) {
    // A thread that already claimed the render loop keeps the claim.
    EXPECT_EQ(ce::freeze_watchdog_policy::ResolveFreezeDumpTargetThread(0x1234, true, 0x2164), 0x1234u);
    // Otherwise the present that never returned names the target.
    EXPECT_EQ(ce::freeze_watchdog_policy::ResolveFreezeDumpTargetThread(0, true, 0x2164), 0x2164u);
    // No present in flight is no evidence, and must not invent a target.
    EXPECT_EQ(ce::freeze_watchdog_policy::ResolveFreezeDumpTargetThread(0, false, 0x2164), 0u);
    EXPECT_EQ(ce::freeze_watchdog_policy::ResolveFreezeDumpTargetThread(0, true, 0), 0u);
}

TEST(FreezeWatchdogPolicyTest, HeartbeatsArmFreezeAssertionsAndAdoptTheRenderThread) {
    FreezeWatchdog watchdog;
    EXPECT_FALSE(watchdog.HasObservedRenderLoop());
    EXPECT_EQ(watchdog.GetMonitoredThreadId(), 0u);

    watchdog.HeartbeatFromHelperThread();
    EXPECT_TRUE(watchdog.HasObservedRenderLoop());
    // Helper heartbeats prove liveness but must not claim to be the render thread.
    EXPECT_EQ(watchdog.GetMonitoredThreadId(), 0u);

    watchdog.Heartbeat();
    EXPECT_EQ(watchdog.GetMonitoredThreadId(), GetCurrentThreadId());
}

// The freeze dump used to be written in-process with every thread suspended.
// Under a foreign overlay's loader/version hooks that walk takes ~60 s
// (20260817_052857), so the dump froze the game far longer than the freeze it
// claimed to document — including when the freeze claim itself was wrong.
TEST(FreezeWatchdogPolicyTest, FreezeDumpPrefersTheExternalHelperUnderAForeignOverlay) {
    EXPECT_TRUE(ce::crash_dump_policy::ShouldPreferExternalCrashDumpHelper(true, true));
    EXPECT_FALSE(ce::crash_dump_policy::ShouldPreferExternalCrashDumpHelper(false, true));
    EXPECT_FALSE(ce::crash_dump_policy::ShouldPreferExternalCrashDumpHelper(true, false));
    // Helper unavailable plus foreign overlay: no dump beats a hung game thread.
    EXPECT_FALSE(ce::crash_dump_policy::ShouldUseInProcessMiniDumpFallbackAfterExternalHelperFailure(true));
    EXPECT_TRUE(ce::crash_dump_policy::ShouldUseInProcessMiniDumpFallbackAfterExternalHelperFailure(false));
}

// Gothic II 20260916_014133. The dialog CE found was owned by the very thread
// it monitors for presents, which is not a thread stuck in a driver, a hook or
// a lock: it is a thread running a modal message pump, which is exactly why the
// heartbeat stopped. CE still records the freeze - the user's game is frozen -
// but a full-memory dump of a process sitting in DialogBox records nothing.
TEST(FreezeWatchdogPolicyTest, ADialogOwnedByTheRenderThreadExplainsItsOwnFreeze) {
    constexpr DWORD kRenderThread = 812;
    constexpr DWORD kOtherThread = 4242;

    EXPECT_TRUE(ce::freeze_watchdog_policy::FreezeIsExplainedByApplicationDialog(
        /*haveDialog=*/true, /*criticalDialog=*/false, kRenderThread, kRenderThread));

    // A dialog on another thread does not explain why the render thread stopped.
    EXPECT_FALSE(ce::freeze_watchdog_policy::FreezeIsExplainedByApplicationDialog(
        /*haveDialog=*/true, /*criticalDialog=*/false, kOtherThread, kRenderThread));

    // No dialog, no explanation.
    EXPECT_FALSE(ce::freeze_watchdog_policy::FreezeIsExplainedByApplicationDialog(
        /*haveDialog=*/false, /*criticalDialog=*/false, kRenderThread, kRenderThread));

    // A known-critical dialog is dumped immediately and in full on purpose.
    EXPECT_FALSE(ce::freeze_watchdog_policy::FreezeIsExplainedByApplicationDialog(
        /*haveDialog=*/true, /*criticalDialog=*/true, kRenderThread, kRenderThread));

    // Nothing is claimed when either thread is unknown.
    EXPECT_FALSE(ce::freeze_watchdog_policy::FreezeIsExplainedByApplicationDialog(
        /*haveDialog=*/true, /*criticalDialog=*/false, 0, 0));
    EXPECT_FALSE(ce::freeze_watchdog_policy::FreezeIsExplainedByApplicationDialog(
        /*haveDialog=*/true, /*criticalDialog=*/false, kRenderThread, 0));
}

// The scope only reaches the dump writer through the helper's command line, so
// the argument the hook emits and the argument the helper parses have to stay
// the same string.
TEST(FreezeWatchdogPolicyTest, TheStackOnlyScopeReachesTheExternalDumpHelper) {
    const std::string hookSide = ce::test_source::ReadLogicalSource(
        std::filesystem::current_path() / "hook" / "main_fatal_dump.cpp");
    const std::string helperSide = ce::test_source::ReadLogicalSource(
        std::filesystem::current_path() / "captureengine" / "dump_helper.cpp");
    ASSERT_FALSE(hookSide.empty());
    ASSERT_FALSE(helperSide.empty());

    EXPECT_NE(hookSide.find("--dump-helper-scope=stacks"), std::string::npos);
    EXPECT_NE(helperSide.find("L\"--dump-helper-scope=\""), std::string::npos);
    EXPECT_NE(helperSide.find("kStackOnlyDumpType"), std::string::npos);
}
