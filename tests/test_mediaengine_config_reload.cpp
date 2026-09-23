#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "source_fragment_reader.h"

namespace fs = std::filesystem;

// Regression: MediaEngine::ReloadConfig assigned `config` while a recording was
// live, although the audio thread, the frame path and StopRecording read its
// strings and vectors without muxMutex - a data race that could hand a
// recording thread a freed buffer. A reload during recording is now parked and
// applied (with the same assign-only semantics) when the next recording starts.
TEST(MediaEngineConfigReloadSourceTest, ReloadDuringRecordingIsDeferredNotAssigned) {
    const std::string config = ce::test_source::ReadFile(fs::current_path() / "mediaengine" / "mediaengine_config.cpp");
    const std::string start =
        ce::test_source::ReadFile(fs::current_path() / "mediaengine" / "mediaengine_recording_start.cpp");
    ASSERT_FALSE(config.empty());
    ASSERT_FALSE(start.empty());

    const size_t reload = config.find("void MediaEngine::ReloadConfig(const AppConfig* newConfig)");
    ASSERT_NE(reload, std::string::npos);
    const size_t recordingGate = config.find("if (recording) {", reload);
    const size_t deferred = config.find("deferredConfig = std::make_unique<AppConfig>(*newConfig);", reload);
    const size_t assignment = config.find("this->config = *newConfig;", reload);
    ASSERT_NE(recordingGate, std::string::npos);
    ASSERT_NE(deferred, std::string::npos);
    ASSERT_NE(assignment, std::string::npos);
    EXPECT_LT(recordingGate, deferred);
    EXPECT_LT(deferred, assignment) << "the recording gate must return before `config` is replaced";

    const size_t startEntry = start.find("bool MediaEngine::StartRecording()");
    const size_t apply = start.find("ApplyConfigDeferredDuringRecording();", startEntry);
    const size_t audioOnlyBranch = start.find("if (audioOnly) {", startEntry);
    ASSERT_NE(apply, std::string::npos);
    EXPECT_LT(apply, audioOnlyBranch) << "the parked config must be in place before the recording reads it";
}
