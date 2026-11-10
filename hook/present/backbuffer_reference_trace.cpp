#include "backbuffer_reference_trace.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>

#include "common/logging/log_meter.h"
#include "hook/hooking/inline_hook.h"
#include "dxgi_shared.h"
#include "hook/metrics/perf_logger.h"
#include "hook/overlay/overlay_compat.h"
#include "hook/runtime/hook_clock.h"
#include "hook/runtime/hook_common.h"
#include "native_swapchain_release_observer.h"
#include "resize_reconcile_hook_policy.h"
#include "swapchain_lifetime_witness.h"

namespace {

namespace trace = ce::backbuffer_reference_trace;
namespace native_release = ce::native_swapchain_release_observer;

using QueryInterfaceFn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, REFIID, void**);
using AddRefFn = ULONG(STDMETHODCALLTYPE*)(IUnknown*);
using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(IUnknown*);
using GetBufferFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, REFIID, void**);

constexpr size_t kQueryInterfaceSlot = 0;
constexpr size_t kAddRefSlot = 1;
constexpr size_t kReleaseSlot = 2;
constexpr size_t kGetBufferSlot = 9;

std::atomic<QueryInterfaceFn> g_OriginalQueryInterface{nullptr};
std::atomic<AddRefFn> g_OriginalAddRef{nullptr};
std::atomic<ReleaseFn> g_OriginalRelease{nullptr};
std::atomic<GetBufferFn> g_OriginalGetBuffer{nullptr};
std::atomic<uint32_t> g_NativeReleaseCount{0};
void** g_ResourceVtable = nullptr;
// The GetBuffer body hook is installed at most once per process (never removed).
bool g_GetBufferInstallAttempted = false;
std::mutex g_InstallMutex;

constexpr size_t kGetBufferStackLimit = 4;
constexpr size_t kGetBufferStackDepth = 12;
struct GetBufferSite {
    std::atomic<uintptr_t> address{0};
    void* frames[kGetBufferStackDepth] = {};
    USHORT count = 0;
};

// Registration and snapshots serialize off the COM hot path. Only a tracked
// buffer takes the shared lock, and it never waits on a resize or a report.
std::shared_mutex g_TraceMutex;
std::atomic<uint64_t> g_TraceGeneration{0};
std::atomic<uint32_t> g_TrackedCount{0};
std::atomic<uintptr_t> g_Tracked[trace::kMaxTrackedBuffers];
trace::SiteTally g_Sites[trace::kMaxTrackedBuffers][trace::kSitesPerBuffer];
GetBufferSite g_GetBufferSites[kGetBufferStackLimit];
std::atomic<uint32_t> g_Overflow{0};
std::atomic<uint32_t> g_ContentionDrops{0};
IDXGISwapChain* g_TrackedChain = nullptr;
HWND g_TrackedWindow = nullptr;
uint64_t g_WitnessGeneration = 0;
uint64_t g_RegisteredAtMs = 0;
char g_TrackedSource[64] = {};

// DXGI's GetBuffer AddRefs/QIs the buffer internally; the reference belongs to
// GetBuffer's caller, so the nested calls are not counted separately.
thread_local int t_InsideGetBuffer = 0;

int TrackedIndex(const void* object) {
    const uint32_t count = g_TrackedCount.load(std::memory_order_acquire);
    const uintptr_t value = reinterpret_cast<uintptr_t>(object);
    for (uint32_t i = 0; i < count; ++i) {
        if (g_Tracked[i].load(std::memory_order_relaxed) == value) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void RecordGetBufferStack(uintptr_t site) {
    for (auto& entry : g_GetBufferSites) {
        uintptr_t address = entry.address.load(std::memory_order_relaxed);
        if (address == 0 && entry.address.compare_exchange_strong(address, site, std::memory_order_relaxed)) {
            entry.count = CaptureStackBackTrace(1, kGetBufferStackDepth, entry.frames, nullptr);
            return;
        }
        if (address == site) {
            return;
        }
    }
}

void Tally(const void* buffer, void* returnAddress, uint64_t generation, bool acquire, bool fromGetBuffer = false) {
    if (t_InsideGetBuffer != 0 || TrackedIndex(buffer) < 0) {
        return;
    }
    std::shared_lock lock(g_TraceMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        g_ContentionDrops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const int index = TrackedIndex(buffer);
    if (index < 0 || g_TraceGeneration.load(std::memory_order_relaxed) != generation) {
        return;
    }
    const auto site = reinterpret_cast<uintptr_t>(returnAddress);
    if (!trace::Record(g_Sites[index], trace::kSitesPerBuffer, site, acquire)) {
        g_Overflow.fetch_add(1, std::memory_order_relaxed);
    }
    if (fromGetBuffer && site != 0) {
        RecordGetBufferStack(site);
    }
}

HRESULT STDMETHODCALLTYPE HookQueryInterface(IUnknown* self, REFIID riid, void** out) {
    const uint64_t generation = g_TraceGeneration.load(std::memory_order_acquire);
    const HRESULT hr = g_OriginalQueryInterface.load(std::memory_order_acquire)(self, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        Tally(self, __builtin_return_address(0), generation, true);
    }
    return hr;
}

ULONG STDMETHODCALLTYPE HookAddRef(IUnknown* self) {
    Tally(self, __builtin_return_address(0), g_TraceGeneration.load(std::memory_order_acquire), true);
    return g_OriginalAddRef.load(std::memory_order_acquire)(self);
}

ULONG STDMETHODCALLTYPE HookRelease(IUnknown* self) {
    // Counted before forwarding: the object may be gone afterwards.
    Tally(self, __builtin_return_address(0), g_TraceGeneration.load(std::memory_order_acquire), false);
    return g_OriginalRelease.load(std::memory_order_acquire)(self);
}

HRESULT STDMETHODCALLTYPE HookGetBuffer(IDXGISwapChain* self, UINT buffer, REFIID riid, void** out) {
    const uint64_t generation = g_TraceGeneration.load(std::memory_order_acquire);
    ++t_InsideGetBuffer;
    const HRESULT hr = g_OriginalGetBuffer.load(std::memory_order_acquire)(self, buffer, riid, out);
    --t_InsideGetBuffer;
    if (SUCCEEDED(hr) && out && *out) {
        Tally(*out, __builtin_return_address(0), generation, true, true);
    }
    return hr;
}

void PublishGetBufferTrampoline(void* trampoline, void*) {
    g_OriginalGetBuffer.store(reinterpret_cast<GetBufferFn>(trampoline), std::memory_order_release);
}

// The observer's subscriber: the chain's references as the trace saw them at its last resize.
void OnNativeSwapchainRelease(const native_release::ReleaseReport& report) {
    const auto& witness = report.witnessBefore;
    bool watched = false;
    {
        std::shared_lock lock(g_TraceMutex, std::try_to_lock);
        watched = lock.owns_lock() && g_TrackedChain == report.chain && witness.generation == g_WitnessGeneration &&
                  witness.state == ce::swapchain_lifetime_witness::ChainState::kAlive;
    }
    if (!watched) {
        return;
    }
    const auto call = g_NativeReleaseCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (report.remaining <= 1 || ce::log_meter::ShouldLogCadence(call, 12, 128)) {
        HookLogImportant("[SL LIFETIME] native Release sc=%p lifetime=%llu caller=%s+0x%llX returnedRefs=%lu "
                         "enterQpcUs=%lld returnQpcUs=%lld call=%u",
                         report.chain, static_cast<unsigned long long>(witness.generation), report.callerImage,
                         static_cast<unsigned long long>(report.callerOffset), report.remaining,
                         static_cast<long long>(report.enterQpcUs), static_cast<long long>(report.returnQpcUs), call);
    }
}

template <typename Fn>
bool PatchSlot(void** vtable, size_t slot, void* detour, std::atomic<Fn>& original) {
    void* current = *static_cast<void* volatile*>(&vtable[slot]);
    if (!current || current == detour) {
        return current == detour;
    }
    // The page stays writable afterwards: restoring it could race another CE
    // patch of the same page between its unprotect and its write.
    DWORD oldProtect = 0;
    if (!VirtualProtect(static_cast<void*>(&vtable[slot]), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        return false;
    }
    original.store(reinterpret_cast<Fn>(current), std::memory_order_release);
    return InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(&vtable[slot]), detour, current) ==
           current;
}

void RestoreSlot(void** vtable, size_t slot, void* detour, void* original) {
    // Another CE claim on the same page may have made it read-only again; a
    // locked write to it would fault (crash 20260811_192706).
    DWORD oldProtect = 0;
    if (vtable && original && vtable[slot] == detour &&
        VirtualProtect(static_cast<void*>(&vtable[slot]), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(&vtable[slot]), original, detour);
    }
}

void InstallHooks(void** resourceVtable, void** swapChainVtable) {
    std::lock_guard<std::mutex> lock(g_InstallMutex);
    if (!g_ResourceVtable && resourceVtable) {
        const bool qi = PatchSlot(resourceVtable, kQueryInterfaceSlot, reinterpret_cast<void*>(&HookQueryInterface),
                                  g_OriginalQueryInterface);
        const bool addRef =
            PatchSlot(resourceVtable, kAddRefSlot, reinterpret_cast<void*>(&HookAddRef), g_OriginalAddRef);
        const bool release =
            PatchSlot(resourceVtable, kReleaseSlot, reinterpret_cast<void*>(&HookRelease), g_OriginalRelease);
        g_ResourceVtable = resourceVtable;
        HookLogImportant("BackBufferRefTrace: resource vtable %p hooked (QueryInterface=%d AddRef=%d Release=%d)",
                         resourceVtable, qi ? 1 : 0, addRef ? 1 : 0, release ? 1 : 0);
    } else if (resourceVtable && resourceVtable != g_ResourceVtable) {
        HookLogImportant("BackBufferRefTrace: buffers use a second resource vtable %p (hooked %p) - not counted",
                         resourceVtable, g_ResourceVtable);
    }
    if (!g_GetBufferInstallAttempted && swapChainVtable) {
        g_GetBufferInstallAttempted = true;
        // Neither the swapchain's vtable slot nor the entry of DXGI's GetBuffer:
        // the Steam overlay patches the entries of the functions a swapchain's
        // vtable points to, and skips one that already jumps into another module
        // (resize_reconcile_hook_policy.h). A swapchain Steam could not hook fully
        // is exactly the refused resize this trace exists to explain, so GetBuffer
        // is hooked below its entry, or not at all.
        void* const getBufferTarget = *static_cast<void* volatile*>(&swapChainVtable[kGetBufferSlot]);
        bool getBuffer = false;
        if (DXGIShared::IsAddressInsideSystemDXGI(getBufferTarget)) {
            getBuffer = InlineHook::InstallDeepHookPublishedWithRetry(
                            "BackBufferRefTrace", "GetBuffer", getBufferTarget, reinterpret_cast<void*>(&HookGetBuffer),
                            PublishGetBufferTrampoline, nullptr,
                            ce::resize_reconcile_hook::kAssumedForeignEntryPatchSize) != nullptr;
        }
        HookLogImportant(
            "BackBufferRefTrace: DXGI GetBuffer %p (swapchain vtable %p; slot and entry left untouched) body "
            "hooked=%d%s",
            getBufferTarget, swapChainVtable, getBuffer ? 1 : 0,
            getBuffer ? "" : " - references handed out by GetBuffer are attributed to DXGI");
    }
}

std::string DescribeSite(uintptr_t site) {
    HMODULE module = nullptr;
    char text[160];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(site), &module) &&
        module) {
        wchar_t path[MAX_PATH] = {};
        char name[80] = "?";
        if (GetModuleFileNameW(module, path, MAX_PATH)) {
            const wchar_t* base = wcsrchr(path, L'\\');
            WideCharToMultiByte(CP_UTF8, 0, base ? base + 1 : path, -1, name, static_cast<int>(sizeof(name)), nullptr,
                                nullptr);
            name[sizeof(name) - 1] = '\0';
        }
        snprintf(text, sizeof(text), "%s+0x%llX", name,
                 static_cast<unsigned long long>(site - reinterpret_cast<uintptr_t>(module)));
    } else {
        snprintf(text, sizeof(text), "%p", reinterpret_cast<void*>(site));
    }
    return text;
}

std::string ModuleOf(const std::string& site) {
    const size_t plus = site.find('+');
    return plus == std::string::npos ? std::string("unknown") : site.substr(0, plus);
}

}  // namespace

void BackBufferReferenceTrace_Track(IDXGISwapChain* swapChain, HWND window, UINT bufferCount, const char* source) {
    if (!swapChain || bufferCount == 0) {
        return;
    }
    const UINT count = bufferCount < trace::kMaxTrackedBuffers ? bufferCount : static_cast<UINT>(trace::kMaxTrackedBuffers);
    ID3D12Resource* buffers[trace::kMaxTrackedBuffers] = {};
    UINT obtained = 0;
    ++t_InsideGetBuffer;
    for (; obtained < count; ++obtained) {
        if (FAILED(swapChain->GetBuffer(obtained, IID_PPV_ARGS(&buffers[obtained]))) || !buffers[obtained]) {
            buffers[obtained] = nullptr;
            break;
        }
    }
    --t_InsideGetBuffer;
    if (obtained == 0) {
        return;  // not a D3D12 chain
    }

    const bool traceNativeRelease =
        HookTraceLoggingEnabled() && ce::overlay_compat::IsStreamlineInterposerModuleLoaded();
    InstallHooks(*reinterpret_cast<void***>(buffers[0]), *reinterpret_cast<void***>(swapChain));
    if (traceNativeRelease) {
        native_release::SetHandler(native_release::Subscriber::kReferenceTrace, &OnNativeSwapchainRelease);
        native_release::Install(swapChain);
    }

    const auto witness = ce::swapchain_lifetime_witness::Describe(swapChain);
    const char* const resizeSource = source ? source : "resize";
    uint64_t generation = 0;
    {
        std::unique_lock lock(g_TraceMutex);
        g_TrackedCount.store(0, std::memory_order_release);
        for (size_t i = 0; i < trace::kMaxTrackedBuffers; ++i) {
            trace::Clear(g_Sites[i], trace::kSitesPerBuffer);
            g_Tracked[i].store(i < obtained ? reinterpret_cast<uintptr_t>(buffers[i]) : 0, std::memory_order_relaxed);
        }
        for (auto& site : g_GetBufferSites) {
            site.count = 0;
            site.address.store(0, std::memory_order_relaxed);
        }
        g_TrackedChain = swapChain;
        g_NativeReleaseCount.store(0, std::memory_order_relaxed);
        native_release::Watch(native_release::Subscriber::kReferenceTrace, traceNativeRelease ? swapChain : nullptr);
        g_TrackedWindow = window;
        g_WitnessGeneration = witness.generation;
        g_RegisteredAtMs = ce::hook_clock::TickCount64();
        g_Overflow.store(0, std::memory_order_relaxed);
        g_ContentionDrops.store(0, std::memory_order_relaxed);
        snprintf(g_TrackedSource, sizeof(g_TrackedSource), "%s", resizeSource);
        g_TrackedCount.store(obtained, std::memory_order_release);
        generation = g_TraceGeneration.fetch_add(1, std::memory_order_release) + 1;
    }

    ++t_InsideGetBuffer;
    for (UINT i = 0; i < obtained; ++i) {
        buffers[i]->Release();
    }
    --t_InsideGetBuffer;
    HookLogImportant("BackBufferRefTrace: tracking %u/%u buffer(s) sc=%p hwnd=%p resize=%llu lifetime=%llu after %s",
                     obtained, bufferCount, swapChain, window, static_cast<unsigned long long>(generation),
                     static_cast<unsigned long long>(witness.generation), resizeSource);
}

namespace {

struct TraceSnapshot {
    struct RefSite {
        uintptr_t address;
        int32_t acquired;
        int32_t released;
        uint32_t buffer;
    };
    struct Stack {
        uintptr_t site = 0;
        std::array<void*, kGetBufferStackDepth> frames{};
        USHORT count = 0;
    };
    std::array<RefSite, trace::kMaxTrackedBuffers * trace::kSitesPerBuffer> sites;
    std::array<Stack, kGetBufferStackLimit> getBufferStacks;
    size_t siteCount = 0;
    IDXGISwapChain* chain = nullptr;
    HWND window = nullptr;
    uint32_t bufferCount = 0;
    uint32_t overflow = 0;
    uint32_t contentionDrops = 0;
    uint64_t generation = 0;
    uint64_t witnessGeneration = 0;
    uint64_t registeredAtMs = 0;
    char resizeSource[64] = {};
};

bool SnapshotTrace(IDXGISwapChain* swapChain, HWND window, bool detailed, TraceSnapshot& snapshot) {
    std::unique_lock lock(g_TraceMutex);
    const uint32_t count = g_TrackedCount.load(std::memory_order_relaxed);
    if (count == 0 || (swapChain ? g_TrackedChain != swapChain : !window || g_TrackedWindow != window)) {
        return false;
    }
    snapshot.chain = g_TrackedChain;
    snapshot.window = g_TrackedWindow;
    snapshot.bufferCount = count;
    snapshot.overflow = g_Overflow.load(std::memory_order_relaxed);
    snapshot.contentionDrops = g_ContentionDrops.load(std::memory_order_relaxed);
    snapshot.generation = g_TraceGeneration.load(std::memory_order_relaxed);
    snapshot.witnessGeneration = g_WitnessGeneration;
    snapshot.registeredAtMs = g_RegisteredAtMs;
    snprintf(snapshot.resizeSource, sizeof(snapshot.resizeSource), "%s", g_TrackedSource);
    for (uint32_t b = 0; b < count; ++b) {
        for (const auto& entry : g_Sites[b]) {
            const uintptr_t address = entry.site.load(std::memory_order_relaxed);
            if (address == 0) {
                break;
            }
            snapshot.sites[snapshot.siteCount++] = {address, entry.acquired.load(std::memory_order_relaxed),
                                                    entry.released.load(std::memory_order_relaxed), b};
        }
    }
    if (detailed) {
        for (size_t i = 0; i < kGetBufferStackLimit; ++i) {
            const auto& entry = g_GetBufferSites[i];
            auto& stack = snapshot.getBufferStacks[i];
            stack.site = entry.address.load(std::memory_order_relaxed);
            stack.count = entry.count;
            std::copy_n(entry.frames, entry.count, stack.frames.begin());
        }
    }
    return true;
}

bool LogTrace(IDXGISwapChain* swapChain, HWND window, const char* source, bool detailed) {
    TraceSnapshot snapshot;
    const char* const stage = source ? source : "?";
    if (!SnapshotTrace(swapChain, window, detailed, snapshot)) {
        HookLogImportant("BackBufferRefTrace: %s sc=%p hwnd=%p has no matching resize trace", stage, swapChain, window);
        return false;
    }
    const auto witness = ce::swapchain_lifetime_witness::Describe(snapshot.chain);
    if (witness.state == ce::swapchain_lifetime_witness::ChainState::kDestroyed ||
        (snapshot.witnessGeneration != 0 && snapshot.witnessGeneration != witness.generation)) {
        HookLogImportant("BackBufferRefTrace: %s sc=%p hwnd=%p resize=%llu trace is from a retired lifetime", stage,
                         snapshot.chain, snapshot.window, static_cast<unsigned long long>(snapshot.generation));
        return false;
    }
    std::map<std::string, long> netByModule;
    std::map<uintptr_t, std::string> siteNames;
    constexpr size_t kDetailedSiteLines = 24;
    for (size_t i = 0; i < snapshot.siteCount; ++i) {
        const auto& site = snapshot.sites[i];
        auto [name, inserted] = siteNames.try_emplace(site.address);
        if (inserted) {
            name->second = DescribeSite(site.address);
        }
        netByModule[ModuleOf(name->second)] += static_cast<long>(site.acquired) - static_cast<long>(site.released);
        if (detailed && i < kDetailedSiteLines) {
            HookLogImportant("BackBufferRefTrace: bb%u %s acq=%d rel=%d", site.buffer, name->second.c_str(),
                             site.acquired, site.released);
        }
    }
    for (const auto& stack : snapshot.getBufferStacks) {
        if (!stack.count) {
            continue;
        }
        std::string callers;
        for (size_t i = 0; i < stack.count; ++i) {
            callers += i == 0 ? "" : " <- ";
            callers += DescribeSite(reinterpret_cast<uintptr_t>(stack.frames[i]));
        }
        HookLogImportant("BackBufferRefTrace: GetBuffer site=%s stack=%.640s", DescribeSite(stack.site).c_str(),
                         callers.c_str());
    }
    std::string summary;
    for (const auto& [module, net] : netByModule) {
        char entry[112];
        snprintf(entry, sizeof(entry), "%s%s=%+ld", summary.empty() ? "" : " ", module.c_str(), net);
        summary += entry;
    }
    const std::string holders = trace::DescribeForeignHolders(netByModule);
    HookLogImportant("BackBufferRefTrace: %s sc=%p hwnd=%p resize=%llu lifetime=%llu ageMs=%llu buffers=%u "
                     "since=%s sites=%zu overflow=%u contentionDrops=%u observedNet=[%.256s] foreignNet=[%.256s]",
                     stage, snapshot.chain, snapshot.window, static_cast<unsigned long long>(snapshot.generation),
                     static_cast<unsigned long long>(snapshot.witnessGeneration),
                     static_cast<unsigned long long>(ce::hook_clock::TickCount64() - snapshot.registeredAtMs),
                     snapshot.bufferCount, snapshot.resizeSource, snapshot.siteCount, snapshot.overflow,
                     snapshot.contentionDrops, summary.c_str(), holders.c_str());
    return true;
}

}  // namespace

bool BackBufferReferenceTrace_Log(IDXGISwapChain* swapChain, const char* source, bool detailed) {
    return LogTrace(swapChain, nullptr, source, detailed);
}

bool BackBufferReferenceTrace_LogForWindow(HWND window, const char* source, bool detailed) {
    return LogTrace(nullptr, window, source, detailed);
}

std::string ce::backbuffer_reference_trace::DescribeForeignHolders(const std::map<std::string, long>& netByModule) {
    std::string holders;
    for (const auto& [module, net] : netByModule) {
        if (net <= 0 || module == "unknown" || ce::swapchain_lifetime_witness::IsReleaseMachineryModule(module.c_str())) {
            continue;
        }
        char entry[112];
        snprintf(entry, sizeof(entry), "%s%s=%+ld", holders.empty() ? "" : " ", module.c_str(), net);
        holders += entry;
    }
    return holders;
}

void BackBufferReferenceTrace_Uninstall() {
    std::lock_guard<std::mutex> lock(g_InstallMutex);
    {
        std::unique_lock traceLock(g_TraceMutex);
        g_TrackedCount.store(0, std::memory_order_release);
        g_TraceGeneration.fetch_add(1, std::memory_order_release);
    }
    RestoreSlot(g_ResourceVtable, kQueryInterfaceSlot, reinterpret_cast<void*>(&HookQueryInterface),
                reinterpret_cast<void*>(g_OriginalQueryInterface.load()));
    RestoreSlot(g_ResourceVtable, kAddRefSlot, reinterpret_cast<void*>(&HookAddRef),
                reinterpret_cast<void*>(g_OriginalAddRef.load()));
    RestoreSlot(g_ResourceVtable, kReleaseSlot, reinterpret_cast<void*>(&HookRelease),
                reinterpret_cast<void*>(g_OriginalRelease.load()));
    // DXGI's GetBuffer body hook stays: the hook DLL is pinned and the detour
    // only forwards once nothing is tracked. The originals stay published: a call
    // already inside a hook still forwards.
    g_ResourceVtable = nullptr;
}
