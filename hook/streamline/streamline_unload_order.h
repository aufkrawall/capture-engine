#pragma once

#include <windows.h>

struct IDXGISwapChain;

// Streamline's own order at a DLSS-G unload, restored for titles that unload before they release (policy and the
// reason: streamline_unload_order_policy.h). This module feeds the policy with what CE observes - the lifecycle calls,
// the DLSS-G chain, the plugin's destroy dispatch, DXGI's native Release - and carries out its decisions. It never
// frees or releases anything of DLSS-G's: the held unload is forwarded to Streamline, which dispatches the destroy
// itself.
//
// Log lines: `[SL UNLOAD ORDER] deferred|forwarded|cancelled|not-deferred|dropped ...`, one per transition.

namespace ce::streamline_unload_order {

struct Callbacks {
    // slSetFeatureLoaded(DLSS-G, false) to Streamline, through the observed lifecycle path.
    void (*forwardHeldUnload)() = nullptr;
    // Whether CE's hook on the DLSS-G plugin's slHookSwapChainDestroyed is installed.
    bool (*destroyHookInstalled)() = nullptr;
};
void SetCallbacks(const Callbacks& callbacks);

// Proven DLSS-G-plugin creation on a D3D12 queue, captured before forwarding the create. Interposer/common/SR
// stack frames alone do not qualify. Installs DXGI's Release observation, idle until an unload is held.
void NoteDlssgChain(IDXGISwapChain* chain);

enum class SetLoadedAction {
    kForward,   // Streamline gets the call as the title made it
    kAnswerOk,  // answered here: the unload is held (or a held one was withdrawn); Streamline is not called
};
SetLoadedAction OnSetFeatureLoaded(bool loaded);

// slIsFeatureLoaded(DLSS-G): true = the unload is held, so the title is answered "not loaded".
bool AnswerNotLoaded();

// The DLSS-G plugin's destroy hook is about to run for `chain`: a release inside it never ends the hold.
void NoteDestroyEntered(IDXGISwapChain* chain);

// The DLSS-G plugin's destroy hook returned for `chain`. Never forwards from inside the hook: it runs inside
// Streamline's loop over the hook list that an unload clears.
void NoteDestroyReturned(IDXGISwapChain* chain);

// A present on a native swapchain: the title still uses a held chain. One compare while nothing is held.
void NoteNativePresent(IDXGISwapChain* chain);

// The Streamline interposer left the process: a held unload has nothing to go to.
void OnInterposerUnloaded();

}  // namespace ce::streamline_unload_order
