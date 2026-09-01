#include "streamline_hook_internal.h"

#include "../common/perf_logger.h"
#include "../common/streamline_pcl_latency.h"

#include <sl_pcl.h>

namespace {

using PclSetMarkerFn = PFun_slPCLSetMarker*;

std::mutex g_pclHookMutex;
PclSetMarkerFn g_originalPclSetMarker = nullptr;
std::atomic<void*> g_pclSetMarkerTarget{nullptr};
std::atomic<void*> g_pclImportFallbackAttemptedTarget{nullptr};
std::atomic<bool> g_pclSetMarkerHooked{false};
std::atomic<bool> g_pclLookupLogged{false};
std::atomic<bool> g_pclReturnedWrapperLogged{false};
std::atomic<bool> g_pclProactiveGapLogged{false};
std::atomic<bool> g_pclReportLogged{false};
ce::system_latency::PclMarkerHistory g_pclMarkerHistory;

void RegisterPclReportProvider() {
    ce::system_latency::SetSupplementalNativeReportProvider(StreamlineHook::QueryPCLLatencyReport);
}

PclSetMarkerFn GetCallableOriginalPclSetMarker() {
    PclSetMarkerFn original = g_originalPclSetMarker;
    return IsSavedStreamlineOriginalCallable("slPCLSetMarker", reinterpret_cast<void*>(original),
                                             g_pclSetMarkerTarget.load(std::memory_order_acquire),
                                             "PCL feature module")
               ? original
               : nullptr;
}

sl::Result Hooked_slPCLSetMarker(sl::PCLMarker marker, const sl::FrameToken& frame) {
    PclSetMarkerFn original = GetCallableOriginalPclSetMarker();
    if (!original)
        return static_cast<sl::Result>(streamline_hook_kSlResultErrorInvalidState);
    if (HookIsShuttingDown())
        return original(marker, frame);

    const uint32_t markerValue = static_cast<uint32_t>(marker);
    const bool captureMarker = markerValue == ce::system_latency::PclMarkerHistory::kSimulationStartMarker ||
                               markerValue == ce::system_latency::PclMarkerHistory::kPresentStartMarker;
    const int64_t markerTimeUs = captureMarker ? PerfLogger::GetQpcUs() : 0;
    const uint64_t frameId = captureMarker ? static_cast<uint32_t>(frame) : 0;

    const sl::Result result = original(marker, frame);
    if (result == sl::Result::eOk && captureMarker)
        g_pclMarkerHistory.Record(markerValue, frameId, markerTimeUs);
    else if (result != sl::Result::eOk && captureMarker) {
        static std::atomic<uint32_t> s_forwardFailureCount{0};
        const uint32_t failureCount = s_forwardFailureCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ce::log_meter::ShouldLogCadence(failureCount, 5, 300)) {
            HookLogImportant("Streamline Hook: slPCLSetMarker forward failed (marker=%u frame=%llu result=%d "
                             "count=%u)",
                             markerValue, static_cast<unsigned long long>(frameId), static_cast<int>(result),
                             failureCount);
        }
    }
    return result;
}

}  // namespace

bool MaybeHookPCLSetMarker(void*& function, bool fallbackToReturnedWrapper) {
    if (!function)
        return false;

    void* detour = reinterpret_cast<void*>(Hooked_slPCLSetMarker);
    if (function == detour) {
        g_pclSetMarkerHooked.store(true, std::memory_order_release);
        RegisterPclReportProvider();
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(g_pclHookMutex);
        if (!g_pclSetMarkerHooked.load(std::memory_order_acquire) ||
            g_pclSetMarkerTarget.load(std::memory_order_acquire) != function) {
            const void* previousTarget = g_pclSetMarkerTarget.load(std::memory_order_acquire);
            InstallInlineHookOnce(function, detour, g_originalPclSetMarker, g_pclSetMarkerHooked,
                                  g_pclSetMarkerTarget, "slPCLSetMarker");
            if (!g_pclSetMarkerHooked.load(std::memory_order_acquire)) {
                TryInstallFeatureImportFallbackForOwningModule(
                    function, "slPCLSetMarker", detour, reinterpret_cast<void**>(&g_originalPclSetMarker),
                    g_pclImportFallbackAttemptedTarget, "slPCLSetMarker");
            }
            if (g_pclSetMarkerHooked.load(std::memory_order_acquire) &&
                g_pclSetMarkerTarget.load(std::memory_order_acquire) == function && previousTarget != function) {
                g_pclMarkerHistory.Reset();
                g_pclReportLogged.store(false, std::memory_order_release);
            }
        }
    }

    const bool hookReady = g_pclSetMarkerHooked.load(std::memory_order_acquire);
    if (hookReady)
        RegisterPclReportProvider();
    if (fallbackToReturnedWrapper && !hookReady) {
        if (!g_originalPclSetMarker)
            g_originalPclSetMarker = reinterpret_cast<PclSetMarkerFn>(function);
        LogReturnedWrapperFallbackOnce(g_pclReturnedWrapperLogged, "slPCLSetMarker", function, detour, hookReady);
        function = detour;
        RegisterPclReportProvider();
        return true;
    }
    return hookReady;
}

bool IsPCLSetMarkerHookReady() {
    return g_pclSetMarkerHooked.load(std::memory_order_acquire);
}

void LogPCLFeatureLookupOutcome(void* originalTarget, void* returnedTarget, bool hookReady) {
    LogFeatureLookupOutcomeOnce(g_pclLookupLogged, "slPCLSetMarker", originalTarget, returnedTarget, hookReady);
}

void LogPCLProactiveFeatureHookGap(void* target) {
    LogProactiveFeatureHookGapOnce(g_pclProactiveGapLogged, "slPCLSetMarker", target);
}

bool InvalidatePCLFeatureHookForModule(const void* moduleBase, size_t moduleSizeBytes, const char* moduleBaseName) {
    void* target = g_pclSetMarkerTarget.load(std::memory_order_acquire);
    void* original = reinterpret_cast<void*>(g_originalPclSetMarker);
    if (!ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(
            target, original, moduleBase, moduleSizeBytes)) {
        return false;
    }

    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&g_originalPclSetMarker), nullptr);
    g_pclSetMarkerTarget.store(nullptr, std::memory_order_release);
    g_pclImportFallbackAttemptedTarget.store(nullptr, std::memory_order_release);
    g_pclSetMarkerHooked.store(false, std::memory_order_release);
    g_pclLookupLogged.store(false, std::memory_order_release);
    g_pclReturnedWrapperLogged.store(false, std::memory_order_release);
    g_pclProactiveGapLogged.store(false, std::memory_order_release);
    HookLogImportant(
        "Streamline Hook: Invalidated slPCLSetMarker hook slot for unloaded %s "
        "(target=%p original=%p base=%p size=0x%zX)",
        moduleBaseName ? moduleBaseName : "Streamline module", target, original, moduleBase, moduleSizeBytes);
    return true;
}

void ResetPCLLatencyCapture() {
    g_pclMarkerHistory.Reset();
    g_pclReportLogged.store(false, std::memory_order_release);
}

namespace StreamlineHook {

bool QueryPCLLatencyReport(ce::system_latency::NativeReport& report) {
    if (!g_pclMarkerHistory.BuildFreshReport(report, PerfLogger::GetQpcUs()))
        return false;
    if (!g_pclReportLogged.exchange(true, std::memory_order_acq_rel)) {
        HookLogImportant("Streamline Hook: PCL marker latency report available (frames=%zu)", report.count);
    }
    return true;
}

}  // namespace StreamlineHook
