#include "xefg_hook_internal.h"

#include <atomic>
#include <mutex>

#include "common/logging/log_meter.h"
#include "hook/hooking/inline_hook.h"
#include "hook/runtime/hook_common.h"

// XeLL (libxell.dll), the latency reduction XeFG requires: the game's sleep mode (a minimum frame interval is a
// frame-rate cap of its own) and its per-frame latency markers. Observed at the exports like XeFG's; every call is
// forwarded unchanged.

namespace XeFGHook {
namespace {

using namespace ce::xell_api;

std::atomic<void*> g_setSleepMode{nullptr};
std::atomic<void*> g_sleep{nullptr};
std::atomic<void*> g_addMarkerData{nullptr};
std::atomic<uint32_t> g_minimumIntervalUs{0};
std::atomic<bool> g_lowLatencyMode{false};
std::atomic<uint64_t> g_sleeps{0};
std::atomic<uint64_t> g_markers{0};

template <typename Fn>
Fn Original(const std::atomic<void*>& slot) {
    return reinterpret_cast<Fn>(slot.load(std::memory_order_acquire));
}

Result Detour_SetSleepMode(Handle context, const SleepParams* params) {
    const auto original = Original<PFN_SetSleepMode>(g_setSleepMode);
    if (!original) {
        return -1;
    }
    const Result result = original(context, params);
    if (result == 0 && params) {
        g_minimumIntervalUs.store(params->minimumIntervalUs, std::memory_order_release);
        g_lowLatencyMode.store(params->lowLatencyMode != 0, std::memory_order_release);
    }
    static ce::log_meter::ChangeGate s_gate;
    const auto verdict = s_gate.Observe(ce::log_meter::FieldKey(
        context, params ? params->minimumIntervalUs : 0u, params ? static_cast<uint32_t>(params->lowLatencyMode) : 0u,
        result));
    if (verdict) {
        HookLogImportant("XeLL: SetSleepMode context=%p minimumIntervalUs=%u (cap %.1f fps) lowLatency=%u result=%d%s",
                         context, params ? params->minimumIntervalUs : 0u,
                         params && params->minimumIntervalUs ? 1000000.0 / params->minimumIntervalUs : 0.0,
                         params ? static_cast<unsigned>(params->lowLatencyMode) : 0u, result,
                         ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
    return result;
}

Result Detour_Sleep(Handle context, uint32_t frameId) {
    const auto original = Original<PFN_Sleep>(g_sleep);
    if (!original) {
        return -1;
    }
    const uint64_t sleeps = g_sleeps.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sleeps == 1) {
        HookLogImportant("XeLL: first Sleep context=%p frameId=%u tid=0x%04lX", context, frameId,
                         static_cast<unsigned long>(GetCurrentThreadId()));
    }
    return original(context, frameId);
}

Result Detour_AddMarkerData(Handle context, uint32_t frameId, int32_t marker) {
    const auto original = Original<PFN_AddMarkerData>(g_addMarkerData);
    if (!original) {
        return -1;
    }
    const uint64_t markers = g_markers.fetch_add(1, std::memory_order_relaxed) + 1;
    if (markers == 1) {
        HookLogImportant("XeLL: first latency marker context=%p frameId=%u marker=%d tid=0x%04lX", context, frameId,
                         marker, static_cast<unsigned long>(GetCurrentThreadId()));
    }
    return original(context, frameId, marker);
}

void PublishTrampoline(void* trampoline, void* context) {
    static_cast<std::atomic<void*>*>(context)->store(trampoline, std::memory_order_release);
}

std::mutex g_installMutex;
HMODULE g_hookedModule = nullptr;

}  // namespace

void InstallXeLLExports(HMODULE module, const char* modulePath) {
    std::lock_guard<std::mutex> lock(g_installMutex);
    if (!module || g_hookedModule == module) {
        return;
    }
    struct Export {
        const char* name;
        void* detour;
        std::atomic<void*>* original;
    } const exports[] = {
        {"xellSetSleepMode", reinterpret_cast<void*>(&Detour_SetSleepMode), &g_setSleepMode},
        {"xellSleep", reinterpret_cast<void*>(&Detour_Sleep), &g_sleep},
        {"xellAddMarkerData", reinterpret_cast<void*>(&Detour_AddMarkerData), &g_addMarkerData},
    };
    constexpr size_t kCount = sizeof(exports) / sizeof(exports[0]);
    InlineHook::PublishedHookSpec specs[kCount] = {};
    void* trampolines[kCount] = {};
    const char* names[kCount] = {};
    size_t queued = 0;
    for (const Export& entry : exports) {
        void* target = reinterpret_cast<void*>(GetProcAddress(module, entry.name));
        if (!target) {
            HookLogImportant("XeLL: %s missing from %s - not observed", entry.name, modulePath ? modulePath : "libxell.dll");
            continue;
        }
        InlineHook::PublishedHookSpec& spec = specs[queued];
        spec.target = target;
        spec.detour = entry.detour;
        spec.outTrampoline = &trampolines[queued];
        spec.publisher = PublishTrampoline;
        spec.publisherContext = entry.original;
        spec.foreignEntry = InlineHook::ForeignEntryPolicy::kLeaveToOwner;
        names[queued++] = entry.name;
    }
    const size_t installed = InlineHook::InstallPublishedBatch(specs, queued);
    for (size_t i = 0; i < queued; ++i) {
        if (!specs[i].installed) {
            HookLogImportant("XeLL: %s %s at %p", specs[i].leftToForeignOwner ? "left to its first patcher" : "not detoured",
                             names[i], specs[i].target);
        }
    }
    g_hookedModule = module;
    HookLogImportant("XeLL: %zu of %zu exports of %s observed", installed, queued, modulePath ? modulePath : "libxell.dll");
}

}  // namespace XeFGHook
