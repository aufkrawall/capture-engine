#pragma once

// Thread names for attribution. With the helper roles on threads of one process (09, 11), a dump, WPA trace,
// Process Explorer or crash.log shows only thread ids unless every thread CaptureEngine starts says which role owns
// it. The name is the system's thread description (SetThreadDescription), so it crosses module boundaries
// (cengine.dll, mediaengine.dll, captureengine.exe) without shared state. Header-only: every module that starts
// threads includes it without a new link dependency.

#include <windows.h>

#include <cstddef>

namespace ce::platform {
namespace detail {

// Resolved at run time: a static import would make every module that names a thread (the hook DLL included)
// depend on Windows 10 1607's kernel32 exports at load time.
struct ThreadDescriptionFunctions {
    HRESULT(WINAPI* set)(HANDLE, PCWSTR) = nullptr;
    HRESULT(WINAPI* get)(HANDLE, PWSTR*) = nullptr;
};

inline const ThreadDescriptionFunctions& ThreadDescriptionApi() {
    static const ThreadDescriptionFunctions functions = [] {
        ThreadDescriptionFunctions resolved;
        if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll")) {
            resolved.set = reinterpret_cast<decltype(resolved.set)>(
                GetProcAddress(kernel, "SetThreadDescription"));
            resolved.get = reinterpret_cast<decltype(resolved.get)>(
                GetProcAddress(kernel, "GetThreadDescription"));
        }
        return resolved;
    }();
    return functions;
}

}  // namespace detail

// Names the calling thread, e.g. NameCurrentThread("CE media encoder"). ASCII; longer names are cut at 63 characters.
inline void NameCurrentThread(const char* name) {
    const auto setDescription = detail::ThreadDescriptionApi().set;
    if (!name || !setDescription)
        return;
    wchar_t wide[64];
    size_t length = 0;
    while (name[length] != '\0' && length + 1 < sizeof(wide) / sizeof(wide[0])) {
        wide[length] = static_cast<wchar_t>(static_cast<unsigned char>(name[length]));
        ++length;
    }
    wide[length] = L'\0';
    setDescription(GetCurrentThread(), wide);
}

// Copies the description of thread `threadId` into `out` as ASCII (anything else becomes '?'); returns its length.
// Empty when the thread has none or cannot be opened. Meant for crash reporting: it allocates nothing itself, only
// GetThreadDescription's own buffer, which it frees.
inline size_t DescribeThread(DWORD threadId, char* out, size_t capacity) {
    if (!out || capacity == 0)
        return 0;
    out[0] = '\0';
    const auto getDescription = detail::ThreadDescriptionApi().get;
    if (!getDescription)
        return 0;
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, threadId);
    if (!thread)
        return 0;
    PWSTR description = nullptr;
    size_t length = 0;
    if (SUCCEEDED(getDescription(thread, &description)) && description) {
        while (description[length] != L'\0' && length + 1 < capacity) {
            const wchar_t c = description[length];
            out[length] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
            ++length;
        }
        out[length] = '\0';
        LocalFree(description);
    }
    CloseHandle(thread);
    return length;
}

}  // namespace ce::platform
