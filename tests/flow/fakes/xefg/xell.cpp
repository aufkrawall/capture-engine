// Fake libxell.dll: the XeLL context XeFG requires (xefgSwapChainSetLatencyReduction), its sleep mode and its
// per-frame latency markers. It records and returns; CE only observes these exports.

#include <d3d12.h>

#include <atomic>
#include <cstdint>

#include "hook/xefg/xefg_api.h"
#include "tests/flow/fakes/fake_runtime_log.h"

using namespace ce::xell_api;

namespace {

constexpr const char* kModule = "libxell";

struct FakeXeLL {
    SleepParams sleep{};
    std::atomic<uint32_t> lastFrame{0};
};

FakeXeLL* ContextOf(Handle handle) {
    return reinterpret_cast<FakeXeLL*>(handle);
}

}  // namespace

extern "C" __declspec(dllexport) Result xellD3D12CreateContext(ID3D12Device* device, Handle* outContext) {
    if (!device || !outContext)
        return -1;
    *outContext = reinterpret_cast<Handle>(new FakeXeLL());
    ce::flow::fake::Log(kModule, "CreateContext -> %p", *outContext);
    return 0;
}

extern "C" __declspec(dllexport) Result xellDestroyContext(Handle context) {
    delete ContextOf(context);
    return 0;
}

extern "C" __declspec(dllexport) Result xellSetSleepMode(Handle context, const SleepParams* params) {
    if (!context || !params)
        return -1;
    ContextOf(context)->sleep = *params;
    ce::flow::fake::Log(kModule, "SetSleepMode minimumIntervalUs=%u lowLatency=%u", params->minimumIntervalUs,
                        static_cast<unsigned>(params->lowLatencyMode));
    return 0;
}

extern "C" __declspec(dllexport) Result xellGetSleepMode(Handle context, SleepParams* params) {
    if (!context || !params)
        return -1;
    *params = ContextOf(context)->sleep;
    return 0;
}

extern "C" __declspec(dllexport) Result xellSleep(Handle context, uint32_t frameId) {
    if (!context)
        return -1;
    ContextOf(context)->lastFrame.store(frameId, std::memory_order_relaxed);
    return 0;
}

extern "C" __declspec(dllexport) Result xellAddMarkerData(Handle context, uint32_t frameId, int32_t) {
    if (!context)
        return -1;
    ContextOf(context)->lastFrame.store(frameId, std::memory_order_relaxed);
    return 0;
}
