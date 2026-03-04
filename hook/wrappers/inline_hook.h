/**
 * Minimal Inline Hook (Trampoline-based Detour)
 *
 * Patches the first bytes of a target function to jump to a detour,
 * and creates a trampoline that executes the original instructions.
 *
 * Unlike VTable hooks, the trampoline truly bypasses the hook -
 * calling through it will NEVER re-enter the detour. This solves
 * the infinite re-entry problem with DXGI Present vtable hooks.
 *
 * Supports x64 (14-byte absolute JMP) and x86 (5-byte relative JMP).
 */

#pragma once

#include <cstdint>

namespace InlineHook {

// Install an inline hook on the target function.
// - target: address of the function to hook
// - detour: address of the detour function
// - outTrampoline: receives a pointer to the trampoline (call this to invoke
//   the original function without re-entering the detour)
// Returns true on success.
bool Install(void* target, void* detour, void** outTrampoline);

// Remove a previously installed inline hook, restoring the original bytes.
// - target: the same target address passed to Install()
// Returns true on success.
bool Remove(void* target);

// Remove all installed hooks and free trampoline memory.
void RemoveAll();

// Install a deep hook that fully wraps a function past an external JMP patch.
// When another component (e.g. Streamline) hooks a function at byte 0 with a
// JMP and uses a saved trampoline for internal calls (bypassing our normal
// hook), this patches the function body at the resume offset to redirect ALL
// callers to wrapperFn — including those using saved trampolines.
//
// The wrapperFn must have the exact same calling convention and signature as
// the target function. It receives all original parameters and should call the
// returned trampoline to invoke the real function.
//
// Requirements: the original bytes [0, resumeOffset) must all be PUSH
// instructions (standard function prolog). The hook undoes these pushes and
// redirects to wrapperFn, which calls the original via the returned trampoline.
//
// - target: address of the function (must have a JMP at byte 0)
// - wrapperFn: replacement function with same signature as target
// Returns trampoline to call original function, or nullptr on failure.
void* InstallDeepHook(void* target, void* wrapperFn);

// Remove a deep hook installed by InstallDeepHook.
bool RemoveDeepHook(void* target);

}  // namespace InlineHook
