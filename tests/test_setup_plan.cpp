#include <gtest/gtest.h>
#include "captureengine/elevation/setup_worker.h"
#include "common/setup/startup_policy.h"
#include <windows.h>
#include <future>

using namespace ce::startup;

namespace {
Preferences FromBits(int bits) {
    return {(bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0};
}

AutostartTarget ClientTarget(const wchar_t* name = L"CeTestClient") {
    AutostartTarget target;
    target.executable = LR"(C:\Program Files\Client\client.exe)";
    target.arguments = L"--minimized";
    target.name = name;
    return target;
}
}  // namespace

TEST(SetupPlanTest, ElevationServiceIsIndependentOfAutostartAndTheElevationPreference) {
    for (int stored = 0; stored < 8; ++stored) {
        for (const bool install : {false, true}) {
            const Preferences old = FromBits(stored);
            const SetupPlan plan = PlanElevationService(old, install);
            EXPECT_EQ(plan.desired.service, install);
            EXPECT_EQ(plan.desired.elevated, old.elevated);
            EXPECT_EQ(plan.desired.autostart, old.autostart);
            EXPECT_EQ(plan.action, install ? Action::InstallService : Action::RemoveService);
            EXPECT_TRUE(plan.needsElevatedHelper);
            EXPECT_FALSE(plan.preferenceOnly);
        }
    }
}

TEST(SetupPlanTest, ApplicationAutostartKeepsTheServiceAndOnlyAnElevatedTaskNeedsTheHelper) {
    for (int stored = 0; stored < 8; ++stored) {
        for (const bool enable : {false, true}) {
            for (const bool administrator : {false, true}) {
                const Preferences old = FromBits(stored);
                const SetupPlan plan = PlanApplicationAutostart(old, enable, administrator);
                EXPECT_EQ(plan.desired.autostart, enable);
                EXPECT_EQ(plan.desired.service, old.service);
                EXPECT_EQ(plan.desired.elevated, old.elevated);
                EXPECT_EQ(plan.action, Action::Autostart);
                EXPECT_FALSE(plan.preferenceOnly);
                // An elevated task is only ever registered for an administrator account with the
                // elevation preference; every other registration is a per-user Run value.
                const bool elevatedInvolved = old.elevated && administrator && (old.autostart || enable);
                EXPECT_EQ(plan.needsElevatedHelper, elevatedInvolved);
            }
        }
    }
}

TEST(SetupPlanTest, ElevationPreferenceReachesTheOsOnlyWhileAutostartIsOn) {
    const Preferences off{true, false, false};
    const SetupPlan stored = PlanLaunchElevated(off, true, true);
    EXPECT_TRUE(stored.preferenceOnly);
    EXPECT_FALSE(stored.needsElevatedHelper);
    EXPECT_TRUE(stored.desired.elevated);
    EXPECT_TRUE(stored.desired.service);

    const Preferences on{false, false, true};
    const SetupPlan toTask = PlanLaunchElevated(on, true, true);
    EXPECT_FALSE(toTask.preferenceOnly);
    EXPECT_EQ(toTask.action, Action::Autostart);
    EXPECT_TRUE(toTask.needsElevatedHelper);
    EXPECT_FALSE(PlanLaunchElevated(on, true, false).needsElevatedHelper);  // standard account: still a Run value
    EXPECT_TRUE(PlanLaunchElevated(Preferences{false, true, true}, false, true).needsElevatedHelper);  // task removed
}

TEST(SetupPlanTest, ExecutionPicksPersistenceHelperOrInProcessExactlyOnce) {
    struct Calls {
        int persisted = 0;
        int launched = 0;
        int applied = 0;
    };
    const auto run = [](const SetupPlan& plan, bool elevated, bool persistOk, Calls& calls) {
        return ExecutePlan(
            plan, elevated, 29,
            [&](const Preferences&) {
                ++calls.persisted;
                return persistOk;
            },
            [&](const SetupPlan&) {
                ++calls.launched;
                return 1223ul;
            },
            [&](const SetupPlan&) {
                ++calls.applied;
                return 0ul;
            });
    };
    Calls calls;
    SetupPlan preferenceOnly;
    preferenceOnly.preferenceOnly = true;
    EXPECT_EQ(run(preferenceOnly, false, true, calls), 0);
    EXPECT_EQ(run(preferenceOnly, false, false, calls), 29);  // the persistence error, nothing else ran
    EXPECT_EQ(calls.persisted, 2);
    EXPECT_EQ(calls.launched + calls.applied, 0);

    calls = {};
    const SetupPlan service = PlanElevationService({}, true);
    EXPECT_EQ(run(service, false, true, calls), 1223);  // not elevated: the UAC helper does the work
    EXPECT_EQ(calls.launched, 1);
    EXPECT_EQ(calls.applied + calls.persisted, 0);
    calls = {};
    EXPECT_EQ(run(service, true, true, calls), 0);  // already elevated: in-process, no second prompt
    EXPECT_EQ(calls.applied, 1);
    EXPECT_EQ(calls.launched + calls.persisted, 0);

    calls = {};
    const SetupPlan plainAutostart = PlanApplicationAutostart({}, true, true);
    EXPECT_FALSE(plainAutostart.needsElevatedHelper);
    EXPECT_EQ(run(plainAutostart, false, true, calls), 0);  // no elevation needed: never prompts
    EXPECT_EQ(calls.applied, 1);
    EXPECT_EQ(calls.launched, 0);
}

TEST(SetupStatusTest, ServiceStateValuesAreTheOnesTheSetupStatusReports) {
    EXPECT_EQ(static_cast<int>(ElevationServiceState::Unknown), -1);
    EXPECT_EQ(static_cast<int>(ElevationServiceState::Absent), 0);
    EXPECT_EQ(static_cast<int>(ElevationServiceState::Installed), 1);
    EXPECT_EQ(static_cast<int>(ElevationServiceState::Running), 2);
}

TEST(SetupStatusTest, TrayServiceLabelsStayWhatTheMenuAlwaysShowed) {
    EXPECT_STREQ(ElevationServiceLabel(ElevationServiceState::Running), L"Use elevation service (running)");
    EXPECT_STREQ(ElevationServiceLabel(ElevationServiceState::Installed),
                 L"Use elevation service (installed, stopped)");
    EXPECT_STREQ(ElevationServiceLabel(ElevationServiceState::Absent), L"Use elevation service (not installed)");
    // A service this account cannot query reads as not installed, as it always did.
    EXPECT_STREQ(ElevationServiceLabel(ElevationServiceState::Unknown), L"Use elevation service (not installed)");
}

TEST(SetupStatusTest, AutostartClassificationReproducesTheOldMatchRules) {
    for (int bits = 0; bits < 16; ++bits) {
        const bool hasRun = (bits & 1) != 0;
        const bool runMatches = hasRun && (bits & 2) != 0;
        const bool hasTask = (bits & 4) != 0;
        const bool taskMatches = hasTask && (bits & 8) != 0;
        const AutostartState state = ClassifyAutostart(hasRun, runMatches, hasTask, taskMatches);
        SCOPED_TRACE(::testing::Message() << "run=" << hasRun << "/" << runMatches << " task=" << hasTask << "/"
                                          << taskMatches);
        // The three expectations the application's autostart check always applied.
        EXPECT_EQ(AutostartSatisfies(state, Registration::None), !hasTask && !hasRun);
        EXPECT_EQ(AutostartSatisfies(state, Registration::UserRun), !hasTask && hasRun && runMatches);
        EXPECT_EQ(AutostartSatisfies(state, Registration::ElevatedTask), hasTask && !hasRun && taskMatches);
    }
    EXPECT_FALSE(AutostartSatisfies(AutostartState::Unknown, Registration::None));
    EXPECT_FALSE(AutostartSatisfies(AutostartState::Mismatch, Registration::UserRun));
    EXPECT_EQ(ClassifyAutostart(true, true, true, true), AutostartState::Mismatch);  // both kinds: never "on"
}

TEST(SetupStatusTest, RepeatedAutostartRequestsNeitherPromptNorRewrite) {
    const Preferences stored{false, false, true};
    const SetupPlan on = PlanApplicationAutostart(stored, true, true);
    EXPECT_TRUE(AutostartPlanApplied(on, stored, AutostartState::UserRun, true));
    EXPECT_FALSE(AutostartPlanApplied(on, stored, AutostartState::Absent, true));    // registration was lost
    EXPECT_FALSE(AutostartPlanApplied(on, stored, AutostartState::Mismatch, true));  // stale path
    EXPECT_FALSE(AutostartPlanApplied(on, stored, AutostartState::Unknown, true));
    const Preferences cleared{false, false, false};
    const SetupPlan off = PlanApplicationAutostart(cleared, false, true);
    EXPECT_TRUE(AutostartPlanApplied(off, cleared, AutostartState::Absent, true));
    EXPECT_FALSE(AutostartPlanApplied(off, stored, AutostartState::Absent, true));  // the record still says on
    EXPECT_FALSE(AutostartPlanApplied(PlanElevationService(stored, true), stored, AutostartState::UserRun, true));
}

TEST(SetupStatusTest, PlainClientAutostartNeverRegistersAnElevatedTask) {
    for (const AutostartState state : {AutostartState::Unknown, AutostartState::Absent, AutostartState::UserRun,
                                       AutostartState::ElevatedTask, AutostartState::Mismatch}) {
        const auto on = PlainAutostartChange(state, true);
        const auto off = PlainAutostartChange(state, false);
        EXPECT_EQ(on.has_value(), state != AutostartState::UserRun);
        if (on)
            EXPECT_EQ(*on, Registration::UserRun);
        EXPECT_EQ(off.has_value(), state != AutostartState::Absent);
        if (off)
            EXPECT_EQ(*off, Registration::None);
    }
}

TEST(AutostartTargetTest, ValidatesNameShapeAndPathWithoutTouchingTheFilesystem) {
    EXPECT_TRUE(IsValidAutostartTarget(ClientTarget()));
    EXPECT_TRUE(IsValidAutostartTarget(AutostartTarget{LR"(\\server\share\client.exe)", L"", L"My-Client_1.2"}));
    EXPECT_TRUE(IsValidAutostartTarget(AutostartTarget{LR"(d:\x.exe)", L"", L"x"}));
    for (const wchar_t* name : {L"", L"has space", L"a/b", L"a\\b", L"a:b", L"quote\"", L"star*"})
        EXPECT_FALSE(IsValidAutostartTarget(ClientTarget(name))) << "name=" << name;
    EXPECT_FALSE(IsValidAutostartName(std::wstring(65, L'a')));
    EXPECT_TRUE(IsValidAutostartName(std::wstring(64, L'a')));

    for (const wchar_t* executable : {L"", L"client.exe", LR"(.\client.exe)", LR"(\client.exe)", LR"(C:client.exe)",
                                      LR"(C:\dir\"quoted".exe)", L"C:\\dir\\line\nbreak.exe"}) {
        AutostartTarget target = ClientTarget();
        target.executable = executable;
        EXPECT_FALSE(IsValidAutostartTarget(target)) << "exe=" << executable;
    }
    AutostartTarget breakInArguments = ClientTarget();
    breakInArguments.arguments = L"--a\r\n--b";
    EXPECT_FALSE(IsValidAutostartTarget(breakInArguments));
    AutostartTarget tooLong = ClientTarget();
    tooLong.arguments = std::wstring(32000, L'a');
    EXPECT_FALSE(IsValidAutostartTarget(tooLong));
}

TEST(SetupWorkerTest, AcceptsOneRequestAtATimeAndDeliversItsResultOnce) {
    SetupWorker worker;
    EXPECT_FALSE(worker.Busy());
    EXPECT_FALSE(worker.TakeResult().has_value());
    EXPECT_FALSE(worker.WaitForResult().has_value());

    std::promise<void> running;
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();
    ASSERT_TRUE(worker.Start([&running, gate] {
        running.set_value();
        gate.wait();
        return 5ul;
    }));
    running.get_future().wait();
    EXPECT_TRUE(worker.Busy());
    EXPECT_FALSE(worker.Start([] { return 9ul; }));  // rejected while in flight
    EXPECT_FALSE(worker.TakeResult().has_value());   // not finished: nothing to collect
    release.set_value();
    const std::optional<unsigned long> result = worker.WaitForResult();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 5ul);
    EXPECT_FALSE(worker.Busy());
    EXPECT_FALSE(worker.WaitForResult().has_value());  // delivered exactly once

    ASSERT_TRUE(worker.Start([] { return 6ul; }));  // idle again
    std::optional<unsigned long> polled;
    while (!(polled = worker.TakeResult()))
        std::this_thread::yield();
    EXPECT_EQ(*polled, 6ul);
    EXPECT_FALSE(worker.Busy());
    EXPECT_FALSE(worker.TakeResult().has_value());
}

TEST(SetupWorkerTest, ABrokenRequestReportsOutOfMemoryInsteadOfTerminating) {
    SetupWorker worker;
    ASSERT_TRUE(worker.Start([]() -> unsigned long { throw 1; }));
    const std::optional<unsigned long> result = worker.WaitForResult();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, static_cast<unsigned long>(ERROR_NOT_ENOUGH_MEMORY));
    EXPECT_FALSE(worker.Busy());
}

TEST(SetupWorkerTest, ShutdownJoinsTheRunningRequestAndDropsItsResult) {
    SetupWorker worker;
    std::promise<void> running;
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();
    std::atomic<bool> finished{false};
    ASSERT_TRUE(worker.Start([&running, gate, &finished] {
        running.set_value();
        gate.wait();
        finished.store(true);
        return 3ul;
    }));
    running.get_future().wait();
    release.set_value();
    worker.Shutdown();
    EXPECT_TRUE(finished.load());  // joined: the request ran to completion before Shutdown returned
    EXPECT_FALSE(worker.Busy());
    EXPECT_FALSE(worker.TakeResult().has_value());
    EXPECT_TRUE(worker.Start([] { return 1ul; }));  // usable again
    EXPECT_EQ(worker.WaitForResult().value_or(0), 1ul);
}
