/**
 * Inline Hook — instruction analysis (internal)
 *
 * Length-disassembly and entry-point inspection shared by the shallow, deep
 * and bypass installers. Not part of the public inline_hook.h contract.
 */

#pragma once

#include <windows.h>
#include <cstdint>

namespace InlineHook {

// Decode the length of the instruction at 'code'. Returns 0 when the encoding
// is not covered by the engine.
int GetInstructionLength(const uint8_t* code, bool is64bit);

// Offset of the 32-bit PC-relative displacement within the instruction at
// 'code', or -1 when the instruction does not use PC-relative addressing.
int GetRipRelativeDispOffset(const uint8_t* code, int instrLen, bool is64bit);

// True when the entry point already carries a recognised inline-hook patch.
bool IsAlreadyHooked(const uint8_t* code, bool is64bit);

}  // namespace InlineHook
