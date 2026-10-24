#include "xefg_hook_internal.h"

#include <atomic>
#include <mutex>

#include "common/logging/log_meter.h"
#include "hook/hooking/inline_hook.h"
#include "hook/metrics/perf_logger.h"
#include "hook/metrics/streamline_pcl_latency.h"
#include "hook/pacing/fps_limiter.h"
#include "hook/runtime/hook_common.h"

// XeLL (libxell.dll), the latency reduction XeFG requires: the game's sleep mode (a minimum frame interval is a
// frame-rate cap of its own) and its per-frame latency markers. Observed at the exports like XeFG's; every call is
// forwarded unchanged.
//
// The game's XeLL cap paces at xellSleep, the frame's start; CE's limiter paces the same frames at SetPresentId,
// their end (xefg_hook.cpp). In series the lower cap is the one that waits and the other finds every frame late
// already, so the two compose without CE touching the game's sleep parameters; both are logged.
//
// The markers carry XeLL's frame identity, the present ID: a frame's SIMULATION_START and PRESENT_START are the
// frame-exact input anchor of CE's PC latency estimate (ce::system_latency::NoteMarkerFrameBegin), as Streamline's
// PCL markers are. XeFG's latency is not NVIDIA's, so no native report is built from them.

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
std::atomic<uint64_t> g_anchoredFrames{0};
ce::system_latency::PclMarkerHistory g_markerHistory;  // XeLL numbers its markers as PCL does

void LogCapComposition(uint32_t gameIntervalUs) {
    const auto limiter = g_SharedFpsLimiter.GetResolvedCadence();
    const double gameFps = gameIntervalUs ? 1000000.0 / gameIntervalUs : 0.0;
    const double ceRenderedFps =
        limiter.intervalUs > 0 ? 1000000.0 / static_cast<double>(limiter.intervalUs) : 0.0;  // per paced frame
    const char* binding = gameFps <= 0.0 && ceRenderedFps <= 0.0 ? "none"
                          : gameFps <= 0.0                       ? "CE"
                          : ceRenderedFps <= 0.0                 ? "game XeLL"
                          : gameFps < ceRenderedFps              ? "game XeLL"
                                                                 : "CE";
    HookLogImportant("XeLL: frame caps in series - game XeLL %.1f fps (minimumIntervalUs=%u), CE limiter %.1f rendered "
                     "fps (target=%d scale=%d intervalUs=%lld, as last resolved); the lower paces: %s",
                     gameFps, gameIntervalUs, ceRenderedFps, limiter.targetFps, limiter.cadenceScale,
                     static_cast<long long>(limiter.intervalUs), binding);
}

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
        if (result == 0 && params && params->minimumIntervalUs) {
            LogCapComposition(params->minimumIntervalUs);
        }
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
    const bool anchorMarker = marker == kSimulationStart || marker == kPresentStart;
    const int64_t markerTimeUs = anchorMarker && !HookIsShuttingDown() ? PerfLogger::GetQpcUs() : 0;
    const Result result = original(context, frameId, marker);
    int64_t simulationStartUs = 0;
    if (result == 0 && markerTimeUs > 0 &&
        g_markerHistory.Record(static_cast<uint32_t>(marker), frameId, markerTimeUs, &simulationStartUs) &&
        simulationStartUs > 0) {
        ce::system_latency::NoteMarkerFrameBegin(simulationStartUs, markerTimeUs, GetCurrentThreadId());
        const uint64_t anchored = g_anchoredFrames.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ce::log_meter::ShouldLogCadence(static_cast<uint32_t>(anchored), 1, 3000)) {
            HookLog("XeLL: frame %u anchors PC latency (simulation start to present start %.2f ms, anchored=%llu)",
                    frameId, static_cast<double>(markerTimeUs - simulationStartUs) / 1000.0,
                    static_cast<unsigned long long>(anchored));
        }
    }
    return result;
}

void PublishTrampoline(void* trampoline, void* context) {
    static_cast<std::atomic<void*>*>(context)->store(trampoline, std::memory_order_release);
}

std::mutex g_installMutex;
HookedRuntimeImage g_image;

}  // namespace

void InstallXeLLExports(HMODULE module, const char* modulePath) {
    std::lock_guard<std::mutex> lock(g_installMutex);
    if (!module || g_image.IsHooked(module)) {
        return;
    }
    if (g_image.IsOwnedByAnotherImage(module)) {
        static ce::log_meter::ChangeGate s_gate;
        if (const auto verdict = s_gate.Observe(ce::log_meter::FieldKey(module))) {
            HookLogImportant("XeLL: second image %s at %p left unobserved - the loaded image owns CE's detours%s",
                             modulePath ? modulePath : "libxell.dll", module,
                             ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
        return;
    }
    if (const size_t forgotten = g_image.ForgetUnloaded()) {
        HookLogImportant("XeLL: %s mapped after an unload at %p - forgot %zu detour record(s) of the unloaded image",
                         modulePath ? modulePath : "libxell.dll", module, forgotten);
    }
    g_image.Begin(module);
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
    void* targets[kCount] = {};
    for (size_t i = 0; i < queued; ++i) {
        targets[i] = specs[i].target;
    }
    g_image.NoteTargets(targets, queued);
    for (size_t i = 0; i < queued; ++i) {
        if (!specs[i].installed) {
            HookLogImportant("XeLL: %s %s at %p", specs[i].leftToForeignOwner ? "left to its first patcher" : "not detoured",
                             names[i], specs[i].target);
        }
    }
    HookLogImportant("XeLL: %zu of %zu exports of %s observed", installed, queued, modulePath ? modulePath : "libxell.dll");
}

bool NoteXeLLModuleUnloaded(const void* moduleBase) {
    return g_image.NoteUnloaded(moduleBase);
}

}  // namespace XeFGHook
