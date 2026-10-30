#include <gtest/gtest.h>

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "common/logging/direct_log_file_sink.h"
#include "common/logging/log_record.h"

namespace {

namespace fs = std::filesystem;
using Append = ce::logging::DirectLogFileSink::Append;

// The resolver is a plain function pointer, as in the hook: it reads the "published" session directory.
std::string g_sessionDirectory;
bool ResolveSessionDirectory(char* directory, size_t directorySize) {
    if (g_sessionDirectory.empty() || g_sessionDirectory.size() >= directorySize)
        return false;
    memcpy(directory, g_sessionDirectory.c_str(), g_sessionDirectory.size() + 1);
    return true;
}

std::vector<bool> g_openedEmpty;
void RecordOpen(HANDLE file, bool emptyFile) {
    g_openedEmpty.push_back(emptyFile);
    if (emptyFile)
        ce::logging::AppendLogRecord(file, "[BUILD] test");
}

ce::logging::DirectLogFileSink* g_reentrantSink = nullptr;
Append g_reentrantResult = Append::kWritten;
void AppendFromInsideTheSink(HANDLE, bool) {
    g_reentrantResult = g_reentrantSink->TryAppend("hook_debug.log", "re-entrant");
}

std::string ReadText(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

class DirectLogFileSinkTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Each test owns its directory; Windows may still be retiring a previous
        // test's files after its sink has closed all handles.
        const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
        root = fs::temp_directory_path() /
               ("ce_direct_log_sink_" + std::to_string(GetCurrentProcessId()) + "_" + test->name());
        fs::remove_all(root);
        fs::create_directories(root);
        first = root / "20261025_211430";
        second = root / "20261025_212736";
        g_sessionDirectory = first.string();
        g_openedEmpty.clear();
    }
    void TearDown() override {
        g_sessionDirectory.clear();
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    fs::path root;
    fs::path first;
    fs::path second;
};

}  // namespace

// Session 20261025_212736: after a replacement CaptureEngine reactivated the resident hook, 2708 of its
// 2725 lines went to the previous session's hook_debug.log, because the direct writer cached the first
// session's directory for the life of the process.
TEST_F(DirectLogFileSinkTest, RebindMovesLaterRecordsIntoTheNewSessionDirectory) {
    ce::logging::DirectLogFileSink sink(&ResolveSessionDirectory, &RecordOpen);
    ASSERT_EQ(sink.TryAppend("hook_debug.log", "before the host restart"), Append::kWritten);

    g_sessionDirectory = second.string();  // the replacement host published its session
    ASSERT_EQ(sink.TryAppend("hook_debug.log", "still the cached session"), Append::kWritten);
    EXPECT_FALSE(fs::exists(second / "hook_debug.log")) << "the directory is bound until Rebind";

    EXPECT_EQ(sink.Rebind(), first.string());
    ASSERT_EQ(sink.TryAppend("hook_debug.log", "after the reconnect"), Append::kWritten);

    const std::string old = ReadText(first / "hook_debug.log");
    const std::string fresh = ReadText(second / "hook_debug.log");
    EXPECT_NE(old.find("before the host restart"), std::string::npos);
    EXPECT_EQ(old.find("after the reconnect"), std::string::npos);
    EXPECT_NE(fresh.find("after the reconnect"), std::string::npos);
    EXPECT_EQ(fresh.find("before the host restart"), std::string::npos);
    EXPECT_EQ(fresh.rfind("[BUILD] test\r\n", 0), 0u) << "the new session's file gets its preamble";
    EXPECT_EQ(g_openedEmpty, (std::vector<bool>{true, true}));

    // Rebind released the previous session's handle: that folder can be deleted while the sink lives.
    std::error_code removeError;
    fs::remove_all(first, removeError);
    EXPECT_FALSE(removeError) << removeError.message();
}

TEST_F(DirectLogFileSinkTest, PreambleSeesWhetherAnotherWriterAlreadyCreatedTheFile) {
    fs::create_directories(first);
    std::ofstream(first / "hook_debug.log", std::ios::binary) << "logger service line\r\n";
    ce::logging::DirectLogFileSink sink(&ResolveSessionDirectory, &RecordOpen);
    ASSERT_EQ(sink.TryAppend("hook_debug.log", "direct line"), Append::kWritten);
    ASSERT_EQ(sink.TryAppend("hook_debug.log", "cached handle"), Append::kWritten);
    EXPECT_EQ(g_openedEmpty, (std::vector<bool>{false})) << "one open per file and binding";
    EXPECT_EQ(ReadText(first / "hook_debug.log"), "logger service line\r\ndirect line\r\ncached handle\r\n");
}

TEST_F(DirectLogFileSinkTest, LostRecordsAreAnnouncedBeforeTheNextWrittenRecord) {
    ce::logging::DirectLogFileSink sink(&ResolveSessionDirectory, nullptr);
    sink.NoteLost();
    sink.NoteLost();
    ASSERT_EQ(sink.TryAppend("hook_debug.log", "next line"), Append::kWritten);
    ASSERT_EQ(sink.TryAppend("hook_debug.log", "following line"), Append::kWritten);
    EXPECT_EQ(ReadText(first / "hook_debug.log"),
              "[LOGGING] 2 line(s) dropped after a log path/open/write failure\r\nnext line\r\nfollowing line\r\n");
}

TEST_F(DirectLogFileSinkTest, UnresolvableDirectoryFailsAndIsRetriedLater) {
    g_sessionDirectory.clear();
    ce::logging::DirectLogFileSink sink(&ResolveSessionDirectory, nullptr);
    EXPECT_EQ(sink.TryAppend("hook_debug.log", "no session yet"), Append::kFailed);
    g_sessionDirectory = first.string();
    EXPECT_EQ(sink.TryAppend("hook_debug.log", "session published"), Append::kWritten);
    EXPECT_EQ(ReadText(first / "hook_debug.log"), "session published\r\n");
}

// The hook's writer never waits: a caller already inside the sink (re-entrancy through a hooked API)
// gets kBusy and writes through its own handle instead of deadlocking.
TEST_F(DirectLogFileSinkTest, ReentrantAppendReportsBusyInsteadOfBlocking) {
    ce::logging::DirectLogFileSink sink(&ResolveSessionDirectory, &AppendFromInsideTheSink);
    g_reentrantSink = &sink;
    g_reentrantResult = Append::kWritten;
    EXPECT_EQ(sink.TryAppend("hook_debug.log", "outer"), Append::kWritten);
    EXPECT_EQ(g_reentrantResult, Append::kBusy);
    g_reentrantSink = nullptr;
}
