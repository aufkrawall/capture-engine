#pragma once

// ce::fg_presenter - which component presents the displayed frames on each native swapchain.
//
// CE's FG routing used to derive the active runtime from API signals alone (slDLSSGSetOptions, ffxConfigure,
// NvPresent cadence). OptiScaler shows that such a signal can be a front for another generator: with
// FGInput=DLSSG and FGOutput=XeFG the game calls slDLSSGSetOptions(on) while XeSS frame generation presents every
// displayed frame and DLSS-G never runs (20261023_183119). A frame-generation runtime that creates the native
// swapchain it presents is that swapchain's presenter for its whole lifetime, whatever a Present's stack shows;
// API signals only say which frames it generates. This unit records that ownership (creator attribution at the
// create, the queue the presenter submits on) and answers it on the Present path from one atomic load when no
// runtime owns a swapchain.

#include <windows.h>

#include <cstdint>

#include "fg_runtime_state.h"
#include "hook/present/present_interposer_cadence.h"

struct IDXGISwapChain;
struct ID3D12CommandQueue;

namespace ce::fg_presenter {

enum class Presenter : uint8_t {
    kApplication = 0,     // no runtime: the application's own swapchain (the default for anything unregistered)
    kStreamlineDLSSG,     // Streamline's DLSS-G presents the outputs
    kFFXSwapchain,        // AMD's FidelityFX frame-generation swapchain
    kXeFGSwapchain,       // Intel's XeSS frame-generation swapchain
    kPresentInterposer,   // NVIDIA Smooth Motion's private output chain
    kUnknownExternal,     // a runtime CE cannot name owns it
};

const char* PresenterName(Presenter presenter);

// --- Pure policies --------------------------------------------------------------------------------------------

// The presenter of a swapchain created now. A runtime's own create scope for the window (opened by CE's detour of
// the runtime's swapchain-init export) is authoritative; the runtime's module on the create stack is the fallback
// for a create CE saw without the scope (a late injection). An FFX frame-generation runtime creating the swapchain
// makes it the FFX swapchain's: CE's FFX routes draw there, and nothing may treat it as the game's. The DLSS-G plugin
// itself on the create stack (ce::streamline_lifecycle::IsDlssgCreatingSwapchain, not Streamline's interposer frames)
// makes it DLSS-G's.
inline Presenter ResolveCreatedSwapchainPresenter(Presenter openRuntimeCreateScope, bool xefgModuleOnCreateStack,
                                                  bool ffxRuntimeCreator = false, bool dlssgPluginCreator = false) {
    if (openRuntimeCreateScope != Presenter::kApplication) {
        return openRuntimeCreateScope;
    }
    if (xefgModuleOnCreateStack) {
        return Presenter::kXeFGSwapchain;
    }
    if (ffxRuntimeCreator) {
        return Presenter::kFFXSwapchain;
    }
    return dlssgPluginCreator ? Presenter::kStreamlineDLSSG : Presenter::kApplication;
}

// A runtime other than DLSS-G that presents a swapchain of its own: while it holds the newest registered chain,
// DLSS-G presents nothing CE could draw on, whatever the DLSS-G API reports.
inline bool IsCompetingPresenter(Presenter presenter) {
    return presenter != Presenter::kApplication && presenter != Presenter::kStreamlineDLSSG;
}

// The newest competing registration holds presentation unless DLSS-G created a chain after it (lifetimes count every
// registration, so a DLSS-G chain at a reused address is still newer).
inline bool DoesCompetingPresenterHoldPresentation(uint64_t newestCompetingLifetime, uint64_t newestDLSSGLifetime) {
    return newestCompetingLifetime != 0 && newestCompetingLifetime > newestDLSSGLifetime;
}

// Streamline's overlay route (PostSL) draws on the chain Streamline hands its present callback; it never may on a chain
// another runtime presents (GTA V Enhanced 20261110_222057: PostSL on FidelityFX's chain removed the device).
inline bool MayStreamlineRouteDrawOn(Presenter presenter) {
    return !IsCompetingPresenter(presenter);
}

// A DLSS-G state observation (slDLSSGGetState's result) belongs to the presenter registrations it started under: one
// that returned after a swapchain registration changed straddled a handoff and proves nothing about the new chain.
inline bool DidObservationStraddleHandoff(uint64_t registrationEpochAtEntry, uint64_t registrationEpochAtReturn) {
    return registrationEpochAtEntry != registrationEpochAtReturn;
}

// A presenter whose swapchain CE composites on directly, on the presenter's own queue, after it generated the
// frame: the overlay lands on every displayed frame and is never interpolated.
inline bool IsRuntimeOwnedOutputPresenter(Presenter presenter) {
    return presenter == Presenter::kXeFGSwapchain;
}

// The Streamline DLSS-G signal is a front - the game's DLSS-G calls reach no DLSS-G - when another frame-generation
// runtime owns the presenting swapchain, or when the DLSS-G API the game was handed is served outside Streamline
// and Streamline presents nothing. A tool serving the API proves no front alone: OptiScaler wraps the slDLSSGSetOptions
// it hands out also when it forwards to the real DLSS-G, and then Streamline's sl.dlss_g presents every frame
// (20261023_221541, first run: DLSS-G 3x, every present from sl.dlss_g.dll, and CE kept DLSS FG off and drew no
// overlay). With FGOutput=FSRFG or XeFG it answers the calls itself and FFX or XeFG presents instead (second run).
// A front must never drive PostSL routing, DLSS-G status or a DLSS-G takeover of another runtime's startup.
// A competing runtime holding presentation (DoesCompetingPresenterHoldPresentation) and a DLSS-G Streamline unloaded
// (the forwarded slSetFeatureLoaded(DLSS-G, false), not the answer CE gave the title while it held the unload) are
// fronts too: GTA V Enhanced kept passing its last DLSS-G options, mode on, to slDLSSGGetState after it unloaded DLSS-G
// and FidelityFX had created the window's chain, and CE took that for DLSS-G (20261110_222057).
inline bool IsStreamlineFGSignalFront(bool dlssgApiServedOutsideStreamline, bool streamlinePresents,
                                      bool runtimeOwnedOutputPresenterActive,
                                      bool competingPresenterHoldsPresentation = false, bool dlssgUnloaded = false) {
    return runtimeOwnedOutputPresenterActive || competingPresenterHoldsPresentation || dlssgUnloaded ||
           (dlssgApiServedOutsideStreamline && !streamlinePresents);
}

// Streamline presents the frames while one of the last kStreamlinePresentWindow presents came from Streamline's
// code (ce::present_origin's originator): counted in presents, never on a clock, so a pause proves nothing either way.
inline constexpr uint64_t kStreamlinePresentWindow = 240;
inline bool IsStreamlinePresenting(uint64_t presents, uint64_t lastStreamlinePresent) {
    return lastStreamlinePresent != 0 && presents >= lastStreamlinePresent &&
           presents - lastStreamlinePresent < kStreamlinePresentWindow;
}

// Another frame-generation runtime creating the window's swapchain is an authoritative handoff: Streamline's earlier
// presents were on the swapchain it replaced, so they prove nothing about the new one. Streamline proves itself again
// with its next present. Without this, a tool's DLSS-G inputs during the first window of a DLSS-G -> FSR switch read
// as a running DLSS-G and tore down the FSR route.
inline bool DoesCreatedSwapchainRestartStreamlineEvidence(Presenter createdPresenter) {
    return createdPresenter != Presenter::kApplication && createdPresenter != Presenter::kStreamlineDLSSG;
}

// The DLSS-G API's provider. CE's own returned-pointer wrappers and SL1->SL2 bridge are no provider of their own.
enum class DLSSGApiProvider : uint8_t { kUnknown, kStreamline, kCaptureEngine, kForeign };
inline DLSSGApiProvider ClassifyDLSSGApiProvider(bool streamlineModule, bool captureEngineModule, bool moduleResolved) {
    if (!moduleResolved) {
        return DLSSGApiProvider::kUnknown;
    }
    if (captureEngineModule) {
        return DLSSGApiProvider::kCaptureEngine;
    }
    return streamlineModule ? DLSSGApiProvider::kStreamline : DLSSGApiProvider::kForeign;
}

// An FG mode change that only toggles generation on a presenter-owned swapchain (XeFG presents in both states):
// presentation does not move, so no draw cooldown may blank the overlay at the edge.
inline bool IsPresenterOwnedRuntimeModeToggle(fg_runtime::RuntimeMode previous, fg_runtime::RuntimeMode next,
                                              bool runtimeOwnedOutputPresenterActive) {
    if (!runtimeOwnedOutputPresenterActive || previous == next) {
        return false;
    }
    const auto presenterSide = [](fg_runtime::RuntimeMode mode) {
        return mode == fg_runtime::RuntimeMode::kXeFG || mode == fg_runtime::RuntimeMode::kOff ||
               mode == fg_runtime::RuntimeMode::kStreamlineNoFG;
    };
    return (previous == fg_runtime::RuntimeMode::kXeFG || next == fg_runtime::RuntimeMode::kXeFG) &&
           presenterSide(previous) && presenterSide(next);
}

// --- Registry ---------------------------------------------------------------------------------------------------

// A runtime's swapchain-init call in progress for `window` (process-wide: the runtime may create on any thread).
// While open, CE's swapchain-create hooks attribute a create for that window to `presenter` and leave its
// descriptor untouched. Scopes do not nest across runtimes; the innermost one wins.
class ScopedRuntimeSwapchainCreate {
public:
    ScopedRuntimeSwapchainCreate(Presenter presenter, HWND window, uintptr_t owner);
    ~ScopedRuntimeSwapchainCreate();
    ScopedRuntimeSwapchainCreate(const ScopedRuntimeSwapchainCreate&) = delete;
    ScopedRuntimeSwapchainCreate& operator=(const ScopedRuntimeSwapchainCreate&) = delete;
    // The swapchain a create inside this scope produced, or null.
    IDXGISwapChain* CreatedSwapchain() const;

private:
    Presenter previousPresenter_;
    HWND previousWindow_;
    uintptr_t previousOwner_;
    IDXGISwapChain* previousCreated_;
};

// The presenter whose create scope is open for `window` (any window when the scope named none), or kApplication.
Presenter OpenRuntimeCreateScopeFor(HWND window);

// A swapchain create returned `swapchain` for `window` on `queue` (the D3D12 create "device"). Attributes it
// (ResolveCreatedSwapchainPresenter) and registers a runtime-owned one; any other swapchain registered for the
// same window is gone (DXGI allows one flip swapchain per window) and is retired. Returns the presenter.
// `creatorAddress`: the create's originator frame on CE's create stack (the code that called the native create).
Presenter NoteSwapchainCreated(IDXGISwapChain* swapchain, HWND window, ID3D12CommandQueue* queue,
                               bool xefgModuleOnCreateStack, const char* context, bool ffxRuntimeCreator = false,
                               bool dlssgPluginCreator = false, const void* creatorAddress = nullptr);

// The queues a ResizeBuffers1 bound the swapchain's buffers to: the presenter's output queue from now on.
void NoteSwapchainPresentQueue(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue, const char* context);

// The runtime that created the swapchains registered under `owner` destroyed its context.
void RetireOwner(uintptr_t owner, const char* reason);

// Present-path queries; one acquire load each when nothing of the kind is registered.
bool HasRuntimeOwnedSwapchains();
// A swapchain whose presenter is a runtime-owned output presenter (IsRuntimeOwnedOutputPresenter) is registered.
bool HasRuntimeOwnedOutputPresenter();
Presenter PresenterFor(IDXGISwapChain* swapchain);
ID3D12CommandQueue* OutputQueue(IDXGISwapChain* swapchain);  // raw identity; null when unknown
// The registration's lifetime number of a runtime-owned output presenter's swapchain (1, 2, ...; 0 for any other
// swapchain). A new registration at a reused address gets a new number: a creation-time proof of a new swapchain
// where pointer identity cannot tell.
uint64_t OutputPresenterLifetime(IDXGISwapChain* swapchain);

// The registration a swapchain is presented under: its presenter, lifetime number, create queue (identity only) and
// the module that called the native create. That module can differ from the presenter's: GTA V Enhanced creates
// FidelityFX's chain through Streamline, whose sl.dlss_g.dll creates the native swapchain and presents it from inside
// AMD's Present (session 20261112_013802).
struct Binding {
    Presenter presenter = Presenter::kApplication;
    uint64_t lifetime = 0;
    ID3D12CommandQueue* queue = nullptr;
    HMODULE creator = nullptr;  // identity only, null when the create stack named no module
};
Binding BindingFor(IDXGISwapChain* swapchain);

// A prewarmed disabled Streamline chain may use its create queue for normal rendering. Admission is published
// only after a real submission on that exact registration; replacement and queue rebinding retire it.
using NormalRenderPromotion = bool (*)();
bool ArmStreamlineNormalRender(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue,
                              NormalRenderPromotion mayArm);
uint64_t StreamlineNormalRenderToken(IDXGISwapChain* swapchain, bool requireSubmitted);
// A live Streamline chain whose admission has a real submission. Promotion retires the startup-handoff flag, so
// this is the lasting evidence of that handoff until FG activation retires it with Streamline FG running.
bool HasSubmittedStreamlineNormalRender();
bool PromoteStreamlineNormalRender(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue, uint64_t token,
                                  NormalRenderPromotion commitTransition);
void InvalidateStreamlineNormalRender(const char* reason);
void InvalidateStreamlineNormalRenderForSwapchain(IDXGISwapChain* swapchain, const char* reason);
// The competing presenter that holds presentation (DoesCompetingPresenterHoldPresentation), or kApplication.
Binding CompetingPresenterHoldingPresentation();
// Counts every registration change (create, owner retirement, DLSS-G load state): DidObservationStraddleHandoff.
uint64_t RegistrationEpoch();

// Streamline's answer to a forwarded slSetFeatureLoaded(DLSS-G, loaded) the title or CE's unload order sent.
void NoteDLSSGFeatureLoaded(bool loaded, const char* context);
bool IsDLSSGUnloaded();

// Every Present's originator (ce::present_origin): the evidence IsStreamlinePresenting counts. NoteSwapchainCreated
// restarts it (DoesCreatedSwapchainRestartStreamlineEvidence).
void NotePresentOriginator(bool streamline);
bool IsStreamlinePresentingNow();

// The presenter's two streams: every output Present on a runtime-owned output swapchain, against the application's
// frames (XeFG: xefgSwapChainSetPresentId). Both streams publish closed measurement windows to g_FGCompat in order
// (present_interposer_cadence.h: one second, 20 application frames at least). Outputs also open a window without any
// present IDs and close it when the application's frames stopped. Reset and publication share the same lifetime lock.
// A presenting/generation edge starts a fresh window atomically with the configured XeFG state.
void SetRuntimeGenerationState(bool presenterActive, bool enabled, int interpolatedFrames);
void NoteRuntimeOutputPresent(int64_t nowUs);
void NoteRuntimeApplicationFrame(int64_t nowUs);
void ResetRuntimeCadence();
// The runtime's input boundary (XeFG: SetPresentId) reports application frames: the application frame rate is
// measured there, not at the outputs. It lapses when those frames stop; each output is the application's frame then.
bool HasRuntimeInputBoundary();

// DLSS-G API provider, recorded when CE resolves slDLSSGSetOptions/slDLSSGGetState.
void NoteDLSSGApiProvider(DLSSGApiProvider provider, const void* target, const char* modulePath);
bool IsDLSSGApiServedOutsideStreamline();
// IsStreamlineFGSignalFront over the live state.
bool IsStreamlineFGSignalFrontNow();

// For the flow harness and unit tests.
void ResetForTesting();

}  // namespace ce::fg_presenter
