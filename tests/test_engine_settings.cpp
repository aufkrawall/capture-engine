// The engine's settings behavior over the transactional store (llm-wiki/library/03 "Settings", 04 "runtime/settings"):
// external edits of a watched config.ini, their refusal, commits through the engine command, and the client-thread
// reader. A real engine thread runs over fake helper children; the store opens a config.ini in a temporary directory,
// never the repository's. Waits are on runtime events with generous bounds, never sleeps.
#include <gtest/gtest.h>

#include "engine_test_fakes.h"
#include "runtime/core/runtime_internal.h"
#include "runtime/core/store_settings_source.h"
#include "runtime/settings/settings_store.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace ce::engine;
using engine_test::ChildrenScript;
using engine_test::FakeChildren;
using engine_test::kWaitMs;
using ce::runtime::CommitStatus;
using ce::runtime::SettingsEdit;
using ce::runtime::SettingsStore;

// What one event says, copied out of the queue record.
struct Seen {
    int32_t type = 0;
    uint64_t request = 0;
    ce_status_t status = CE_OK;
    uint64_t revision = 0;
    int32_t source = 0;
    std::string message;
};

// Reads events until `match` accepts one (returned) or the bound passes; every event is released.
std::optional<Seen> WaitFor(Runtime& runtime, const std::function<bool(const ce_event_t&)>& match,
                            std::vector<Seen>* seen = nullptr) {
    for (;;) {
        const ce_event_t* event = nullptr;
        if (runtime.Events().Next(kWaitMs, &event) != CE_OK)
            return std::nullopt;
        Seen copy;
        copy.type = event->type;
        copy.request = event->request;
        copy.status = event->status;
        if (event->message)
            copy.message = event->message;
        if (event->type == CE_EVENT_SETTINGS_CHANGED) {
            copy.revision = event->data.settings.revision;
            copy.source = event->data.settings.source;
        }
        const bool matched = match(*event);
        runtime.Events().Release(event);
        if (seen)
            seen->push_back(copy);
        if (matched)
            return copy;
    }
}

bool IsRuntimeState(const ce_event_t& event, int32_t state) {
    return event.type == CE_EVENT_RUNTIME_STATE && event.data.runtime.state == state;
}

size_t CountType(const std::vector<Seen>& seen, int32_t type) {
    return static_cast<size_t>(std::count_if(seen.begin(), seen.end(), [type](const Seen& e) { return e.type == type; }));
}

// A directory under the temp path for one test, removed with everything in it.
class TempConfigDirectory {
public:
    TempConfigDirectory() {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        path_ = std::filesystem::temp_directory_path() /
                ("engine_settings." + std::to_string(GetCurrentProcessId()) + "." + info->name());
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempConfigDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempConfigDirectory(const TempConfigDirectory&) = delete;
    TempConfigDirectory& operator=(const TempConfigDirectory&) = delete;
    std::filesystem::path config() const { return path_ / "config.ini"; }

private:
    std::filesystem::path path_;
};

// An engine over fake children and the store of a temporary config.ini.
class EngineSettingsHarness {
public:
    explicit EngineSettingsHarness(const std::string& initialIni) {
        WriteConfig(initialIni);
        store_ = std::make_shared<SettingsStore>(directory_.config().wstring());
    }

    void WriteConfig(const std::string& text) {
        std::ofstream file(directory_.config(), std::ios::binary | std::ios::trunc);
        file << text;
    }

    std::string ReadConfig() const {
        std::ifstream file(directory_.config(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    std::unique_ptr<Runtime> Start(uint32_t features) {
        RuntimeParts parts;
        parts.options.features = features;
        parts.store = store_;
        auto script = children_;
        parts.makeChildren = [script](void (*)(), bool (*)()) {
            return std::unique_ptr<ce::runtime::HostChildren>(std::make_unique<FakeChildren>(script));
        };
        parts.settings = MakeStoreSettings(store_);
        ce_status_t status = CE_E_INTERNAL;
        std::unique_ptr<Runtime> runtime = RuntimeFactory::Assemble(std::move(parts), &status);
        EXPECT_EQ(status, CE_OK);
        return runtime;
    }

    const std::shared_ptr<SettingsStore>& store() const { return store_; }
    const std::shared_ptr<ChildrenScript>& children() const { return children_; }

private:
    TempConfigDirectory directory_;
    std::shared_ptr<ChildrenScript> children_ = std::make_shared<ChildrenScript>();
    std::shared_ptr<SettingsStore> store_;
};

// Admits a command that completes without touching the settings, and waits for its completion. Completions come from
// the loop iteration that also services the settings, so once this returns every settings change written before the
// call has been seen by the engine (applied, or refused).
void RoundTrip(Runtime& runtime, std::vector<Seen>& seen) {
    Command toggle;
    toggle.kind = CommandKind::ToggleOverlay;
    const Admission admission = runtime.Submit(toggle);
    ASSERT_EQ(admission.status, CE_OK);
    ASSERT_TRUE(WaitFor(
        runtime, [&](const ce_event_t& e) { return e.type == CE_EVENT_COMMAND_COMPLETED && e.request == admission.request; },
        &seen));
}

// Admits a settings commit and waits for its completion.
Seen Commit(Runtime& runtime, std::vector<SettingsEdit> edits, uint64_t baseRevision, std::vector<Seen>& seen) {
    Command commit;
    commit.kind = CommandKind::CommitSettings;
    commit.edits = std::move(edits);
    commit.baseRevision = baseRevision;
    const Admission admission = runtime.Submit(commit);
    EXPECT_EQ(admission.status, CE_OK);
    const auto completed = WaitFor(
        runtime, [&](const ce_event_t& e) { return e.type == CE_EVENT_COMMAND_COMPLETED && e.request == admission.request; },
        &seen);
    EXPECT_TRUE(completed.has_value());
    return completed.value_or(Seen{});
}

class EngineRuntimeSettingsTest : public ::testing::Test {};

TEST_F(EngineRuntimeSettingsTest, AWatchedExternalEditIsAppliedAndFannedOut) {
    EngineSettingsHarness harness("[Video]\r\nfps=60\r\n");
    auto runtime = harness.Start(CE_FEATURE_WATCH_SETTINGS);
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    const uint64_t openRevision = harness.store()->Revision();

    harness.WriteConfig("[Video]\r\nfps=75\r\n");
    const auto changed = WaitFor(*runtime, [](const ce_event_t& e) { return e.type == CE_EVENT_SETTINGS_CHANGED; });
    ASSERT_TRUE(changed);
    EXPECT_EQ(changed->source, CE_SETTINGS_SOURCE_FILE);
    EXPECT_EQ(changed->revision, openRevision + 1);
    EXPECT_EQ(runtime->Status().settingsRevision, openRevision + 1);
    EXPECT_EQ(harness.store()->Current()->video.fps, 75);
    const auto calls = harness.children()->Calls();
    EXPECT_NE(std::find(calls.begin(), calls.end(), "reconfigure"), calls.end());
    EXPECT_NE(std::find(calls.begin(), calls.end(),
                        "send-all-" + std::to_string(static_cast<int>(ProcessCommand::ReloadConfig))),
              calls.end());
}

TEST_F(EngineRuntimeSettingsTest, AnInvalidExternalEditIsRejectedAndThePublishedSettingsStay) {
    EngineSettingsHarness harness("[Video]\r\nfps=60\r\n");
    auto runtime = harness.Start(CE_FEATURE_WATCH_SETTINGS);
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    const auto before = harness.store()->Current();
    const uint64_t openRevision = harness.store()->Revision();

    harness.WriteConfig("[Capture]\r\ncapture_method=bogus_method\r\n");
    std::vector<Seen> seen;
    const auto rejected =
        WaitFor(*runtime, [](const ce_event_t& e) { return e.type == CE_EVENT_SETTINGS_REJECTED; }, &seen);
    ASSERT_TRUE(rejected);
    EXPECT_NE(rejected->message.find("[Capture]"), std::string::npos) << rejected->message;
    RoundTrip(*runtime, seen);
    EXPECT_EQ(CountType(seen, CE_EVENT_SETTINGS_CHANGED), 0u) << "a refused edit is not fanned out";
    EXPECT_EQ(CountType(seen, CE_EVENT_SETTINGS_REJECTED), 1u) << "and is announced once";
    EXPECT_EQ(harness.store()->Current(), before) << "the published snapshot is kept";
    EXPECT_EQ(runtime->Status().settingsRevision, openRevision);

    harness.WriteConfig("[Video]\r\nfps=80\r\n");
    const auto changed = WaitFor(*runtime, [](const ce_event_t& e) { return e.type == CE_EVENT_SETTINGS_CHANGED; });
    ASSERT_TRUE(changed) << "the next valid edit is applied";
    EXPECT_EQ(harness.store()->Current()->video.fps, 80);
}

TEST_F(EngineRuntimeSettingsTest, WithoutWatchSettingsAnExternalEditIsNotApplied) {
    EngineSettingsHarness harness("[Video]\r\nfps=60\r\n");
    auto runtime = harness.Start(0);
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    const auto before = harness.store()->Current();
    const uint64_t openRevision = harness.store()->Revision();

    harness.WriteConfig("[Video]\r\nfps=75\r\n");
    std::vector<Seen> seen;
    RoundTrip(*runtime, seen);
    RoundTrip(*runtime, seen);
    EXPECT_EQ(CountType(seen, CE_EVENT_SETTINGS_CHANGED), 0u);
    EXPECT_EQ(harness.store()->Current(), before);
    EXPECT_EQ(harness.store()->Revision(), openRevision);
    EXPECT_EQ(runtime->Status().settingsRevision, openRevision);
}

TEST_F(EngineRuntimeSettingsTest, ACommittedEditIsCompletedThenFannedOutWithTheAPISource) {
    EngineSettingsHarness harness("[Video]\r\nfps=60\r\n");
    auto runtime = harness.Start(CE_FEATURE_WATCH_SETTINGS);
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    const uint64_t openRevision = harness.store()->Revision();

    std::vector<Seen> seen;
    const Seen completed = Commit(*runtime, {SettingsEdit{"Video", "fps", std::string("90")}}, openRevision, seen);
    EXPECT_EQ(completed.status, CE_OK);
    const auto changed = WaitFor(*runtime, [](const ce_event_t& e) { return e.type == CE_EVENT_SETTINGS_CHANGED; }, &seen);
    ASSERT_TRUE(changed);
    EXPECT_EQ(changed->source, CE_SETTINGS_SOURCE_API);
    EXPECT_EQ(changed->revision, openRevision + 1);
    EXPECT_EQ(runtime->Status().settingsRevision, openRevision + 1);
    EXPECT_EQ(harness.store()->Current()->video.fps, 90);
    EXPECT_NE(harness.ReadConfig().find("fps=90"), std::string::npos) << "the edit is written to config.ini";

    const auto completedAt = std::find_if(seen.begin(), seen.end(), [](const Seen& e) { return e.type == CE_EVENT_COMMAND_COMPLETED; });
    const auto changedAt = std::find_if(seen.begin(), seen.end(), [](const Seen& e) { return e.type == CE_EVENT_SETTINGS_CHANGED; });
    ASSERT_NE(changedAt, seen.end());
    EXPECT_LT(completedAt - seen.begin(), changedAt - seen.begin()) << "COMMAND_COMPLETED comes before SETTINGS_CHANGED";

    const auto calls = harness.children()->Calls();
    const auto reconfigure = std::find(calls.begin(), calls.end(), "reconfigure");
    ASSERT_NE(reconfigure, calls.end());
    EXPECT_NE(std::find(reconfigure, calls.end(),
                        "send-all-" + std::to_string(static_cast<int>(ProcessCommand::ReloadConfig))),
              calls.end());
}

TEST_F(EngineRuntimeSettingsTest, AnInvalidCommitCompletesWithSettingsAndChangesNothing) {
    EngineSettingsHarness harness("[Video]\r\nfps=60\r\n");
    auto runtime = harness.Start(CE_FEATURE_WATCH_SETTINGS);
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    const auto before = harness.store()->Current();
    const uint64_t openRevision = harness.store()->Revision();
    const std::string fileBefore = harness.ReadConfig();

    std::vector<Seen> seen;
    const Seen completed =
        Commit(*runtime, {SettingsEdit{"Capture", "capture_method", std::string("bogus_method")}}, openRevision, seen);
    EXPECT_EQ(completed.status, CE_E_SETTINGS);
    EXPECT_NE(completed.message.find("[Capture]"), std::string::npos) << completed.message;
    RoundTrip(*runtime, seen);
    EXPECT_EQ(CountType(seen, CE_EVENT_SETTINGS_CHANGED), 0u);
    EXPECT_EQ(harness.store()->Current(), before);
    EXPECT_EQ(harness.store()->Revision(), openRevision);
    EXPECT_EQ(harness.ReadConfig(), fileBefore);
}

TEST_F(EngineRuntimeSettingsTest, ACommitAgainstAnOlderRevisionCompletesWithConflict) {
    EngineSettingsHarness harness("[Video]\r\nfps=60\r\n");
    auto runtime = harness.Start(CE_FEATURE_WATCH_SETTINGS);
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    const uint64_t openRevision = harness.store()->Revision();

    harness.WriteConfig("[Video]\r\nfps=75\r\n");
    std::vector<Seen> seen;
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return e.type == CE_EVENT_SETTINGS_CHANGED; }, &seen));
    const Seen completed = Commit(*runtime, {SettingsEdit{"Video", "fps", std::string("90")}}, openRevision, seen);
    EXPECT_EQ(completed.status, CE_E_CONFLICT);
    RoundTrip(*runtime, seen);
    EXPECT_EQ(CountType(seen, CE_EVENT_SETTINGS_CHANGED), 1u) << "only the external edit was fanned out";
    EXPECT_EQ(harness.store()->Current()->video.fps, 75);
}

TEST_F(EngineRuntimeSettingsTest, ACommitThatChangesNothingCompletesWithoutAFanOut) {
    EngineSettingsHarness harness("[Video]\r\nfps=60\r\n");
    auto runtime = harness.Start(CE_FEATURE_WATCH_SETTINGS);
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    const uint64_t openRevision = harness.store()->Revision();

    std::vector<Seen> seen;
    const Seen completed = Commit(*runtime, {SettingsEdit{"Video", "fps", std::string("60")}}, openRevision, seen);
    EXPECT_EQ(completed.status, CE_OK);
    RoundTrip(*runtime, seen);
    EXPECT_EQ(CountType(seen, CE_EVENT_SETTINGS_CHANGED), 0u);
    EXPECT_EQ(harness.store()->Revision(), openRevision);
}

TEST_F(EngineRuntimeSettingsTest, TheClientReaderSeesTheStoreAndChecksWithoutWriting) {
    EngineSettingsHarness harness("[Video]\r\nfps=60\r\n");
    auto runtime = harness.Start(CE_FEATURE_WATCH_SETTINGS);
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    const SettingsReader& settings = runtime->Settings();
    const uint64_t openRevision = settings.Revision();
    EXPECT_EQ(openRevision, harness.store()->Revision());
    EXPECT_EQ(settings.Value("Video", "fps"), std::optional<std::string>("60"));
    EXPECT_EQ(settings.Value("Video", "missing"), std::nullopt);

    EXPECT_EQ(settings.Check({SettingsEdit{"Video", "fps", std::string("90")}}, openRevision).status, CommitStatus::Committed);
    EXPECT_EQ(settings.Check({SettingsEdit{"Capture", "capture_method", std::string("bogus_method")}}, openRevision).status,
              CommitStatus::Invalid);
    EXPECT_EQ(settings.Revision(), openRevision);
    EXPECT_NE(harness.ReadConfig().find("fps=60"), std::string::npos) << "a check never writes";
}

}  // namespace
