#include <gtest/gtest.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "dx12_overlay_policy/overlay_submission.h"
#include "process_thread_walk.h"
#include "source_fragment_reader.h"
#include "window_text_safe.h"

// Regressions for the three startup paths that could stall a game for an
// unbounded or system-load-dependent amount of time:
//   * ThreadQuiescence enumerating every thread on the machine, once per inline
//     hook entry patch, with every peer thread suspended for all of it,
//   * the temp-swapchain Present-hook bootstrap building a throwaway WARP D3D12
//     device in a process that presents through DirectDraw, and
//   * GetWindowText sending WM_GETTEXT with no timeout into a thread of the
//     game's own process, from the render thread and from the freeze watchdog.

namespace {

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

constexpr DWORD kWalkAccess = THREAD_QUERY_INFORMATION | SYNCHRONIZE;

std::vector<DWORD> CollectViaProcessScopedWalk(DWORD excludeThreadId, ce::process_threads::WalkResult* outResult) {
    std::vector<DWORD> ids;
    const auto result = ce::process_threads::WalkCurrentProcessThreads(
        excludeThreadId, kWalkAccess, [&](HANDLE handle, DWORD threadId) {
            ids.push_back(threadId);
            CloseHandle(handle);
            return true;
        });
    if (outResult) {
        *outResult = result;
    }
    return ids;
}

std::vector<DWORD> CollectViaSystemSnapshot(DWORD excludeThreadId) {
    std::vector<DWORD> ids;
    ce::process_threads::WalkCurrentProcessThreadsViaSystemSnapshot(excludeThreadId, kWalkAccess,
                                                                    [&](HANDLE handle, DWORD threadId) {
                                                                        ids.push_back(threadId);
                                                                        CloseHandle(handle);
                                                                        return true;
                                                                    });
    return ids;
}

// A thread that parks on an event and never pumps messages, so a window it owns
// can only ever be answered by the caller's timeout.
class ParkedWindowThread {
public:
    ParkedWindowThread() {
        ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        thread_ = std::thread([this]() {
            window_ = CreateWindowExW(0, L"STATIC", L"ParkedTitle", WS_OVERLAPPED, 0, 0, 10, 10, nullptr, nullptr,
                                      GetModuleHandleW(nullptr), nullptr);
            threadId_ = GetCurrentThreadId();
            SetEvent(ready_);
            WaitForSingleObject(stop_, INFINITE);
            if (window_) {
                DestroyWindow(window_);
            }
        });
        WaitForSingleObject(ready_, INFINITE);
    }

    ~ParkedWindowThread() {
        SetEvent(stop_);
        thread_.join();
        CloseHandle(ready_);
        CloseHandle(stop_);
    }

    HWND window() const {
        return window_;
    }
    DWORD threadId() const {
        return threadId_;
    }

private:
    std::thread thread_;
    HANDLE ready_ = nullptr;
    HANDLE stop_ = nullptr;
    HWND window_ = nullptr;
    DWORD threadId_ = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// Process-scoped thread walk
// ---------------------------------------------------------------------------

TEST(ProcessThreadWalkTest, ProcessScopedWalkSeesTheSameThreadsAsTheSystemSnapshot) {
    ce::process_threads::ResetProcessScopedWalkAvailabilityForTesting();
    ParkedWindowThread parked;  // Guarantees at least one peer thread exists.

    const DWORD self = GetCurrentThreadId();
    ce::process_threads::WalkResult result = ce::process_threads::WalkResult::kAborted;
    const std::vector<DWORD> scoped = CollectViaProcessScopedWalk(self, &result);

    if (result == ce::process_threads::WalkResult::kUnavailable) {
        GTEST_SKIP() << "NtGetNextThread unavailable on this host";
    }
    ASSERT_EQ(result, ce::process_threads::WalkResult::kCompleted);

    const std::vector<DWORD> snapshot = CollectViaSystemSnapshot(self);

    // Threads can start and exit around either walk, so the invariant is that
    // both routes agree about the threads that were alive across both of them -
    // including the parked peer, which cannot exit during the test.
    EXPECT_NE(std::find(scoped.begin(), scoped.end(), parked.threadId()), scoped.end());
    EXPECT_NE(std::find(snapshot.begin(), snapshot.end(), parked.threadId()), snapshot.end());
}

TEST(ProcessThreadWalkTest, BothWalksExcludeTheCallingThread) {
    ce::process_threads::ResetProcessScopedWalkAvailabilityForTesting();
    const DWORD self = GetCurrentThreadId();

    ce::process_threads::WalkResult result = ce::process_threads::WalkResult::kAborted;
    const std::vector<DWORD> scoped = CollectViaProcessScopedWalk(self, &result);
    if (result == ce::process_threads::WalkResult::kCompleted) {
        EXPECT_EQ(std::find(scoped.begin(), scoped.end(), self), scoped.end());
    }

    const std::vector<DWORD> snapshot = CollectViaSystemSnapshot(self);
    EXPECT_EQ(std::find(snapshot.begin(), snapshot.end(), self), snapshot.end());
}

TEST(ProcessThreadWalkTest, AVisitorThatStopsAbortsTheWalkInsteadOfReportingCompletion) {
    ce::process_threads::ResetProcessScopedWalkAvailabilityForTesting();
    ParkedWindowThread parked;

    int visited = 0;
    const auto scopedResult = ce::process_threads::WalkCurrentProcessThreads(
        GetCurrentThreadId(), kWalkAccess, [&](HANDLE handle, DWORD) {
            ++visited;
            CloseHandle(handle);
            return false;
        });
    if (scopedResult != ce::process_threads::WalkResult::kUnavailable) {
        EXPECT_EQ(scopedResult, ce::process_threads::WalkResult::kAborted);
        EXPECT_EQ(visited, 1);
    }

    visited = 0;
    const auto snapshotResult = ce::process_threads::WalkCurrentProcessThreadsViaSystemSnapshot(
        GetCurrentThreadId(), kWalkAccess, [&](HANDLE handle, DWORD) {
            ++visited;
            CloseHandle(handle);
            return false;
        });
    EXPECT_EQ(snapshotResult, ce::process_threads::WalkResult::kAborted);
    EXPECT_EQ(visited, 1);
}

TEST(ProcessThreadWalkTest, VisitorIsANonAllocatingViewBecauseTheWalkRunsWithPeersSuspended) {
    // A second pass runs while peer threads are already suspended, and one of
    // them may hold the heap lock. A std::function-shaped visitor would
    // allocate there; a two-pointer view cannot.
    static_assert(sizeof(ce::process_threads::ThreadVisitor) <= 2 * sizeof(void*),
                  "ThreadVisitor must stay a non-owning callable view");
    static_assert(std::is_trivially_copyable_v<ce::process_threads::ThreadVisitor>,
                  "ThreadVisitor must not own anything it has to destroy");

    const std::string header = ReadSource("hook/common/process_thread_walk.h");
    ASSERT_FALSE(header.empty());
    EXPECT_EQ(header.find("std::function<"), std::string::npos);
    EXPECT_EQ(header.find("#include <functional>"), std::string::npos);
}

TEST(ProcessThreadWalkSourceTest, QuiescencePrefersTheProcessScopedWalkAndKeepsTheSnapshotFallback) {
    const std::string source = ReadSource("hook/wrappers/hook_patch_transaction.cpp");
    ASSERT_FALSE(source.empty());

    const size_t scoped = source.find("WalkCurrentProcessThreads(currentThreadId");
    const size_t fallback = source.find("WalkCurrentProcessThreadsViaSystemSnapshot(currentThreadId");
    ASSERT_NE(scoped, std::string::npos);
    ASSERT_NE(fallback, std::string::npos);
    EXPECT_LT(scoped, fallback);

    // The system-wide snapshot must never be reached except through the
    // unavailability answer, and never by name inside this unit again.
    EXPECT_EQ(source.find("CreateToolhelp32Snapshot"), std::string::npos);
    EXPECT_NE(source.find("WalkResult::kUnavailable"), std::string::npos);

    // A whole-process freeze that takes real time has to be visible in the log,
    // and the report has to happen AFTER every peer resumes: the logger takes a
    // lock and can allocate, and a suspended peer may hold either.
    const size_t measure = source.find("quiesceElapsedMs_ = GetTickCount64() - enterMs;");
    const size_t destructor = source.find("ThreadQuiescence::~ThreadQuiescence()");
    const size_t report = source.find("ThreadQuiescence: %zu peer thread(s) suspended for");
    ASSERT_NE(measure, std::string::npos);
    ASSERT_NE(destructor, std::string::npos);
    ASSERT_NE(report, std::string::npos);
    EXPECT_LT(measure, destructor);
    EXPECT_LT(destructor, report);

    // Nothing may log from inside Quiesce() itself: every call between its body
    // and the destructor would run with peers suspended.
    const size_t quiesceBody = source.find("void ThreadQuiescence::Quiesce()");
    ASSERT_NE(quiesceBody, std::string::npos);
    ASSERT_LT(quiesceBody, destructor);
    EXPECT_GT(source.find("HookLog(", quiesceBody), destructor)
        << "a log call sits inside Quiesce(), where every peer thread is suspended";
}

// ---------------------------------------------------------------------------
// Temp-swapchain bootstrap gating
// ---------------------------------------------------------------------------

TEST(TempSwapchainBootstrapPolicyTest, LegacyDirectDrawProcessDoesNotPayForAWarpDevice) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipTempSwapchainForLegacyPresentationProcess(
        /*ddrawOrD3d8ModuleLoaded=*/true, /*d3d11Or10ModuleLoaded=*/false,
        /*d3d11Or10DeviceCreated=*/false, /*d3d12DeviceCreated=*/false));
}

TEST(TempSwapchainBootstrapPolicyTest, AnyDxgiPresentationEvidenceKeepsTheBootstrap) {
    // A DirectDraw-to-DXGI translation wrapper maps d3d11.dll.
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipTempSwapchainForLegacyPresentationProcess(true, true, false,
                                                                                              false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipTempSwapchainForLegacyPresentationProcess(true, false, true,
                                                                                              false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipTempSwapchainForLegacyPresentationProcess(true, false, false,
                                                                                              true));
}

TEST(TempSwapchainBootstrapPolicyTest, ProcessWithoutLegacyModulesIsUnaffected) {
    // A DX12 title that has neither ddraw nor d3d8 mapped keeps the historical
    // behaviour, including before its own device has been observed.
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipTempSwapchainForLegacyPresentationProcess(false, false, false,
                                                                                              false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipTempSwapchainForLegacyPresentationProcess(false, true, false,
                                                                                              false));
}

TEST(TempSwapchainBootstrapSourceTest, GuardedRouteRefusesBeforeSpendingAnAttempt) {
    const std::string source = ReadSource("hook/apis/dx12_hook_main.cpp");
    ASSERT_FALSE(source.empty());

    const size_t refusal = source.find("TempSwapchainRefusedForLegacyPresentationProcess()");
    const size_t attemptCounter = source.find("s_attemptLogCount.fetch_add");
    ASSERT_NE(refusal, std::string::npos);
    ASSERT_NE(attemptCounter, std::string::npos);
    EXPECT_LT(refusal, attemptCounter);
}

TEST(TempSwapchainBootstrapSourceTest, ExpensiveRoutineRefusesBeforeCreatingAnything) {
    const std::string source = ReadSource("hook/apis/dx12_hook_hook_install.cpp");
    ASSERT_FALSE(source.empty());

    const size_t refusal = source.find("if (TempSwapchainRefusedForLegacyPresentationProcess())");
    const size_t createDevice = source.find("pD3D12CreateDevice(");
    ASSERT_NE(refusal, std::string::npos);
    ASSERT_NE(createDevice, std::string::npos);
    EXPECT_LT(refusal, createDevice);
}

// ---------------------------------------------------------------------------
// Bounded window-title reads
// ---------------------------------------------------------------------------

TEST(BoundedWindowTextTest, SameThreadWindowStillReportsItsTitle) {
    HWND window = CreateWindowExW(0, L"STATIC", L"SameThreadTitle", WS_OVERLAPPED, 0, 0, 10, 10, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(window, nullptr);

    char title[64] = {};
    ce::window_text::ReadWindowTitleBounded(window, title, static_cast<int>(sizeof(title)));
    EXPECT_STREQ(title, "SameThreadTitle");

    DestroyWindow(window);
}

TEST(BoundedWindowTextTest, WindowOwnedByANonPumpingThreadReturnsEmptyInsteadOfBlocking) {
    ParkedWindowThread parked;
    ASSERT_NE(parked.window(), nullptr);
    ASSERT_NE(parked.threadId(), GetCurrentThreadId());

    char title[64] = "poison";
    ce::window_text::ReadWindowTitleBounded(parked.window(), title, static_cast<int>(sizeof(title)));

    // The owning thread parks without a message pump, so the only way this
    // call can return is the bound. Without it the caller waits forever.
    EXPECT_STREQ(title, "");
}

TEST(BoundedWindowTextTest, DegenerateArgumentsAreRejectedWithoutASend) {
    char title[8] = "poison";
    ce::window_text::ReadWindowTitleBounded(nullptr, title, static_cast<int>(sizeof(title)));
    EXPECT_STREQ(title, "");

    // Must not write through a null buffer or a non-positive size.
    ce::window_text::ReadWindowTitleBounded(nullptr, nullptr, 16);
    ce::window_text::ReadWindowTitleBounded(nullptr, title, 0);
}

TEST(BoundedWindowTextSourceTest, InProcessTitleReadersUseTheBoundedReader) {
    const std::string watchdog = ReadSource("hook/common/freeze_watchdog.cpp");
    ASSERT_FALSE(watchdog.empty());
    EXPECT_NE(watchdog.find("ce::window_text::ReadWindowTitleBounded"), std::string::npos);
    EXPECT_EQ(watchdog.find("GetWindowTextA(hwnd"), std::string::npos);

    const std::string moduleTable = ReadSource("hook/common/overlay_compat_detail/module_table.h");
    ASSERT_FALSE(moduleTable.empty());
    EXPECT_NE(moduleTable.find("ce::window_text::ReadWindowTitleBounded"), std::string::npos);
    EXPECT_EQ(moduleTable.find("GetWindowTextA(hwnd"), std::string::npos);
}
