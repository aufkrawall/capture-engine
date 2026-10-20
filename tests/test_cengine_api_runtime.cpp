// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// The runtime entry points of the C ABI (llm-wiki/library/03), called exactly as a client calls them. Engines are
// assembled from fakes through the test seam (runtime/api/capi_testing.h): no helper spawns, no hotkey registers,
// no window opens. The production package check runs against temp directories. Every wait is on a runtime event or
// a Win32 event with a generous bound; nothing sleeps.
#include "include/cengine/cengine.h"

#include "common/platform/ansi_path.h"
#include "runtime/api/capi_testing.h"
#include "runtime/api/capi_text.h"
#include "runtime/api/runtime_desc.h"
#include "runtime/api/runtime_handle.h"
#include "runtime/api/runtime_package.h"
#include "runtime/core/runtime_internal.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace ce::engine;
using ce::runtime::HostChild;
namespace testing_seam = ce::capi::testing;

constexpr uint32_t kWaitMs = 20000;     // upper bound only: every wait ends on an event
constexpr int kMaxEventReads = 4096;    // bound on one WaitFor: a runaway event stream fails the test, not hangs it

// What the fake children did and how they answer. Shared with the test; the engine thread owns the fake.
struct ChildrenScript {
    std::mutex mutex;
    std::vector<std::string> calls;
    bool injectStarts = true;
    bool mediaStarts = true;
    bool shutdownSucceeds = true;
    HANDLE injectGate = nullptr;   // when set, Ensure(Inject) waits for it (holds the engine in STARTING)
    HANDLE shutdownGate = nullptr;  // when set, Shutdown() waits for it (holds the engine in STOPPING)
    std::atomic<bool> live{false};
    std::map<HostChild, bool> present;

    void Record(std::string call) {
        std::lock_guard<std::mutex> lock(mutex);
        calls.push_back(std::move(call));
    }
    std::vector<std::string> Calls() {
        std::lock_guard<std::mutex> lock(mutex);
        return calls;
    }
};

class FakeChildren final : public ce::runtime::HostChildren {
public:
    explicit FakeChildren(std::shared_ptr<ChildrenScript> script) : script_(std::move(script)) {}
    bool Ensure(HostChild child, uint32_t) override {
        script_->Record("ensure-" + Name(child));
        if (child == HostChild::Inject && script_->injectGate)
            WaitForSingleObject(script_->injectGate, kWaitMs);
        const bool ok = child == HostChild::Inject ? script_->injectStarts
                        : child == HostChild::Media ? script_->mediaStarts
                                                    : true;
        std::lock_guard<std::mutex> lock(script_->mutex);
        script_->present[child] = ok;
        return ok;
    }
    bool Ready(HostChild child) const override { return Present(child); }
    bool Present(HostChild child) const override {
        std::lock_guard<std::mutex> lock(script_->mutex);
        const auto it = script_->present.find(child);
        return it != script_->present.end() && it->second;
    }
    bool Running(HostChild child) const override { return Present(child); }
    bool Send(HostChild child, ProcessCommand command, const char*, ProcessResponse* response, uint32_t) override {
        script_->Record("send-" + Name(child) + "-" + std::to_string(static_cast<int>(command)));
        if (!Present(child))
            return false;
        if (response)
            *response = ProcessResponse::Ack;
        return true;
    }
    void SendToAll(ProcessCommand command) override {
        script_->Record("send-all-" + std::to_string(static_cast<int>(command)));
    }
    void RetireMedia(uint64_t) override {
        script_->Record("retire-media");
        std::lock_guard<std::mutex> lock(script_->mutex);
        script_->present[HostChild::Media] = false;
    }
    std::vector<ce::runtime::FinalizedMedia> CollectFinalizedMedia() override { return {}; }
    uint32_t FinalizingCount() const override { return 0; }
    ce::ipc::CommandOutcome StopRecording(HostChild child, uint32_t) override {
        script_->Record("stop-" + Name(child));
        return ce::ipc::CommandOutcome::Accepted;
    }
    void Service(ce::runtime::AuxiliaryServices, const std::function<void()>& beforeRecovery) override {
        if (beforeRecovery)
            beforeRecovery();
    }
    void ReconfigureServices(ce::runtime::AuxiliaryServices) override { script_->Record("reconfigure"); }
    bool StopSensorsForSetup() override {
        script_->Record("stop-sensors");
        return true;
    }
    void ResumeSensorsAfterSetup() override { script_->Record("resume-sensors"); }
    bool Shutdown() override {
        script_->Record("shutdown");
        if (script_->shutdownGate)
            WaitForSingleObject(script_->shutdownGate, kWaitMs);
        return script_->shutdownSucceeds;
    }
    ce::ipc::ControlResult PublishRecordingIntent(RecordingStartIntent) override { return {}; }
    ce::ipc::ControlResult PublishNotification(OverlayNotificationType, uint64_t) override { return {}; }
    ce::ipc::ControlResult ReadRecordingHealth(ce::ipc::RecordingHealthObservation& observation) override {
        observation = {};
        observation.live = script_->live.load();  // no live stamp: never stale against the start
        return {};
    }
    void ClearMediaFailure(uint32_t, bool) override {}
    ce::ipc::InjectControlChannel InjectChannel() const override { return ce::ipc::InjectControlChannel(); }

private:
    static std::string Name(HostChild child) {
        switch (child) {
            case HostChild::Inject:
                return "inject";
            case HostChild::Media:
                return "media";
            case HostChild::Logger:
                return "logger";
            case HostChild::Sensors:
                return "sensors";
        }
        return "?";
    }
    std::shared_ptr<ChildrenScript> script_;
};

// Fixed settings: the engine's configuration never changes during these tests.
class FakeSettings final : public SettingsSource {
public:
    FakeSettings() { config_.logLevel = LogLevel::Off; }  // no logger helper, no sensors
    const AppConfig& Current() const override { return config_; }
    std::optional<AppConfig> Poll() override { return std::nullopt; }
    uint32_t WaitMs() const override { return 5; }  // service often; every assertion still waits on an event

private:
    AppConfig config_;
};

// One ce_event_t, copied out so the test may read it after the release.
struct SeenEvent {
    int32_t type = 0;
    uint64_t request = 0;
    ce_status_t status = CE_OK;
    int32_t state = 0;
    uint32_t failure = 0;
};

bool IsRuntimeState(const ce_event_t& event, int32_t state) {
    return event.type == CE_EVENT_RUNTIME_STATE && event.data.runtime.state == state;
}

// Reads and releases events until `match` accepts one (the copy is returned) or the bound passes.
std::optional<SeenEvent> WaitFor(ce_runtime_t* rt, const std::function<bool(const ce_event_t&)>& match,
                                 std::vector<SeenEvent>* seen = nullptr) {
    for (int reads = 0; reads < kMaxEventReads; ++reads) {
        const ce_event_t* event = nullptr;
        if (ce_runtime_next_event(rt, kWaitMs, &event) != CE_OK || !event)
            return std::nullopt;
        SeenEvent copy;
        copy.type = event->type;
        copy.request = event->request;
        copy.status = event->status;
        if (event->type == CE_EVENT_RUNTIME_STATE)
            copy.state = event->data.runtime.state;
        if (event->type == CE_EVENT_RECORDING_STATE) {
            copy.state = event->data.recording.state;
            copy.failure = event->data.recording.failure;
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

// A directory under the system temp path, removed with everything in it.
class TempDirectory {
public:
    TempDirectory() {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("ce_api_test_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(++counter));
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    const std::filesystem::path& path() const { return path_; }
    std::string utf8() const {
        const auto text = path_.u8string();
        return std::string(reinterpret_cast<const char*>(text.data()), text.size());
    }

private:
    std::filesystem::path path_;
};

// The fixture: a fake engine factory behind the seam, and the bookkeeping that releases gates and destroys any
// runtime a failed test left behind, so one failure does not cascade into BUSY in the next test.
class CengineApiRuntimeTest : public ::testing::Test {
protected:
    void SetUp() override { InstallFakeEngine(); }

    // The seam's factory: an engine assembled from the fakes above, recording what the create call asked for.
    void InstallFakeEngine() {
        testing_seam::SetRuntimeFactory([this](const RuntimeOptions& options, ce_status_t* status) {
            ++factoryCalls;
            seenOptions = options;
            RuntimeParts parts;
            parts.options = options;
            auto script = children_;
            parts.makeChildren = [script](void (*)(), bool (*)()) {
                return std::unique_ptr<ce::runtime::HostChildren>(std::make_unique<FakeChildren>(script));
            };
            parts.settings = std::make_unique<FakeSettings>();
            parts.capture = [this](const std::string&, const std::string&) {
                ++captures;
                return ce::runtime::ScreenshotCapture{true, {"C:\\shots\\a.png", "C:\\shots\\a.avif"}};
            };
            return RuntimeFactory::Assemble(std::move(parts), status);
        });
    }

    void TearDown() override {
        // Open gates first: a held helper would otherwise hold the engine (and this test) for the whole bound.
        if (children_->injectGate)
            SetEvent(children_->injectGate);
        if (children_->shutdownGate)
            SetEvent(children_->shutdownGate);
        if (rt_) {
            ce_runtime_request_shutdown(rt_);
            ce_runtime_shutdown(rt_, kWaitMs);
            ce_runtime_destroy(rt_);
            rt_ = nullptr;
        }
        testing_seam::SetRuntimeFactory({});
        for (HANDLE* gate : {&children_->injectGate, &children_->shutdownGate}) {
            if (*gate)
                CloseHandle(*gate);
            *gate = nullptr;
        }
    }

    ce_runtime_desc_t MakeDesc(uint32_t features = 0) const {
        ce_runtime_desc_t desc{};
        EXPECT_EQ(ce_runtime_desc_init(&desc, sizeof(desc), CE_API_VERSION), CE_OK);
        desc.client_name = "test-client";
        desc.features = features;
        return desc;
    }

    // Creates through the public entry point and remembers the handle for TearDown.
    ce_status_t Create(ce_runtime_t** out, uint32_t features = 0) {
        const ce_runtime_desc_t desc = MakeDesc(features);
        const ce_status_t status = ce_runtime_create(&desc, out);
        if (status == CE_OK)
            rt_ = *out;
        return status;
    }

    // Creates a runtime and waits for it to reach READY.
    ce_runtime_t* CreateReady() {
        ce_runtime_t* rt = nullptr;
        EXPECT_EQ(Create(&rt), CE_OK);
        EXPECT_TRUE(WaitFor(rt, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
        return rt;
    }

    // Shuts the runtime down and waits for its STOPPED event; the handle stays valid for the caller to destroy.
    void ShutDownAndWait(ce_runtime_t* rt) {
        ASSERT_EQ(ce_runtime_request_shutdown(rt), CE_OK);
        ASSERT_TRUE(WaitFor(rt, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_STOPPED); }));
    }

    // Destroys a runtime this test shut down, and forgets it so TearDown does not touch it again.
    ce_status_t Destroy(ce_runtime_t* rt) {
        const ce_status_t status = ce_runtime_destroy(rt);
        if (status == CE_OK)
            rt_ = nullptr;
        return status;
    }

    std::shared_ptr<ChildrenScript> children_ = std::make_shared<ChildrenScript>();
    std::atomic<int> factoryCalls{0};
    std::atomic<int> captures{0};
    std::optional<RuntimeOptions> seenOptions;

private:
    ce_runtime_t* rt_ = nullptr;
};

// ---- Descriptor, create, destroy, handles ----------------------------------------------------------------------

TEST_F(CengineApiRuntimeTest, NullDescriptorOrOutputIsInvalidArgument) {
    ce_runtime_t* rt = reinterpret_cast<ce_runtime_t*>(0x1);
    EXPECT_EQ(ce_runtime_create(nullptr, &rt), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(rt, nullptr) << "a failed create clears *out";
    const ce_runtime_desc_t desc = MakeDesc();
    EXPECT_EQ(ce_runtime_create(&desc, nullptr), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(factoryCalls.load(), 0);
}

TEST_F(CengineApiRuntimeTest, DescriptorExtentAndVersionAreCheckedBeforeAnythingElse) {
    ce_runtime_t* rt = nullptr;
    ce_runtime_desc_t small = MakeDesc();
    small.struct_size = 8;
    EXPECT_EQ(ce_runtime_create(&small, &rt), CE_E_INVALID_ARGUMENT);
    ce_runtime_desc_t otherMajor = MakeDesc();
    otherMajor.api_version = (3u << 16) | CE_API_VERSION_MINOR;
    EXPECT_EQ(ce_runtime_create(&otherMajor, &rt), CE_E_VERSION);
    ce_runtime_desc_t newerMinor = MakeDesc();
    newerMinor.api_version = (CE_API_VERSION_MAJOR << 16) | (CE_API_VERSION_MINOR + 1u);
    EXPECT_EQ(ce_runtime_create(&newerMinor, &rt), CE_E_VERSION);
    EXPECT_EQ(rt, nullptr);
    EXPECT_EQ(factoryCalls.load(), 0) << "a refused descriptor never reaches the engine";
}

TEST_F(CengineApiRuntimeTest, UnknownFeatureBitsAreRefused) {
    ce_runtime_t* rt = nullptr;
    const ce_runtime_desc_t desc = MakeDesc(1u << 31);
    EXPECT_EQ(ce_runtime_create(&desc, &rt), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(factoryCalls.load(), 0);
}

TEST_F(CengineApiRuntimeTest, FeatureBitsReachTheEngineOptions) {
    ce_runtime_t* rt = nullptr;
    ASSERT_EQ(Create(&rt, CE_FEATURE_GLOBAL_HOTKEYS | CE_FEATURE_WATCH_SETTINGS), CE_OK);
    ASSERT_TRUE(seenOptions);
    EXPECT_EQ(seenOptions.value().features, CE_FEATURE_GLOBAL_HOTKEYS | CE_FEATURE_WATCH_SETTINGS);
}

TEST_F(CengineApiRuntimeTest, CreateRefusesASecondRuntimeWhileOneIsLive) {
    ce_runtime_t* first = CreateReady();
    ASSERT_NE(first, nullptr);
    ce_runtime_t* second = reinterpret_cast<ce_runtime_t*>(0x1);
    EXPECT_EQ(Create(&second), CE_E_BUSY);
    EXPECT_EQ(second, nullptr);
    EXPECT_EQ(factoryCalls.load(), 1) << "the refused create built nothing";
}

TEST_F(CengineApiRuntimeTest, RuntimeRunsToStoppedThenDestroysAndCreatesAgain) {
    ce_runtime_t* rt = CreateReady();
    ASSERT_NE(rt, nullptr);
    EXPECT_EQ(ce_runtime_request_shutdown(rt), CE_OK);
    EXPECT_EQ(ce_runtime_request_shutdown(rt), CE_OK) << "request_shutdown is idempotent";
    ASSERT_TRUE(WaitFor(rt, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_STOPPED); }));
    EXPECT_EQ(Destroy(rt), CE_OK);

    ce_runtime_t* again = CreateReady();
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(factoryCalls.load(), 2);
    ShutDownAndWait(again);
    EXPECT_EQ(Destroy(again), CE_OK);
}

TEST_F(CengineApiRuntimeTest, DestroyWhileReadyIsInvalidStateAndTheHandleStaysValid) {
    ce_runtime_t* rt = CreateReady();
    ASSERT_NE(rt, nullptr);
    EXPECT_EQ(Destroy(rt), CE_E_INVALID_STATE);
    ce_status_info_t info{};
    info.struct_size = sizeof(info);
    EXPECT_EQ(ce_runtime_get_status(rt, &info), CE_OK) << "a refused destroy leaves the handle usable";
    EXPECT_EQ(info.runtime_state, CE_RUNTIME_READY);
}

TEST_F(CengineApiRuntimeTest, StaleHandleAfterDestroyIsInvalidArgument) {
    ce_runtime_t* rt = CreateReady();
    ASSERT_NE(rt, nullptr);
    ShutDownAndWait(rt);
    ASSERT_EQ(Destroy(rt), CE_OK);

    // No new runtime is created here: a later create may reuse the address, which pointer handles cannot tell apart.
    ce_status_info_t info{};
    info.struct_size = sizeof(info);
    EXPECT_EQ(ce_runtime_get_status(rt, &info), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(ce_runtime_request_shutdown(rt), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(ce_runtime_destroy(rt), CE_E_INVALID_ARGUMENT) << "a second destroy fails cleanly";
    const ce_event_t* event = nullptr;
    EXPECT_EQ(ce_runtime_next_event(rt, 0, &event), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(event, nullptr);
    EXPECT_EQ(ce_recording_stop(rt, nullptr, nullptr), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(ce_runtime_log(rt, CE_LOG_INFO, "late"), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(ce_runtime_event_handle(rt), nullptr);
}

TEST_F(CengineApiRuntimeTest, AFailedRuntimeCanBeShutDownAndDestroyed) {
    children_->injectStarts = false;
    ce_runtime_t* rt = nullptr;
    ASSERT_EQ(Create(&rt), CE_OK);
    ASSERT_TRUE(WaitFor(rt, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_FAILED); }));
    EXPECT_EQ(ce_runtime_shutdown(rt, kWaitMs), CE_OK);
    EXPECT_EQ(Destroy(rt), CE_OK);
}

TEST_F(CengineApiRuntimeTest, AFailedRuntimeIsNotDestroyedWhileItsHelpersStillShutDown) {
    children_->injectStarts = false;
    children_->shutdownGate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(children_->shutdownGate, nullptr);
    ce_runtime_t* rt = nullptr;
    ASSERT_EQ(Create(&rt), CE_OK);
    // FAILED arrives first; the engine then shuts the helpers down, held here at the gate.
    ASSERT_TRUE(WaitFor(rt, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_FAILED); }));
    EXPECT_EQ(Destroy(rt), CE_E_INVALID_STATE) << "destroy must not block on the helper shutdown";
    SetEvent(children_->shutdownGate);
    EXPECT_EQ(ce_runtime_shutdown(rt, kWaitMs), CE_OK);
    EXPECT_EQ(Destroy(rt), CE_OK);
}

TEST_F(CengineApiRuntimeTest, ShutdownTimeoutKeepsTheHandleValidAndCanBeRetried) {
    children_->shutdownGate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(children_->shutdownGate, nullptr);
    ce_runtime_t* rt = CreateReady();
    ASSERT_NE(rt, nullptr);

    // The helper shutdown is held at its gate, so the engine cannot stop: a zero budget times out at once.
    EXPECT_EQ(ce_runtime_shutdown(rt, 0), CE_E_TIMEOUT);
    ce_status_info_t info{};
    info.struct_size = sizeof(info);
    EXPECT_EQ(ce_runtime_get_status(rt, &info), CE_OK) << "the handle survives a timeout";
    EXPECT_NE(info.runtime_state, CE_RUNTIME_STOPPED);
    EXPECT_EQ(Destroy(rt), CE_E_INVALID_STATE) << "a runtime that has not stopped cannot be destroyed";

    SetEvent(children_->shutdownGate);
    EXPECT_EQ(ce_runtime_shutdown(rt, kWaitMs), CE_OK) << "the retry completes the shutdown";
    EXPECT_EQ(Destroy(rt), CE_OK);
}

TEST_F(CengineApiRuntimeTest, ExceptionsNeverCrossTheBoundary) {
    EXPECT_EQ(ce::capi::RunGuarded("test", []() -> ce_status_t { throw std::bad_alloc(); }), CE_E_NO_MEMORY);
    EXPECT_EQ(ce::capi::RunGuarded("test", []() -> ce_status_t { throw std::runtime_error("boom"); }), CE_E_INTERNAL);
    EXPECT_EQ(ce::capi::RunGuarded("test", []() -> ce_status_t { return CE_OK; }), CE_OK);
}

TEST_F(CengineApiRuntimeTest, FailedCreateReleasesTheSession) {
    testing_seam::SetRuntimeFactory([](const RuntimeOptions&, ce_status_t* status) {
        *status = CE_E_PACKAGE;
        return std::unique_ptr<Runtime>();
    });
    ce_runtime_t* rt = reinterpret_cast<ce_runtime_t*>(0x1);
    EXPECT_EQ(Create(&rt), CE_E_PACKAGE);
    EXPECT_EQ(rt, nullptr);

    // The next create is not refused as busy: the failed one gave the session back.
    InstallFakeEngine();
    ce_runtime_t* ok = CreateReady();
    EXPECT_NE(ok, nullptr);
}

// ---- Package and production path -------------------------------------------------------------------------------

TEST_F(CengineApiRuntimeTest, AMissingRoleHostIsAPackageErrorAndReleasesTheSession) {
    testing_seam::SetRuntimeFactory({});  // the production path: package check, then the production engine
    TempDirectory dir;
    ce_runtime_desc_t desc = MakeDesc();
    const std::string packageDir = dir.utf8();
    desc.package_dir = packageDir.c_str();
    ce_runtime_t* rt = reinterpret_cast<ce_runtime_t*>(0x1);
    EXPECT_EQ(ce_runtime_create(&desc, &rt), CE_E_PACKAGE);
    EXPECT_EQ(rt, nullptr);
    EXPECT_EQ(ce_runtime_create(&desc, &rt), CE_E_PACKAGE) << "not CE_E_BUSY: the first failure gave the session back";
}

TEST_F(CengineApiRuntimeTest, ARelativePackageDirectoryIsInvalidArgument) {
    testing_seam::SetRuntimeFactory({});
    ce_runtime_desc_t desc = MakeDesc();
    desc.package_dir = "relative\\package";
    ce_runtime_t* rt = nullptr;
    EXPECT_EQ(ce_runtime_create(&desc, &rt), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(rt, nullptr);
}

TEST(CengineApiRuntimePackage, ResolvesTheRoleHostAndTheConfigurationBesideThePackage) {
    TempDirectory dir;
    {
        std::ofstream roleHost(dir.path() / "captureengine.exe", std::ios::binary);
        roleHost << "placeholder: only its presence is checked";
    }
    ce::capi::PackageLayout layout;
    const char* reason = nullptr;
    ASSERT_EQ(ce::capi::ResolvePackage(dir.utf8(), std::nullopt, layout, &reason), CE_OK) << (reason ? reason : "");
    EXPECT_EQ(layout.roleHost, (dir.path() / L"captureengine.exe").wstring());
    EXPECT_EQ(layout.configPath, ce::ansi_path::CompatiblePath((dir.path() / L"config.ini").wstring()));
    EXPECT_EQ(layout.logsRoot, ce::ansi_path::CompatiblePath(dir.path().wstring()) + "\\logs")
        << "the diagnostics session lives below the package, as captureengine.exe's sessions always did";
    EXPECT_NE(layout.windowModule, nullptr) << "the module containing the runtime code";
}

// The app's --config= reaches the runtime as settings_path: the store and every helper use that file.
TEST(CengineApiRuntimePackage, ASettingsPathReplacesThePackageConfiguration) {
    TempDirectory dir;
    {
        std::ofstream roleHost(dir.path() / "captureengine.exe", std::ios::binary);
        roleHost << "placeholder: only its presence is checked";
    }
    const std::filesystem::path settings = dir.path() / L"elsewhere.ini";
    ce::capi::PackageLayout layout;
    const char* reason = nullptr;
    ASSERT_EQ(ce::capi::ResolvePackage(dir.utf8(), dir.utf8() + "\\elsewhere.ini", layout, &reason), CE_OK)
        << (reason ? reason : "");
    EXPECT_EQ(layout.configPathWide, settings.wstring());
    EXPECT_EQ(layout.configPath, ce::ansi_path::CompatiblePath(settings.wstring()));

    EXPECT_EQ(ce::capi::ResolvePackage(dir.utf8(), std::string("relative.ini"), layout, &reason),
              CE_E_INVALID_ARGUMENT);
    ASSERT_NE(reason, nullptr);
    EXPECT_NE(std::string(reason).find("settings_path"), std::string::npos) << reason;
}

TEST(CengineApiRuntimePackage, AMissingRoleHostNamesTheMissingFileAsPackageError) {
    TempDirectory dir;
    ce::capi::PackageLayout layout;
    const char* reason = nullptr;
    EXPECT_EQ(ce::capi::ResolvePackage(dir.utf8(), std::nullopt, layout, &reason), CE_E_PACKAGE);
    ASSERT_NE(reason, nullptr);
    EXPECT_NE(std::string(reason).find("captureengine.exe"), std::string::npos) << reason;
}

// ---- Commands --------------------------------------------------------------------------------------------------

TEST_F(CengineApiRuntimeTest, CommandsBeforeReadyAreRefusedWithoutARequestId) {
    children_->injectGate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(children_->injectGate, nullptr);
    ce_runtime_t* rt = nullptr;
    ASSERT_EQ(Create(&rt), CE_OK);
    ce_request_t request = 77;
    EXPECT_EQ(ce_overlay_toggle(rt, &request), CE_E_INVALID_STATE);
    EXPECT_EQ(request, 0u) << "a refused command writes 0 to its request output";
    EXPECT_EQ(ce_recording_start(rt, CE_MODE_VIDEO, "early", &request), CE_E_INVALID_STATE);
    EXPECT_EQ(request, 0u);

    SetEvent(children_->injectGate);
    ASSERT_TRUE(WaitFor(rt, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    EXPECT_EQ(ce_overlay_toggle(rt, &request), CE_OK);
    EXPECT_NE(request, 0u);
}

TEST_F(CengineApiRuntimeTest, EveryAcceptedCommandCompletesExactlyOnceWithItsRequestId) {
    ce_runtime_t* rt = CreateReady();
    ASSERT_NE(rt, nullptr);
    std::vector<ce_request_t> requests(5, 0);
    ASSERT_EQ(ce_overlay_toggle(rt, &requests[0]), CE_OK);
    ASSERT_EQ(ce_benchmark_toggle(rt, &requests[1]), CE_OK);
    ASSERT_EQ(ce_screenshot_take(rt, &requests[2]), CE_OK);
    ASSERT_EQ(ce_recording_stop(rt, "test", &requests[3]), CE_OK);
    ASSERT_EQ(ce_target_launch(rt, "C:\\tools\\app.exe", &requests[4]), CE_OK);
    for (ce_request_t request : requests)
        ASSERT_NE(request, 0u);

    // Collect every event up to STOPPED: each admitted command must complete exactly once, and nothing completes
    // twice or for an unknown id before the runtime is gone.
    std::vector<SeenEvent> seen;
    const auto completedCount = [&seen]() {
        size_t count = 0;
        for (const SeenEvent& event : seen)
            count += event.type == CE_EVENT_COMMAND_COMPLETED ? 1 : 0;
        return count;
    };
    while (completedCount() < requests.size())
        ASSERT_TRUE(WaitFor(rt, [](const ce_event_t& e) { return e.type == CE_EVENT_COMMAND_COMPLETED; }, &seen));
    ASSERT_EQ(ce_runtime_request_shutdown(rt), CE_OK);
    ASSERT_TRUE(WaitFor(rt, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_STOPPED); }, &seen));

    std::map<uint64_t, int> completions;
    for (const SeenEvent& event : seen)
        if (event.type == CE_EVENT_COMMAND_COMPLETED)
            ++completions[event.request];
    for (ce_request_t request : requests)
        EXPECT_EQ(completions[request], 1) << "request " << request;
    EXPECT_EQ(completions.size(), requests.size()) << "no completion for an id that was never issued";
    EXPECT_EQ(captures.load(), 1) << "the screenshot ran once";
}

TEST_F(CengineApiRuntimeTest, RecordingCommandsRefuseBadModesAndStrings) {
    ce_runtime_t* rt = CreateReady();
    ASSERT_NE(rt, nullptr);
    ce_request_t request = 9;
    EXPECT_EQ(ce_recording_start(rt, 7, "bad mode", &request), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(request, 0u);
    EXPECT_EQ(ce_recording_toggle(rt, 0, "bad mode", &request), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(ce_recording_start(rt, CE_MODE_VIDEO, "\xC3\x28", &request), CE_E_INVALID_ARGUMENT)
        << "invalid UTF-8 is refused";
    const std::string tooLong(ce::capi::kMaxReasonBytes + 1, 'r');
    EXPECT_EQ(ce_recording_start(rt, CE_MODE_VIDEO, tooLong.c_str(), &request), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(ce_target_launch(rt, nullptr, &request), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(request, 0u);
    EXPECT_EQ(ce_target_launch(rt, "", &request), CE_E_INVALID_ARGUMENT) << "an empty command line is refused";
    EXPECT_EQ(ce_recording_stop(rt, nullptr, nullptr), CE_OK) << "NULL reason and NULL request are both allowed";
}

// ---- Events ----------------------------------------------------------------------------------------------------

TEST_F(CengineApiRuntimeTest, NextEventRefusesTheReservedTimeoutAndANullOutput) {
    ce_runtime_t* rt = CreateReady();
    ASSERT_NE(rt, nullptr);
    const ce_event_t* event = reinterpret_cast<const ce_event_t*>(0x1);
    EXPECT_EQ(ce_runtime_next_event(rt, UINT32_MAX, &event), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(event, nullptr);
    EXPECT_EQ(ce_runtime_next_event(rt, 0, nullptr), CE_E_INVALID_ARGUMENT);
}

TEST_F(CengineApiRuntimeTest, EventHandleIsSignaledExactlyWhileEventsArePending) {
    ce_runtime_t* rt = nullptr;
    ASSERT_EQ(Create(&rt), CE_OK);
    HANDLE handle = static_cast<HANDLE>(ce_runtime_event_handle(rt));
    ASSERT_NE(handle, nullptr);
    ASSERT_EQ(WaitForSingleObject(handle, kWaitMs), WAIT_OBJECT_0) << "READY is pending, so the handle is signaled";

    // Drain: once nothing is pending the handle is not signaled, and a zero wait proves it without blocking.
    for (const ce_event_t* event = nullptr; ce_runtime_next_event(rt, 0, &event) == CE_OK; event = nullptr)
        ASSERT_EQ(ce_runtime_release_event(rt, event), CE_OK);
    EXPECT_EQ(WaitForSingleObject(handle, 0), WAIT_TIMEOUT) << "nothing pending: not signaled";

    // A command's completion makes the handle signaled again.
    ce_request_t request = 0;
    ASSERT_EQ(ce_overlay_toggle(rt, &request), CE_OK);
    EXPECT_EQ(WaitForSingleObject(handle, kWaitMs), WAIT_OBJECT_0);
    ASSERT_TRUE(WaitFor(rt, [&](const ce_event_t& e) {
        return e.type == CE_EVENT_COMMAND_COMPLETED && e.request == request;
    }));
    ShutDownAndWait(rt);
    EXPECT_EQ(Destroy(rt), CE_OK);
}

TEST_F(CengineApiRuntimeTest, AReleasedEventCannotBeReleasedAgain) {
    ce_runtime_t* rt = CreateReady();  // READY is consumed here: the next event must come from a command
    ASSERT_NE(rt, nullptr);
    ce_request_t request = 0;
    ASSERT_EQ(ce_overlay_toggle(rt, &request), CE_OK);
    const ce_event_t* event = nullptr;
    for (int reads = 0; reads < kMaxEventReads; ++reads) {
        ASSERT_EQ(ce_runtime_next_event(rt, kWaitMs, &event), CE_OK);
        if (event->type == CE_EVENT_COMMAND_COMPLETED)
            break;
        ASSERT_EQ(ce_runtime_release_event(rt, event), CE_OK);
    }
    ASSERT_EQ(event->type, CE_EVENT_COMMAND_COMPLETED);
    ASSERT_EQ(event->request, request);
    ASSERT_EQ(ce_runtime_release_event(rt, event), CE_OK);
    EXPECT_EQ(ce_runtime_release_event(rt, event), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(ce_runtime_release_event(rt, nullptr), CE_E_INVALID_ARGUMENT);
}

// ---- Status ----------------------------------------------------------------------------------------------------

TEST_F(CengineApiRuntimeTest, GetStatusCopiesOnlyTheFieldsThatFitTheCallersExtent) {
    ce_runtime_t* rt = CreateReady();
    ASSERT_NE(rt, nullptr);

    // An older client knows the fields up to recording_id; the rest of its buffer (and the guard) must stay as it was.
    struct Guarded {
        ce_status_info_t info;
        unsigned char tail[16];
    } guarded;
    std::memset(&guarded, 0xAB, sizeof(guarded));
    guarded.info.struct_size = static_cast<uint32_t>(offsetof(ce_status_info_t, helpers_ready));
    ASSERT_EQ(ce_runtime_get_status(rt, &guarded.info), CE_OK);
    EXPECT_EQ(guarded.info.struct_size, static_cast<uint32_t>(offsetof(ce_status_info_t, helpers_ready)))
        << "struct_size is the caller's";
    EXPECT_EQ(guarded.info.runtime_state, CE_RUNTIME_READY);
    EXPECT_EQ(guarded.info.recording_state, CE_RECORDING_IDLE);
    const auto* raw = reinterpret_cast<const unsigned char*>(&guarded);
    for (size_t i = offsetof(ce_status_info_t, helpers_ready); i < sizeof(guarded); ++i)
        ASSERT_EQ(raw[i], 0xAB) << "byte " << i << " is beyond the caller's extent";

    ce_status_info_t full{};
    full.struct_size = sizeof(full);
    ASSERT_EQ(ce_runtime_get_status(rt, &full), CE_OK);
    EXPECT_TRUE(full.helpers_ready & CE_HELPER_INJECT);
    EXPECT_EQ(full.settings_revision, 0u);
}

TEST_F(CengineApiRuntimeTest, GetStatusRefusesAnExtentThatCannotCarryTheState) {
    ce_runtime_t* rt = CreateReady();
    ASSERT_NE(rt, nullptr);
    ce_status_info_t info{};
    for (uint32_t extent : {0u, 4u, 7u}) {
        info.struct_size = extent;
        EXPECT_EQ(ce_runtime_get_status(rt, &info), CE_E_INVALID_ARGUMENT) << "extent " << extent;
    }
    EXPECT_EQ(ce_runtime_get_status(rt, nullptr), CE_E_INVALID_ARGUMENT);
}

// ---- Log -------------------------------------------------------------------------------------------------------

TEST_F(CengineApiRuntimeTest, RuntimeLogRefusesABadLevelNullOrInvalidTextAndShortensLongText) {
    ce_runtime_t* rt = CreateReady();
    ASSERT_NE(rt, nullptr);
    EXPECT_EQ(ce_runtime_log(rt, 0, "level zero"), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(ce_runtime_log(rt, 9, "level nine"), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(ce_runtime_log(rt, CE_LOG_INFO, nullptr), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(ce_runtime_log(rt, CE_LOG_INFO, "\xFF"), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(ce_runtime_log(rt, CE_LOG_INFO, "100% sure\nsecond line"), CE_OK);
    const std::string long_(ce::capi::kMaxLogMessageBytes * 4, 'x');
    EXPECT_EQ(ce_runtime_log(rt, CE_LOG_DEBUG, long_.c_str()), CE_OK) << "long text is shortened, not refused";
}

// ---- Text helpers ----------------------------------------------------------------------------------------------

TEST(CengineApiText, ReadClientTextBoundsLengthAndRequiresUtf8) {
    std::string out;
    EXPECT_EQ(ce::capi::ReadClientText("plain", 16, out), CE_OK);
    EXPECT_EQ(out, "plain");
    EXPECT_EQ(ce::capi::ReadClientText("abcd", 3, out), CE_E_INVALID_ARGUMENT) << "one byte over the limit";
    EXPECT_EQ(ce::capi::ReadClientText("abc", 3, out), CE_OK) << "exactly the limit";
    EXPECT_EQ(ce::capi::ReadClientText("\xC3\x28", 16, out), CE_E_INVALID_ARGUMENT) << "invalid UTF-8";
    EXPECT_EQ(ce::capi::ReadClientText("\xE2\x82\xAC", 16, out), CE_OK) << "a three-byte character";
}

TEST(CengineApiText, TruncateUtf8KeepsWholeCharacters) {
    EXPECT_EQ(ce::capi::TruncateUtf8("abc", 2), "ab");
    EXPECT_EQ(ce::capi::TruncateUtf8("abc", 8), "abc");
    // 'a' then a three-byte euro sign: a cut inside the sign backs up to the lead byte.
    EXPECT_EQ(ce::capi::TruncateUtf8("a\xE2\x82\xAC", 3), "a");
    EXPECT_EQ(ce::capi::TruncateUtf8("a\xE2\x82\xAC", 4), "a\xE2\x82\xAC");
}

TEST(CengineApiText, SanitizeLogTextKeepsOneLine) {
    EXPECT_EQ(ce::capi::SanitizeLogText("a\r\nb\tc\x7f"), "a  b c ");
}

TEST(CengineApiText, Utf8ToWideIsStrict) {
    std::wstring wide;
    EXPECT_TRUE(ce::capi::Utf8ToWideStrict("\xC3\xA9", wide));
    EXPECT_EQ(wide, L"\u00e9");
    EXPECT_FALSE(ce::capi::Utf8ToWideStrict("\xFF", wide));
    EXPECT_TRUE(wide.empty());
}

}  // namespace
