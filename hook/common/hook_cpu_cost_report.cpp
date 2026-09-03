#include "hook_cpu_cost.h"

#include "hook_common.h"
#include "../../common/logging.h"

// The two hook sites a frame-generation runtime drives hardest. Both are
// process-wide singletons so the report can name their combined share of a core
// without threading a context through every call site.
bool HookCpuCostMeasurementEnabled() {
    static const bool enabled = Log_IsEnabled(LogLevel::Debug);
    return enabled;
}

HookCpuCost& HookPresentCpuCost() {
    static HookCpuCost cost;
    return cost;
}

HookCpuCost& HookExecuteCommandListsCpuCost() {
    static HookCpuCost cost;
    return cost;
}

// Reports how much CPU CE's per-frame hooks took over the last window. Cycles
// are turned into a share of one core against elapsed wall time and the
// counter's own rate, so no CPU frequency has to be assumed.
void ReportHookCpuCostIfDue() {
    static std::atomic<uint64_t> s_lastReportTickMs{0};
    static std::atomic<uint64_t> s_lastPresentCycles{0};
    static std::atomic<uint64_t> s_lastEclCycles{0};
    static std::atomic<uint64_t> s_lastPresentWallUs{0};
    static std::atomic<uint64_t> s_lastEclWallUs{0};
    if (!HookCpuCostMeasurementEnabled()) {
        return;
    }
    const uint64_t now = GetTickCount64();
    uint64_t last = s_lastReportTickMs.load(std::memory_order_relaxed);
    if (now - last < 10'000) {
        return;
    }
    // Exactly one caller wins the window; the rest return without reporting, so
    // two present threads cannot both consume the same interval's deltas.
    if (!s_lastReportTickMs.compare_exchange_strong(last, now, std::memory_order_acq_rel)) {
        return;
    }
    if (last == 0) {
        return;  // The first window only establishes the baseline.
    }
    const uint64_t elapsedMs = now - last;
    const uint64_t presentCycles = HookPresentCpuCost().cycles();
    const uint64_t eclCycles = HookExecuteCommandListsCpuCost().cycles();
    const uint64_t presentDelta = presentCycles - s_lastPresentCycles.exchange(presentCycles,
                                                                              std::memory_order_relaxed);
    const uint64_t eclDelta = eclCycles - s_lastEclCycles.exchange(eclCycles, std::memory_order_relaxed);
    // Wall time the hooks HELD their threads, forwarded runtime calls excluded. A hook
    // that blocks costs a frame-generation game everything and costs cycles nothing, so
    // usPerMs far above the cycle share is the signature to look for: it says CE is
    // waiting on the runtime's submission or present thread, not computing.
    const uint64_t presentWallUs = HookPresentCpuCost().wallUs();
    const uint64_t eclWallUs = HookExecuteCommandListsCpuCost().wallUs();
    const uint64_t presentWallDelta =
        presentWallUs - s_lastPresentWallUs.exchange(presentWallUs, std::memory_order_relaxed);
    const uint64_t eclWallDelta = eclWallUs - s_lastEclWallUs.exchange(eclWallUs, std::memory_order_relaxed);
    // Cycles per millisecond of one core, measured from this machine's own
    // counter rather than a nominal clock: a busy core advances the cycle
    // counter at its actual rate, so the ratio is the share of a core.
    LARGE_INTEGER frequency = {};
    QueryPerformanceFrequency(&frequency);
    HookLogImportant(
        "[OVERLAY COST] CE hook CPU over %llu ms: present(calls=%llu avgCycles=%llu maxCycles=%llu "
        "cyclesPerMs=%llu avgWallUs=%llu maxWallUs=%llu usPerMs=%llu) executeCommandLists(calls=%llu "
        "avgCycles=%llu maxCycles=%llu cyclesPerMs=%llu avgWallUs=%llu maxWallUs=%llu usPerMs=%llu) "
        "— divide cyclesPerMs by the core's MHz to read it as a share of one core; usPerMs is the wall "
        "time CE held the thread per millisecond, and usPerMs >> the cycle share means CE is blocking",
        static_cast<unsigned long long>(elapsedMs),
        static_cast<unsigned long long>(HookPresentCpuCost().calls()),
        static_cast<unsigned long long>(HookPresentCpuCost().averageCycles()),
        static_cast<unsigned long long>(HookPresentCpuCost().maxCycles()),
        static_cast<unsigned long long>(presentDelta / elapsedMs),
        static_cast<unsigned long long>(HookPresentCpuCost().averageWallUs()),
        static_cast<unsigned long long>(HookPresentCpuCost().maxWallUs()),
        static_cast<unsigned long long>(presentWallDelta / elapsedMs),
        static_cast<unsigned long long>(HookExecuteCommandListsCpuCost().calls()),
        static_cast<unsigned long long>(HookExecuteCommandListsCpuCost().averageCycles()),
        static_cast<unsigned long long>(HookExecuteCommandListsCpuCost().maxCycles()),
        static_cast<unsigned long long>(eclDelta / elapsedMs),
        static_cast<unsigned long long>(HookExecuteCommandListsCpuCost().averageWallUs()),
        static_cast<unsigned long long>(HookExecuteCommandListsCpuCost().maxWallUs()),
        static_cast<unsigned long long>(eclWallDelta / elapsedMs));
}
