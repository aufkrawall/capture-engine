#pragma once

// CE-owned liveness ledger for real (unwrapped) DXGI swapchains.
//
// CE tracks real swapchain pointers raw, deliberately: an extra COM reference
// pins the chain and makes DXGI refuse the per-HWND flip-model replacement
// create with E_ACCESSDENIED. Raw tracking means a pointer goes stale the
// moment the chain dies, and there is NO way to ask a COM object whether it is
// still alive. A freed heap block stays committed (VirtualQuery still reports
// MEM_COMMIT), its first quadword still points at real vtable code, and a
// "net-zero" AddRef/Release probe therefore resurrects the corpse and then
// destroys it a second time. With a proxy chain (ReShade, OptiScaler,
// Special K) the second destruction re-runs the proxy's own C++ destructor and
// frees pointers it already freed: STATUS_HEAP_CORRUPTION (0xC0000374) on game
// close (session 20260819_000437, Strange Brigade DX12 with OptiScaler,
// Special K, ReShade and the Steam overlay all injected).
//
// So CE never probes a chain it does not hold a reference to. It records what
// it learned at the one instant it could learn it soundly: when the wrapper
// destructor released the last reference CE owned, IUnknown::Release returned
// the exact number of references still pinning the chain. Diagnostics read that
// note instead of touching the object.

#include <cstddef>

namespace ce::swapchain_liveness {

struct LivenessNote {
    bool known = false;                          // CE has a recorded observation for this address.
    bool ceReleasedLastOwnedReference = false;   // CE let go of its last reference on the chain.
    unsigned long residualRefsAtCeRelease = 0;   // References still pinning it at that instant.
};

// Record the count IUnknown::Release returned when CE dropped its last owned reference.
void NoteCeReleasedLastOwnedReference(const void* chain, unsigned long residualRefs);

// A live chain occupies this address again (it was just created or re-tracked), so any earlier
// note describes a different object: drop it. The allocator reuses addresses, and reporting a
// dead chain's residual count for a live one would be worse than reporting nothing.
void ForgetNote(const void* chain);

LivenessNote Query(const void* chain);

// Retained note count. The ledger is bounded so a long session with many FG-driven swapchain
// recreations cannot grow it without limit.
size_t NoteCount();
size_t MaxNotes();
void ResetForTesting();

}  // namespace ce::swapchain_liveness
