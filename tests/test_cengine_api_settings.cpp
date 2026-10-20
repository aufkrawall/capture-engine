// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// The settings functions of the C ABI (llm-wiki/library/03 "Settings"), called exactly as a client calls them. The
// engine is assembled from fakes through the test seam (runtime/api/capi_testing.h), but its settings are a real store
// over a config.ini in a temporary directory. Every wait is on a runtime event or a Win32 event with a generous bound;
// nothing sleeps.
#include "include/cengine/cengine.h"

#include "engine_test_fakes.h"
#include "runtime/api/capi_testing.h"
#include "runtime/api/runtime_desc.h"
#include "runtime/core/runtime_internal.h"
#include "runtime/core/store_settings_source.h"
#include "runtime/settings/settings_store.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
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
using ce::runtime::SettingsStore;
namespace testing_seam = ce::capi::testing;

constexpr int kMaxEventReads = 4096;  // bound on one WaitFor: a runaway event stream fails the test, not hangs it

// One ce_event_t, copied out so the test may read it after the release.
struct Seen {
    int32_t type = 0;
    uint64_t request = 0;
    ce_status_t status = CE_OK;
    std::string message;
    uint64_t revision = 0;
    int32_t source = 0;
};

// Reads and releases events until `match` accepts one (the copy is returned) or the bound passes.
std::optional<Seen> WaitFor(ce_runtime_t* rt, const std::function<bool(const ce_event_t&)>& match,
                            std::vector<Seen>* seen = nullptr) {
    for (int reads = 0; reads < kMaxEventReads; ++reads) {
        const ce_event_t* event = nullptr;
        if (ce_runtime_next_event(rt, kWaitMs, &event) != CE_OK || !event)
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
        EXPECT_EQ(ce_runtime_release_event(rt, event), CE_OK);
        if (seen)
            seen->push_back(copy);
        if (matched)
            return copy;
    }
    return std::nullopt;
}

bool IsRuntimeState(const ce_event_t& event, int32_t state) {
    return event.type == CE_EVENT_RUNTIME_STATE && event.data.runtime.state == state;
}

bool IsCompletion(const ce_event_t& event, ce_request_t request) {
    return event.type == CE_EVENT_COMMAND_COMPLETED && event.request == request;
}

// A directory under the temp path for one test, with its config.ini, removed with everything in it.
class TempConfigDirectory {
public:
    TempConfigDirectory() {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        path_ = std::filesystem::temp_directory_path() /
                ("cengine_api_settings." + std::to_string(GetCurrentProcessId()) + "." + info->name());
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

// The fixture: a config.ini with [Video] fps=60, a store on it, and an engine factory behind the seam that runs the
// engine over that store. TearDown opens gates and destroys a runtime the test left behind.
class CengineApiSettingsTest : public ::testing::Test {
protected:
    void SetUp() override {
        WriteConfig("[Video]\r\nfps=60\r\n");
        store_ = std::make_shared<SettingsStore>(directory_.config().wstring());
        testing_seam::SetRuntimeFactory([this](const RuntimeOptions& options, ce_status_t* status) {
            RuntimeParts parts;
            parts.options = options;
            parts.store = store_;
            auto script = children_;
            parts.makeChildren = [script](void (*)(), bool (*)()) {
                return std::unique_ptr<ce::runtime::HostChildren>(std::make_unique<FakeChildren>(script));
            };
            parts.settings = MakeStoreSettings(store_);
            return RuntimeFactory::Assemble(std::move(parts), status);
        });
    }

    void TearDown() override {
        // Open the gate first: a held helper would otherwise hold the engine (and this test) for the whole bound.
        if (children_->injectGate)
            SetEvent(children_->injectGate);
        if (rt_) {
            ce_runtime_request_shutdown(rt_);
            ce_runtime_shutdown(rt_, kWaitMs);
            ce_runtime_destroy(rt_);
            rt_ = nullptr;
        }
        testing_seam::SetRuntimeFactory({});
        if (children_->injectGate) {
            CloseHandle(children_->injectGate);
            children_->injectGate = nullptr;
        }
    }

    void WriteConfig(const std::string& text) {
        std::ofstream file(directory_.config(), std::ios::binary | std::ios::trunc);
        file << text;
    }

    std::string ReadConfig() const {
        std::ifstream file(directory_.config(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    ce_runtime_desc_t MakeDesc(uint32_t features) const {
        ce_runtime_desc_t desc{};
        EXPECT_EQ(ce_runtime_desc_init(&desc, sizeof(desc), CE_API_VERSION), CE_OK);
        desc.client_name = "test-client";
        desc.features = features;
        return desc;
    }

    // Creates through the public entry point and remembers the handle for TearDown.
    ce_status_t Create(ce_runtime_t** out, uint32_t features = CE_FEATURE_WATCH_SETTINGS) {
        const ce_runtime_desc_t desc = MakeDesc(features);
        const ce_status_t status = ce_runtime_create(&desc, out);
        if (status == CE_OK)
            rt_ = *out;
        return status;
    }

    // Creates a runtime and waits for it to reach READY.
    ce_runtime_t* CreateReady(uint32_t features = CE_FEATURE_WATCH_SETTINGS) {
        ce_runtime_t* rt = nullptr;
        EXPECT_EQ(Create(&rt, features), CE_OK);
        EXPECT_TRUE(WaitFor(rt, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
        return rt;
    }

    // Begins an edit and fails the test when it does not begin.
    ce_settings_edit_t* Begin(ce_runtime_t* rt) {
        ce_settings_edit_t* edit = nullptr;
        EXPECT_EQ(ce_settings_begin(rt, &edit), CE_OK);
        return edit;
    }

    // The stored value of a key through the API, or "" when it is absent.
    std::string Get(ce_runtime_t* rt, const char* section, const char* key) {
        size_t needed = 0;
        if (ce_settings_get(rt, section, key, nullptr, 0, &needed) != CE_OK)
            return {};
        std::string value(needed, '\0');
        EXPECT_EQ(ce_settings_get(rt, section, key, value.data(), value.size(), &needed), CE_OK);
        value.resize(needed - 1);
        return value;
    }

    TempConfigDirectory directory_;
    std::shared_ptr<ChildrenScript> children_ = std::make_shared<ChildrenScript>();
    std::shared_ptr<SettingsStore> store_;
    ce_runtime_t* rt_ = nullptr;
};

TEST_F(CengineApiSettingsTest, GetReportsWhatItNeedsAndReturnsTheStoredValue) {
    ce_runtime_t* rt = CreateReady();
    ASSERT_NE(rt, nullptr);
    size_t needed = 99;
    EXPECT_EQ(ce_settings_get(rt, "Video", "fps", nullptr, 0, &needed), CE_OK) << "a NULL buffer queries the size";
    EXPECT_EQ(needed, 3u) << "\"60\" and its terminator";

    char small[2] = {};
    EXPECT_EQ(ce_settings_get(rt, "Video", "fps", small, sizeof(small), &needed), CE_E_BUFFER_TOO_SMALL);
    EXPECT_EQ(needed, 3u) << "a short buffer reports what it needs";

    char buf[16] = {};
    EXPECT_EQ(ce_settings_get(rt, "Video", "fps", buf, sizeof(buf), &needed), CE_OK);
    EXPECT_STREQ(buf, "60");
    EXPECT_EQ(needed, 3u);

    needed = 99;
    EXPECT_EQ(ce_settings_get(rt, "Video", "missing", nullptr, 0, &needed), CE_E_NOT_FOUND);
    EXPECT_EQ(needed, 0u) << "a failed query clears *needed";
    EXPECT_EQ(ce_settings_get(rt, nullptr, "fps", nullptr, 0, nullptr), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(ce_settings_get(nullptr, "Video", "fps", nullptr, 0, nullptr), CE_E_INVALID_ARGUMENT);
}

TEST_F(CengineApiSettingsTest, ACommittedEditCompletesThenAnnouncesTheChange) {
    ce_runtime_t* rt = CreateReady();
    ASSERT_NE(rt, nullptr);
    const uint64_t base = store_->Revision();
    ce_settings_edit_t* edit = Begin(rt);
    ASSERT_NE(edit, nullptr);
    EXPECT_EQ(ce_settings_set(edit, "Video", "fps", "90"), CE_OK);

    ce_request_t request = 0;
    ASSERT_EQ(ce_settings_commit(edit, &request), CE_OK) << "the edit is consumed; it is not used again";
    ASSERT_NE(request, 0u);

    std::vector<Seen> seen;
    const auto completed = WaitFor(rt, [&](const ce_event_t& e) { return IsCompletion(e, request); }, &seen);
    ASSERT_TRUE(completed);
    EXPECT_EQ(completed.value().status, CE_OK);
    const auto changed = WaitFor(rt, [](const ce_event_t& e) { return e.type == CE_EVENT_SETTINGS_CHANGED; }, &seen);
    ASSERT_TRUE(changed);
    EXPECT_EQ(changed.value().source, CE_SETTINGS_SOURCE_API);
    EXPECT_EQ(changed.value().revision, base + 1);
    EXPECT_EQ(Get(rt, "Video", "fps"), "90");
    EXPECT_NE(ReadConfig().find("fps=90"), std::string::npos) << "the edit is written to config.ini";
}

TEST_F(CengineApiSettingsTest, AnInvalidEditReportsDiagnosticsAndStaysOpenUntilFixed) {
    ce_runtime_t* rt = CreateReady();
    ASSERT_NE(rt, nullptr);
    ce_settings_edit_t* edit = Begin(rt);
    ASSERT_NE(edit, nullptr);
    EXPECT_EQ(ce_settings_set(edit, "Capture", "capture_method", "bogus_method"), CE_OK);

    ce_request_t request = 7;
    EXPECT_EQ(ce_settings_commit(edit, &request), CE_E_SETTINGS);
    EXPECT_EQ(request, 0u) << "a refused commit admits nothing";

    uint32_t count = 0;
    ASSERT_EQ(ce_settings_diagnostic_count(edit, &count), CE_OK);
    ASSERT_GE(count, 1u);
    ce_settings_diagnostic_t diagnostic{};
    ASSERT_EQ(ce_settings_get_diagnostic(edit, 0, &diagnostic), CE_OK);
    EXPECT_EQ(diagnostic.struct_size, sizeof(diagnostic));
    EXPECT_EQ(diagnostic.severity, CE_DIAG_ERROR);
    EXPECT_STREQ(diagnostic.section, "Capture");
    EXPECT_GT(std::strlen(diagnostic.message), 0u);
    EXPECT_EQ(ce_settings_get_diagnostic(edit, count, &diagnostic), CE_E_INVALID_ARGUMENT) << "past the last one";
    EXPECT_EQ(Get(rt, "Capture", "capture_method"), "") << "nothing was published";

    // A later edit of the same key wins, so the candidate is valid and the same edit commits.
    EXPECT_EQ(ce_settings_set(edit, "Capture", "capture_method", "wgc"), CE_OK);
    ASSERT_EQ(ce_settings_commit(edit, &request), CE_OK);
    ASSERT_NE(request, 0u);
    const auto completed = WaitFor(rt, [&](const ce_event_t& e) { return IsCompletion(e, request); });
    ASSERT_TRUE(completed);
    EXPECT_EQ(completed.value().status, CE_OK);
    EXPECT_TRUE(WaitFor(rt, [](const ce_event_t& e) { return e.type == CE_EVENT_SETTINGS_CHANGED; }));
    EXPECT_EQ(Get(rt, "Capture", "capture_method"), "wgc");
}

TEST_F(CengineApiSettingsTest, ACommitAfterAnExternalWriteIsAConflictUntilTheEditStartsOver) {
    ce_runtime_t* rt = CreateReady();
    ASSERT_NE(rt, nullptr);
    const uint64_t base = store_->Revision();
    ce_settings_edit_t* edit = Begin(rt);
    ASSERT_NE(edit, nullptr);
    EXPECT_EQ(ce_settings_set(edit, "Video", "fps", "90"), CE_OK);

    WriteConfig("[Video]\r\nfps=75\r\n");
    std::vector<Seen> seen;
    const auto external = WaitFor(rt, [](const ce_event_t& e) { return e.type == CE_EVENT_SETTINGS_CHANGED; }, &seen);
    ASSERT_TRUE(external);
    EXPECT_EQ(external.value().source, CE_SETTINGS_SOURCE_FILE);

    ce_request_t request = 0;
    EXPECT_EQ(ce_settings_commit(edit, &request), CE_E_CONFLICT);
    EXPECT_EQ(request, 0u);
    ce_settings_discard(edit);

    edit = Begin(rt);
    ASSERT_NE(edit, nullptr);
    EXPECT_EQ(ce_settings_set(edit, "Video", "fps", "90"), CE_OK);
    ASSERT_EQ(ce_settings_commit(edit, &request), CE_OK);
    const auto completed = WaitFor(rt, [&](const ce_event_t& e) { return IsCompletion(e, request); }, &seen);
    ASSERT_TRUE(completed);
    EXPECT_EQ(completed.value().status, CE_OK);
    const auto changed = WaitFor(rt, [](const ce_event_t& e) { return e.type == CE_EVENT_SETTINGS_CHANGED; }, &seen);
    ASSERT_TRUE(changed);
    EXPECT_EQ(changed.value().source, CE_SETTINGS_SOURCE_API);
    EXPECT_EQ(changed.value().revision, base + 2);
    EXPECT_EQ(Get(rt, "Video", "fps"), "90");
}

TEST_F(CengineApiSettingsTest, NullArgumentsAreRefusedAndDiscardFreesTheEdit) {
    ce_runtime_t* rt = CreateReady();
    ASSERT_NE(rt, nullptr);
    ce_settings_edit_t* edit = nullptr;
    EXPECT_EQ(ce_settings_begin(rt, nullptr), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(ce_settings_begin(nullptr, &edit), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(edit, nullptr);

    edit = Begin(rt);
    ASSERT_NE(edit, nullptr);
    EXPECT_EQ(ce_settings_set(nullptr, "Video", "fps", "90"), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(ce_settings_set(edit, nullptr, "fps", "90"), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(ce_settings_set(edit, "Video", nullptr, "90"), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(ce_settings_set(edit, "Video", "fps", nullptr), CE_OK) << "a NULL value removes the key";

    ce_request_t request = 5;
    EXPECT_EQ(ce_settings_commit(nullptr, &request), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(request, 0u);
    uint32_t count = 9;
    EXPECT_EQ(ce_settings_diagnostic_count(edit, nullptr), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(ce_settings_diagnostic_count(edit, &count), CE_OK);
    EXPECT_EQ(count, 0u) << "no commit attempt, no diagnostics";

    ce_settings_discard(nullptr);  // a no-op
    ce_settings_discard(edit);
    EXPECT_EQ(Get(rt, "Video", "fps"), "60") << "a discarded edit changes nothing";
}

TEST_F(CengineApiSettingsTest, AnEditOfADestroyedRuntimeIsRefused) {
    ce_runtime_t* rt = CreateReady();
    ASSERT_NE(rt, nullptr);
    ce_settings_edit_t* edit = Begin(rt);
    ASSERT_NE(edit, nullptr);
    EXPECT_EQ(ce_settings_set(edit, "Video", "fps", "90"), CE_OK);

    ASSERT_EQ(ce_runtime_request_shutdown(rt), CE_OK);
    ASSERT_TRUE(WaitFor(rt, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_STOPPED); }));
    ASSERT_EQ(ce_runtime_destroy(rt), CE_OK);
    rt_ = nullptr;

    ce_request_t request = 0;
    EXPECT_EQ(ce_settings_commit(edit, &request), CE_E_INVALID_ARGUMENT) << "the runtime is gone";
    EXPECT_EQ(request, 0u);
    ce_settings_edit_t* other = nullptr;
    EXPECT_EQ(ce_settings_begin(rt, &other), CE_E_INVALID_ARGUMENT) << "a stale handle";
    EXPECT_EQ(other, nullptr);
    ce_settings_discard(edit);
}

TEST_F(CengineApiSettingsTest, ReadsWorkWhileStartingButACommitIsNotAdmittedUntilReady) {
    children_->injectGate = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // holds the engine in STARTING
    ASSERT_NE(children_->injectGate, nullptr);
    ce_runtime_t* rt = nullptr;
    ASSERT_EQ(Create(&rt), CE_OK);

    EXPECT_EQ(Get(rt, "Video", "fps"), "60") << "the store is readable before READY";
    ce_settings_edit_t* edit = Begin(rt);
    ASSERT_NE(edit, nullptr);
    EXPECT_EQ(ce_settings_set(edit, "Video", "fps", "90"), CE_OK);
    ce_request_t request = 0;
    EXPECT_EQ(ce_settings_commit(edit, &request), CE_E_INVALID_STATE) << "admission needs READY";
    EXPECT_EQ(request, 0u);

    ASSERT_TRUE(SetEvent(children_->injectGate));
    ASSERT_TRUE(WaitFor(rt, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    ASSERT_EQ(ce_settings_commit(edit, &request), CE_OK) << "the same edit commits once the runtime is ready";
    const auto completed = WaitFor(rt, [&](const ce_event_t& e) { return IsCompletion(e, request); });
    ASSERT_TRUE(completed);
    EXPECT_EQ(completed.value().status, CE_OK);
}

}  // namespace
