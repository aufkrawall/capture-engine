// C++ exception object logging for the crash handler (see cpp_exception_message.h).

#include "cpp_exception_message.h"

// clang-format off
#include <windows.h>
// clang-format on

#include <cstdio>
#include <cstring>

#include "crash_handler_internal.h"

namespace {

bool IsReadableRange(const void* address, size_t size) {
    if (!address || size == 0) {
        return false;
    }
    const uintptr_t begin = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = begin + size;
    if (end < begin) {
        return false;  // overflow
    }
    for (uintptr_t cursor = begin; cursor < end;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == 0) {
            return false;
        }
        const DWORD readableProtection =
            PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
            PAGE_EXECUTE_WRITECOPY;
        if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 ||
            (info.Protect & readableProtection) == 0) {
            return false;
        }
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (regionEnd <= cursor) {
            return false;
        }
        cursor = regionEnd > end ? end : regionEnd;
    }
    return true;
}

bool ReadBounded(const void* address, uint8_t* out, size_t size) {
    if (!IsReadableRange(address, size)) {
        return false;
    }
    std::memcpy(out, address, size);
    return true;
}

std::string FormatHex(const uint8_t* bytes, size_t size) {
    std::string result;
    result.reserve(size * 3 + 1);
    char buffer[4];
    for (size_t index = 0; index < size; ++index) {
        std::snprintf(buffer, sizeof(buffer), "%02X ", bytes[index]);
        result += buffer;
    }
    return result;
}

}  // namespace

namespace ce::crash_diagnostics {

void LogCppExceptionDiagnostics(const void* exceptionRecord) {
    const EXCEPTION_RECORD* record = static_cast<const EXCEPTION_RECORD*>(exceptionRecord);
    if (!record || record->NumberParameters < 2) {
        return;
    }
    const DWORD code = record->ExceptionCode;
    if (code != 0xE06D7363 && code != 0x20474343) {
        return;
    }
    const uintptr_t objectAddress = static_cast<uintptr_t>(record->ExceptionInformation[1]);
    if (objectAddress == 0) {
        return;
    }

    constexpr size_t kObjectBytes = 128;
    uint8_t objectBytes[kObjectBytes]{};
    const bool haveObject = ReadBounded(reinterpret_cast<const void*>(objectAddress), objectBytes, kObjectBytes);

    std::string message;
    if (haveObject) {
        message = ExtractPrintableMessage(objectBytes, kObjectBytes);
        // Dereference the common what()-style pointer at +8 (MSVC std::string
        // data pointer / libc++ __libcpp_refstring payload).
        uintptr_t messagePointer = 0;
        std::memcpy(&messagePointer, objectBytes + 8, sizeof(messagePointer));
        if (messagePointer >= 0x10000 && messagePointer <= 0x7FFFFFFFFFFFULL) {
            uint8_t messageBytes[512]{};
            if (ReadBounded(reinterpret_cast<const void*>(messagePointer), messageBytes, sizeof(messageBytes))) {
                const std::string dereferenced = ExtractPrintableMessage(messageBytes, sizeof(messageBytes));
                if (!dereferenced.empty()) {
                    message = dereferenced;
                }
            }
        }
    }

    char header[256];
    std::snprintf(header, sizeof(header), "[CrashHandler] C++ exception 0x%08lX object at 0x%llX message=\"%s\"", code,
                  static_cast<unsigned long long>(objectAddress), message.c_str());
    TraceCrash(header);
    if (haveObject) {
        const std::string hex = FormatHex(objectBytes, kObjectBytes);
        std::string hexLine = "[CrashHandler] C++ exception object bytes: " + hex;
        TraceCrash(hexLine.c_str());
    }
}

}  // namespace ce::crash_diagnostics
