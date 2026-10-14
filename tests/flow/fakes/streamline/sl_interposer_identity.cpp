// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <windows.h>

// Which physical copy of the fake interposer answered: the takeover flow
// scenarios import this name, which no takeover thunk gates, and check that it
// moves to the configured copy together with the thunked sl* API.
extern "C" __declspec(dllexport) HMODULE flowInterposerIdentity() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&flowInterposerIdentity), &self);
    return self;
}
