// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <windows.h>

// As takeover_game_module.cpp, plus an import only the resident interposer
// variant exports: the configured copy cannot take every import over.
extern "C" __declspec(dllimport) int slInit(const void* preferences, unsigned long long sdkVersion);
extern "C" __declspec(dllimport) HMODULE flowInterposerIdentity();
extern "C" __declspec(dllimport) int flowResidentOnlyExport();

extern "C" __declspec(dllexport) int TakeoverGameInit() {
    return slInit(nullptr, 0);
}

extern "C" __declspec(dllexport) HMODULE TakeoverGameInterposer() {
    return flowResidentOnlyExport() == 7 ? flowInterposerIdentity() : nullptr;
}
