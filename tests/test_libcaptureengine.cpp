#include <gtest/gtest.h>

#include <stdexcept>
#include <thread>

// Exercise the actual C boundary without starting controller children or taking screenshots.
// The product unit is not otherwise linked into unit_tests.exe.
// NOLINTNEXTLINE(bugprone-suspicious-include) - deliberate implementation test with fake callbacks
#include "captureengine/app/libcaptureengine.cpp"

namespace {
ce_recording_intent_t testIntent = CE_RECORDING_INTENT_IDLE;
ce_status_t testResult = CE_SUCCESS;
ce::api::Command testCommand = ce::api::Command::Stop;
int testCalls = 0;
bool testThrow = false;
bool testReenter = false;
ce_engine_t* testEngine = nullptr;
const char* testReason = nullptr;
uint32_t testTimeout = 0;

ce_status_t TestCommand(ce::api::Command command, ce_recording_intent_t intent, const char* reason) {
    ++testCalls;
    testCommand = command;
    testReason = reason;
    if (testThrow)
        throw std::runtime_error("callback failure");
    if (testReenter) {
        EXPECT_EQ(ce_engine_destroy(testEngine), CE_ERROR_INVALID_STATE);
        EXPECT_EQ(ce_engine_toggle_overlay(testEngine), CE_ERROR_INVALID_STATE);
    }
    if (testResult == CE_SUCCESS && command == ce::api::Command::Start)
        testIntent = intent;
    if (testResult == CE_SUCCESS && command == ce::api::Command::Stop)
        testIntent = CE_RECORDING_INTENT_IDLE;
    return testResult;
}

ce_recording_intent_t TestIntent() {
    return testIntent;
}

ce_status_t TestPoll(uint32_t timeout) {
    testTimeout = timeout;
    EXPECT_EQ(ce_engine_destroy(testEngine), CE_ERROR_INVALID_STATE);
    EXPECT_EQ(ce_engine_poll_events(testEngine, 0), CE_ERROR_INVALID_STATE);
    // A hotkey dispatched by the pump must still be able to invoke the C API.
    return ce_engine_take_screenshot(testEngine);
}

class EngineApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        testIntent = CE_RECORDING_INTENT_IDLE;
        testResult = CE_SUCCESS;
        testCalls = 0;
        testThrow = false;
        testReenter = false;
        testReason = nullptr;
        ASSERT_TRUE(ce::api::BindControllerBackend({TestCommand, TestIntent, TestPoll}));
        ASSERT_EQ(ce_engine_create(nullptr, &testEngine), CE_SUCCESS);
    }
    void TearDown() override {
        EXPECT_EQ(ce_engine_destroy(testEngine), CE_SUCCESS);
        testEngine = nullptr;
        ce::api::UnbindControllerBackend();
    }
};
}  // namespace

TEST(EngineApiLifecycleTest, CreationRequiresAnInitializedControllerAndClearsOutputOnFailure) {
    ce_engine_t* engine = reinterpret_cast<ce_engine_t*>(uintptr_t{1});
    EXPECT_EQ(ce_engine_create(nullptr, &engine), CE_ERROR_NOT_INITIALIZED);
    EXPECT_EQ(engine, nullptr);
    EXPECT_EQ(ce_engine_create(nullptr, nullptr), CE_ERROR_INVALID_ARGUMENT);
    EXPECT_FALSE(ce::api::BindControllerBackend({}));
}

TEST_F(EngineApiTest, RejectsMultipleInstancesAndUnknownHandlesWithoutDispatch) {
    ce_engine_t* engine = reinterpret_cast<ce_engine_t*>(uintptr_t{1});
    EXPECT_EQ(ce_engine_create(nullptr, &engine), CE_ERROR_ALREADY_INITIALIZED);
    EXPECT_EQ(engine, nullptr);
    EXPECT_FALSE(ce::api::BindControllerBackend({TestCommand, TestIntent, TestPoll}));
    EXPECT_EQ(ce_engine_destroy(nullptr), CE_ERROR_INVALID_ARGUMENT);
    const auto invalid = reinterpret_cast<ce_engine_t*>(uintptr_t{1});
    EXPECT_EQ(ce_engine_destroy(invalid), CE_ERROR_INVALID_ARGUMENT);
    for (auto* handle : {static_cast<ce_engine_t*>(nullptr), invalid}) {
        EXPECT_EQ(ce_engine_start_recording(handle, CE_RECORDING_INTENT_VIDEO, nullptr), CE_ERROR_INVALID_ARGUMENT);
        EXPECT_EQ(ce_engine_stop_recording(handle, nullptr), CE_ERROR_INVALID_ARGUMENT);
        EXPECT_EQ(ce_engine_toggle_recording(handle), CE_ERROR_INVALID_ARGUMENT);
        EXPECT_EQ(ce_engine_toggle_audio_only(handle), CE_ERROR_INVALID_ARGUMENT);
        EXPECT_EQ(ce_engine_toggle_overlay(handle), CE_ERROR_INVALID_ARGUMENT);
        EXPECT_EQ(ce_engine_toggle_benchmark(handle), CE_ERROR_INVALID_ARGUMENT);
        EXPECT_EQ(ce_engine_take_screenshot(handle), CE_ERROR_INVALID_ARGUMENT);
        EXPECT_EQ(ce_engine_poll_events(handle, 0), CE_ERROR_INVALID_ARGUMENT);
        bool recording = true;
        EXPECT_EQ(ce_engine_is_recording(handle, &recording), CE_ERROR_INVALID_ARGUMENT);
        EXPECT_FALSE(recording);
        ce_recording_stats_t stats{};
        stats.struct_size = sizeof(stats);
        EXPECT_EQ(ce_engine_get_recording_stats(handle, &stats), CE_ERROR_INVALID_ARGUMENT);
    }
    EXPECT_EQ(testCalls, 0);
}

TEST_F(EngineApiTest, ValidatesConfigurationAndRejectsSettingsItCannotApply) {
    ce_engine_config_t config{};
    ASSERT_EQ(ce_engine_config_init_default(&config), CE_SUCCESS);
    EXPECT_EQ(config.struct_size, sizeof(config));
    EXPECT_EQ(config.config_file_path, nullptr);
    EXPECT_EQ(config.log_directory, nullptr);
    EXPECT_TRUE(config.enable_hotkeys);
    EXPECT_TRUE(config.enable_system_tray);
    EXPECT_FALSE(config.start_minimized);
    EXPECT_EQ(ce_engine_config_init_default(nullptr), CE_ERROR_INVALID_ARGUMENT);
    ce_engine_t* other = nullptr;
    config.struct_size = 4;
    EXPECT_EQ(ce_engine_create(&config, &other), CE_ERROR_INVALID_ARGUMENT);
    config.struct_size = sizeof(config);
    config.log_level = static_cast<ce_log_level_t>(7);
    EXPECT_EQ(ce_engine_create(&config, &other), CE_ERROR_INVALID_ARGUMENT);
    for (int field = 0; field < 6; ++field) {
        ce_engine_config_init_default(&config);
        switch (field) {
            case 0:
                config.config_file_path = "custom.ini";
                break;
            case 1:
                config.log_directory = "custom";
                break;
            case 2:
                config.log_level = CE_LOG_LEVEL_DEBUG;
                break;
            case 3:
                config.enable_hotkeys = false;
                break;
            case 4:
                config.enable_system_tray = false;
                break;
            case 5:
                config.start_minimized = true;
                break;
            default:
                FAIL() << "Unknown configuration field";
        }
        EXPECT_EQ(ce_engine_create(&config, &other), CE_ERROR_UNSUPPORTED);
        EXPECT_EQ(other, nullptr);
    }
}

TEST_F(EngineApiTest, ValidatesRecordingIntentAndReportsBackendFailures) {
    EXPECT_EQ(ce_engine_start_recording(testEngine, CE_RECORDING_INTENT_IDLE, nullptr), CE_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(ce_engine_start_recording(testEngine, static_cast<ce_recording_intent_t>(3), nullptr),
              CE_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(testCalls, 0);
    EXPECT_EQ(ce_engine_start_recording(testEngine, CE_RECORDING_INTENT_AUDIO_ONLY, "test start"), CE_SUCCESS);
    EXPECT_STREQ(testReason, "test start");
    bool recording = false;
    EXPECT_EQ(ce_engine_is_recording(testEngine, &recording), CE_SUCCESS);
    EXPECT_TRUE(recording);
    EXPECT_EQ(ce_engine_stop_recording(testEngine, "test stop"), CE_SUCCESS);
    EXPECT_STREQ(testReason, "test stop");
    EXPECT_EQ(ce_engine_is_recording(testEngine, &recording), CE_SUCCESS);
    EXPECT_FALSE(recording);
    EXPECT_EQ(ce_engine_is_recording(testEngine, nullptr), CE_ERROR_INVALID_ARGUMENT);
    testResult = CE_ERROR_PROCESS_FAILURE;
    EXPECT_EQ(ce_engine_start_recording(testEngine, CE_RECORDING_INTENT_VIDEO, nullptr), testResult);
    testResult = CE_ERROR_IPC_FAILURE;
    EXPECT_EQ(ce_engine_stop_recording(testEngine, nullptr), testResult);
    EXPECT_EQ(ce_engine_toggle_recording(testEngine), testResult);
    EXPECT_EQ(testCommand, ce::api::Command::ToggleVideo);
    EXPECT_EQ(ce_engine_toggle_audio_only(testEngine), testResult);
    EXPECT_EQ(testCommand, ce::api::Command::ToggleAudio);
    EXPECT_EQ(ce_engine_toggle_overlay(testEngine), testResult);
    EXPECT_EQ(ce_engine_toggle_benchmark(testEngine), testResult);
    testResult = CE_ERROR_IO_FAILURE;
    EXPECT_EQ(ce_engine_take_screenshot(testEngine), testResult);
}

TEST_F(EngineApiTest, StatisticsRequireTheExpectedSizeAndNeverFabricateTelemetry) {
    EXPECT_EQ(ce_engine_get_recording_stats(testEngine, nullptr), CE_ERROR_INVALID_ARGUMENT);
    struct SmallStats {
        uint32_t size;
        uint32_t canary;
    } small{sizeof(SmallStats), 0x12345678};
    EXPECT_EQ(ce_engine_get_recording_stats(testEngine, reinterpret_cast<ce_recording_stats_t*>(&small)),
              CE_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(small.canary, 0x12345678u);
    ce_recording_stats_t stats{};
    stats.struct_size = sizeof(stats);
    stats.recorded_frames = 123;
    stats.duration_seconds = 12.5;
    EXPECT_EQ(ce_engine_get_recording_stats(testEngine, &stats), CE_ERROR_UNSUPPORTED);
    EXPECT_EQ(stats.recorded_frames, 123u);
    EXPECT_DOUBLE_EQ(stats.duration_seconds, 12.5);
}

TEST_F(EngineApiTest, RejectsWrongThreadAccessWithoutTouchingControllerState) {
    std::thread worker([] {
        ce_engine_t* other = nullptr;
        EXPECT_EQ(ce_engine_create(nullptr, &other), CE_ERROR_INVALID_STATE);
        EXPECT_EQ(ce_engine_toggle_recording(testEngine), CE_ERROR_INVALID_STATE);
        EXPECT_EQ(ce_engine_poll_events(testEngine, 0), CE_ERROR_INVALID_STATE);
        EXPECT_EQ(ce_engine_destroy(testEngine), CE_ERROR_INVALID_STATE);
    });
    worker.join();
    EXPECT_EQ(testCalls, 0);
}

TEST_F(EngineApiTest, ExceptionsAndReentrantCommandsCannotEscapeOrDestroyActiveHandles) {
    testThrow = true;
    EXPECT_EQ(ce_engine_toggle_recording(testEngine), CE_ERROR_PROCESS_FAILURE);
    testThrow = false;
    testReenter = true;
    EXPECT_EQ(ce_engine_toggle_recording(testEngine), CE_SUCCESS);
}

TEST_F(EngineApiTest, PollingAllowsHotkeysButRejectsNestedPollingAndDestruction) {
    EXPECT_EQ(ce_engine_poll_events(testEngine, UINT32_MAX), CE_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(testCalls, 0);
    EXPECT_EQ(ce_engine_poll_events(testEngine, 50), CE_SUCCESS);
    EXPECT_EQ(testTimeout, 50u);
    EXPECT_EQ(testCommand, ce::api::Command::Screenshot);
    EXPECT_EQ(testCalls, 1);
}

TEST_F(EngineApiTest, DetachedBackendCannotRunCommands) {
    ce::api::UnbindControllerBackend();
    EXPECT_EQ(ce_engine_toggle_recording(testEngine), CE_ERROR_NOT_INITIALIZED);
    EXPECT_EQ(testCalls, 0);
}

TEST(EngineApiVersionTest, UsesTheRuntimeBuildIdentity) {
    EXPECT_STREQ(ce_version_string(), GetCaptureVersion());
    EXPECT_STREQ(ce_status_to_string(CE_ERROR_UNSUPPORTED), "Unsupported");
    EXPECT_STREQ(ce_status_to_string(static_cast<ce_status_t>(-99)), "Unknown error");
}
