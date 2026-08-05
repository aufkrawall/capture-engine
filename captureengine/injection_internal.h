#pragma once

#include "injection.h"

#include <psapi.h>

#include <tlhelp32.h>

#include <algorithm>

#include <array>

#include <filesystem>

#include <iostream>

#include <system_error>

#include <thread>

#include <vector>

#include "../common/logging.h"

#include "../common/module_enumeration.h"

#include "../common/raii_helpers.h"

#include "../common/thread_wait.h"

#include "injection_policy.h"

#include <aclapi.h>

#include <bcrypt.h>

#include <ntstatus.h>

#include <oleauto.h>

#include <softpub.h>

#include <wintrust.h>

#include <fstream>

#include <iomanip>

#include <sstream>

#ifdef _MSC_VER
#pragma comment(lib, "wintrust.lib")
#endif

namespace fs = std::filesystem;

inline double injection_QpcDeltaToMs(int64_t deltaUs) {
    return static_cast<double>(deltaUs) / 1000.0;
}

// Read a null-terminated string from a remote process.
inline bool injection_ReadRemoteString(HANDLE hProc, LPCVOID address, char* buffer, size_t bufferSize) {
    if (!buffer || bufferSize == 0)
        return false;
    buffer[0] = '\0';
    size_t offset = 0;
    while (offset < bufferSize - 1) {
        char c;
        if (!ReadProcessMemory(hProc, static_cast<const char*>(address) + offset, &c, 1, NULL))
            return false;
        buffer[offset++] = c;
        if (c == '\0')
            return true;
    }
    buffer[bufferSize - 1] = '\0';
    return true;
}

// Resolve a function address in a remote 32-bit (WoW64) module by manually
// parsing its PE export directory. Used for cross-bitness injection where
// GetProcAddress from the local 64-bit module returns the wrong address.
inline LPVOID injection_GetRemoteProcAddress(HANDLE hProc, HMODULE hModule, const char* funcName) {
    IMAGE_DOS_HEADER dosHeader;
    if (!ReadProcessMemory(hProc, hModule, &dosHeader, sizeof(dosHeader), NULL))
        return nullptr;
    if (dosHeader.e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;

    BYTE* pNtHeaders = (BYTE*)hModule + dosHeader.e_lfanew;
    IMAGE_NT_HEADERS32 ntHeaders;
    if (!ReadProcessMemory(hProc, pNtHeaders, &ntHeaders, sizeof(ntHeaders), NULL))
        return nullptr;
    if (ntHeaders.Signature != IMAGE_NT_SIGNATURE)
        return nullptr;

    DWORD exportDirRVA = ntHeaders.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    if (exportDirRVA == 0)
        return nullptr;

    IMAGE_EXPORT_DIRECTORY exportDir;
    if (!ReadProcessMemory(hProc, (BYTE*)hModule + exportDirRVA, &exportDir, sizeof(exportDir), NULL))
        return nullptr;

    // Read Name Table
    std::vector<DWORD> nameRVAs(exportDir.NumberOfNames);
    if (!ReadProcessMemory(hProc, (BYTE*)hModule + exportDir.AddressOfNames, nameRVAs.data(),
                           nameRVAs.size() * sizeof(DWORD), NULL))
        return nullptr;

    for (DWORD i = 0; i < exportDir.NumberOfNames; i++) {
        char buffer[256];
        if (injection_ReadRemoteString(hProc, (BYTE*)hModule + nameRVAs[i], buffer, sizeof(buffer))) {
            if (strcmp(buffer, funcName) == 0) {
                WORD ordinal;
                if (!ReadProcessMemory(hProc, (BYTE*)hModule + exportDir.AddressOfNameOrdinals + (i * sizeof(WORD)),
                                       &ordinal, sizeof(WORD), NULL))
                    return nullptr;

                DWORD funcRVA;
                if (!ReadProcessMemory(hProc, (BYTE*)hModule + exportDir.AddressOfFunctions + (ordinal * sizeof(DWORD)),
                                       &funcRVA, sizeof(DWORD), NULL))
                    return nullptr;

                return (BYTE*)hModule + funcRVA;
            }
        }
    }
    return nullptr;
}

// Resolve a function address in a remote 32-bit module by module name.
// Opens the remote module enumeration and delegates to injection_GetRemoteProcAddress above.
inline LPVOID injection_GetRemoteModuleProcAddress(HANDLE hProc, const wchar_t* moduleName, const char* funcName) {
    int maxRetries = 20;
    for (int retry = 0; retry < maxRetries; retry++) {
        std::vector<HMODULE> hMods;
        if (ce::EnumerateProcessModulesEx(hProc, LIST_MODULES_32BIT, hMods)) {
            for (size_t i = 0; i < hMods.size(); i++) {
                char szModName[MAX_PATH];
                if (GetModuleFileNameExA(hProc, hMods[i], szModName, sizeof(szModName))) {
                    std::string modName = szModName;
                    std::transform(modName.begin(), modName.end(), modName.begin(),
                                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

                    // Convert wide module name to lower for comparison
                    char narrowModuleName[MAX_PATH];
                    WideCharToMultiByte(CP_UTF8, 0, moduleName, -1, narrowModuleName, sizeof(narrowModuleName), NULL,
                                        NULL);
                    std::string lowerTarget = narrowModuleName;
                    std::transform(lowerTarget.begin(), lowerTarget.end(), lowerTarget.begin(),
                                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

                    if (modName.find(lowerTarget) != std::string::npos) {
                        return injection_GetRemoteProcAddress(hProc, hMods[i], funcName);
                    }
                }
            }
        }
        Sleep(100);
    }
    return nullptr;
}

inline constexpr uint64_t injection_kPendingInjectionDelayMs = 1;

namespace {
struct BstrGuard {
    BSTR value = nullptr;

    explicit BstrGuard(const wchar_t* text) : value(SysAllocString(text)) {}

    ~BstrGuard() {
        if (value) {
            SysFreeString(value);
        }
    }

    BstrGuard(const BstrGuard&) = delete;
    BstrGuard& operator=(const BstrGuard&) = delete;

    operator BSTR() const {
        return value;
    }

    bool valid() const {
        return value != nullptr;
    }
};
}
