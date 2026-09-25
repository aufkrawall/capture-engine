#include "ffx_hook_internal.h"
#include "../../common/log_meter.h"

// Bookkeeping for the expensive half of ffx_hook_InstallHooksForModule (the IAT walks and the cached-slot scan):
// which inputs a completed sweep covered per FFX runtime image, and the sweep diagnostics. The decision itself is
// the pure policy in hook/common/ffx_module_rescan_policy.h. Callers hold ffx_hook_g_InitMutex.

namespace {

struct SweepSlot {
    HMODULE module = nullptr;
    ce::ffx_module_rescan::State state;
};

// Two official runtimes can be resident at once (e.g. amd_fidelityfx_dx12 plus a framegeneration DLL); each keeps
// its own state so alternating passes over them do not look like a new image every time.
SweepSlot g_SweepSlots[4];
size_t g_NextSweepSlot = 0;

SweepSlot& SlotFor(HMODULE module) {
    for (SweepSlot& slot : g_SweepSlots) {
        if (slot.module == module) {
            return slot;
        }
    }
    SweepSlot& slot = g_SweepSlots[g_NextSweepSlot];
    g_NextSweepSlot = (g_NextSweepSlot + 1) % _countof(g_SweepSlots);
    slot = SweepSlot{module, {}};
    return slot;
}

}  // namespace

FfxModuleSweep ffx_hook_DecideModuleSweep(HMODULE module) {
    FfxModuleSweep sweep;
    sweep.module = module;
    // Sampled BEFORE the sweep: a module load or trapped call during it still differs afterwards.
    sweep.inputs.runtimeModule = module;
    sweep.inputs.loaderNotificationsLive = ce::overlay_compat::module_address_cache::IsEnabled();
    sweep.inputs.moduleSetGeneration = ce::overlay_compat::module_address_cache::ModuleSetGeneration();
    sweep.inputs.unroutedCallEvidence = ffx_hook_g_UnroutedCallEvidence.load(std::memory_order_acquire);
    sweep.reason = ce::ffx_module_rescan::Decide(SlotFor(module).state, sweep.inputs);
    if (sweep.Run()) {
        QueryPerformanceCounter(&sweep.start);
    }
    return sweep;
}

void ffx_hook_CompleteModuleSweep(const FfxModuleSweep& sweep, const char* moduleName,
                                  const ce::ffx_cached_pointer_router::RefreshResult& routes) {
    if (!sweep.Run()) {
        return;
    }
    ce::ffx_module_rescan::Commit(SlotFor(sweep.module).state, sweep.inputs);

    LARGE_INTEGER end = {};
    LARGE_INTEGER frequency = {};
    QueryPerformanceCounter(&end);
    QueryPerformanceFrequency(&frequency);
    const double sweepMs =
        static_cast<double>(end.QuadPart - sweep.start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
    static std::atomic<uint32_t> s_sweepCount{0};
    const uint32_t sweeps = s_sweepCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ce::log_meter::ShouldLogCadence(sweeps, 20, 100) ||
        sweep.reason == ce::ffx_module_rescan::Reason::kNewRuntimeModule) {
        HookLog("FFX Hook: import/cached-slot sweep for %s reason=%s ms=%.1f modules=%zu sections=%zu routed=%zu "
                "moduleSet=%llu evidence=%llu (sweep #%u; skipped until one of these changes)",
                moduleName ? moduleName : "FFX", ce::ffx_module_rescan::ReasonName(sweep.reason), sweepMs,
                routes.modulesScanned, routes.writableSectionsScanned, routes.pointerSlotsPatched,
                static_cast<unsigned long long>(sweep.inputs.moduleSetGeneration),
                static_cast<unsigned long long>(sweep.inputs.unroutedCallEvidence), sweeps);
    }
}
