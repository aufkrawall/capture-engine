#pragma once

// Swapchains a frame-generation runtime creates and presents itself (Intel XeSS frame generation): the D3D12 side
// of ce::fg_presenter's creator attribution.

#include <windows.h>

struct IDXGISwapChain;
struct IUnknown;

// From the CreateSwapChain* hooks after a successful create: registers a swapchain a runtime created inside its
// init scope (or with its module on the create stack). True when the runtime presents it; the hook then hands it
// back unwrapped.
bool DX12_NoteFrameGenerationRuntimeSwapchainCreate(const char* context, IDXGISwapChain* swapchain, IUnknown* device,
                                                    HWND window, bool xefgModuleOnCreateStack);

// The application swapchain a runtime is about to take over: xefgSwapChainD3D12InitFromSwapChain releases it,
// requires that release to be the last reference, and creates its own swapchain for the window. CE drops every
// reference it holds to the swapchain and its buffers first (the overlay's render targets, the capture copy
// source, the tracking maps), exactly as before a ResizeBuffers.
void DX12_ReleaseSwapchainForRuntimeTakeover(IDXGISwapChain* swapchain, const char* reason);
