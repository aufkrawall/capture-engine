// Fake sl.reflex.dll: the Reflex plugin a DLSS-G game calls every frame (slReflexSleep) and on mode changes
// (slReflexSetOptions). CE hooks both; the fake only records the mode.

#define SL_INTERPOSER

#include <atomic>
#include <cstring>

#include "sl.h"
#include "sl_reflex.h"
#include "tests/flow/fakes/fake_runtime_log.h"

namespace {

std::atomic<uint32_t> g_mode{0};
std::atomic<uint32_t> g_sleeps{0};

sl::Result ReflexSetOptions(const sl::ReflexOptions& options) {
    ce::flow::fake::Log("sl.reflex", "slReflexSetOptions mode=%u (was %u)", static_cast<uint32_t>(options.mode),
                        g_mode.exchange(static_cast<uint32_t>(options.mode)));
    return sl::Result::eOk;
}

sl::Result ReflexSleep(const sl::FrameToken& frame) {
    (void)frame;
    g_sleeps.fetch_add(1, std::memory_order_relaxed);
    return sl::Result::eOk;
}

sl::Result ReflexGetState(sl::ReflexState& state) {
    state.lowLatencyAvailable = true;
    state.latencyReportAvailable = false;
    state.flashIndicatorDriverControlled = false;
    return sl::Result::eOk;
}

}  // namespace

extern "C" __declspec(dllexport) void* slGetPluginFunction(const char* name) {
    if (!name)
        return nullptr;
    if (std::strcmp(name, "slReflexSetOptions") == 0)
        return reinterpret_cast<void*>(&ReflexSetOptions);
    if (std::strcmp(name, "slReflexSleep") == 0)
        return reinterpret_cast<void*>(&ReflexSleep);
    if (std::strcmp(name, "slReflexGetState") == 0)
        return reinterpret_cast<void*>(&ReflexGetState);
    return nullptr;
}
