#include "vulkan_fg_switch_test_internal.h"

namespace testapp::vkfg {

bool SetReflexMode(bool enabled, const char* reason) {
    if (!g_App.sl.reflexSetOptions || !g_App.sl.reflexSupported) {
        return !enabled;
    }
    if (g_App.sl.reflexOptionsConfigured && g_App.sl.reflexActive == enabled) {
        return true;
    }
    sl::ReflexOptions options{};
    options.mode = enabled ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eOff;
    const sl::Result result = g_App.sl.reflexSetOptions(options);
    testapp::Log(
        "[FG-DIAG] slReflexSetOptions requested=%s frameLimitUs=%u "
        "automaticDriverPacing=unmodified reason=%s result=%d(%s)\n",
        enabled ? "low-latency" : "off", options.frameLimitUs,
        reason ? reason : "unknown", static_cast<int>(result), SlResultName(result));
    if (result == sl::Result::eOk) {
        g_App.sl.reflexActive = enabled;
        g_App.sl.reflexOptionsConfigured = true;
    }
    testapp::LogFlush();
    return result == sl::Result::eOk;
}
void PollReflexState(bool force) {
    if (!g_App.sl.reflexGetState || !g_App.sl.reflexSupported ||
        (!force && (g_App.frameId % 120) != 0)) {
        return;
    }
    sl::ReflexState state{};
    const sl::Result result = g_App.sl.reflexGetState(state);
    g_App.sl.reflexStateAvailable = result == sl::Result::eOk && state.lowLatencyAvailable;
    testapp::Log(
        "[FG-DIAG] slReflexGetState result=%d(%s) lowLatencyAvailable=%d "
        "latencyReportAvailable=%d configuredMode=%s frameLimitUs=0\n",
        static_cast<int>(result), SlResultName(result), state.lowLatencyAvailable ? 1 : 0,
        state.latencyReportAvailable ? 1 : 0,
        g_App.sl.reflexActive ? "low-latency" : "off");
}
void PollStreamlineState() {
    PollReflexState(false);
    if (!g_App.sl.dlssgGetState || g_App.swapchain.owner != SwapchainOwner::Streamline) {
        return;
    }
    sl::DLSSGState state{};
    const sl::Result result = g_App.sl.dlssgGetState(g_App.sl.viewport, state, nullptr);
    if (result == sl::Result::eOk) {
        const bool vsyncSupported = state.bIsVsyncSupportAvailable == sl::Boolean::eTrue;
        if (!g_App.sl.dlssgVsyncSupportKnown ||
            g_App.sl.dlssgVsyncSupported != vsyncSupported) {
            g_App.sl.dlssgVsyncSupportKnown = true;
            g_App.sl.dlssgVsyncSupported = vsyncSupported;
            testapp::Log(
                "[FG-DIAG] DLSS-G Vulkan VSync support available=%d; frameLimitUs=0 "
                "(no application-side limiter or emulation)\n",
                vsyncSupported ? 1 : 0);
            if (!vsyncSupported) {
                testapp::Log(
                    "[FG-DIAG] WARN automatic VSync/VRR below-refresh pacing is unavailable "
                    "while Streamline Vulkan DLSS-G owns presentation\n");
            }
            testapp::LogFlush();
        }
        const uint32_t presented = std::max(state.numFramesActuallyPresented, 1u);
        if (presented > 1) {
            g_App.generatedFrames += presented - 1;
            g_App.sl.generatedPresentCount += presented - 1;
        }
    }
    if (g_App.frameId < 5 || result != sl::Result::eOk || (g_App.frameId % 120) == 0) {
        testapp::Log(
            "[FG-DIAG] slDLSSGGetState result=%d(%s) status=0x%x presented=%u maxGenerated=%u "
            "dynamicMFG=%d vsyncSupport=%d requested=%d configured=%d effective=%d\n",
            static_cast<int>(result), SlResultName(result), static_cast<unsigned>(state.status),
            state.numFramesActuallyPresented, state.numFramesToGenerateMax,
            state.bIsDynamicMFGSupported == sl::Boolean::eTrue ? 1 : 0,
            state.bIsVsyncSupportAvailable == sl::Boolean::eTrue ? 1 : 0,
            g_App.transition.currentMode == FgMode::Dlss && !g_App.transition.suspended ? 1 : 0,
            g_App.sl.dlssFgConfigured ? 1 : 0,
            result == sl::Result::eOk && state.status == sl::DLSSGStatus::eOk &&
                    g_App.sl.dlssFgConfigured
                ? 1
                : 0);
    }
}
void ShutdownStreamline() {
    if (!g_App.sl.initialized) {
        if (g_App.sl.module) {
            FreeLibrary(g_App.sl.module);
            g_App.sl.module = nullptr;
        }
        return;
    }
    SetReflexMode(false, "final Streamline shutdown");
    const sl::Result result = g_App.sl.shutdown ? g_App.sl.shutdown() : sl::Result::eErrorNotInitialized;
    testapp::Log("[FG-DIAG] slShutdown(before Vulkan destruction) result=%d(%s)\n", static_cast<int>(result),
                 SlResultName(result));
    g_App.sl.initialized = false;
    if (g_App.sl.module) {
        FreeLibrary(g_App.sl.module);
        g_App.sl.module = nullptr;
    }
    testapp::LogFlush();
}

}  // namespace testapp::vkfg
