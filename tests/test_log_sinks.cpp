#include "test_config_shared.h"

#include <cstdio>
#include <string>
#include <thread>

#include "common/logging/logging.h"

// Thread-bound log sinks (DR-40): a role running on a runtime thread writes its own file, every other
// thread keeps writing the process log, and threads a role starts inherit its sink explicitly.

namespace {

bool Contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

class LogSinkTest : public ::testing::Test {
protected:
    void SetUp() override {
        processLog_ = MakeTestPath("test_log_sinks_process.log");
        roleLog_ = MakeTestPath("test_log_sinks_role.log");
        otherLog_ = MakeTestPath("test_log_sinks_other.log");
        Log_Init(processLog_, LogLevel::Debug);
    }
    void TearDown() override {
        Log_Shutdown();
        Log_SetLevel(LogLevel::Trace);
        remove(processLog_.c_str());
        remove(roleLog_.c_str());
        remove(otherLog_.c_str());
    }
    // Closes the process log so its content can be read; sinks close when their last reference goes.
    std::string CloseProcessLog() {
        Log_Shutdown();
        return ReadTextFile(processLog_);
    }

    std::string processLog_;
    std::string roleLog_;
    std::string otherLog_;
};

}  // namespace

TEST_F(LogSinkTest, BoundThreadWritesItsSinkAndNotTheProcessLog) {
    auto sink = ce::log::OpenSink(roleLog_);
    ASSERT_NE(sink, nullptr);
    {
        ce::log::SinkScope scope(sink);
        LogInfo("role line %d", 1);
    }
    LogInfo("process line %d", 2);
    sink.reset();

    const std::string role = ReadTextFile(roleLog_);
    const std::string process = CloseProcessLog();
    EXPECT_TRUE(Contains(role, "[BUILD] Version=")) << role;
    EXPECT_TRUE(Contains(role, "[INFO] role line 1")) << role;
    EXPECT_FALSE(Contains(role, "process line")) << role;
    EXPECT_TRUE(Contains(process, "[INFO] process line 2")) << process;
    EXPECT_FALSE(Contains(process, "role line")) << process;
}

TEST_F(LogSinkTest, ScopesNestAndRestoreThePreviousBinding) {
    auto role = ce::log::OpenSink(roleLog_);
    auto other = ce::log::OpenSink(otherLog_);
    ASSERT_NE(role, nullptr);
    ASSERT_NE(other, nullptr);
    EXPECT_EQ(ce::log::CurrentSink(), nullptr);
    {
        ce::log::SinkScope outer(role);
        LogWarn("outer before");
        {
            ce::log::SinkScope inner(other);
            EXPECT_EQ(ce::log::CurrentSink(), other);
            LogWarn("inner");
        }
        EXPECT_EQ(ce::log::CurrentSink(), role);
        LogWarn("outer after");
    }
    EXPECT_EQ(ce::log::CurrentSink(), nullptr);
    role.reset();
    other.reset();

    const std::string roleText = ReadTextFile(roleLog_);
    const std::string otherText = ReadTextFile(otherLog_);
    EXPECT_TRUE(Contains(roleText, "outer before")) << roleText;
    EXPECT_TRUE(Contains(roleText, "outer after")) << roleText;
    EXPECT_FALSE(Contains(roleText, "inner")) << roleText;
    EXPECT_TRUE(Contains(otherText, "inner")) << otherText;
}

TEST_F(LogSinkTest, AStartedThreadWritesTheProcessLogUntilItBindsTheInheritedSink) {
    auto sink = ce::log::OpenSink(roleLog_);
    ASSERT_NE(sink, nullptr);
    {
        ce::log::SinkScope scope(sink);
        const std::shared_ptr<ce::log::Sink> inherited = ce::log::CurrentSink();
        std::thread worker([inherited] {
            LogInfo("worker unbound");
            ce::log::SinkScope bound(inherited);
            LogInfo("worker bound");
        });
        worker.join();
    }
    sink.reset();

    const std::string role = ReadTextFile(roleLog_);
    const std::string process = CloseProcessLog();
    EXPECT_TRUE(Contains(role, "worker bound")) << role;
    EXPECT_FALSE(Contains(role, "worker unbound")) << role;
    EXPECT_TRUE(Contains(process, "worker unbound")) << process;
}

TEST_F(LogSinkTest, TheLevelIsProcessWide) {
    auto sink = ce::log::OpenSink(roleLog_);
    ASSERT_NE(sink, nullptr);
    Log_SetLevel(LogLevel::Warn);
    {
        ce::log::SinkScope scope(sink);
        EXPECT_FALSE(Log_IsEnabled(LogLevel::Info));
        LogInfo("filtered");
        LogWarn("kept");
    }
    sink.reset();

    const std::string role = ReadTextFile(roleLog_);
    EXPECT_FALSE(Contains(role, "filtered")) << role;
    EXPECT_TRUE(Contains(role, "[WARN] kept")) << role;
}

TEST_F(LogSinkTest, AnUnopenablePathYieldsNoSink) {
    EXPECT_EQ(ce::log::OpenSink(MakeTestPath("missing_dir_for_log_sinks\\role.log")), nullptr);
}
