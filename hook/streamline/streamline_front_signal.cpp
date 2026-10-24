#include "streamline_hook_internal.h"

#include "hook/fg/fg_presenter.h"
#include "hook/overlay/overlay_compat.h"

// The Streamline DLSS-G signal as a front (fg_presenter.h): the game calls slDLSSGSetOptions(on), but the DLSS-G API
// it was handed is served outside Streamline, or another frame-generation runtime presents every displayed frame.
// OptiScaler with FGInput=DLSSG strips kFeatureDLSS_G at slInit, returns its own slDLSSGSetOptions/GetState from
// slGetFeatureFunction and generates with XeFG or FSR FG instead; CE took the signal for DLSS-G, ran PostSL on the
// presents that had Streamline on their stack and published "DLSS FG 2x" (Talos + OptiScaler XeFG,
// 20261023_183119). OptiScaler also wraps the API of a DLSS-G that does run; then Streamline presents and the
// signal is real (ce::fg_presenter::IsStreamlineFGSignalFront). Under a front CE's DLSS-G state stays off and the
// observation is only logged.

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

// The front verdict CE's DLSS-G state was last derived under: -1 before the first runtime state, then 0 or 1. The
// verdict also moves without a DLSS-G call (Streamline starts or stops presenting, a runtime takes the swapchain);
// ReconcileFrontSignal re-derives the state when it no longer matches.
std::atomic<int> g_appliedFrontVerdict{-1};
// A front absorbed the game's explicit slDLSSGSetOptions enable: its provenance is replayed when the front ends.
std::atomic<bool> g_frontHeldExplicitEnable{false};

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

bool ApplyStreamlineFrontSignal(bool active, int multiplier, bool explicitSetOptionsEnableSignal, const char* source) {
    const bool front = ce::fg_presenter::IsStreamlineFGSignalFrontNow();
    g_appliedFrontVerdict.store(front ? 1 : 0, std::memory_order_release);
    if (!front) {
        g_frontHeldExplicitEnable.store(false, std::memory_order_release);
        return false;
    }
    if (explicitSetOptionsEnableSignal || !active) {
        g_frontHeldExplicitEnable.store(explicitSetOptionsEnableSignal, std::memory_order_release);
    }
    static ce::log_meter::ChangeGate s_gate;
    const auto verdict = s_gate.Observe(ce::log_meter::FieldKey(active, active ? multiplier : 0));
    if (verdict) {
        HookLogImportant(
            "Streamline Hook: FG signal %s (%dx) via %s is a front - DLSS-G API served outside Streamline=%d, "
            "Streamline presents=%d, runtime-owned presenter=%d; CE's DLSS-G state stays off%s",
            active ? "ON" : "OFF", active ? multiplier : 0, source ? source : "runtime-state",
            ce::fg_presenter::IsDLSSGApiServedOutsideStreamline() ? 1 : 0,
            ce::fg_presenter::IsStreamlinePresentingNow() ? 1 : 0,
            ce::fg_presenter::HasRuntimeOwnedOutputPresenter() ? 1 : 0,
            ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
    RetireStreamlineFGStateForFront(source);
    return true;
}

namespace StreamlineHook {

void ReconcileFrontSignal(const char* reason) {
    const int front = ce::fg_presenter::IsStreamlineFGSignalFrontNow() ? 1 : 0;
    const int applied = g_appliedFrontVerdict.load(std::memory_order_acquire);
    // Nothing derived yet and nothing to retire: the game's first DLSS-G call applies the verdict itself.
    if (applied == front || (applied < 0 && !front) || HookIsShuttingDown()) {
        return;
    }
    // The game's latest request, through the one gate: a front retires CE's DLSS-G state, a real signal applies it
    // as an enable would have (a wrapped DLSS-G enabled before Streamline's first present stayed OFF otherwise).
    int multiplier = 0;
    const bool active = GetCombinedViewportRuntimeState(&multiplier);
    const bool explicitEnable =
        !front && active && g_frontHeldExplicitEnable.exchange(false, std::memory_order_acq_rel);
    static ce::log_meter::ChangeGate s_gate;
    if (const auto verdict = s_gate.Observe(ce::log_meter::FieldKey(front, active, multiplier))) {
        HookLogImportant(
            "Streamline Hook: Streamline FG signal %s a front (%s) - re-deriving CE's DLSS-G state from the game's "
            "latest request (%s %dx explicitEnable=%d)%s",
            front ? "became" : "is no longer", reason ? reason : "reconcile", active ? "ON" : "OFF",
            active ? multiplier : 0, explicitEnable ? 1 : 0, ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
    ApplyCombinedStreamlineRuntimeState(active, multiplier, explicitEnable, reason);
}

bool IsFGSignalFront() {
    return ce::fg_presenter::IsStreamlineFGSignalFrontNow();
}

}  // namespace StreamlineHook
