#pragma once

// Parent identity of a process, read from that process's own kernel record.
//
// Callers used to find a parent by walking a CreateToolhelp32Snapshot, which
// copies every process on the machine. These helpers touch only the process the
// caller already holds a handle to. Header-only, because the Vulkan negotiation
// gate links no CaptureEngine library and imports nothing beyond
// kernel32/advapi32: ntdll is resolved at run time for the same reason.

#include <windows.h>

#include <cstdint>
#include <string>

namespace ce::process_parent {

namespace detail {

// PROCESS_BASIC_INFORMATION with the parent field named; winternl.h calls it
// Reserved3.
struct ProcessBasicInformationView {
    LONG exitStatus;
    PVOID pebBaseAddress;
    ULONG_PTR affinityMask;
    LONG basePriority;
    ULONG_PTR uniqueProcessId;
    ULONG_PTR inheritedFromUniqueProcessId;
};
static_assert(sizeof(ProcessBasicInformationView) == 6 * sizeof(void*), "PROCESS_BASIC_INFORMATION layout");

using PfnNtQueryInformationProcess = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
inline constexpr ULONG kProcessBasicInformationClass = 0;

}  // namespace detail

// The PID `process` was created from. The handle needs
// PROCESS_QUERY_LIMITED_INFORMATION. The parent may have exited since and its
// PID may belong to a newer process; pair this with CreationTime when the
// answer must name the actual parent.
inline bool QueryParentProcessId(HANDLE process, DWORD* parentPid) {
    if (!parentPid)
        return false;
    *parentPid = 0;
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto query = ntdll ? reinterpret_cast<detail::PfnNtQueryInformationProcess>(
                                   GetProcAddress(ntdll, "NtQueryInformationProcess"))
                             : nullptr;
    detail::ProcessBasicInformationView basic = {};
    ULONG returned = 0;
    if (!query || query(process, detail::kProcessBasicInformationClass, &basic, sizeof(basic), &returned) < 0)
        return false;
    *parentPid = static_cast<DWORD>(basic.inheritedFromUniqueProcessId);
    return true;
}

// Creation time as a FILETIME tick count; 0 when it cannot be read.
inline uint64_t CreationTime(HANDLE process) {
    FILETIME created = {};
    FILETIME exited = {};
    FILETIME kernel = {};
    FILETIME user = {};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user))
        return 0;
    return (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

// A PID names a process's parent only if the process holding it now existed
// before the child. A later creation time means the parent exited and its PID
// was reused, so nothing about that process speaks for the child.
inline bool ParentPredatesChild(uint64_t parentCreationTime, uint64_t childCreationTime) {
    return parentCreationTime != 0 && childCreationTime != 0 && parentCreationTime <= childCreationTime;
}

// The full executable path of `process` in UTF-16; empty when it cannot be read.
inline std::wstring ImagePath(HANDLE process) {
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &length) || length == 0 || length >= path.size())
        return {};
    path.resize(length);
    return path;
}

// The executable base name of `process` in UTF-16; empty when it cannot be read.
inline std::wstring ImageBaseName(HANDLE process) {
    std::wstring path = ImagePath(process);
    if (path.empty())
        return {};
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

}  // namespace ce::process_parent
