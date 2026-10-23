// Fake tool module in a tool's folder (OptiScaler's): the fake sl.interposer hands the game this module's
// slDLSSGSetOptions/slDLSSGGetState in place of Streamline's, as OptiScaler does by hooking slGetFeatureFunction.
// CE_FLOW_DLSSG_API_TOOL_MODE picks what they do:
//   answer  - OptiScaler with FGInput=DLSSG and its own frame generation output: the calls never reach DLSS-G, which
//             stays off; GetState reports what the game asked for (20261023_221541, second run).
//   forward - a wrapper over a DLSS-G that runs: every call goes on to Streamline's plugin (first run).

#include <windows.h>

#include <atomic>
#include <cstring>

#include "sl.h"
#include "sl_dlss_g.h"
#include "tests/flow/fakes/fake_runtime_log.h"

namespace {

constexpr const char* kModule = "dlssg_api_tool";

PFun_slDLSSGSetOptions* g_streamlineSetOptions = nullptr;
PFun_slDLSSGGetState* g_streamlineGetState = nullptr;
std::atomic<uint32_t> g_mode{static_cast<uint32_t>(sl::DLSSGMode::eOff)};
std::atomic<uint32_t> g_framesToGenerate{1};

bool Forwards() {
    char mode[16] = {};
    GetEnvironmentVariableA("CE_FLOW_DLSSG_API_TOOL_MODE", mode, sizeof(mode));
    return std::strcmp(mode, "forward") == 0;
}

sl::Result ToolDLSSGSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options) {
    if (Forwards() && g_streamlineSetOptions)
        return g_streamlineSetOptions(viewport, options);
    ce::flow::fake::Log(kModule, "slDLSSGSetOptions answered by the tool: mode=%u frames=%u",
                        static_cast<uint32_t>(options.mode), options.numFramesToGenerate);
    g_mode.store(static_cast<uint32_t>(options.mode), std::memory_order_relaxed);
    g_framesToGenerate.store(options.numFramesToGenerate, std::memory_order_relaxed);
    return sl::Result::eOk;
}

sl::Result ToolDLSSGGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& state,
                             const sl::DLSSGOptions* options) {
    if (Forwards() && g_streamlineGetState)
        return g_streamlineGetState(viewport, state, options);
    state = {};
    state.status = sl::DLSSGStatus::eOk;
    state.minWidthOrHeight = 128;
    state.numFramesToGenerateMax = 3;
    state.numFramesActuallyPresented =
        g_mode.load(std::memory_order_relaxed) == static_cast<uint32_t>(sl::DLSSGMode::eOn)
            ? g_framesToGenerate.load(std::memory_order_relaxed) + 1
            : 1;
    return sl::Result::eOk;
}

}  // namespace

// The fake interposer's slGetFeatureFunction asks for the function the game gets instead of `streamlineFunction`.
extern "C" __declspec(dllexport) void* flowToolFeatureFunction(const char* name, void* streamlineFunction) {
    if (!name)
        return streamlineFunction;
    if (std::strcmp(name, "slDLSSGSetOptions") == 0) {
        g_streamlineSetOptions = reinterpret_cast<PFun_slDLSSGSetOptions*>(streamlineFunction);
        return reinterpret_cast<void*>(&ToolDLSSGSetOptions);
    }
    if (std::strcmp(name, "slDLSSGGetState") == 0) {
        g_streamlineGetState = reinterpret_cast<PFun_slDLSSGGetState*>(streamlineFunction);
        return reinterpret_cast<void*>(&ToolDLSSGGetState);
    }
    return streamlineFunction;
}
