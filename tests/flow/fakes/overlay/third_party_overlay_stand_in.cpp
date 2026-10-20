// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// A module named like a third-party overlay's hook (RivaTuner's RTSSHooks64.dll). It does nothing: the loader reports
// its load to the flow hook, which must report the overlay by name at the end of the scenario.
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE /*module*/, DWORD /*reason*/, LPVOID /*reserved*/) {
    return TRUE;
}
