#include <gtest/gtest.h>

#include "captureengine/app/configuration_state.h"
#include "captureengine/app/runtime_configuration.h"

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>

namespace {
using namespace ce::runtime::detail;
using ce::config_reload::FileIdentity;

class ConfigFiles final : public ConfigurationFiles {
public:
    uint32_t NowMs() const noexcept override {
        return now;
    }
    FileIdentity Identity() const override {
        return identity;
    }
    ce::config_reload::LoadEvidence Load(AppConfig& candidate, FileIdentity observed) override {
        ++loads;
        candidate.video.outputDir = "new-settings";
        if (onLoad)
            onLoad();
        return {readable, failures, observed, identity};
    }
    void Pending(FileIdentity) override {
        ++pending;
    }
    void Deferred(const ce::config_reload::LoadEvidence&) override {
        ++deferred;
    }
    uint32_t now = 0;
    FileIdentity identity{true, 2, 200};
    unsigned loads = 0;
    unsigned pending = 0;
    unsigned deferred = 0;
    bool readable = true;
    uint64_t failures = 0;
    std::function<void()> onLoad;
};
AppConfig InitialSettings() {
    AppConfig initial;
    initial.video.outputDir = "loaded-settings";
    initial.logFilePath = "host.log";
    return initial;
}
class ConfigurationStateTest : public ::testing::Test {
protected:
    std::optional<AppConfig> Check() {
        files.now += state.WaitMs(files.now);
        return state.Poll(files);
    }
    ConfigFiles files;
    ConfigurationState state{InitialSettings(), {true, 1, 100}, 0};
};

TEST_F(ConfigurationStateTest, ChangeBetweenStartupLoadAndFirstPollIsNotMistakenForAppliedSettings) {
    EXPECT_FALSE(Check());
    EXPECT_EQ(state.Current().video.outputDir, "loaded-settings");
    EXPECT_EQ(state.WaitMs(files.now), 250u);
    auto old = Check();
    ASSERT_TRUE(old);
    if (old.has_value())
        EXPECT_EQ(old->video.outputDir, "loaded-settings");
    EXPECT_EQ(state.Current().video.outputDir, "new-settings");
    EXPECT_EQ(state.Current().logFilePath, "host.log");
    EXPECT_EQ(state.WaitMs(files.now), 1000u);
    EXPECT_FALSE(Check());
    EXPECT_EQ(files.loads, 1u);
}

TEST_F(ConfigurationStateTest, UnreadableCandidateCannotPublishOrCommitAndAStableRetrySucceeds) {
    files.readable = false;
    EXPECT_FALSE(Check());
    EXPECT_FALSE(Check());
    EXPECT_EQ(state.Current().video.outputDir, "loaded-settings");
    EXPECT_EQ(files.deferred, 1u);
    files.readable = true;
    EXPECT_FALSE(Check());
    ASSERT_TRUE(Check());
    EXPECT_EQ(files.loads, 2u);
}

TEST_F(ConfigurationStateTest, ChangedFileDuringLoadDiscardsTheCompleteCandidate) {
    EXPECT_FALSE(Check());
    files.onLoad = [&] { files.identity = {true, 3, 300}; };
    EXPECT_FALSE(Check());
    EXPECT_EQ(state.Current().video.outputDir, "loaded-settings");
    files.onLoad = {};
    EXPECT_FALSE(Check());
    EXPECT_TRUE(Check());
    EXPECT_EQ(files.loads, 2u);
}

TEST_F(ConfigurationStateTest, ReadFailuresKeepThePriorSnapshotAndRemainRetryable) {
    files.failures = 1;
    EXPECT_FALSE(Check());
    EXPECT_FALSE(Check());
    EXPECT_EQ(state.Current().video.outputDir, "loaded-settings");
    files.failures = 0;
    EXPECT_FALSE(Check());
    EXPECT_TRUE(Check());
}

TEST_F(ConfigurationStateTest, MissingAndEmptyFilesNeverReplaceThePublishedSettings) {
    files.identity = {};
    EXPECT_FALSE(Check());
    EXPECT_FALSE(Check());
    files.identity = {true, 2, 0};
    EXPECT_FALSE(Check());
    EXPECT_EQ(files.loads, 0u);
    files.identity = {true, 3, 100};
    EXPECT_FALSE(Check());
    EXPECT_TRUE(Check());
}

TEST_F(ConfigurationStateTest, SchedulingIsOwnedAndTickWrapDoesNotSkipChanges) {
    ConfigurationState wrapped(InitialSettings(), {true, 1, 100}, UINT32_MAX - 99);
    files.now = 50;
    EXPECT_EQ(wrapped.WaitMs(files.now), 850u);
    EXPECT_FALSE(wrapped.Poll(files));
    EXPECT_EQ(files.pending, 0u);
    files.now = 900;
    EXPECT_FALSE(wrapped.Poll(files));
    files.now = 1150;
    EXPECT_TRUE(wrapped.Poll(files));
    EXPECT_EQ(files.loads, 1u);
}

TEST_F(ConfigurationStateTest, RecursivePollingCannotLoadOrPublishTwice) {
    EXPECT_FALSE(Check());
    bool reentered = false;
    files.onLoad = [&] {
        if (!reentered) {
            reentered = true;
            EXPECT_FALSE(state.Poll(files));
            EXPECT_EQ(state.Current().video.outputDir, "loaded-settings");
        }
    };
    EXPECT_TRUE(Check());
    EXPECT_EQ(files.loads, 1u);
}

TEST_F(ConfigurationStateTest, ThrowingLoadLeavesTheSnapshotAndIdentityAvailableForRetry) {
    EXPECT_FALSE(Check());
    files.onLoad = [] { throw std::runtime_error("controlled read failure"); };
    EXPECT_THROW(Check(), std::runtime_error);
    EXPECT_EQ(state.Current().video.outputDir, "loaded-settings");
    files.onLoad = {};
    EXPECT_TRUE(Check());
    EXPECT_EQ(files.loads, 2u);
}

uint32_t nativeNow = 0;
uint32_t NativeNow() noexcept {
    return nativeNow;
}
class RuntimeConfigurationTest : public ::testing::Test {
protected:
    void SetUp() override {
        nativeNow = 0;
        path = std::filesystem::absolute("runtime_config_fixture." + std::to_string(GetCurrentProcessId()) + ".ini")
                   .string();
        Write("fixture-output");
    }
    void TearDown() override {
        EXPECT_TRUE(DeleteFileA(path.c_str()));
    }
    void Write(const char* output) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file);
        file << "[Output]\r\noutput_dir=" << output << "\r\n";
        ASSERT_TRUE(file);
    }
    std::string path;
};

TEST_F(RuntimeConfigurationTest, ActualHeadlessOwnerLoadsAndReloadsWithoutControllerMainOrTray) {
    ScopedQuietConfigLog quiet;
    ce::runtime::RuntimeConfigurationSession owner(path, NativeNow);
    ASSERT_TRUE(owner.IsReady());
    EXPECT_EQ(ce::runtime::RuntimeConfiguration().video.outputDir, "fixture-output");
    ce::runtime::SetRuntimeProcessLogPath("host-log-fixture");
    Write("fresh-settings-output");  // Size changes; no timestamp-resolution assumption.
    nativeNow = 1000;
    EXPECT_FALSE(ce::runtime::PollRuntimeConfiguration());
    EXPECT_EQ(ce::runtime::RuntimeConfigurationWaitMs(), 250u);
    EXPECT_EQ(ce::runtime::RuntimeConfiguration().video.outputDir, "fixture-output");
    nativeNow = 1250;
    auto old = ce::runtime::PollRuntimeConfiguration();
    ASSERT_TRUE(old);
    if (old.has_value())
        EXPECT_EQ(old->video.outputDir, "fixture-output");
    EXPECT_EQ(ce::runtime::RuntimeConfiguration().video.outputDir, "fresh-settings-output");
    EXPECT_EQ(ce::runtime::RuntimeConfiguration().logFilePath, "host-log-fixture");
}

TEST_F(RuntimeConfigurationTest, CompetingOwnerIsRejectedAndAClosedScopeCanBeRecreated) {
    ScopedQuietConfigLog quiet;
    {
        ce::runtime::RuntimeConfigurationSession owner(path, NativeNow);
        ASSERT_TRUE(owner.IsReady());
        ce::runtime::RuntimeConfigurationSession rejected(path, NativeNow);
        EXPECT_FALSE(rejected.IsReady());
        EXPECT_EQ(ce::runtime::RuntimeConfiguration().video.outputDir, "fixture-output");
    }
    EXPECT_THROW(ce::runtime::RuntimeConfiguration(), std::logic_error);
    ce::runtime::RuntimeConfigurationSession next(path, NativeNow);
    EXPECT_TRUE(next.IsReady());
}
}  // namespace
