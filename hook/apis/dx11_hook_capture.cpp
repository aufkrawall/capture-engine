#include "dx11_hook_internal.h"

// Called from DXGI SwapChain wrapper for frame capture (wrapper-only
// architecture)
void DX11_ProcessFrameExternal(IDXGISwapChain* pSwapChain) {
    HandleDX11ProcessFrame(pSwapChain, true);
}
