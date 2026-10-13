// Fake sl.reflex.dll: the Reflex plugin a DLSS-G game calls every frame (slReflexSleep) and on mode changes
// (slReflexSetOptions). CE hooks both; the fake only records the mode.

#define SL_INTERPOSER

#include <atomic>
#include <cstring>
#include <dxgi1_6.h>

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

// Loaded under an overlay module name by coexistence scenarios. Keep the return
// frame inside the DLL so CE observes a foreign follower rather than a tail call.
extern "C" __declspec(dllexport) HRESULT CEFake_ForwardPresent(IDXGISwapChain* swapchain, bool present1) {
    HRESULT result;
    if (present1) {
        IDXGISwapChain1* chain = nullptr;
        result = swapchain->QueryInterface(IID_PPV_ARGS(&chain));
        if (FAILED(result)) return result;
        DXGI_PRESENT_PARAMETERS parameters{};
        result = chain->Present1(0, 0, &parameters);
        chain->Release();
    } else {
        result = swapchain->Present(0, 0);
    }
    volatile HRESULT completed = result;
    return completed;
}

extern "C" __declspec(dllexport) void* CEFake_ResolveD3D11(HMODULE runtime) {
    return reinterpret_cast<void*>(GetProcAddress(runtime, "D3D11CreateDevice"));
}

extern "C" FARPROC (WINAPI* __imp_GetProcAddress)(HMODULE, LPCSTR);
extern "C" __declspec(dllexport) void* CEFake_ExchangeResolver(void* replacement) {
    auto* slot = reinterpret_cast<void**>(&__imp_GetProcAddress);
    DWORD protection = 0;
    if (!VirtualProtect(static_cast<void*>(slot), sizeof(*slot), PAGE_READWRITE, &protection)) return nullptr;
    void* previous = InterlockedExchangePointer(slot, replacement);
    DWORD ignored = 0;
    VirtualProtect(static_cast<void*>(slot), sizeof(*slot), protection, &ignored);
    return previous;
}

#include "late_present_entry.h"
