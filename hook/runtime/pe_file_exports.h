#pragma once

// Export names of a PE file on disk, read without the loader.
//
// The Streamline takeover must know, before it routes a game's imports to the
// configured sl.interposer.dll, that the configured copy actually exports every
// name the game imports. Mapping it to ask (LoadLibraryEx AS_DATAFILE, or a
// SEC_IMAGE section) re-enters the loader or announces an image load from
// DllMain, so the file is read and parsed as plain bytes instead. The parser is
// bounds-checked against the buffer and never trusts a header field.

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ce::pe_file_exports {

inline constexpr size_t kMaxExportNames = 65536;
inline constexpr size_t kMaxExportNameChars = 512;
inline constexpr size_t kMaxFileBytes = 64u * 1024u * 1024u;

namespace detail {

template <typename T>
bool ReadStruct(const uint8_t* data, size_t size, size_t offset, T* out) {
    if (offset > size || sizeof(T) > size - offset) {
        return false;
    }
    std::memcpy(out, data + offset, sizeof(T));
    return true;
}

}  // namespace detail

// Parses the export-name table of the PE image in `data`. False on any
// malformed structure; true with an empty list for an image without exports.
inline bool ParseExportNames(const uint8_t* data, size_t size, std::vector<std::string>* names) {
    if (!names) {
        return false;
    }
    names->clear();
    if (!data) {
        return false;
    }
    IMAGE_DOS_HEADER dos = {};
    if (!detail::ReadStruct(data, size, 0, &dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) {
        return false;
    }
    const size_t ntOffset = static_cast<size_t>(dos.e_lfanew);
    DWORD signature = 0;
    IMAGE_FILE_HEADER fileHeader = {};
    if (!detail::ReadStruct(data, size, ntOffset, &signature) || signature != IMAGE_NT_SIGNATURE ||
        !detail::ReadStruct(data, size, ntOffset + sizeof(DWORD), &fileHeader)) {
        return false;
    }
    const size_t optOffset = ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    WORD magic = 0;
    if (!detail::ReadStruct(data, size, optOffset, &magic)) {
        return false;
    }
    IMAGE_DATA_DIRECTORY exportDir = {};
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        IMAGE_OPTIONAL_HEADER64 opt = {};
        if (fileHeader.SizeOfOptionalHeader < sizeof(opt) || !detail::ReadStruct(data, size, optOffset, &opt)) {
            return false;
        }
        if (opt.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT) {
            exportDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        }
    } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        IMAGE_OPTIONAL_HEADER32 opt = {};
        if (fileHeader.SizeOfOptionalHeader < sizeof(opt) || !detail::ReadStruct(data, size, optOffset, &opt)) {
            return false;
        }
        if (opt.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT) {
            exportDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        }
    } else {
        return false;
    }
    if (exportDir.VirtualAddress == 0 || exportDir.Size == 0) {
        return true;  // No exports.
    }

    const size_t sectionOffset = optOffset + fileHeader.SizeOfOptionalHeader;
    const size_t sectionCount = fileHeader.NumberOfSections;
    if (sectionCount == 0 || sectionCount > 96 || sectionOffset > size ||
        sectionCount * sizeof(IMAGE_SECTION_HEADER) > size - sectionOffset) {
        return false;
    }
    auto rvaToOffset = [&](uint32_t rva, size_t span, size_t* out) {
        for (size_t i = 0; i < sectionCount; ++i) {
            IMAGE_SECTION_HEADER section = {};
            detail::ReadStruct(data, size, sectionOffset + i * sizeof(IMAGE_SECTION_HEADER), &section);
            const uint32_t va = section.VirtualAddress;
            const uint32_t raw = section.SizeOfRawData;
            const uint32_t limit = section.Misc.VirtualSize > raw ? section.Misc.VirtualSize : raw;
            if (rva < va || rva - va >= limit) {
                continue;
            }
            const uint32_t delta = rva - va;
            if (delta > raw || span > raw - delta) {
                return false;
            }
            const size_t offset = static_cast<size_t>(section.PointerToRawData) + delta;
            if (offset > size || span > size - offset) {
                return false;
            }
            *out = offset;
            return true;
        }
        return false;
    };

    size_t directoryOffset = 0;
    IMAGE_EXPORT_DIRECTORY directory = {};
    if (!rvaToOffset(exportDir.VirtualAddress, sizeof(directory), &directoryOffset) ||
        !detail::ReadStruct(data, size, directoryOffset, &directory)) {
        return false;
    }
    if (directory.NumberOfNames == 0) {
        return true;
    }
    if (directory.NumberOfNames > kMaxExportNames) {
        return false;
    }
    size_t namesOffset = 0;
    if (!rvaToOffset(directory.AddressOfNames, static_cast<size_t>(directory.NumberOfNames) * sizeof(DWORD),
                     &namesOffset)) {
        return false;
    }
    names->reserve(directory.NumberOfNames);
    for (DWORD i = 0; i < directory.NumberOfNames; ++i) {
        DWORD nameRva = 0;
        size_t nameOffset = 0;
        if (!detail::ReadStruct(data, size, namesOffset + static_cast<size_t>(i) * sizeof(DWORD), &nameRva) ||
            !rvaToOffset(nameRva, 1, &nameOffset)) {
            names->clear();
            return false;
        }
        const size_t available = size - nameOffset;
        const size_t limit = available < kMaxExportNameChars ? available : kMaxExportNameChars;
        const auto* name = reinterpret_cast<const char*>(data + nameOffset);
        const size_t length = strnlen(name, limit);
        if (length == 0 || length >= limit) {
            names->clear();
            return false;
        }
        names->emplace_back(name, length);
    }
    return true;
}

// Reads `path` (at most kMaxFileBytes) and parses its export names. Plain file
// I/O only, so it is safe under the loader lock.
inline bool ReadFileExportNames(const wchar_t* path, std::vector<std::string>* names) {
    if (names) {
        names->clear();
    }
    if (!path || !path[0] || !names) {
        return false;
    }
    HANDLE file = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    struct FileCloser {
        HANDLE handle;
        ~FileCloser() { ::CloseHandle(handle); }
    } closer{file};
    LARGE_INTEGER fileSize = {};
    if (!::GetFileSizeEx(file, &fileSize) || fileSize.QuadPart <= 0 ||
        static_cast<uint64_t>(fileSize.QuadPart) > kMaxFileBytes) {
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(fileSize.QuadPart));
    size_t total = 0;
    while (total < bytes.size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(bytes.size() - total > (1u << 20) ? (1u << 20) : bytes.size() - total);
        if (!::ReadFile(file, bytes.data() + total, chunk, &read, nullptr) || read == 0) {
            return false;
        }
        total += read;
    }
    return ParseExportNames(bytes.data(), bytes.size(), names);
}

}  // namespace ce::pe_file_exports
