#include <gtest/gtest.h>
#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "common/crash/crash_diagnostic_arm.h"
#include "common/crash/crash_dump_policy.h"
#include "common/crash/crash_first_chance.h"
#include "common/crash/crash_handler_internal.h"
#include "hook/fg/fg_transition_trace.h"

std::optional<int> RunFatalExitChildProbe(int argc, char** argv) {
    if (argc < 2) return std::nullopt;
    bool isProbe = false;
    int exitCode = 3;
    bool nested = false;
    bool candidate = false;
    std::string sessionDir;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--fatal-exit-child-probe") == 0) {
            isProbe = true;
        } else if (std::strncmp(argv[i], "--exit-code=", 12) == 0) {
            exitCode = std::atoi(argv[i] + 12);
        } else if (std::strcmp(argv[i], "--nested") == 0) {
            nested = true;
        } else if (std::strcmp(argv[i], "--candidate-fault") == 0) {
            candidate = true;
        } else if (std::strncmp(argv[i], "--session-dir=", 14) == 0) {
            sessionDir = argv[i] + 14;
        }
    }

    if (!isProbe) {
        return std::nullopt;
    }

    if (!sessionDir.empty()) {
        SetCrashDumpDirectory(sessionDir, false);
        char target[MAX_PATH] = {};
        if (ce::crash_dump_policy::ReadDiagnosticArmMarker(sessionDir.c_str(), target, sizeof(target)) &&
            ce::crash_dump_policy::IsDiagnosticArmMatch(target, "unit_tests.exe")) {
            ce::crash_first_chance::SetDiagnosticArmEnabled(true);
        }
    }

    if (candidate) {
        EXCEPTION_RECORD rec{};
        rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
        rec.ExceptionAddress = reinterpret_cast<void*>(0xDEADBEEF);
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        EXCEPTION_POINTERS ptrs{&rec, &ctx};
        ce::crash_first_chance::RecordFault(&ptrs);
    }

    const bool armed = ce::crash_first_chance::IsDiagnosticArmEnabled();
    std::atomic<bool> attempted{false};
    unsigned dumpWrites = 0;
    const auto captureIfNeeded = [&] {
        if (sessionDir.empty() ||
            !ce::crash_dump_policy::ShouldCapturePreTerminationDump(
                true, static_cast<DWORD>(exitCode), attempted.load(), true,
                ce::crash_dump_policy::TerminationOrigin::kPrimaryModule, false, false, armed) ||
            !ce::crash_dump_policy::TryClaimPreTerminationDump(attempted)) {
            return true;
        }

        const HMODULE dbgHelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dbgHelp) {
            return false;
        }
        g_pMiniDumpWriteDump = reinterpret_cast<MINIDUMPWRITEDUMP>(GetProcAddress(dbgHelp, "MiniDumpWriteDump"));
        const std::string name = "fatal_exit_probe_" + std::to_string(++dumpWrites) + ".dmp";
        const bool wroteDump = WriteSupplementalCrashDump(name.c_str(), GetCurrentProcess(), GetCurrentProcessId(),
                                                          ce::crash_dump_policy::kMinimalDumpType, nullptr);
        g_pMiniDumpWriteDump = nullptr;
        FreeLibrary(dbgHelp);
        return wroteDump && ce::fg_session::WriteTransitionHistoryFile(
                               sessionDir.c_str(), "fatal_exit_child_probe", static_cast<DWORD>(exitCode));
    };
    if (!captureIfNeeded() || (nested && !captureIfNeeded())) {
        return 100;
    }

    ExitProcess(static_cast<UINT>(exitCode));
    return exitCode;
}

namespace {

class ScopedMarkerEnvironment {
public:
    ScopedMarkerEnvironment() {
        const DWORD length = GetEnvironmentVariableA(ce::crash_dump_policy::kDiagnosticArmEnvVar, nullptr, 0);
        if (length != 0) {
            original_.resize(length);
            GetEnvironmentVariableA(ce::crash_dump_policy::kDiagnosticArmEnvVar, original_.data(), length);
        }
        SetEnvironmentVariableA(ce::crash_dump_policy::kDiagnosticArmEnvVar, nullptr);
    }

    ~ScopedMarkerEnvironment() {
        SetEnvironmentVariableA(ce::crash_dump_policy::kDiagnosticArmEnvVar,
                                original_.empty() ? nullptr : original_.c_str());
    }

private:
    std::string original_;
};

std::filesystem::path CreateTestSessionDir(const char* name) {
    static std::atomic<uint32_t> s_counter{0};
    const auto dir = std::filesystem::temp_directory_path() /
                     ("ce_test_fatal_dump_" + std::to_string(GetCurrentProcessId()) + "_" + name + "_" +
                      std::to_string(s_counter.fetch_add(1, std::memory_order_relaxed)));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

bool LaunchChildProbe(const std::filesystem::path& sessionDir, int exitCode, bool nested, bool candidate,
                      DWORD* outExitCode, DWORD* outPid = nullptr) {
    wchar_t selfPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH);

    std::wstring cmd = L"\"" + std::wstring(selfPath) + L"\" --fatal-exit-child-probe --session-dir=\"" +
                       sessionDir.wstring() + L"\" --exit-code=" + std::to_wstring(exitCode);
    if (nested) {
        cmd += L" --nested";
    }
    if (candidate) {
        cmd += L" --candidate-fault";
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> cmdBuffer(cmd.begin(), cmd.end());
    cmdBuffer.push_back(L'\0');

    if (!CreateProcessW(nullptr, cmdBuffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi)) {
        return false;
    }

    if (outPid) {
        *outPid = pi.dwProcessId;
    }

    const bool exited = WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0;
    if (!exited) {
        TerminateProcess(pi.hProcess, 101);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD code = 0;
    const bool gotCode = GetExitCodeProcess(pi.hProcess, &code) != FALSE;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (outExitCode) {
        *outExitCode = code;
    }
    return exited && gotCode;
}

size_t CountDumpFiles(const std::filesystem::path& dir) {
    size_t count = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!ec && entry.is_regular_file(ec) && entry.path().extension() == ".dmp") {
            ++count;
        }
    }
    return count;
}

}  // namespace

TEST(FatalExitDumpTest, DiagnosticArmMarkerMatching) {
    const ScopedMarkerEnvironment markerEnvironment;
    EXPECT_TRUE(ce::crash_dump_policy::IsDiagnosticArmMatch("afop.exe", "D:\\Games\\Avatar\\afop.exe"));
    EXPECT_TRUE(ce::crash_dump_policy::IsDiagnosticArmMatch("  AFOP.EXE \r\n", "afop.exe"));
    EXPECT_TRUE(ce::crash_dump_policy::IsDiagnosticArmMatch("afop.exe", "AFOP.EXE"));
    EXPECT_FALSE(ce::crash_dump_policy::IsDiagnosticArmMatch("afop.exe", "other.exe"));
    EXPECT_FALSE(ce::crash_dump_policy::IsDiagnosticArmMatch(nullptr, "afop.exe"));
    EXPECT_FALSE(ce::crash_dump_policy::IsDiagnosticArmMatch("afop.exe", nullptr));

    const auto sessionDir = CreateTestSessionDir("marker");
    char target[MAX_PATH] = {};
    EXPECT_FALSE(ce::crash_dump_policy::ReadDiagnosticArmMarker(sessionDir.string().c_str(), target, sizeof(target)));

    const auto markerPath = sessionDir / ce::crash_dump_policy::kDiagnosticArmMarkerFileName;
    {
        std::ofstream out(markerPath);
        out << "  afop.exe  \r\n";
    }

    EXPECT_TRUE(ce::crash_dump_policy::ReadDiagnosticArmMarker(sessionDir.string().c_str(), target, sizeof(target)));
    EXPECT_STREQ(target, "afop.exe");
    EXPECT_TRUE(ce::crash_dump_policy::IsDiagnosticArmMatch(target, "afop.exe"));

    std::error_code ec;
    std::filesystem::remove_all(sessionDir, ec);
}

TEST(FatalExitDumpTest, OverlongMarkerCannotArmATruncatedProcessName) {
    const auto sessionDir = CreateTestSessionDir("long_marker");
    const auto marker = sessionDir / ce::crash_dump_policy::kDiagnosticArmMarkerFileName;
    {
        std::ofstream out(marker);
        out << std::string(MAX_PATH - 1 - std::strlen("game.exe"), ' ') << "game.exe.extra";
    }
    char target[MAX_PATH] = {};
    EXPECT_FALSE(ce::crash_dump_policy::ReadDiagnosticArmMarkerFile(marker.string().c_str(), target, sizeof(target)));

    std::error_code ec;
    std::filesystem::remove_all(sessionDir, ec);
}

TEST(FatalExitDumpTest, PolicyAllowsArmedNonzeroSelfTermination) {
    using namespace ce::crash_dump_policy;

    // Unarmed self-termination from primary image with exit code 3 (Avatar CrashAnalyser)
    EXPECT_FALSE(ShouldCapturePreTerminationDump(/*targetIsCurrentProcess=*/true, /*exitCode=*/3,
                                                 /*alreadyAttempted=*/false,
                                                 /*frameGenerationRuntimeActiveOrRecent=*/true,
                                                 /*origin=*/TerminationOrigin::kPrimaryModule,
                                                 /*terminationFollowsUnresolvedFault=*/false,
                                                 /*terminationFollowsApplicationErrorDialog=*/false,
                                                 /*diagnosticArmActive=*/false));

    // Armed self-termination with exit code 3: ALLOWED!
    EXPECT_TRUE(ShouldCapturePreTerminationDump(/*targetIsCurrentProcess=*/true, /*exitCode=*/3,
                                                /*alreadyAttempted=*/false,
                                                /*frameGenerationRuntimeActiveOrRecent=*/true,
                                                /*origin=*/TerminationOrigin::kPrimaryModule,
                                                /*terminationFollowsUnresolvedFault=*/false,
                                                /*terminationFollowsApplicationErrorDialog=*/false,
                                                /*diagnosticArmActive=*/true));

    // Armed clean exit (0): SKIPPED (clean quits stay clean)
    EXPECT_FALSE(ShouldCapturePreTerminationDump(/*targetIsCurrentProcess=*/true, /*exitCode=*/0,
                                                 /*alreadyAttempted=*/false,
                                                 /*frameGenerationRuntimeActiveOrRecent=*/true,
                                                 /*origin=*/TerminationOrigin::kPrimaryModule,
                                                 /*terminationFollowsUnresolvedFault=*/false,
                                                 /*terminationFollowsApplicationErrorDialog=*/false,
                                                 /*diagnosticArmActive=*/true));

    // Nested termination call (alreadyAttempted == true): PREVENTS duplicate dump
    EXPECT_FALSE(ShouldCapturePreTerminationDump(/*targetIsCurrentProcess=*/true, /*exitCode=*/3,
                                                 /*alreadyAttempted=*/true,
                                                 /*frameGenerationRuntimeActiveOrRecent=*/true,
                                                 /*origin=*/TerminationOrigin::kPrimaryModule,
                                                 /*terminationFollowsUnresolvedFault=*/false,
                                                 /*terminationFollowsApplicationErrorDialog=*/false,
                                                 /*diagnosticArmActive=*/true));

    // Target is not current process: SKIPPED
    EXPECT_FALSE(ShouldCapturePreTerminationDump(/*targetIsCurrentProcess=*/false, /*exitCode=*/3,
                                                 /*alreadyAttempted=*/false,
                                                 /*frameGenerationRuntimeActiveOrRecent=*/true,
                                                 /*origin=*/TerminationOrigin::kPrimaryModule,
                                                 /*terminationFollowsUnresolvedFault=*/false,
                                                 /*terminationFollowsApplicationErrorDialog=*/false,
                                                 /*diagnosticArmActive=*/true));

    for (const DWORD sentinel : {kProcessIsTerminatingExitCode, kExternalDumpStormTerminationExitCode}) {
        EXPECT_FALSE(ShouldCapturePreTerminationDump(true, sentinel, false, true, TerminationOrigin::kUnknown,
                                                     true, true, true));
    }

    // Scope check: armed dump selects fatal assert scope
    EXPECT_EQ(PreTerminationDumpScope(/*crashLikeExit=*/false, /*followsFault=*/false, /*armed=*/true),
              ExternalDumpScope::kFatalAssert);
}

TEST(FatalExitDumpTest, BoundedFirstChanceFaultHistoryPreservesCandidateDistinction) {
    ce::crash_first_chance::ResetForTesting();
    ce::crash_first_chance::SetDiagnosticArmEnabled(true);

    EXCEPTION_RECORD record1{};
    record1.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record1.ExceptionAddress = reinterpret_cast<void*>(0x12345678);

    CONTEXT context1{};
    context1.ContextFlags = CONTEXT_CONTROL;
#ifdef _WIN64
    context1.Rip = 0x12345678;
#else
    context1.Eip = 0x12345678;
#endif

    EXCEPTION_POINTERS pointers1{};
    pointers1.ExceptionRecord = &record1;
    pointers1.ContextRecord = &context1;

    // Record on current thread
    ce::crash_first_chance::RecordFault(&pointers1);

    ce::crash_first_chance::FaultCandidate candidate{};
    EXPECT_TRUE(ce::crash_first_chance::FindBestFaultCandidate(&candidate, GetCurrentThreadId(), 3));
    EXPECT_TRUE(candidate.onTerminatingThread);
    EXPECT_EQ(candidate.threadId, GetCurrentThreadId());
    EXPECT_EQ(candidate.exceptionCode, static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION));
    EXPECT_EQ(candidate.instructionAddress, 0x12345678ULL);

    // Asynchronous reporter thread (e.g. CrashAnalyser) terminating the process with code 3:
    // candidate from another thread is returned as correlation evidence.
    ce::crash_first_chance::FaultCandidate asyncCandidate{};
    const DWORD reporterTid = GetCurrentThreadId() + 100;
    EXPECT_TRUE(ce::crash_first_chance::FindBestFaultCandidate(&asyncCandidate, reporterTid, 3));
    EXPECT_FALSE(asyncCandidate.onTerminatingThread);
    EXPECT_EQ(asyncCandidate.threadId, GetCurrentThreadId());
    EXPECT_EQ(asyncCandidate.exceptionCode, static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION));

    // Wrap the history and verify the newest candidate wins within both match classes.
    for (uintptr_t i = 0; i < 24; ++i) {
        record1.ExceptionAddress = reinterpret_cast<void*>(0x2000 + i);
        ce::crash_first_chance::RecordFault(&pointers1);
    }
    ASSERT_TRUE(ce::crash_first_chance::FindBestFaultCandidate(&candidate, GetCurrentThreadId(), 3));
    EXPECT_EQ(candidate.instructionAddress, 0x2017u);
    ASSERT_TRUE(ce::crash_first_chance::FindBestFaultCandidate(&candidate, reporterTid, EXCEPTION_ACCESS_VIOLATION));
    EXPECT_EQ(candidate.instructionAddress, 0x2017u);
    EXPECT_EQ(candidate.sequence, 25u);

    // Continuing execution clears active slot but preserves candidate history
    ce::crash_first_chance::ClearFaultForCurrentThread();
    EXPECT_FALSE(ce::crash_first_chance::CopyFaultForCurrentThread(nullptr, nullptr));

    ce::crash_first_chance::FaultCandidate retainedCandidate{};
    EXPECT_TRUE(ce::crash_first_chance::FindBestFaultCandidate(&retainedCandidate, reporterTid, 3));
    EXPECT_EQ(retainedCandidate.exceptionCode, static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION));

    ce::crash_first_chance::SetDiagnosticArmEnabled(false);
    ce::crash_first_chance::SetDiagnosticArmEnabled(true);
    EXPECT_FALSE(ce::crash_first_chance::FindBestFaultCandidate(&candidate, GetCurrentThreadId(), 3));
    ce::crash_first_chance::ResetForTesting();
}

TEST(FatalExitDumpTest, TransitionTraceSnapshotWritesValidFile) {
    const auto sessionDir = CreateTestSessionDir("trace");

    ce::fg_session::RecordFFXContextCreate(reinterpret_cast<void*>(0x1000), 0x0001, 0, false);
    ce::fg_session::RecordFFXConfigure(reinterpret_cast<void*>(0x1000), true, reinterpret_cast<void*>(0x2000),
                                       reinterpret_cast<void*>(0x3000), 0);
    ce::fg_session::RecordStreamlineDLSSGOptions(1, true, 0);
    ce::fg_session::RecordSwapchainTransition("Create", reinterpret_cast<void*>(0x4000),
                                              reinterpret_cast<void*>(0x5000), reinterpret_cast<void*>(0x6000), 0);

    ce::fg_session::RecordStreamlineReflexSleepFailure(5);
    ce::fg_session::RecordStreamlineReflexOptions(2, 0);
    for (uint64_t frame = 0; frame < 1000; ++frame) {
        ce::fg_session::RecordFFXCallbackArrival(reinterpret_cast<void*>(0x3000), frame, (frame & 1) != 0, true);
        ce::fg_session::RecordStreamlineDLSSGOptions(1, true, 0);
        ce::fg_session::RecordStreamlineDLSSGOptions(2, false, 0);
        ce::fg_session::RecordStreamlineReflexSleepFailure(5);
    }
    ce::fg_session::RecordFFXCallbackArrival(reinterpret_cast<void*>(0x3000), 1000, false, false);

    EXPECT_TRUE(ce::fg_session::WriteTransitionHistoryFile(sessionDir.string().c_str(), "test_trigger", 3));
    EXPECT_FALSE(ce::fg_session::WriteTransitionHistoryFile(sessionDir.string().c_str(), "nested_exit", 0));

    const auto tracePath = sessionDir / ("fg_transition_trace_" + std::to_string(GetCurrentProcessId()) + ".txt");
    EXPECT_TRUE(std::filesystem::exists(tracePath));

    std::ifstream stream(tracePath);
    std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("Capture Engine Frame Generation Transition Trace"), std::string::npos);
    EXPECT_NE(content.find("ExitCode: 0x00000003"), std::string::npos);
    EXPECT_NE(content.find("ContextCreate"), std::string::npos);
    EXPECT_NE(content.find("DLSSGOptions: viewport=1 enabled=1"), std::string::npos);
    EXPECT_NE(content.find("DLSSGOptions: viewport=2 enabled=0"), std::string::npos);
    EXPECT_NE(content.find("SwapchainTransition"), std::string::npos);
    EXPECT_NE(content.find("ReflexOptions: mode=2"), std::string::npos);
    EXPECT_NE(content.find("ReflexSleepFailed"), std::string::npos);
    EXPECT_NE(content.find("CallbackArrived: frameId=0"), std::string::npos);
    EXPECT_NE(content.find("CallbackArrived: frameId=1000"), std::string::npos);
    EXPECT_EQ(content.find("CallbackArrived: frameId=999"), std::string::npos);
    stream.close();

    std::error_code ec;
    std::filesystem::remove_all(sessionDir, ec);
}

TEST(FatalExitDumpTest, ChildProbeUnarmedProducesNoDump) {
    const ScopedMarkerEnvironment markerEnvironment;
    const auto sessionDir = CreateTestSessionDir("child_unarmed");
    DWORD exitCode = 0;
    DWORD pid = 0;

    ASSERT_TRUE(LaunchChildProbe(sessionDir, 3, /*nested=*/false, /*candidate=*/false, &exitCode, &pid));
    EXPECT_EQ(exitCode, 3u);
    EXPECT_EQ(CountDumpFiles(sessionDir), 0u);

    std::error_code ec;
    std::filesystem::remove_all(sessionDir, ec);
}

TEST(FatalExitDumpTest, ChildProbeArmedProducesOneReadableDumpAndTransitionTrace) {
    const ScopedMarkerEnvironment markerEnvironment;
    const auto sessionDir = CreateTestSessionDir("child_armed");

    // Arm the diagnostic for unit_tests.exe
    const auto markerPath = sessionDir / ce::crash_dump_policy::kDiagnosticArmMarkerFileName;
    {
        std::ofstream out(markerPath);
        out << "unit_tests.exe\r\n";
    }

    DWORD exitCode = 0;
    DWORD pid = 0;
    ASSERT_TRUE(LaunchChildProbe(sessionDir, 3, /*nested=*/false, /*candidate=*/true, &exitCode, &pid));
    EXPECT_EQ(exitCode, 3u);

    // Exactly one dump created!
    EXPECT_EQ(CountDumpFiles(sessionDir), 1u);

    // Transition trace exists and contains child's PID and exit code
    const auto tracePath = sessionDir / ("fg_transition_trace_" + std::to_string(pid) + ".txt");
    ASSERT_TRUE(std::filesystem::exists(tracePath));
    std::ifstream stream(tracePath);
    std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("PID: " + std::to_string(pid)), std::string::npos);
    EXPECT_NE(content.find("ExitCode: 0x00000003"), std::string::npos);
    EXPECT_NE(content.find("Candidate[0]: seq=1"), std::string::npos);
    stream.close();

    // Verify dump file header is readable
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(sessionDir, ec)) {
        if (entry.path().extension() == ".dmp") {
            std::ifstream dumpStream(entry.path(), std::ios::binary);
            MINIDUMP_HEADER header{};
            dumpStream.read(reinterpret_cast<char*>(&header), sizeof(header));
            EXPECT_EQ(header.Signature, MINIDUMP_SIGNATURE);
            EXPECT_EQ(LOWORD(header.Version), MINIDUMP_VERSION);
            EXPECT_GT(header.NumberOfStreams, 0u);
            dumpStream.seekg(header.StreamDirectoryRva);
            MINIDUMP_DIRECTORY directory{};
            ASSERT_TRUE(dumpStream.read(reinterpret_cast<char*>(&directory), sizeof(directory)).good());
            EXPECT_NE(directory.StreamType, UnusedStream);
        }
    }

    std::filesystem::remove_all(sessionDir, ec);
}

TEST(FatalExitDumpTest, ChildProbeArmedNestedHooksProduceOnlyOneDump) {
    const ScopedMarkerEnvironment markerEnvironment;
    const auto sessionDir = CreateTestSessionDir("child_nested");

    // Arm the diagnostic for unit_tests.exe
    const auto markerPath = sessionDir / ce::crash_dump_policy::kDiagnosticArmMarkerFileName;
    {
        std::ofstream out(markerPath);
        out << "unit_tests.exe\r\n";
    }

    DWORD exitCode = 0;
    DWORD pid = 0;
    // Launch child with nested calls
    ASSERT_TRUE(LaunchChildProbe(sessionDir, 3, /*nested=*/true, /*candidate=*/false, &exitCode, &pid));
    EXPECT_EQ(exitCode, 3u);

    // Nested call attempt is guarded by atomic attempt guard: still exactly 1 dump!
    EXPECT_EQ(CountDumpFiles(sessionDir), 1u);

    std::error_code ec;
    std::filesystem::remove_all(sessionDir, ec);
}
