// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <windows.h>
#include <psapi.h>
#include <cstdio>

extern "C" __declspec(dllimport) void slInit();

int main() {
    // Retain a genuine static import without initializing the fake SL runtime.
    auto volatile imported = &slInit;
    if (!imported) return 1;
    HMODULE modules[256]{};
    DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes) || bytes > sizeof(modules))
        return 2;
    unsigned count = 0;
    wchar_t path[32768]{};
    for (unsigned i = 0; i < bytes / sizeof(HMODULE); ++i) {
        wchar_t name[260]{};
        if (GetModuleBaseNameW(GetCurrentProcess(), modules[i], name, 260) &&
            _wcsicmp(name, L"sl.interposer.dll") == 0) {
            ++count;
            if (!GetModuleFileNameW(modules[i], path, 32768)) return 3;
        }
    }
    char utf8[65536]{};
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, utf8, sizeof(utf8), nullptr, nullptr)) return 4;
    std::printf("%u\n%s\n", count, utf8);
    return count == 1 ? 0 : 5;
}
