#pragma once

/**
 * String optimization utilities for hot paths
 * 
 * Provides stack-based string formatting and cached string operations
 * to avoid heap allocations in performance-critical code paths.
 */

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <array>
#include <string_view>

// Forward declaration - HookLog is defined in hook_common.h
void HookLog(const char* fmt, ...);

namespace ce {

// Thread-local fixed buffer for string formatting in hot paths
// Usage: 
//   char* buf = HotPathBuffer::Get();
//   snprintf(buf, HotPathBuffer::Size, "format", args...);
class HotPathBuffer {
public:
    static constexpr size_t Size = 256;
    
    // Get thread-local buffer
    static char* Get() {
        thread_local char buffer[Size];
        return buffer;
    }
    
    // Format to thread-local buffer (returns formatted string)
    static const char* Format(const char* fmt, ...) {
        char* buf = Get();
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, Size, fmt, args);
        va_end(args);
        buf[Size - 1] = '\0';
        return buf;
    }
};

// Fast string comparison for hot paths
// Uses constexpr hash for compile-time string literals
inline constexpr uint32_t Fnv1aHash(const char* str, size_t len) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        hash ^= static_cast<uint8_t>(str[i]);
        hash *= 16777619u;
    }
    return hash;
}

inline constexpr uint32_t Fnv1aHash(std::string_view sv) {
    return Fnv1aHash(sv.data(), sv.size());
}

// String view with cached hash for O(1) comparisons
class FastString {
public:
    constexpr FastString() = default;
    constexpr FastString(std::string_view sv) : view_(sv), hash_(Fnv1aHash(sv)) {}
    constexpr FastString(const char* str) : FastString(std::string_view(str)) {}
    constexpr FastString(const char* str, size_t len) : FastString(std::string_view(str, len)) {}
    
    [[nodiscard]] constexpr std::string_view View() const { return view_; }
    [[nodiscard]] constexpr uint32_t Hash() const { return hash_; }
    [[nodiscard]] constexpr size_t Length() const { return view_.length(); }
    [[nodiscard]] constexpr bool Empty() const { return view_.empty(); }
    
    [[nodiscard]] constexpr bool operator==(const FastString& other) const {
        return hash_ == other.hash_ && view_ == other.view_;
    }
    [[nodiscard]] constexpr bool operator!=(const FastString& other) const {
        return !(*this == other);
    }
    
    [[nodiscard]] constexpr bool operator==(std::string_view sv) const {
        return view_ == sv;
    }
    
    [[nodiscard]] constexpr const char* Data() const { return view_.data(); }
    
private:
    std::string_view view_;
    uint32_t hash_ = 0;
};

// Hot path logger - batches logs to reduce lock contention
// Only logs every Nth call or when buffer is full
class HotPathLogger {
public:
    static constexpr size_t BufferSize = 16;
    
    // Log with rate limiting (logs every Nth call)
    // Returns true if message was logged
    static bool LogRateLimited(const char* fmt, ...) {
        thread_local int counter = 0;
        if (++counter % 100 != 0) return false;  // Log every 100th call
        
        char* buf = HotPathBuffer::Get();
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, HotPathBuffer::Size, fmt, args);
        va_end(args);
        
        // Forward to actual logger
        HookLog("%s", buf);
        return true;
    }
    
    // Log with cooldown (max once per N milliseconds)
    static bool LogWithCooldown(DWORD cooldownMs, const char* fmt, ...) {
        thread_local DWORD lastLog = 0;
        DWORD now = GetTickCount();
        if (now - lastLog < cooldownMs) return false;
        lastLog = now;
        
        char* buf = HotPathBuffer::Get();
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, HotPathBuffer::Size, fmt, args);
        va_end(args);
        
        HookLog("%s", buf);
        return true;
    }
};

// Fast integer to string conversion (no heap allocation)
inline char* FastIntToString(int64_t value, char* buffer, size_t bufferSize) {
    if (bufferSize < 2) return nullptr;
    
    char* ptr = buffer + bufferSize - 1;
    *ptr = '\0';
    
    bool negative = value < 0;
    uint64_t uvalue = negative ? -value : value;
    
    if (uvalue == 0) {
        *(--ptr) = '0';
        return ptr;
    }
    
    while (uvalue > 0 && ptr > buffer) {
        *(--ptr) = '0' + (uvalue % 10);
        uvalue /= 10;
    }
    
    if (negative && ptr > buffer) {
        *(--ptr) = '-';
    }
    
    return ptr;
}

// Fast float to string (2 decimal places, no heap allocation)
inline char* FastFloatToString(float value, char* buffer, size_t bufferSize) {
    if (bufferSize < 4) return nullptr;
    
    int64_t intPart = static_cast<int64_t>(value);
    int64_t fracPart = static_cast<int64_t>((value - intPart) * 100);
    if (fracPart < 0) fracPart = -fracPart;
    
    char* ptr = buffer + bufferSize - 1;
    *ptr = '\0';
    
    // Fractional part (2 digits)
    *(--ptr) = '0' + (fracPart % 10);
    *(--ptr) = '0' + ((fracPart / 10) % 10);
    *(--ptr) = '.';
    
    // Integer part
    bool negative = intPart < 0;
    uint64_t uintPart = negative ? -intPart : intPart;
    
    if (uintPart == 0) {
        *(--ptr) = '0';
    } else {
        while (uintPart > 0 && ptr > buffer + 1) {
            *(--ptr) = '0' + (uintPart % 10);
            uintPart /= 10;
        }
    }
    
    if (negative && ptr > buffer) {
        *(--ptr) = '-';
    }
    
    return ptr;
}

// Compile-time string literal for use in switch statements
#define CE_STR_HASH(str) (::ce::Fnv1aHash(str))

} // namespace ce
