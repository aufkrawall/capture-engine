// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <windows.h>
#include "sl_core_types.h"

// A game module that statically imports the fake sl.interposer.dll, like a
// title linked against sl.interposer.lib: thunked exports (slInit and the four
// whose import slots CE's Streamline hook also patches, as witcher3.exe imports
// them) and one the takeover rebinds directly (flowInterposerIdentity).
extern "C" __declspec(dllimport) int slInit(const void* preferences, unsigned long long sdkVersion);
extern "C" __declspec(dllimport) int slSetD3DDevice(void* device);
extern "C" __declspec(dllimport) int slGetFeatureFunction(unsigned int feature, const char* name, void*& function);
extern "C" __declspec(dllimport) int slSetTagForFrame(const void*, const void*, const void*, unsigned int, void*);
extern "C" __declspec(dllimport) int slEvaluateFeature(unsigned int, const void*, const void**, unsigned int, void*);
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

extern "C" __declspec(dllexport) int TakeoverGameSetDevice(void* device) {
    return slSetD3DDevice(device);
}

// Keeps the remaining hooked-name imports in the import table.
extern "C" __declspec(dllexport) const void* TakeoverGameHookedImports(unsigned int index) {
    const void* imports[] = {reinterpret_cast<const void*>(&slGetFeatureFunction),
                             reinterpret_cast<const void*>(&slSetTagForFrame),
                             reinterpret_cast<const void*>(&slEvaluateFeature)};
    return index < 3 ? imports[index] : nullptr;
}
