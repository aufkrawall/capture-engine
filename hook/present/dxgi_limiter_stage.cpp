#include "dxgi_limiter_stage.h"

#include "dxgi_shared_internal.h"
#include "common/logging/log_meter.h"
#include "hook/pacing/streamline_output_site.h"

namespace DXGIShared {
namespace {

ce::streamline_output_site::ApplicationPacedSwapchains s_applicationPacedSwapchains;

// Only Streamline's outputs consult the record, so other games never write it. One line per swapchain.
void RecordApplicationPresentPaced(IDXGISwapChain* pSwapChain, const char* site) {
    if (!IsSLInterposerLoaded() || !s_applicationPacedSwapchains.Note(pSwapChain)) {
        return;
    }
    HookLogImportant(
        "DXGI limiter: sc=%p is also paced at %s (tid=0x%04X); Streamline outputs on it stay application-present "
        "evaluations instead of a final-output boundary",
        (void*)pSwapChain, site, GetCurrentThreadId());
}

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

bool IsStreamlineFinalOutputForLimiter(IDXGISwapChain* pSwapChain, bool isD3D12SwapChain, bool streamlineFGRunning,
                                       bool callerFromStreamlineModule, bool callerFromFFXFrameGenerationModule,
                                       bool callbackProvenRuntimeOutput) {
    using namespace ce::streamline_output_site;
    if (IsApplicationPresent(callerFromStreamlineModule, callerFromFFXFrameGenerationModule,
                             callbackProvenRuntimeOutput)) {
        RecordApplicationPresentPaced(pSwapChain, "the application's own Present");
        return false;
    }
    const bool streamlineOutput = streamlineFGRunning && callerFromStreamlineModule;
    const bool applicationPaced = streamlineOutput && s_applicationPacedSwapchains.Contains(pSwapChain);
    const bool boundary =
        IsFinalOutputBoundary(isD3D12SwapChain, streamlineFGRunning, callerFromStreamlineModule, applicationPaced);
    if (streamlineOutput) {
        static ce::log_meter::ChangeGate s_gate;
        if (const auto verdict = s_gate.Observe(ce::log_meter::FieldKey(pSwapChain, boundary, applicationPaced))) {
            HookLogImportant(
                "DXGI limiter: Streamline outputs on sc=%p are %s (applicationPaced=%d d3d12=%d tid=0x%04X)%s",
                (void*)pSwapChain, boundary ? "a final-output boundary" : "application-present evaluations",
                applicationPaced ? 1 : 0, isD3D12SwapChain ? 1 : 0, GetCurrentThreadId(),
                ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
    }
    return boundary;
}

void NoteApplicationPresentPacedOnSwapchain(IDXGISwapChain* pSwapChain, const char* site) {
    RecordApplicationPresentPaced(pSwapChain, site);
}

}  // namespace DXGIShared
