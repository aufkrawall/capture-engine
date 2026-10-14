#include "streamline_takeover_imports.h"

#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include "hook/hooking/iat_hook.h"
#include "hook/hooking/iat_import_table.h"
#include "hook/runtime/pe_file_exports.h"
#include "hook/runtime/runtime_override_paths.h"
#include "hook/streamline/streamline_interposer_takeover.h"

namespace ce::streamline_interposer_takeover {

namespace {

constexpr const char* kInterposerModuleName = "sl.interposer.dll";

struct ImageImports {
    BYTE* base = nullptr;
    size_t size = 0;
    DWORD directoryRva = 0;
    DWORD directorySize = 0;
};

bool GetImageImports(HMODULE module, ImageImports* out) {
    if (!module || !out) {
        return false;
    }
    MODULEINFO modInfo = {};
    if (!::GetModuleInformation(::GetCurrentProcess(), module, &modInfo, sizeof(modInfo)) ||
        modInfo.lpBaseOfDll != module || modInfo.SizeOfImage < sizeof(IMAGE_DOS_HEADER)) {
        return false;
    }
    auto* base = reinterpret_cast<BYTE*>(module);
    const size_t imageSize = modInfo.SizeOfImage;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        !IATHook::detail::ImageRangeContains(imageSize, static_cast<uintptr_t>(dos->e_lfanew),
                                             sizeof(IMAGE_NT_HEADERS))) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER) ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) {
        return false;
    }
    const auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.VirtualAddress == 0 || importDir.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) ||
        !IATHook::detail::ImageRangeContains(imageSize, importDir.VirtualAddress, importDir.Size)) {
        return false;
    }
    *out = {base, imageSize, importDir.VirtualAddress, importDir.Size};
    return true;
}

template <typename Visit>
IATHook::detail::ImportTableIssue ForEachInterposerImport(HMODULE module, Visit visit) {
    ImageImports imports;
    if (!GetImageImports(module, &imports)) {
        return IATHook::detail::ImportTableIssue::InvalidDirectory;
    }
    return IATHook::detail::ForEachImportOf(imports.base, imports.size, imports.directoryRva, imports.directorySize,
                                            kInterposerModuleName, visit,
                                            &IATHook::detail::IsCommittedReadableMemory);
}

bool IsTerminalIssue(IATHook::detail::ImportTableIssue issue) {
    // A walk ends with "unterminated descriptors" when the directory has no
    // null descriptor inside its declared size; the loader accepts that, and
    // every descriptor inside the size was visited.
    return issue != IATHook::detail::ImportTableIssue::None &&
           issue != IATHook::detail::ImportTableIssue::UnterminatedDescriptors;
}

}  // namespace

bool ModuleImportsDll(HMODULE module, const char* sourceModule) {
    if (!sourceModule || !sourceModule[0]) {
        return false;
    }
    ImageImports imports;
    if (!GetImageImports(module, &imports)) {
        return false;
    }
    const size_t count = imports.directorySize / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    const auto* descriptors = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(imports.base + imports.directoryRva);
    for (size_t i = 0; i < count; ++i) {
        if (descriptors[i].Name == 0) {
            break;
        }
        if (IATHook::detail::MatchImportModuleName(imports.base, imports.size, descriptors[i].Name, sourceModule,
                                                   &IATHook::detail::IsCommittedReadableMemory) ==
            IATHook::detail::ImageStringMatch::Match) {
            return true;
        }
    }
    return false;
}

bool IsThunkedExportName(const char* name) {
    if (!name) {
        return false;
    }
    for (const char* thunked : kTakeoverExportNames) {
        if (std::strcmp(thunked, name) == 0) {
            return true;
        }
    }
    return false;
}

ImportCoverage MeasureImportCoverage(const std::vector<HMODULE>& modules, const std::string& overridePathUtf8) {
    ImportCoverage coverage;
    std::vector<std::string> overrideExports;
    coverage.overrideExportsKnown = ce::pe_file_exports::ReadFileExportNames(
        ce::runtime_override_path::Utf8ToWide(overridePathUtf8).c_str(), &overrideExports);
    std::sort(overrideExports.begin(), overrideExports.end());
    for (HMODULE module : modules) {
        const auto issue = ForEachInterposerImport(
            module, [&](IATHook::detail::ImportNameKind kind, const char* name, IMAGE_THUNK_DATA*) {
                switch (kind) {
                    case IATHook::detail::ImportNameKind::Named:
                        RecordNamedImport(&coverage, name, IsThunkedExportName(name), overrideExports);
                        break;
                    case IATHook::detail::ImportNameKind::Ordinal:
                        ++coverage.ordinal;
                        break;
                    case IATHook::detail::ImportNameKind::Unnamed:
                        ++coverage.unnamed;
                        break;
                }
            });
        if (IsTerminalIssue(issue)) {
            coverage.malformed = true;
        }
    }
    return coverage;
}

DirectRebindResult CheckImportBindings(const std::vector<HMODULE>& modules, HMODULE resident,
                                      ThunkForName thunkForName) {
    DirectRebindResult result;
    for (HMODULE module : modules) {
        const auto issue = ForEachInterposerImport(module, [&](IATHook::detail::ImportNameKind kind,
                                                              const char* name, IMAGE_THUNK_DATA* entry) {
            void* previous = kind == IATHook::detail::ImportNameKind::Named && resident
                ? reinterpret_cast<void*>(::GetProcAddress(resident, name)) : nullptr;
            void* gate = thunkForName && name ? thunkForName(name) : nullptr;
            const auto current = std::atomic_ref<ULONG_PTR>(entry->u1.Function).load(std::memory_order_acquire);
            if (previous && current == reinterpret_cast<ULONG_PTR>(gate ? gate : previous)) {
                ++result.rebound;
            } else if (result.failed++ == 0) {
                result.firstFailed = name ? name : "unnamed import";
            }
        });
        if (IsTerminalIssue(issue) && result.failed++ == 0)
            result.firstFailed = IATHook::detail::ImportTableIssueName(issue);
    }
    return result;
}

DirectRebindResult BindUnthunkedImports(const std::vector<HMODULE>& modules, HMODULE resident, HMODULE custom,
                                       ThunkForName thunkForName) {
    DirectRebindResult result = CheckImportBindings(modules, resident, thunkForName);
    if (result.failed != 0)
        return result;
    result.rebound = 0;
    std::vector<IATHook::BaselineChange> changes;
    for (HMODULE module : modules) {
        const auto issue = ForEachInterposerImport(module, [&](IATHook::detail::ImportNameKind kind,
                                                              const char* name, IMAGE_THUNK_DATA* entry) {
            if (kind != IATHook::detail::ImportNameKind::Named || IsThunkedExportName(name))
                return;
            void* target = custom ? reinterpret_cast<void*>(::GetProcAddress(custom, name)) : nullptr;
            void* previous = reinterpret_cast<void*>(::GetProcAddress(resident, name));
            if (!target || !previous) {
                if (result.failed++ == 0)
                    result.firstFailed = name;
                return;
            }
            changes.push_back({reinterpret_cast<void**>(&entry->u1.Function), previous, target});
        });
        if (IsTerminalIssue(issue) && result.failed++ == 0)
            result.firstFailed = IATHook::detail::ImportTableIssueName(issue);
    }
    if (result.failed == 0) {
        if (IATHook::RebindIATBaselinesTransaction(changes)) {
            result.rebound = changes.size();
        } else {
            result.failed = 1;
            result.firstFailed = "import binding transaction";
        }
    }
    return result;
}

DirectRebindResult RebindUnthunkedImports(HMODULE module, HMODULE superseded, HMODULE custom) {
    DirectRebindResult result;
    if (!module || !custom) {
        return result;
    }
    std::vector<std::string> names;
    ForEachInterposerImport(module, [&](IATHook::detail::ImportNameKind kind, const char* name, IMAGE_THUNK_DATA*) {
        if (kind == IATHook::detail::ImportNameKind::Named && !IsThunkedExportName(name)) {
            names.emplace_back(name);
        }
    });
    for (const std::string& name : names) {
        void* target = reinterpret_cast<void*>(::GetProcAddress(custom, name.c_str()));
        void* previous = superseded ? reinterpret_cast<void*>(::GetProcAddress(superseded, name.c_str())) : nullptr;
        if (target && IATHook::RebindIATBaseline(module, kInterposerModuleName, name.c_str(), previous, target)) {
            ++result.rebound;
            continue;
        }
        if (result.failed++ == 0) {
            result.firstFailed = name;
        }
    }
    return result;
}

}  // namespace ce::streamline_interposer_takeover
