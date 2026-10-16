#pragma once

// x86 deep hook: the byte-level rules, kept free of Windows calls so they are
// unit-tested on any host.
//
// A deep hook patches a function body past the bytes a foreign overlay owns at
// its entry, so the foreign chain keeps calling through and CE runs below it.
// On x86 those entry bytes are the hot-patch prologue every system DLL starts
// with (`mov edi,edi; push ebp; mov ebp,esp`): Steam and the other x86 hookers
// replace exactly these five bytes with `jmp rel32`, and their trampolines
// re-run them and continue at +5.
//
// When the patch at the resume offset runs, the prologue before it has already
// executed (in the function or in a foreign trampoline). With a frame pointer
// set up, `mov esp,ebp; pop ebp` restores the exact entry state whatever the
// prologue did to esp afterwards (alignment, locals, pushes of registers that
// still hold their values), so CE's detour is entered as if called directly.

#include <cstdint>
#include <cstring>

namespace ce::deep_hook_x86 {

struct PrologUndo {
    bool framePointerOnly = false;  // resume directly after `mov ebp,esp`: esp == ebp, `pop ebp` suffices
};

// [0, prologLength) must be: optional `mov edi,edi` (8B FF), `push ebp` (55),
// `mov ebp,esp` (8B EC or 89 E5), then only instructions that touch esp or
// push without changing ebp: and/sub esp imm8/imm32, push reg, push imm8/imm32.
// False for anything else; the caller then keeps its non-deep fallback.
inline bool TryAnalyzeProlog(const unsigned char* bytes, int prologLength, PrologUndo* out) {
    if (!bytes || prologLength <= 0) {
        return false;
    }
    int pos = 0;
    if (pos + 1 < prologLength && bytes[pos] == 0x8B && bytes[pos + 1] == 0xFF) {
        pos += 2;
    }
    if (pos >= prologLength || bytes[pos] != 0x55) {
        return false;
    }
    ++pos;
    if (pos + 2 > prologLength) {
        return false;
    }
    const bool movEbpEsp = (bytes[pos] == 0x8B && bytes[pos + 1] == 0xEC) || (bytes[pos] == 0x89 && bytes[pos + 1] == 0xE5);
    if (!movEbpEsp) {
        return false;
    }
    pos += 2;
    const int frameEnd = pos;
    while (pos < prologLength) {
        const unsigned char b = bytes[pos];
        if (b >= 0x50 && b <= 0x57) {  // push r32
            pos += 1;
        } else if (b == 0x6A && pos + 2 <= prologLength) {  // push imm8
            pos += 2;
        } else if (b == 0x68 && pos + 5 <= prologLength) {  // push imm32
            pos += 5;
        } else if (b == 0x83 && pos + 3 <= prologLength && (bytes[pos + 1] == 0xE4 || bytes[pos + 1] == 0xEC)) {
            pos += 3;  // and esp,imm8 / sub esp,imm8
        } else if (b == 0x81 && pos + 6 <= prologLength && (bytes[pos + 1] == 0xE4 || bytes[pos + 1] == 0xEC)) {
            pos += 6;  // and esp,imm32 / sub esp,imm32
        } else {
            return false;
        }
    }
    if (pos != prologLength) {
        return false;
    }
    if (out) {
        out->framePointerOnly = frameEnd == prologLength;
    }
    return true;
}

// The patch written at the resume offset: the prologue undo, then `jmp rel32`
// to the detour, padded with NOP up to `displaceSize`.
inline int PatchUndoSize(const PrologUndo& undo) {
    return undo.framePointerOnly ? 1 : 3;
}

inline int PatchSize(const PrologUndo& undo) {
    return PatchUndoSize(undo) + 5;
}

// rel32 wraps modulo 2^32 on x86, so every target is reachable.
inline uint32_t Rel32(uint32_t instructionEnd, uint32_t target) {
    return target - instructionEnd;
}

// False when `displaceSize` cannot hold the patch or exceeds `capacity`.
inline bool BuildPatch(const PrologUndo& undo, uint32_t patchAddress, uint32_t detourAddress, int displaceSize,
                       unsigned char* out, int capacity) {
    if (!out || displaceSize < PatchSize(undo) || displaceSize > capacity) {
        return false;
    }
    int pos = 0;
    if (!undo.framePointerOnly) {
        out[pos++] = 0x8B;  // mov esp,ebp
        out[pos++] = 0xE5;
    }
    out[pos++] = 0x5D;  // pop ebp
    out[pos++] = 0xE9;  // jmp rel32
    const uint32_t rel = Rel32(patchAddress + static_cast<uint32_t>(pos) + 4u, detourAddress);
    std::memcpy(out + pos, &rel, sizeof(rel));
    pos += 4;
    while (pos < displaceSize) {
        out[pos++] = 0x90;
    }
    return true;
}

}  // namespace ce::deep_hook_x86
