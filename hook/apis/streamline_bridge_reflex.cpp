// Streamline is a 64-bit runtime; see streamline_bridge_translate.cpp.
#if defined(_M_X64) || defined(__x86_64__)

#include "streamline_bridge_reflex.h"

#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <memory>
#include <mutex>

#include "../common/hook_common.h"
#include "sl_pcl.h"
#include "streamline_bridge_diag.h"
#include "streamline_bridge_policy.h"
#include "streamline_bridge_present.h"
#include "streamline_bridge_v1_abi.h"
#include "streamline_hook.h"

namespace ce::streamline_bridge {
namespace {

PFun_slReflexSetOptions* g_slReflexSetOptions = nullptr;
PFun_slReflexSleep* g_slReflexSleep = nullptr;
PFun_slReflexGetState* g_slReflexGetState = nullptr;
PFun_slPCLSetMarker* g_slPCLSetMarker = nullptr;
PFun_slPCLGetState* g_slPCLGetState = nullptr;
std::mutex g_resolveMutex;

std::atomic<uint32_t> g_forwardedReflexMode{UINT32_MAX};
// The mode the title itself last asked for through slSetFeatureConstants(Reflex).
std::atomic<uint32_t> g_titleReflexMode{kNoTitleReflexMode};
// Set by the first sleep the title drives itself; synthesized sleeps stop from then on.
std::atomic<bool> g_titleDrivesSleep{false};

// Per-marker counts for the periodic summary, indexed by 2.x PCLMarker value (0..8).
std::atomic<uint32_t> g_markerCounts[kV1ReflexMarkerPCLatencyPing + 1]{};
std::atomic<uint32_t> g_titleSleeps{0};
std::atomic<uint32_t> g_synthesizedSleeps{0};
std::atomic<uint32_t> g_markerFailures{0};
std::atomic<uint32_t> g_synthesizedPresents{0};

void LogMarkerSummary(uint32_t frameIndex) {
    HookLogImportant(
        "Streamline bridge: Reflex markers so far (frame %u) - simStart=%u simEnd=%u submitStart=%u submitEnd=%u "
        "presentStart=%u presentEnd=%u flash=%u ping=%u sleeps(title=%u synthesized=%u) "
        "re-marked presents=%u failures=%u",
        frameIndex, g_markerCounts[0].load(), g_markerCounts[1].load(), g_markerCounts[2].load(),
        g_markerCounts[3].load(), g_markerCounts[4].load(), g_markerCounts[5].load(), g_markerCounts[7].load(),
        g_markerCounts[8].load(), g_titleSleeps.load(), g_synthesizedSleeps.load(), g_synthesizedPresents.load(),
        g_markerFailures.load());
}

}  // namespace

bool ResolveReflexFunctions(PFun_slGetFeatureFunction* getFeatureFunction) {
    if (g_slReflexSetOptions && g_slReflexSleep && g_slReflexGetState && g_slPCLSetMarker && g_slPCLGetState) {
        return true;
    }
    std::lock_guard<std::mutex> lock(g_resolveMutex);
    if (!getFeatureFunction) {
        return false;
    }
    auto resolve = [getFeatureFunction](sl::Feature feature, const char* name, auto*& target) {
        if (!target) {
            getFeatureFunction(feature, name, reinterpret_cast<void*&>(target));
        }
    };
    resolve(sl::kFeatureReflex, "slReflexSetOptions", g_slReflexSetOptions);
    resolve(sl::kFeatureReflex, "slReflexSleep", g_slReflexSleep);
    resolve(sl::kFeatureReflex, "slReflexGetState", g_slReflexGetState);
    resolve(sl::kFeaturePCL, "slPCLSetMarker", g_slPCLSetMarker);
    resolve(sl::kFeaturePCL, "slPCLGetState", g_slPCLGetState);
    return g_slReflexSetOptions && g_slReflexSleep && g_slReflexGetState && g_slPCLSetMarker && g_slPCLGetState;
}

void* ReflexSetOptionsEntry() { return reinterpret_cast<void*>(g_slReflexSetOptions); }
void* ReflexSleepEntry() { return reinterpret_cast<void*>(g_slReflexSleep); }

bool ForwardReflexMode(sl::ReflexMode mode, bool synthesized) {
    const uint32_t modeValue = static_cast<uint32_t>(mode);
    if (g_forwardedReflexMode.load(std::memory_order_relaxed) == modeValue) {
        return true;
    }
    if (!g_slReflexSetOptions) {
        return false;
    }

    sl::ReflexOptions options{};
    options.mode = mode;
    static ResultTracker latch;
    if (!ResultOk(g_slReflexSetOptions(options), "slReflexSetOptions", latch)) {
        return false;
    }
    g_forwardedReflexMode.store(modeValue, std::memory_order_relaxed);
    HookLogImportant("Streamline bridge: %s Reflex mode=%u", synthesized ? "synthesized for DLSS-G" : "translated",
                     modeValue);
    return true;
}

// Reflex. 1.x configures it through slSetFeatureConstants; 2.x through slReflexSetOptions.
//
// DLSS-G does not engage with Reflex off, so refusing this call - which is what the bridge did
// at first - would leave frame generation configured and inert. Only `mode` is carried, because
// only `mode` was measured; see V1ReflexConstants for why the rest of that capture is stack.
bool TranslateReflexConstants(const void* constants1x, bool dlssgEnabled) {
    if (!g_slReflexSetOptions) {
        static std::atomic<bool> latch{false};
        RefuseOnce(latch, "slSetFeatureConstants(Reflex)", "the 2.x runtime has no slReflexSetOptions yet");
        return false;
    }
    const auto& in = *static_cast<const V1ReflexConstants*>(constants1x);
    // eOff / eLowLatency / eLowLatencyWithBoost, identical in both generations. Anything outside
    // that is refused rather than cast into an enum it does not belong to.
    if (in.mode > static_cast<uint32_t>(sl::ReflexMode::eLowLatencyWithBoost)) {
        static std::atomic<bool> latch{false};
        RefuseOnce(latch, "slSetFeatureConstants(Reflex)", "the 1.x Reflex mode is outside the known range");
        return false;
    }
    g_titleReflexMode.store(in.mode, std::memory_order_relaxed);
    const sl::ReflexMode requested = static_cast<sl::ReflexMode>(in.mode);
    return ForwardReflexMode(dlssgEnabled ? sl::ReflexMode::eLowLatencyWithBoost : requested,
                             /*synthesized=*/dlssgEnabled);
}

bool UpdateReflexForDlssg(bool dlssgEnabled) {
    const uint32_t titleMode = g_titleReflexMode.load(std::memory_order_relaxed);
    const uint32_t mode = ReflexModeForDlssgState(dlssgEnabled, titleMode);
    const bool synthesized = dlssgEnabled || titleMode == kNoTitleReflexMode;
    return ForwardReflexMode(static_cast<sl::ReflexMode>(mode), synthesized);
}

bool TranslateReflexSettings(void* settings1x) {
    if (!settings1x || !g_slReflexGetState) {
        static std::atomic<bool> latch{false};
        RefuseOnce(latch, "slGetFeatureSettings(Reflex)", "no settings buffer or no 2.x slReflexGetState yet");
        return false;
    }
    // ReflexState carries 128 frame reports; keep it off the game thread's stack.
    static std::mutex mutex;
    static std::unique_ptr<sl::ReflexState> state;
    std::lock_guard<std::mutex> lock(mutex);
    if (!state) {
        state = std::make_unique<sl::ReflexState>();
    }
    *state = sl::ReflexState{};
    static ResultTracker latch;
    if (!ResultOk(g_slReflexGetState(*state), "slReflexGetState", latch)) {
        return false;
    }
    uint32_t statsWindowMessage = state->statsWindowMessage;
    if (g_slPCLGetState) {
        sl::PCLState pcl{};
        if (g_slPCLGetState(pcl) == sl::Result::eOk) {
            statsWindowMessage = pcl.statsWindowMessage;
        }
    }
    // Exactly the four fields the game's 1.5.6 runtime writes; see V1ReflexSettings.
    auto& out = *static_cast<V1ReflexSettings*>(settings1x);
    out.lowLatencyAvailable = state->lowLatencyAvailable ? 1 : 0;
    out.latencyReportAvailable = state->latencyReportAvailable ? 1 : 0;
    out.statsWindowMessage = statsWindowMessage;
    out.flashIndicatorDriverControlled = state->flashIndicatorDriverControlled ? 1 : 0;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        HookLogImportant(
            "Streamline bridge: answered slGetFeatureSettings(Reflex) - lowLatencyAvailable=%d "
            "latencyReportAvailable=%d flashDriverControlled=%d statsWindowMessage=0x%X",
            out.lowLatencyAvailable, out.latencyReportAvailable, out.flashIndicatorDriverControlled,
            statsWindowMessage);
    }
    return true;
}

bool TranslateReflexEvaluate(uint32_t id, uint32_t frameIndex, const sl::FrameToken* token) {
    uint32_t marker = 0;
    const V1ReflexEvaluate kind = ClassifyV1ReflexEvaluate(id, &marker);
    if (kind == V1ReflexEvaluate::kDeprecated) {
        return true;  // 2.x dropped the input-sample marker; the 1.x runtime accepted it
    }
    if (kind == V1ReflexEvaluate::kUnknown) {
        static std::atomic<bool> latch{false};
        RefuseOnce(latch, "slEvaluateFeature(Reflex)", "the marker id is outside the 1.x Reflex marker set");
        return false;
    }
    if (!token) {
        static std::atomic<bool> latch{false};
        RefuseOnce(latch, "slEvaluateFeature(Reflex)", "no 2.x frame token exists for the marker's frame yet");
        return false;
    }
    if (kind == V1ReflexEvaluate::kSleep) {
        if (!g_slReflexSleep) {
            return false;
        }
        if (!g_titleDrivesSleep.exchange(true, std::memory_order_relaxed)) {
            HookLogImportant("Streamline bridge: the title drives its own Reflex sleep (frame %u, token %u) - "
                             "synthesized sleeps stop",
                             frameIndex, static_cast<uint32_t>(*token));
        }
        g_titleSleeps.fetch_add(1, std::memory_order_relaxed);
        static ResultTracker latch;
        return ResultOk(g_slReflexSleep(*token), "slReflexSleep", latch);
    }

    if (!g_slPCLSetMarker) {
        static std::atomic<bool> latch{false};
        RefuseOnce(latch, "slEvaluateFeature(Reflex marker)", "the 2.x runtime has no slPCLSetMarker yet");
        return false;
    }
    const sl::Result result = g_slPCLSetMarker(static_cast<sl::PCLMarker>(marker), *token);
    const uint32_t count = g_markerCounts[marker].fetch_add(1, std::memory_order_relaxed) + 1;
    if (result != sl::Result::eOk) {
        g_markerFailures.fetch_add(1, std::memory_order_relaxed);
    } else if (marker == static_cast<uint32_t>(sl::PCLMarker::ePresentStart)) {
        NoteTitlePresentStart(frameIndex);
    }
    static std::atomic<bool> loggedFirst{false};
    if (!loggedFirst.exchange(true, std::memory_order_relaxed)) {
        HookLogImportant("Streamline bridge: first Reflex marker translated - marker=%u frame=%u token=%u "
                         "sl::Result=%d",
                         marker, frameIndex, static_cast<uint32_t>(*token), static_cast<int>(result));
    }
    if (marker == static_cast<uint32_t>(sl::PCLMarker::ePresentStart) && (count == 1 || count % 1000 == 0)) {
        LogMarkerSummary(frameIndex);
    }
    static ResultTracker latch;
    return ResultOk(result, "slPCLSetMarker", latch);
}

bool SynthesizePresentMarkers(const sl::FrameToken& token) {
    if (!g_slPCLSetMarker) {
        return false;
    }
    // A complete pair, so the driver's report for the re-marked frame stays START-before-END.
    // Issued from inside sl.dlss_g's Present hook: not the title's frame boundary.
    StreamlineHook::CeIssuedFrameMarkerScope ceIssued;
    const sl::Result start = g_slPCLSetMarker(sl::PCLMarker::ePresentStart, token);
    const sl::Result end = g_slPCLSetMarker(sl::PCLMarker::ePresentEnd, token);
    g_synthesizedPresents.fetch_add(1, std::memory_order_relaxed);
    const bool ok = start == sl::Result::eOk && end == sl::Result::eOk;
    if (!ok) {
        g_markerFailures.fetch_add(1, std::memory_order_relaxed);
    }
    return ok;
}

bool MaybeSynthesizeReflexSleep(uint32_t frameIndex, const sl::FrameToken* token, bool dlssgEnabled) {
    if (!token || !dlssgEnabled || g_titleDrivesSleep.load(std::memory_order_relaxed)) {
        return true;
    }
    static std::atomic<uint32_t> attemptedFrame{UINT32_MAX};
    if (attemptedFrame.exchange(frameIndex, std::memory_order_relaxed) == frameIndex) {
        return true;
    }
    if (!g_slReflexSleep) {
        static std::atomic<bool> latch{false};
        RefuseOnce(latch, "slReflexSleep", "the 2.x runtime did not provide the Reflex frame entry point");
        return false;
    }
    g_synthesizedSleeps.fetch_add(1, std::memory_order_relaxed);
    static ResultTracker latch;
    return ResultOk(g_slReflexSleep(*token), "slReflexSleep", latch);
}

}  // namespace ce::streamline_bridge

#endif  // x64
