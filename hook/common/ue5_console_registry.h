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

// Progress of the sweep that locates the registry's element storage.
//
// Locating the map means walking every committed private RW region, and that
// does not fit in one time-boxed pass on a large title: Industria 2
// (20260815_214219) covered 218 MB in the 400 ms budget and stopped mid-heap
// with 31 of 34 anchors placed. Freezing that partial result was wrong twice
// over - the regions it never reached could hold the very elements the second
// pass is looking for, and every "this name is not registered" verdict rests
// on having actually looked everywhere. The sweep therefore carries a cursor
// and resumes at the exact chunk it did not reach, and only a sweep that ran
// the enumeration to its end may support an absence conclusion.
struct SweepProgress {
    // Address the next pass examines first. Regions entirely below it were
    // already covered. The cursor only moves forward: memory that appears
    // below it was committed after CE swept there, and element storage that
    // moves is caught by re-reading the anchor element instead.
    uintptr_t cursor = 0;
    uint64_t sweptBytes = 0;
    uint32_t passes = 0;
    // The enumeration was walked to its end.
    bool complete = false;
    // The cumulative bound ran out before that happened.
    bool budgetExhausted = false;
    // Every region of the allocation holding the map's elements has been read.
    //
    // This, not whole-heap exhaustion, is the criterion that actually answers
    // "is this name registered": a TMap's element storage is a single
    // allocation, so once all of it has been seen, a name absent from it is
    // absent from the registry - no matter how much unrelated heap was never
    // touched. Sweeping the whole heap instead is both far more expensive and
    // strictly weaker evidence about the map. Talos (20260816_014003) read
    // 3698 MB across 32 passes without reaching the end and had to give up,
    // while its map lived in three regions found in the first 180 MB.
    bool allocationCovered = false;
};

// Whether "the name is absent" is a supportable reading of "the name was not
// found". Either the map's own allocation was covered, or - failing that - the
// whole heap was read. A paused or abandoned sweep proves neither.
constexpr bool SweepProvesAbsence(const SweepProgress& progress) noexcept {
    return progress.allocationCovered || (progress.complete && !progress.budgetExhausted);
}

constexpr bool SweepCanContinue(const SweepProgress& progress, uint32_t maxPasses,
                                uint64_t maxBytes) noexcept {
    return !progress.allocationCovered && !progress.complete && !progress.budgetExhausted &&
           progress.passes < maxPasses && progress.sweptBytes < maxBytes;
}

struct RegionSpan {
    uintptr_t base = 0;
    std::size_t size = 0;
};

// True once the cursor has moved past the whole region, so a resumed pass may
// skip it. Written as a subtraction because base + size can overflow.
constexpr bool RegionAlreadySwept(const RegionSpan& region, uintptr_t cursor) noexcept {
    return cursor >= region.base && (cursor - region.base) >= region.size;
}

// Offset a resumed pass re-enters a region at: the start when the cursor lies
// below it, the cursor itself when it points inside. The caller stores the
// offset of the chunk it did *not* scan, so resuming here re-reads that chunk
// whole and no element can fall through the pause.
constexpr std::size_t SweepResumeOffset(const RegionSpan& region, uintptr_t cursor) noexcept {
    if (cursor <= region.base)
        return 0;
    const uintptr_t into = cursor - region.base;
    return into >= region.size ? region.size : static_cast<std::size_t>(into);
}

}  // namespace ce::ue5_registry
