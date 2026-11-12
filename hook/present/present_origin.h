#pragma once

// Present provenance: which module issued a Present CE intercepted, and the [PRESENT ORIGIN] record of it.

#include <windows.h>

#include <cstdint>

#include "present_originator_policy.h"

struct IDXGISwapChain;

namespace DXGIShared {

struct PresentOrigin {
    static constexpr int kMaxModules = 8;
    static constexpr int kModuleNameLength = 32;

    ce::present_originator::Originator originator = ce::present_originator::Originator::kUnresolved;
    bool fromStreamline = false;
    bool fromFFXFrameGeneration = false;
    bool fromXeFG = false;
    // Resolved from the frames above a foreign chain; otherwise it is the immediate caller.
    bool walked = false;
    char module[MAX_PATH] = {};  // the originator's path, empty when unresolved
    // The originator's return address: its call into the layer below (a wrapped swapchain's Present), or null.
    const void* originatorFrame = nullptr;
    // Base names of the modules walked up to and including the originator (consecutive repeats folded).
    uint8_t moduleCount = 0;
    char modules[kMaxModules][kModuleNameLength] = {};
};

// Below a foreign Present chain the immediate caller is always the last foreign overlay in it, so the
// originator is resolved from the frames above them: past CE's own frames, the tracked overlays and the
// system's dispatch modules. A process with no frame-generation runtime keeps the thread's last walk
// (RuntimeModuleWatch); otherwise a bounded walk on every Present, with each frame's module classified once.
// The reference is the thread's own and stays valid until its next call.
const PresentOrigin& ResolvePresentOriginatorBelowForeignChain();

// Every module the loader reports and the scan of what was already mapped feed this; see RuntimeModuleWatch.
// Loader-safe in the sense the loader notification needs: no lock of CE's, and the export lookup is the same one
// that notification already makes for its DLSS consumers.
void NoteModuleForPresentOriginator(HMODULE module);
// Runs once, off the Present thread, after the loader notification is registered.
void SeedPresentOriginatorModules();

// The same classification for the immediate caller (CE above or without a foreign chain).
void ClassifyPresentCallerOrigin(const void* callerAddress, PresentOrigin* out);

// Everything the per-Present classifiers say about the immediate caller's module, kept per thread until a
// module unloads or the identified-overlay list changes: the same few dozen substring scans over the same
// path on every Present were most of the detour's caller stage (HotS on D3D12, 20261103_215427: 25 us).
// Copy what is needed: the reference may be reused by a later lookup on this thread.
struct PresentCallerVerdict {
    bool pathResolved = false;
    bool thirdPartyOverlay = false;
    bool streamlineModule = false;
    bool ffxFrameGenerationModule = false;
    char path[MAX_PATH] = {};
    PresentOrigin origin;
};
const PresentCallerVerdict& ClassifyPresentCaller(const void* callerAddress);

// [PRESENT ORIGIN]: one line per (swapchain, thread, Present/Present1) stream whenever its originator changes,
// with the repeats in between, plus a summary of every originator kind each 10 s.
void NotePresentOrigin(IDXGISwapChain* swapchain, bool present1, const PresentOrigin& origin);

}  // namespace DXGIShared
