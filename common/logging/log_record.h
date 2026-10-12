#pragma once

#include <windows.h>
#include <cstring>
#include <string_view>

namespace ce::logging {

inline constexpr size_t kMaxLogRecordBytes = 8192;

// All append handles sharing a file must submit the terminator with the message.
// A separate newline write lets another process splice its record into this one.
inline bool AppendLogRecord(HANDLE file, std::string_view message) {
    if (message.size() > kMaxLogRecordBytes) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return false;
    }
    char record[kMaxLogRecordBytes + 2];
    if (!message.empty()) {
        std::memcpy(record, message.data(), message.size());
    }
    record[message.size()] = '\r';
    record[message.size() + 1] = '\n';
    const DWORD length = static_cast<DWORD>(message.size() + 2);
    DWORD written = 0;
    if (!WriteFile(file, record, length, &written, nullptr)) {
        return false;
    }
    if (written != length) {
        SetLastError(ERROR_WRITE_FAULT);
        return false;
    }
    return true;
}

}  // namespace ce::logging
