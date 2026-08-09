#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>

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

TEST(CrashDumpPolicyTest, PreTerminationDumpCapturesActiveFrameGenerationRuntimeExits) {
    EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(true, 0, false, true));
    EXPECT_TRUE(policy::ShouldCapturePreTerminationDump(true, 1, false, true));

    EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(false, 0, false, true));
    EXPECT_FALSE(policy::ShouldCapturePreTerminationDump(true, 0, true, true));
    EXPECT_FALSE(
        policy::ShouldCapturePreTerminationDump(true, policy::kExternalDumpStormTerminationExitCode, false, true));
}

TEST(CrashDumpPolicyTest, BreakpointExceptionsDumpWhenNoDebuggerOwnsThem) {
    EXPECT_FALSE(policy::ShouldSkipBreakpointExceptionDump(false, false));
    EXPECT_TRUE(policy::ShouldSkipBreakpointExceptionDump(false, true));
    EXPECT_FALSE(policy::ShouldSkipBreakpointExceptionDump(true, true));
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
