#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace ce::ue5_rr {

inline constexpr char kDenoiserModeCVar[] = "r.NGX.DLSS.DenoiserMode";
inline constexpr std::size_t kNotFound = (std::numeric_limits<std::size_t>::max)();

constexpr uint8_t FoldAscii(uint8_t value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<uint8_t>(value + ('a' - 'A')) : value;
}

inline bool MatchesUtf16LeAsciiInsensitive(const uint8_t* bytes, std::size_t available,
                                           const char* ascii) noexcept {
    if (!bytes || !ascii)
        return false;

    std::size_t index = 0;
    while (ascii[index]) {
        if ((index + 1) * 2 > available || bytes[index * 2 + 1] != 0 ||
            FoldAscii(bytes[index * 2]) != FoldAscii(static_cast<uint8_t>(ascii[index]))) {
            return false;
        }
        ++index;
    }
    return (index + 1) * 2 <= available && bytes[index * 2] == 0 && bytes[index * 2 + 1] == 0;
}

inline std::size_t FindUtf16LeAsciiInsensitive(const uint8_t* bytes, std::size_t size,
                                               const char* ascii) noexcept {
    if (!bytes || !ascii)
        return kNotFound;

    const std::size_t asciiLength = std::strlen(ascii);
    const std::size_t required = (asciiLength + 1) * 2;
    if (required > size)
        return kNotFound;

    for (std::size_t offset = 0; offset <= size - required; ++offset) {
        if (MatchesUtf16LeAsciiInsensitive(bytes + offset, size - offset, ascii))
            return offset;
    }
    return kNotFound;
}

struct RipRelativeReference {
    bool valid = false;
    uint8_t opcode = 0;
    uint8_t registerIndex = 0;
    std::size_t length = 0;
    uintptr_t target = 0;

    constexpr bool TakesAddress() const noexcept { return opcode == 0x8D; }
};

// Decode the compact x64 forms emitted for MSVC/Clang static-object setup and
// access: [REX] LEA/MOV reg,[RIP+disp32] and [REX] MOV [RIP+disp32],reg.
inline RipRelativeReference DecodeRipRelativeReference(const uint8_t* bytes, std::size_t available,
                                                        uintptr_t instructionAddress) noexcept {
    RipRelativeReference result;
    if (!bytes || available < 6)
        return result;

    std::size_t cursor = 0;
    uint8_t rex = 0;
    if (bytes[cursor] >= 0x40 && bytes[cursor] <= 0x4F) {
        rex = bytes[cursor++];
    }
    if (available - cursor < 6)
        return result;

    const uint8_t opcode = bytes[cursor++];
    if (opcode != 0x8D && opcode != 0x8B && opcode != 0x89)
        return result;

    const uint8_t modrm = bytes[cursor++];
    if ((modrm & 0xC7) != 0x05)
        return result;

    int32_t displacement = 0;
    std::memcpy(&displacement, bytes + cursor, sizeof(displacement));
    cursor += sizeof(displacement);

    const uintptr_t instructionEnd = instructionAddress + cursor;
    uintptr_t target = 0;
    if (displacement >= 0) {
        const uintptr_t positive = static_cast<uint32_t>(displacement);
        if (instructionEnd > (std::numeric_limits<uintptr_t>::max)() - positive)
            return result;
        target = instructionEnd + positive;
    } else {
        const uintptr_t magnitude = static_cast<uintptr_t>(-(static_cast<int64_t>(displacement)));
        if (instructionEnd < magnitude)
            return result;
        target = instructionEnd - magnitude;
    }

    result.valid = true;
    result.opcode = opcode;
    result.registerIndex = static_cast<uint8_t>(((modrm >> 3) & 7) + ((rex & 0x04) ? 8 : 0));
    result.length = cursor;
    result.target = target;
    return result;
}

constexpr bool IsPlausibleDenoiserModeShadow(int32_t value) noexcept {
    return value == 0 || value == 1;
}

struct CandidateEvidence {
    bool nameLoadedIntoSecondArgument = false;
    bool objectLoadedIntoThis = false;
    bool objectAligned = false;
    bool objectInWritableSection = false;
    bool objectPageWritable = false;
    bool objectVtableCallable = false;
    bool targetObjectCallable = false;
    bool referenceDataReadable = false;
    bool shadowValuesPlausible = false;
    std::size_t instructionDistance = 0;
    uint32_t baseReferenceCount = 0;
    uint32_t targetFieldReferenceCount = 0;
    uint32_t referenceFieldReferenceCount = 0;
};

constexpr int ScoreCandidate(const CandidateEvidence& evidence) noexcept {
    if (!evidence.nameLoadedIntoSecondArgument || !evidence.objectAligned || !evidence.objectInWritableSection ||
        !evidence.objectPageWritable || !evidence.objectVtableCallable || !evidence.targetObjectCallable ||
        !evidence.referenceDataReadable || !evidence.shadowValuesPlausible) {
        return -1;
    }

    int score = 110;
    score += evidence.objectLoadedIntoThis ? 16 : 0;
    score += evidence.instructionDistance < 64 ? static_cast<int>((64 - evidence.instructionDistance) / 4) : 0;
    score += evidence.baseReferenceCount > 3 ? 3 : static_cast<int>(evidence.baseReferenceCount);
    score += evidence.targetFieldReferenceCount > 3 ? 3 : static_cast<int>(evidence.targetFieldReferenceCount);
    score += evidence.referenceFieldReferenceCount > 3 ? 6 : static_cast<int>(evidence.referenceFieldReferenceCount * 2);
    return score;
}

constexpr bool IsUniquelyStrongCandidate(int bestScore, int secondBestScore) noexcept {
    return bestScore >= 125 && (secondBestScore < 0 || bestScore - secondBestScore >= 6);
}

}  // namespace ce::ue5_rr
