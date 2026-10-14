// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <windows.h>

// A game module that statically imports the fake sl.interposer.dll, like a
// title linked against sl.interposer.lib: one thunked export (slInit) and one
// the takeover rebinds directly (flowInterposerIdentity).
extern "C" __declspec(dllimport) int slInit(const void* preferences, unsigned long long sdkVersion);
extern "C" __declspec(dllimport) HMODULE flowInterposerIdentity();

extern "C" __declspec(dllexport) int TakeoverGameInit() {
    return slInit(nullptr, 0);
}

extern "C" __declspec(dllexport) HMODULE TakeoverGameInterposer() {
    return flowInterposerIdentity();
}
