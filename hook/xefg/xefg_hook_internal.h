#pragma once

// Shared between the XeFG and XeLL detour units (xefg_hook.cpp, xell_hook.cpp).

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "xefg_api.h"

namespace XeFGHook {

// The runtime image (libxess_fg.dll or libxell.dll) whose export entries carry CE's detours, and their lifetime. The
// loader may unload it and map a fresh, unpatched image at the same address; the inline-hook engine's records of
// the old entries would refuse the new install, and the module handle alone cannot tell the two images apart.
// The detours forward through one set of original slots, so one image owns them at a time: a second copy mapped
// from another folder while the first is loaded would re-point the first image's detours at its own code.
class HookedRuntimeImage {
public:
    // Install side, under the owner's install mutex. True while `module` is the image already detoured.
    bool IsHooked(HMODULE module) const;
    // True while another, still mapped image owns the detours (`module` stays unobserved).
    bool IsOwnedByAnotherImage(HMODULE module) const;
    // Before installing: forgets the records of an image that unloaded since (never touching its old addresses) and
    // returns how many targets it forgot.
    size_t ForgetUnloaded();
    // Around the install: the image being detoured, then its targets (detoured or not).
    void Begin(HMODULE module);
    void NoteTargets(void* const* targets, size_t count);
    // Loader notification, loader lock held: atomics only. True when `moduleBase` is the detoured image.
    bool NoteUnloaded(const void* moduleBase);

private:
    static constexpr size_t kMaxTargets = 16;
    std::atomic<HMODULE> module_{nullptr};
    std::atomic<bool> unloaded_{false};
    void* targets_[kMaxTargets] = {};
    size_t targetCount_ = 0;
};

// One application frame (xefgSwapChainSetPresentId on the game thread, before the proxy Present): paced first, then
// counted.
void PaceApplicationFrame(ce::xefg_api::Handle handle);
void NoteApplicationFrame(ce::xefg_api::Handle handle, uint32_t presentId);

// Detours the XeLL exports of `module` (xell_hook.cpp).
void InstallXeLLExports(HMODULE module, const char* modulePath);
// Loader notification: true when `moduleBase` is the XeLL image CE detoured (xell_hook.cpp).
bool NoteXeLLModuleUnloaded(const void* moduleBase);

}  // namespace XeFGHook
