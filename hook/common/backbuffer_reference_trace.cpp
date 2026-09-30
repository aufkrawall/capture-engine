#include "backbuffer_reference_trace.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdio>
#include <map>
#include <mutex>
#include <string>

#include "../wrappers/inline_hook.h"
#include "dxgi_shared.h"
#include "hook_common.h"
#include "resize_reconcile_hook_policy.h"

namespace {

namespace trace = ce::backbuffer_reference_trace;

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
void** g_ResourceVtable = nullptr;
// The GetBuffer body hook is installed at most once per process (never removed).
bool g_GetBufferInstallAttempted = false;
std::mutex g_InstallMutex;

std::atomic<uint32_t> g_TrackedCount{0};
std::atomic<uintptr_t> g_Tracked[trace::kMaxTrackedBuffers];
trace::SiteTally g_Sites[trace::kMaxTrackedBuffers][trace::kSitesPerBuffer];
std::atomic<uint32_t> g_Overflow{0};
ULONG g_BaselineHeldByOthers[trace::kMaxTrackedBuffers] = {};
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

void Tally(int index, void* returnAddress, bool acquire) {
    if (index < 0 || t_InsideGetBuffer != 0) {
        return;
    }
    if (!trace::Record(g_Sites[index], trace::kSitesPerBuffer, reinterpret_cast<uintptr_t>(returnAddress), acquire)) {
        g_Overflow.fetch_add(1, std::memory_order_relaxed);
    }
}

HRESULT STDMETHODCALLTYPE HookQueryInterface(IUnknown* self, REFIID riid, void** out) {
    const HRESULT hr = g_OriginalQueryInterface.load(std::memory_order_acquire)(self, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        Tally(TrackedIndex(self), __builtin_return_address(0), true);
    }
    return hr;
}

ULONG STDMETHODCALLTYPE HookAddRef(IUnknown* self) {
    Tally(TrackedIndex(self), __builtin_return_address(0), true);
    return g_OriginalAddRef.load(std::memory_order_acquire)(self);
}

ULONG STDMETHODCALLTYPE HookRelease(IUnknown* self) {
    // Counted before forwarding: the object may be gone afterwards.
    Tally(TrackedIndex(self), __builtin_return_address(0), false);
    return g_OriginalRelease.load(std::memory_order_acquire)(self);
}

HRESULT STDMETHODCALLTYPE HookGetBuffer(IDXGISwapChain* self, UINT buffer, REFIID riid, void** out) {
    ++t_InsideGetBuffer;
    const HRESULT hr = g_OriginalGetBuffer.load(std::memory_order_acquire)(self, buffer, riid, out);
    --t_InsideGetBuffer;
    if (SUCCEEDED(hr) && out && *out) {
        Tally(TrackedIndex(*out), __builtin_return_address(0), true);
    }
    return hr;
}

void PublishGetBufferTrampoline(void* trampoline, void*) {
    g_OriginalGetBuffer.store(reinterpret_cast<GetBufferFn>(trampoline), std::memory_order_release);
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
            getBuffer = InlineHook::InstallDeepHookPublished(getBufferTarget, reinterpret_cast<void*>(&HookGetBuffer),
                                                             PublishGetBufferTrampoline, nullptr,
                                                             ce::resize_reconcile_hook::kAssumedForeignEntryPatchSize) !=
                        nullptr;
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

void BackBufferReferenceTrace_Track(IDXGISwapChain* swapChain, UINT bufferCount, const char* source) {
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

    // Unpublish first so no hook counts into a table that is being cleared.
    g_TrackedCount.store(0, std::memory_order_release);
    for (size_t i = 0; i < trace::kMaxTrackedBuffers; ++i) {
        trace::Clear(g_Sites[i], trace::kSitesPerBuffer);
        g_Tracked[i].store(i < obtained ? reinterpret_cast<uintptr_t>(buffers[i]) : 0, std::memory_order_relaxed);
    }
    g_Overflow.store(0, std::memory_order_relaxed);
    snprintf(g_TrackedSource, sizeof(g_TrackedSource), "%s", source ? source : "resize");

    InstallHooks(*reinterpret_cast<void***>(buffers[0]), *reinterpret_cast<void***>(swapChain));

    char baseline[96] = {};
    size_t used = 0;
    ++t_InsideGetBuffer;
    for (UINT i = 0; i < obtained; ++i) {
        buffers[i]->AddRef();
        g_BaselineHeldByOthers[i] = buffers[i]->Release() - 1;  // minus this function's own GetBuffer reference
        const int written = snprintf(baseline + used, sizeof(baseline) - used, i == 0 ? "%lu" : ",%lu",
                                     static_cast<unsigned long>(g_BaselineHeldByOthers[i]));
        if (written > 0 && used + static_cast<size_t>(written) < sizeof(baseline)) {
            used += static_cast<size_t>(written);
        }
        buffers[i]->Release();
    }
    --t_InsideGetBuffer;
    g_TrackedCount.store(obtained, std::memory_order_release);
    HookLogImportant("BackBufferRefTrace: tracking %u buffer(s) of sc=%p after %s (heldByOthers at start=[%s])",
                     obtained, swapChain, g_TrackedSource, baseline);
}

void BackBufferReferenceTrace_Log(IDXGISwapChain* swapChain, const char* source) {
    const uint32_t count = g_TrackedCount.load(std::memory_order_acquire);
    if (count == 0) {
        HookLogImportant("BackBufferRefTrace: %s on sc=%p - no buffers tracked", source ? source : "?", swapChain);
        return;
    }
    HookLogImportant(
        "BackBufferRefTrace: %s on sc=%p - references taken (acq) and returned (rel) per call site since %s; "
        "overflow=%u. A module whose acq exceeds its rel holds the difference",
        source ? source : "?", swapChain, g_TrackedSource, g_Overflow.load(std::memory_order_relaxed));
    std::map<std::string, long> netByModule;
    for (uint32_t b = 0; b < count; ++b) {
        for (size_t s = 0; s < trace::kSitesPerBuffer; ++s) {
            const uintptr_t site = g_Sites[b][s].site.load(std::memory_order_acquire);
            if (site == 0) {
                break;
            }
            const int32_t acquired = g_Sites[b][s].acquired.load(std::memory_order_relaxed);
            const int32_t released = g_Sites[b][s].released.load(std::memory_order_relaxed);
            const std::string where = DescribeSite(site);
            netByModule[ModuleOf(where)] += static_cast<long>(acquired) - static_cast<long>(released);
            HookLogImportant("BackBufferRefTrace: bb%u %s acq=%d rel=%d", b, where.c_str(), acquired, released);
        }
    }
    std::string summary;
    for (const auto& [module, net] : netByModule) {
        char entry[112];
        snprintf(entry, sizeof(entry), "%s%s=%+ld", summary.empty() ? "" : " ", module.c_str(), net);
        summary += entry;
    }
    HookLogImportant("BackBufferRefTrace: net references by module over %u buffer(s): %s", count, summary.c_str());
}

void BackBufferReferenceTrace_Uninstall() {
    std::lock_guard<std::mutex> lock(g_InstallMutex);
    g_TrackedCount.store(0, std::memory_order_release);
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
