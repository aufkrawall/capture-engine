#include "streamline_hook_internal.h"


const char* GetModuleBaseName(const char* moduleNameOrPath) {


    if (!moduleNameOrPath || !moduleNameOrPath[0]) {
        return nullptr;
    }

    const char* baseName = moduleNameOrPath;
    for (const char* cursor = moduleNameOrPath; *cursor; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            baseName = cursor + 1;
        }
    }
    return baseName;

}


bool IsStreamlineModuleName(const char* moduleNameOrPath) {


    return ce::streamline_runtime_policy::IsStreamlineModuleNameForFeatureHooking(moduleNameOrPath);

}


bool ShouldHookStreamlineCoreExports(const char* moduleNameOrPath) {


    return ce::streamline_runtime_policy::ShouldHookStreamlineCoreExportsOnLoad(moduleNameOrPath);

}


bool IsStreamlineCoreDynamicHookModule(const char* moduleBaseName,  HMODULE) {


    return ShouldHookStreamlineCoreExports(moduleBaseName);

}


bool IsStreamlineDLSSGDynamicHookModule(const char* moduleBaseName,  HMODULE) {


    return ce::streamline_runtime_policy::IsStreamlineDLSSGFeatureModuleName(moduleBaseName);

}


bool IsStreamlineReflexDynamicHookModule(const char* moduleBaseName,  HMODULE) {


    return ce::streamline_runtime_policy::IsStreamlineReflexFeatureModuleName(moduleBaseName);

}


uint32_t GetModuleMaskBit(const char* moduleNameOrPath) {


    const char* baseName = GetModuleBaseName(moduleNameOrPath);
    if (!baseName) {
        return 0;
    }
    if (!_stricmp(baseName, "sl.interposer.dll")) {
        return 1u << 0;
    }
    if (!_stricmp(baseName, "sl.common.dll")) {
        return 1u << 1;
    }
    return 0;

}


void LogSkippedStreamlineCoreExportsOnce(const char* moduleBaseName,  HMODULE module,  bool hasGetFeature, 
                                         bool hasGetPlugin,  bool hasSetD3DDevice) {


    if (!moduleBaseName || !moduleBaseName[0]) {
        return;
    }

    static std::mutex s_logMutex;
    static std::unordered_map<std::string, bool> s_loggedModules;

    std::string key = moduleBaseName;
    key += '|';
    key += std::to_string(reinterpret_cast<uintptr_t>(module));

    {
        std::lock_guard<std::mutex> lock(s_logMutex);
        if (s_loggedModules.find(key) != s_loggedModules.end()) {
            return;
        }
        s_loggedModules.emplace(key, true);
    }

    HookLogImportant(
        "Streamline Hook: Skipping generic core exports for unloadable feature module %s (%p) "
        "getFeature=%d getPlugin=%d setD3DDevice=%d",
        moduleBaseName, module, hasGetFeature ? 1 : 0, hasGetPlugin ? 1 : 0, hasSetD3DDevice ? 1 : 0);

}


size_t GetModuleImageSizeBytes(HMODULE module) {


    if (!module) {
        return 0;
    }
    const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    const auto* ntHeaders =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(module) + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }
    return ntHeaders->OptionalHeader.SizeOfImage;

}


bool DoesAddressBelongToLoadedModule(void* address,  HMODULE* ownerModule,  char* ownerPath,  DWORD ownerPathCapacity, 
                                     DWORD* outError) {


    if (ownerModule) {
        *ownerModule = nullptr;
    }
    if (ownerPath && ownerPathCapacity > 0) {
        ownerPath[0] = '\0';
    }
    if (outError) {
        *outError = ERROR_SUCCESS;
    }
    if (!address) {
        if (outError) {
            *outError = ERROR_INVALID_ADDRESS;
        }

        return false;
    }

    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(address), &module) ||
        !module) {
        if (outError) {
            *outError = GetLastError();
        }
        return false;
    }

    if (ownerModule) {
        *ownerModule = module;
    }
    if (ownerPath && ownerPathCapacity > 0) {
        const DWORD pathLen = GetModuleFileNameA(module, ownerPath, ownerPathCapacity);
        if (pathLen == 0 || pathLen >= ownerPathCapacity) {
            if (outError) {
                *outError = pathLen >= ownerPathCapacity ? ERROR_INSUFFICIENT_BUFFER : GetLastError();
            }
            ownerPath[0] = '\0';
        }
    }
    return true;

}


bool TryGetOwningModulePath(void* address,  char* modulePath,  DWORD modulePathCapacity,  DWORD* outError) {


    if (outError) {
        *outError = ERROR_SUCCESS;
    }
    if (!address || !modulePath || modulePathCapacity == 0) {
        if (outError) {
            *outError = ERROR_INVALID_PARAMETER;
        }
        return false;
    }

    HMODULE ownerModule = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(address), &ownerModule) ||
        !ownerModule) {
        if (outError) {
            *outError = GetLastError();
        }
        return false;
    }

    const DWORD pathLen = GetModuleFileNameA(ownerModule, modulePath, modulePathCapacity);
    if (pathLen == 0 || pathLen >= modulePathCapacity) {
        if (outError) {
            *outError = pathLen >= modulePathCapacity ? ERROR_INSUFFICIENT_BUFFER : GetLastError();
        }
        return false;
    }
    return true;

}
