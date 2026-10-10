// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "startup_imports.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>

namespace ce::startup_imports {
namespace {

constexpr DWORD kMaxHeader = 1024 * 1024;
constexpr DWORD kMaxImage = 0x80000000u;
constexpr unsigned kMaxImports = 512;
constexpr unsigned kMaxName = 32768;

bool InRange(DWORD rva, size_t bytes, DWORD size) {
    return rva <= size && bytes <= static_cast<size_t>(size - rva);
}

template <typename T> bool Read(const Reader& read, DWORD rva, T& value) {
    return read(rva, std::as_writable_bytes(std::span(&value, 1)));
}

bool ReadName(const Reader& read, DWORD rva, DWORD size, std::string& name) {
    name.clear();
    std::array<char, 32> buffer{};
    while (name.size() < kMaxName && InRange(rva, 1, size)) {
        size_t count = std::min({buffer.size(), static_cast<size_t>(size - rva), kMaxName - name.size()});
        if (!read(rva, std::as_writable_bytes(std::span(buffer.data(), count)))) {
            // A valid name can end immediately before an unreadable page.
            count = 1;
            if (!Read(read, rva, buffer[0])) return false;
        }
        for (size_t i = 0; i < count; ++i) {
            if (!buffer[i]) return !name.empty();
            name.push_back(buffer[i]);
        }
        rva += static_cast<DWORD>(count);
    }
    return false;
}

template <typename Header> Status InspectHeader(const Reader& read, DWORD ntRva, Image& image) {
    Header header{};
    if (!Read(read, ntRva, header) || header.Signature != IMAGE_NT_SIGNATURE ||
        header.FileHeader.SizeOfOptionalHeader < sizeof(header.OptionalHeader) ||
        header.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT) {
        return Status::InvalidImage;
    }
    image.machine = header.FileHeader.Machine;
    image.size = header.OptionalHeader.SizeOfImage;
    if (image.size == 0 || image.size > kMaxImage ||
        !InRange(ntRva, sizeof(header), image.size)) return Status::InvalidImage;
    image.boundDirectoryRva = ntRva + offsetof(Header, OptionalHeader) +
        offsetof(decltype(header.OptionalHeader), DataDirectory) +
        IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT * sizeof(IMAGE_DATA_DIRECTORY);
    image.boundDirectory = header.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT];
    const auto directory = header.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0 && directory.Size == 0) return Status::Unchanged;
    if (directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) ||
        !InRange(directory.VirtualAddress, directory.Size, image.size)) return Status::InvalidImage;
    for (unsigned i = 0; i < kMaxImports; ++i) {
        const size_t offset = static_cast<size_t>(i) * sizeof(IMAGE_IMPORT_DESCRIPTOR);
        if (!InRange(static_cast<DWORD>(offset), sizeof(IMAGE_IMPORT_DESCRIPTOR), directory.Size))
            return Status::InvalidImage;
        Import entry;
        entry.descriptorRva = directory.VirtualAddress + static_cast<DWORD>(offset);
        if (!Read(read, entry.descriptorRva, entry.descriptor)) return Status::InvalidImage;
        const IMAGE_IMPORT_DESCRIPTOR empty{};
        if (std::memcmp(&entry.descriptor, &empty, sizeof(empty)) == 0) return Status::Unchanged;
        if (!entry.descriptor.Name || !entry.descriptor.FirstThunk ||
            !InRange(entry.descriptor.FirstThunk, sizeof(DWORD), image.size) ||
            !ReadName(read, entry.descriptor.Name, image.size, entry.name)) return Status::InvalidImage;
        image.imports.push_back(std::move(entry));
    }
    return Status::InvalidImage;
}

bool RemoteRead(HANDLE process, uintptr_t address, std::span<std::byte> bytes) {
    SIZE_T count = 0;
    return ReadProcessMemory(process, reinterpret_cast<void*>(address), bytes.data(), bytes.size(), &count) &&
        count == bytes.size();
}

uintptr_t FindExecutable(HANDLE process) {
    uintptr_t address = 0;
    MEMORY_BASIC_INFORMATION region{};
    // VirtualQueryEx jumps across entire free regions; no page-by-page scan or
    // private PEB/loader offsets, including when inspecting a WOW64 child.
    for (unsigned i = 0; i < 65536 && VirtualQueryEx(process, reinterpret_cast<void*>(address),
                                                    &region, sizeof(region)); ++i) {
        if (region.Type == MEM_IMAGE && region.AllocationBase == region.BaseAddress) {
            const auto base = reinterpret_cast<uintptr_t>(region.BaseAddress);
            IMAGE_DOS_HEADER dos{};
            if (RemoteRead(process, base, std::as_writable_bytes(std::span(&dos, 1))) &&
                dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew >= static_cast<LONG>(sizeof(dos)) &&
                static_cast<DWORD>(dos.e_lfanew) < kMaxHeader) {
                struct Prefix { DWORD signature; IMAGE_FILE_HEADER file; } prefix{};
                if (RemoteRead(process, base + dos.e_lfanew, std::as_writable_bytes(std::span(&prefix, 1))) &&
                    prefix.signature == IMAGE_NT_SIGNATURE && !(prefix.file.Characteristics & IMAGE_FILE_DLL))
                    return base;
            }
        }
        const auto start = reinterpret_cast<uintptr_t>(region.BaseAddress);
        if (region.RegionSize > std::numeric_limits<uintptr_t>::max() - start) break;
        const auto next = start + region.RegionSize;
        if (next <= address) break;
        address = next;
    }
    return 0;
}

void* AllocateName(HANDLE process, uintptr_t base, size_t bytes) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const uintptr_t granularity = info.dwAllocationGranularity;
    const auto maxAddress = std::numeric_limits<uintptr_t>::max();
    const uintptr_t limit = base > maxAddress - MAXDWORD ? maxAddress : base + MAXDWORD;
    if (base > maxAddress - granularity) return nullptr;
    uintptr_t address = (base + granularity) & ~(granularity - 1);
    MEMORY_BASIC_INFORMATION region{};
    while (address < limit && bytes <= limit - address &&
           VirtualQueryEx(process, reinterpret_cast<void*>(address), &region, sizeof(region))) {
        const auto start = reinterpret_cast<uintptr_t>(region.BaseAddress);
        if (region.RegionSize > maxAddress - start) break;
        const auto end = start + region.RegionSize;
        if (region.State == MEM_FREE && end > address && bytes <= end - address) {
            if (void* allocation = VirtualAllocEx(process, reinterpret_cast<void*>(address), bytes,
                                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return allocation;
        }
        if (end > maxAddress - (granularity - 1)) break;
        const auto next = (end + granularity - 1) & ~(granularity - 1);
        if (next <= address) break;
        address = next;
    }
    return nullptr;
}

bool Write(HANDLE process, uintptr_t address, const void* data, size_t bytes, DWORD* originalProtect = nullptr) {
    DWORD oldProtect = 0;
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(address), bytes, PAGE_READWRITE, &oldProtect))
        return false;
    if (originalProtect) *originalProtect = oldProtect;
    SIZE_T written = 0;
    const bool ok = WriteProcessMemory(process, reinterpret_cast<void*>(address), data, bytes, &written) &&
        written == bytes;
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    DWORD unused = 0;
    const bool restored = VirtualProtectEx(process, reinterpret_cast<void*>(address), bytes, oldProtect, &unused);
    if (!ok) SetLastError(error);
    return ok && restored;
}

bool Restore(HANDLE process, uintptr_t address, const void* data, size_t bytes, DWORD protection) {
    if (!protection) return true;  // Protection acquisition failed before any write.
    const bool written = Write(process, address, data, bytes);
    DWORD unused = 0;
    const bool protectedAgain = VirtualProtectEx(process, reinterpret_cast<void*>(address), bytes, protection, &unused);
    return written && protectedAgain;
}

}  // namespace

Status Inspect(const Reader& read, Image& image) {
    image = {};
    IMAGE_DOS_HEADER dos{};
    if (!Read(read, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos)) ||
        static_cast<DWORD>(dos.e_lfanew) > kMaxHeader) return Status::InvalidImage;
    const DWORD ntRva = static_cast<DWORD>(dos.e_lfanew);
    WORD magic = 0;
    if (!Read(read, ntRva + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER), magic)) return Status::InvalidImage;
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) return InspectHeader<IMAGE_NT_HEADERS64>(read, ntRva, image);
    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) return InspectHeader<IMAGE_NT_HEADERS32>(read, ntRva, image);
    return Status::UnsupportedImage;
}

Result Redirect(HANDLE process, const std::string& module, const std::string& absolutePath) {
    Result result;
    if (module.empty() || absolutePath.empty() || absolutePath.size() >= kMaxName ||
        absolutePath.find('\0') != std::string::npos ||
        (!(absolutePath.size() > 2 && absolutePath[1] == ':' &&
           (absolutePath[2] == '\\' || absolutePath[2] == '/')) && !absolutePath.starts_with("\\\\"))) {
        result.status = Status::Failed;
        result.error = ERROR_INVALID_PARAMETER;
        return result;
    }
    const uintptr_t base = FindExecutable(process);
    if (!base) {
        result.status = Status::Failed;
        result.error = ERROR_BAD_EXE_FORMAT;
        return result;
    }
    Image image;
    result.status = Inspect([&](DWORD rva, std::span<std::byte> bytes) {
        return rva <= std::numeric_limits<uintptr_t>::max() - base &&
            RemoteRead(process, base + rva, bytes);
    }, image);
    result.machine = image.machine;
    if (result.status != Status::Unchanged) return result;
    std::vector<Import> selected;
    for (const auto& entry : image.imports) {
        if (_stricmp(entry.name.c_str(), module.c_str()) != 0) continue;
        // A bound IAT without a separate lookup table cannot be safely rebound.
        if (entry.descriptor.TimeDateStamp && !entry.descriptor.OriginalFirstThunk) {
            result.status = Status::UnsupportedImage;
            return result;
        }
        selected.push_back(entry);
    }
    if (selected.empty()) return result;
    void* name = AllocateName(process, base, absolutePath.size() + 1);
    if (!name) {
        result.status = Status::Failed;
        result.error = GetLastError();
        return result;
    }
    SIZE_T count = 0;
    bool ok = WriteProcessMemory(process, name, absolutePath.c_str(), absolutePath.size() + 1, &count) &&
        count == absolutePath.size() + 1;
    DWORD protection = 0;
    ok = ok && VirtualProtectEx(process, name, absolutePath.size() + 1, PAGE_READONLY, &protection);
    const IMAGE_DATA_DIRECTORY empty{};
    // Invalidate bound imports, then redirect names. No code executes and no
    // second interposer is loaded: Windows resolves the original lookup tables.
    unsigned attempted = 0;
    DWORD boundProtection = 0;
    std::array<DWORD, kMaxImports> descriptorProtection{};
    if (ok) ok = Write(process, base + image.boundDirectoryRva, &empty, sizeof(empty), &boundProtection);
    if (ok) {
        for (const auto& entry : selected) {
            auto descriptor = entry.descriptor;
            descriptor.Name = static_cast<DWORD>(reinterpret_cast<uintptr_t>(name) - base);
            descriptor.TimeDateStamp = 0;
            ++attempted;
            if (!Write(process, base + entry.descriptorRva, &descriptor, sizeof(descriptor),
                       &descriptorProtection[attempted - 1])) {
                ok = false;
                break;
            }
        }
    }
    if (!ok) {
        result.error = GetLastError();
        bool restored = true;
        for (unsigned i = 0; i < attempted; ++i)
            restored = Restore(process, base + selected[i].descriptorRva, &selected[i].descriptor,
                               sizeof(IMAGE_IMPORT_DESCRIPTOR), descriptorProtection[i]) && restored;
        restored = Restore(process, base + image.boundDirectoryRva, &image.boundDirectory,
                           sizeof(image.boundDirectory), boundProtection) && restored;
        if (restored) VirtualFreeEx(process, name, 0, MEM_RELEASE);
        result.status = restored ? Status::Failed : Status::RollbackFailed;
        return result;
    }
    result.status = Status::Redirected;
    result.redirected = static_cast<unsigned>(selected.size());
    return result;
}

}  // namespace ce::startup_imports
