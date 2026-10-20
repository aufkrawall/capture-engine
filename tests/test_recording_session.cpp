#include <gtest/gtest.h>

#include <string>
#include <functional>
#include <optional>
#include <vector>

// Exercise the product session implementation without the controller's UI/process globals.
// NOLINTNEXTLINE(bugprone-suspicious-include) - deliberately compile the actual controller owner
#include "runtime/recording/recording_session.cpp"
#include "runtime/children/host_children.h"
#include "runtime/core/recording_owner.h"

namespace {
using namespace ce::controller;

struct Effects final : RecordingEffects {
    std::vector<std::string> events;
    bool ready = true;
    bool inject = true;
    bool publication = true;
    CommandOutcome start = CommandOutcome::Accepted;
    CommandOutcome audio = CommandOutcome::Accepted;
    CommandOutcome stop = CommandOutcome::Accepted;
    int failures = 0;
    int lives = 0;
    int finalizing = 0;
    int stops = 0;
    int injectStops = 0;
    int mediaReleases = 0;
    int stopResults = 0;
    std::optional<CommandOutcome> injectStop;
    std::function<void()> onStop;
    int starts = 0;
    int automaticDisabled = 0;
    uint64_t pendingMs = 0;
    uint64_t liveMs = 0;
    bool exact = false;
    bool streamingFailure = false;
    bool deadMediaCleared = false;
    std::function<void()> onReady;

    void PrepareIdentity() override { events.emplace_back("identity"); }
    bool PublishIntent(RecordingStartIntent intent, const char*) override {
        events.emplace_back(intent == RecordingStartIntent::Idle ? "idle" : "intent");
        return publication;
    }
    bool EnsureMedia() override {
        events.emplace_back("ready");
        if (onReady)
            onReady();
        return ready;
    }
    void EnsureSensor() override { events.emplace_back("sensor"); }
    bool InjectConnected() const override { return inject; }
    CommandOutcome StartInject() override { ++starts; return start; }
    CommandOutcome StartAudioMedia() override { events.emplace_back("audio"); return audio; }
    CommandOutcome StopMedia(const char*, uint32_t) override {
        events.emplace_back("stop"); ++stops;
        if (onStop) onStop();
        return stop;
    }
    CommandOutcome StopInject(const char*, uint32_t) override {
        events.emplace_back("inject-stop"); ++injectStops;
        return injectStop.value_or(stop);
    }
    void ReleaseMedia() override { events.emplace_back("release-media"); ++mediaReleases; }
    void Notice(RecordingNotice notice, const RecordingSnapshot& snapshot, const char*, uint64_t elapsed,
                bool mediaExact) override {
        if (notice == RecordingNotice::StopResult) ++stopResults;
        if (notice == RecordingNotice::Requested)
            events.emplace_back(snapshot.requested ? "requested" : "released");
        if (notice == RecordingNotice::Failed) { ++failures; streamingFailure = snapshot.streaming; }
        if (notice == RecordingNotice::Finalizing) { ++finalizing; pendingMs = elapsed; }
        if (notice == RecordingNotice::Live) { ++lives; liveMs = elapsed; exact = mediaExact; }
    }
    void ClearMediaFailure(uint32_t, bool mediaGone) override { deadMediaCleared = mediaGone; }
    void DisableAutomaticRecording() override { ++automaticDisabled; }
};

class RecordingSessionTest : public testing::Test {
protected:
    Effects effects;
    RecordingSession session{effects};
    CommandOutcome Start(RecordingStartIntent intent = RecordingStartIntent::Video) {
        return session.Start(intent, true, 100, "test start");
    }
    RecordingObservation Observation() {
        RecordingObservation result;
        result.request = session.Snapshot().request;
        result.now = 500;
        return result;
    }
};
}  // namespace

TEST_F(RecordingSessionTest, AcceptedStartPublishesBeforeReadinessAndDoesNotClaimLive) {
    EXPECT_EQ(Start(), CommandOutcome::Accepted);
    EXPECT_EQ(effects.events, (std::vector<std::string>{"identity", "intent", "requested", "sensor", "ready"}));
    EXPECT_TRUE(session.Snapshot().requested);
    EXPECT_FALSE(session.Snapshot().observedLive);
    EXPECT_EQ(session.Snapshot().pendingSince, 100u);
    EXPECT_EQ(Start(), CommandOutcome::Rejected);
}

TEST_F(RecordingSessionTest, PendingStopClearsOwnershipBeforeChildAcceptanceAndAllowsImmediateRestart) {
    Start();
    effects.events.clear();
    EXPECT_EQ(session.Stop("stop", 250), CommandOutcome::Accepted);
    EXPECT_EQ(effects.events, (std::vector<std::string>{"idle", "released", "stop", "release-media"}));
    EXPECT_EQ(effects.pendingMs, 150u);
    EXPECT_FALSE(session.Snapshot().requested);
    EXPECT_EQ(session.Snapshot().pendingSince, 0u);
    EXPECT_EQ(session.Snapshot().pendingIntent, RecordingStartIntent::Idle);
    EXPECT_EQ(session.Stop("repeat", 251), CommandOutcome::Accepted);
    EXPECT_EQ(effects.finalizing, 1);
    EXPECT_EQ(effects.stops, 1);
    EXPECT_EQ(session.Start(RecordingStartIntent::AudioOnly, false, 300, "next"), CommandOutcome::Accepted);
    EXPECT_EQ(session.Snapshot().request, 2u);
}

TEST_F(RecordingSessionTest, UnknownAndRejectedStopsDoNotRestoreRequestedOwnership) {
    for (auto outcome : {CommandOutcome::AcknowledgementUnknown, CommandOutcome::Rejected}) {
        Start();
        effects.stop = outcome;
        EXPECT_EQ(session.Stop("stop", 150), outcome);
        EXPECT_FALSE(session.Snapshot().requested);
        EXPECT_EQ(session.Snapshot().lastStop, outcome);
    }
}

TEST_F(RecordingSessionTest, MediaStampCommitsLiveExactlyOnceAndStaleRequestsCannotConfirmNextStart) {
    Start();
    auto old = Observation();
    old.live = true;
    old.liveSince = 300;
    session.Observe(old);
    session.Observe(old);
    EXPECT_EQ(effects.lives, 1);
    EXPECT_EQ(effects.liveMs, 200u);
    EXPECT_TRUE(effects.exact);
    session.Stop("stop", 550);
    session.Start(RecordingStartIntent::Video, false, 600, "next");
    session.Observe(old);
    EXPECT_EQ(effects.lives, 1);
    auto staleStamp = Observation();
    staleStamp.live = true;
    staleStamp.liveSince = 300;
    session.Observe(staleStamp);
    EXPECT_EQ(effects.lives, 1);
    EXPECT_FALSE(session.Snapshot().observedLive);
}

TEST_F(RecordingSessionTest, MissingLiveStampUsesObservationWithoutClaimingExactTiming) {
    Start();
    auto observation = Observation();
    observation.live = true;
    session.Observe(observation);
    EXPECT_EQ(effects.liveMs, 400u);
    EXPECT_FALSE(effects.exact);
}

TEST_F(RecordingSessionTest, ReadinessAndStartFailuresRollbackAndPublishOneFailure) {
    effects.ready = false;
    EXPECT_EQ(Start(), CommandOutcome::Rejected);
    EXPECT_EQ(effects.failures, 1);
    EXPECT_TRUE(effects.streamingFailure);
    EXPECT_FALSE(session.Snapshot().requested);
    effects.ready = true;
    effects.start = CommandOutcome::AcknowledgementUnknown;
    EXPECT_EQ(Start(), CommandOutcome::AcknowledgementUnknown);
    EXPECT_EQ(effects.starts, 2);
    EXPECT_EQ(effects.failures, 2);
    EXPECT_EQ(session.Snapshot().pendingSince, 0u);
}

TEST_F(RecordingSessionTest, AudioOnlyCanStartWithoutInjectAndDoesNotUseStreamingOrSensors) {
    effects.inject = false;
    effects.publication = false;
    EXPECT_EQ(Start(RecordingStartIntent::AudioOnly), CommandOutcome::Accepted);
    EXPECT_FALSE(session.Snapshot().streaming);
    EXPECT_EQ(effects.starts, 0);
    EXPECT_EQ(effects.events, (std::vector<std::string>{"identity", "intent", "requested", "ready", "intent", "audio"}));
    EXPECT_EQ(session.Toggle(RecordingStartIntent::Video, false, 250, "toggle"), CommandOutcome::Accepted);
    EXPECT_EQ(effects.stops, 1);
}

TEST_F(RecordingSessionTest, ChildLossBeforeLiveAndIntegrityFailureAreReconciledOnce) {
    Start();
    auto observation = Observation();
    observation.injectAvailable = false;
    session.Observe(observation);
    session.Observe(observation);
    EXPECT_EQ(effects.failures, 1);
    EXPECT_EQ(effects.stops, 1);
    EXPECT_EQ(effects.automaticDisabled, 0);
    Start(RecordingStartIntent::AudioOnly);
    observation = Observation();
    observation.failure = 1;
    session.Observe(observation);
    session.Observe(observation);
    EXPECT_EQ(effects.failures, 2);
    EXPECT_EQ(effects.automaticDisabled, 1);
    EXPECT_FALSE(effects.streamingFailure);
}

TEST_F(RecordingSessionTest, MediaDeathAfterLiveClearsVisibleStateAndDisablesAutomaticRecording) {
    Start();
    auto observation = Observation();
    observation.live = true;
    observation.liveSince = 200;
    session.Observe(observation);
    observation.mediaAvailable = false;
    session.Observe(observation);
    EXPECT_TRUE(effects.deadMediaCleared);
    EXPECT_EQ(effects.failures, 1);
    EXPECT_EQ(effects.automaticDisabled, 1);
    EXPECT_EQ(effects.stops, 0);
    EXPECT_FALSE(session.Snapshot().requested);
}

TEST_F(RecordingSessionTest, ShutdownDisarmsPendingStartWithoutClaimingFinalization) {
    Start();
    session.Shutdown("shutdown");
    EXPECT_FALSE(session.Snapshot().requested);
    EXPECT_EQ(session.Snapshot().pendingSince, 0u);
    EXPECT_EQ(effects.finalizing, 0);
}

TEST_F(RecordingSessionTest, ReentrantStopDuringReadinessCancelsTheInFlightStartWithoutSendingStart) {
    effects.onReady = [&] {
        EXPECT_EQ(session.Start(RecordingStartIntent::AudioOnly, false, 120, "nested"), CommandOutcome::Rejected);
        EXPECT_EQ(session.Stop("cancel during readiness", 150), CommandOutcome::Accepted);
    };
    EXPECT_EQ(Start(), CommandOutcome::Rejected);
    EXPECT_FALSE(session.Snapshot().requested);
    EXPECT_EQ(effects.starts, 0);
    EXPECT_EQ(effects.stops, 1);
    EXPECT_EQ(effects.failures, 0);
    effects.onReady = {};
    EXPECT_EQ(Start(), CommandOutcome::Accepted);
}

TEST_F(RecordingSessionTest, MissingInjectAndAudioCommandFailuresDisarmEveryStart) {
    effects.inject = false;
    EXPECT_EQ(Start(), CommandOutcome::Rejected);
    EXPECT_FALSE(session.Snapshot().requested);
    effects.audio = CommandOutcome::Rejected;
    EXPECT_EQ(Start(RecordingStartIntent::AudioOnly), CommandOutcome::Rejected);
    EXPECT_EQ(effects.failures, 2);
    EXPECT_FALSE(effects.streamingFailure);
    EXPECT_EQ(session.Snapshot().pendingSince, 0u);
}

TEST_F(RecordingSessionTest, StopOwnerClassifiesEveryEndpointCombinationAndReleasesExactlyOnce) {
    for (auto media : {CommandOutcome::Accepted, CommandOutcome::Rejected, CommandOutcome::AcknowledgementUnknown}) {
        for (auto inject : {CommandOutcome::Accepted, CommandOutcome::Rejected, CommandOutcome::AcknowledgementUnknown}) {
            Start();
            effects.stop = media;
            effects.injectStop = inject;
            effects.events.clear();
            const int releases = effects.mediaReleases;
            const int results = effects.stopResults;
            effects.onStop = [&] {
                EXPECT_FALSE(session.Snapshot().requested);
                EXPECT_EQ(session.Snapshot().pendingIntent, RecordingStartIntent::Idle);
                EXPECT_EQ(session.Start(RecordingStartIntent::AudioOnly, false, 200, "reentry"), CommandOutcome::Rejected);
            };
            const auto expected = media == CommandOutcome::Accepted || inject == CommandOutcome::Accepted
                ? CommandOutcome::Accepted
                : media == CommandOutcome::AcknowledgementUnknown || inject == CommandOutcome::AcknowledgementUnknown
                    ? CommandOutcome::AcknowledgementUnknown : CommandOutcome::Rejected;
            EXPECT_EQ(session.Stop("stop", 250), expected);
            EXPECT_EQ(session.Snapshot().lastStop, expected);
            std::vector<std::string> order{"idle", "released", "stop"};
            if (media != CommandOutcome::Accepted) order.emplace_back("inject-stop");
            order.emplace_back("release-media");
            EXPECT_EQ(effects.events, order);
            EXPECT_EQ(effects.mediaReleases, releases + 1);
            EXPECT_EQ(effects.stopResults, results + 1);
            EXPECT_EQ(session.Stop("repeat", 251), CommandOutcome::Accepted);
            EXPECT_EQ(effects.mediaReleases, releases + 1);
            EXPECT_EQ(effects.stopResults, results + 1);
        }
    }
}

namespace {
// The engine-side consumers of recording transitions (desktop overlay, events) as a probe. Each call
// is one event, in the order the recording owner reports it.
struct ObserverProbe final : ce::engine::RecordingObserver {
    std::vector<std::string> events;
    void OnStartIntent(RecordingStartIntent intent) override {
        events.emplace_back(intent == RecordingStartIntent::Video       ? "intent-video"
                            : intent == RecordingStartIntent::AudioOnly ? "intent-audio"
                                                                        : "intent-idle");
    }
    void OnRequestedChanged(bool requested) override { events.emplace_back(requested ? "requested" : "released"); }
    void OnFinalizing() override { events.emplace_back("finalizing"); }
    void OnFailed(bool integrity) override { events.emplace_back(integrity ? "integrity-failure" : "failure"); }
};

struct FixedSettings final : ce::engine::SettingsSource {
    AppConfig config;
    const AppConfig& Current() const override { return config; }
    std::optional<AppConfig> Poll() override { return std::nullopt; }
    uint32_t WaitMs() const override { return 2000; }
};

uint64_t FixedNowMs() noexcept {
    return 100;
}
int64_t FixedNowUs() noexcept {
    return 100000;
}

// Helper children that exist only on paper: inject is connected, media never becomes ready, every
// shared-memory transaction is unavailable. Nothing spawns and nothing waits.
struct PaperChildren final : ce::runtime::HostChildren {
    bool Ensure(ce::runtime::HostChild child, uint32_t) override { return child != ce::runtime::HostChild::Media; }
    bool Ready(ce::runtime::HostChild child) const override { return child == ce::runtime::HostChild::Inject; }
    bool Present(ce::runtime::HostChild child) const override { return Ready(child); }
    bool Running(ce::runtime::HostChild child) const override { return Ready(child); }
    bool Send(ce::runtime::HostChild, ProcessCommand, const char*, ProcessResponse*, uint32_t) override {
        return false;
    }
    void SendToAll(ProcessCommand) override {}
    void RetireMedia(uint64_t) override {}
    std::vector<ce::runtime::FinalizedMedia> CollectFinalizedMedia() override { return {}; }
    uint32_t FinalizingCount() const override { return 0; }
    CommandOutcome StopRecording(ce::runtime::HostChild, uint32_t) override { return CommandOutcome::Accepted; }
    void Service(ce::runtime::AuxiliaryServices, const std::function<void()>&) override {}
    void ReconfigureServices(ce::runtime::AuxiliaryServices) override {}
    bool StopSensorsForSetup() override { return true; }
    void ResumeSensorsAfterSetup() override {}
    bool Shutdown() override { return true; }
    ce::ipc::ControlResult PublishRecordingIntent(RecordingStartIntent) override { return {}; }
    ce::ipc::ControlResult PublishNotification(OverlayNotificationType, uint64_t) override { return {}; }
    ce::ipc::ControlResult ReadRecordingHealth(ce::ipc::RecordingHealthObservation& observation) override {
        observation = {};
        return {};
    }
    void ClearMediaFailure(uint32_t, bool) override {}
    ce::ipc::InjectControlChannel InjectChannel() const override { return ce::ipc::InjectControlChannel(); }
};

// The real recording owner over paper children: media readiness fails, so every start rolls back.
class RecordingObserverTest : public testing::Test {
protected:
    ObserverProbe probe;
    FixedSettings settings;
    PaperChildren children;
    ce::engine::RecordingOwner owner{children, probe, settings, ce::engine::Clock{&FixedNowMs, &FixedNowUs}};
    RecordingEffects& effects = owner.EffectsForTesting();
};
}  // namespace

TEST_F(RecordingObserverTest, StartIntentReachesTheObserverBeforeMediaReadinessAndFailureReleasesIt) {
    EXPECT_EQ(owner.Start(RecordingStartIntent::Video, "test start"), CommandOutcome::Rejected);
    // Intent and ownership are reported first, in the order the overlay needs them; the rollback follows
    // only because the media readiness step, which comes after them, failed (a plain failure).
    EXPECT_EQ(probe.events,
              (std::vector<std::string>{"intent-video", "requested", "intent-idle", "released", "failure"}));
    EXPECT_FALSE(owner.Snapshot().requested);
}

TEST_F(RecordingObserverTest, AudioOnlyStartReportsItsOwnIntent) {
    EXPECT_EQ(owner.Start(RecordingStartIntent::AudioOnly, "test start"), CommandOutcome::Rejected);
    EXPECT_EQ(probe.events,
              (std::vector<std::string>{"intent-audio", "requested", "intent-idle", "released", "failure"}));
}

TEST_F(RecordingObserverTest, ShutdownAndStopOfAnIdleSessionReleaseOwnershipOnly) {
    owner.Shutdown("test shutdown");
    EXPECT_EQ(probe.events, (std::vector<std::string>{"intent-idle", "released"}));
    probe.events.clear();
    // Not active: no finalization is announced.
    EXPECT_TRUE(owner.Stop("test stop"));
    EXPECT_EQ(probe.events, (std::vector<std::string>{"intent-idle", "released"}));
}

TEST_F(RecordingObserverTest, FinalizingAndFailuresAreForwardedAtTheirEffectsSteps) {
    RecordingSnapshot snapshot;
    snapshot.requested = true;
    effects.Notice(RecordingNotice::Requested, snapshot, "test");
    effects.Notice(RecordingNotice::Finalizing, snapshot, "test", 120);
    effects.Notice(RecordingNotice::Failed, snapshot, "test");
    effects.DisableAutomaticRecording();
    snapshot.requested = false;
    effects.Notice(RecordingNotice::Requested, snapshot, "test");
    EXPECT_EQ(probe.events, (std::vector<std::string>{"requested", "finalizing", "failure", "integrity-failure",
                                                      "released"}));
}

TEST_F(RecordingObserverTest, NoticesWithoutAPresentationEffectDoNotReachTheObserver) {
    RecordingSnapshot snapshot;
    for (auto notice : {RecordingNotice::Clear, RecordingNotice::Live, RecordingNotice::StopResult})
        effects.Notice(notice, snapshot, "test", 10, true);
    EXPECT_TRUE(probe.events.empty());
}
