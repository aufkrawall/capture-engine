#pragma once

#include "dxgi_shared.h"
#include "hook_common.h"
#include "swapchain_flag_policy.h"

// Single application point for the `backbuffer_count` creation-descriptor
// override. Every CreateSwapChain/CreateSwapChainForHwnd/D3D11CreateDeviceAndSwapChain
// entry CE hooks used to carry its own copy of this logic, which is how one of
// them could keep adding the waitable object after the reconciliation that
// hides it again had stopped existing. The rule now lives in
// swapchain_flag_policy.h and is applied here, once.
namespace ce::swapchain_flag_policy {

// Works for both DXGI_SWAP_CHAIN_DESC and DXGI_SWAP_CHAIN_DESC1: only
// BufferCount, Flags and SwapEffect are read or written.
template <typename SwapChainDesc>
inline Decision ApplyBackbufferCountOverrideToDesc(SwapChainDesc& desc, const GraphicsConfig& gfx,
                                                   const char* source) {
    const bool reconciles = DXGIShared::ReconcilesApplicationResizeFlags();
    const Decision decision = DecideBackbufferCountOverride(desc.BufferCount, desc.Flags, desc.SwapEffect,
                                                           gfx.backbufferCount, reconciles);
    const char* label = source && source[0] ? source : "CreateSwapChain";

    if (decision.bufferCountAction == BufferCountAction::Applied) {
        HookLogImportant("%s: Overriding BufferCount %u -> %u", label, desc.BufferCount, decision.bufferCount);
    } else if (decision.bufferCountAction == BufferCountAction::PacedInsteadOfShrunk) {
        HookLogImportant("%s: Keeping the application's BufferCount %u above the configured %d (flip model) — %s",
                         label, desc.BufferCount, gfx.backbufferCount,
                         decision.waitableObjectRequested || (desc.Flags & kCeOwnedCreationFlags) != 0
                             ? "the flip queue depth is paced by the waitable object instead"
                             : "the configured depth cannot be enforced");
    }
    if (decision.waitableObjectWithheld) {
        // Adding the flag here would make every application ResizeBuffers call
        // that passes its own creation flags fail with E_INVALIDARG, which is a
        // hard startup failure in games that check the result.
        HookLogImportant(
            "%s: NOT adding the frame-latency waitable object for backbuffer_count=%d — CE has no ResizeBuffers "
            "reconciliation for this process, and an application resize with its own flags would fail "
            "E_INVALIDARG",
            label, gfx.backbufferCount);
    }

    desc.BufferCount = decision.bufferCount;
    desc.Flags = decision.flags;
    return decision;
}

}  // namespace ce::swapchain_flag_policy
