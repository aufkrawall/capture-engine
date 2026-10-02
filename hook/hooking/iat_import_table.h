/**
 * Bounds-checked PE import-table lookup
 *
 * Old linkers may omit OriginalFirstThunk. In a loaded image FirstThunk then
 * contains resolved function pointers, not IMAGE_IMPORT_BY_NAME RVAs. Keep the
 * two formats explicit so resolved addresses can never be dereferenced as PE
 * metadata.
 */

#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace IATHook::detail {

enum class ImportTableIssue {
    None,
    InvalidDirectory,
    InvalidDescriptor,
    UnterminatedDescriptors,
    InvalidThunkTable,
    UnterminatedThunkTable,
    InvalidImportName,
};

using ImportMemoryReadable = bool (*)(const void* address, size_t size);
using TrackedImportLookup = bool (*)(void* context, void** iatEntry, void** originalFunction);

struct ImportEntryLookup {
    IMAGE_THUNK_DATA* iatEntry = nullptr;
    void* trackedOriginal = nullptr;
    ImportTableIssue issue = ImportTableIssue::None;
    bool sourceModuleFound = false;
    bool usedResolvedAddress = false;
};

inline bool ImageRangeContains(size_t imageSize, uintptr_t rva, size_t size) {
    return rva <= imageSize && size <= imageSize - rva;
}

inline bool ImportRangeReadable(const BYTE* imageBase, size_t imageSize, uintptr_t rva, size_t size,
                                ImportMemoryReadable memoryReadable) {
    return imageBase && ImageRangeContains(imageSize, rva, size) &&
           (!memoryReadable || memoryReadable(imageBase + rva, size));
}

enum class ImageStringMatch {
    Invalid,
    Mismatch,
    Match,
};

inline char FoldAscii(char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

inline ImageStringMatch MatchImageString(const BYTE* imageBase, size_t imageSize, uintptr_t rva,
                                         const char* expected, bool caseInsensitive,
                                         ImportMemoryReadable memoryReadable) {
    if (!expected)
        return ImageStringMatch::Mismatch;

    const size_t expectedLength = std::strlen(expected);
    if (!ImportRangeReadable(imageBase, imageSize, rva, expectedLength + 1, memoryReadable))
        return ImageStringMatch::Invalid;

    const auto* candidate = reinterpret_cast<const char*>(imageBase + rva);
    for (size_t index = 0; index < expectedLength; ++index) {
        const char left = caseInsensitive ? FoldAscii(candidate[index]) : candidate[index];
        const char right = caseInsensitive ? FoldAscii(expected[index]) : expected[index];
        if (left != right)
            return ImageStringMatch::Mismatch;
    }
    return candidate[expectedLength] == '\0' ? ImageStringMatch::Match : ImageStringMatch::Mismatch;
}

inline ImportEntryLookup FindImportTableEntry(BYTE* imageBase, size_t imageSize, DWORD importDirectoryRva,
                                               DWORD importDirectorySize, const char* sourceModule,
                                               const char* functionName, void* expectedFunction,
                                               void* hookFunction, ImportMemoryReadable memoryReadable,
                                               TrackedImportLookup trackedLookup = nullptr,
                                               void* trackedContext = nullptr) {
    ImportEntryLookup result;
    if (!sourceModule || !functionName || !imageBase ||
        !ImageRangeContains(imageSize, importDirectoryRva, importDirectorySize) ||
        importDirectorySize < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        result.issue = ImportTableIssue::InvalidDirectory;
        return result;
    }

    const size_t descriptorCount = importDirectorySize / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    auto* descriptors = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(imageBase + importDirectoryRva);
    for (size_t descriptorIndex = 0; descriptorIndex < descriptorCount; ++descriptorIndex) {
        auto* descriptor = descriptors + descriptorIndex;
        if (!memoryReadable || memoryReadable(descriptor, sizeof(*descriptor))) {
            if (descriptor->Name == 0)
                return result;
        } else {
            result.issue = ImportTableIssue::InvalidDescriptor;
            return result;
        }

        const ImageStringMatch moduleMatch =
            MatchImageString(imageBase, imageSize, descriptor->Name, sourceModule, true, memoryReadable);
        if (moduleMatch == ImageStringMatch::Invalid) {
            result.issue = ImportTableIssue::InvalidDescriptor;
            return result;
        }
        if (moduleMatch != ImageStringMatch::Match)
            continue;

        result.sourceModuleFound = true;
        const bool hasNameTable = descriptor->OriginalFirstThunk != 0;
        const uintptr_t lookupRva = hasNameTable ? descriptor->OriginalFirstThunk : descriptor->FirstThunk;
        const uintptr_t iatRva = descriptor->FirstThunk;
        if (!ImportRangeReadable(imageBase, imageSize, lookupRva, sizeof(IMAGE_THUNK_DATA), memoryReadable) ||
            !ImportRangeReadable(imageBase, imageSize, iatRva, sizeof(IMAGE_THUNK_DATA), memoryReadable)) {
            result.issue = ImportTableIssue::InvalidThunkTable;
            return result;
        }

        const size_t lookupCapacity = (imageSize - lookupRva) / sizeof(IMAGE_THUNK_DATA);
        const size_t iatCapacity = (imageSize - iatRva) / sizeof(IMAGE_THUNK_DATA);
        const size_t thunkCount = lookupCapacity < iatCapacity ? lookupCapacity : iatCapacity;
        auto* lookupThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(imageBase + lookupRva);
        auto* iatEntry = reinterpret_cast<IMAGE_THUNK_DATA*>(imageBase + iatRva);
        for (size_t thunkIndex = 0; thunkIndex < thunkCount; ++thunkIndex, ++lookupThunk, ++iatEntry) {
            if (memoryReadable &&
                (!memoryReadable(lookupThunk, sizeof(*lookupThunk)) || !memoryReadable(iatEntry, sizeof(*iatEntry)))) {
                result.issue = ImportTableIssue::InvalidThunkTable;
                return result;
            }

            const ULONG_PTR lookupValue = lookupThunk->u1.AddressOfData;
            if (lookupValue == 0)
                return result;

            const auto currentFunction = reinterpret_cast<void*>(static_cast<uintptr_t>(iatEntry->u1.Function));
            if (!hasNameTable) {
                if (expectedFunction && currentFunction == expectedFunction) {
                    result.iatEntry = iatEntry;
                    result.usedResolvedAddress = true;
                    return result;
                }
                if (currentFunction == hookFunction && trackedLookup) {
                    void* originalFunction = nullptr;
                    if (trackedLookup(trackedContext, reinterpret_cast<void**>(&iatEntry->u1.Function),
                                      &originalFunction)) {
                        result.iatEntry = iatEntry;
                        result.trackedOriginal = originalFunction;
                        result.usedResolvedAddress = true;
                        return result;
                    }
                }
                continue;
            }

            if ((lookupThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) != 0)
                continue;

            constexpr size_t nameOffset = offsetof(IMAGE_IMPORT_BY_NAME, Name);
            if (lookupValue > UINTPTR_MAX - nameOffset) {
                result.issue = ImportTableIssue::InvalidImportName;
                return result;
            }
            const ImageStringMatch functionMatch = MatchImageString(
                imageBase, imageSize, static_cast<uintptr_t>(lookupValue) + nameOffset, functionName, false,
                memoryReadable);
            if (functionMatch == ImageStringMatch::Invalid) {
                result.issue = ImportTableIssue::InvalidImportName;
                return result;
            }
            if (functionMatch == ImageStringMatch::Match) {
                result.iatEntry = iatEntry;
                return result;
            }
        }

        result.issue = ImportTableIssue::UnterminatedThunkTable;
        return result;
    }

    result.issue = ImportTableIssue::UnterminatedDescriptors;
    return result;
}

inline const char* ImportTableIssueName(ImportTableIssue issue) {
    switch (issue) {
        case ImportTableIssue::None:
            return "none";
        case ImportTableIssue::InvalidDirectory:
            return "invalid-directory";
        case ImportTableIssue::InvalidDescriptor:
            return "invalid-descriptor";
        case ImportTableIssue::UnterminatedDescriptors:
            return "unterminated-descriptors";
        case ImportTableIssue::InvalidThunkTable:
            return "invalid-thunk-table";
        case ImportTableIssue::UnterminatedThunkTable:
            return "unterminated-thunk-table";
        case ImportTableIssue::InvalidImportName:
            return "invalid-import-name";
    }
    return "unknown";
}

}  // namespace IATHook::detail
