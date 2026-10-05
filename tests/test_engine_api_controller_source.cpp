#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "source_fragment_reader.h"

namespace {
std::string Source(const char* name) {
    return ce::test_source::ReadLogicalSource(ce::test_source::FindSource("captureengine", name));
}
}  // namespace

TEST(EngineApiControllerSourceTest, FrontendsUseTheOwnedRecordingSession) {
    const auto recording = Source("main_recording.cpp");
    const auto api = Source("libcaptureengine_controller.cpp");
    EXPECT_NE(recording.find("ToggleControllerRecording(RecordingStartIntent::Video, \"record hotkey\")"), std::string::npos);
    EXPECT_NE(recording.find("ToggleControllerRecording(RecordingStartIntent::AudioOnly, \"audio-only hotkey\")"), std::string::npos);
    EXPECT_NE(recording.find("ToggleControllerRecording(RecordingStartIntent::Video"), std::string::npos);
    EXPECT_NE(api.find("StartControllerRecording("), std::string::npos);
    EXPECT_NE(api.find("StopControllerRecording("), std::string::npos);
    EXPECT_NE(api.find("ToggleControllerRecording("), std::string::npos);
    EXPECT_NE(Source("main_entry.cpp").find("ControllerRecordingSessionScope recordingSession"), std::string::npos);
    for (const auto& source : {recording, api, Source("main_internal.h")}) {
        EXPECT_EQ(source.find("main_g_Recording"), std::string::npos);
        EXPECT_EQ(source.find("main_g_LiveStreamRecording"), std::string::npos);
    }
}

TEST(EngineApiControllerSourceTest, PollUsesControllerThreadDispatchAndAnEventWait) {
    const std::string source = Source("libcaptureengine_controller.cpp");
    const size_t start = source.find("ce_status_t PollControllerEvents(");
    const size_t end = source.find("}  // namespace", start);
    ASSERT_NE(start, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    const std::string body = source.substr(start, end - start);
    EXPECT_NE(body.find("MsgWaitForMultipleObjectsEx"), std::string::npos);
    EXPECT_NE(body.find("MWMO_INPUTAVAILABLE"), std::string::npos);
    EXPECT_NE(body.find("DispatchControllerMessage(msg)"), std::string::npos);
    EXPECT_NE(body.find("timeoutMs == INFINITE"), std::string::npos);
    EXPECT_EQ(body.find("Sleep("), std::string::npos);
    const std::string main = Source("main_entry.cpp");
    EXPECT_NE(main.find("void DispatchControllerMessage(const MSG& msg)"), std::string::npos);
    EXPECT_NE(main.find("ce_engine_take_screenshot(GetControllerEngine())"), std::string::npos);
    EXPECT_EQ(main.find("ce_engine_take_screenshot(nullptr)"), std::string::npos);
}

TEST(MediaEngineFrameAbiSourceTest, LoaderUsesDescriptorExportsAndClearsRejectedPointers) {
    const std::string source = Source("mediaengine_loader.cpp");
    EXPECT_NE(source.find("\"MediaEngine_SubmitFrame\", &MediaEngine_ProcessFrame"), std::string::npos);
    EXPECT_NE(source.find("\"MediaEngine_SubmitFrameD3D11\", &MediaEngine_ProcessFrameD3D11.raw"), std::string::npos);
    const size_t rejection = source.find("if (!success)");
    ASSERT_NE(rejection, std::string::npos);
    const size_t cleared = source.find("MediaEngine_Unload();", rejection);
    const size_t success = source.find("All function pointers resolved", rejection);
    ASSERT_NE(cleared, std::string::npos);
    EXPECT_LT(cleared, success);
}

TEST(EngineApiControllerSourceTest, InitializationFailureStillRunsControllerResourceCleanup) {
    const std::string source = Source("main_entry.cpp");
    const size_t start = source.find("if (!controllerApi.IsReady())");
    const size_t end = source.find("\n    }", start);
    ASSERT_NE(start, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    const std::string failure = source.substr(start, end - start);
    EXPECT_NE(failure.find("main_g_Running = false"), std::string::npos);
    EXPECT_EQ(failure.find("return"), std::string::npos);
    const size_t exit = source.find("return controllerApi.IsReady() ? 0 : 1", end);
    ASSERT_NE(exit, std::string::npos);
    for (const char* cleanup :
         {"StopHotkeyInputHook();", "ce::startup::Shutdown();", "ShutdownChildProcesses();", "tray->Remove();"}) {
        const size_t pos = source.find(cleanup, end);
        ASSERT_NE(pos, std::string::npos) << cleanup;
        EXPECT_LT(pos, exit) << cleanup;
    }
}
