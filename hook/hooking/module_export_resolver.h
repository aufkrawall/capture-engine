#pragma once

#include <windows.h>
#include <psapi.h>
#include <cstddef>
#include <cstring>

namespace ce::module_export {

// Resolve a named export directly from a loaded module's PE export directory.
// This bypasses GetProcAddress interceptors while preserving an inline patch
// already installed at the returned export entry.
inline void* ResolveAddressDirect(HMODULE exportingModule, const char* functionName) {
    if (!exportingModule || !functionName || !*functionName)
        return nullptr;

    MODULEINFO moduleInfo = {};
    if (!GetModuleInformation(GetCurrentProcess(), exportingModule, &moduleInfo, sizeof(moduleInfo)) ||
        moduleInfo.lpBaseOfDll != exportingModule || moduleInfo.SizeOfImage < sizeof(IMAGE_DOS_HEADER)) {
        return nullptr;
    }

    auto* base = static_cast<BYTE*>(moduleInfo.lpBaseOfDll);
    const size_t imageSize = static_cast<size_t>(moduleInfo.SizeOfImage);
    const auto contains = [imageSize](size_t offset, size_t size) {
        return offset <= imageSize && size <= imageSize - offset;
    };
    const auto arrayFits = [&contains, imageSize](DWORD offset, DWORD count, size_t elementSize) {
        return elementSize != 0 && static_cast<size_t>(count) <= imageSize / elementSize &&
               contains(static_cast<size_t>(offset), static_cast<size_t>(count) * elementSize);
    };

    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        !contains(static_cast<size_t>(dos->e_lfanew), sizeof(IMAGE_NT_HEADERS))) {
        return nullptr;
    }
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) {
        return nullptr;
    }

    const IMAGE_DATA_DIRECTORY& exportData = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!exportData.VirtualAddress || exportData.Size < sizeof(IMAGE_EXPORT_DIRECTORY) ||
        !contains(exportData.VirtualAddress, exportData.Size)) {
        return nullptr;
    }
    auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + exportData.VirtualAddress);
    if (!arrayFits(exports->AddressOfNames, exports->NumberOfNames, sizeof(DWORD)) ||
        !arrayFits(exports->AddressOfNameOrdinals, exports->NumberOfNames, sizeof(WORD)) ||
        !arrayFits(exports->AddressOfFunctions, exports->NumberOfFunctions, sizeof(DWORD))) {
        return nullptr;
    }

    auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
    auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        const DWORD nameRva = names[i];
        if (!contains(nameRva, 1))
            return nullptr;
        const char* name = reinterpret_cast<const char*>(base + nameRva);
        if (!std::memchr(name, '\0', imageSize - nameRva))
            return nullptr;
        if (std::strcmp(name, functionName) != 0)
            continue;

        const WORD ordinal = ordinals[i];
        if (ordinal >= exports->NumberOfFunctions)
            return nullptr;
        const DWORD functionRva = functions[ordinal];
        if (!functionRva || !contains(functionRva, 1))
            return nullptr;

        const size_t exportStart = exportData.VirtualAddress;
        const size_t exportEnd = exportStart + exportData.Size;
        if (functionRva >= exportStart && functionRva < exportEnd) {
            // Forwarded exports are strings inside the export directory and
            // need loader resolution. NGX core exports are direct code.
            return nullptr;
        }
        return base + functionRva;
    }
    return nullptr;
}

}  // namespace ce::module_export
