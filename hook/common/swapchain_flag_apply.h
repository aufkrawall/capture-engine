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

// Marks a swapchain whose frame-latency waitable object CE added, so the
// `backbuffer_count` pacing wait knows the object is CE's to wait on. A
// waitable the creator asked for itself is waited on by that creator, and a
// second wait by CE stalls the present (present_pacing_policy.h). DXGI private
// data lives and dies with the swapchain, so the tag cannot outlive it or be
// inherited by a later chain at the same address.
// {B3321024-12F9-4675-BE2C-6D3168AB3C14}
inline constexpr GUID kCeAddedFrameLatencyWaitableGuid = {
    0xb3321024, 0x12f9, 0x4675, {0xbe, 0x2c, 0x6d, 0x31, 0x68, 0xab, 0x3c, 0x14}};

// Call after the create whose descriptor CE changed. `ceAddedWaitable` is that
// decision's `waitableObjectRequested`, and `created` is the object the real
// DXGI create returned (never a CE wrapper around it).
inline void NoteCeAddedFrameLatencyWaitable(bool ceAddedWaitable, HRESULT createHr, IUnknown* created,
                                            const char* source) {
    if (!ceAddedWaitable || FAILED(createHr) || !created) {
        return;
    }
    IDXGIObject* object = nullptr;
    if (FAILED(created->QueryInterface(IID_PPV_ARGS(&object))) || !object) {
        HookLogImportant("%s: could not tag the CE-added frame-latency waitable on %p (no IDXGIObject) - "
                         "backbuffer_count will not pace this swapchain",
                         source && source[0] ? source : "CreateSwapChain", created);
        return;
    }
    const uint8_t marker = 1;
    const HRESULT hr = object->SetPrivateData(kCeAddedFrameLatencyWaitableGuid, sizeof(marker), &marker);
    object->Release();
    if (FAILED(hr)) {
        HookLogImportant("%s: tagging the CE-added frame-latency waitable on %p failed hr=0x%08X - "
                         "backbuffer_count will not pace this swapchain",
                         source && source[0] ? source : "CreateSwapChain", created, static_cast<unsigned>(hr));
    }
}

inline bool DidCeAddFrameLatencyWaitable(IDXGIObject* swapchain) {
    if (!swapchain) {
        return false;
    }
    uint8_t marker = 0;
    UINT size = sizeof(marker);
    return SUCCEEDED(swapchain->GetPrivateData(kCeAddedFrameLatencyWaitableGuid, &size, &marker)) &&
           size == sizeof(marker) && marker == 1;
}

}  // namespace ce::swapchain_flag_policy
