#include "native_swapchain_release_observer.h"

#include <atomic>
#include <cstring>
#include <mutex>

#include "dxgi_shared.h"
#include "hook/hooking/inline_hook.h"
#include "hook/metrics/perf_logger.h"
#include "hook/overlay/overlay_compat.h"
#include "hook/runtime/hook_common.h"
#include "resize_reconcile_hook_policy.h"

namespace ce::native_swapchain_release_observer {
namespace {

using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(IUnknown*);

constexpr size_t kReleaseSlot = 2;

std::atomic<ReleaseFn> g_OriginalRelease{nullptr};
std::atomic<IDXGISwapChain*> g_Watched[kSubscriberCount] = {};
std::atomic<ReleaseHandler> g_Handlers[kSubscriberCount] = {};
std::atomic<bool> g_Installed{false};
bool g_InstallAttempted = false;
std::mutex g_InstallMutex;

ULONG STDMETHODCALLTYPE HookNativeSwapchainRelease(IUnknown* self) {
    const auto original = g_OriginalRelease.load(std::memory_order_acquire);
    auto* const chain = reinterpret_cast<IDXGISwapChain*>(self);
    uint32_t subscribers = 0;
    for (size_t i = 0; i < kSubscriberCount; ++i) {
        if (g_Watched[i].load(std::memory_order_relaxed) == chain) {
            subscribers |= 1u << i;
        }
    }
    if (subscribers == 0 || IsProcessTerminating()) {
        return original(self);
    }

    // This is the real Release of an already owned reference: the report uses the chain's address as an identity and
    // never dereferences it.
    const void* const caller = __builtin_return_address(0);
    static const void* ownImage = [] {
        void* image = nullptr;
        RtlPcToFileHeader(reinterpret_cast<void*>(&HookNativeSwapchainRelease), &image);
        return image;
    }();
    HMODULE callerModule = nullptr;
    char path[MAX_PATH] = {};
    if (DXGIShared::IsAddressInsideSystemDXGI(caller) ||
        !ce::overlay_compat::module_address_cache::Lookup(caller, &callerModule, path, sizeof(path), nullptr) ||
        callerModule == ownImage) {
        return original(self);
    }

    ReleaseReport report;
    report.chain = chain;
    report.caller = caller;
    report.callerModule = callerModule;
    const char* const name = std::strrchr(path, '\\');
    report.callerImage = name ? name + 1 : path;
    report.callerOffset = reinterpret_cast<uintptr_t>(caller) - reinterpret_cast<uintptr_t>(callerModule);
    report.threadId = GetCurrentThreadId();
    report.witnessBefore = ce::swapchain_lifetime_witness::Describe(chain);
    report.enterQpcUs = PerfLogger::GetQpcUs();
    report.remaining = original(self);
    report.returnQpcUs = PerfLogger::GetQpcUs();
    for (size_t i = 0; i < kSubscriberCount; ++i) {
        if ((subscribers & (1u << i)) == 0) {
            continue;
        }
        if (const auto handler = g_Handlers[i].load(std::memory_order_acquire)) {
            handler(report);
        }
    }
    return report.remaining;
}

void PublishTrampoline(void* trampoline, void*) {
    g_OriginalRelease.store(reinterpret_cast<ReleaseFn>(trampoline), std::memory_order_release);
}

}  // namespace

void SetHandler(Subscriber subscriber, ReleaseHandler handler) {
    g_Handlers[static_cast<size_t>(subscriber)].store(handler, std::memory_order_release);
}

void Watch(Subscriber subscriber, IDXGISwapChain* chain) {
    g_Watched[static_cast<size_t>(subscriber)].store(chain, std::memory_order_relaxed);
}

bool Install(IDXGISwapChain* liveChain) {
    if (g_Installed.load(std::memory_order_acquire)) {
        return true;
    }
    if (!liveChain) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_InstallMutex);
    if (g_InstallAttempted) {
        return g_Installed.load(std::memory_order_acquire);
    }
    void** const vtable = *reinterpret_cast<void***>(liveChain);
    void* const target = *static_cast<void* volatile*>(&vtable[kReleaseSlot]);
    if (!DXGIShared::IsAddressInsideSystemDXGI(target)) {
        static std::atomic<uint32_t> s_foreignLogs{0};
        if (s_foreignLogs.fetch_add(1, std::memory_order_relaxed) < 4) {
            HookLogImportant("[SL LIFETIME] native Release observation target=%p hooked=0 (not DXGI's Release)", target);
        }
        return false;
    }
    g_InstallAttempted = true;
    const bool hooked =
        InlineHook::InstallDeepHookPublishedWithRetry("BackBufferRefTrace", "native Release", target,
                                                      reinterpret_cast<void*>(&HookNativeSwapchainRelease),
                                                      PublishTrampoline, nullptr,
                                                      ce::resize_reconcile_hook::kAssumedForeignEntryPatchSize) !=
        nullptr;
    g_Installed.store(hooked, std::memory_order_release);
    HookLogImportant("[SL LIFETIME] native Release observation target=%p hooked=%d", target, hooked ? 1 : 0);
    return hooked;
}

bool IsInstalled() {
    return g_Installed.load(std::memory_order_acquire);
}

}  // namespace ce::native_swapchain_release_observer
