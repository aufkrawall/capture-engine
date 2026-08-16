#pragma once

// Deciding which console-variable layout an `IConsoleObject` actually carries,
// before anything writes to it.
//
// CE used to assume exactly one layout for every object it resolved through
// UE's console registry: a data pointer at `object+0x50` with the
// `{game, render}` shadow pair right behind it. That is `FConsoleVariableRef<T>`
// and it is only one of the shapes UE registers.
//
// `ShowFlag.*` is registered as `FConsoleVariableBitRef`, whose value is a
// single bit in two process-wide force masks, so the same two qwords are
// `{Force0MaskPtr, Force1MaskPtr}` instead of `{RefValue, MainValue}`. The
// 20260816_161158 Talos session proved it: all four ShowFlag objects reported
// the identical `object+0x58` qword `0x00007FF73B3A16F0`, an address inside the
// exe's writable data. A per-variable shadow pair cannot be identical across
// four different variables; a shared mask pointer is exactly that. Redirecting
// it did nothing the engine reads and left the object holding CE's storage in
// place of the engine's mask.
//
// Nothing here dereferences memory. The caller reads the object and reports
// what it found, so the classification rules stay unit-testable without a live
// UE process - and so that "no layout matched" is a decision with evidence
// behind it rather than a silent write to whatever was there.

#include "ue5_cvar_override_policy.h"

#include <cstddef>
#include <cstdint>

namespace ce::ue5_layout {

enum class Kind : uint8_t {
    None,
    // `FConsoleVariableRef<T>`: `T& RefValue` followed by the
    // `TConsoleVariableData<T>` {game, render} shadow pair.
    ReferencePointer,
    // `FConsoleVariable<T>`: the {game, render} shadow pair stored inline, with
    // no separate global to reference.
    InlinePair,
    // `FConsoleVariableBitRef` (UE's `ShowFlag.*`): {Force0MaskPtr,
    // Force1MaskPtr, BitNumber}. The value is one bit in each mask.
    BitReference,
};

// Offset of the value block in `FConsoleVariableBase`-derived objects, measured
// in UE 5.4 (Talos) and UE 5.6 (Industria): the base occupies 0x50 bytes.
inline constexpr std::size_t kPrimaryValueOffset = 0x50;
// The pointer-shaped checks are strong enough to probe a window around it, so a
// build whose base differs by one field is still reachable.
inline constexpr std::size_t kValueOffsets[] = {0x48, kPrimaryValueOffset, 0x58};

// UE's show flag count is far below this. The bound only has to keep a bogus
// dword from selecting a byte outside the mask.
inline constexpr uint32_t kMaxBitNumber = 512;

// What the caller read at one candidate offset inside the console object.
struct ObjectProbe {
    std::size_t offset = 0;
    // Raw qwords at `offset` and `offset+8`, plus the dword at `offset+16`.
    uint64_t firstQword = 0;
    uint64_t secondQword = 0;
    uint32_t bitNumber = 0;
    // Value read through `firstQword` treated as a pointer.
    uint32_t pointedValue = 0;
    // Each qword, treated as a pointer, addresses committed writable memory.
    bool firstTargetWritable = false;
    bool secondTargetWritable = false;
    bool pointedValueRead = false;
    bool bitNumberRead = false;
    // Both qwords resolve into the writable data of the same loaded module,
    // which is where UE's two show flag force masks live.
    bool pointersShareModuleData = false;
};

constexpr uint32_t LowDword(uint64_t value) noexcept {
    return static_cast<uint32_t>(value);
}

constexpr uint32_t HighDword(uint64_t value) noexcept {
    return static_cast<uint32_t>(value >> 32);
}

// Two distinct writable pointers into one module's data, followed by a small
// bit index. A `{RefValue, MainValue}` object cannot look like this: its second
// qword is a pair of 32-bit values, and a value pair that also happens to be a
// committed writable address is not something UE produces.
constexpr bool IsBitReference(const ObjectProbe& probe) noexcept {
    return probe.firstTargetWritable && probe.secondTargetWritable &&
           probe.pointersShareModuleData && probe.firstQword != probe.secondQword &&
           probe.bitNumberRead && probe.bitNumber < kMaxBitNumber;
}

// A reference layout is only accepted when the shadow pair actually mirrors the
// global the pointer addresses. That agreement is what separates a real
// `{RefValue, MainValue}` from any other qword that happens to be a readable
// pointer - and it is what the ShowFlag objects fail: their pointed value read 0
// while the following qword was a mask address.
inline bool IsReferencePointer(const ObjectProbe& probe, std::size_t specIndex) noexcept {
    if (probe.secondTargetWritable)
        return false;  // Mask-pointer shaped; not a value pair.
    return probe.firstTargetWritable && probe.pointedValueRead &&
           ce::ue5_cvar::IsPlausibleShadowValue(specIndex, probe.pointedValue) &&
           LowDword(probe.secondQword) == probe.pointedValue;
}

// The inline shadow pair holds one value twice. This is the weakest of the three
// shapes - zeroed padding satisfies it as readily as a real variable - so the
// caller only offers it at `kPrimaryValueOffset`, where the value block is known
// to start, instead of anywhere in the probe window.
inline bool IsInlinePair(const ObjectProbe& probe, std::size_t specIndex) noexcept {
    if (probe.firstTargetWritable)
        return false;  // A usable pointer means the reference layout, not a pair.
    return LowDword(probe.firstQword) == HighDword(probe.firstQword) &&
           ce::ue5_cvar::IsPlausibleShadowValue(specIndex, LowDword(probe.firstQword));
}

struct Selection {
    Kind kind = Kind::None;
    std::size_t offset = 0;
    // Two offsets classified the same way, so which one holds the value is not
    // decided. Reported separately from "nothing matched" because the two say
    // different things about the object.
    bool ambiguous = false;
};

// Picks the layout, most specific shape first, and refuses on a tie.
//
// `allowBitReference` is the caller's statement that this variable can be a bit
// reference at all. UE registers exactly the `ShowFlag.*` names that way, and
// keeping the check off every other variable means an ordinary CVar that
// happens to carry two adjacent module pointers can never be driven as a bit.
inline Selection SelectLayout(const ObjectProbe* probes, std::size_t count, std::size_t specIndex,
                              bool allowBitReference) noexcept {
    Selection selection;
    if (!probes)
        return selection;

    if (allowBitReference) {
        std::size_t matches = 0;
        std::size_t offset = 0;
        for (std::size_t index = 0; index < count; ++index) {
            if (!IsBitReference(probes[index]))
                continue;
            if (matches++ == 0)
                offset = probes[index].offset;
        }
        if (matches == 1) {
            selection.kind = Kind::BitReference;
            selection.offset = offset;
            return selection;
        }
        if (matches > 1) {
            selection.ambiguous = true;
            return selection;
        }
    }

    std::size_t matches = 0;
    std::size_t offset = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (!IsReferencePointer(probes[index], specIndex))
            continue;
        if (matches++ == 0)
            offset = probes[index].offset;
    }
    if (matches == 1) {
        selection.kind = Kind::ReferencePointer;
        selection.offset = offset;
        return selection;
    }
    if (matches > 1) {
        selection.ambiguous = true;
        return selection;
    }

    for (std::size_t index = 0; index < count; ++index) {
        if (probes[index].offset != kPrimaryValueOffset || !IsInlinePair(probes[index], specIndex))
            continue;
        selection.kind = Kind::InlinePair;
        selection.offset = probes[index].offset;
        return selection;
    }
    return selection;
}

// Which byte of a force mask carries a flag, and which bit inside it.
constexpr std::size_t BitByteIndex(uint32_t bitNumber) noexcept {
    return bitNumber / 8;
}

constexpr uint8_t BitMask(uint32_t bitNumber) noexcept {
    return static_cast<uint8_t>(1u << (bitNumber % 8));
}

// `FConsoleVariableBitRef` reads the pair as: neither bit set means "leave the
// game's own setting alone", the force-1 bit wins over the force-0 bit. So a
// value is only expressible when it is 0 or 1, and driving it means owning the
// bit in both masks rather than only setting one of them.
constexpr bool IsExpressibleBitValue(uint32_t bits) noexcept {
    return bits == 0 || bits == 1;
}

}  // namespace ce::ue5_layout
