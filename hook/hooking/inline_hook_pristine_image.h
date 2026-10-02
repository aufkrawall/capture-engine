#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ce::inline_hook_pristine_image {

enum class ImageBytesIssue {
    None,
    InvalidArgument,
    InvalidDosHeader,
    InvalidNtHeaders,
    UnsupportedImage,
    InvalidSectionTable,
    TargetNotBackedByFile,
    MissingRelocations,
    InvalidRelocationDirectory,
    UnsupportedRelocation,
};

inline const char* ImageBytesIssueName(ImageBytesIssue issue) {
    switch (issue) {
        case ImageBytesIssue::None:
            return "none";
        case ImageBytesIssue::InvalidArgument:
            return "invalid argument";
        case ImageBytesIssue::InvalidDosHeader:
            return "invalid DOS header";
        case ImageBytesIssue::InvalidNtHeaders:
            return "invalid NT headers";
        case ImageBytesIssue::UnsupportedImage:
            return "unsupported PE image";
        case ImageBytesIssue::InvalidSectionTable:
            return "invalid section table";
        case ImageBytesIssue::TargetNotBackedByFile:
            return "target range is not backed by file data";
        case ImageBytesIssue::MissingRelocations:
            return "image moved without a base-relocation directory";
        case ImageBytesIssue::InvalidRelocationDirectory:
            return "invalid base-relocation directory";
        case ImageBytesIssue::UnsupportedRelocation:
            return "unsupported relocation overlaps target bytes";
    }
    return "unknown";
}

struct ImageBytesResult {
    ImageBytesIssue issue = ImageBytesIssue::InvalidArgument;
    size_t relocationsApplied = 0;

    bool Succeeded() const { return issue == ImageBytesIssue::None; }
};

namespace detail {

inline bool ContainsRange(size_t size, size_t offset, size_t length) {
    return offset <= size && length <= size - offset;
}

template <typename T>
inline bool ReadStruct(const uint8_t* data, size_t size, size_t offset, T* value) {
    if (!data || !value || !ContainsRange(size, offset, sizeof(T)))
        return false;
    std::memcpy(value, data + offset, sizeof(T));
    return true;
}

struct ImageLayout {
    const uint8_t* fileData = nullptr;
    size_t fileSize = 0;
    size_t sectionTableOffset = 0;
    WORD sectionCount = 0;
    DWORD sizeOfHeaders = 0;
    DWORD sizeOfImage = 0;
    uint64_t preferredImageBase = 0;
    DWORD relocationRva = 0;
    DWORD relocationSize = 0;
    bool is64Bit = false;
};

inline ImageBytesIssue ParseImageLayout(const uint8_t* fileData, size_t fileSize, ImageLayout* layout) {
    if (!fileData || !layout)
        return ImageBytesIssue::InvalidArgument;

    IMAGE_DOS_HEADER dos = {};
    if (!ReadStruct(fileData, fileSize, 0, &dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0)
        return ImageBytesIssue::InvalidDosHeader;

    const size_t ntOffset = static_cast<size_t>(dos.e_lfanew);
    DWORD signature = 0;
    IMAGE_FILE_HEADER fileHeader = {};
    if (!ReadStruct(fileData, fileSize, ntOffset, &signature) || signature != IMAGE_NT_SIGNATURE ||
        !ReadStruct(fileData, fileSize, ntOffset + sizeof(signature), &fileHeader)) {
        return ImageBytesIssue::InvalidNtHeaders;
    }

    const size_t optionalOffset = ntOffset + sizeof(signature) + sizeof(fileHeader);
    WORD optionalMagic = 0;
    if (!ReadStruct(fileData, fileSize, optionalOffset, &optionalMagic))
        return ImageBytesIssue::InvalidNtHeaders;

    ImageLayout parsed = {};
    parsed.fileData = fileData;
    parsed.fileSize = fileSize;
    parsed.sectionCount = fileHeader.NumberOfSections;
    if (optionalMagic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        IMAGE_OPTIONAL_HEADER32 optional = {};
        if (fileHeader.Machine != IMAGE_FILE_MACHINE_I386 || fileHeader.SizeOfOptionalHeader < sizeof(optional) ||
            !ReadStruct(fileData, fileSize, optionalOffset, &optional)) {
            return ImageBytesIssue::UnsupportedImage;
        }
        parsed.preferredImageBase = optional.ImageBase;
        parsed.sizeOfHeaders = optional.SizeOfHeaders;
        parsed.sizeOfImage = optional.SizeOfImage;
        if (optional.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_BASERELOC) {
            parsed.relocationRva = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
            parsed.relocationSize = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;
        }
    } else if (optionalMagic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        IMAGE_OPTIONAL_HEADER64 optional = {};
        if (fileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || fileHeader.SizeOfOptionalHeader < sizeof(optional) ||
            !ReadStruct(fileData, fileSize, optionalOffset, &optional)) {
            return ImageBytesIssue::UnsupportedImage;
        }
        parsed.is64Bit = true;
        parsed.preferredImageBase = optional.ImageBase;
        parsed.sizeOfHeaders = optional.SizeOfHeaders;
        parsed.sizeOfImage = optional.SizeOfImage;
        if (optional.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_BASERELOC) {
            parsed.relocationRva = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
            parsed.relocationSize = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;
        }
    } else {
        return ImageBytesIssue::UnsupportedImage;
    }

    if (parsed.sizeOfImage == 0 || parsed.sectionCount == 0)
        return ImageBytesIssue::InvalidNtHeaders;

    parsed.sectionTableOffset = optionalOffset + fileHeader.SizeOfOptionalHeader;
    if (!ContainsRange(fileSize, parsed.sectionTableOffset,
                       static_cast<size_t>(parsed.sectionCount) * sizeof(IMAGE_SECTION_HEADER))) {
        return ImageBytesIssue::InvalidSectionTable;
    }

    *layout = parsed;
    return ImageBytesIssue::None;
}

inline bool RvaToFileRange(const ImageLayout& layout, DWORD rva, size_t length, size_t* fileOffset) {
    const uint64_t rangeEnd = static_cast<uint64_t>(rva) + length;
    if (rangeEnd > layout.sizeOfImage)
        return false;

    if (rva < layout.sizeOfHeaders && rangeEnd <= layout.sizeOfHeaders) {
        if (!ContainsRange(layout.fileSize, rva, length))
            return false;
        if (fileOffset)
            *fileOffset = rva;
        return true;
    }

    for (WORD index = 0; index < layout.sectionCount; ++index) {
        IMAGE_SECTION_HEADER section = {};
        const size_t sectionOffset =
            layout.sectionTableOffset + static_cast<size_t>(index) * sizeof(IMAGE_SECTION_HEADER);
        if (!ReadStruct(layout.fileData, layout.fileSize, sectionOffset, &section))
            return false;

        const uint64_t sectionStart = section.VirtualAddress;
        const uint64_t rawEnd = sectionStart + section.SizeOfRawData;
        if (rva < sectionStart || rangeEnd > rawEnd)
            continue;

        const uint64_t rawOffset =
            static_cast<uint64_t>(section.PointerToRawData) + (static_cast<uint64_t>(rva) - sectionStart);
        if (rawOffset > SIZE_MAX || !ContainsRange(layout.fileSize, static_cast<size_t>(rawOffset), length))
            return false;
        if (fileOffset)
            *fileOffset = static_cast<size_t>(rawOffset);
        return true;
    }
    return false;
}

inline bool IsAllZero(const uint8_t* data, size_t length) {
    for (size_t index = 0; index < length; ++index) {
        if (data[index] != 0)
            return false;
    }
    return true;
}

}  // namespace detail

// Returns pristine bytes as they would appear in the loaded image. The PE file
// contains preferred-base absolute operands, while a bypass trampoline executes
// in the address space of the relocated module. Copying those raw operands is a
// deterministic crash when the image did not load at its preferred base.
inline ImageBytesResult ReadRelocatedImageBytes(const uint8_t* fileData, size_t fileSize, uintptr_t loadedImageBase,
                                                DWORD targetRva, uint8_t* output, size_t outputSize) {
    ImageBytesResult result = {};
    if (!fileData || !output || outputSize == 0) {
        result.issue = ImageBytesIssue::InvalidArgument;
        return result;
    }

    detail::ImageLayout layout = {};
    result.issue = detail::ParseImageLayout(fileData, fileSize, &layout);
    if (result.issue != ImageBytesIssue::None)
        return result;

    size_t targetFileOffset = 0;
    if (!detail::RvaToFileRange(layout, targetRva, outputSize, &targetFileOffset)) {
        result.issue = ImageBytesIssue::TargetNotBackedByFile;
        return result;
    }
    std::memcpy(output, fileData + targetFileOffset, outputSize);

    const uint64_t loadedBase = loadedImageBase;
    if (loadedBase == layout.preferredImageBase) {
        result.issue = ImageBytesIssue::None;
        return result;
    }

    if (layout.relocationRva == 0 || layout.relocationSize == 0) {
        result.issue = ImageBytesIssue::MissingRelocations;
        return result;
    }

    size_t relocationFileOffset = 0;
    if (!detail::RvaToFileRange(layout, layout.relocationRva, layout.relocationSize, &relocationFileOffset)) {
        result.issue = ImageBytesIssue::InvalidRelocationDirectory;
        return result;
    }

    const uint64_t targetStart = targetRva;
    const uint64_t targetEnd = targetStart + outputSize;
    const uint8_t* relocationData = fileData + relocationFileOffset;
    size_t cursor = 0;
    while (cursor < layout.relocationSize) {
        const size_t remaining = layout.relocationSize - cursor;
        if (remaining < sizeof(IMAGE_BASE_RELOCATION)) {
            if (detail::IsAllZero(relocationData + cursor, remaining))
                break;
            result.issue = ImageBytesIssue::InvalidRelocationDirectory;
            return result;
        }

        IMAGE_BASE_RELOCATION block = {};
        std::memcpy(&block, relocationData + cursor, sizeof(block));
        if (block.VirtualAddress == 0 && block.SizeOfBlock == 0) {
            if (detail::IsAllZero(relocationData + cursor, remaining))
                break;
            result.issue = ImageBytesIssue::InvalidRelocationDirectory;
            return result;
        }
        if (block.VirtualAddress >= layout.sizeOfImage || block.SizeOfBlock < sizeof(block) ||
            block.SizeOfBlock > remaining || ((block.SizeOfBlock - sizeof(block)) % sizeof(WORD)) != 0) {
            result.issue = ImageBytesIssue::InvalidRelocationDirectory;
            return result;
        }

        const size_t entryCount = (block.SizeOfBlock - sizeof(block)) / sizeof(WORD);
        for (size_t entryIndex = 0; entryIndex < entryCount; ++entryIndex) {
            WORD entry = 0;
            std::memcpy(&entry, relocationData + cursor + sizeof(block) + entryIndex * sizeof(entry), sizeof(entry));
            const WORD type = static_cast<WORD>(entry >> 12);
            if (type == IMAGE_REL_BASED_ABSOLUTE)
                continue;

            const uint64_t fieldRva = static_cast<uint64_t>(block.VirtualAddress) + (entry & 0x0FFFu);
            size_t fieldSize = 8;
            bool supported = false;
            if (!layout.is64Bit && type == IMAGE_REL_BASED_HIGHLOW) {
                fieldSize = sizeof(uint32_t);
                supported = true;
            } else if (layout.is64Bit && type == IMAGE_REL_BASED_DIR64) {
                fieldSize = sizeof(uint64_t);
                supported = true;
            }

            const uint64_t fieldEnd = fieldRva + fieldSize;
            if (fieldRva >= targetEnd || fieldEnd <= targetStart)
                continue;
            if (!supported || fieldEnd > layout.sizeOfImage) {
                result.issue = ImageBytesIssue::UnsupportedRelocation;
                return result;
            }

            size_t fieldFileOffset = 0;
            if (!detail::RvaToFileRange(layout, static_cast<DWORD>(fieldRva), fieldSize, &fieldFileOffset)) {
                result.issue = ImageBytesIssue::InvalidRelocationDirectory;
                return result;
            }

            uint8_t relocatedBytes[sizeof(uint64_t)] = {};
            if (fieldSize == sizeof(uint32_t)) {
                uint32_t value = 0;
                std::memcpy(&value, fileData + fieldFileOffset, sizeof(value));
                value += static_cast<uint32_t>(loadedBase - layout.preferredImageBase);
                std::memcpy(relocatedBytes, &value, sizeof(value));
            } else {
                uint64_t value = 0;
                std::memcpy(&value, fileData + fieldFileOffset, sizeof(value));
                value += loadedBase - layout.preferredImageBase;
                std::memcpy(relocatedBytes, &value, sizeof(value));
            }

            const uint64_t copyStart = fieldRva > targetStart ? fieldRva : targetStart;
            const uint64_t copyEnd = fieldEnd < targetEnd ? fieldEnd : targetEnd;
            for (uint64_t byteRva = copyStart; byteRva < copyEnd; ++byteRva) {
                output[static_cast<size_t>(byteRva - targetStart)] =
                    relocatedBytes[static_cast<size_t>(byteRva - fieldRva)];
            }
            ++result.relocationsApplied;
        }
        cursor += block.SizeOfBlock;
    }

    result.issue = ImageBytesIssue::None;
    return result;
}

}  // namespace ce::inline_hook_pristine_image
