#include "system_latency_metrics.h"

#include "reflex_defs.h"

#include <windows.h>

#include <atomic>
#include <mutex>

namespace ce::system_latency {
namespace {

std::atomic<SupplementalNativeReportProvider> g_supplementalReportProvider{nullptr};

PFN_NvAPI_D3D_GetLatency ResolveGetLatency() {
    static std::mutex resolveMutex;
    static HMODULE resolvedModule = nullptr;
    static PFN_NvAPI_D3D_GetLatency getLatency = nullptr;

#ifdef _WIN64
    HMODULE module = GetModuleHandleW(L"nvapi64.dll");
#else
    HMODULE module = GetModuleHandleW(L"nvapi.dll");
#endif
    if (!module)
        return nullptr;

    std::lock_guard<std::mutex> lock(resolveMutex);
    if (module == resolvedModule && getLatency)
        return getLatency;

    // A caller-filtered GetProcAddress hook can be arming while the first
    // overlay query runs. Cache successes, but retry a null resolution on the
    // next bounded telemetry poll instead of disabling Reflex data forever.
    resolvedModule = module;
    getLatency = nullptr;
    const auto queryInterface =
        reinterpret_cast<PFN_NvAPI_QueryInterface>(GetProcAddress(module, "nvapi_QueryInterface"));
    if (queryInterface) {
        getLatency = reinterpret_cast<PFN_NvAPI_D3D_GetLatency>(queryInterface(NVAPI_ID_D3D_GetLatency));
    }
    return getLatency;
}

}  // namespace

void SetSupplementalNativeReportProvider(SupplementalNativeReportProvider provider) {
    g_supplementalReportProvider.store(provider, std::memory_order_release);
}

bool QueryNativeReport(void* device, NativeReport& report) {
    report = {};

    // Streamline PCL markers do not populate NvAPI_D3D_GetLatency; that API is
    // explicitly scoped to NvAPI_D3D_SetLatencyMarker. Prefer the game's real
    // cross-IHV PCL calls captured by the Streamline hook when available.
    const SupplementalNativeReportProvider supplementalProvider =
        g_supplementalReportProvider.load(std::memory_order_acquire);
    if (supplementalProvider && supplementalProvider(report))
        return true;
    if (!device)
        return false;

    const auto getLatency = ResolveGetLatency();
    if (!getLatency)
        return false;

    NV_LATENCY_RESULT_PARAMS nativeReport{};
    nativeReport.version = NV_LATENCY_RESULT_PARAMS_VER;
    if (getLatency(static_cast<IUnknown*>(device), &nativeReport) != NVAPI_OK)
        return false;

    bool hasMarkers = false;
    report.count = report.frames.size();
    for (size_t i = 0; i < report.frames.size(); ++i) {
        const auto& source = nativeReport.frameReport[i];
        auto& destination = report.frames[i];
        // NVAPI returns the Reflex timeline in microseconds. NVIDIA's
        // Streamline D3D adapter preserves these values verbatim; they are
        // not raw QueryPerformanceCounter ticks.
        destination.frameId = source.frameID;
        destination.inputSampleTimeUs = source.inputSampleTime;
        destination.simulationStartTimeUs = source.simStartTime;
        destination.presentStartTimeUs = source.presentStartTime;
        destination.gpuRenderEndTimeUs = source.gpuRenderEndTime;
        hasMarkers = hasMarkers || (source.simStartTime != 0 && source.presentStartTime != 0);
    }
    if (!hasMarkers)
        report = {};
    return hasMarkers;
}

}  // namespace ce::system_latency
