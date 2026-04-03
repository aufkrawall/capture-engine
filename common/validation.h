#pragma once

// Validation and Assertion Framework for CaptureEngine
// Provides macros for input validation, error checking, and debug assertions.
// All macros are designed for minimal overhead in release builds.

#include <atomic>
#include <cstdint>
#include <cstdio>

// Forward declaration for logging (defined in logging.h or hook_common.h)
#if defined(__GNUC__) || defined(__clang__)
#define CE_PRINTF_FORMAT(fmt_index, first_arg) __attribute__((format(printf, fmt_index, first_arg)))
#else
#define CE_PRINTF_FORMAT(fmt_index, first_arg)
#endif

void CE_LogImpl(const char* level, const char* module, int line, const char* fmt, ...) CE_PRINTF_FORMAT(4, 5);

// Global debug logging flag (set from config.ini)
extern std::atomic<bool> g_DebugLoggingEnabled;

// Compact log format: [CE:L:M] msg  or [CE:L:M:line] msg for errors
// L = Level (D/I/W/E), M = Module

#define CE_LOG_DEBUG(module, fmt, ...)                      \
    do {                                                    \
        if (g_DebugLoggingEnabled)                          \
            CE_LogImpl("D", module, 0, fmt, ##__VA_ARGS__); \
    } while (0)

#define CE_LOG_INFO(module, fmt, ...) CE_LogImpl("I", module, 0, fmt, ##__VA_ARGS__)

#define CE_LOG_WARN(module, fmt, ...) CE_LogImpl("W", module, 0, fmt, ##__VA_ARGS__)

#define CE_LOG_ERROR(module, fmt, ...) CE_LogImpl("E", module, __LINE__, fmt, ##__VA_ARGS__)

// CE_VALIDATE - Validates condition, logs error and returns false on failure
// Use in functions that return bool
#define CE_VALIDATE(cond, msg)              \
    do {                                    \
        if (!(cond)) {                      \
            CE_LOG_ERROR("Val", "%s", msg); \
            return false;                   \
        }                                   \
    } while (0)

// CE_VALIDATE_RET - Validates condition, returns custom value on failure
#define CE_VALIDATE_RET(cond, msg, ret_val) \
    do {                                    \
        if (!(cond)) {                      \
            CE_LOG_ERROR("Val", "%s", msg); \
            return ret_val;                 \
        }                                   \
    } while (0)

// CE_VALIDATE_HR - Validates HRESULT, logs and returns false on failure
#define CE_VALIDATE_HR(hr, msg)                                     \
    do {                                                            \
        HRESULT _ce_hr = (hr);                                      \
        if (FAILED(_ce_hr)) {                                       \
            CE_LOG_ERROR("HR", "0x%08X %s", (unsigned)_ce_hr, msg); \
            return false;                                           \
        }                                                           \
    } while (0)

// CE_VALIDATE_HR_RET - Validates HRESULT, returns custom value on failure
#define CE_VALIDATE_HR_RET(hr, msg, ret_val)                        \
    do {                                                            \
        HRESULT _ce_hr = (hr);                                      \
        if (FAILED(_ce_hr)) {                                       \
            CE_LOG_ERROR("HR", "0x%08X %s", (unsigned)_ce_hr, msg); \
            return ret_val;                                         \
        }                                                           \
    } while (0)

// CE_VALIDATE_VK - Validates VkResult, logs and returns false on failure
// VkResult: 0 = VK_SUCCESS, negative = error
#define CE_VALIDATE_VK(result, msg)                   \
    do {                                              \
        int _ce_vk = (int)(result);                   \
        if (_ce_vk < 0) {                             \
            CE_LOG_ERROR("VK", "%d %s", _ce_vk, msg); \
            return false;                             \
        }                                             \
    } while (0)

// CE_VALIDATE_VK_RET - Validates VkResult, returns custom value on failure
#define CE_VALIDATE_VK_RET(result, msg, ret_val)      \
    do {                                              \
        int _ce_vk = (int)(result);                   \
        if (_ce_vk < 0) {                             \
            CE_LOG_ERROR("VK", "%d %s", _ce_vk, msg); \
            return ret_val;                           \
        }                                             \
    } while (0)

// CE_ENSURE_NOT_NULL - Validates pointer is not null
#define CE_ENSURE_NOT_NULL(ptr) CE_VALIDATE((ptr) != nullptr, #ptr " is null")

// CE_ENSURE_NOT_NULL_RET - Validates pointer, returns custom value
#define CE_ENSURE_NOT_NULL_RET(ptr, ret_val) CE_VALIDATE_RET((ptr) != nullptr, #ptr " is null", ret_val)

// CE_UNREACHABLE - Marks code that should never execute
#define CE_UNREACHABLE()                                   \
    do {                                                   \
        CE_LOG_ERROR("Logic", "unreachable code reached"); \
        CE_ASSERT(false && "unreachable");                 \
    } while (0)

// CE_ASSERT - Debug-only assertion with breakpoint
// Completely compiled out in release builds
#ifdef _DEBUG
#define CE_ASSERT(cond)                                  \
    do {                                                 \
        if (!(cond)) {                                   \
            CE_LOG_ERROR("Assert", "failed: %s", #cond); \
            __debugbreak();                              \
        }                                                \
    } while (0)

#define CE_ASSERT_MSG(cond, msg)                          \
    do {                                                  \
        if (!(cond)) {                                    \
            CE_LOG_ERROR("Assert", "%s: %s", msg, #cond); \
            __debugbreak();                               \
        }                                                 \
    } while (0)
#else
#define CE_ASSERT(cond) ((void)0)
#define CE_ASSERT_MSG(cond, msg) ((void)0)
#endif

// CE_DEBUG_ONLY - Code block that only executes in debug builds or when debug
// logging enabled
#ifdef _DEBUG
#define CE_DEBUG_ONLY(code) \
    do {                    \
        code;               \
    } while (0)
#else
#define CE_DEBUG_ONLY(code)          \
    do {                             \
        if (g_DebugLoggingEnabled) { \
            code;                    \
        }                            \
    } while (0)
#endif

// Error codes for structured error handling
namespace ce {

enum class ErrorCode : int32_t {
    Success = 0,

    // General errors (1-99)
    Unknown = 1,
    InvalidArgument = 2,
    NullPointer = 3,
    OutOfMemory = 4,
    NotInitialized = 5,
    AlreadyInitialized = 6,
    InvalidState = 7,
    Timeout = 8,

    // COM/D3D errors (100-199)
    ComError = 100,
    DeviceCreationFailed = 101,
    ResourceCreationFailed = 102,
    QueryInterfaceFailed = 103,
    DeviceLost = 104,

    // Vulkan errors (200-299)
    VkError = 200,
    VkDeviceCreationFailed = 201,
    VkResourceCreationFailed = 202,
    VkSyncFailed = 203,

    // IPC errors (300-399)
    IpcError = 300,
    SharedMemoryFailed = 301,
    ConnectionFailed = 302,
    VersionMismatch = 303,

    // Capture errors (400-499)
    CaptureError = 400,
    NoSwapchain = 401,
    TextureCreationFailed = 402,
    FenceTimeout = 403,

    // Encoding errors (500-599)
    EncoderError = 500,
    CodecNotFound = 501,
    EncoderInitFailed = 502,
    FrameEncodeFailed = 503,
    FileOpenFailed = 504,
};

// Result type for functions that can fail with details
struct Result {
    ErrorCode code;
    const char* message;  // Static string, do not free

    Result() : code(ErrorCode::Success), message(nullptr) {}
    Result(ErrorCode c) : code(c), message(nullptr) {}
    Result(ErrorCode c, const char* msg) : code(c), message(msg) {}

    bool ok() const {
        return code == ErrorCode::Success;
    }
    bool failed() const {
        return code != ErrorCode::Success;
    }

    explicit operator bool() const {
        return ok();
    }
};

inline Result Ok() {
    return Result(ErrorCode::Success);
}
inline Result Err(ErrorCode c, const char* msg = nullptr) {
    return Result(c, msg);
}

}  // namespace ce

// Inline implementation of CE_LogImpl for header-only usage
// Can be overridden by defining CE_CUSTOM_LOG before including this header
#ifndef CE_CUSTOM_LOG
#include <windows.h>
#include <cstdarg>
#include <cstdio>

inline std::atomic<bool> g_DebugLoggingEnabled{false};
inline void CE_LogImpl(const char* level, const char* module, int line, const char* fmt, ...) {
    if (!g_DebugLoggingEnabled)
        return;
    char buffer[512];
    char* p = buffer;
    int remaining = sizeof(buffer);
    int written;

    // Format: [CE:L:M] or [CE:L:M:line]
    if (line > 0) {
        written = snprintf(p, remaining, "[CE:%s:%s:%d] ", level, module, line);
    } else {
        written = snprintf(p, remaining, "[CE:%s:%s] ", level, module);
    }

    if (written > 0 && written < remaining) {
        p += written;
        remaining -= written;
    }

    va_list args;
    va_start(args, fmt);
    vsnprintf(p, remaining, fmt, args);
    va_end(args);

    buffer[sizeof(buffer) - 1] = '\0';

    // Output to debug console
    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");
}
#endif  // CE_CUSTOM_LOG
