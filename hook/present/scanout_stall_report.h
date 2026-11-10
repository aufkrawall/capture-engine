#pragma once

#include <windows.h>

#include <cstdint>

struct IDXGISwapChain;

// The facts around a screen that stopped changing while the game kept presenting (judgement: scanout_stall_policy.h),
// and how long a new swapchain takes to reach the screen at all.
//
// GTA V Enhanced, session 20261110_182518: the new DLSS-G swapchain presented for 19.5 s and no frame reached the
// screen; the hook log could not say why. At the start of such an episode the report logs, once, what decides it:
//   - the swapchain (description, frame statistics, present count, fullscreen state, the output's hardware
//     composition support) and the results its Presents returned (DXGI answers an occluded window's Presents with a
//     status and drops them);
//   - the window (visible, iconic, cloaked, foreground, rectangle against its monitor, extended styles) and the
//     independent-flip blockers presentation_path_policy.h names for it;
//   - the visible top-level windows above the game that intersect it (class, executable, styles), at most 16.
// A running episode repeats a compact line at most every two seconds; its end line gives the duration, the presents
// made, what the display resumed with (a composed frame or the process's own flip) and whether the window facts
// changed. Lines carry the prefix `[SCANOUT STALL] ep=<n>`; episodes beyond the sixteenth are sampled.
//
// No window titles or contents are read or logged.

namespace ce::scanout_stall {

// A Present of `swapChain` returned `result`. Called on the presenting thread by the D3D12 routes' one result report:
// the hooked Present and Present1 of an unwrapped chain (NoteDX12PresentResultForVtablePath) and the swapchain
// wrapper's own Present and Present1. One try-lock and a few atomic reads unless an episode starts, continues or ends.
// Never delays the Present.
void OnPresentReturned(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags, HRESULT result);

// A fresh Streamline handoff created `swapChain`: logs `[SL STARTUP] handoff=<n> ... firstScreenChange` once, at the
// first change of the display after this call, with the presents the chain made before it.
void ArmFirstScreenChange(uint64_t handoff, IDXGISwapChain* swapChain);

}  // namespace ce::scanout_stall
