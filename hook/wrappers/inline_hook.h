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

}  // namespace InlineHook
