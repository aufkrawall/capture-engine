#include "streamline_hook_internal.h"

#include "hook/fg/fg_presenter.h"
#include "hook/overlay/overlay_compat.h"

// The Streamline DLSS-G signal as a front (fg_presenter.h): the game calls slDLSSGSetOptions(on), but the DLSS-G API
// it was handed is served outside Streamline, or another frame-generation runtime presents every displayed frame.
// OptiScaler with FGInput=DLSSG strips kFeatureDLSS_G at slInit, returns its own slDLSSGSetOptions/GetState from
// slGetFeatureFunction and generates with XeFG or FSR FG instead; CE took the signal for DLSS-G, ran PostSL on the
// presents that had Streamline on their stack and published "DLSS FG 2x" (Talos + OptiScaler XeFG,
// 20261023_183119). Under a front CE's DLSS-G state stays off and the observation is only logged.

namespace DXGIShared {
bool IsStreamlineModuleHandle(HMODULE moduleHandle);
}

namespace {

HMODULE CaptureEngineModule() {
    static HMODULE s_module = [] {
        HMODULE module = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&CaptureEngineModule), &module);
        return module;
    }();
    return s_module;
}

std::atomic<bool> g_frontObservedActive{false};
std::atomic<int> g_frontObservedMultiplier{0};

// CE's DLSS-G state derived from an earlier, non-front signal is retired the way an accepted OFF retires it.
void RetireStreamlineFGStateForFront(const char* reason) {
    const bool wasRunning = DXGIShared::g_StreamlineFGRunning.exchange(false, std::memory_order_acq_rel);
    g_FGCompat.SetStreamlineFGSignal(false);
    if (!wasRunning) {
        return;
    }
    ApplyCombinedDLSSFGState(false, 0);
    ce::dx12_streamline_ui_overlay::EndActivation("Streamline FG signal is a front");
    DX12_OnStreamlineFGStateChanged(false);
    HookLogImportant("Streamline Hook: FG state transition ON->OFF - the Streamline FG signal is a front (%s)",
                     reason ? reason : "front");
}

}  // namespace

void NoteDLSSGApiProviderForTarget(const void* target) {
    if (!target) {
        return;
    }
    char modulePath[MAX_PATH] = {};
    HMODULE module = nullptr;
    const bool resolved = ce::overlay_compat::TryGetModulePathFromCodeAddress(target, modulePath, sizeof(modulePath),
                                                                              &module) &&
                          module != nullptr;
    const bool captureEngine = resolved && module == CaptureEngineModule();
    // A tool's proxy (OptiScaler's dxgi.dll) is never Streamline, even if it exports a Streamline-named function.
    const bool streamline = resolved && !captureEngine && !ce::overlay_compat::IsThirdPartyOverlayModulePath(modulePath) &&
                            (ce::overlay_compat::IsStreamlineFrameGenerationModulePath(modulePath) ||
                             DXGIShared::IsStreamlineModuleHandle(module));
    ce::fg_presenter::NoteDLSSGApiProvider(
        ce::fg_presenter::ClassifyDLSSGApiProvider(streamline, captureEngine, resolved), target, modulePath);
}

bool ApplyStreamlineFrontSignal(bool active, int multiplier, const char* source) {
    if (!ce::fg_presenter::IsStreamlineFGSignalFrontNow()) {
        return false;
    }
    g_frontObservedActive.store(active, std::memory_order_release);
    g_frontObservedMultiplier.store(active ? multiplier : 0, std::memory_order_release);
    static ce::log_meter::ChangeGate s_gate;
    const auto verdict = s_gate.Observe(ce::log_meter::FieldKey(active, active ? multiplier : 0));
    if (verdict) {
        HookLogImportant(
            "Streamline Hook: FG signal %s (%dx) via %s is a front - DLSS-G API served outside Streamline=%d, "
            "runtime-owned presenter=%d; CE's DLSS-G state stays off%s",
            active ? "ON" : "OFF", active ? multiplier : 0, source ? source : "runtime-state",
            ce::fg_presenter::IsDLSSGApiServedOutsideStreamline() ? 1 : 0,
            ce::fg_presenter::HasRuntimeOwnedSwapchains() ? 1 : 0,
            ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
    RetireStreamlineFGStateForFront(source);
    return true;
}

namespace StreamlineHook {

void ReconcileFrontSignal(const char* reason) {
    if (!ce::fg_presenter::IsStreamlineFGSignalFrontNow()) {
        return;
    }
    RetireStreamlineFGStateForFront(reason);
}

bool IsFGSignalFront() {
    return ce::fg_presenter::IsStreamlineFGSignalFrontNow();
}

}  // namespace StreamlineHook
