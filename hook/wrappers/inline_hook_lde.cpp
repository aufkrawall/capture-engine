/**
 * Inline Hook — instruction analysis
 *
 * The Length Disassembly Engine (LDE) for x86/x64, the PC-relative
 * displacement locator that drives trampoline fixups, and the entry-point
 * inspection helpers that decide whether a target is already patched.
 *
 * Split out of inline_hook.cpp; see inline_hook_lde.h for the shared surface.
 */

#include "inline_hook_lde.h"

#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "../common/hook_common.h"

namespace InlineHook {

// ============================================================================
// Length Disassembly Engine (LDE)
// ============================================================================

// Opcode property flags
enum : uint8_t {
    OP_NONE = 0x00,
    OP_MODRM = 0x01,    // Has ModR/M byte
    OP_IMM8 = 0x02,     // Has 8-bit immediate
    OP_IMM32 = 0x04,    // Has 32-bit immediate (16-bit with 66h prefix)
    OP_2BYTE = 0x08,    // Is 0F-prefixed (set during parsing, not in table)
    OP_SPECIAL = 0x80,  // Needs special handling
};

// clang-format off

// One-byte opcode table (properties for opcodes 0x00-0xFF)
// M = ModR/M, I1 = imm8, I4 = imm32, MI1 = ModR/M + imm8, MI4 = ModR/M + imm32
static const uint8_t g_oneByteOpcodes[256] = {
    // 0x00-0x0F: ADD, OR, 2-byte escape
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_IMM8, OP_IMM32, OP_NONE, OP_NONE,  // 00-07
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_IMM8, OP_IMM32, OP_NONE, OP_NONE,  // 08-0F (0F=escape, handled separately)

    // 0x10-0x1F: ADC, SBB
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_IMM8, OP_IMM32, OP_NONE, OP_NONE,  // 10-17
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_IMM8, OP_IMM32, OP_NONE, OP_NONE,  // 18-1F

    // 0x20-0x2F: AND, SUB (26=ES prefix, 2E=CS prefix)
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_IMM8, OP_IMM32, OP_NONE, OP_NONE,  // 20-27
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_IMM8, OP_IMM32, OP_NONE, OP_NONE,  // 28-2F

    // 0x30-0x3F: XOR, CMP (36=SS prefix, 3E=DS prefix)
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_IMM8, OP_IMM32, OP_NONE, OP_NONE,  // 30-37
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_IMM8, OP_IMM32, OP_NONE, OP_NONE,  // 38-3F

    // 0x40-0x4F: INC/DEC (x86) or REX prefixes (x64) - handled specially
    OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE,  // 40-47
    OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE,  // 48-4F

    // 0x50-0x5F: PUSH/POP reg
    OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE,  // 50-57
    OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE,  // 58-5F

    // 0x60-0x6F: PUSHA/POPA(x86), BOUND, MOVSXD, prefixes, PUSH/IMUL
    OP_NONE, OP_NONE, OP_MODRM, OP_MODRM, OP_NONE, OP_NONE, OP_NONE, OP_NONE,         // 60-67 (64/65/66/67=prefixes)
    OP_IMM32, OP_MODRM|OP_IMM32, OP_IMM8, OP_MODRM|OP_IMM8, OP_NONE, OP_NONE, OP_NONE, OP_NONE,  // 68-6F

    // 0x70-0x7F: Jcc short (rel8)
    OP_IMM8, OP_IMM8, OP_IMM8, OP_IMM8, OP_IMM8, OP_IMM8, OP_IMM8, OP_IMM8,  // 70-77
    OP_IMM8, OP_IMM8, OP_IMM8, OP_IMM8, OP_IMM8, OP_IMM8, OP_IMM8, OP_IMM8,  // 78-7F

    // 0x80-0x8F: Group1 ALU, TEST, XCHG, MOV, LEA, POP
    OP_MODRM|OP_IMM8, OP_MODRM|OP_IMM32, OP_MODRM|OP_IMM8, OP_MODRM|OP_IMM8,  // 80-83
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // 84-87
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // 88-8B
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // 8C-8F

    // 0x90-0x9F: NOP, XCHG, CBW, CDQ, CALL FAR, FWAIT, PUSHF, POPF, SAHF, LAHF
    OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE,  // 90-97
    OP_NONE, OP_NONE, OP_SPECIAL, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE,  // 98-9F (9A=CALL FAR special)

    // 0xA0-0xAF: MOV moffs, MOVS, CMPS, TEST, STOS, LODS, SCAS
    OP_SPECIAL, OP_SPECIAL, OP_SPECIAL, OP_SPECIAL,  // A0-A3: MOV AL/AX,moffs (special addr size)
    OP_NONE, OP_NONE, OP_NONE, OP_NONE,              // A4-A7: MOVS/CMPS
    OP_IMM8, OP_IMM32, OP_NONE, OP_NONE,             // A8-AB: TEST AL/AX, STOS
    OP_NONE, OP_NONE, OP_NONE, OP_NONE,              // AC-AF: LODS/SCAS

    // 0xB0-0xBF: MOV reg, imm
    OP_IMM8, OP_IMM8, OP_IMM8, OP_IMM8, OP_IMM8, OP_IMM8, OP_IMM8, OP_IMM8,  // B0-B7: MOV r8, imm8
    OP_IMM32, OP_IMM32, OP_IMM32, OP_IMM32, OP_IMM32, OP_IMM32, OP_IMM32, OP_IMM32,  // B8-BF: MOV r32, imm32 (imm64 with REX.W!)

    // 0xC0-0xCF: Shift, RET, VEX, MOV, ENTER, LEAVE, RET, INT
    OP_MODRM|OP_IMM8, OP_MODRM|OP_IMM8, OP_SPECIAL, OP_NONE,  // C0-C3 (C2=RET imm16)
    OP_SPECIAL, OP_SPECIAL, OP_MODRM|OP_IMM8, OP_MODRM|OP_IMM32,  // C4-C7 (C4=VEX3/LES, C5=VEX2/LDS)
    OP_SPECIAL, OP_NONE, OP_SPECIAL, OP_NONE,  // C8-CB (C8=ENTER, CA=RETF imm16)
    OP_NONE, OP_IMM8, OP_NONE, OP_NONE,  // CC-CF: INT3, INT imm8, INTO, IRET

    // 0xD0-0xDF: Shift, AAM/AAD, FPU
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // D0-D3: Shift
    OP_IMM8, OP_IMM8, OP_NONE, OP_NONE,        // D4-D7 (D4=AAM, D5=AAD, D6=SALC, D7=XLAT)
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // D8-DB: FPU
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // DC-DF: FPU

    // 0xE0-0xEF: LOOP, JCXZ, IN/OUT, CALL, JMP
    OP_IMM8, OP_IMM8, OP_IMM8, OP_IMM8,  // E0-E3: LOOPx/JCXZ rel8
    OP_IMM8, OP_IMM8, OP_IMM8, OP_IMM8,  // E4-E7: IN/OUT imm8
    OP_IMM32, OP_IMM32, OP_SPECIAL, OP_IMM8,  // E8-EB: CALL/JMP rel32, JMP FAR, JMP rel8
    OP_NONE, OP_NONE, OP_NONE, OP_NONE,  // EC-EF: IN/OUT DX

    // 0xF0-0xFF: LOCK, REP, HLT, CMC, Group3, CLC/STC, CLI/STI, CLD/STD, Group4/5
    OP_NONE, OP_NONE, OP_NONE, OP_NONE,        // F0-F3 (F0=LOCK, F2=REPNE, F3=REP - prefixes)
    OP_NONE, OP_NONE, OP_SPECIAL, OP_SPECIAL,  // F4-F7 (F6/F7=Group3 - TEST has imm)
    OP_NONE, OP_NONE, OP_NONE, OP_NONE,        // F8-FB
    OP_NONE, OP_NONE, OP_MODRM, OP_MODRM,     // FC-FF
};

// Two-byte opcode table (0F xx)
static const uint8_t g_twoByteOpcodes[256] = {
    // 0x00-0x0F: Group6/7, LAR, LSL, CLTS, SYSRET, etc.
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_NONE, OP_NONE, OP_NONE, OP_NONE,  // 00-07
    OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_MODRM, OP_NONE, OP_NONE,      // 08-0F

    // 0x10-0x1F: SSE moves
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // 10-17
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // 18-1F

    // 0x20-0x2F: MOV CR/DR, SSE
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_NONE, OP_NONE, OP_NONE, OP_NONE,       // 20-27
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // 28-2F

    // 0x30-0x3F: WRMSR, RDTSC, RDMSR, RDPMC, SYSENTER, SYSEXIT, 3-byte escapes
    OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE,  // 30-37
    OP_SPECIAL, OP_NONE, OP_SPECIAL, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE,  // 38-3F (38/3A=3-byte escape)

    // 0x40-0x4F: CMOVcc
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // 40-47
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // 48-4F

    // 0x50-0x5F: SSE
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // 50-57
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // 58-5F

    // 0x60-0x6F: MMX/SSE pack/unpack
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // 60-67
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // 68-6F

    // 0x70-0x7F: SSE shuffle/pack
    OP_MODRM|OP_IMM8, OP_MODRM|OP_IMM8, OP_MODRM|OP_IMM8, OP_MODRM|OP_IMM8,  // 70-73
    OP_MODRM, OP_MODRM, OP_MODRM, OP_NONE,                                      // 74-77
    OP_MODRM, OP_MODRM, OP_NONE, OP_NONE, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // 78-7F

    // 0x80-0x8F: Jcc rel32
    OP_IMM32, OP_IMM32, OP_IMM32, OP_IMM32, OP_IMM32, OP_IMM32, OP_IMM32, OP_IMM32,  // 80-87
    OP_IMM32, OP_IMM32, OP_IMM32, OP_IMM32, OP_IMM32, OP_IMM32, OP_IMM32, OP_IMM32,  // 88-8F

    // 0x90-0x9F: SETcc
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // 90-97
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // 98-9F

    // 0xA0-0xAF: PUSH/POP FS/GS, BT, SHLD, SHRD, IMUL
    OP_NONE, OP_NONE, OP_NONE, OP_MODRM,                                                  // A0-A3
    OP_MODRM|OP_IMM8, OP_MODRM, OP_NONE, OP_NONE,                                        // A4-A7
    OP_NONE, OP_NONE, OP_NONE, OP_MODRM,                                                  // A8-AB
    OP_MODRM|OP_IMM8, OP_MODRM, OP_MODRM, OP_MODRM,                                     // AC-AF

    // 0xB0-0xBF: CMPXCHG, LSS, BTR, LFS, LGS, MOVZX, Group8, POPCNT, BTC, BSR/BSF, MOVSX
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // B0-B7
    OP_MODRM, OP_NONE, OP_MODRM|OP_IMM8, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // B8-BF

    // 0xC0-0xCF: XADD, SSE compare, PINSRW, PEXTRW, SHUFPS, Group9, BSWAP
    OP_MODRM, OP_MODRM, OP_MODRM|OP_IMM8, OP_MODRM,                          // C0-C3
    OP_MODRM|OP_IMM8, OP_MODRM|OP_IMM8, OP_MODRM|OP_IMM8, OP_MODRM,        // C4-C7
    OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE, OP_NONE,  // C8-CF (BSWAP)

    // 0xD0-0xDF: SSE
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // D0-D7
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // D8-DF

    // 0xE0-0xEF: SSE
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // E0-E7
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // E8-EF

    // 0xF0-0xFF: SSE/MMX
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM,  // F0-F7
    OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_MODRM, OP_NONE,   // F8-FF
};

// clang-format on

// Parse ModR/M byte and return additional bytes (SIB + displacement)
static int ParseModRM(const uint8_t* code, bool hasAddrPrefix) {
    uint8_t modrm = code[0];
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t rm = modrm & 7;
    int extra = 1;  // The ModR/M byte itself

    if (mod == 3) {
        // Register direct - no extra bytes
        return extra;
    }

    bool use16bit = false;
#ifndef _WIN64
    use16bit = !hasAddrPrefix;  // Default 32-bit, 67h switches to 16-bit
    // Actually on 32-bit, default is 32-bit addressing, 67h switches to 16
    use16bit = hasAddrPrefix;
#else
    use16bit = hasAddrPrefix;  // 64-bit: 67h switches to 32-bit addressing
                               // (we don't support 16-bit addressing in 64-bit mode)
                               // In 64-bit mode with 67h, addressing uses 32-bit regs but same ModR/M
                               // encoding
#endif

    if (!use16bit) {
        // 32/64-bit addressing
        if (rm == 4) {
            extra++;  // SIB byte
            uint8_t sib = code[1];
            uint8_t base = sib & 7;
            if (mod == 0 && base == 5) {
                extra += 4;  // disp32
            }
        }

        if (mod == 0) {
            if (rm == 5) {
                extra += 4;  // disp32 (or RIP-relative in x64)
            }
        } else if (mod == 1) {
            extra += 1;  // disp8
        } else if (mod == 2) {
            extra += 4;  // disp32
        }
    } else {
        // 16-bit addressing (rare, mainly for x86 with 67h prefix)
        if ((mod == 0 && rm == 6) || mod == 2) {
            extra += 2;  // disp16
        } else if (mod == 1) {
            extra += 1;  // disp8
        }
    }

    return extra;
}

// Get the length of an instruction at the given address.
// Returns 0 if the instruction cannot be decoded (unsupported/unknown).
int GetInstructionLength(const uint8_t* code, bool is64bit) {
    const uint8_t* p = code;
    bool hasOpSize = false;    // 66h prefix
    bool hasAddrSize = false;  // 67h prefix
    bool hasREX_W = false;     // REX.W prefix (x64 only)

    // 1. Skip prefixes
    for (;;) {
        uint8_t b = *p;
        if (b == 0x66) {
            hasOpSize = true;
            p++;
        } else if (b == 0x67) {
            hasAddrSize = true;
            p++;
        } else if (b == 0xF0 || b == 0xF2 || b == 0xF3 || b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E ||
                   b == 0x64 || b == 0x65) {
            p++;  // LOCK, REPNE, REP, segment overrides
        } else if (is64bit && (b >= 0x40 && b <= 0x4F)) {
            hasREX_W = (b & 0x08) != 0;
            p++;  // REX prefix
        } else {
            break;
        }
    }

    uint8_t opcode = *p++;
    uint8_t flags;
    bool isTwoByte = false;

    // 2. Handle two-byte escape
    if (opcode == 0x0F) {
        opcode = *p++;
        isTwoByte = true;

        // Three-byte escape (0F 38 xx, 0F 3A xx) - treat all as ModR/M
        if (opcode == 0x38) {
            p++;  // third opcode byte
            int modrm = ParseModRM(p, hasAddrSize);
            return (int)(p + modrm - code);
        }
        if (opcode == 0x3A) {
            p++;  // third opcode byte
            int modrm = ParseModRM(p, hasAddrSize);
            return (int)(p + modrm + 1 - code);  // +1 for imm8
        }

        flags = g_twoByteOpcodes[opcode];
    } else {
        flags = g_oneByteOpcodes[opcode];
    }

    // 3. Handle special opcodes
    if (flags & OP_SPECIAL) {
        if (!isTwoByte) {
            switch (opcode) {
                case 0x9A:  // CALL FAR (x86 only)
                    return (int)(p - code) + (hasOpSize ? 4 : 6);
                case 0xA0:  // MOV AL, moffs
                case 0xA2:  // MOV moffs, AL
                case 0xA1:  // MOV rAX, moffs
                case 0xA3:  // MOV moffs, rAX
                    return (int)(p - code) + (is64bit && !hasAddrSize ? 8 : (hasAddrSize ? 2 : 4));
                case 0xC2:  // RET imm16
                case 0xCA:  // RETF imm16
                    return (int)(p - code) + 2;
                case 0xC4:  // VEX 3-byte prefix (x64) or LES (x86)
                    if (is64bit) {
                        p += 2;  // VEX payload bytes
                        p++;     // opcode after VEX
                        // VEX instructions always have ModR/M
                        int modrm = ParseModRM(p, hasAddrSize);
                        // Some VEX opcodes have imm8 - check the VEX map
                        // For safety, check if the original 0F 3A map was selected (pp bits)
                        uint8_t vexByte1 = code[p - code - 3];
                        uint8_t mmmmm = vexByte1 & 0x1F;
                        if (mmmmm == 3) {  // 0F 3A map: has imm8
                            return (int)(p + modrm + 1 - code);
                        }
                        return (int)(p + modrm - code);
                    }
                    return 0;  // LES - unsupported, shouldn't appear in prologues
                case 0xC5:     // VEX 2-byte prefix (x64) or LDS (x86)
                    if (is64bit) {
                        p += 1;  // VEX payload byte
                        p++;     // opcode after VEX
                        int modrm = ParseModRM(p, hasAddrSize);
                        return (int)(p + modrm - code);
                    }
                    return 0;  // LDS - unsupported
                case 0xC8:     // ENTER imm16, imm8
                    return (int)(p - code) + 3;
                case 0xEA:  // JMP FAR (x86 only)
                    return (int)(p - code) + (hasOpSize ? 4 : 6);
                case 0xF6:  // Group 3 byte - TEST r/m8, imm8 (reg=0) or NOT/NEG/MUL/etc
                {
                    int modrm = ParseModRM(p, hasAddrSize);
                    uint8_t reg = (p[0] >> 3) & 7;
                    int immSize = (reg == 0 || reg == 1) ? 1 : 0;  // TEST has imm8
                    return (int)(p + modrm + immSize - code);
                }
                case 0xF7:  // Group 3 dword - TEST r/m32, imm32 (reg=0) or NOT/NEG/MUL/etc
                {
                    int modrm = ParseModRM(p, hasAddrSize);
                    uint8_t reg = (p[0] >> 3) & 7;
                    int immSize = (reg == 0 || reg == 1) ? (hasOpSize ? 2 : 4) : 0;
                    return (int)(p + modrm + immSize - code);
                }
                default:
                    return 0;  // Unknown special
            }
        }
        return 0;  // Unknown two-byte special
    }

    // 4. ModR/M
    if (flags & OP_MODRM) {
        int modrm = ParseModRM(p, hasAddrSize);
        p += modrm;
    }

    // 5. Immediate
    if (flags & OP_IMM8) {
        p += 1;
    }
    if (flags & OP_IMM32) {
        if (hasOpSize && !isTwoByte) {
            p += 2;  // 66h prefix: 16-bit immediate instead of 32-bit
        } else {
            p += 4;
        }
        // Special: MOV r64, imm64 (B8-BF with REX.W)
        if (!isTwoByte && opcode >= 0xB8 && opcode <= 0xBF && hasREX_W) {
            p += 4;  // Additional 4 bytes for 64-bit immediate (total 8)
        }
    }

    int len = (int)(p - code);
    return (len > 0 && len <= 15) ? len : 0;  // x86 max instruction = 15 bytes
}

// ============================================================================
// RIP-Relative Instruction Fixup
// ============================================================================

// Check if an instruction at 'code' uses PC-relative addressing.
// For both 32-bit and 64-bit: CALL/JMP/Jcc rel32 need fixup.
// For 64-bit only: RIP-relative ModR/M addressing also needs fixup.
// Returns the offset of the 32-bit displacement within the instruction,
// or -1 if not PC-relative.
int GetRipRelativeDispOffset(const uint8_t* code, int instrLen, bool is64bit) {
    const uint8_t* p = code;

    // Skip prefixes
    while (p < code + instrLen) {
        uint8_t b = *p;
        if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 || b == 0x26 || b == 0x2E || b == 0x36 ||
            b == 0x3E || b == 0x64 || b == 0x65 || (is64bit && b >= 0x40 && b <= 0x4F)) {
            p++;
        } else {
            break;
        }
    }

    [[maybe_unused]] const uint8_t* opcodeStart = p;
    uint8_t opcode = *p++;
    bool isTwoByte = false;

    if (opcode == 0x0F) {
        opcode = *p++;
        isTwoByte = true;

        // 3-byte escape: 0F 38 or 0F 3A
        if (opcode == 0x38 || opcode == 0x3A) {
            p++;  // skip third opcode byte
        }
    }

    // Check for CALL/JMP rel32 (PC-relative on BOTH 32-bit and 64-bit)
    if (!isTwoByte && (opcode == 0xE8 || opcode == 0xE9)) {
        // The 32-bit displacement starts right after the opcode
        int result = (int)(p - code);
        HookLog("GetRipRelativeDispOffset: Found %s rel32 at offset %d", opcode == 0xE8 ? "CALL" : "JMP", result);
        return result;
    }

    // Check for Jcc rel32 (0F 80-8F) - also PC-relative on both architectures
    if (isTwoByte && opcode >= 0x80 && opcode <= 0x8F) {
        int result = (int)(p - code);
        HookLog("GetRipRelativeDispOffset: Found Jcc rel32 at offset %d", result);
        return result;
    }

    // RIP-relative ModR/M addressing is x64-only
    if (!is64bit) {
        // For 32-bit: CALL/JMP/Jcc are the only PC-relative instructions
        // Other instructions use absolute addressing or register-based
        return -1;
    }

    // Check for ModR/M with RIP-relative addressing
    uint8_t tableFlags;
    if (isTwoByte) {
        tableFlags = g_twoByteOpcodes[opcode];
    } else {
        tableFlags = g_oneByteOpcodes[opcode];
    }

    if (!(tableFlags & OP_MODRM))
        return -1;

    // p now points to the ModR/M byte
    uint8_t modrm = *p;
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t rm = modrm & 7;

    // RIP-relative: mod=00, rm=101 (no SIB)
    if (mod == 0 && rm == 5) {
        return (int)(p + 1 - code);  // disp32 starts after ModR/M
    }

    return -1;
}

// Check if the target function appears to already be hooked.
// Returns true if we detect a common hook pattern at the start of the function.
bool IsAlreadyHooked(const uint8_t* code, bool is64bit) {
    // Check for common inline hook patterns:
    // E9 xx xx xx xx - JMP rel32 (5-byte relative jump)
    // FF 25 xx xx xx xx - JMP [rip+disp32] (6-byte absolute jump on x64)
    // E8 xx xx xx xx - CALL rel32 (sometimes used for hooks)

    uint8_t firstByte = code[0];

    // JMP rel32 (E9) - common hook pattern on both x86 and x64
    if (firstByte == 0xE9) {
        // Read the displacement to log where it points
        int32_t disp;
        memcpy(&disp, code + 1, 4);
        uintptr_t jumpTarget = (uintptr_t)(code + 5) + disp;
        HookLog("InlineHook: IsAlreadyHooked: Detected JMP rel32 at %p (jumps to %p, disp=0x%08X)", (void*)code,
                (void*)jumpTarget, (unsigned)disp);
        return true;
    }

    // On x64, check for FF 25 (JMP [rip+disp32])
    if (is64bit && firstByte == 0xFF && code[1] == 0x25) {
        // Read the RIP-relative displacement
        int32_t disp;
        memcpy(&disp, code + 2, 4);
        uintptr_t addrPtr = (uintptr_t)(code + 6) + disp;
        // Safely read the target address - check if pointer is in valid memory range
        uintptr_t jumpTarget = 0;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void*)addrPtr, &mbi, sizeof(mbi)) > 0 && mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {
            jumpTarget = *(uintptr_t*)addrPtr;
        }
        HookLog("InlineHook: IsAlreadyHooked: Detected JMP [rip+disp32] at %p (indirect addr=%p, target=%p)",
                (void*)code, (void*)addrPtr, (void*)jumpTarget);
        return true;
    }

    // CALL rel32 (E8) - less common but sometimes used
    if (firstByte == 0xE8) {
        int32_t disp;
        memcpy(&disp, code + 1, 4);
        uintptr_t callTarget = (uintptr_t)(code + 5) + disp;
        HookLog("InlineHook: IsAlreadyHooked: Detected CALL rel32 at %p (calls %p, disp=0x%08X)", (void*)code,
                (void*)callTarget, (unsigned)disp);
        return true;
    }

    return false;
}

// Try to restore a function that has been hooked by patching a known prologue.
// This is used when we detect a stale hook from a previous process.
// Returns true if restoration was successful.
[[maybe_unused]] static bool TryRestoreHookedFunction(void* target, bool is64bit) {
    // Common DXGI Present prologue patterns (x64):
    // The standard prologue typically saves rbx, rsi, rdi, rbp, and r12-r15
    // Pattern 1 (most common):
    //   48 89 5C 24 08 - mov [rsp+8], rbx
    //   48 89 6C 24 10 - mov [rsp+10h], rbp
    //   48 89 74 24 18 - mov [rsp+18h], rsi
    // Pattern 2:
    //   48 89 5C 24 10 - mov [rsp+10h], rbx
    //   48 89 74 24 18 - mov [rsp+18h], rsi
    //   55             - push rbp
    //   57             - push rdi
    //   41 56          - push r14

    uint8_t* code = (uint8_t*)target;

    // Check if bytes 5+ match expected prologue pattern
    // This suggests bytes 0-4 were overwritten with a JMP
    bool looksLikeDxgiPresent = false;

    // Check pattern: bytes 5-9 should look like mov [rsp+xx], rxx
    // 48 89 xx 24 yy where xx indicates register:
    //   5C = rbx, 6C = rbp, 74 = rsi, 7C = rdi
    if (code[5] == 0x48 && code[6] == 0x89 && code[8] == 0x24) {
        uint8_t regByte = code[7];
        if (regByte >= 0x5C && regByte <= 0x7C) {
            looksLikeDxgiPresent = true;
            HookLog("InlineHook: TryRestoreHookedFunction: Bytes 5+ look like mov [rsp+%02Xh], r%x", code[9],
                    (regByte >> 3) & 7);
        }
    }
    // Also check for push instructions
    else if (code[5] == 0x55 || code[5] == 0x57 || code[5] == 0x56) {
        looksLikeDxgiPresent = true;
        HookLog("InlineHook: TryRestoreHookedFunction: Bytes 5+ look like push instruction (0x%02X)", code[5]);
    }

    if (!looksLikeDxgiPresent) {
        HookLog("InlineHook: TryRestoreHookedFunction: Cannot restore - unknown prologue pattern at bytes 5+");
        return false;
    }

    // The correct first 5 bytes depend on the pattern we see at bytes 5+
    // Standard convention: the first saved register is rbx (5C)
    // Pattern detection:
    //   If bytes 5-9 = "48 89 74 24 18" (mov [rsp+18h], rsi)
    //   Then bytes 0-4 should be "48 89 5C 24 10" (mov [rsp+10h], rbx)
    //
    //   If bytes 5-9 = "48 89 6C 24 10" (mov [rsp+10h], rbp)
    //   Then bytes 0-4 should be "48 89 5C 24 08" (mov [rsp+8], rbx)

    uint8_t restoredBytes[14];
    memset(restoredBytes, 0, sizeof(restoredBytes));

    // Determine the first instruction based on what's at bytes 5+
    // Key insight: rbx (5C) is always saved first in the standard prologue

    if (code[9] == 0x18 && code[5] == 0x48 && code[6] == 0x89) {
        // Bytes 5-9 are "mov [rsp+18h], rxx"
        // First instruction should be "mov [rsp+10h], rbx"
        restoredBytes[0] = 0x48;
        restoredBytes[1] = 0x89;
        restoredBytes[2] = 0x5C;  // rbx - always rbx first!
        restoredBytes[3] = 0x24;
        restoredBytes[4] = 0x10;
        HookLog("InlineHook: TryRestoreHookedFunction: Inferred prologue: mov [rsp+10h], rbx");
    } else if (code[9] == 0x10 && code[5] == 0x48 && code[6] == 0x89) {
        // Bytes 5-9 are "mov [rsp+10h], rxx"
        // First instruction should be "mov [rsp+8], rbx"
        restoredBytes[0] = 0x48;
        restoredBytes[1] = 0x89;
        restoredBytes[2] = 0x5C;  // rbx - always rbx first!
        restoredBytes[3] = 0x24;
        restoredBytes[4] = 0x08;
        HookLog("InlineHook: TryRestoreHookedFunction: Inferred prologue: mov [rsp+8], rbx");
    } else if (code[5] == 0x55 || code[5] == 0x57 || code[5] == 0x56) {
        // Bytes 5+ start with push
        // First instruction is likely "mov [rsp+10h], rbx" for this pattern
        restoredBytes[0] = 0x48;
        restoredBytes[1] = 0x89;
        restoredBytes[2] = 0x5C;  // rbx
        restoredBytes[3] = 0x24;
        restoredBytes[4] = 0x10;
        HookLog("InlineHook: TryRestoreHookedFunction: Inferred prologue: mov [rsp+10h], rbx (push pattern)");
    } else {
        // Unknown pattern - can't safely restore
        HookLog("InlineHook: TryRestoreHookedFunction: Cannot infer prologue from offset pattern (byte9=0x%02X)",
                code[9]);
        return false;
    }

    // Copy remaining bytes
    memcpy(restoredBytes + 5, code + 5, 9);

    // Make the memory writable and restore
    DWORD oldProtect;
    if (!VirtualProtect(target, 14, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        HookLog("InlineHook: TryRestoreHookedFunction: VirtualProtect failed (error=%lu)", GetLastError());
        return false;
    }

    // Write restored bytes
    memcpy(target, restoredBytes, 14);

    // Restore protection
    DWORD dummy;
    VirtualProtect(target, 14, oldProtect, &dummy);
    FlushInstructionCache(GetCurrentProcess(), target, 14);

    HookLog("InlineHook: TryRestoreHookedFunction: Successfully restored prologue at %p", target);
    HookLog("InlineHook: TryRestoreHookedFunction: Restored bytes:");
    for (int i = 0; i < 14; i++) {
        HookLog("  [%02d] 0x%02X", i, ((uint8_t*)target)[i]);
    }

    return true;
}

}  // namespace InlineHook
