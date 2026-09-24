#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../common/crash_dump_policy.h"
#include "../common/crash_first_chance.h"
#include "../common/crash_handler.h"
#include "../common/log_privacy.h"
#include "source_fragment_reader.h"

namespace policy = ce::crash_dump_policy;
namespace privacy = ce::privacy;

// The vectored filter is the unit that turns an exception into crash.log lines
// and dump-policy decisions. crash_dump_writer.cpp publishes it for tests the
// same way crash_handler.cpp publishes DispatchCrashExecutionFaultHandlerForTesting;
// the declaration lives here because crash_handler.h is not the test seam.
LONG WINAPI CrashHandlerExceptionFilterForTesting(EXCEPTION_POINTERS* pExceptionPointers);

namespace {

std::filesystem::path MakeTruthDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("ce_crash_output_truth_" + std::to_string(GetCurrentProcessId()) + "_" + name);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

std::string ReadTruthFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

std::string ReadCrashLog(const std::filesystem::path& dir) {
    return ReadTruthFile(dir / "crash.log");
}

size_t CountOccurrences(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

int CountFilesWithPrefix(const std::filesystem::path& dir, const std::string& prefix) {
    int count = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        if (entry.is_regular_file(ec) && entry.path().filename().string().rfind(prefix, 0) == 0) {
            ++count;
        }
    }
    return count;
}

int g_ExternalCaptureCalls = 0;
std::vector<std::string> g_ExternalCaptureHints;

bool RecordExternalCapture(const char* dumpFileNameHint, bool, const ExternalDumpException*) {
    ++g_ExternalCaptureCalls;
    g_ExternalCaptureHints.emplace_back(dumpFileNameHint ? dumpFileNameHint : "");
    return true;
}

bool ForeignOverlayLoadedStub() {
    return true;
}

EXCEPTION_POINTERS MakeSyntheticException(EXCEPTION_RECORD& record, CONTEXT& context, DWORD code) {
    record = {};
    context = {};
    record.ExceptionCode = code;
    record.ExceptionAddress = reinterpret_cast<void*>(static_cast<uintptr_t>(0x00007FF610001000ULL));
#ifdef _WIN64
    context.Rip = reinterpret_cast<DWORD64>(record.ExceptionAddress);
#else
    context.Eip = static_cast<DWORD>(reinterpret_cast<uintptr_t>(record.ExceptionAddress));
#endif
    return EXCEPTION_POINTERS{&record, &context};
}

// Restores the process-wide crash-dump hooks and the temp directory whatever a
// test body does, so the behavioral tests here can never leak state into the
// rest of the suite (test_crash_handler.cpp asserts the unregistered answer).
class CrashOutputTruthTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = MakeTruthDir("behavioral");
        SetCrashDumpDirectory(dir_.string(), /*archiveInstalledSymbols=*/false);
        ActivateCrashTrace();
    }

    void TearDown() override {
        RegisterCrashDumpEnvironmentHooks(CrashDumpEnvironmentHooks{});
        SetCrashDumpDirectory(".\\logs", /*archiveInstalledSymbols=*/false);
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::filesystem::path dir_;
};

}  // namespace

// crash.log is shared in support workflows like every other log, so it follows
// the same privacy contract (log_privacy.h): no Windows account component may
// reach the file. Regression: crash.log was the one funnel that wrote messages
// raw, so every crash session leaked C:\Users\<user>\... paths.
TEST_F(CrashOutputTruthTest, TraceCrashMasksTheWindowsAccountComponent) {
    const std::string message =
        "Assert dump: C:\\Users\\SupportSecret\\AppData\\Local\\captureproject\\logs\\assert_20260924_1.dmp";
    TraceCrash(message.c_str());

    const std::string log = ReadCrashLog(dir_);
    ASSERT_FALSE(log.empty());
    EXPECT_EQ(log.find("SupportSecret"), std::string::npos) << log;
    EXPECT_NE(log.find(privacy::RedactUserAccountComponents(message)), std::string::npos) << log;
    // Everything but the account token keeps its diagnostic value.
    EXPECT_NE(log.find("assert_20260924_1.dmp"), std::string::npos) << log;
}

// The UE5 ensure() "quick assert" dump ran in-process, synchronously, uncapped
// and without the foreign-overlay guard the rich path has - with Steam/Social
// overlays loaded that is the measured ~61.6 s per-MiniDumpWriteDump stall, and
// UE5 `ensure` can fire repeatedly. The dump must go through the external
// helper under a foreign overlay, and stop entirely after the per-process cap.
TEST_F(CrashOutputTruthTest, QuickAssertDumpsRespectTheForeignOverlayGuardAndPerProcessCap) {
    g_ExternalCaptureCalls = 0;
    g_ExternalCaptureHints.clear();
    CrashDumpEnvironmentHooks hooks;
    hooks.captureWithExternalHelper = &RecordExternalCapture;
    hooks.foreignOverlayLoaded = &ForeignOverlayLoadedStub;
    RegisterCrashDumpEnvironmentHooks(hooks);

    constexpr uint32_t kEvents = policy::kQuickAssertDumpPerProcessLimit + 2;
    for (uint32_t i = 0; i < kEvents; ++i) {
        EXCEPTION_RECORD record = {};
        CONTEXT context = {};
        EXCEPTION_POINTERS pointers = MakeSyntheticException(record, context, policy::kUe5EnsureExceptionCode);
        EXPECT_EQ(CrashHandlerExceptionFilterForTesting(&pointers), EXCEPTION_CONTINUE_SEARCH);
    }

    // Every allowed assert dump left the process through the helper; the events
    // past the cap logged only. Nothing may run MiniDumpWriteDump in-process
    // while a foreign overlay is loaded.
    EXPECT_EQ(g_ExternalCaptureCalls, static_cast<int>(policy::kQuickAssertDumpPerProcessLimit));
    for (const std::string& hint : g_ExternalCaptureHints) {
        EXPECT_EQ(hint.rfind("assert_", 0), 0u) << hint;
    }
    EXPECT_EQ(CountFilesWithPrefix(dir_, "assert_"), 0);

    const std::string log = ReadCrashLog(dir_);
    EXPECT_NE(log.find("captured the quick assert dump"), std::string::npos) << log;
    EXPECT_NE(log.find("Quick assert dump suppressed"), std::string::npos) << log;
    EXPECT_EQ(log.find("Quick assert dump written"), std::string::npos) << log;
}

// Anti-cheat integrity int3s and another hooking engine's patch races are
// handled by their raiser. A first-chance STATUS_BREAKPOINT therefore never
// dumps immediately: that cost a dump stall and latched the process's one crash
// dump, so the real crash after a handled int3 got none. An escaped breakpoint
// dumps through the unhandled filter's forceDump re-entry instead.
TEST_F(CrashOutputTruthTest, UnownedBreakpointsNeverDumpAtFirstChance) {
    if (IsDebuggerPresent()) {
        GTEST_SKIP() << "a debugger owns every breakpoint in this run";
    }

    constexpr int kBreakpoints = 3;
    for (int i = 0; i < kBreakpoints; ++i) {
        EXCEPTION_RECORD record = {};
        CONTEXT context = {};
        EXCEPTION_POINTERS pointers = MakeSyntheticException(record, context, EXCEPTION_BREAKPOINT);
        // The recorder only hands a record back to the thread whose stack it
        // faulted on, so the synthetic fault sits on this thread's stack.
#ifdef _WIN64
        context.Rsp = reinterpret_cast<DWORD64>(&record);
#else
        context.Esp = static_cast<DWORD>(reinterpret_cast<uintptr_t>(&record));
#endif
        EXPECT_EQ(CrashHandlerExceptionFilterForTesting(&pointers), EXCEPTION_CONTINUE_SEARCH);
    }

    // Recorded, not dumped: the thread's record is what a later termination
    // would dump with. Forget it so no other test inherits it.
    EXCEPTION_RECORD recorded = {};
    CONTEXT recordedContext = {};
    EXPECT_TRUE(ce::crash_first_chance::CopyFaultForCurrentThread(&recorded, &recordedContext));
    EXPECT_EQ(recorded.ExceptionCode, static_cast<DWORD>(EXCEPTION_BREAKPOINT));
    ce::crash_first_chance::ClearFaultForCurrentThread();

    const std::string log = ReadCrashLog(dir_);
    EXPECT_EQ(CountOccurrences(log, "CRASH DETECTED - Handling exception"), 0u) << log;
}

// The redaction must run inside TraceCrash itself: every call site funnels
// through it, including the ones that pass a full dump path.
TEST(CrashOutputTruthSourceTest, TraceCrashRedactsAccountNamesBeforeWriting) {
    namespace fs = std::filesystem;
    const std::string handler =
        ce::test_source::ReadLogicalSource(fs::current_path() / "common" / "crash_handler.cpp");
    ASSERT_FALSE(handler.empty());

    const size_t trace = handler.find("void TraceCrash(const char* msg) {");
    ASSERT_NE(trace, std::string::npos);
    const size_t traceEnd = handler.find("\n}\n", trace);
    ASSERT_NE(traceEnd, std::string::npos);
    const std::string body = handler.substr(trace, traceEnd - trace);

    EXPECT_NE(body.find("ce::privacy::RedactUserAccountComponents(redacted)"), std::string::npos)
        << "crash.log must pass every message through account redaction before writing it";
    EXPECT_NE(body.find("GetCurrentThreadId(), redacted)"), std::string::npos);
    EXPECT_EQ(body.find("GetCurrentThreadId(), msg)"), std::string::npos)
        << "the raw message must never reach crash.log";
}

// Source side of the quick-assert policy: the cap and the foreign-overlay guard
// both have to gate the in-process MiniDumpWriteDump, in that order.
TEST(CrashOutputTruthSourceTest, QuickAssertDumpBranchIsCappedAndGuardedBeforeItDumpsInProcess) {
    namespace fs = std::filesystem;
    const std::string writer =
        ce::test_source::ReadLogicalSource(fs::current_path() / "common" / "crash_dump_writer.cpp");
    ASSERT_FALSE(writer.empty());

    const size_t branch = writer.find("if (action == ce::crash_dump_policy::FirstChanceAction::kQuickAssertDump) {");
    ASSERT_NE(branch, std::string::npos);
    const size_t branchEnd = writer.find("ANY exception that reaches here", branch);
    ASSERT_NE(branchEnd, std::string::npos);
    const std::string body = writer.substr(branch, branchEnd - branch);

    const size_t cap = body.find("ShouldWriteQuickAssertDump(");
    const size_t helper = body.find("ShouldPreferExternalCrashDumpHelper(");
    const size_t fallback = body.find("ShouldUseInProcessMiniDumpFallbackAfterExternalHelperFailure(");
    const size_t inProcess = body.find("g_pMiniDumpWriteDump(GetCurrentProcess()");
    ASSERT_NE(cap, std::string::npos) << "an uncapped assert storm re-freezes the process once per ensure";
    ASSERT_NE(helper, std::string::npos) << "the quick assert path must obey the foreign-overlay helper preference";
    ASSERT_NE(fallback, std::string::npos) << "the in-process walk must be refused under a foreign overlay";
    ASSERT_NE(inProcess, std::string::npos);
    EXPECT_LT(cap, inProcess);
    EXPECT_LT(helper, inProcess);
    EXPECT_LT(fallback, inProcess);

    EXPECT_NE(body.find("ce::privacy::CollapsePathForLog(dumpPath)"), std::string::npos)
        << "the dump path crash.log names must not expose the private directory layout";
}

// Source side of the breakpoint policy: no per-run immediate-dump budget may
// come back - the classifier alone decides, and it records breakpoints first.
TEST(CrashOutputTruthSourceTest, BreakpointsCarryNoImmediateDumpBudget) {
    namespace fs = std::filesystem;
    const std::string writer =
        ce::test_source::ReadLogicalSource(fs::current_path() / "common" / "crash_dump_writer.cpp");
    const std::string policyHeader =
        ce::test_source::ReadLogicalSource(fs::current_path() / "common" / "crash_dump_policy.h");
    ASSERT_FALSE(writer.empty());
    ASSERT_FALSE(policyHeader.empty());

    EXPECT_EQ(writer.find("g_BreakpointImmediateDumps"), std::string::npos);
    EXPECT_EQ(policyHeader.find("kBreakpointImmediateDumpBudget"), std::string::npos);
    const size_t breakpointCase = policyHeader.find("case static_cast<DWORD>(EXCEPTION_BREAKPOINT):");
    ASSERT_NE(breakpointCase, std::string::npos);
    const size_t caseEnd = policyHeader.find("default:", breakpointCase);
    ASSERT_NE(caseEnd, std::string::npos);
    const std::string body = policyHeader.substr(breakpointCase, caseEnd - breakpointCase);
    EXPECT_EQ(body.find("FirstChanceAction::kDumpNow"), std::string::npos)
        << "a first-chance breakpoint must never dump immediately";
}
