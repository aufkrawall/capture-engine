#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>

#include "source_fragment_reader.h"

#include "../common/crash_dump_policy.h"
#include "../common/cpp_exception_message.h"

namespace policy = ce::crash_dump_policy;

namespace {

bool HasDumpFlag(MINIDUMP_TYPE value, MINIDUMP_TYPE flag) {
    const auto rawValue = static_cast<unsigned int>(value);
    const auto rawFlag = static_cast<unsigned int>(flag);
    return (rawValue & rawFlag) == rawFlag;
}

}  // namespace

TEST(CrashDumpPolicyTest, RichCrashDumpAddsHighValueContextWithoutFullMemory) {
    const auto flags = policy::kRichCrashDumpType;

    EXPECT_TRUE(HasDumpFlag(flags, MiniDumpWithHandleData));
    EXPECT_TRUE(HasDumpFlag(flags, MiniDumpWithThreadInfo));
    EXPECT_TRUE(HasDumpFlag(flags, MiniDumpWithUnloadedModules));
    EXPECT_TRUE(HasDumpFlag(flags, MiniDumpWithIndirectlyReferencedMemory));
    EXPECT_TRUE(HasDumpFlag(flags, MiniDumpWithProcessThreadData));
    EXPECT_TRUE(HasDumpFlag(flags, MiniDumpWithFullMemoryInfo));
    EXPECT_TRUE(HasDumpFlag(flags, MiniDumpScanMemory));
    EXPECT_TRUE(HasDumpFlag(flags, MiniDumpIgnoreInaccessibleMemory));
    EXPECT_FALSE(HasDumpFlag(flags, MiniDumpWithFullMemory));
}

TEST(CrashDumpPolicyTest, RichFreezeDumpKeepsFreezeRelevantMetadata) {
    const auto flags = policy::kRichFreezeDumpType;

    EXPECT_TRUE(HasDumpFlag(flags, MiniDumpWithHandleData));
    EXPECT_TRUE(HasDumpFlag(flags, MiniDumpWithThreadInfo));
    EXPECT_TRUE(HasDumpFlag(flags, MiniDumpWithUnloadedModules));
    EXPECT_TRUE(HasDumpFlag(flags, MiniDumpWithIndirectlyReferencedMemory));
    EXPECT_TRUE(HasDumpFlag(flags, MiniDumpWithProcessThreadData));
    EXPECT_TRUE(HasDumpFlag(flags, MiniDumpWithFullMemoryInfo));
    EXPECT_FALSE(HasDumpFlag(flags, MiniDumpWithFullMemory));
}

TEST(CrashDumpPolicyTest, ArtifactArchiverKeepsRuntimeImagesAndPdbsOnly) {
    EXPECT_TRUE(policy::ShouldArchiveInstalledCrashArtifactFileName("captureengine.exe"));
    EXPECT_TRUE(policy::ShouldArchiveInstalledCrashArtifactFileName("capture_hook_x64.dll"));
    EXPECT_TRUE(policy::ShouldArchiveInstalledCrashArtifactFileName("captureengine.pdb"));
    EXPECT_TRUE(policy::ShouldArchiveInstalledCrashArtifactFileName("VK_LAYER_CE_overlay_x86.DLL"));

    EXPECT_FALSE(policy::ShouldArchiveInstalledCrashArtifactFileName("config.ini"));
    EXPECT_FALSE(policy::ShouldArchiveInstalledCrashArtifactFileName("manifest.txt"));
    EXPECT_FALSE(policy::ShouldArchiveInstalledCrashArtifactFileName("capture_hook_x64.dll.old.1234"));
    EXPECT_FALSE(policy::ShouldArchiveInstalledCrashArtifactFileName("notes.pdb.txt"));
}

TEST(CrashDumpPolicyTest, ExternalDumpMirrorSkipsSessionLocalTargets) {
    EXPECT_FALSE(policy::ShouldMirrorExternalDumpToSessionDirectory(
        R"(C:\captureengine\logs\20260413_231005\crash_foo.dmp)", R"(C:\captureengine\logs\20260413_231005)"));
    EXPECT_FALSE(policy::ShouldMirrorExternalDumpToSessionDirectory(
        R"(C:/captureengine/logs/20260413_231005/external_bar.dmp)", R"(C:\captureengine\logs\20260413_231005\)"));
    EXPECT_TRUE(policy::ShouldMirrorExternalDumpToSessionDirectory(
        R"(C:\Users\TestUser\AppData\Local\Rockstar Games\GTAV Enhanced\CrashLogs\51e9b489-70bb-4998-a4ec-254bdd858cbd.dmp)",
        R"(C:\captureengine\logs\20260413_231005)"));
    EXPECT_TRUE(
        policy::ShouldMirrorExternalDumpToSessionDirectory(nullptr, R"(C:\captureengine\logs\20260413_231005)"));
}

TEST(CrashDumpPolicyTest, ExternalDumpMirrorBuildsStableDestinationFileNames) {
    const std::string mirrored = policy::BuildMirroredExternalDumpFileName(
        R"(C:\Users\TestUser\AppData\Local\Rockstar Games\GTAV Enhanced\CrashLogs\51e9b489-70bb-4998-a4ec-254bdd858cbd.dmp)");
    EXPECT_NE(mirrored.find("external_51e9b489-70bb-4998-a4ec-254bdd858cbd_"), std::string::npos);
    EXPECT_TRUE(policy::EndsWithAsciiInsensitive(mirrored.c_str(), ".dmp"));

    EXPECT_NE(policy::BuildMirroredExternalDumpFileName(R"(C:\a\sl-sha-da40c631.dmp)"),
              policy::BuildMirroredExternalDumpFileName(R"(C:\b\sl-sha-da40c631.dmp)"));
    EXPECT_NE(policy::BuildMirroredExternalDumpFileName("crashcontext").find("external_crashcontext_"),
              std::string::npos);
    EXPECT_EQ(policy::BuildMirroredExternalDumpFileName(nullptr), "external_dump.dmp");
}

TEST(CrashDumpPolicyTest, SupplementalExternalCrashDumpBuildsStableDestinationFileNames) {
    const std::string supplemental = policy::BuildSupplementalCrashDumpFileNameFromExternalSource(
        R"(C:\Users\TestUser\AppData\Local\Rockstar Games\GTAV Enhanced\CrashLogs\103327d5-227b-4bb7-b529-7c8a38cccdbf.dmp)");
    EXPECT_NE(supplemental.find("crash_external_103327d5-227b-4bb7-b529-7c8a38cccdbf_"), std::string::npos);
    EXPECT_TRUE(policy::EndsWithAsciiInsensitive(supplemental.c_str(), ".dmp"));
    EXPECT_NE(policy::BuildSupplementalCrashDumpFileNameFromExternalSource("sl-sha-11cf43f")
                  .find("crash_external_sl-sha-11cf43f_"),
              std::string::npos);
    EXPECT_EQ(policy::BuildSupplementalCrashDumpFileNameFromExternalSource(nullptr), "crash_external_dump.dmp");
}

TEST(CrashDumpPolicyTest, InProgressDumpFileNamesAreNotFinalDmpArtifacts) {
    const std::string inProgressName = policy::BuildInProgressDumpFileName("crash_20260513_032512.dmp");

    EXPECT_EQ(inProgressName, "crash_20260513_032512.dmp.inprogress");
    EXPECT_FALSE(policy::EndsWithAsciiInsensitive(inProgressName.c_str(), ".dmp"));
    EXPECT_EQ(policy::BuildInProgressDumpFileName(nullptr), "dump.dmp.inprogress");
}

TEST(CrashDumpPolicyTest, EmptyInProgressDumpArtifactsAreStaleCleanupTargets) {
    EXPECT_TRUE(policy::IsStaleEmptyInProgressDumpArtifact("crash_external_fatal_exit.dmp.inprogress", 0));
    EXPECT_TRUE(policy::IsStaleEmptyInProgressDumpArtifact("CRASH.DMP.INPROGRESS", 0));

    EXPECT_FALSE(policy::IsStaleEmptyInProgressDumpArtifact("crash_external_fatal_exit.dmp.inprogress", 1));
    EXPECT_FALSE(policy::IsStaleEmptyInProgressDumpArtifact("crash_external_fatal_exit.dmp", 0));
    EXPECT_FALSE(policy::IsStaleEmptyInProgressDumpArtifact(nullptr, 0));
}

TEST(CrashDumpPolicyTest, ExternalDumpStormUsesStrongSignatureForTermination) {
    policy::ExternalDumpSignature signature;
    signature.processId = 1234;
    signature.dumpBaseName = "sl-sha-da40c631.dmp";
    signature.exceptionCode = 0xE06D7363;
    signature.exceptionAddress = 0x7ff600001234;
    signature.exceptionThreadId = 99;
    signature.hasExceptionInfo = true;

    EXPECT_TRUE(policy::IsStrongExternalDumpSignature(signature));
    EXPECT_NE(policy::BuildExternalDumpSignatureKey(signature).find("sl-sha-da40c631.dmp"), std::string::npos);
    EXPECT_FALSE(policy::ShouldSuppressDuplicateExternalDumpArtifacts(1, false));
    EXPECT_TRUE(policy::ShouldSuppressDuplicateExternalDumpArtifacts(2, true));
    EXPECT_TRUE(policy::ShouldTerminateAfterExternalDumpStorm(true, 3, 1000, 2000, true, false));
    EXPECT_FALSE(policy::ShouldTerminateAfterExternalDumpStorm(false, 3, 1000, 2000, true, false));
    EXPECT_FALSE(policy::ShouldTerminateAfterExternalDumpStorm(true, 3, 1000, 40000, true, false));
    EXPECT_FALSE(policy::ShouldTerminateAfterExternalDumpStorm(true, 3, 1000, 2000, false, false));
    EXPECT_FALSE(policy::ShouldTerminateAfterExternalDumpStorm(true, 3, 1000, 2000, true, true));
}

TEST(CrashDumpPolicyTest, WeakExternalDumpSignatureCanDedupButCannotTerminate) {
    policy::ExternalDumpSignature signature;
    signature.processId = 1234;
    signature.dumpBaseName = "sl-sha-da40c631.dmp";

    EXPECT_FALSE(policy::IsStrongExternalDumpSignature(signature));
    EXPECT_TRUE(policy::ShouldSuppressDuplicateExternalDumpArtifacts(5, true));
    EXPECT_FALSE(policy::ShouldTerminateAfterExternalDumpStorm(policy::IsStrongExternalDumpSignature(signature), 5, 10,
                                                               20, true, false));
}

TEST(CrashDumpPolicyTest, PreTerminationDumpCapturesOnlyCurrentProcessCrashLikeExitCodesOnce) {
    EXPECT_TRUE(policy::ShouldCapturePreTerminationDump(true, policy::kFailFastExceptionExitCode, false));
    EXPECT_TRUE(policy::ShouldCapturePreTerminationDump(true, policy::kBreakpointExceptionExitCode, false));
    EXPECT_TRUE(policy::ShouldCapturePreTerminationDump(true, EXCEPTION_ACCESS_VIOLATION, false));
    EXPECT_TRUE(policy::ShouldCapturePreTerminationDump(true, 0xC000001D, false));

    EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(false, policy::kFailFastExceptionExitCode, false));
    EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(true, policy::kFailFastExceptionExitCode, true));
    EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(true, policy::kExternalDumpStormTerminationExitCode, false));
    EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(true, 0, false));
    EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(true, 1, false));

    // STATUS_PROCESS_IS_TERMINATING (0xC000004B) is NVIDIA's DLSS snippet
    // worker teardown race, not a crash: the game exits cleanly (code 0) and
    // the concurrent NtTerminateProcess loses the race. No dump should be
    // written, with or without an active FG runtime.
    EXPECT_FALSE(policy::IsCrashLikeProcessExitCode(policy::kProcessIsTerminatingExitCode));
    EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(true, policy::kProcessIsTerminatingExitCode, false));
    EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(true, policy::kProcessIsTerminatingExitCode, false, true));
}

// Session 20260924_073945: Strange Brigade quit through exit(-1) from a Steam
// callback and CE held the exit 1.7 s for a 49 MB "fatal exit" dump.
TEST(CrashDumpPolicyTest, SmallNegativeApplicationExitCodesAreNotCrashes) {
    for (const DWORD code : {0xFFFFFFFFu, 0xFFFFFFFEu, 0xFFFFFF00u, 0xFFFF0001u}) {
        EXPECT_TRUE(policy::IsSmallNegativeApplicationExitCode(code)) << std::hex << code;
        EXPECT_FALSE(policy::IsCrashLikeProcessExitCode(code)) << std::hex << code;
        EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(true, code, false)) << std::hex << code;
        EXPECT_FALSE(policy::ShouldAdoptWerDumpForTrackedProcessExit(code, false)) << std::hex << code;
        EXPECT_STREQ(policy::DescribeProcessExitCodeClass(code), "normal exit");
    }
    // Real exception/NTSTATUS exits keep their crash classification.
    EXPECT_FALSE(policy::IsSmallNegativeApplicationExitCode(EXCEPTION_ACCESS_VIOLATION));
    EXPECT_FALSE(policy::IsSmallNegativeApplicationExitCode(0xE06D7363u));
    EXPECT_TRUE(policy::IsCrashLikeProcessExitCode(0xE06D7363u));
    EXPECT_TRUE(policy::IsCrashLikeProcessExitCode(policy::kFailFastExceptionExitCode));
    EXPECT_FALSE(policy::IsSmallNegativeApplicationExitCode(0));
    // exit(-1) after an unresolved fault still dumps: the fault decides, not the code.
    EXPECT_TRUE(policy::ShouldCapturePreTerminationDump(true, 0xFFFFFFFFu, false, false,
                                                        policy::TerminationOrigin::kPrimaryModule, true));
}

TEST(CrashDumpPolicyTest, PreTerminationDumpCapturesActiveFrameGenerationRuntimeExits) {
    EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(true, 0, false, true));
    EXPECT_TRUE(policy::ShouldCapturePreTerminationDump(true, 1, false, true));

    EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(false, 0, false, true));
    EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(true, 0, true, true));
    EXPECT_FALSE(
        policy::ShouldCapturePreTerminationDump(true, policy::kExternalDumpStormTerminationExitCode, false, true));
}

// Portal RTX sessions 20260901_202149 and 20260902_071853: the 64-bit
// NvRemixBridge.exe renderer ends itself with TerminateProcess(1) from its own
// image every time the game quits, and DLSS-G is still nominally active because
// quitting never turns frame generation off first. Both runs therefore produced
// a 191 MB "crash" dump for a clean exit, taking 2.5s at shutdown, while the two
// genuine crashes in the same log set (DOOM STATUS_BREAKPOINT, Witcher 3
// access violation) were caught by the crash-like exit code and never depended
// on this fallback. A live FG runtime is the normal state at exit, so it cannot
// by itself mean the exit was abnormal.
TEST(CrashDumpPolicyTest, PreTerminationDumpSkipsAnApplicationTerminatingItself) {
    EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(true, 1, false, true,
                                                         policy::TerminationOrigin::kPrimaryModule));

    // Everything the fallback exists for still dumps: a request raised from a
    // loaded module - an FG runtime, a driver, an injected component - and any
    // request whose origin could not be resolved.
    EXPECT_TRUE(
        policy::ShouldCapturePreTerminationDump(true, 1, false, true, policy::TerminationOrigin::kLoadedModule));
    EXPECT_TRUE(policy::ShouldCapturePreTerminationDump(true, 1, false, true, policy::TerminationOrigin::kUnknown));
}

// Portal RTX session 20260914_130052: the same quit produced both decisions.
// NvRemixBridge.exe called TerminateProcess(1) from its own image, CE's
// TerminateProcess hook resolved kPrimaryModule and correctly suppressed the
// dump - and then KERNELBASE's own implementation called NtTerminateProcess,
// where CE's second hook saw KERNELBASE as the caller, called the request a
// loaded-module one, and wrote the 183 MB dump the first hook had just refused.
// A request is attributed to the frame that made it, never to the layer
// carrying it down.
TEST(CrashDumpPolicyTest, LayeredTerminationRequestIsAttributedToItsRequester) {
    using Kind = policy::TerminationFrameKind;
    const Kind portalRtxNtTerminateProcess[] = {
        Kind::kCaptureEngine,        // HookedNtTerminateProcess
        Kind::kCaptureEngine,        // CapturePreTerminationDumpIfNeeded
        Kind::kTerminationPlumbing,  // KERNELBASE!TerminateProcess
        Kind::kPrimaryModule,        // NvRemixBridge.exe - the requester
        Kind::kTerminationPlumbing,  // ntdll thread start
    };
    size_t requesterFrame = 0;
    EXPECT_EQ(policy::ResolveTerminationOriginFromFrames(portalRtxNtTerminateProcess,
                                                         std::size(portalRtxNtTerminateProcess), &requesterFrame),
              policy::TerminationOrigin::kPrimaryModule);
    EXPECT_EQ(requesterFrame, 3u);

    // The outermost hook of that same request sees the requester directly, and
    // both layers must therefore agree.
    const Kind portalRtxTerminateProcess[] = {Kind::kCaptureEngine, Kind::kPrimaryModule};
    EXPECT_EQ(policy::ResolveTerminationOriginFromFrames(portalRtxTerminateProcess,
                                                         std::size(portalRtxTerminateProcess)),
              policy::TerminationOrigin::kPrimaryModule);
}

// Everything the FG fallback exists for still reaches the dump: a runtime that
// ends the process while tearing down is found behind the same plumbing.
TEST(CrashDumpPolicyTest, LayeredTerminationRequestFromALoadedModuleStillDumps) {
    using Kind = policy::TerminationFrameKind;
    const Kind frames[] = {Kind::kCaptureEngine, Kind::kTerminationPlumbing, Kind::kOtherModule,
                           Kind::kPrimaryModule};
    size_t requesterFrame = 0;
    EXPECT_EQ(policy::ResolveTerminationOriginFromFrames(frames, std::size(frames), &requesterFrame),
              policy::TerminationOrigin::kLoadedModule);
    EXPECT_EQ(requesterFrame, 2u);
}

// The classifier fails open in every direction it cannot prove: an
// unattributable frame stops the walk rather than letting the search run past
// it to a module that did not make the request, a stack of nothing but carrying
// layers attributes nothing, and neither can suppress a dump.
TEST(CrashDumpPolicyTest, UnattributableTerminationRequestResolvesToUnknown) {
    using Kind = policy::TerminationFrameKind;
    const Kind opaqueFrame[] = {Kind::kCaptureEngine, Kind::kTerminationPlumbing, Kind::kUnresolved,
                                Kind::kPrimaryModule};
    size_t requesterFrame = 0;
    EXPECT_EQ(policy::ResolveTerminationOriginFromFrames(opaqueFrame, std::size(opaqueFrame), &requesterFrame),
              policy::TerminationOrigin::kUnknown);
    EXPECT_EQ(requesterFrame, std::size(opaqueFrame));

    const Kind plumbingOnly[] = {Kind::kCaptureEngine, Kind::kTerminationPlumbing, Kind::kTerminationPlumbing};
    EXPECT_EQ(policy::ResolveTerminationOriginFromFrames(plumbingOnly, std::size(plumbingOnly)),
              policy::TerminationOrigin::kUnknown);
    EXPECT_EQ(policy::ResolveTerminationOriginFromFrames(plumbingOnly, 0), policy::TerminationOrigin::kUnknown);
    EXPECT_EQ(policy::ResolveTerminationOriginFromFrames(nullptr, 4), policy::TerminationOrigin::kUnknown);

    // kUnknown is the answer that still dumps, so an unattributable request is
    // never quietly dropped.
    EXPECT_TRUE(policy::ShouldCapturePreTerminationDump(true, 1, false, true, policy::TerminationOrigin::kUnknown));
}

// Only the modules that forward a termination request into one another count as
// plumbing. The list must stay narrow: NVIDIA's FG runtimes are loaded from the
// DriverStore under the Windows directory, and an FG runtime killing the
// process during teardown is exactly what the fallback exists to capture.
TEST(CrashDumpPolicyTest, OnlyTerminationForwardersCountAsPlumbing) {
    EXPECT_TRUE(policy::IsTerminationPlumbingModuleName("ntdll.dll"));
    EXPECT_TRUE(policy::IsTerminationPlumbingModuleName("KERNELBASE.dll"));
    EXPECT_TRUE(policy::IsTerminationPlumbingModuleName("KERNEL32.DLL"));
    EXPECT_TRUE(policy::IsTerminationPlumbingModuleName("ucrtbase.dll"));
    EXPECT_TRUE(policy::IsTerminationPlumbingModuleName("msvcrt.dll"));

    EXPECT_FALSE(policy::IsTerminationPlumbingModuleName("nvngx_dlssg.dll"));
    EXPECT_FALSE(policy::IsTerminationPlumbingModuleName("sl.interposer.dll"));
    EXPECT_FALSE(policy::IsTerminationPlumbingModuleName("amd_fidelityfx_dx12.dll"));
    EXPECT_FALSE(policy::IsTerminationPlumbingModuleName("NvRemixBridge.exe"));
    EXPECT_FALSE(policy::IsTerminationPlumbingModuleName("ntdll.dll.bak"));
    EXPECT_FALSE(policy::IsTerminationPlumbingModuleName("ntdll"));
    EXPECT_FALSE(policy::IsTerminationPlumbingModuleName(""));
    EXPECT_FALSE(policy::IsTerminationPlumbingModuleName(nullptr));
}

// The origin only ever gates the frame-generation fallback. A crash-like exit
// code is dump-worthy on its own, wherever the request came from, so a game
// whose statically linked CRT calls abort() from inside the executable still
// produces its dump.
TEST(CrashDumpPolicyTest, TerminationOriginNeverSuppressesACrashLikeExit) {
    for (const policy::TerminationOrigin origin : {policy::TerminationOrigin::kUnknown,
                                                   policy::TerminationOrigin::kPrimaryModule,
                                                   policy::TerminationOrigin::kLoadedModule}) {
        EXPECT_TRUE(policy::ShouldCapturePreTerminationDump(true, policy::kFailFastExceptionExitCode, false, false,
                                                            origin));
        EXPECT_TRUE(policy::ShouldCapturePreTerminationDump(true, policy::kBreakpointExceptionExitCode, false, false,
                                                            origin));
        EXPECT_TRUE(
            policy::ShouldCapturePreTerminationDump(true, EXCEPTION_ACCESS_VIOLATION, false, false, origin));
        EXPECT_TRUE(policy::ShouldCapturePreTerminationDump(true, EXCEPTION_STACK_OVERFLOW, false, true, origin));

        // And the origin cannot resurrect an exit the other rules already
        // refused, whichever way it points.
        EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(true, 0, false, true, origin));
        EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(true, 1, false, false, origin));
        EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(false, 1, false, true, origin));
        EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(true, 1, true, true, origin));
        EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(true, policy::kProcessIsTerminatingExitCode, false, true,
                                                             origin));
    }
}

TEST(CrashDumpPolicyTest, InProcessDumpFallbackRefusedWithForeignOverlayLoaded) {
    // The in-process MiniDumpWriteDump fallback deadlocked the game's render thread inside the Steam
    // overlay's hooked version APIs (session 20260813_222058: dbgcore -> GetFileVersionInfoW ->
    // gameoverlayrenderer64 blocked until the FreezeWatchdog killed the app). The fallback stays legal
    // only while no foreign overlay module is loaded; the external helper process has none.
    EXPECT_TRUE(policy::ShouldUseInProcessMiniDumpFallbackAfterExternalHelperFailure(false));
    EXPECT_FALSE(policy::ShouldUseInProcessMiniDumpFallbackAfterExternalHelperFailure(true));
}

TEST(CrashDumpPolicyTest, FirstChanceExceptionsBelowErrorSeverityAreNotCrashes) {
    using Action = policy::FirstChanceAction;
    // Black Myth: Wukong exit (session 20260817_052857): CEF cancels its session
    // notification wait while shutting down and rpcrt4 raises
    // RPC_S_CALL_CANCELLED (0x0000071A) through RaiseException. RPC handles it
    // itself. CE classified it as a crash twice, froze the exiting game for
    // ~62 s per dump, and then had no dump budget left for the real access
    // violation that followed.
    EXPECT_EQ(policy::ClassifyFirstChanceException(0x0000071AUL, false, false), Action::kIgnore);
    EXPECT_EQ(policy::ClassifyFirstChanceException(0x406D1388UL, false, false), Action::kIgnore);  // thread naming
    EXPECT_EQ(policy::ClassifyFirstChanceException(0x40010006UL, false, false), Action::kIgnore);  // OutputDebugString
    EXPECT_EQ(policy::ClassifyFirstChanceException(0x000006BAUL, false, false), Action::kIgnore);
    EXPECT_EQ(policy::ClassifyFirstChanceException(0xE06D7363UL, false, false), Action::kIgnore);  // MSVC C++ EH
    EXPECT_EQ(policy::ClassifyFirstChanceException(0x20474343UL, false, false), Action::kIgnore);  // GCC C++ EH

    // The top-level unhandled filter re-enters with forceDump, so an exception
    // that really did reach it is never lost to this gate.
    EXPECT_EQ(policy::ClassifyFirstChanceException(0x0000071AUL, true, false), Action::kDumpNow);
    EXPECT_EQ(policy::ClassifyFirstChanceException(EXCEPTION_ACCESS_VIOLATION, true, false), Action::kDumpNow);
}

// Regression: every first-chance error-severity exception was dumped. Mono
// (Unity) and the JVM handle access violations as null checks and safepoints,
// emulators as memory mapping, .NET and LuaJIT raise error-severity codes for
// every managed/script exception - each of them cost an in-process dump stall,
// the process's only dump budget, and three crash.log writes per exception.
TEST(CrashDumpPolicyTest, FirstChanceFaultsAreRecordedNotDumped) {
    using Action = policy::FirstChanceAction;
    EXPECT_EQ(policy::ClassifyFirstChanceException(EXCEPTION_ACCESS_VIOLATION, false, false), Action::kRecordFault);
    EXPECT_EQ(policy::ClassifyFirstChanceException(EXCEPTION_ILLEGAL_INSTRUCTION, false, false), Action::kRecordFault);
    EXPECT_EQ(policy::ClassifyFirstChanceException(EXCEPTION_PRIV_INSTRUCTION, false, false), Action::kRecordFault);
    EXPECT_EQ(policy::ClassifyFirstChanceException(EXCEPTION_INT_DIVIDE_BY_ZERO, false, false), Action::kRecordFault);
    EXPECT_EQ(policy::ClassifyFirstChanceException(EXCEPTION_IN_PAGE_ERROR, false, false), Action::kRecordFault);

    // Application-defined codes (customer bit) are software raises a runtime
    // handles itself; an unhandled one reaches the top-level filter.
    EXPECT_EQ(policy::ClassifyFirstChanceException(0xE0434352UL, false, false), Action::kIgnore);  // .NET
    EXPECT_EQ(policy::ClassifyFirstChanceException(0xE24C4A02UL, false, false), Action::kIgnore);  // LuaJIT
    EXPECT_TRUE(policy::IsApplicationDefinedExceptionCode(0xE0434352UL));
    EXPECT_FALSE(policy::IsApplicationDefinedExceptionCode(EXCEPTION_ACCESS_VIOLATION));

    // COM/DXGI HRESULTs raised as exceptions are handled by their raisers; they
    // no longer dump at first chance.
    EXPECT_EQ(policy::ClassifyFirstChanceException(0x80010108UL, false, false), Action::kIgnore);
    EXPECT_EQ(policy::ClassifyFirstChanceException(0x887a0007UL, false, false), Action::kIgnore);
}

TEST(CrashDumpPolicyTest, InherentlyFatalCodesStillDumpImmediately) {
    using Action = policy::FirstChanceAction;
    EXPECT_EQ(policy::ClassifyFirstChanceException(EXCEPTION_STACK_OVERFLOW, false, false), Action::kDumpNow);
    EXPECT_EQ(policy::ClassifyFirstChanceException(policy::kFailFastExceptionExitCode, false, false),
              Action::kDumpNow);
    EXPECT_EQ(policy::ClassifyFirstChanceException(0xC0000374UL, false, false), Action::kDumpNow);  // heap corruption
    EXPECT_EQ(policy::ClassifyFirstChanceException(0xC000041DUL, false, false), Action::kDumpNow);
    EXPECT_EQ(policy::ClassifyFirstChanceException(policy::kUe5EnsureExceptionCode, false, false),
              Action::kQuickAssertDump);
    EXPECT_EQ(policy::ClassifyFirstChanceException(EXCEPTION_BREAKPOINT, false, true), Action::kIgnore);
}

// A recorded fault becomes a dump when the process dies of it: the terminating
// thread is still inside that exception's dispatch (a game's own unhandled
// filter or crash reporter calling TerminateProcess), or it recorded a fault no
// handler resumed from. A clean zero exit never qualifies.
TEST(CrashDumpPolicyTest, TerminationFollowingAnUnresolvedFaultIsDumped) {
    EXPECT_TRUE(policy::IsTerminationFollowingUnresolvedFault(3, true, false));
    EXPECT_TRUE(policy::IsTerminationFollowingUnresolvedFault(1, false, true));
    EXPECT_FALSE(policy::IsTerminationFollowingUnresolvedFault(0, true, true));
    EXPECT_FALSE(policy::IsTerminationFollowingUnresolvedFault(3, false, false));

    // Exit code 3 from the game's own image is normally suppressed ...
    EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(true, 3, false, true,
                                                         policy::TerminationOrigin::kPrimaryModule));
    // ... but not when it follows a fault the process never recovered from.
    EXPECT_TRUE(policy::ShouldCapturePreTerminationDump(true, 3, false, false,
                                                        policy::TerminationOrigin::kPrimaryModule, true));
    // The once-per-process budget and the sentinel exit codes still win.
    EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(true, 3, true, false,
                                                         policy::TerminationOrigin::kPrimaryModule, true));
    EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(true, policy::kProcessIsTerminatingExitCode, false, false,
                                                         policy::TerminationOrigin::kUnknown, true));
    EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(false, 3, false, false,
                                                         policy::TerminationOrigin::kUnknown, true));
}

TEST(CrashDumpPolicyTest, ExceptionSeverityClassificationFollowsNtstatusBits) {
    EXPECT_TRUE(policy::IsErrorSeverityExceptionCode(0xC0000005UL));
    EXPECT_TRUE(policy::IsErrorSeverityExceptionCode(0xE06D7363UL));  // MSVC C++ EH
    EXPECT_FALSE(policy::IsErrorSeverityExceptionCode(0x0000071AUL));
    EXPECT_FALSE(policy::IsErrorSeverityExceptionCode(0x40010006UL));
    EXPECT_FALSE(policy::IsErrorSeverityExceptionCode(0x80000003UL));
    EXPECT_FALSE(policy::IsErrorSeverityExceptionCode(0UL));
}

TEST(CrashDumpPolicyTest, CrashDumpsPreferTheExternalHelperWithForeignOverlaysLoaded) {
    // Same hazard as the pre-termination fallback, on the vectored-handler path:
    // an in-process dbghelp module walk goes through the foreign overlay's
    // loader/version hooks while every other thread stays suspended.
    EXPECT_TRUE(policy::ShouldPreferExternalCrashDumpHelper(true, true));
    EXPECT_FALSE(policy::ShouldPreferExternalCrashDumpHelper(true, false));
    EXPECT_FALSE(policy::ShouldPreferExternalCrashDumpHelper(false, true));

    // With no overlay loaded the in-process worker stays the direct path, and
    // with one loaded but no helper registered a missing dump beats a frozen
    // game.
    EXPECT_TRUE(policy::ShouldUseInProcessMiniDumpFallbackAfterExternalHelperFailure(false));
    EXPECT_FALSE(policy::ShouldUseInProcessMiniDumpFallbackAfterExternalHelperFailure(true));
}

// Anti-cheat integrity int3s and another hooking engine's patch races are
// handled by their raiser. Dumping a first-chance STATUS_BREAKPOINT immediately
// cost a dump stall and latched the process's one crash dump, so a real crash
// after a handled int3 got none; a "first breakpoint only" budget was spent on
// exactly those handled int3s. Every unowned breakpoint is recorded first; an
// escaped one still dumps through the unhandled filter (forceDump) or the
// termination that follows it.
TEST(CrashDumpPolicyTest, UnownedBreakpointsAreAlwaysRecordedFirst) {
    using Action = policy::FirstChanceAction;
    for (int occurrence = 0; occurrence < 3; ++occurrence) {
        EXPECT_EQ(policy::ClassifyFirstChanceException(EXCEPTION_BREAKPOINT, false, false), Action::kRecordFault)
            << "occurrence " << occurrence;
    }
    EXPECT_EQ(policy::ClassifyFirstChanceException(EXCEPTION_BREAKPOINT, true, false), Action::kDumpNow);
    EXPECT_EQ(policy::ClassifyFirstChanceException(EXCEPTION_BREAKPOINT, false, true), Action::kIgnore);

    // The recorded breakpoint becomes a dump when the process dies of it.
    EXPECT_TRUE(policy::IsCrashLikeProcessExitCode(policy::kBreakpointExceptionExitCode));
}

// UE5 `ensure` is continuable and can re-fire every frame; uncapped, each one
// wrote another assert_*.dmp at a full synchronous MiniDumpWriteDump stall.
TEST(CrashDumpPolicyTest, QuickAssertDumpsAreBudgetedPerProcess) {
    for (uint32_t written = 0; written < policy::kQuickAssertDumpPerProcessLimit; ++written) {
        EXPECT_TRUE(policy::ShouldWriteQuickAssertDump(written));
    }
    EXPECT_FALSE(policy::ShouldWriteQuickAssertDump(policy::kQuickAssertDumpPerProcessLimit));
    EXPECT_FALSE(policy::ShouldWriteQuickAssertDump(1000));
}

TEST(CrashDumpPolicyTest, ExtractPrintableMessageFindsTextAmongBinaryNoise) {
    // Mimics a thrown std::out_of_range object: vtable-ish pointers around an
    // inline (small-string) message.
    uint8_t object[128]{};
    const std::string message = "basic_string::substr: __pos (which is 49) > this->size()";
    std::copy(message.begin(), message.end(), object + 16);
    EXPECT_EQ(ce::crash_diagnostics::ExtractPrintableMessage(object, sizeof(object)), message);
}

TEST(CrashDumpPolicyTest, ExtractPrintableMessageReturnsLongestPrintableRun) {
    // Mimics a what()-style payload where the message run also carries a
    // printable prefix/suffix.
    const std::string message = "invalid argument";
    const std::string padded = std::string("XX") + message + std::string("YY");
    EXPECT_EQ(ce::crash_diagnostics::ExtractPrintableMessage(
                  reinterpret_cast<const uint8_t*>(padded.data()), padded.size()),
              "XXinvalid argumentYY");
}

TEST(CrashDumpPolicyTest, ExtractPrintableMessageRejectsShortRunsAndEmptyInput) {
    const uint8_t shortRun[] = {0xAA, 0xBB, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 0xCC};
    EXPECT_TRUE(ce::crash_diagnostics::ExtractPrintableMessage(shortRun, sizeof(shortRun)).empty());
    EXPECT_TRUE(ce::crash_diagnostics::ExtractPrintableMessage(nullptr, 0).empty());

    const uint8_t trailingSpace[] = {'m', 'e', 's', 's', 'a', 'g', 'e', ' ', ' ', 0x00};
    EXPECT_EQ(ce::crash_diagnostics::ExtractPrintableMessage(trailingSpace, sizeof(trailingSpace)), "message");
}

// Gothic II 20260916_014133: the game's own `Error-Message` box came up after
// four DDERR_UNSUPPORTEDMODE primary creations, its render thread parked in the
// dialog's message pump, and the watchdog wrote 30 MB of process memory for it
// once the heartbeat went stale. The freeze is real, so it is still recorded -
// but there is nothing in that process's memory worth thirty megabytes.
TEST(CrashDumpPolicyTest, TheStackOnlyDumpKeepsThreadsAndModulesWithoutProcessMemory) {
    const auto flags = policy::kStackOnlyDumpType;

    EXPECT_TRUE(HasDumpFlag(flags, MiniDumpWithThreadInfo));
    EXPECT_TRUE(HasDumpFlag(flags, MiniDumpWithUnloadedModules));
    EXPECT_TRUE(HasDumpFlag(flags, MiniDumpIgnoreInaccessibleMemory));

    EXPECT_FALSE(HasDumpFlag(flags, MiniDumpWithFullMemory));
    EXPECT_FALSE(HasDumpFlag(flags, MiniDumpWithFullMemoryInfo));
    EXPECT_FALSE(HasDumpFlag(flags, MiniDumpWithIndirectlyReferencedMemory));
    EXPECT_FALSE(HasDumpFlag(flags, MiniDumpWithDataSegs));
    EXPECT_FALSE(HasDumpFlag(flags, MiniDumpWithHandleData));
}

// Witcher 3 + NVIDIA Smooth Motion, session 20260919_154534. The game ended
// with 0xC0000409 and CE's session directory held no dump at all: __fastfail is
// dispatched with FirstChance = FALSE, so the VEH, the SEH chain and the
// unhandled-exception filter CE installs are all skipped. The only record is
// the one WER wrote, and CE has to say so rather than report "no session dump
// exists" and leave the reader to work out why.
TEST(CrashDumpPolicyTest, FailFastIsTheExitClassNoInProcessHandlerCanSee) {
    EXPECT_TRUE(policy::IsInProcessHandlerBypassingExitCode(policy::kFailFastExceptionExitCode));
    EXPECT_FALSE(policy::IsInProcessHandlerBypassingExitCode(EXCEPTION_ACCESS_VIOLATION));
    EXPECT_FALSE(policy::IsInProcessHandlerBypassingExitCode(EXCEPTION_STACK_OVERFLOW));
    EXPECT_FALSE(policy::IsInProcessHandlerBypassingExitCode(0));

    const std::string failFast = policy::DescribeProcessExitCodeClass(policy::kFailFastExceptionExitCode);
    EXPECT_NE(failFast.find("__fastfail"), std::string::npos);
    EXPECT_NE(failFast.find("VEH"), std::string::npos);

    EXPECT_STREQ(policy::DescribeProcessExitCodeClass(EXCEPTION_ACCESS_VIOLATION), "STATUS_ACCESS_VIOLATION");
    EXPECT_STREQ(policy::DescribeProcessExitCodeClass(0), "normal exit");
    EXPECT_STREQ(policy::DescribeProcessExitCodeClass(1), "normal exit");
}

TEST(CrashDumpPolicyTest, WerLocalDumpNamesMatchWhatWerFaultWritesAndWhatCeKeeps) {
    EXPECT_EQ(policy::BuildWerLocalDumpFileName("witcher3.exe", 9512), "witcher3.exe.9512.dmp");
    // WER names the file after the image alone, so a full path must be reduced
    // to its file name before the lookup or the dump is never found.
    EXPECT_EQ(policy::BuildWerLocalDumpFileName(R"(H:\SteamLibrary\common\bin\x64\witcher3.exe)", 9512),
              "witcher3.exe.9512.dmp");
    EXPECT_TRUE(policy::BuildWerLocalDumpFileName("", 9512).empty());
    EXPECT_TRUE(policy::BuildWerLocalDumpFileName(nullptr, 9512).empty());

    EXPECT_EQ(policy::BuildAdoptedWerCrashDumpFileName("witcher3.exe", 9512), "crash_wer_witcher3.exe_pid9512.dmp");
    // The pid token is what SessionDirectoryHasDumpForProcess matches on, so it
    // has to survive a missing image name.
    EXPECT_EQ(policy::BuildAdoptedWerCrashDumpFileName(nullptr, 9512), "crash_wer_process_pid9512.dmp");
}

TEST(CrashDumpPolicyTest, WerDumpIsClaimedOnlyForACrashCeDidNotRecordItself) {
    EXPECT_TRUE(policy::ShouldAdoptWerDumpForTrackedProcessExit(policy::kFailFastExceptionExitCode, false));
    EXPECT_TRUE(policy::ShouldAdoptWerDumpForTrackedProcessExit(EXCEPTION_ACCESS_VIOLATION, false));

    // CE's own dump is the authoritative record when it exists.
    EXPECT_FALSE(policy::ShouldAdoptWerDumpForTrackedProcessExit(policy::kFailFastExceptionExitCode, true));
    // A clean quit leaves the WER store alone.
    EXPECT_FALSE(policy::ShouldAdoptWerDumpForTrackedProcessExit(0, false));
    EXPECT_FALSE(policy::ShouldAdoptWerDumpForTrackedProcessExit(1, false));
    // A live FG runtime at exit is normal, not a crash (fg-exit-dump policy).
    EXPECT_FALSE(policy::ShouldAdoptWerDumpForTrackedProcessExit(policy::kProcessIsTerminatingExitCode, false));
}

TEST(CrashDumpPolicyTest, TheAdoptionWindowExpiresRatherThanWaitingOnWerFault) {
    EXPECT_FALSE(policy::HasWerDumpAdoptionWindowExpired(1000, 1000));
    EXPECT_FALSE(policy::HasWerDumpAdoptionWindowExpired(1000, 1000 + policy::kWerDumpAdoptionWindowMs));
    EXPECT_TRUE(policy::HasWerDumpAdoptionWindowExpired(1000, 1000 + policy::kWerDumpAdoptionWindowMs + 1));
    // A clock that went backwards must not be read as "expired long ago".
    EXPECT_FALSE(policy::HasWerDumpAdoptionWindowExpired(1000, 999));
}

// The HKCU LocalDumps values CE used to write were never read by WER and left
// one subkey per game behind, each naming a CE session directory. Purging them
// must recognise CE's own entries by that path and touch nothing else.
TEST(CrashDumpPolicyTest, OnlyCeWrittenLocalDumpsSubkeysAreRecognised) {
    const char* logsRoot = R"(C:\Users\TestUser\Programme\build\captureproject\installed\captureengine\logs)";

    EXPECT_TRUE(policy::IsCaptureEngineWrittenLocalDumpsSubkey(
        R"(C:\Users\TestUser\Programme\build\captureproject\installed\captureengine\logs\20260919_154534)", logsRoot));
    // The same path with the separators CE wrote in some builds.
    EXPECT_TRUE(policy::IsCaptureEngineWrittenLocalDumpsSubkey(
        R"(C:\Users\TestUser\PROGRAMME\build\captureproject\installed\CAPTUREENGINE\logs\20260916_011148)", logsRoot));

    EXPECT_FALSE(policy::IsCaptureEngineWrittenLocalDumpsSubkey(R"(%APPDATA%\SystemInformer\CrashDumps)", logsRoot));
    EXPECT_FALSE(policy::IsCaptureEngineWrittenLocalDumpsSubkey("", logsRoot));
    EXPECT_FALSE(policy::IsCaptureEngineWrittenLocalDumpsSubkey(nullptr, logsRoot));
    EXPECT_FALSE(policy::IsCaptureEngineWrittenLocalDumpsSubkey(R"(C:\anything)", ""));
}

// WER is the only mechanism that records a __fastfail termination, so this
// process must stay visible to it - SEM_NOGPFAULTERRORBOX makes the default
// unhandled filter terminate without invoking WER at all. Staying visible is
// only acceptable because the fault-report UI is suppressed instead, and that
// suppression is WER_FAULT_REPORTING_NO_UI (0x20).
//
// This regression exists because the call passed 0x3 while its comment claimed
// NO_UI. 0x3 is NOHEAP | QUEUE: QUEUE keeps the report out of the interactive
// submit flow but does not suppress the dialog, so from the moment
// SEM_NOGPFAULTERRORBOX was dropped a crashing game could show a fault dialog CE
// used to suppress. This hook DLL is loaded into the game, so that is the
// player's screen.
TEST(CrashDumpPolicyTest, WerStaysVisibleWithItsFaultReportUiSuppressed) {
    namespace fs = std::filesystem;
    const std::string handler =
        ce::test_source::ReadLogicalSource(fs::current_path() / "common" / "crash_handler.cpp");
    ASSERT_FALSE(handler.empty());

    const size_t setErrorMode = handler.find("SetErrorMode(");
    ASSERT_NE(setErrorMode, std::string::npos);
    const std::string errorModeCall = handler.substr(setErrorMode, 120);
    EXPECT_EQ(errorModeCall.find("SEM_NOGPFAULTERRORBOX"), std::string::npos)
        << "it makes UnhandledExceptionFilter terminate without invoking WER, which loses the __fastfail dump";

    const size_t werSetFlags = handler.find("pfnWerSetFlags(");
    ASSERT_NE(werSetFlags, std::string::npos);
    const std::string flagsCall = handler.substr(werSetFlags, 200);
    EXPECT_NE(flagsCall.find("kWerFaultReportingNoUi"), std::string::npos)
        << "dropping SEM_NOGPFAULTERRORBOX is only safe while the WER fault-report UI is suppressed here";
    EXPECT_EQ(flagsCall.find("0x00000003"), std::string::npos)
        << "0x3 is NOHEAP | QUEUE, which does not suppress the dialog";

    // Both are process-wide settings of the HOST process when this is the
    // injected hook: CE adds its bits and keeps what the game already chose.
    EXPECT_NE(errorModeCall.find("GetErrorMode() |"), std::string::npos)
        << "SetErrorMode replaces the whole mode; the game's own flags must survive";
    EXPECT_NE(flagsCall.find("existingWerFlags |"), std::string::npos)
        << "WerSetFlags replaces the whole flag set; the game's own flags must survive";
    EXPECT_NE(handler.find("pfnWerGetFlags(GetCurrentProcess(), &existingWerFlags)"), std::string::npos);

    // The named constants must match werapi.h, since CE mirrors rather than
    // includes them.
    EXPECT_NE(handler.find("kWerFaultReportingFlagNoHeap = 0x00000001"), std::string::npos);
    EXPECT_NE(handler.find("kWerFaultReportingFlagQueue = 0x00000002"), std::string::npos);
    EXPECT_NE(handler.find("kWerFaultReportingNoUi = 0x00000020"), std::string::npos);
}
