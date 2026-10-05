#include <gtest/gtest.h>

#include <thread>
#include <filesystem>
#include "source_fragment_reader.h"
#include "common/platform/window_heartbeat.h"

namespace {
namespace heartbeat = ce::window_heartbeat;

ApplicationProfile Profile(const char* process = "game.exe", bool enabled = true) {
    ApplicationProfile profile;
    profile.target.pattern = process;
    profile.windowHeartbeatEnabled = enabled;
    return profile;
}

heartbeat::Window Candidate() {
    heartbeat::Window window;
    window.handle = reinterpret_cast<HWND>(static_cast<uintptr_t>(100));
    window.processId = 123;
    window.threadId = 456;
    window.processName = "GAME.EXE";
    window.visible = true;
    window.cloaked = false;
    window.borderlessFullscreen = true;
    return window;
}

class FakeBackend final : public heartbeat::Backend {
public:
    DWORD foreground = 789;
    DWORD sendError = ERROR_SUCCESS;
    bool enumerationSucceeds = true;
    std::vector<heartbeat::Window> windows{Candidate()};
    std::vector<ULONG_PTR> tokens;
    DWORD ForegroundProcessId() override { return foreground; }
    bool Enumerate(std::vector<heartbeat::Window>& out) override {
        out = windows;
        return enumerationSucceeds;
    }
    DWORD SendNull(const heartbeat::Window&, ULONG_PTR token, SENDASYNCPROC) override {
        tokens.push_back(token);
        return sendError;
    }
};

TEST(WindowHeartbeatTest, CadenceIs250MsAndDelayedTicksNeverCatchUpInABurst) {
    heartbeat::Pump pump(999);
    FakeBackend backend;
    pump.Configure({Profile()});
    EXPECT_EQ(pump.WaitMs(0), 0u);
    EXPECT_EQ(pump.Tick(0, backend, nullptr).sent, 1u);
    pump.Complete(backend.windows[0].handle, backend.tokens.back());
    EXPECT_EQ(pump.WaitMs(100), 150u);
    EXPECT_FALSE(pump.Tick(249, backend, nullptr).ran);
    EXPECT_EQ(pump.Tick(250, backend, nullptr).sent, 1u);
    pump.Complete(backend.windows[0].handle, backend.tokens.back());
    EXPECT_EQ(pump.Tick(10000, backend, nullptr).sent, 1u);
    EXPECT_FALSE(pump.Tick(10000, backend, nullptr).ran);
    EXPECT_EQ(backend.tokens.size(), 3u);
}

TEST(WindowHeartbeatTest, StalledWindowKeepsOnlyOneRequestUntilCompletion) {
    heartbeat::Pump pump(999);
    FakeBackend backend;
    pump.Configure({Profile()});
    ASSERT_EQ(pump.Tick(0, backend, nullptr).sent, 1u);
    for (uint64_t now = 250; now <= 10000; now += 250) {
        const auto result = pump.Tick(now, backend, nullptr);
        EXPECT_EQ(result.sent, 0u);
        EXPECT_EQ(result.pending, 1u);
    }
    ASSERT_EQ(backend.tokens.size(), 1u);
    pump.Complete(backend.windows[0].handle, backend.tokens[0] + 1);
    EXPECT_EQ(pump.Tick(10250, backend, nullptr).sent, 0u);
    pump.Complete(backend.windows[0].handle, backend.tokens[0]);
    EXPECT_EQ(pump.Tick(10500, backend, nullptr).sent, 1u);
}

TEST(WindowHeartbeatTest, DisabledReloadCannotDuplicateAnOutstandingRequest) {
    heartbeat::Pump pump(999);
    FakeBackend backend;
    pump.Configure({Profile()});
    ASSERT_EQ(pump.Tick(0, backend, nullptr).sent, 1u);
    pump.Configure({Profile("game.exe", false)});
    EXPECT_FALSE(pump.Enabled());
    EXPECT_EQ(pump.WaitMs(250), INFINITE);
    EXPECT_FALSE(pump.Tick(250, backend, nullptr).ran);
    pump.Configure({Profile()});
    EXPECT_EQ(pump.Tick(500, backend, nullptr).sent, 0u);
    pump.Complete(backend.windows[0].handle, backend.tokens[0]);
    EXPECT_EQ(pump.Tick(750, backend, nullptr).sent, 1u);
    pump.Configure({});
    EXPECT_FALSE(pump.Tick(1000, backend, nullptr).ran);
}

TEST(WindowHeartbeatTest, ForegroundOwnHiddenMinimizedCloakedAndOrdinaryWindowsAreExcluded) {
    const auto check = [](const heartbeat::Window& window, DWORD foreground, DWORD owner) {
        heartbeat::Pump pump(owner);
        FakeBackend backend;
        backend.windows = {window};
        backend.foreground = foreground;
        pump.Configure({Profile()});
        EXPECT_EQ(pump.Tick(0, backend, nullptr).sent, 0u);
        EXPECT_TRUE(backend.tokens.empty());
    };
    const auto base = Candidate();
    check(base, base.processId, 999);
    check(base, 789, base.processId);
    check(base, 0, 999);
    auto window = base;
    window.visible = false;
    check(window, 789, 999);
    window = base;
    window.minimized = true;
    check(window, 789, 999);
    window = base;
    window.cloaked = true;
    check(window, 789, 999);
    window = base;
    window.borderlessFullscreen = false;
    check(window, 789, 999);
    window = base;
    window.processName = "othergame.exe";
    check(window, 789, 999);
    window = base;
    window.handle = nullptr;
    check(window, 789, 999);
}

TEST(WindowHeartbeatTest, FailedDeliveryAndFailedEnumerationRemainBoundedAndRecover) {
    heartbeat::Pump pump(999);
    FakeBackend backend;
    pump.Configure({Profile()});
    backend.sendError = ERROR_ACCESS_DENIED;
    const auto failure = pump.Tick(0, backend, nullptr);
    EXPECT_EQ(failure.failed, 1u);
    EXPECT_EQ(failure.lastError, ERROR_ACCESS_DENIED);
    EXPECT_FALSE(pump.Tick(249, backend, nullptr).ran);
    backend.sendError = ERROR_SUCCESS;
    EXPECT_EQ(pump.Tick(250, backend, nullptr).sent, 1u);
    backend.enumerationSucceeds = false;
    EXPECT_EQ(pump.Tick(500, backend, nullptr).failed, 1u);
    backend.enumerationSucceeds = true;
    EXPECT_EQ(pump.Tick(750, backend, nullptr).sent, 0u);
    EXPECT_EQ(backend.tokens.size(), 2u);
}

TEST(WindowHeartbeatTest, ReusedWindowHandleRejectsThePreviousLifetimeCompletion) {
    heartbeat::Pump pump(999);
    FakeBackend backend;
    pump.Configure({Profile()});
    ASSERT_EQ(pump.Tick(0, backend, nullptr).sent, 1u);
    ++backend.windows[0].threadId;
    ASSERT_EQ(pump.Tick(250, backend, nullptr).sent, 1u);
    pump.Complete(backend.windows[0].handle, backend.tokens[0]);
    EXPECT_EQ(pump.Tick(500, backend, nullptr).sent, 0u);
    pump.Complete(backend.windows[0].handle, backend.tokens[1]);
    EXPECT_EQ(pump.Tick(750, backend, nullptr).sent, 1u);
}

TEST(WindowHeartbeatTest, WorkerCanDisableStopAndRestartWithoutWaitingForTargetMessages) {
    heartbeat::Service service;
    EXPECT_TRUE(service.UpdateProfiles({}));
    EXPECT_TRUE(service.UpdateProfiles({Profile("nonexistent-heartbeat-test.exe")}));
    EXPECT_TRUE(service.UpdateProfiles({}));
    service.Stop();
    EXPECT_TRUE(service.UpdateProfiles({Profile("nonexistent-heartbeat-test.exe")}));
    service.Stop();
}

TEST(WindowHeartbeatTest, SameThreadAndStaleIdentityCannotEnterSynchronousWindowDispatch) {
    const HWND hwnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    ASSERT_NE(hwnd, nullptr);
    heartbeat::Window window;
    window.handle = hwnd;
    window.threadId = GetWindowThreadProcessId(hwnd, &window.processId);
    EXPECT_EQ(heartbeat::SendAsyncNull(window, 1, nullptr), ERROR_RETRY);
    ++window.threadId;
    EXPECT_EQ(heartbeat::SendAsyncNull(window, 1, nullptr), ERROR_RETRY);
    DestroyWindow(hwnd);
    EXPECT_EQ(heartbeat::SendAsyncNull(window, 1, nullptr), ERROR_RETRY);
}

TEST(WindowHeartbeatTest, WorkerCannotActivateWindowsSimulateInputOrAccessGameMemory) {
    const std::string source = ce::test_source::ReadLogicalSource(
        std::filesystem::current_path() / "common/platform/window_heartbeat.cpp");
    ASSERT_FALSE(source.empty());
    for (const char* forbidden : {"SetForegroundWindow", "SetActiveWindow", "SetFocus", "SendInput",
                                  "SetWindowsHookEx", "CreateRemoteThread", "ReadProcessMemory",
                                  "WriteProcessMemory", "VirtualAllocEx", "WM_ACTIVATE", "AttachThreadInput"}) {
        EXPECT_EQ(source.find(forbidden), std::string::npos) << forbidden;
    }
}

thread_local HANDLE callbackEvent = nullptr;
thread_local ULONG_PTR completedToken = 0;

void CALLBACK Received(HWND, UINT message, ULONG_PTR token, LRESULT) {
    EXPECT_EQ(message, WM_NULL);
    completedToken = token;
    SetEvent(callbackEvent);
}

TEST(WindowHeartbeatTest, NativeNullDeliveryWakesAMessageWaitAndCompletesWithoutChangingFocus) {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE completion = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(ready, nullptr);
    ASSERT_NE(completion, nullptr);
    ASSERT_NE(stop, nullptr);
    heartbeat::Window window;
    std::thread receiver([&]() {
        window.handle = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        window.processId = GetCurrentProcessId();
        window.threadId = GetCurrentThreadId();
        SetEvent(ready);
        while (true) {
            const DWORD wait = MsgWaitForMultipleObjectsEx(1, &stop, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED)
                break;
            MSG message = {};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                DispatchMessageW(&message);
        }
        DestroyWindow(window.handle);
    });
    const DWORD readyResult = WaitForSingleObject(ready, 5000);
    EXPECT_EQ(readyResult, WAIT_OBJECT_0);
    if (readyResult == WAIT_OBJECT_0 && window.handle) {
        callbackEvent = completion;
        completedToken = 0;
        EXPECT_EQ(heartbeat::SendAsyncNull(window, 77, Received), ERROR_SUCCESS);
        const uint64_t deadline = GetTickCount64() + 5000;
        while (WaitForSingleObject(completion, 0) != WAIT_OBJECT_0 && GetTickCount64() < deadline) {
            MSG message = {};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                DispatchMessageW(&message);
            const uint64_t now = GetTickCount64();
            if (now < deadline)
                MsgWaitForMultipleObjectsEx(1, &completion, static_cast<DWORD>(deadline - now), QS_ALLINPUT,
                                            MWMO_INPUTAVAILABLE);
        }
        EXPECT_EQ(completedToken, 77u);
        EXPECT_NE(GetForegroundWindow(), window.handle);
    }
    SetEvent(stop);
    receiver.join();
    callbackEvent = nullptr;
    CloseHandle(completion);
    CloseHandle(ready);
    CloseHandle(stop);
}
}  // namespace
