#include <gtest/gtest.h>

#include "../common/crash_dump_policy.h"

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
    EXPECT_EQ(
        policy::BuildMirroredExternalDumpFileName(
            R"(C:\Users\TestUser\AppData\Local\Rockstar Games\GTAV Enhanced\CrashLogs\51e9b489-70bb-4998-a4ec-254bdd858cbd.dmp)"),
        "external_51e9b489-70bb-4998-a4ec-254bdd858cbd.dmp");
    EXPECT_EQ(policy::BuildMirroredExternalDumpFileName("crashcontext"), "external_crashcontext.dmp");
    EXPECT_EQ(policy::BuildMirroredExternalDumpFileName(nullptr), "external_dump.dmp");
}

TEST(CrashDumpPolicyTest, SupplementalExternalCrashDumpBuildsStableDestinationFileNames) {
    EXPECT_EQ(
        policy::BuildSupplementalCrashDumpFileNameFromExternalSource(
            R"(C:\Users\TestUser\AppData\Local\Rockstar Games\GTAV Enhanced\CrashLogs\103327d5-227b-4bb7-b529-7c8a38cccdbf.dmp)"),
        "crash_external_103327d5-227b-4bb7-b529-7c8a38cccdbf.dmp");
    EXPECT_EQ(policy::BuildSupplementalCrashDumpFileNameFromExternalSource("sl-sha-11cf43f"),
              "crash_external_sl-sha-11cf43f.dmp");
    EXPECT_EQ(policy::BuildSupplementalCrashDumpFileNameFromExternalSource(nullptr),
              "crash_external_dump.dmp");
}
