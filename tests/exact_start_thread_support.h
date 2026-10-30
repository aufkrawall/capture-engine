// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include <windows.h>

// Creates a running thread whose Win32 start address (ThreadQuerySetWin32StartAddress) is exactly `start`.
//
// kernel32!CreateThread is not usable for tests that identify a thread by its start routine: the ASan runtime
// interposes it and starts every thread through its own thunk, so the kernel reports the sanitizer's address
// instead of the caller's routine (the sanitizer stage then fails while the plain build passes). NtCreateThreadEx
// is not interposed and records `start` verbatim, which is also what the product sees in an unsanitized process.
// Returns nullptr when ntdll lacks the export or the kernel refuses; callers must ASSERT a valid handle.
namespace ce::test_support {

inline HANDLE CreateThreadWithExactStart(LPTHREAD_START_ROUTINE start, LPVOID parameter) {
    using PFN_NtCreateThreadEx = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, PVOID, HANDLE, PVOID, PVOID, ULONG, SIZE_T,
                                              SIZE_T, SIZE_T, PVOID);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return nullptr;
    const auto create = reinterpret_cast<PFN_NtCreateThreadEx>(GetProcAddress(ntdll, "NtCreateThreadEx"));
    if (!create)
        return nullptr;
    HANDLE thread = nullptr;
    // Flags 0 = start running; zero stack sizes = image defaults; no attribute list.
    if (create(&thread, THREAD_ALL_ACCESS, nullptr, GetCurrentProcess(), reinterpret_cast<PVOID>(start), parameter, 0, 0,
               0, 0, nullptr) < 0)
        return nullptr;
    return thread;
}

}  // namespace ce::test_support
