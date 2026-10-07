#pragma once

#include <dxgi1_2.h>

namespace DXGIShared {
// Owns the physical vtable claim and its independently retained predecessors.
// The caller retains the underlying swapchain throughout installation/calls.
bool InstallSwapchainPresentVTableHooks(IDXGISwapChain* swapchain);

// The adapter must preserve SDK/foreign caller provenance for frame routing.
const void* ResolvePresentDetourCaller(IDXGISwapChain* swapchain, const void* inlineCaller);
const void* ResolvePresent1DetourCaller(IDXGISwapChain* swapchain, const void* inlineCaller);
bool IsPresentDetourAddress(const void* entry);
bool IsPresent1DetourAddress(const void* entry);

// A vtable interception forwards its captured next link before any inline/body
// transport. While that link runs, reentrant inline calls keep their own route.
// Returns false outside the corresponding receiver/method's vtable call.
bool TryForwardPresentVTableCall(IDXGISwapChain* swapchain, UINT interval, UINT flags, HRESULT& result);
bool TryForwardPresent1VTableCall(IDXGISwapChain* swapchain, UINT interval, UINT flags,
                                  const DXGI_PRESENT_PARAMETERS* parameters, HRESULT& result);
}  // namespace DXGIShared
