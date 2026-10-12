#include "translated_swapchain_cache.h"

#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <wrl/client.h>

#include "common/logging/log_meter.h"
#include "hook/d3d12/execute_dispatch_registry.h"
#include "hook/hooking/vtable_hook.h"
#include "hook/runtime/hook_common.h"

namespace ce::translated_swapchain::detail {
namespace {
using Release = ULONG(STDMETHODCALLTYPE*)(IDXGISwapChain*);
// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - initialized before any frontend lifetime hook is installed.
ce::dx12::ExecuteDispatchRegistry<Release> releaseTargets;
// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - process-lifetime synchronization.
std::mutex decisionMutex;
struct Decision {
    const void* native;
    bool delegated;
    unsigned releases = 0;
    bool retired = false;
};
std::unordered_map<IDXGISwapChain*, Decision> decisions;
std::atomic<uint64_t> generation{1};

ULONG STDMETHODCALLTYPE DetourRelease(IDXGISwapChain* frontend) {
    void** vtable = *reinterpret_cast<void***>(frontend);
    const auto original = releaseTargets.ResolveInterception(vtable, &DetourRelease, [&] {
        void* saved = nullptr;
        VTableHook::GetOriginal(&vtable[2], reinterpret_cast<void*>(&DetourRelease), &saved);
        return reinterpret_cast<Release>(saved);
    }, [&] { return reinterpret_cast<Release>(std::atomic_ref<void*>(vtable[2]).load(std::memory_order_acquire)); });
    if (!original) {
        static ce::log_meter::ChangeGate missing;
        if (missing.Observe(ce::log_meter::FieldKey(vtable)))
            HookLogImportant("DXGI translation: frontend Release has no exact predecessor (vtable=%p)", vtable);
        return 0;
    }
    if (HookIsShuttingDown()) return original(frontend);
    bool tracked = false;
    {
        std::lock_guard<std::mutex> lock(decisionMutex);
        const auto found = decisions.find(frontend);
        if (found != decisions.end()) {
            ++found->second.releases;
            generation.fetch_add(1, std::memory_order_release);
            tracked = true;
        }
    }
    // No cache/install lock crosses foreign code. The receiver may be destroyed here;
    // only its address and the already-resolved predecessor are used after this call.
    const ULONG remaining = original(frontend);
    if (tracked) {
        std::lock_guard<std::mutex> lock(decisionMutex);
        const auto found = decisions.find(frontend);
        if (found != decisions.end()) {
            auto& decision = found->second;
            decision.retired |= remaining == 0;
            if (--decision.releases == 0 && decision.retired) decisions.erase(found);
            generation.fetch_add(1, std::memory_order_release);
        }
    }
    return remaining;
}

bool CoverFrontendRelease(IDXGISwapChain* frontend) {
    // A forwarded IUnknown is a different object's lifetime. Such a frontend is
    // still detected normally, but cannot borrow that object's cached decision.
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if (FAILED(frontend->QueryInterface(IID_PPV_ARGS(&identity))) || identity.Get() != frontend) return false;
    void** vtable = *reinterpret_cast<void***>(frontend);
    const auto current = reinterpret_cast<Release>(std::atomic_ref<void*>(vtable[2]).load(std::memory_order_acquire));
    const auto installed = releaseTargets.Install(vtable, current, &DetourRelease, [&](Release* original) {
        return VTableHook::Create(&vtable[2], reinterpret_cast<void*>(&DetourRelease),
                                  reinterpret_cast<void**>(original)) == VTableHook::Success;
    });
    using Result = decltype(releaseTargets)::InstallResult;
    // A foreign replacement may bypass CE completely. Preserve it and recheck the API
    // instead of claiming a lifetime boundary that is no longer physically covered.
    return installed.result == Result::kCaptured || installed.result == Result::kKnown;
}
}  // namespace

std::optional<bool> FindDecision(IDXGISwapChain* frontend) {
    void** vtable = *reinterpret_cast<void***>(frontend);
    if (std::atomic_ref<void*>(vtable[2]).load(std::memory_order_acquire) != reinterpret_cast<void*>(&DetourRelease)) {
        std::lock_guard<std::mutex> lock(decisionMutex);
        const auto found = decisions.find(frontend);
        if (found != decisions.end()) {
            if (found->second.releases) found->second.retired = true;
            else decisions.erase(found);
            generation.fetch_add(1, std::memory_order_release);
        }
        return std::nullopt;
    }
    struct Cached {
        IDXGISwapChain* frontend = nullptr;
        const void* native = nullptr;
        uint64_t generation = 0;
        bool delegated = false;
    };
    // Preserve nested frontend hits without allocating, taking a lock or performing device QI.
    static thread_local std::array<Cached, 4> cached;
    static thread_local unsigned next = 0;
    const uint64_t current = generation.load(std::memory_order_acquire);
    for (const auto& entry : cached)
        if (entry.generation == current && entry.frontend == frontend)
            return entry.delegated;
    std::lock_guard<std::mutex> lock(decisionMutex);
    const auto found = decisions.find(frontend);
    if (found == decisions.end() || found->second.releases || found->second.retired)
        return std::nullopt;
    const bool delegated = found->second.delegated;
    cached[next++ % cached.size()] = {frontend, found->second.native, generation.load(std::memory_order_relaxed), delegated};
    return delegated;
}

CachePublication PublishDecision(IDXGISwapChain* frontend, const void* native, bool delegated) {
    if (!CoverFrontendRelease(frontend)) {
        static ce::log_meter::ChangeGate unavailable;
        if (unavailable.Observe(0))
            HookLogImportant("DXGI translation: frontend lifetime is not covered; rechecking API ownership (firstFrontend=%p)",
                             frontend);
        return CachePublication::kUnavailable;
    }
    std::lock_guard<std::mutex> lock(decisionMutex);
    const auto found = decisions.find(frontend);
    if (found != decisions.end()) {
        auto& decision = found->second;
        if (decision.releases || decision.retired) return CachePublication::kUnavailable;
        if (decision.native == native && decision.delegated == delegated) return CachePublication::kKnown;
        decision = {native, delegated};
    } else {
        try {
            decisions.emplace(frontend, Decision{native, delegated});
        } catch (...) {
            return CachePublication::kUnavailable;
        }
    }
    generation.fetch_add(1, std::memory_order_release);
    return CachePublication::kChanged;
}

}  // namespace ce::translated_swapchain::detail
