#pragma once

// Best-effort diagnostics for unhandled C++ exceptions. The minimal-first
// minidump normally does NOT include the thrown exception object, so the crash
// handler logs its raw bytes and a decoded message to crash.log directly.
// Works for both MSVC (0xE06D7363) and MinGW/clang (0x20474343) EH runtimes.

#include <cstddef>
#include <cstdint>
#include <string>

namespace ce::crash_diagnostics {

// Returns the longest printable ASCII run (>= 8 chars) inside
// [bytes, bytes+size), trimmed of trailing whitespace; empty when nothing
// plausible is found. Layout-agnostic: it locates the message whether it sits
// inline (small-string storage) or behind a what()-style pointer.
inline std::string ExtractPrintableMessage(const uint8_t* bytes, size_t size) {
    if (!bytes || size == 0) {
        return {};
    }
    size_t bestStart = SIZE_MAX;
    size_t bestLength = 0;
    size_t runStart = SIZE_MAX;
    size_t runLength = 0;
    for (size_t index = 0; index < size; ++index) {
        const bool printable = bytes[index] >= 0x20 && bytes[index] <= 0x7E;
        if (printable) {
            if (runStart == SIZE_MAX) {
                runStart = index;
                runLength = 0;
            }
            ++runLength;
        } else {
            if (runLength >= 8 && runLength > bestLength) {
                bestStart = runStart;
                bestLength = runLength;
            }
            runStart = SIZE_MAX;
            runLength = 0;
        }
    }
    if (runLength >= 8 && runLength > bestLength) {
        bestStart = runStart;
        bestLength = runLength;
    }
    if (bestStart == SIZE_MAX) {
        return {};
    }
    std::string result(reinterpret_cast<const char*>(bytes + bestStart), bestLength);
    while (!result.empty() && (result.back() == ' ' || result.back() == '\t')) {
        result.pop_back();
    }
    return result;
}

// Windows: logs the C++ exception object for codes 0xE06D7363 / 0x20474343.
// Safe on the crashing thread: every read is bounded and VirtualQuery-guarded.
// No-op when the record carries no usable object pointer. exceptionRecord must
// point to an EXCEPTION_RECORD.
void LogCppExceptionDiagnostics(const void* exceptionRecord);

}  // namespace ce::crash_diagnostics
