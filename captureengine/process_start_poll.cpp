#include "process_start_poll.h"

#include <winternl.h>

#include <unordered_set>
#include <vector>

#include "../common/logging.h"

namespace ce::process_start {

namespace {

// SYSTEM_PROCESS_INFORMATION as ntdll actually returns it. winternl.h declares
// the type but documents only a prefix of it, and the members CE needs
// (NextEntryOffset, ImageName, UniqueProcessId) are all inside that documented
// prefix. Declaring the walk against the documented fields only is what keeps
// this from depending on the rest of the layout, which is version-specific.
constexpr ULONG kSystemProcessInformationClass = 5;  // SystemProcessInformation

using PfnNtQuerySystemInformation = NTSTATUS(NTAPI*)(ULONG systemInformationClass, PVOID systemInformation,
                                                     ULONG systemInformationLength, PULONG returnLength);

PfnNtQuerySystemInformation ResolveNtQuerySystemInformation() {
    // ntdll is mapped in every process and never unloaded, so this needs no
    // module reference and cannot go stale.
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    // FARPROC is already a function pointer, so this converts directly between
    // function pointer types. Routing it through void* would be the
    // bugprone-casting-through-void pattern and buys nothing here.
    return ntdll ? reinterpret_cast<PfnNtQuerySystemInformation>(GetProcAddress(ntdll, "NtQuerySystemInformation"))
                 : nullptr;
}

std::string Utf8FromUnicodeString(const UNICODE_STRING& value) {
    if (!value.Buffer || value.Length == 0) {
        return {};
    }
    const int wideLength = static_cast<int>(value.Length / sizeof(wchar_t));
    const int byteLength = WideCharToMultiByte(CP_UTF8, 0, value.Buffer, wideLength, nullptr, 0, nullptr, nullptr);
    if (byteLength <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(byteLength), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.Buffer, wideLength, result.data(), byteLength, nullptr, nullptr) !=
        byteLength) {
        return {};
    }
    return result;
}

// Fills `out` with (pid, image name) for every live process. Returns false when
// the snapshot could not be taken at all, which must leave the caller's previous
// baseline untouched rather than be read as "every process exited".
bool SnapshotProcesses(PfnNtQuerySystemInformation query, std::vector<std::pair<DWORD, std::string>>& out) {
    if (!query) {
        return false;
    }
    out.clear();

    // The process set can grow between the size query and the read, so the
    // buffer is grown until the call stops reporting a length mismatch. The
    // bound stops a pathological system from turning this into a spin.
    std::vector<unsigned char> buffer(256 * 1024);
    for (int attempt = 0; attempt < 8; ++attempt) {
        ULONG returned = 0;
        const NTSTATUS status =
            query(kSystemProcessInformationClass, buffer.data(), static_cast<ULONG>(buffer.size()), &returned);
        if (status == 0) {
            break;
        }
        // STATUS_INFO_LENGTH_MISMATCH
        if (status != static_cast<NTSTATUS>(0xC0000004L)) {
            return false;
        }
        const size_t suggested = returned != 0 ? static_cast<size_t>(returned) : buffer.size();
        buffer.resize(suggested + 64 * 1024);
        if (attempt == 7) {
            return false;
        }
    }

    size_t offset = 0;
    while (offset + sizeof(SYSTEM_PROCESS_INFORMATION) <= buffer.size()) {
        const auto* entry = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(buffer.data() + offset);
        const auto pid = static_cast<DWORD>(reinterpret_cast<uintptr_t>(entry->UniqueProcessId));
        if (pid != 0) {
            out.emplace_back(pid, Utf8FromUnicodeString(entry->ImageName));
        }
        if (entry->NextEntryOffset == 0) {
            break;
        }
        const size_t next = offset + entry->NextEntryOffset;
        if (next <= offset || next >= buffer.size()) {
            break;  // Malformed or truncated chain: stop rather than walk out of the buffer.
        }
        offset = next;
    }
    return true;
}

}  // namespace

Poller::~Poller() {
    Stop();
}

bool Poller::Start(StartCallback callback, unsigned intervalMs) {
    if (thread_ || !callback) {
        return false;
    }
    callback_ = std::move(callback);
    intervalMs_ = ClampPollIntervalMs(intervalMs);

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) {
        LogError("[ProcessPoll] Cannot create the stop event (error=%lu)", GetLastError());
        callback_ = nullptr;
        return false;
    }

    thread_ = CreateThread(
        nullptr, 0,
        [](LPVOID parameter) -> DWORD {
            static_cast<Poller*>(parameter)->Run();
            return 0;
        },
        this, 0, nullptr);
    if (!thread_) {
        LogError("[ProcessPoll] Cannot start the poll thread (error=%lu)", GetLastError());
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
        callback_ = nullptr;
        return false;
    }
    LogInfo("[ProcessPoll] Native process-start poll active at %u ms", intervalMs_);
    return true;
}

void Poller::Stop() {
    if (stopEvent_) {
        SetEvent(stopEvent_);
    }
    if (thread_) {
        // The sweep is bounded work with no blocking calls, so the thread always
        // reaches its wait; a deadline here would be a timing assumption, not a
        // safety net.
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
    callback_ = nullptr;
}

bool Poller::IsRunning() const {
    return thread_ != nullptr;
}

void Poller::Run() {
    const PfnNtQuerySystemInformation query = ResolveNtQuerySystemInformation();
    if (!query) {
        LogError("[ProcessPoll] ntdll!NtQuerySystemInformation unavailable; no process-start notifications");
        return;
    }

    std::unordered_set<DWORD> known;
    std::vector<std::pair<DWORD, std::string>> snapshot;
    bool baselineEstablished = false;

    for (;;) {
        if (SnapshotProcesses(query, snapshot)) {
            if (!baselineEstablished) {
                // Everything alive right now predates CE's interest in it.
                // Reporting this sweep would hand the injector the whole process
                // table as "just started"; the existing-process scan owns that.
                for (const auto& [pid, name] : snapshot) {
                    (void)name;
                    known.insert(pid);
                }
                baselineEstablished = true;
            } else {
                std::unordered_set<DWORD> current;
                current.reserve(snapshot.size());
                for (const auto& [pid, name] : snapshot) {
                    current.insert(pid);
                    if (known.find(pid) == known.end() && !name.empty()) {
                        callback_(pid, name);
                    }
                }
                // Replacing rather than merging is what stops `known` from
                // growing without bound across a long session, and it is also
                // what lets a recycled PID be reported again - which is correct,
                // because a recycled PID IS a different process.
                known = std::move(current);
            }
        }

        if (WaitForSingleObject(stopEvent_, intervalMs_) == WAIT_OBJECT_0) {
            return;
        }
    }
}

}  // namespace ce::process_start
