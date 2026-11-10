#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

struct IDXGISwapChain;

// Whether a swapchain CE saw is still alive, and who dropped its last reference - without holding one.
//
// DXGI refuses a replacement swapchain for a window (E_ACCESSDENIED) while the window's previous swapchain lives.
// GTA V Enhanced DLSS FG -> FSR FG (logs/gtacrash, 0.1.7492): FidelityFX's replacement create was refused through
// every CE recovery and the game dereferenced the missing swapchain. CE tracks swapchains by raw address only (a
// reference would itself pin the window), so its diagnostics could not say whether the old DLSS-G chain was still
// alive, and the dump held no heap image of it. A destruction witness answers that: a small COM object attached to
// the chain as DXGI private data. DXGI keeps a reference on the witness, not the other way round, and releases it
// only when it destroys the chain, so the witness's final Release is the chain's destruction - observed, never
// probed, on whichever thread drops the last reference.
//
// The witness changes no slot, entry or reference count of the chain; DXGI stores one more private-data item.

namespace ce::swapchain_lifetime_witness {

enum class ChainState {
    kUnwatched,  // never witnessed (or its record was evicted)
    kAlive,      // witnessed and DXGI has not destroyed it
    kDestroyed,  // DXGI released the witness: the chain is gone
};

struct ChainReport {
    ChainState state = ChainState::kUnwatched;
    HWND window = nullptr;
    uint64_t generation = 0;
    uint64_t watchedAtMs = 0;
    uint64_t destroyedAtMs = 0;
    DWORD destroyingThread = 0;
    // The first caller frame outside DXGI, the D3D runtimes, the system DLLs and CE when the chain was destroyed:
    // the module that released the chain's last reference, e.g. "sl.interposer.dll".
    char destroyedBy[64] = {};
};

// Attaches a witness to a live swapchain of `window`. Idempotent while the chain lives; a new chain at the address
// of a destroyed one gets a new witness. False when DXGI refuses the private data (nothing is recorded then).
bool Watch(IDXGISwapChain* swapchain, HWND window);

// What the witness knows about this address. Never dereferences the pointer.
ChainReport Describe(const IDXGISwapChain* swapchain);

// One line per witnessed chain of `window` (alive, or destroyed within the record window), for the
// E_ACCESSDENIED diagnostics: which chain still holds the window and which ones are gone, and who released them.
// Returns the number of chains still alive.
size_t LogWindowChains(HWND window, const char* stage);

const char* StateName(ChainState state);

// The modules of a destruction stack that belong to the releasing machinery, not to the releasing owner: DXGI, the
// D3D runtimes and the system DLLs between them and the caller. Names as an image's export directory spells them.
bool IsReleaseMachineryModule(const char* imageName);

// The frame that names the releaser in a destruction stack, top first: the first frame outside the release machinery
// once the stack has entered it (the frames above it are the witness's own). -1 when the stack never leaves it.
int SelectReleaserFrame(const char* const* imageNames, size_t count);

void ResetForTesting();

}  // namespace ce::swapchain_lifetime_witness
