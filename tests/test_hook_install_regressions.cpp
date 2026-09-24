#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "../hook/common/child_inject_policy.h"
#include "../hook/common/hook_jump_policy.h"
#include "source_fragment_reader.h"

// Regression coverage for hook-side child injection and hook-install
// hardening: child injection must key on the program a CreateProcess call
// actually launches and must never free the remote DLL path under a running
// LoadLibraryW; entry-patch failures must not log while peer threads are
// suspended; rel32 emission must be computed and range-decided explicitly.

namespace {

std::string ReadSource(const std::string& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

std::string FunctionBody(const std::string& source, const std::string& signature, const std::string& nextSignature) {
    const size_t begin = source.find(signature);
    if (begin == std::string::npos)
        return {};
    const size_t end = source.find(nextSignature, begin + signature.size());
    return source.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

// Text of the innermost { ... } block enclosing `position`, which for a stack
// object is exactly the region its destructor runs at the end of.
std::string EnclosingBlock(const std::string& source, size_t position) {
    int depth = 0;
    size_t begin = std::string::npos;
    for (size_t i = position; i-- > 0;) {
        if (source[i] == '}') {
            ++depth;
        } else if (source[i] == '{') {
            if (depth == 0) {
                begin = i;
                break;
            }
            --depth;
        }
    }
    if (begin == std::string::npos)
        return {};
    int forward = 0;
    for (size_t i = begin; i < source.size(); ++i) {
        if (source[i] == '{') {
            ++forward;
        } else if (source[i] == '}') {
            --forward;
            if (forward == 0)
                return source.substr(begin, i - begin + 1);
        }
    }
    return {};
}

const void* Address(uint64_t value) {
    return reinterpret_cast<const void*>(static_cast<uintptr_t>(value));
}

}  // namespace

// --- CreateProcess program resolution (child injection whitelist) ------------

TEST(HookInstallRegressionTest, QuotedPathWithArgumentsResolvesToTheProgramFileName) {
    EXPECT_EQ(ce::child_inject_policy::ProgramPath(nullptr, "\"C:\\Games\\foo\\game.exe\" -dx12"),
              "C:\\Games\\foo\\game.exe");
    EXPECT_EQ(ce::child_inject_policy::ProgramFileName(nullptr, "\"C:\\Games\\foo\\game.exe\" -dx12"), "game.exe");
    EXPECT_EQ(ce::child_inject_policy::ProgramFileName(nullptr, "\"C:\\Games\\foo\\game.exe\""), "game.exe");
}

TEST(HookInstallRegressionTest, UnquotedPathWithArgumentsResolvesToTheProgramFileName) {
    EXPECT_EQ(ce::child_inject_policy::ProgramFileName(nullptr, "C:\\Games\\foo\\game.exe -dx12"), "game.exe");
    EXPECT_EQ(ce::child_inject_policy::ProgramFileName(nullptr, "  C:\\Games\\foo\\game.exe -dx12 -windowed"),
              "game.exe");
}

TEST(HookInstallRegressionTest, ApplicationNameIsAPathAndUsedVerbatim) {
    // lpApplicationName may contain unquoted spaces; parsing it like a command
    // line would resolve "C:\Program" and miss the whitelist entirely.
    EXPECT_EQ(ce::child_inject_policy::ProgramPath("C:\\Program Files\\foo\\game.exe", nullptr),
              "C:\\Program Files\\foo\\game.exe");
    EXPECT_EQ(ce::child_inject_policy::ProgramFileName("C:\\Program Files\\foo\\game.exe", nullptr), "game.exe");
    EXPECT_EQ(ce::child_inject_policy::ProgramFileName("C:\\Program Files\\foo\\game.exe", "ignored.exe -x"),
              "game.exe");
}

TEST(HookInstallRegressionTest, PlainExeNameResolvesToItself) {
    EXPECT_EQ(ce::child_inject_policy::ProgramFileName(nullptr, "game.exe"), "game.exe");
    EXPECT_EQ(ce::child_inject_policy::ProgramFileName("game.exe", nullptr), "game.exe");
    EXPECT_EQ(ce::child_inject_policy::ProgramFileName(nullptr, "launcher.exe -arg \"quoted value\""),
              "launcher.exe");
}

TEST(HookInstallRegressionTest, EmptyLaunchInputResolvesToNoProgram) {
    EXPECT_TRUE(ce::child_inject_policy::ProgramFileName(nullptr, nullptr).empty());
    EXPECT_TRUE(ce::child_inject_policy::ProgramFileName(nullptr, "   ").empty());
    EXPECT_TRUE(ce::child_inject_policy::ProgramFileName(nullptr, "\"\"").empty());
}

TEST(HookInstallRegressionTest, FileNameOfPathHandlesBothSeparators) {
    EXPECT_EQ(ce::child_inject_policy::FileNameOfPath("C:\\Games\\foo\\game.exe"), "game.exe");
    EXPECT_EQ(ce::child_inject_policy::FileNameOfPath("C:/Games/foo/game.exe"), "game.exe");
    EXPECT_EQ(ce::child_inject_policy::FileNameOfPath("game.exe"), "game.exe");
}

// --- Hook module directory derivation (UTF-16) -------------------------------

TEST(HookInstallRegressionTest, StripLastPathSegmentWTrimsOnlyTheFinalSegment) {
    wchar_t path[] = L"C:\\a\\b\\c.dll";
    ASSERT_TRUE(ce::child_inject_policy::StripLastPathSegmentW(path));
    EXPECT_EQ(std::wstring_view(path), L"C:\\a\\b");

    wchar_t trailing[] = L"C:\\a\\b\\";
    ASSERT_TRUE(ce::child_inject_policy::StripLastPathSegmentW(trailing));
    EXPECT_EQ(std::wstring_view(trailing), L"C:\\a");

    wchar_t noSeparator[] = L"c.dll";
    EXPECT_FALSE(ce::child_inject_policy::StripLastPathSegmentW(noSeparator));
    EXPECT_EQ(std::wstring_view(noSeparator), L"c.dll");
}

TEST(HookInstallRegressionTest, HookModuleDirectoryLosesNoCharactersOfTheImagePath) {
    const HMODULE self = GetModuleHandleW(nullptr);
    wchar_t path[MAX_PATH] = {};
    wchar_t directory[MAX_PATH] = {};
    ASSERT_TRUE(ce::child_inject_policy::GetHookModulePathW(self, path, MAX_PATH));
    ASSERT_TRUE(ce::child_inject_policy::GetHookModuleDirectoryW(self, directory, MAX_PATH));

    const std::wstring_view pathView(path);
    const std::wstring_view directoryView(directory);
    ASSERT_FALSE(pathView.empty());
    ASSERT_FALSE(directoryView.empty());
    EXPECT_EQ(pathView.substr(0, directoryView.size()), directoryView);
    EXPECT_EQ(pathView[directoryView.size()], L'\\');
    EXPECT_NE(GetFileAttributesW(directory) & FILE_ATTRIBUTE_DIRECTORY, 0u);
}

// --- rel32 displacement policy ----------------------------------------------

TEST(HookInstallRegressionTest, Rel32DisplacementInRangeIsAcceptedExactly) {
    int32_t rel = 0;
    ASSERT_TRUE(ce::hook_jump_policy::TryRel32Displacement(Address(0x1000), Address(0x2000),
                                                           ce::hook_jump_policy::Rel32Semantics::kSignExtended, &rel));
    EXPECT_EQ(rel, 0x0FFB);  // target - (dest + 5)

    ASSERT_TRUE(ce::hook_jump_policy::TryRel32Displacement(Address(0x2000), Address(0x1000),
                                                           ce::hook_jump_policy::Rel32Semantics::kSignExtended, &rel));
    EXPECT_EQ(rel, -0x1005);

    // The exact INT32 boundaries are still in range; one byte past is not.
    ASSERT_TRUE(ce::hook_jump_policy::TryRel32Displacement(Address(0x1000), Address(0x80001004),
                                                           ce::hook_jump_policy::Rel32Semantics::kSignExtended, &rel));
    EXPECT_EQ(rel, INT32_MAX);
    ASSERT_TRUE(ce::hook_jump_policy::TryRel32Displacement(Address(0x7FFFFFFBull), Address(0),
                                                           ce::hook_jump_policy::Rel32Semantics::kSignExtended, &rel));
    EXPECT_EQ(rel, INT32_MIN);
}

TEST(HookInstallRegressionTest, Rel32DisplacementOutOfRangeFailsTheInstall) {
    int32_t rel = 0;
    EXPECT_FALSE(ce::hook_jump_policy::TryRel32Displacement(Address(0x1000), Address(0x80001005),
                                                            ce::hook_jump_policy::Rel32Semantics::kSignExtended, &rel));
    EXPECT_FALSE(ce::hook_jump_policy::TryRel32Displacement(Address(0x7FFFFFFCull), Address(0),
                                                            ce::hook_jump_policy::Rel32Semantics::kSignExtended, &rel));
    // Game code and a hook DLL more than 2GB apart: the case a sign-extended
    // rel32 cannot express and the install must refuse rather than truncate.
    EXPECT_FALSE(ce::hook_jump_policy::TryRel32Displacement(Address(0x10000000), Address(0xA0000000),
                                                            ce::hook_jump_policy::Rel32Semantics::kSignExtended, &rel));
}

TEST(HookInstallRegressionTest, X86WrapSemanticsLandFarDisplacementsWithoutTruncation) {
    int32_t rel = 0;
    ASSERT_TRUE(ce::hook_jump_policy::TryRel32Displacement(Address(0x10000000), Address(0xA0000000),
                                                           ce::hook_jump_policy::Rel32Semantics::kWrapAddress32,
                                                           &rel));
    // The emitted displacement must round-trip through x86 E9 landing rules
    // (32-bit wrap), not merely be a truncation of the true distance.
    const uint32_t landed = static_cast<uint32_t>(0x10000005u) + static_cast<uint32_t>(rel);
    EXPECT_EQ(landed, 0xA0000000u);

    // Wrapping cannot reach outside the 32-bit address space at all.
    EXPECT_FALSE(ce::hook_jump_policy::TryRel32Displacement(Address(0x1000), Address(0x100000000ull),
                                                            ce::hook_jump_policy::Rel32Semantics::kWrapAddress32,
                                                            &rel));
}

// --- Source policy: child injection worker (remote LoadLibraryW) -------------

TEST(HookInstallRegressionTest, ChildInjectWorkerReportsLoadLibraryCompletionHonestly) {
    const std::string source = ReadSource("hook/main_injection.cpp");
    ASSERT_FALSE(source.empty());
    const std::string body =
        FunctionBody(source, "static DWORD WINAPI ChildInjectWorker(LPVOID param)", "void InjectIntoChild(");
    ASSERT_FALSE(body.empty());

    // UTF-16 end to end: wide path sizing and the wide loader entry.
    EXPECT_NE(body.find("wcslen(p->dllPath)"), std::string::npos);
    EXPECT_NE(body.find("\"LoadLibraryW\""), std::string::npos);
    EXPECT_EQ(body.find("\"LoadLibraryA\""), std::string::npos);

    const size_t wait = body.find("WaitForSingleObject(hRemote, 5000)");
    const size_t timeoutCheck = body.find("waitResult != WAIT_OBJECT_0");
    const size_t exitCode = body.find("GetExitCodeThread");
    const size_t successLog = body.find("\"[ChildInject] Injected into child process.\"");
    ASSERT_NE(wait, std::string::npos);
    ASSERT_NE(timeoutCheck, std::string::npos);
    ASSERT_NE(exitCode, std::string::npos);
    ASSERT_NE(successLog, std::string::npos);
    EXPECT_LT(wait, timeoutCheck);
    EXPECT_LT(timeoutCheck, exitCode);
    EXPECT_LT(exitCode, successLog);

    // A pending remote LoadLibraryW keeps its path buffer: no free between the
    // timeout check and the completed path.
    const size_t pendingEnd = body.find("DWORD remoteModule", timeoutCheck);
    ASSERT_NE(pendingEnd, std::string::npos);
    const std::string pendingRegion = body.substr(timeoutCheck, pendingEnd - timeoutCheck);
    EXPECT_EQ(pendingRegion.find("VirtualFreeEx"), std::string::npos);
    EXPECT_NE(pendingRegion.find("still pending"), std::string::npos);
    EXPECT_NE(body.find("VirtualFreeEx", pendingEnd), std::string::npos)
        << "the completed path must still release the remote buffer";
}

// --- Source policy: program resolution in the CreateProcess hooks ------------

TEST(HookInstallRegressionTest, CreateProcessHooksResolveTheProgramBeforeTheWhitelist) {
    const std::string source = ReadSource("hook/main_injection.cpp");
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("bool ShouldInjectChild(const char *exePath)"), std::string::npos);
    const std::string decision =
        FunctionBody(source, "bool ShouldInjectChild(const char *exePath)", "// Hooked CreateProcessA");
    ASSERT_FALSE(decision.empty());
    EXPECT_NE(decision.find("ce::child_inject_policy::FileNameOfPath"), std::string::npos);
    EXPECT_EQ(decision.find("find_last_of"), std::string::npos)
        << "the raw-string last-slash substring kept command-line arguments in the program name";

    EXPECT_NE(source.find("ce::child_inject_policy::ProgramPath(lpApp, lpCmd)"), std::string::npos);
    EXPECT_NE(source.find("ce::child_inject_policy::ProgramPath(applicationPath, commandLine)"), std::string::npos);
}

// --- Source policy: UTF-16 module path derivation ----------------------------

TEST(HookInstallRegressionTest, HookModulePathsAreDerivedAsUtf16) {
    const std::string hookThread = ReadSource("hook/main_hookthread.cpp");
    const std::string dllMain = ReadSource("hook/main_dllmain.cpp");
    const std::string pristineUnit = ReadSource("hook/wrappers/inline_hook_pristine_image.cpp");
    const std::string injection = ReadSource("hook/main_injection.cpp");
    ASSERT_FALSE(hookThread.empty());
    ASSERT_FALSE(dllMain.empty());
    ASSERT_FALSE(pristineUnit.empty());
    ASSERT_FALSE(injection.empty());

    EXPECT_EQ(hookThread.find("GetModuleFileNameA"), std::string::npos);
    EXPECT_NE(hookThread.find("GetHookModuleDirectoryW"), std::string::npos);
    EXPECT_NE(hookThread.find("LoadLibraryW(wrapperDll.c_str())"), std::string::npos);

    const std::string crashSetup = FunctionBody(dllMain, "std::string crashDir;", "// DllMain runs under the loader lock");
    ASSERT_FALSE(crashSetup.empty());
    EXPECT_NE(crashSetup.find("GetHookModuleDirectoryW"), std::string::npos);
    EXPECT_NE(crashSetup.find("CreateDirectoryW"), std::string::npos);
    // Every crash-directory consumer (CreateFileA in the dump writer, the
    // external helper's command line, DiscoveryInfo.logsPath) reads code-page
    // text; a UTF-8 directory sent the dumps of a non-ASCII install nowhere.
    EXPECT_NE(crashSetup.find("ce::path::AnsiCompatiblePath(logsDir.wstring()"), std::string::npos);
    EXPECT_EQ(crashSetup.find("NarrowFromWideUtf8"), std::string::npos);
    const std::string crashHandler = ReadSource("common/crash_handler.cpp");
    ASSERT_FALSE(crashHandler.empty());
    EXPECT_EQ(crashHandler.find("MultiByteToWideChar(CP_UTF8, 0, CrashDumpDirectoryStorage()"), std::string::npos);
    EXPECT_NE(crashHandler.find("MultiByteToWideChar(CP_ACP, 0, CrashDumpDirectoryStorage()"), std::string::npos);

    const std::string pristineRead =
        FunctionBody(pristineUnit, "bool ReadOrigBytesFromDisk(", "}  // namespace InlineHook");
    ASSERT_FALSE(pristineRead.empty());
    EXPECT_NE(pristineRead.find("GetHookModulePathW"), std::string::npos);
    EXPECT_NE(pristineRead.find("CreateFileW"), std::string::npos);
    EXPECT_EQ(pristineRead.find("GetModuleFileNameA"), std::string::npos);
    EXPECT_EQ(pristineRead.find("CreateFileA"), std::string::npos);

    EXPECT_EQ(injection.find("GetModuleFileNameA"), std::string::npos);
}

// --- Source policy: no logging while peer threads are suspended --------------

TEST(HookInstallRegressionTest, ThreadQuiescenceScopesNeverLog) {
    // The logger takes a lock a suspended peer may hold: a log line inside the
    // transaction can deadlock the game frozen with every thread suspended.
    for (const char* relativePath : {"hook/wrappers/inline_hook_entry_patch.cpp", "hook/wrappers/inline_hook_batch.cpp"}) {
        const std::string source = ReadSource(relativePath);
        ASSERT_FALSE(source.empty()) << relativePath;
        size_t search = 0;
        int scopes = 0;
        while ((search = source.find("ce::hook_patch::ThreadQuiescence", search)) != std::string::npos) {
            const std::string scope = EnclosingBlock(source, search);
            ASSERT_FALSE(scope.empty()) << relativePath << " offset " << search;
            EXPECT_EQ(scope.find("HookLog"), std::string::npos)
                << relativePath << " logs inside a ThreadQuiescence scope at offset " << search;
            ++scopes;
            ++search;
        }
        EXPECT_GE(scopes, 1) << relativePath;
    }

    const std::string entryPatch = ReadSource("hook/wrappers/inline_hook_entry_patch.cpp");
    ASSERT_FALSE(entryPatch.empty());
    EXPECT_NE(entryPatch.find("ReportEntryPatchFailure"), std::string::npos)
        << "failures inside the transaction must be reported after it ends";
}

// --- Source policy: rel32 emission -------------------------------------------

TEST(HookInstallRegressionTest, JumpEmissionUsesTheRangeCheckedPolicy) {
    const std::string trampoline = ReadSource("hook/wrappers/inline_hook_trampoline.cpp");
    const std::string entryPatch = ReadSource("hook/wrappers/inline_hook_entry_patch.cpp");
    ASSERT_FALSE(trampoline.empty());
    ASSERT_FALSE(entryPatch.empty());

    const std::string writeJump =
        FunctionBody(trampoline, "bool WriteJump(uint8_t* dest, void* target)", "static bool IsShortConditionalJumpOpcode");
    ASSERT_FALSE(writeJump.empty());
    EXPECT_NE(writeJump.find("TryRel32Displacement"), std::string::npos);
    EXPECT_EQ(writeJump.find("Verification:"), std::string::npos);

    const std::string entryJump =
        FunctionBody(entryPatch, "static bool WriteJumpWithoutLogging(", "#ifdef _WIN64\nstatic bool WriteNearJump");
    ASSERT_FALSE(entryJump.empty());
    EXPECT_NE(entryJump.find("TryRel32Displacement"), std::string::npos);
}

TEST(HookInstallRegressionTest, TrampolinePoolsSearchNearTheTargetOnEveryArchitecture) {
    const std::string trampoline = ReadSource("hook/wrappers/inline_hook_trampoline.cpp");
    ASSERT_FALSE(trampoline.empty());

    const std::string poolSearch = FunctionBody(trampoline, "static uint8_t* AllocateTrampolinePool(void* nearAddr)",
                                               "bool IsInTrampolinePool(");
    ASSERT_FALSE(poolSearch.empty());
    EXPECT_NE(poolSearch.find("ClosestPoolAddressInRegion"), std::string::npos);
    EXPECT_EQ(poolSearch.find("#ifdef _WIN64"), std::string::npos)
        << "x86 trampolines allocated anywhere, out of reach of rewritten short branches";
}

// --- Source policy: Steam init diagnostics -----------------------------------

TEST(HookInstallRegressionTest, SteamInitDoesNotPeekTheStaleFixedCallbackRva) {
    const std::string routing = ReadSource("hook/common/dxgi_shared_steam_routing.cpp");
    ASSERT_FALSE(routing.empty());
    EXPECT_EQ(routing.find("0x1621d8"), std::string::npos)
        << "the fixed RVA moved between Steam builds; only the VEH's dynamically resolved slot is valid";
}
