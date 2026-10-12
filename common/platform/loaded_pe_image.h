#pragma once

#include <windows.h>
#include <cstdint>
#include <limits>
#include <optional>

namespace ce::pe {
// Read only committed bytes from this allocation, never a neighbouring mapping.
inline bool Readable(const void* allocation, uintptr_t address, size_t bytes) {
    if (bytes > std::numeric_limits<uintptr_t>::max() - address) return false;
    const uintptr_t end = address + bytes;
    while (address < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) != sizeof(region) ||
            region.AllocationBase != allocation || region.State != MEM_COMMIT ||
            (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const auto begin = reinterpret_cast<uintptr_t>(region.BaseAddress);
        if (begin > address || region.RegionSize > std::numeric_limits<uintptr_t>::max() - begin) return false;
        const auto next = begin + region.RegionSize;
        if (next <= address) return false;
        address = next < end ? next : end;
    }
    return true;
}

struct LoadedImage {
    uint8_t* base = nullptr;
    size_t size = 0;
    const IMAGE_NT_HEADERS* headers = nullptr;
    bool Contains(size_t rva, size_t bytes) const { return rva <= size && bytes <= size - rva; }
    template<class T> const T* Read(size_t rva, size_t count = 1) const {
        if (count > std::numeric_limits<size_t>::max() / sizeof(T)) return nullptr;
        const size_t bytes = count * sizeof(T);
        if (!Contains(rva, bytes) || !Readable(base, reinterpret_cast<uintptr_t>(base) + rva, bytes)) return nullptr;
        return reinterpret_cast<const T*>(base + rva);
    }
    static std::optional<LoadedImage> Open(void* module) {
        const auto address = reinterpret_cast<uintptr_t>(module);
        if (!module || !Readable(module, address, sizeof(IMAGE_DOS_HEADER))) return std::nullopt;
        const auto* dos = static_cast<const IMAGE_DOS_HEADER*>(module);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < static_cast<LONG>(sizeof(*dos))) return std::nullopt;
        const auto offset = static_cast<size_t>(dos->e_lfanew);
        if (offset > std::numeric_limits<uintptr_t>::max() - address ||
            !Readable(module, address + offset, sizeof(IMAGE_NT_HEADERS))) return std::nullopt;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(address + offset);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC ||
            nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER)) return std::nullopt;
        LoadedImage image{static_cast<uint8_t*>(module), nt->OptionalHeader.SizeOfImage, nt};
        if (!image.Contains(offset, sizeof(*nt)) || !image.size ||
            image.size > std::numeric_limits<uintptr_t>::max() - address) return std::nullopt;
        // SizeOfImage itself is untrusted: every claimed image region must belong to this allocation.
        uintptr_t cursor = address;
        const uintptr_t end = address + image.size;
        while (cursor < end) {
            MEMORY_BASIC_INFORMATION region{};
            if (VirtualQuery(reinterpret_cast<void*>(cursor), &region, sizeof(region)) != sizeof(region) ||
                region.AllocationBase != module || !region.RegionSize) return std::nullopt;
            const auto begin = reinterpret_cast<uintptr_t>(region.BaseAddress);
            if (begin > cursor || region.RegionSize > std::numeric_limits<uintptr_t>::max() - begin) return std::nullopt;
            const auto next = begin + region.RegionSize;
            if (next <= cursor) return std::nullopt;
            cursor = next < end ? next : end;
        }
        const size_t sections = offset + offsetof(IMAGE_NT_HEADERS, OptionalHeader) + nt->FileHeader.SizeOfOptionalHeader;
        if (!image.Read<IMAGE_SECTION_HEADER>(sections, nt->FileHeader.NumberOfSections)) return std::nullopt;
        return image;
    }
};
}  // namespace ce::pe
