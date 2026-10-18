#include "dxgi_limiter_stage.h"

#include "dxgi_shared_internal.h"
#include "common/logging/log_meter.h"

namespace DXGIShared {
namespace {

// One line per device that starts or stops receiving the submission, not per frame.
void LogSubmissionTarget(const void* swapChain, const void* device, const char* api) {
    static ce::log_meter::ChangeGate s_gate;
    if (const auto verdict = s_gate.Observe(ce::log_meter::FieldKey(device, api))) {
        HookLog("DXGI: FPS limiter submits the recorded frame before it waits (sc=%p device=%p api=%s)%s", swapChain,
                device, api, ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
}

}  // namespace

void SubmitD3D1xRecordedWork(void* swapChain) {
    auto* sc = static_cast<IDXGISwapChain*>(swapChain);
    if (!sc) {
        return;
    }
    ID3D11Device* device = nullptr;
    if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&device))) && device) {
        ID3D11DeviceContext* immediate = nullptr;
        device->GetImmediateContext(&immediate);
        if (immediate) {
            immediate->Flush();
            immediate->Release();
        }
        LogSubmissionTarget(sc, device, immediate ? "D3D11" : "D3D11 (no immediate context)");
        device->Release();
        return;
    }
    ID3D10Device* device10 = nullptr;
    if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&device10))) && device10) {
        device10->Flush();
        LogSubmissionTarget(sc, device10, "D3D10");
        device10->Release();
        return;
    }
    // A D3D12 swapchain's device is its command queue: the game submitted its lists itself.
    LogSubmissionTarget(sc, nullptr, "none (not D3D10/11)");
}

void ApplyFpsLimiterStage(IDXGISwapChain* pSwapChain, bool allowPostPresentReflexCadence,
                          ce::fps_limiter_policy::PresentSite site) {
    if (!g_IPC) {
        return;
    }
    const FpsLimiter::ScopedPreWaitSubmission submission({pSwapChain, &SubmitD3D1xRecordedWork});
    g_SharedFpsLimiter.SetIPCClient(g_IPC);
    g_SharedFpsLimiter.Apply(allowPostPresentReflexCadence, site);
    ApplyPresentFrameLatencyOverrides(pSwapChain);
}

}  // namespace DXGIShared
