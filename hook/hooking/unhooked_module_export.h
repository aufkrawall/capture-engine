#pragma once

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

// Disk exports bypass another overlay's in-memory EAT edits. Modules passed to
// this cache must remain loaded for its lifetime (the graphics runtimes are pinned).
namespace ce::module_export {

inline void* ResolveUnhookedExport(HMODULE module, const char* funcName) {
    if (!module || !funcName || !*funcName)
        return nullptr;

    wchar_t path[MAX_PATH] = {};
    const DWORD pathLength = GetModuleFileNameW(module, path, MAX_PATH);
    if (pathLength == 0 || pathLength >= MAX_PATH)
        return nullptr;

    HMODULE hDisk = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    if (!hDisk)
        return nullptr;

    // Low bits encode the load type; mask them to get the actual image base.
    auto* base = reinterpret_cast<const BYTE*>(reinterpret_cast<uintptr_t>(hDisk) & ~uintptr_t{3});
    void* result = nullptr;

    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const auto& ed = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (ed.VirtualAddress) {
            auto exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + ed.VirtualAddress);
            auto names = reinterpret_cast<const DWORD*>(base + exp->AddressOfNames);
            auto ordinals = reinterpret_cast<const WORD*>(base + exp->AddressOfNameOrdinals);
            auto functions = reinterpret_cast<const DWORD*>(base + exp->AddressOfFunctions);
            for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
                if (strcmp(reinterpret_cast<const char*>(base + names[i]), funcName) == 0) {
                    DWORD rva = functions[ordinals[i]];
                    if (rva >= ed.VirtualAddress && rva - ed.VirtualAddress < ed.Size) {
                        // A forwarder RVA names another DLL's export, not executable code.
                        // Let the Windows loader resolve its target (including API sets and ordinals).
                        result = reinterpret_cast<void*>(GetProcAddress(module, funcName));
                    } else if (rva != 0) {
                        result = reinterpret_cast<BYTE*>(module) + rva;
                    }
                    break;
                }
            }
        }
    }

    FreeLibrary(hDisk);
    return result;
}

// Each module keeps its own immutable result. Resolve outside the cache lock:
// the loader can re-enter a factory wrapper or wait for another cache caller.
class CachedUnhookedExport {
public:
    using Resolver = std::function<void*(HMODULE, const char*)>;
    explicit CachedUnhookedExport(const char* name, Resolver resolve = &ResolveUnhookedExport)
        : name_(name), resolve_(std::move(resolve)) {}

    void* Get(HMODULE module) {
        if (!module)
            return nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& entry : entries_) {
                if (entry.first == module)
                    return entry.second;
            }
        }
        void* resolved = resolve_(module, name_);
        if (!resolved)
            return nullptr;  // a transient resolution failure is retried
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : entries_) {
            if (entry.first == module)
                return entry.second;
        }
        entries_.emplace_back(module, resolved);
        return resolved;
    }

private:
    const char* name_;
    Resolver resolve_;
    std::mutex mutex_;
    std::vector<std::pair<HMODULE, void*>> entries_;
};

}  // namespace ce::module_export
