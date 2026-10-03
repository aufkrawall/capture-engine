// Fake sl.pcl.dll: the PC Latency plugin whose frame markers a DLSS-G game sets every frame
// (slPCLSetMarker). CE hooks it: the PresentStart marker is its title-thread frame clock.

#define SL_INTERPOSER

#include <atomic>
#include <cstring>

#include "sl.h"
#include "sl_pcl.h"
#include "tests/flow/fakes/fake_runtime_log.h"

namespace {

std::atomic<uint32_t> g_markers{0};

sl::Result PCLSetMarker(sl::PCLMarker marker, const sl::FrameToken& frame) {
    if (g_markers.fetch_add(1, std::memory_order_relaxed) == 0) {
        ce::flow::fake::Log("sl.pcl", "first slPCLSetMarker marker=%u frame=%u", static_cast<uint32_t>(marker),
                            static_cast<uint32_t>(frame));
    }
    return sl::Result::eOk;
}

sl::Result PCLGetState(sl::PCLState& state) {
    state.statsWindowMessage = 0;
    return sl::Result::eOk;
}

sl::Result PCLSetOptions(const sl::PCLOptions&) {
    return sl::Result::eOk;
}

}  // namespace

extern "C" __declspec(dllexport) void* slGetPluginFunction(const char* name) {
    if (!name)
        return nullptr;
    if (std::strcmp(name, "slPCLSetMarker") == 0)
        return reinterpret_cast<void*>(&PCLSetMarker);
    if (std::strcmp(name, "slPCLGetState") == 0)
        return reinterpret_cast<void*>(&PCLGetState);
    if (std::strcmp(name, "slPCLSetOptions") == 0)
        return reinterpret_cast<void*>(&PCLSetOptions);
    return nullptr;
}
