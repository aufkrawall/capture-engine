// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <windows.h>
#include "sl_core_types.h"

// A game module that statically imports the fake sl.interposer.dll, like a
// title linked against sl.interposer.lib: one thunked export (slInit) and one
// the takeover rebinds directly (flowInterposerIdentity).
extern "C" __declspec(dllimport) int slInit(const void* preferences, unsigned long long sdkVersion);
extern "C" __declspec(dllimport) HMODULE flowInterposerIdentity();

extern "C" __declspec(dllexport) int TakeoverGameInit() {
    sl::Preferences preferences{};
    return slInit(&preferences, 0);
}

extern "C" __declspec(dllexport) int TakeoverGameInitWithPreferences(const void* preferences,
                                                                   unsigned long long sdkVersion) {
    return slInit(preferences, sdkVersion);
}

extern "C" __declspec(dllexport) FARPROC TakeoverGameResolveInit() {
    return GetProcAddress(GetModuleHandleW(L"sl.interposer.dll"), "slInit");
}

extern "C" __declspec(dllexport) HMODULE TakeoverGameInterposer() {
    return flowInterposerIdentity();
}
