#include "direct_log_file_sink.h"

#include <cstdio>

#include "log_record.h"

namespace ce::logging {

DirectLogFileSink::DirectLogFileSink(DirectoryResolver resolveDirectory, FileOpened onOpened) noexcept
    : resolveDirectory_(resolveDirectory), onOpened_(onOpened) {}

DirectLogFileSink::~DirectLogFileSink() {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseHandlesLocked();
}

DirectLogFileSink::Append DirectLogFileSink::TryAppend(std::string_view fileName, std::string_view record) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return Append::kBusy;
    return AppendLocked(fileName, record);
}

DirectLogFileSink::Append DirectLogFileSink::Write(std::string_view fileName, std::string_view record) {
    std::lock_guard<std::mutex> lock(mutex_);
    return AppendLocked(fileName, record);
}

void DirectLogFileSink::NoteLost() noexcept {
    lostRecords_.fetch_add(1, std::memory_order_relaxed);
}

std::string DirectLogFileSink::Rebind() {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseHandlesLocked();
    std::string previous;
    previous.swap(directory_);
    return previous;
}

DirectLogFileSink::Append DirectLogFileSink::AppendLocked(std::string_view fileName, std::string_view record) {
    if (directory_.empty()) {
        char directory[MAX_PATH] = {};
        if (!resolveDirectory_ || !resolveDirectory_(directory, sizeof(directory)) || directory[0] == '\0')
            return Append::kFailed;
        CreateDirectoryA(directory, nullptr);
        directory_ = directory;
    }

    const std::string key(fileName);
    HANDLE file = INVALID_HANDLE_VALUE;
    if (const auto cached = handles_.find(key); cached != handles_.end()) {
        file = cached->second;
    } else {
        char path[MAX_PATH] = {};
        const int length = snprintf(path, sizeof(path), "%s\\%.*s", directory_.c_str(),
                                    static_cast<int>(fileName.size()), fileName.data());
        if (length <= 0 || length >= static_cast<int>(sizeof(path)))
            return Append::kFailed;
        file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return Append::kFailed;
        if (onOpened_) {
            LARGE_INTEGER size{};
            onOpened_(file, GetFileSizeEx(file, &size) && size.QuadPart == 0);
        }
        handles_.emplace(key, file);
    }

    // Restore the loss count if its own line cannot be written either.
    const uint32_t lost = lostRecords_.exchange(0, std::memory_order_relaxed);
    if (lost != 0) {
        char lostLine[128];
        const int length = snprintf(lostLine, sizeof(lostLine),
                                    "[LOGGING] %u line(s) dropped after a log path/open/write failure", lost);
        if (length <= 0 || !AppendLogRecord(file, std::string_view(lostLine, static_cast<size_t>(length))))
            lostRecords_.fetch_add(lost, std::memory_order_relaxed);
    }

    if (!AppendLogRecord(file, record)) {
        CloseHandle(file);
        handles_.erase(key);
        return Append::kFailed;
    }
    return Append::kWritten;
}

void DirectLogFileSink::CloseHandlesLocked() {
    for (auto& entry : handles_) {
        if (entry.second != INVALID_HANDLE_VALUE)
            CloseHandle(entry.second);
    }
    handles_.clear();
}

}  // namespace ce::logging
