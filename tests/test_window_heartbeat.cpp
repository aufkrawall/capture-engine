#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <thread>
#include "source_fragment_reader.h"
#include "common/platform/window_heartbeat.h"

namespace {
namespace heartbeat = ce::window_heartbeat;
namespace progress = ce::present_progress;

constexpr uint64_t kThresholdUs = 2'000'000;
constexpr DWORD kGame = 123;
constexpr DWORD kPresentThread = 7;
constexpr RECT kScreen = {0, 0, 1920, 1080};

ApplicationProfile Profile(const char* process = "game.exe", bool enabled = true) {
    ApplicationProfile profile;
    profile.target.pattern = process;
    profile.windowHeartbeatEnabled = enabled;
    return profile;
}

heartbeat::Window Candidate() {
    heartbeat::Window window;
    window.handle = reinterpret_cast<HWND>(static_cast<uintptr_t>(100));
    window.processId = kGame;
    window.threadId = 456;
    window.rect = kScreen;
    window.visible = true;
    window.cloaked = false;
    window.frame = true;
    return window;
}

bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

class FakeBackend final : public heartbeat::Backend {
public:
    DWORD foreground = 789;
    DWORD sendError = ERROR_SUCCESS;
    DWORD placeError = ERROR_SUCCESS;
    bool enumerationSucceeds = true;
    bool applyPlacements = true;  // the window thread runs every queued change before the next tick
    std::vector<heartbeat::Window> windows{Candidate()};
    std::map<DWORD, std::string> names{{kGame, "GAME.EXE"}, {321, "launcher.exe"}};
    std::map<DWORD, std::string> paths{{kGame, R"(C:\Games\Default\Game.exe)"}};
    std::vector<ULONG_PTR> tokens;
    std::vector<std::pair<HWND, RECT>> placements;
    int nameQueries = 0;
    int pathQueries = 0;

    DWORD ForegroundProcessId() override { return foreground; }
    bool Enumerate(std::vector<heartbeat::Window>& out) override {
        out = windows;
        return enumerationSucceeds;
    }
    std::string ProcessName(DWORD processId) override {
        ++nameQueries;
        return names[processId];
    }
    std::string ProcessPath(DWORD processId) override {
        ++pathQueries;
        return paths[processId];
    }
    DWORD SendNull(const heartbeat::Window&, ULONG_PTR token, SENDASYNCPROC) override {
        tokens.push_back(token);
        return sendError;
    }
    DWORD Place(const heartbeat::Window& window, const RECT& rect) override {
        if (placeError != ERROR_SUCCESS)
            return placeError;
        placements.emplace_back(window.handle, rect);
        if (applyPlacements) {
            for (auto& candidate : windows) {
                if (candidate.handle == window.handle)
                    candidate.rect = rect;
            }
        }
        return ERROR_SUCCESS;
    }
};

// A pump, its backend and the tracker on a 1 MHz clock (QPC ticks are microseconds).
struct Rig {
    heartbeat::Pump pump{999, kThresholdUs};
    FakeBackend backend;
    progress::Tracker tracker{1'000'000};
    uint64_t nowMs = 0;

    Rig() { pump.Configure({Profile()}); }
    heartbeat::TickResult Tick() {
        const auto result = pump.Tick(nowMs, static_cast<int64_t>(nowMs) * 1000, backend, tracker, nullptr);
        nowMs += heartbeat::kIntervalMs;
        return result;
    }
    // The game's render thread enters Present at `atMs` and never returns.
    void StallAt(uint64_t atMs) {
        tracker.Track({kGame});
        tracker.ObserveStart(kGame, kPresentThread, static_cast<int64_t>(atMs) * 1000 - 33'000, false);
        tracker.ObserveReturn(kGame, kPresentThread, static_cast<int64_t>(atMs) * 1000 - 32'000);
        tracker.ObserveStart(kGame, kPresentThread, static_cast<int64_t>(atMs) * 1000, true);
    }
    void PresentReturns() {
        tracker.ObserveReturn(kGame, kPresentThread, static_cast<int64_t>(nowMs) * 1000);
    }
    void AnswerProbe() { pump.Complete(backend.windows[0].handle, backend.tokens.back(), nowMs); }
};

TEST(WindowHeartbeatTest, CadenceIs250MsAndDelayedTicksNeverCatchUpInABurst) {
    Rig rig;
    EXPECT_EQ(rig.pump.WaitMs(0), 0u);
    EXPECT_TRUE(rig.pump.Tick(0, 0, rig.backend, rig.tracker, nullptr).ran);
    EXPECT_EQ(rig.pump.WaitMs(100), 150u);
    EXPECT_FALSE(rig.pump.Tick(249, 249'000, rig.backend, rig.tracker, nullptr).ran);
    EXPECT_TRUE(rig.pump.Tick(250, 250'000, rig.backend, rig.tracker, nullptr).ran);
    EXPECT_TRUE(rig.pump.Tick(10000, 10'000'000, rig.backend, rig.tracker, nullptr).ran);
    EXPECT_FALSE(rig.pump.Tick(10000, 10'000'000, rig.backend, rig.tracker, nullptr).ran);
}

TEST(WindowHeartbeatTest, PathProfileMatchesOnlyTheConfiguredInstallation) {
    Rig matched;
    matched.tracker.Track({kGame});
    matched.pump.Configure({Profile(R"(Modded\Game.exe)")});
    matched.backend.paths[kGame] = R"(D:\Games\Modded\Game.exe)";
    EXPECT_EQ(matched.Tick().background, std::vector<DWORD>{kGame});

    Rig unmatched;
    unmatched.tracker.Track({kGame});
    unmatched.pump.Configure({Profile(R"(Modded\Game.exe)")});
    unmatched.backend.paths[kGame] = R"(D:\Games\Default\Game.exe)";
    EXPECT_TRUE(unmatched.Tick().background.empty());
}

TEST(WindowHeartbeatTest, MoreSpecificDisabledProfileShadowsEnabledNameProfile) {
    Rig rig;
    rig.tracker.Track({kGame});
    ApplicationProfile generic = Profile("Game.exe");
    ApplicationProfile disabled = Profile(R"(Modded\Game.exe)");
    disabled.windowHeartbeatEnabled = false;
    rig.pump.Configure({generic, disabled});
    rig.backend.paths[kGame] = R"(D:\Games\Modded\Game.exe)";
    EXPECT_TRUE(rig.Tick().background.empty());

    Rig fallback;
    fallback.tracker.Track({kGame});
    fallback.pump.Configure({generic, disabled});
    fallback.backend.paths[kGame] = R"(D:\Games\Default\Game.exe)";
    EXPECT_EQ(fallback.Tick().background, std::vector<DWORD>{kGame});
}

TEST(WindowHeartbeatTest, ReloadedPathProfileAppliesToAnAlreadyEnumeratedProcess) {
    Rig rig;
    ASSERT_EQ(rig.Tick().background, std::vector<DWORD>{kGame});
    EXPECT_EQ(rig.backend.pathQueries, 0);
    rig.pump.Configure({Profile("GAME.EXE"), Profile(R"(Default\Game.exe)", false)});
    EXPECT_TRUE(rig.Tick().background.empty());
    EXPECT_EQ(rig.backend.pathQueries, 1);
}

TEST(WindowHeartbeatTest, AnUnresolvedPathIsRetriedAndResolvedPathsStayCached) {
    Rig rig;
    rig.pump.Configure({Profile(R"(Default\Game.exe)")});
    rig.backend.paths[kGame].clear();
    EXPECT_TRUE(rig.Tick().background.empty());
    rig.backend.paths[kGame] = R"(C:\Games\Default\Game.exe)";
    EXPECT_EQ(rig.Tick().background, std::vector<DWORD>{kGame});
    EXPECT_EQ(rig.backend.pathQueries, 2);
    rig.Tick();
    EXPECT_EQ(rig.backend.pathQueries, 2);
}

TEST(WindowHeartbeatTest, DisablingOrRemovingAProfileRestoresItsPendingNudge) {
    for (const bool keepOtherProfile : {false, true}) {
        Rig rig;
        rig.nowMs = 10'000;
        rig.StallAt(8'000);
        ASSERT_EQ(rig.Tick().episodes, 1u);
        if (keepOtherProfile)
            rig.pump.Configure({Profile("other.exe")});
        else
            rig.pump.Configure({});
        EXPECT_TRUE(rig.pump.Enabled()) << "pending restoration keeps cleanup active";
        EXPECT_EQ(rig.Tick().episodes, 0u);
        EXPECT_TRUE(SameRect(rig.backend.windows[0].rect, kScreen));
        EXPECT_EQ(rig.backend.placements.size(), 2u);
        EXPECT_EQ(rig.pump.Enabled(), keepOtherProfile);
    }
}

TEST(WindowHeartbeatTest, ShutdownQueuesTheRestoreAfterANudgeThatHasNotRunYet) {
    Rig rig;
    rig.backend.applyPlacements = false;
    rig.nowMs = 10'000;
    rig.StallAt(8'000);
    ASSERT_EQ(rig.Tick().episodes, 1u);
    rig.pump.Stop(rig.backend);
    ASSERT_EQ(rig.backend.placements.size(), 2u);
    EXPECT_TRUE(SameRect(rig.backend.placements.back().second, kScreen));
    EXPECT_FALSE(rig.pump.Enabled());
    rig.pump.Stop(rig.backend);
    EXPECT_EQ(rig.backend.placements.size(), 2u);
}

TEST(WindowHeartbeatTest, DisablingDuringTheResizeRestoresTheOriginalSize) {
    Rig rig;
    rig.nowMs = 10'000;
    rig.StallAt(8'000);
    for (int tick = 0; tick < 4; ++tick)
        rig.Tick();
    ASSERT_EQ(rig.backend.placements.size(), 3u);
    ASSERT_EQ(rig.backend.windows[0].rect.right, kScreen.right - 1);
    rig.pump.Configure({});
    EXPECT_EQ(rig.Tick().episodes, 0u);
    EXPECT_TRUE(SameRect(rig.backend.windows[0].rect, kScreen));
}

TEST(WindowHeartbeatTest, CancellationNeverOverwritesForeignGeometryOrAReusedWindow) {
    for (const bool reusedWindow : {false, true}) {
        Rig rig;
        rig.nowMs = 10'000;
        rig.StallAt(8'000);
        rig.Tick();
        if (reusedWindow)
            ++rig.backend.windows[0].threadId;
        else
            rig.backend.windows[0].rect = {100, 100, 1380, 820};
        rig.pump.Stop(rig.backend);
        EXPECT_EQ(rig.backend.placements.size(), 1u);
    }
}

TEST(WindowHeartbeatTest, ABackgroundGameThatPresentsOrPausesIsWatchedButNeverTouched) {
    Rig rig;
    rig.tracker.Track({kGame});
    for (int frame = 0; frame < 30; ++frame) {
        rig.tracker.ObserveStart(kGame, kPresentThread, frame * 33'333, false);
        rig.tracker.ObserveReturn(kGame, kPresentThread, frame * 33'333 + 400);
    }
    for (int tick = 0; tick < 240; ++tick) {
        const auto result = rig.Tick();
        ASSERT_TRUE(result.enumerated);
        ASSERT_EQ(result.background, std::vector<DWORD>{kGame});
        EXPECT_EQ(result.episodes, 0u);
    }
    EXPECT_TRUE(rig.backend.placements.empty());
    EXPECT_TRUE(rig.backend.tokens.empty());
}

TEST(WindowHeartbeatTest, AStallMovesTheWindowOnePixelRestoresItAndEndsWhenPresentReturns) {
    Rig rig;
    rig.nowMs = 10'000;
    rig.StallAt(10'000 - 2'000);
    auto result = rig.Tick();  // detection: move queued, probe sent
    ASSERT_EQ(result.episodes, 1u);
    ASSERT_EQ(rig.backend.placements.size(), 1u);
    EXPECT_TRUE(SameRect(rig.backend.placements[0].second, RECT{1, 0, 1921, 1080}));
    EXPECT_EQ(rig.backend.tokens.size(), 1u);
    rig.Tick();  // the move was applied: restore queued
    ASSERT_EQ(rig.backend.placements.size(), 2u);
    EXPECT_TRUE(SameRect(rig.backend.placements[1].second, kScreen));
    rig.Tick();  // restored: observing
    rig.PresentReturns();
    result = rig.Tick();
    EXPECT_EQ(result.episodes, 0u);
    EXPECT_EQ(rig.backend.placements.size(), 2u) << "no resize once the move helped";
    EXPECT_TRUE(SameRect(rig.backend.windows[0].rect, kScreen));
}

TEST(WindowHeartbeatTest, AMoveThatDoesNotHelpEscalatesToOneResizeAndThenGivesUpForThatStall) {
    Rig rig;
    rig.nowMs = 10'000;
    rig.StallAt(8'000);
    for (int tick = 0; tick < 40; ++tick)
        rig.Tick();
    ASSERT_EQ(rig.backend.placements.size(), 4u);
    EXPECT_TRUE(SameRect(rig.backend.placements[0].second, RECT{1, 0, 1921, 1080}));
    EXPECT_TRUE(SameRect(rig.backend.placements[1].second, kScreen));
    EXPECT_TRUE(SameRect(rig.backend.placements[2].second, RECT{0, 0, 1919, 1080}));
    EXPECT_TRUE(SameRect(rig.backend.placements[3].second, kScreen));
    EXPECT_EQ(rig.Tick().episodes, 1u) << "the attempt stays until the stall clears";
    rig.PresentReturns();
    EXPECT_EQ(rig.Tick().episodes, 0u);
    // A later stall is a new attempt.
    rig.StallAt(rig.nowMs);
    rig.nowMs += 2'000;
    EXPECT_EQ(rig.Tick().episodes, 1u);
    EXPECT_EQ(rig.backend.placements.size(), 5u);
}

TEST(WindowHeartbeatTest, ANudgeTheWindowIgnoresIsJudgedOnlyAfterItsThreadAnswered) {
    Rig rig;
    rig.backend.applyPlacements = false;
    rig.nowMs = 10'000;
    rig.StallAt(8'000);
    rig.Tick();
    ASSERT_EQ(rig.backend.placements.size(), 1u);
    for (int tick = 0; tick < 20; ++tick)
        rig.Tick();
    EXPECT_EQ(rig.backend.placements.size(), 1u) << "a thread that pumps nothing cannot have refused the move";
    rig.AnswerProbe();
    rig.Tick();  // answered and a tick passed: the move left no trace
    rig.Tick();  // observed one tick without a return: resize
    ASSERT_EQ(rig.backend.placements.size(), 2u);
    EXPECT_TRUE(SameRect(rig.backend.placements[1].second, RECT{0, 0, 1919, 1080}));
    EXPECT_EQ(rig.backend.tokens.size(), 2u) << "the resize gets its own probe";
}

TEST(WindowHeartbeatTest, TheForegroundWindowIsNeverNudgedAndActivationRestoresAndEndsAnAttempt) {
    Rig rig;
    rig.backend.foreground = kGame;
    rig.nowMs = 10'000;
    rig.StallAt(8'000);
    auto result = rig.Tick();
    EXPECT_TRUE(result.background.empty());
    EXPECT_EQ(result.episodes, 0u);
    EXPECT_TRUE(rig.backend.placements.empty());

    rig.backend.foreground = 789;
    rig.StallAt(rig.nowMs - 2'000);
    ASSERT_EQ(rig.Tick().episodes, 1u);
    ASSERT_EQ(rig.backend.placements.size(), 1u);
    rig.backend.foreground = kGame;  // the user switches to the game while it is moved
    result = rig.Tick();
    EXPECT_EQ(result.episodes, 0u);
    ASSERT_EQ(rig.backend.placements.size(), 2u);
    EXPECT_TRUE(SameRect(rig.backend.placements[1].second, kScreen));
}

TEST(WindowHeartbeatTest, OwnHiddenMinimizedCloakedChildAndUnprofiledWindowsAreNeverWatched) {
    const auto check = [](const heartbeat::Window& window, DWORD foreground, DWORD owner) {
        heartbeat::Pump pump(owner, kThresholdUs);
        FakeBackend backend;
        progress::Tracker tracker(1'000'000);
        backend.windows = {window};
        backend.foreground = foreground;
        pump.Configure({Profile()});
        tracker.Track({kGame});
        tracker.ObserveReturn(kGame, kPresentThread, 0);
        tracker.ObserveStart(kGame, kPresentThread, 1, false);
        const auto result = pump.Tick(60'000, 60'000'000, backend, tracker, nullptr);
        EXPECT_TRUE(result.background.empty());
        EXPECT_TRUE(backend.placements.empty());
        EXPECT_TRUE(backend.tokens.empty());
    };
    const auto base = Candidate();
    check(base, base.processId, 999);
    check(base, 789, base.processId);
    check(base, 0, 999);  // unknown foreground fails closed
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
    window.frame = false;
    check(window, 789, 999);
    window = base;
    window.rect = {};
    check(window, 789, 999);
    window = base;
    window.processId = 321;
    check(window, 789, 999);
    window = base;
    window.handle = nullptr;
    check(window, 789, 999);
}

TEST(WindowHeartbeatTest, TheLargestFrameWindowOfTheGameIsTheOneNudged) {
    Rig rig;
    auto splash = Candidate();
    splash.handle = reinterpret_cast<HWND>(static_cast<uintptr_t>(200));
    splash.rect = {700, 400, 1100, 700};
    rig.backend.windows.insert(rig.backend.windows.begin(), splash);
    rig.nowMs = 10'000;
    rig.StallAt(8'000);
    rig.Tick();
    ASSERT_EQ(rig.backend.placements.size(), 1u);
    EXPECT_EQ(rig.backend.placements[0].first, Candidate().handle);
}

TEST(WindowHeartbeatTest, ARefusedPlacementGivesUpWithoutRetryingEveryTick) {
    Rig rig;
    rig.backend.placeError = ERROR_ACCESS_DENIED;
    rig.nowMs = 10'000;
    rig.StallAt(8'000);
    const auto result = rig.Tick();
    EXPECT_EQ(result.failed, 1u);
    EXPECT_EQ(result.lastError, static_cast<DWORD>(ERROR_ACCESS_DENIED));
    for (int tick = 0; tick < 20; ++tick)
        EXPECT_EQ(rig.Tick().failed, 0u);
    EXPECT_TRUE(rig.backend.tokens.empty());
}

TEST(WindowHeartbeatTest, SomeoneElseResizingTheWindowEndsTheAttemptWithoutARestore) {
    Rig rig;
    rig.nowMs = 10'000;
    rig.StallAt(8'000);
    rig.Tick();
    rig.backend.windows[0].rect = {100, 100, 1380, 820};
    EXPECT_EQ(rig.Tick().episodes, 0u);
    EXPECT_EQ(rig.backend.placements.size(), 1u);
}

TEST(WindowHeartbeatTest, ProcessNamesAreQueriedOncePerProcessAndEnumerationFailureKeepsTheTrace) {
    Rig rig;
    for (int tick = 0; tick < 10; ++tick)
        rig.Tick();
    EXPECT_EQ(rig.backend.nameQueries, 1);
    rig.backend.enumerationSucceeds = false;
    const auto failure = rig.Tick();
    EXPECT_FALSE(failure.enumerated);
    EXPECT_EQ(failure.failed, 1u);
    rig.backend.enumerationSucceeds = true;
    EXPECT_TRUE(rig.Tick().enumerated);
}

TEST(WindowHeartbeatTest, AStaleProbeCompletionCannotStandInForTheWindowsAnswer) {
    Rig rig;
    rig.backend.applyPlacements = false;
    rig.nowMs = 10'000;
    rig.StallAt(8'000);
    rig.Tick();
    rig.pump.Complete(rig.backend.windows[0].handle, rig.backend.tokens.back() + 1, rig.nowMs);
    for (int tick = 0; tick < 4; ++tick)
        rig.Tick();
    EXPECT_EQ(rig.backend.placements.size(), 1u);
}

class FakeFeed final : public progress::Feed {
public:
    bool accept = true;
    bool running = false;
    int starts = 0;
    int stops = 0;
    bool Start(progress::Tracker&) override {
        ++starts;
        running = accept;
        return accept;
    }
    void Stop() override {
        ++stops;
        running = false;
    }
    bool Running() const override { return running; }
};

TEST(WindowHeartbeatTest, TheTraceRunsOnlyWhileAProfiledWindowIsInTheBackground) {
    FakeFeed feed;
    progress::Tracker tracker(1'000'000);
    heartbeat::FeedControl control(&feed);
    control.Steer({}, tracker);
    EXPECT_EQ(feed.starts, 0);
    control.Steer({kGame}, tracker);
    control.Steer({kGame}, tracker);
    EXPECT_EQ(feed.starts, 1);
    EXPECT_TRUE(control.Live());
    control.Steer({}, tracker);
    EXPECT_EQ(feed.stops, 1);
    EXPECT_FALSE(control.Live());
}

TEST(WindowHeartbeatTest, ARefusedTraceIsRetriedOnlyWhenTheBackgroundSetChangesOrAfterItEnded) {
    FakeFeed feed;
    feed.accept = false;
    progress::Tracker tracker(1'000'000);
    heartbeat::FeedControl control(&feed);
    for (int tick = 0; tick < 10; ++tick)
        control.Steer({kGame}, tracker);
    EXPECT_EQ(feed.starts, 1);
    control.Steer({kGame, 456}, tracker);
    EXPECT_EQ(feed.starts, 2);
    feed.accept = true;
    control.Steer({kGame}, tracker);
    EXPECT_EQ(feed.starts, 3);
    feed.running = false;  // the service restarted under a running trace
    control.Steer({kGame}, tracker);
    EXPECT_EQ(feed.starts, 4);
    EXPECT_TRUE(control.Live());
}

TEST(WindowHeartbeatTest, WorkerCanDisableStopAndRestartWithoutWaitingForTargetMessages) {
    heartbeat::Service service(std::make_unique<FakeFeed>());
    EXPECT_TRUE(service.UpdateProfiles({}));
    EXPECT_TRUE(service.UpdateProfiles({Profile("nonexistent-heartbeat-test.exe")}));
    EXPECT_TRUE(service.UpdateProfiles({}));
    service.Stop();
    EXPECT_TRUE(service.UpdateProfiles({Profile("nonexistent-heartbeat-test.exe")}));
    service.Stop();
}

TEST(WindowHeartbeatTest, TheStallThresholdIsTheGraphicsKernelsGpuTimeout) {
    EXPECT_GE(heartbeat::GpuTimeoutUs(), 1'000'000u);
}

TEST(WindowHeartbeatTest, SameThreadAndStaleIdentityCannotEnterSynchronousWindowDispatch) {
    const HWND hwnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    ASSERT_NE(hwnd, nullptr);
    heartbeat::Window window;
    window.handle = hwnd;
    window.threadId = GetWindowThreadProcessId(hwnd, &window.processId);
    EXPECT_EQ(heartbeat::SendAsyncNull(window, 1, nullptr, GetCurrentProcessId() + 1), ERROR_RETRY);
    EXPECT_EQ(heartbeat::PlaceAsync(window, kScreen), ERROR_RETRY);
    ++window.threadId;
    EXPECT_EQ(heartbeat::SendAsyncNull(window, 1, nullptr, GetCurrentProcessId() + 1), ERROR_RETRY);
    EXPECT_EQ(heartbeat::PlaceAsync(window, kScreen), ERROR_RETRY);
    DestroyWindow(hwnd);
    EXPECT_EQ(heartbeat::SendAsyncNull(window, 1, nullptr, GetCurrentProcessId() + 1), ERROR_RETRY);
    EXPECT_EQ(heartbeat::PlaceAsync(window, kScreen), ERROR_RETRY);
}

TEST(WindowHeartbeatTest, WorkerCannotActivateWindowsSimulateInputOrAccessGameMemory) {
    for (const char* file : {"common/platform/window_heartbeat.cpp", "common/platform/window_heartbeat_recovery.cpp",
                             "common/platform/present_progress.cpp"}) {
        const std::string source = ce::test_source::ReadLogicalSource(std::filesystem::current_path() / file);
        ASSERT_FALSE(source.empty()) << file;
        for (const char* forbidden : {"SetForegroundWindow", "SetActiveWindow", "SetFocus", "SendInput",
                                      "SetWindowsHookEx", "CreateRemoteThread", "ReadProcessMemory",
                                      "WriteProcessMemory", "VirtualAllocEx", "WM_ACTIVATE", "AttachThreadInput",
                                      "PROCESS_VM_", "PROCESS_ALL_ACCESS"}) {
            EXPECT_EQ(source.find(forbidden), std::string::npos) << file << ": " << forbidden;
        }
    }
}

// A window on its own thread, pumping messages until `stop`, reporting each applied geometry change.
struct Receiver {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    heartbeat::Window window;
    std::thread thread;

    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_WINDOWPOSCHANGED) {
            if (auto* self = reinterpret_cast<Receiver*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)))
                SetEvent(self->changed);
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    Receiver() {
        thread = std::thread([this]() {
            WNDCLASSW type = {};
            type.lpfnWndProc = Proc;
            type.hInstance = GetModuleHandleW(nullptr);
            type.lpszClassName = L"CEWindowHeartbeatTestReceiver";
            RegisterClassW(&type);
            // Hidden popup: a real top-level window that can be placed, without ever showing.
            window.handle = CreateWindowExW(WS_EX_TOOLWINDOW, type.lpszClassName, L"", WS_POPUP, 10, 20, 300, 200,
                                            nullptr, nullptr, type.hInstance, nullptr);
            if (window.handle)
                SetWindowLongPtrW(window.handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
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
            if (window.handle)
                DestroyWindow(window.handle);
        });
    }
    ~Receiver() {
        SetEvent(stop);
        thread.join();
        CloseHandle(ready);
        CloseHandle(changed);
        CloseHandle(stop);
    }
};

TEST(WindowHeartbeatTest, NativePlacementRunsOnTheWindowsOwnThreadWithoutActivatingIt) {
    Receiver receiver;
    ASSERT_EQ(WaitForSingleObject(receiver.ready, 5000), WAIT_OBJECT_0);
    ASSERT_NE(receiver.window.handle, nullptr);
    const HWND foreground = GetForegroundWindow();
    const RECT moved = {11, 20, 311, 220};
    ASSERT_EQ(heartbeat::PlaceAsync(receiver.window, moved), ERROR_SUCCESS);
    ASSERT_EQ(WaitForSingleObject(receiver.changed, 5000), WAIT_OBJECT_0);
    RECT rect = {};
    ASSERT_TRUE(GetWindowRect(receiver.window.handle, &rect));
    EXPECT_TRUE(SameRect(rect, moved));
    EXPECT_FALSE(IsWindowVisible(receiver.window.handle));
    EXPECT_EQ(GetForegroundWindow(), foreground);
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
        EXPECT_EQ(heartbeat::SendAsyncNull(window, 77, Received, GetCurrentProcessId() + 1), ERROR_SUCCESS);
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
