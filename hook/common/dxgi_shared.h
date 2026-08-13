#pragma once

// Shared DXGI present/swapchain state and the Post-SL overlay callback contract.
//
// This file is the umbrella; the topic headers below hold the declarations.

#include "dxgi_shared_detail/types_and_state.h"
#include "dxgi_shared_detail/streamline_present_routing.h"

namespace DXGIShared {
// Evidence-based Vulkan renderer state, published by CheckAndInstallHooks
// (hook/main_install.cpp) and consulted by the DXGI present/resize paths.
// True only when Vulkan actually owns rendering (VK_LAYER_CE_overlay active,
// or vulkan-1.dll loaded without any D3D usage evidence).  A DX12 UE5 process
// that merely loads vulkan-1.dll as a transitive dependency must NOT disable
// the DXGI present/overlay path.
void SetVulkanActiveForDXGIPresentPath(bool active);
bool IsVulkanActive();
}

namespace DXGIShared {
// True while CE's Present view is the deep body hook installed below a foreign overlay chain
// (dxgi!Present / dxgi!Present1, past the five entry bytes the foreign overlays keep restoring
// and re-patching). In that state CE runs after every foreign overlay that patches the entry,
// so its overlay composite is the topmost layer — and below the chain the immediate caller of
// dxgi!Present proves nothing about who originated the present. Declared here for the api-layer
// overlay routes; the definition lives with the shared Present state (dxgi_shared.cpp).
bool IsPresentInterceptedBelowForeignChain();
}

namespace DXGIShared {
// Wrapper-mode PostSL service: invoked from CWrapDXGISwapChain::Present when the wrapper is
// the Streamline-runtime (non-retaining) one and CE left the Present entry to the foreign
// chain. Mirrors the entry-hook routing's confirmed-standalone Streamline Present invocation
// so the overlay is drawn after SL's FG processing (ProcessFrame suppresses its own pre-SL
// draw exactly while PostSL is expected to draw).
void MaybeInvokePostSLOverlayRenderFromWrappedRuntimePresent(IDXGISwapChain* pSwapChain, const char* source);
}

namespace DXGIShared {
// Called after CE wrapped the FG runtime's own swapchain (Streamline runtime create). When two
// or more foreign overlays share the Present entry, this removes CE's entry prepend
// (ownership-checked) and publishes the leave-entry state, so the foreign chain is byte-identical
// to a process without CE and the wrapper is CE's interception for runtime presents too.
// `pRealSwapChain` is the unwrapped real swapchain the wrapper now forwards for.
void MaybeTransitionPresentEntryToForeignChainForWrappedRuntimeSwapchain(IDXGISwapChain* pRealSwapChain,
                                                                         const char* source);
}
