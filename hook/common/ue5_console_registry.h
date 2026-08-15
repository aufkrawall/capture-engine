#pragma once

// Decoding rules for UE's console-object registry.
//
// The literal scanner can only find CVars whose exact name is a compiled-in
// UTF-16 string. UE composes some names at runtime - `ShowFlag.%s` via
// FString::Printf in UE 5.4/5.6 - so no literal exists to match and those
// variables are unreachable by scanning alone. UE does, however, keep every
// registered variable in `FConsoleManager`'s `TMap<FString, IConsoleObject*>`,
// where the composed name is present as an ordinary heap FString.
//
// Nothing here dereferences memory or hard-codes a UE map layout: the caller
// supplies bytes, and the key-to-value distance is *derived* from elements
// that are already known-good (a name CE resolved by scanning, next to the
// object CE installed). These predicates only decide whether a decoded
// candidate is self-consistent, which keeps them unit-testable without a
// live UE process.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ce::ue5_registry {

// FString is a TArray<TCHAR>: {TCHAR* Data; int32 ArrayNum; int32 ArrayMax},
// where ArrayNum counts the terminating null.
struct StringHeader {
    uintptr_t data = 0;
    int32_t num = 0;
    int32_t max = 0;
};

// Longest CVar name CE will accept from the registry. UE's own names stay far
// below this; the bound keeps a bogus header from requesting a huge read.
inline constexpr int32_t kMaxNameLength = 512;

// Distances from the key to the value that CE will consider. UE's
// TSetElement<TTuple<FString, IConsoleObject*>> puts the value directly after
// the 16-byte FString, but the offset is confirmed against a known element
// before use rather than assumed.
inline constexpr std::size_t kCandidateValueOffsets[] = {16, 24, 8, 32};

constexpr bool IsPlausibleStringHeader(const StringHeader& header) noexcept {
    if (header.num <= 1 || header.max < header.num || header.num > kMaxNameLength)
        return false;
    if (header.max > kMaxNameLength * 4)
        return false;
    // FString data is TCHAR-aligned and never null for a non-empty string.
    return header.data != 0 && (header.data % 2) == 0;
}

// True when `bytes` holds exactly `ascii` as a null-terminated UTF-16LE
// string of `header.num` code units (case-insensitive: UE console names are
// matched case-insensitively by the console manager itself).
inline bool MatchesName(const uint8_t* bytes, std::size_t available, const StringHeader& header,
                        const char* ascii) noexcept {
    if (!bytes || !ascii)
        return false;
    const std::size_t length = std::strlen(ascii);
    if (static_cast<std::size_t>(header.num) != length + 1)
        return false;
    if (available < (length + 1) * 2)
        return false;
    for (std::size_t index = 0; index < length; ++index) {
        if (bytes[index * 2 + 1] != 0)
            return false;
        uint8_t left = bytes[index * 2];
        uint8_t right = static_cast<uint8_t>(ascii[index]);
        if (left >= 'A' && left <= 'Z')
            left = static_cast<uint8_t>(left + ('a' - 'A'));
        if (right >= 'A' && right <= 'Z')
            right = static_cast<uint8_t>(right + ('a' - 'A'));
        if (left != right)
            return false;
    }
    return bytes[length * 2] == 0 && bytes[length * 2 + 1] == 0;
}

// A registry entry is only trusted when the element that anchored the layout
// and the element being resolved agree on the key-to-value distance, and the
// resolved object is a real, distinct heap object.
struct ResolvedEntry {
    uintptr_t keyAddress = 0;
    uintptr_t valueAddress = 0;
    uintptr_t object = 0;
    std::size_t valueOffset = 0;
};

constexpr bool IsAcceptableValueOffset(std::size_t offset) noexcept {
    for (std::size_t candidate : kCandidateValueOffsets) {
        if (candidate == offset)
            return true;
    }
    return false;
}

constexpr bool IsPlausibleConsoleObject(uintptr_t object) noexcept {
    // Heap objects are at least pointer-aligned, and a user-mode address is
    // never in the lowest page or above the 47-bit canonical range.
    return object >= 0x10000 && (object % 8) == 0 && object < (uintptr_t{1} << 47);
}

}  // namespace ce::ue5_registry
