#pragma once

// When CE's own substitute UI texture must be cleared before a proxy-backbuffer
// overlay route draws.
//
// AMD composites the registered UI resource in BOTH present paths: with frame
// generation enabled from the presenter thread, and with it disabled in the
// game-thread no-interpolation path, which copies the proxy backbuffer and then
// runs presentWithUiComposition (FidelityFX SDK FrameInterpolationSwapChainDX12,
// Present -> presentWithUiComposition). The UI texture is therefore blended
// over whatever CE drew on the proxy backbuffer.
//
// A game-owned UI texture is rewritten by its producer every frame, so CE's
// pixels from the active-FG route vanish on their own. CE's substitute has no
// producer: only the UI-resource route writes it. When FSR FG suspends (GTA V
// Enhanced's Alt+F4 quit dialog, session 20261018_210920) the overlay moves to
// the proxy backbuffer and the substitute keeps its last overlay frame, which
// AMD keeps compositing on top: a frozen ghost over the live overlay.
namespace ce::dx12_overlay_policy {

inline bool ShouldRetireCESubstituteBeforeProxyBackbufferRoute(bool proxyBackbufferRoute,
                                                               bool registeredUiIsCESubstitute,
                                                               bool substituteAlreadyRetiredForRoute) {
    return proxyBackbufferRoute && registeredUiIsCESubstitute && !substituteAlreadyRetiredForRoute;
}

}  // namespace ce::dx12_overlay_policy
