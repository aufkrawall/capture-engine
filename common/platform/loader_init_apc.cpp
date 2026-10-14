#include "loader_init_apc.h"

#include <tlhelp32.h>
#include <winternl.h>

#include <cstddef>

namespace ce::loader_init_apc {

namespace {

// x64 PEB: Ldr (PPEB_LDR_DATA) and CrossProcessFlags (bit 1 = ProcessInitializing).
constexpr size_t kPebLdrOffset = 0x18;
constexpr size_t kPebCrossProcessFlagsOffset = 0x50;
constexpr uint32_t kProcessInitializingFlag = 0x2;

using PfnNtQueryInformationProcess = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
    ~ScopedHandle() {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    HANDLE get() const { return handle_; }
    explicit operator bool() const { return handle_ && handle_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE handle_;
};

uint64_t FileTimeValue(const FILETIME& time) {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

// The process's first thread is the one ntdll initializes the process on.
// Loader worker threads exist during initialization too, so pick by creation
// time rather than by snapshot order.
DWORD FindInitialThread(DWORD pid, DWORD* error) {
    ScopedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    if (!snapshot) {
        *error = GetLastError();
        return 0;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    DWORD initial = 0;
    uint64_t earliest = UINT64_MAX;
    for (BOOL more = Thread32First(snapshot.get(), &entry); more; more = Thread32Next(snapshot.get(), &entry)) {
        if (entry.th32OwnerProcessID != pid) {
            continue;
        }
        ScopedHandle thread(OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID));
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!thread || !GetThreadTimes(thread.get(), &creation, &exit, &kernel, &user)) {
            continue;
        }
        const uint64_t created = FileTimeValue(creation);
        if (created < earliest) {
            earliest = created;
            initial = entry.th32ThreadID;
        }
    }
    if (!initial) {
        *error = ERROR_NOT_FOUND;
    }
    return initial;
}

}  // namespace

Phase ReadPhase(HANDLE process, DWORD* error) {
    DWORD ignored = 0;
    if (!error) {
        error = &ignored;
    }
    static const auto query = reinterpret_cast<PfnNtQueryInformationProcess>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (!query) {
        *error = ERROR_PROC_NOT_FOUND;
        return Phase::Unreadable;
    }
    PROCESS_BASIC_INFORMATION basic{};
    ULONG returned = 0;
    const NTSTATUS status = query(process, ProcessBasicInformation, &basic, sizeof(basic), &returned);
    if (status < 0 || !basic.PebBaseAddress) {
        *error = static_cast<DWORD>(status);
        return Phase::Unreadable;
    }
    const auto* peb = reinterpret_cast<const unsigned char*>(basic.PebBaseAddress);
    uint64_t ldr = 0;
    uint32_t flags = 0;
    if (!ReadProcessMemory(process, peb + kPebLdrOffset, &ldr, sizeof(ldr), nullptr) ||
        !ReadProcessMemory(process, peb + kPebCrossProcessFlagsOffset, &flags, sizeof(flags), nullptr)) {
        *error = GetLastError();
        return Phase::Unreadable;
    }
    return ClassifyPhase(ldr, flags);
}

const char* PhaseName(Phase phase) {
    switch (phase) {
        case Phase::NotStarted: return "not-started";
        case Phase::Initializing: return "initializing";
        case Phase::Complete: return "complete";
        case Phase::Unsupported: return "unsupported";
        case Phase::Unreadable: return "unreadable";
    }
    return "unknown";
}

Phase ClassifyPhase(uint64_t pebLdr, uint32_t crossProcessFlags) {
    if (pebLdr == 0) {
        return Phase::NotStarted;
    }
    return (crossProcessFlags & kProcessInitializingFlag) != 0 ? Phase::Initializing : Phase::Complete;
}

bool RunsBeforeEntryPoint(Phase phase) {
    return phase == Phase::NotStarted || phase == Phase::Initializing;
}

Result QueueBeforeEntryPoint(HANDLE process, DWORD pid, PAPCFUNC routine, ULONG_PTR argument) {
    Result result;
#if defined(_WIN64)
    BOOL wow64 = FALSE;
    if (!process || !routine || pid == GetCurrentProcessId() || !IsWow64Process(process, &wow64) || wow64) {
        result.phase = Phase::Unsupported;
        return result;
    }
    result.initialThreadId = FindInitialThread(pid, &result.error);
    if (!result.initialThreadId) {
        return result;
    }
    ScopedHandle thread(OpenThread(THREAD_SUSPEND_RESUME | THREAD_SET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                   FALSE, result.initialThreadId));
    if (!thread) {
        result.error = GetLastError();
        return result;
    }
    // Suspended, the thread cannot pass ntdll's APC delivery between the read
    // and the queue call, so the phase read here is the phase the APC meets.
    if (SuspendThread(thread.get()) == static_cast<DWORD>(-1)) {
        result.error = GetLastError();
        return result;
    }
    result.phase = ReadPhase(process, &result.error);
    if (RunsBeforeEntryPoint(result.phase)) {
        if (QueueUserAPC(routine, thread.get(), argument)) {
            result.queued = true;
        } else {
            result.error = GetLastError();
        }
    }
    ResumeThread(thread.get());
#else
    (void)process;
    (void)pid;
    (void)routine;
    (void)argument;
    result.phase = Phase::Unsupported;
#endif
    return result;
}

}  // namespace ce::loader_init_apc
