#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unordered_set>

#include "common/logging/log_record.h"
#include "source_fragment_reader.h"

TEST(LogRecordTest, IndependentAppendHandlesKeepConcurrentRecordsWhole) {
    wchar_t directory[MAX_PATH] = {};
    wchar_t path[MAX_PATH] = {};
    ASSERT_GT(GetTempPathW(MAX_PATH, directory), 0u);
    ASSERT_NE(GetTempFileNameW(directory, L"cel", 0, path), 0u);
    struct Cleanup {
        const wchar_t* path;
        HANDLE first = INVALID_HANDLE_VALUE;
        HANDLE second = INVALID_HANDLE_VALUE;
        ~Cleanup() {
            if (first != INVALID_HANDLE_VALUE) CloseHandle(first);
            if (second != INVALID_HANDLE_VALUE) CloseHandle(second);
            DeleteFileW(path);
        }
    } cleanup{path};
    cleanup.first = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    cleanup.second = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(cleanup.first, INVALID_HANDLE_VALUE);
    ASSERT_NE(cleanup.second, INVALID_HANDLE_VALUE);
    std::atomic<bool> succeeded{true};
    const auto write = [&](HANDLE handle, char prefix) {
        for (int index = 0; index < 200; ++index) {
            const std::string line = std::string(1, prefix) + std::to_string(index) + ":" + std::string(700, prefix);
            if (!ce::logging::AppendLogRecord(handle, line)) succeeded.store(false);
        }
    };
    std::thread first(write, cleanup.first, 'A');
    std::thread second(write, cleanup.second, 'B');
    first.join();
    second.join();
    ASSERT_TRUE(succeeded.load());
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    std::unordered_set<std::string> actual;
    std::string line;
    while (std::getline(file, line)) {
        ASSERT_FALSE(line.empty());
        ASSERT_EQ(line.back(), '\r');
        line.pop_back();
        EXPECT_TRUE(actual.insert(line).second);
    }
    EXPECT_EQ(actual.size(), 400u);
    for (const char prefix : {'A', 'B'}) {
        for (int index = 0; index < 200; ++index) {
            EXPECT_EQ(actual.count(std::string(1, prefix) + std::to_string(index) + ":" + std::string(700, prefix)), 1u);
        }
    }
}

TEST(LogRecordTest, InvalidHandleAndOversizedRecordReportFailure) {
    EXPECT_FALSE(ce::logging::AppendLogRecord(INVALID_HANDLE_VALUE, "message"));
    EXPECT_EQ(GetLastError(), ERROR_INVALID_HANDLE);
    EXPECT_FALSE(ce::logging::AppendLogRecord(INVALID_HANDLE_VALUE,
                                             std::string(ce::logging::kMaxLogRecordBytes + 1, 'x')));
    EXPECT_EQ(GetLastError(), ERROR_INSUFFICIENT_BUFFER);
}

TEST(LogRecordSourceTest, HookAndServiceAppendCompleteRecordsWithCheckedWrites) {
    for (const char* path : {"hook/runtime/hook_common.cpp", "captureengine/diagnostics/logger_service.cpp"}) {
        const auto source = ce::test_source::ReadLogicalSource(std::filesystem::current_path() / path);
        EXPECT_NE(source.find("ce::logging::AppendLogRecord("), std::string::npos) << path;
        EXPECT_EQ(source.find("WriteFile(hFile, \"\\r\\n\""), std::string::npos) << path;
    }
}
