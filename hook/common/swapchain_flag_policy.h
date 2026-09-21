#pragma once

#include <dxgi.h>

#include <cstdint>

#include "../../common/shared_defs.h"

// Ownership rules for the one swapchain creation flag CE adds on the
// application's behalf.
//
// `backbuffer_count` bounds the flip queue without shrinking the physical
// BufferCount: CE adds DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT to
// the application's creation descriptor and waits on that object before each
// Present (see present_pacing_policy.h).  That flag is *not* private to CE.
// DXGI stores it in the swapchain and CDXGISwapChain::ValidateResizeBuffers
// compares it against every later ResizeBuffers/ResizeBuffers1 call:
//
//     eax = creationFlags ^ callerFlags
//     test al, 0x40                  ; FRAME_LATENCY_WAITABLE_OBJECT
//     jne  -> return E_INVALIDARG    ; 0x80070057
//
// An application that remembers its own creation flags and passes them back —
// which is the normal, correct thing to do — therefore gets E_INVALIDARG for a
// flag it never asked for.
//
// Strange Brigade (DX12) session `20260921_173511` is that failure.  The game
// created a 3840x2160 FLIP_DISCARD chain with Flags=0x802, CE turned it into
// 0x842 (confirmed in the dump at swapchain+0x184), the game called
// ResizeBuffers back with its own 0x802 and reported
// "Can't recover from driver error. Error Code 80070057", then exited without
// ever presenting a frame.
//
// CE normally hides the flag again by rewriting the application's
// ResizeBuffers arguments, in CWrapDXGISwapChain or in
// DXGIShared::DetourResizeBuffers.  Neither existed in that session: Steam's
// overlay owned the dxgi!Present entry, so CE kept the swapchain vtable
// pristine and handed the game the real swapchain.  The mutation was
// unconditional while the compensation was not.
//
// The invariant these helpers exist to keep: CE adds the waitable flag only
// when it will also reconcile the application's later resize calls, and the
// reconciliation is driven by the swapchain's *real* creation flags rather than
// by re-deriving intent from the current config.
namespace ce::swapchain_flag_policy {

// Every creation flag CE may add on the application's behalf. Only these bits
// are ever rewritten in an application resize call; all others belong to the
// application and are forwarded untouched.
inline constexpr UINT kCeOwnedCreationFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

inline bool IsFlipSwapEffect(DXGI_SWAP_EFFECT swapEffect) {
    return swapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || swapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD;
}

enum class BufferCountAction : uint8_t {
    // No `backbuffer_count` override is configured, or it already matches.
    None,
    // The configured depth is applied to the physical BufferCount.
    Applied,
    // A flip chain already queues deeper than the configured depth. Shrinking
    // BufferCount would change what the application allocated, so the depth is
    // enforced by the waitable object instead.
    PacedInsteadOfShrunk,
};

struct Decision {
    UINT bufferCount = 0;
    UINT flags = 0;
    BufferCountAction bufferCountAction = BufferCountAction::None;
    // CE added the waitable object and owns reconciling it on resize.
    bool waitableObjectRequested = false;
    // CE wanted the waitable object but has no way to hide it from the
    // application's own resize calls, so it left the descriptor alone. The
    // `backbuffer_count` depth is then whatever BufferCount alone can express.
    bool waitableObjectWithheld = false;
};

// `ceReconcilesApplicationResizeFlags` must be true only when CE will rewrite
// the flags of every ResizeBuffers/ResizeBuffers1 call the application makes on
// the resulting swapchain — through the CE swapchain wrapper, or through a CE
// hook on the swapchain's ResizeBuffers vtable slots.
inline Decision DecideBackbufferCountOverride(UINT applicationBufferCount, UINT applicationFlags,
                                              DXGI_SWAP_EFFECT swapEffect, int32_t configuredBackbufferCount,
                                              bool ceReconcilesApplicationResizeFlags) {
    Decision decision;
    decision.bufferCount = applicationBufferCount;
    decision.flags = applicationFlags;
    if (!HasBackbufferCountOverride(configuredBackbufferCount)) {
        return decision;
    }

    const UINT requested = static_cast<UINT>(configuredBackbufferCount);
    const bool isFlip = IsFlipSwapEffect(swapEffect);
    if (!isFlip) {
        // A blit-model chain has no flip queue to pace and no waitable object;
        // the depth is expressed by BufferCount alone.
        if (applicationBufferCount != requested) {
            decision.bufferCount = requested;
            decision.bufferCountAction = BufferCountAction::Applied;
        }
        return decision;
    }

    if (ceReconcilesApplicationResizeFlags) {
        decision.flags |= kCeOwnedCreationFlags;
        decision.waitableObjectRequested = (applicationFlags & kCeOwnedCreationFlags) == 0;
    } else {
        decision.waitableObjectWithheld = (applicationFlags & kCeOwnedCreationFlags) == 0;
    }

    if (requested < applicationBufferCount) {
        decision.bufferCountAction = BufferCountAction::PacedInsteadOfShrunk;
    } else if (applicationBufferCount != requested) {
        decision.bufferCount = requested;
        decision.bufferCountAction = BufferCountAction::Applied;
    }
    return decision;
}

// Forces the flags an application passes to ResizeBuffers/ResizeBuffers1 to
// agree with the swapchain's real creation flags in exactly the bits CE owns.
// Reading the live descriptor rather than re-deriving intent from the config is
// what makes this correct across a config reload, a chain created before the
// override existed, and a chain CE never touched at all: DXGI requires exact
// agreement in both directions, so a stale "add the bit" is as fatal as a
// missing one.
inline UINT ReconcileApplicationResizeFlags(UINT callerFlags, UINT creationFlags) {
    return (callerFlags & ~kCeOwnedCreationFlags) | (creationFlags & kCeOwnedCreationFlags);
}

// True when the caller's flags would have been rejected by DXGI. Used only to
// keep the reconciliation diagnosable; the rewrite itself is unconditional.
inline bool WouldDXGIRejectResizeFlags(UINT callerFlags, UINT creationFlags) {
    return ((callerFlags ^ creationFlags) & kCeOwnedCreationFlags) != 0;
}

}  // namespace ce::swapchain_flag_policy
