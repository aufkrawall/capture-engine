// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "runtime/children/process_residue.h"

#include <windows.h>

#include <dxgi1_4.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <cstdio>
#include <utility>

namespace ce::runtime {
namespace {

uint32_t CountThisProcessThreads() {
    const DWORD pid = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    uint32_t count = 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID == pid)
            ++count;
    }
    CloseHandle(snapshot);
    return count;
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
    sample.threads = CountThisProcessThreads();
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
    const ProcessResidueSample now = sample_ ? sample_() : ProcessResidueSample{};
    return "sessions=" + std::to_string(sessionsSinceBaseline_) + " " + DescribeResidue(baseline_, now);
}

}  // namespace ce::runtime
