#include "runtime_process_identity.h"

#include <windows.h>

#include <mutex>

#include "common/logging/logging.h"
#include "common/platform/process_parent.h"

namespace ce::runtime_identity {

namespace {

std::mutex g_hostMutex;
HostIdentity g_host;

// The facts of a live process from one limited-information handle (no memory is read).
bool QueryFacts(uint32_t pid, ProcessFacts* facts) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return false;
    DWORD parentPid = 0;
    const bool haveParent = ce::process_parent::QueryParentProcessId(process, &parentPid);
    facts->pid = pid;
    facts->parentPid = parentPid;
    facts->creationTime = ce::process_parent::CreationTime(process);
    CloseHandle(process);
    return haveParent;
}

std::wstring ImagePathOfProcess(HANDLE process) {
    std::wstring path(1024, L'\0');
    for (;;) {
        DWORD length = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(process, 0, path.data(), &length)) {
            path.resize(length);
            return path;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || path.size() >= 32768)
            return {};
        path.resize(path.size() * 4);
    }
}

std::wstring ImagePathOf(uint32_t pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return {};
    std::wstring path = ImagePathOfProcess(process);
    CloseHandle(process);
    return path;
}

}  // namespace

bool SameImage(const std::wstring& lhs, const std::wstring& rhs) {
    return !lhs.empty() && lhs.size() == rhs.size() &&
           CompareStringOrdinal(lhs.c_str(), static_cast<int>(lhs.size()), rhs.c_str(), static_cast<int>(rhs.size()),
                                TRUE) == CSTR_EQUAL;
}

void RegisterHost(uint32_t hostPid) {
    RegisterHost(hostPid, ImagePathOfProcess(GetCurrentProcess()));
}

void RegisterHost(uint32_t hostPid, const std::wstring& roleHostImage) {
    if (hostPid == 0)
        return;
    ProcessFacts hostFacts;
    QueryFacts(hostPid, &hostFacts);
    const uint64_t creationTime = hostFacts.creationTime;
    {
        std::lock_guard<std::mutex> lock(g_hostMutex);
        if (g_host.pid == hostPid && g_host.roleHostImage == roleHostImage &&
            (g_host.creationTime == creationTime || creationTime == 0))
            return;
        g_host = HostIdentity{hostPid, creationTime, roleHostImage};
    }
    // The registration itself is already announced by the IPC accept line; only the degraded cases are
    // worth a line of their own.
    if (creationTime == 0 || roleHostImage.empty())
        LogWarn("[RuntimeIdentity] Runtime host PID %lu registered without %s; %s", static_cast<unsigned long>(hostPid),
                creationTime == 0 ? "a creation time" : "a role host image",
                creationTime == 0 ? "PID reuse cannot be told apart" : "its helper processes cannot be recognised");
}

HostIdentity RegisteredHost() {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    return g_host;
}

void ClearHostForTests() {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    g_host = HostIdentity{};
}

Relation ClassifyAgainstRegisteredHost(uint32_t pid) {
    const HostIdentity host = RegisteredHost();
    if (host.pid == 0)
        return Relation::Unrelated;
    return Classify(host, pid, QueryFacts, ImagePathOf);
}

bool IsRegisteredRuntimeProcess(uint32_t pid) {
    return ClassifyAgainstRegisteredHost(pid) != Relation::Unrelated;
}

}  // namespace ce::runtime_identity
