// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "runtime/children/process_residue.h"

#include <windows.h>

#include <dxgi1_4.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <map>
#include <utility>

#include "common/platform/thread_label.h"

namespace ce::runtime {
namespace {

std::vector<uint32_t> ThisProcessThreadIds() {
    std::vector<uint32_t> ids;
    const DWORD pid = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return ids;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID == pid)
            ids.push_back(entry.th32ThreadID);
    }
    CloseHandle(snapshot);
    std::sort(ids.begin(), ids.end());
    return ids;
}

// The module that contains `address`, as an ASCII base name; "?" when none does.
std::string ModuleBaseName(const void* address) {
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH];
    if (!address ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &module) ||
        GetModuleFileNameW(module, path, MAX_PATH) == 0)
        return "?";
    const wchar_t* base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    std::string ascii;
    for (; *base; ++base)
        ascii.push_back(*base < 0x80 ? static_cast<char>(*base) : '?');
    return ascii;
}

// The thread's name, else the module of its start address ("?" when neither can be read).
std::string AttributeThread(uint32_t threadId) {
    char name[64];
    if (ce::platform::DescribeThread(threadId, name, sizeof(name)) != 0)
        return std::string(1, '\'') + name + std::string(1, '\'');
    using QueryThread = LONG(WINAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<QueryThread>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    constexpr int kThreadQuerySetWin32StartAddress = 9;
    HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, threadId);
    if (!thread)
        return "?";
    void* start = nullptr;
    const LONG status = query ? query(thread, kThreadQuerySetWin32StartAddress, static_cast<void*>(&start),
                                      sizeof(start), nullptr) : -1;
    CloseHandle(thread);
    return status >= 0 ? ModuleBaseName(start) : std::string("?");
}

// This process's usage on every adapter (DXGI 1.4 reports it per process). False when no adapter reports it.
bool ThisProcessVideoMemory(uint64_t* bytes) {
    *bytes = 0;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory)
        return false;
    bool known = false;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) == S_OK; ++index) {
        IDXGIAdapter3* adapter3 = nullptr;
        if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3)))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO local{};
            DXGI_QUERY_VIDEO_MEMORY_INFO nonLocal{};
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
                SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal))) {
                *bytes += local.CurrentUsage + nonLocal.CurrentUsage;
                known = true;
            }
            adapter3->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return known;
}

std::string SignedCount(uint32_t before, uint32_t after) {
    char text[24];
    const long long delta = static_cast<long long>(after) - static_cast<long long>(before);
    snprintf(text, sizeof(text), "%+lld", delta);
    return text;
}

std::string SignedMiB(uint64_t before, uint64_t after) {
    char text[32];
    const double delta = (static_cast<double>(after) - static_cast<double>(before)) / (1024.0 * 1024.0);
    snprintf(text, sizeof(text), "%+.1fMiB", delta);
    return text;
}

}  // namespace

ProcessResidueSample SampleThisProcess() {
    ProcessResidueSample sample;
    HANDLE process = GetCurrentProcess();
    DWORD handles = 0;
    if (GetProcessHandleCount(process, &handles))
        sample.handles = handles;
    sample.threadIds = ThisProcessThreadIds();
    sample.threads = static_cast<uint32_t>(sample.threadIds.size());
    sample.gdiObjects = GetGuiResources(process, GR_GDIOBJECTS);
    sample.userObjects = GetGuiResources(process, GR_USEROBJECTS);
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (K32GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
        sample.privateBytes = memory.PrivateUsage;
    sample.videoMemoryKnown = ThisProcessVideoMemory(&sample.videoMemoryBytes);
    return sample;
}

std::string DescribeResidue(const ProcessResidueSample& before, const ProcessResidueSample& after) {
    std::string text = "handles=" + SignedCount(before.handles, after.handles);
    text += " threads=" + SignedCount(before.threads, after.threads);
    text += " gdi=" + SignedCount(before.gdiObjects, after.gdiObjects);
    text += " user=" + SignedCount(before.userObjects, after.userObjects);
    text += " private=" + SignedMiB(before.privateBytes, after.privateBytes);
    text += " vram=";
    text += before.videoMemoryKnown && after.videoMemoryKnown ? SignedMiB(before.videoMemoryBytes, after.videoMemoryBytes)
                                                               : std::string("n/a");
    return text;
}

std::string DescribeNewThreads(const ProcessResidueSample& before, const ProcessResidueSample& after,
                               const std::function<std::string(uint32_t)>& attribute, size_t maxGroups) {
    std::vector<uint32_t> added;
    std::set_difference(after.threadIds.begin(), after.threadIds.end(), before.threadIds.begin(),
                        before.threadIds.end(), std::back_inserter(added));
    if (added.empty())
        return {};
    std::map<std::string, uint32_t> groups;
    for (const uint32_t id : added)
        ++groups[attribute ? attribute(id) : AttributeThread(id)];
    std::vector<std::pair<std::string, uint32_t>> ordered(groups.begin(), groups.end());
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string text;
    for (size_t index = 0; index < ordered.size() && index < maxGroups; ++index) {
        if (!text.empty())
            text += ", ";
        text += ordered[index].first + " x" + std::to_string(ordered[index].second);
    }
    if (ordered.size() > maxGroups)
        text += " (+" + std::to_string(ordered.size() - maxGroups) + " groups)";
    return text;
}

ResidueMeter::ResidueMeter(std::function<ProcessResidueSample()> sample) : sample_(std::move(sample)) {}

void ResidueMeter::Starting() {
    if (live_ == 0) {
        baseline_ = sample_ ? sample_() : ProcessResidueSample{};
        sessionsSinceBaseline_ = 0;
    }
    ++live_;
    ++sessionsSinceBaseline_;
}

void ResidueMeter::Abandoned() {
    if (live_ == 0)
        return;
    --live_;
    if (sessionsSinceBaseline_ > 0)
        --sessionsSinceBaseline_;
}

std::optional<std::string> ResidueMeter::Ended() {
    if (live_ == 0)
        return std::nullopt;
    if (--live_ != 0)
        return std::nullopt;
    lastReport_ = sample_ ? sample_() : ProcessResidueSample{};
    return "sessions=" + std::to_string(sessionsSinceBaseline_) + " " + DescribeResidue(baseline_, lastReport_);
}

std::string ResidueMeter::NewThreadsOfLastReport() const {
    return DescribeNewThreads(baseline_, lastReport_);
}

}  // namespace ce::runtime
