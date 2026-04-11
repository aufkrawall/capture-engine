/**
 * Minimal Inline Hook Implementation
 *
 * Contains:
 * - Length Disassembly Engine (LDE) for x86/x64
 * - Trampoline allocation near target (within ±2GB for RIP-relative fixups)
 * - Hook installation and removal
 */

#include "inline_hook.h"

#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// Forward declaration for HookLog (defined in hook_common.cpp)
void HookLog(const char* fmt, ...);

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
        if (mod == 0 && rm == 6) {
            extra += 2;  // disp16
        } else if (mod == 1) {
            extra += 1;  // disp8
        } else if (mod == 2) {
            extra += 2;  // disp16
        }
    }

    return extra;
}

// Get the length of an instruction at the given address.
// Returns 0 if the instruction cannot be decoded (unsupported/unknown).
static int GetInstructionLength(const uint8_t* code, bool is64bit) {
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
        } else if (b == 0xF0 || b == 0xF2 || b == 0xF3) {
            p++;  // LOCK, REPNE, REP
        } else if (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65) {
            p++;  // Segment overrides
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
                    return (int)(p - code) + (is64bit && !hasAddrSize ? 8 : (hasAddrSize ? 2 : 4));
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
static int GetRipRelativeDispOffset(const uint8_t* code, int instrLen, bool is64bit) {
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

// ============================================================================
// Hook State
// ============================================================================

struct HookEntry {
    void* target;
    void* trampoline;
    uint8_t origBytes[32];
    int patchSize;
    bool installed;
};

static std::vector<HookEntry> g_hooks;
static std::mutex g_hookMutex;
static uint8_t* g_trampolinePool = nullptr;
static std::vector<uint8_t*> g_trampolinePools;
static size_t g_trampolineOffset = 0;
static constexpr size_t TRAMPOLINE_POOL_SIZE = 4096;
static constexpr size_t TRAMPOLINE_ENTRY_SIZE = 64;  // Max per hook

// Deep hook data structures (forward declaration for RemoveAll)
struct DeepHookEntry {
    void* target;           // Original function address
    void* hookAddr;         // Address where the JMP was written (target + resumeOffset)
    int patchSize;          // Size of displaced instructions at hookAddr
    uint8_t origBytes[32];  // Original bytes at hookAddr (for removal)
    uint8_t* trampoline;    // VirtualAlloc'd executable trampoline memory
    bool installed;
};

static std::vector<DeepHookEntry> g_deepHooks;

#ifdef _WIN64
static constexpr int PATCH_SIZE = 14;  // FF 25 00 00 00 00 + 8-byte address
#else
static constexpr int PATCH_SIZE = 5;  // E9 + 4-byte relative offset
#endif

// Allocate executable memory near the target (within ±2GB for x64)
static uint8_t* AllocateTrampolinePool(void* nearAddr) {
#ifdef _WIN64
    // Try to allocate within ±2GB of target for RIP-relative fixups
    uintptr_t target = (uintptr_t)nearAddr;
    uintptr_t low = target > 0x7FFF0000ULL ? target - 0x7FFF0000ULL : 0x10000ULL;
    uintptr_t high = target + 0x7FFF0000ULL;

    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t addr = low; addr < high;) {
        if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == 0)
            break;

        if (mbi.State == MEM_FREE && mbi.RegionSize >= TRAMPOLINE_POOL_SIZE) {
            // Align to allocation granularity (64KB)
            uintptr_t aligned = (addr + 0xFFFF) & ~(uintptr_t)0xFFFF;
            if (aligned + TRAMPOLINE_POOL_SIZE <= addr + mbi.RegionSize) {
                void* p = VirtualAlloc((void*)aligned, TRAMPOLINE_POOL_SIZE, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_EXECUTE_READWRITE);
                if (p)
                    return (uint8_t*)p;
            }
        }
        addr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
#endif
    // Fallback: allocate anywhere
    return (uint8_t*)VirtualAlloc(nullptr, TRAMPOLINE_POOL_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

static bool IsTrampolinePoolNearTarget(const uint8_t* pool, void* nearAddr) {
#ifdef _WIN64
    if (!pool || !nearAddr)
        return false;
    uintptr_t poolAddr = reinterpret_cast<uintptr_t>(pool);
    uintptr_t targetAddr = reinterpret_cast<uintptr_t>(nearAddr);
    int64_t distance = static_cast<int64_t>(targetAddr) - static_cast<int64_t>(poolAddr);
    return distance <= INT32_MAX && distance >= INT32_MIN;
#else
    (void)pool;
    (void)nearAddr;
    return true;
#endif
}

static uint8_t* GetTrampolineSlot(void* nearAddr) {
    bool needNewPool = false;
    if (!g_trampolinePool) {
        needNewPool = true;
    } else if (!IsTrampolinePoolNearTarget(g_trampolinePool, nearAddr)) {
#ifdef _WIN64
        uintptr_t poolAddr = reinterpret_cast<uintptr_t>(g_trampolinePool);
        uintptr_t targetAddr = reinterpret_cast<uintptr_t>(nearAddr);
        int64_t distance = static_cast<int64_t>(targetAddr) - static_cast<int64_t>(poolAddr);
        HookLog("GetTrampolineSlot: Existing pool too far from target (distance=%lld), allocating additional pool",
                static_cast<long long>(distance));
#else
        HookLog("GetTrampolineSlot: Existing pool too far from target, allocating additional pool");
#endif
        needNewPool = true;
    } else if (g_trampolineOffset + TRAMPOLINE_ENTRY_SIZE > TRAMPOLINE_POOL_SIZE) {
        HookLog("GetTrampolineSlot: Existing pool exhausted, allocating additional pool");
        needNewPool = true;
    }

    if (needNewPool) {
        uint8_t* newPool = AllocateTrampolinePool(nearAddr);
        if (!newPool)
            return nullptr;
        g_trampolinePool = newPool;
        g_trampolineOffset = 0;
        g_trampolinePools.push_back(g_trampolinePool);
        HookLog("GetTrampolineSlot: New pool allocated at %p (total pools=%zu)", g_trampolinePool,
                g_trampolinePools.size());
    }

    uint8_t* slot = g_trampolinePool + g_trampolineOffset;
    HookLog("GetTrampolineSlot: Allocating slot at offset %zu (addr=%p) for target %p", g_trampolineOffset, slot,
            nearAddr);
    g_trampolineOffset += TRAMPOLINE_ENTRY_SIZE;
    return slot;
}

// Write an absolute jump at 'dest' to 'target'
static void WriteJump(uint8_t* dest, void* target) {
#ifdef _WIN64
    // FF 25 00 00 00 00 [8-byte address]
    dest[0] = 0xFF;
    dest[1] = 0x25;
    dest[2] = 0x00;
    dest[3] = 0x00;
    dest[4] = 0x00;
    dest[5] = 0x00;
    memcpy(dest + 6, &target, 8);
    HookLog("WriteJump: x64 JMP [RIP+0] -> %p at %p", target, dest);
#else
    // E9 [4-byte relative offset]
    // CRITICAL: The displacement is relative to the instruction AFTER the JMP
    // JMP rel32 means: RIP = (address of next instruction) + rel32
    // So: target = (dest + 5) + rel32
    // Therefore: rel32 = target - (dest + 5)
    dest[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)target - (uintptr_t)(dest + 5));
    memcpy(dest + 1, &rel, 4);
    HookLog("WriteJump: x86 JMP rel32 -> %p at %p (rel=0x%08X, dest+5=%p)", target, dest, (unsigned)rel,
            (void*)(dest + 5));
    // Verify: dest+5 + rel should equal target
    uintptr_t verify = (uintptr_t)(dest + 5) + rel;
    HookLog(
        "WriteJump: Verification: dest+5(0x%p) + rel(0x%08X) = 0x%p "
        "(expected %p)",
        (void*)(dest + 5), (unsigned)rel, (void*)verify, target);
#endif
}

enum class ShortControlRelocationResult {
    kNotHandled,
    kHandled,
    kFailed,
};

static bool IsShortConditionalJumpOpcode(uint8_t opcode) {
    return opcode >= 0x70 && opcode <= 0x7F;
}

static bool IsShortUnconditionalJumpOpcode(uint8_t opcode) {
    return opcode == 0xEB;
}

static bool IsShortLoopControlOpcode(uint8_t opcode) {
    return opcode >= 0xE0 && opcode <= 0xE3;
}

static ShortControlRelocationResult TryRelocateExternalShortControlTransfer(
    const uint8_t* instrBytes, uintptr_t instrAddr, int instrLen, uintptr_t copiedBlockBase, size_t copiedBlockSize,
    uint8_t* trampoline, int* trampolineOffset, bool is64bit, const char* ownerTag) {
    if (!instrBytes || instrLen < 2 || !trampoline || !trampolineOffset) {
        return ShortControlRelocationResult::kNotHandled;
    }

    const uint8_t opcode = instrBytes[0];
    if (!IsShortConditionalJumpOpcode(opcode) && !IsShortUnconditionalJumpOpcode(opcode) &&
        !IsShortLoopControlOpcode(opcode)) {
        return ShortControlRelocationResult::kNotHandled;
    }

    const int8_t origDisp = static_cast<int8_t>(instrBytes[1]);
    const uintptr_t absTarget = instrAddr + instrLen + origDisp;
    const uintptr_t copiedBlockEnd = copiedBlockBase + copiedBlockSize;

    // The GTA FSR->DLSS crash came from a short branch that escaped the copied
    // prologue and then landed inside the trampoline's appended jump stub.
    // Keep intra-block short hops on the byte-for-byte path and rewrite only
    // branches that leave the copied block.
    if (absTarget >= copiedBlockBase && absTarget < copiedBlockEnd) {
        return ShortControlRelocationResult::kNotHandled;
    }

    if (IsShortLoopControlOpcode(opcode)) {
        HookLog("%s: Cannot relocate external short loop/control opcode 0x%02X at %p (target=%p)",
                ownerTag ? ownerTag : "InlineHook", opcode, reinterpret_cast<void*>(instrAddr),
                reinterpret_cast<void*>(absTarget));
        return ShortControlRelocationResult::kFailed;
    }

    if (IsShortUnconditionalJumpOpcode(opcode)) {
        const int64_t newDisp = static_cast<int64_t>(absTarget) -
                                static_cast<int64_t>(reinterpret_cast<uintptr_t>(trampoline + *trampolineOffset + 5));
        if (newDisp >= INT32_MIN && newDisp <= INT32_MAX) {
            trampoline[*trampolineOffset] = 0xE9;
            const int32_t newDisp32 = static_cast<int32_t>(newDisp);
            memcpy(trampoline + *trampolineOffset + 1, &newDisp32, 4);
            *trampolineOffset += 5;
        }
#ifdef _WIN64
        else if (is64bit) {
            WriteJump(trampoline + *trampolineOffset, reinterpret_cast<void*>(absTarget));
            *trampolineOffset += PATCH_SIZE;
        }
#endif
        else {
            HookLog("%s: Cannot relocate external short JMP at %p (target=%p, x64=%d)",
                    ownerTag ? ownerTag : "InlineHook", reinterpret_cast<void*>(instrAddr),
                    reinterpret_cast<void*>(absTarget), is64bit ? 1 : 0);
            return ShortControlRelocationResult::kFailed;
        }

        HookLog("%s: Rewrote external short JMP at %p to target %p", ownerTag ? ownerTag : "InlineHook",
                reinterpret_cast<void*>(instrAddr), reinterpret_cast<void*>(absTarget));
        return ShortControlRelocationResult::kHandled;
    }

    const uint8_t condCode = static_cast<uint8_t>(opcode & 0x0F);
    const int64_t newDisp = static_cast<int64_t>(absTarget) -
                            static_cast<int64_t>(reinterpret_cast<uintptr_t>(trampoline + *trampolineOffset + 6));
    if (newDisp >= INT32_MIN && newDisp <= INT32_MAX) {
        trampoline[*trampolineOffset] = 0x0F;
        trampoline[*trampolineOffset + 1] = static_cast<uint8_t>(0x80 | condCode);
        const int32_t newDisp32 = static_cast<int32_t>(newDisp);
        memcpy(trampoline + *trampolineOffset + 2, &newDisp32, 4);
        *trampolineOffset += 6;
    }
#ifdef _WIN64
    else if (is64bit) {
        trampoline[*trampolineOffset] = static_cast<uint8_t>(0x70 | (condCode ^ 1u));
        trampoline[*trampolineOffset + 1] = static_cast<uint8_t>(PATCH_SIZE);
        WriteJump(trampoline + *trampolineOffset + 2, reinterpret_cast<void*>(absTarget));
        *trampolineOffset += 2 + PATCH_SIZE;
    }
#endif
    else {
        HookLog("%s: Cannot relocate external short Jcc opcode 0x%02X at %p (target=%p, x64=%d)",
                ownerTag ? ownerTag : "InlineHook", opcode, reinterpret_cast<void*>(instrAddr),
                reinterpret_cast<void*>(absTarget), is64bit ? 1 : 0);
        return ShortControlRelocationResult::kFailed;
    }

    HookLog("%s: Rewrote external short Jcc opcode 0x%02X at %p to target %p", ownerTag ? ownerTag : "InlineHook",
            opcode, reinterpret_cast<void*>(instrAddr), reinterpret_cast<void*>(absTarget));
    return ShortControlRelocationResult::kHandled;
}

// ============================================================================
// Public API
// ============================================================================

// Check if the target function appears to already be hooked.
// Returns true if we detect a common hook pattern at the start of the function.
static bool IsAlreadyHooked(const uint8_t* code, bool is64bit) {
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

bool Install(void* target, void* detour, void** outTrampoline) {
    // Use existing optional hook logger; avoid absolute-path file writes from injected code.
    auto LogDirect = [](const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        char buf[1024];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
        vsnprintf(buf, sizeof(buf), fmt, args);
#pragma GCC diagnostic pop
        va_end(args);
        HookLog("%s", buf);
    };

    LogDirect("=== Install called: target=%p, detour=%p", target, detour);

    if (!target || !detour || !outTrampoline) {
        LogDirect("FAILED: null parameter");
        return false;
    }

    std::lock_guard<std::mutex> lock(g_hookMutex);

    // Check if already hooked
    for (auto& h : g_hooks) {
        if (h.target == target && h.installed) {
            LogDirect("FAILED: Target %p already hooked by us", target);
            HookLog("InlineHook: Target %p already hooked", target);
            return false;
        }
    }

#ifdef _WIN64
    bool is64bit = true;
#else
    bool is64bit = false;
#endif

    LogDirect("is64bit=%d, checking if externally hooked...", is64bit ? 1 : 0);

    // CRITICAL: Check if the target function appears to already be hooked
    // by another component (another DLL, another process, overlay, etc.)
    // CRITICAL FIX: Do NOT attempt to restore the prologue - the guessing is
    // unreliable and causes black screens when the guess is wrong.
    // Instead, skip hooking entirely and let the external overlay handle things.
    const uint8_t* code = (const uint8_t*)target;

    // Dump first bytes of target
    // SECURITY FIX: Use safe string concatenation
    char firstBytes[64] = {0};
    size_t remaining = sizeof(firstBytes) - 1;
    char* dest = firstBytes;
    for (int i = 0; i < 8 && remaining > 3; i++) {
        int written = snprintf(dest, remaining, "%02X ", code[i]);
        if (written > 0 && (size_t)written < remaining) {
            dest += written;
            remaining -= written;
        }
    }
    LogDirect("First bytes of target: %s", firstBytes);

    if (IsAlreadyHooked(code, is64bit)) {
        // External overlay detected - try chain hooking
        // Resolve the chain target from either a JMP rel32 (E9) or indirect JMP (FF 25)
        LogDirect("External hook detected, attempting chain hooking...");

        uintptr_t chainTarget = 0;

        if (code[0] == 0xE9) {
            // JMP rel32 - valid on both x86 and x64
            int32_t rel32 = *(const int32_t*)(code + 1);
            uintptr_t overlayTarget = (uintptr_t)target + 5 + rel32;
            LogDirect("JMP rel32 detected: rel32=0x%08X, overlay target=%p", (unsigned)rel32, (void*)overlayTarget);
            HookLog("InlineHook: Chaining to overlay hook at %p", (void*)overlayTarget);

            // Follow one more level of JMP rel32 if present (multi-level chaining)
            const uint8_t* overlayCode = (const uint8_t*)overlayTarget;
            if (overlayCode[0] == 0xE9) {
                LogDirect("Overlay target also has JMP, following chain...");
                int32_t rel32_2 = *(const int32_t*)(overlayCode + 1);
                uintptr_t finalTarget = overlayTarget + 5 + rel32_2;
                LogDirect("Second JMP target: %p", (void*)finalTarget);
                overlayTarget = finalTarget;
            }
            chainTarget = overlayTarget;
        } else if (is64bit && code[0] == 0xFF && code[1] == 0x25) {
            // JMP [rip+disp32] on x64 - dereference indirect pointer to find real target
            int32_t disp;
            memcpy(&disp, code + 2, 4);
            uintptr_t addrPtr = (uintptr_t)(code + 6) + disp;
            MEMORY_BASIC_INFORMATION mbiFF;
            if (VirtualQuery((void*)addrPtr, &mbiFF, sizeof(mbiFF)) > 0 && mbiFF.State == MEM_COMMIT &&
                (mbiFF.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {
                chainTarget = *(uintptr_t*)addrPtr;
                LogDirect("JMP [rip+disp32] chain: indirect addr=%p, real target=%p", (void*)addrPtr,
                          (void*)chainTarget);
            } else {
                LogDirect("FAILED: Cannot read FF25 indirect address %p", (void*)addrPtr);
            }
        }

        if (chainTarget != 0) {
            // Verify the chain target is executable
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery((void*)chainTarget, &mbi, sizeof(mbi)) == sizeof(mbi)) {
                LogDirect("Overlay target memory: Base=%p, Protect=0x%X", mbi.BaseAddress, mbi.Protect);

                // CRITICAL: Check if chain target is inside a known overlay module.
                // Installing a hook inside Steam/discord overlay code causes infinite recursion
                // because the overlay's trampoline calls back into the original function.
                HMODULE hModule = nullptr;
                if (GetModuleHandleExA(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCSTR)chainTarget, &hModule)) {
                    char moduleName[MAX_PATH] = {};
                    GetModuleFileNameA(hModule, moduleName, MAX_PATH);
                    std::string modLower(moduleName);
                    std::transform(modLower.begin(), modLower.end(), modLower.begin(), ::tolower);
                    if (modLower.find("gameoverlayrenderer") != std::string::npos ||
                        modLower.find("d3doverlay") != std::string::npos ||
                        modLower.find("discord") != std::string::npos || modLower.find("nvidia") != std::string::npos ||
                        modLower.find("amd") != std::string::npos) {
                        LogDirect("Chain target is inside overlay module %s - skipping to avoid recursion", moduleName);
                        HookLog("InlineHook: Skipping chain hook into overlay module %s (would cause recursion)",
                                moduleName);
                        LogDirect("FAILED: Function at %p is already hooked by external overlay", target);
                        HookLog("InlineHook: Function at %p is already hooked by external overlay", target);
                        HookLog("InlineHook: Chain hooking skipped (overlay module protection)");
                        return false;
                    }
                }

                if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
                    LogDirect("Overlay target is executable, attempting hook...");

                    if (!IsAlreadyHooked((const uint8_t*)chainTarget, is64bit)) {
                        LogDirect("Attempting chain install at overlay target %p", (void*)chainTarget);

                        // Decode enough instructions to cover PATCH_SIZE bytes
                        int chainCopySize = 0;
                        const uint8_t* chainCode = (const uint8_t*)chainTarget;
                        while (chainCopySize < PATCH_SIZE) {
                            int len = GetInstructionLength(chainCode + chainCopySize, is64bit);
                            if (len == 0) {
                                LogDirect("FAILED: Cannot decode instruction at overlay target+%d", chainCopySize);
                                break;
                            }
                            chainCopySize += len;
                        }

                        if (chainCopySize >= PATCH_SIZE) {
                            LogDirect("Chain hook: copySize=%d, installing at %p", chainCopySize, (void*)chainTarget);

                            // Allocate trampoline near chain target for RIP-relative safety
                            uint8_t* chainTrampoline = GetTrampolineSlot((void*)chainTarget);
                            if (!chainTrampoline) {
                                LogDirect("FAILED: Could not allocate trampoline for chain hook");
                                return false;
                            }
                            LogDirect("Chain hook trampoline allocated at %p", chainTrampoline);

                            // Copy instructions to trampoline with RIP-relative fixups.
                            // On x64 instructions may reference data via RIP+disp32; the displacement
                            // must be recomputed when the instruction executes from a different address.
                            int trampolineOff = 0;
                            int srcOff = 0;
                            bool fixupFailed = false;
                            uintptr_t chainPendingAbsCallTarget = 0;
                            bool chainHasPendingAbsCall = false;
                            int chainPendingCallInstrOff = -1;
                            while (srcOff < chainCopySize) {
                                int instrLen = GetInstructionLength(chainCode + srcOff, is64bit);

                                const auto shortBranchResult = TryRelocateExternalShortControlTransfer(
                                    chainCode + srcOff, reinterpret_cast<uintptr_t>(chainCode + srcOff), instrLen,
                                    reinterpret_cast<uintptr_t>(chainCode), chainCopySize, chainTrampoline,
                                    &trampolineOff, is64bit, "InlineHook(chain)");
                                if (shortBranchResult == ShortControlRelocationResult::kFailed) {
                                    LogDirect("Chain hook: short control relocation failed at srcOff=%d", srcOff);
                                    fixupFailed = true;
                                    break;
                                }
                                if (shortBranchResult == ShortControlRelocationResult::kHandled) {
                                    srcOff += instrLen;
                                    continue;
                                }

                                memcpy(chainTrampoline + trampolineOff, chainCode + srcOff, instrLen);

                                int dispOff = GetRipRelativeDispOffset(chainCode + srcOff, instrLen, is64bit);
                                if (dispOff >= 0) {
                                    int32_t origDisp;
                                    memcpy(&origDisp, chainCode + srcOff + dispOff, 4);
                                    uintptr_t absTarget = (uintptr_t)(chainCode + srcOff + instrLen) + origDisp;
                                    uintptr_t newInstrEnd = (uintptr_t)(chainTrampoline + trampolineOff + instrLen);
                                    int64_t newDisp = (int64_t)absTarget - (int64_t)newInstrEnd;

                                    if (newDisp > INT32_MAX || newDisp < INT32_MIN) {
                                        uint8_t op = chainCode[srcOff];
                                        if (op == 0xE9 || op == 0xE8) {
                                            if (op == 0xE9) {
                                                // JMP: FF 25 00 00 00 00 [8-byte address]
                                                chainTrampoline[trampolineOff] = 0xFF;
                                                chainTrampoline[trampolineOff + 1] = 0x25;
                                                chainTrampoline[trampolineOff + 2] = 0;
                                                chainTrampoline[trampolineOff + 3] = 0;
                                                chainTrampoline[trampolineOff + 4] = 0;
                                                chainTrampoline[trampolineOff + 5] = 0;
                                                memcpy(chainTrampoline + trampolineOff + 6, &absTarget, 8);
                                                trampolineOff += 14;
                                            } else {
                                                // CALL: write FF 15 with placeholder; patch disp
                                                // after loop+JMP-back when ptr location is known.
                                                chainPendingCallInstrOff = trampolineOff;
                                                chainTrampoline[trampolineOff] = 0xFF;
                                                chainTrampoline[trampolineOff + 1] = 0x15;
                                                chainTrampoline[trampolineOff + 2] = 0;  // placeholder
                                                chainTrampoline[trampolineOff + 3] = 0;
                                                chainTrampoline[trampolineOff + 4] = 0;
                                                chainTrampoline[trampolineOff + 5] = 0;
                                                trampolineOff += 6;
                                                chainPendingAbsCallTarget = absTarget;
                                                chainHasPendingAbsCall = true;
                                            }
                                            srcOff += instrLen;
                                            continue;
                                        }
                                        LogDirect("Chain hook: RIP fixup out of range at srcOff=%d, aborting", srcOff);
                                        fixupFailed = true;
                                        break;
                                    }
                                    int32_t newDisp32 = (int32_t)newDisp;
                                    memcpy(chainTrampoline + trampolineOff + dispOff, &newDisp32, 4);
                                }
                                trampolineOff += instrLen;
                                srcOff += instrLen;
                            }
                            if (fixupFailed || static_cast<size_t>(trampolineOff + 5) > TRAMPOLINE_ENTRY_SIZE) {
                                LogDirect("Chain hook: trampoline build failed (off=%d)", trampolineOff);
                                return false;
                            }

                            // Add JMP back to chain code after the copied bytes.
                            // E9 rel32 is safe: trampoline is within ±2GB of chainTarget.
                            uint8_t* jmpSite = chainTrampoline + trampolineOff;
                            jmpSite[0] = 0xE9;  // JMP rel32
                            int32_t jmpOffset =
                                (int32_t)((uintptr_t)(chainCode + chainCopySize) - (uintptr_t)(jmpSite + 5));
                            memcpy(jmpSite + 1, &jmpOffset, 4);
                            // If a CALL abs conversion was deferred, its ptr goes after the
                            // E9 JMP-back. Patch the displacement in the FF 15 instruction now
                            // that we know both locations.
                            if (chainHasPendingAbsCall) {
                                uint8_t* ptrAddr = jmpSite + 5;  // right after the 5-byte E9 JMP
                                int32_t disp =
                                    (int32_t)((uint8_t*)ptrAddr - (chainTrampoline + chainPendingCallInstrOff + 6));
                                memcpy(chainTrampoline + chainPendingCallInstrOff + 2, &disp, 4);
                                memcpy(ptrAddr, &chainPendingAbsCallTarget, 8);
                            }
                            LogDirect("Chain trampoline: %d src bytes -> %d trampoline bytes, JMP -> %p (rel=0x%08X)",
                                      chainCopySize, trampolineOff, (void*)(chainCode + chainCopySize),
                                      (unsigned)jmpOffset);

                            // Create the hook entry
                            HookEntry newHook = {};
                            newHook.target = (void*)chainTarget;
                            newHook.trampoline = chainTrampoline;
                            newHook.patchSize = chainCopySize;
                            newHook.installed = false;
                            memcpy(newHook.origBytes, chainCode, chainCopySize);

                            // Save trampoline to caller's storage
                            if (outTrampoline) {
                                *outTrampoline = chainTrampoline;
                            }

                            // Patch chain target: use WriteJump for proper x64 support
                            // (14-byte FF25+addr on x64, 5-byte E9 rel32 on x86)
                            DWORD oldProtect;
                            if (VirtualProtect((void*)chainTarget, chainCopySize, PAGE_EXECUTE_READWRITE,
                                               &oldProtect)) {
                                WriteJump((uint8_t*)chainTarget, detour);
                                // NOP fill remaining bytes beyond PATCH_SIZE
                                for (int i = PATCH_SIZE; i < chainCopySize; i++) {
                                    ((uint8_t*)chainTarget)[i] = 0x90;
                                }

                                FlushInstructionCache(GetCurrentProcess(), (void*)chainTarget, chainCopySize);

                                DWORD dummy;
                                VirtualProtect((void*)chainTarget, chainCopySize, oldProtect, &dummy);

                                newHook.installed = true;
                                g_hooks.push_back(newHook);

                                LogDirect("SUCCESS: Chain hook installed at %p -> %p (trampoline=%p)",
                                          (void*)chainTarget, detour, chainTrampoline);
                                HookLog("InlineHook: Chain hook installed at %p (trampoline=%p)", (void*)chainTarget,
                                        chainTrampoline);
                                return true;
                            } else {
                                LogDirect("FAILED: VirtualProtect failed for chain target");
                            }
                        }
                    } else {
                        LogDirect("Overlay target also appears hooked, skipping");
                    }
                } else {
                    LogDirect("Overlay target is NOT executable (Protect=0x%X)", mbi.Protect);
                }
            } else {
                LogDirect("VirtualQuery failed for overlay target");
            }
        }

        LogDirect("FAILED: Function at %p is already hooked by external overlay", target);
        HookLog("InlineHook: Function at %p is already hooked by external overlay", target);
        HookLog("InlineHook: Chain hooking failed or not supported");
        return false;
    }

    LogDirect("Not externally hooked, decoding instructions...");

    // Determine how many bytes to copy (must be >= PATCH_SIZE on instruction
    // boundary)
    int copySize = 0;
    while (copySize < PATCH_SIZE) {
        int len = GetInstructionLength(code + copySize, is64bit);
        if (len == 0) {
            LogDirect("FAILED: Failed to decode instruction at %p+%d (byte=0x%02X)", target, copySize, code[copySize]);
            HookLog(
                "InlineHook: Failed to decode instruction at %p+%d "
                "(byte=0x%02X)",
                target, copySize, code[copySize]);
            return false;
        }
        copySize += len;
    }

    LogDirect("Instructions decoded, copySize=%d bytes", copySize);
    HookLog("InlineHook: Hooking %p, patch=%d bytes, detour=%p, is64bit=%d", target, copySize, detour, is64bit ? 1 : 0);

    // Dump original bytes for diagnosis
    // SECURITY FIX: Use safe string concatenation
    char bytesStr[256] = {0};
    size_t bytesRemaining = sizeof(bytesStr) - 1;
    char* bytesDest = bytesStr;
    for (int i = 0; i < copySize && i < 16 && bytesRemaining > 3; i++) {
        int written = snprintf(bytesDest, bytesRemaining, "%02X ", code[i]);
        if (written > 0 && (size_t)written < bytesRemaining) {
            bytesDest += written;
            bytesRemaining -= written;
        }
    }
    LogDirect("Original bytes: %s", bytesStr);

    HookLog("InlineHook: Original bytes at %p:", target);
    for (int i = 0; i < copySize && i < 16; i++) {
        HookLog("  [%02d] 0x%02X", i, code[i]);
    }

    // Allocate trampoline
    LogDirect("Allocating trampoline...");
    uint8_t* trampoline = GetTrampolineSlot(target);
    if (!trampoline) {
        LogDirect("FAILED: Failed to allocate trampoline");
        HookLog("InlineHook: Failed to allocate trampoline");
        return false;
    }
    LogDirect("Trampoline allocated at %p", trampoline);
    HookLog("InlineHook: Trampoline allocated at %p", trampoline);

    // Copy original instructions to trampoline, fixing up RIP-relative refs
    int trampolineOffset = 0;
    int srcOffset = 0;
    // For CALL rel32→absolute: write FF 15 [placeholder] now, patch displacement after loop.
    uintptr_t pendingAbsCallTarget = 0;
    bool hasPendingAbsCall = false;
    int pendingCallInstrOffset = -1;  // trampoline offset where the FF 15 CALL was written
    while (srcOffset < copySize) {
        int instrLen = GetInstructionLength(code + srcOffset, is64bit);

        // Log instruction being copied
        HookLog("InlineHook: Copying instruction at offset %d, len=%d:", srcOffset, instrLen);
        for (int i = 0; i < instrLen && i < 8; i++) {
            HookLog("  [%02d] 0x%02X", i, code[srcOffset + i]);
        }

        const auto shortBranchResult = TryRelocateExternalShortControlTransfer(
            code + srcOffset, reinterpret_cast<uintptr_t>(code + srcOffset), instrLen,
            reinterpret_cast<uintptr_t>(code), copySize, trampoline, &trampolineOffset, is64bit, "InlineHook");
        if (shortBranchResult == ShortControlRelocationResult::kFailed) {
            return false;
        }
        if (shortBranchResult == ShortControlRelocationResult::kHandled) {
            srcOffset += instrLen;
            continue;
        }

        memcpy(trampoline + trampolineOffset, code + srcOffset, instrLen);

        // Fix up RIP-relative addressing
        int dispOff = GetRipRelativeDispOffset(code + srcOffset, instrLen, is64bit);
        if (dispOff >= 0) {
            // Read original displacement
            int32_t origDisp;
            memcpy(&origDisp, code + srcOffset + dispOff, 4);

            // Calculate absolute target address
            // RIP-relative: target = instruction_end + displacement
            uintptr_t absTarget = (uintptr_t)(code + srcOffset + instrLen) + origDisp;

            // Calculate new displacement from trampoline position
            uintptr_t newInstrEnd = (uintptr_t)(trampoline + trampolineOffset + instrLen);
            int64_t newDisp = (int64_t)absTarget - (int64_t)newInstrEnd;

            HookLog(
                "InlineHook: PC-relative fixup at srcOff=%d, dispOff=%d, "
                "origDisp=0x%08X, absTarget=%p, newInstrEnd=%p, newDisp=0x%08llX",
                srcOffset, dispOff, (unsigned)origDisp, (void*)absTarget, (void*)newInstrEnd, (long long)newDisp);

            if (newDisp > INT32_MAX || newDisp < INT32_MIN) {
                // Check if this is a JMP rel32 (0xE9) or CALL rel32 (0xE8) that we can convert to absolute
                uint8_t opcode = code[srcOffset];
                if (opcode == 0xE9 || opcode == 0xE8) {
                    HookLog("InlineHook: Converting %s rel32 to absolute at offset %d (target=%p)",
                            opcode == 0xE9 ? "JMP" : "CALL", srcOffset, (void*)absTarget);

                    if (opcode == 0xE9) {
                        // JMP: FF 25 00 00 00 00 [8-byte address] (14 bytes).
                        // Never returns, so pointer-after-instruction layout is fine.
                        trampoline[trampolineOffset] = 0xFF;
                        trampoline[trampolineOffset + 1] = 0x25;
                        trampoline[trampolineOffset + 2] = 0x00;
                        trampoline[trampolineOffset + 3] = 0x00;
                        trampoline[trampolineOffset + 4] = 0x00;
                        trampoline[trampolineOffset + 5] = 0x00;
                        memcpy(trampoline + trampolineOffset + 6, &absTarget, 8);
                        trampolineOffset += 14;
                    } else {
                        // CALL: write FF 15 with a placeholder displacement; we patch the
                        // real disp AFTER the loop+WriteJump when we know the ptr location.
                        // Return address = trampolineOffset+6 (post-fetch RIP).
                        // Ptr is written after WriteJump; disp is patched at that point.
                        pendingCallInstrOffset = trampolineOffset;
                        trampoline[trampolineOffset] = 0xFF;
                        trampoline[trampolineOffset + 1] = 0x15;
                        trampoline[trampolineOffset + 2] = 0;  // placeholder
                        trampoline[trampolineOffset + 3] = 0;
                        trampoline[trampolineOffset + 4] = 0;
                        trampoline[trampolineOffset + 5] = 0;
                        trampolineOffset += 6;  // only the CALL instruction; pointer written later
                        pendingAbsCallTarget = absTarget;
                        hasPendingAbsCall = true;
                    }
                    srcOffset += instrLen;
                    continue;  // Skip the normal fixup path
                }

                HookLog("InlineHook: RIP-relative fixup out of range at %p+%d (opcode=0x%02X)", target, srcOffset,
                        opcode);
                return false;
            }

            int32_t newDisp32 = (int32_t)newDisp;
            memcpy(trampoline + trampolineOffset + dispOff, &newDisp32, 4);
        } else {
            HookLog(
                "InlineHook: No PC-relative fixup needed for instruction at "
                "offset %d",
                srcOffset);
        }

        trampolineOffset += instrLen;
        srcOffset += instrLen;
    }

    // Add jump back to original function after the patched area
    void* jumpTarget = (void*)(code + copySize);
    HookLog("InlineHook: Writing jump back from trampoline+%d to %p (original+%d)", trampolineOffset, jumpTarget,
            copySize);
    WriteJump(trampoline + trampolineOffset, jumpTarget);
    trampolineOffset += PATCH_SIZE;

    // If a CALL rel32→absolute was converted, its target pointer goes here
    // (after the WriteJump), so the CALL return address correctly falls through
    // to the WriteJump continuation above.
    if (hasPendingAbsCall) {
        // Ptr lands here (after WriteJump). Patch the displacement back into the
        // FF 15 instruction: disp = ptrOffset - returnAddrOffset
        //   returnAddr = trampoline + pendingCallInstrOffset + 6  (post-fetch RIP)
        //   ptrOffset   = trampoline + trampolineOffset
        int32_t disp = trampolineOffset - (pendingCallInstrOffset + 6);
        memcpy(trampoline + pendingCallInstrOffset + 2, &disp, 4);
        memcpy(trampoline + trampolineOffset, &pendingAbsCallTarget, 8);
        trampolineOffset += 8;
    }

    // Dump trampoline bytes for diagnosis
    HookLog("InlineHook: Trampoline bytes (%d bytes total):", trampolineOffset);
    for (int i = 0; i < trampolineOffset && i < 32; i++) {
        HookLog("  [%02d] 0x%02X", i, trampoline[i]);
    }

    // Save original bytes
    HookEntry entry = {};
    entry.target = target;
    entry.trampoline = trampoline;
    entry.patchSize = copySize;
    entry.installed = true;
    memcpy(entry.origBytes, code, copySize);

    LogDirect("Patching target function...");
    // Patch the target function
    DWORD oldProtect;
    if (!VirtualProtect(target, copySize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogDirect("FAILED: VirtualProtect failed (error=%lu)", GetLastError());
        HookLog("InlineHook: VirtualProtect failed (error=%lu)", GetLastError());
        return false;
    }
    LogDirect("VirtualProtect succeeded, oldProtect=0x%08X", oldProtect);

    // Write the jump to the detour function FIRST, then NOP-fill the rest,
    // then do a single FlushInstructionCache. This avoids the race window
    // where another thread could execute past a partial INT3 fill.
    volatile uint8_t* pTarget = (volatile uint8_t*)target;
#ifdef _WIN64
    // x64: Use absolute jump via [RIP+0] - 14 bytes total
    // FF 25 00 00 00 00 [8-byte absolute address]
    uint8_t jmpBuf[14];
    jmpBuf[0] = 0xFF;
    jmpBuf[1] = 0x25;
    jmpBuf[2] = 0x00;
    jmpBuf[3] = 0x00;
    jmpBuf[4] = 0x00;
    jmpBuf[5] = 0x00;
    memcpy(jmpBuf + 6, &detour, 8);

    // Copy all bytes to target
    for (int i = 0; i < PATCH_SIZE; i++) {
        pTarget[i] = jmpBuf[i];
    }
#else
    // x86: Calculate displacement for the actual target location
    // JMP rel32: target = (instruction_address + 5) + displacement
    pTarget[0] = 0xE9;  // JMP rel32 opcode
    int32_t rel = (int32_t)((uintptr_t)detour - (uintptr_t)((uint8_t*)target + 5));
    memcpy((void*)(pTarget + 1), &rel, 4);
    HookLog("InlineHook: Target JMP at %p -> %p (rel=0x%08X)", target, detour, (unsigned)rel);
    // Verify the calculation
    uintptr_t verify = (uintptr_t)((uint8_t*)target + 5) + rel;
    HookLog("InlineHook: Verification: %p + 5 + 0x%08X = %p (expected %p)", target, (unsigned)rel, (void*)verify,
            detour);
#endif

    // Fill any remaining bytes after the jump with NOPs
    for (int i = PATCH_SIZE; i < copySize; i++) {
        pTarget[i] = 0x90;
    }

    VirtualProtect(target, copySize, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, copySize);

    g_hooks.push_back(entry);
    *outTrampoline = trampoline;

    LogDirect("SUCCESS: Hook installed at %p -> %p (trampoline=%p)", target, detour, trampoline);
    HookLog("InlineHook: Installed hook at %p -> %p (trampoline=%p)", target, detour, trampoline);
    return true;
}

bool Remove(void* target) {
    if (!target)
        return false;

    std::lock_guard<std::mutex> lock(g_hookMutex);

    for (auto& h : g_hooks) {
        if (h.target == target && h.installed) {
            DWORD oldProtect;
            if (VirtualProtect(h.target, h.patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                memcpy(h.target, h.origBytes, h.patchSize);
                VirtualProtect(h.target, h.patchSize, oldProtect, &oldProtect);
                FlushInstructionCache(GetCurrentProcess(), h.target, h.patchSize);
                h.installed = false;
                HookLog("InlineHook: Removed hook at %p", target);
                return true;
            }
            HookLog(
                "InlineHook: Failed to remove hook at %p (VirtualProtect "
                "error=%lu)",
                target, GetLastError());
            return false;
        }
    }

    HookLog("InlineHook: No hook found at %p", target);
    return false;
}

void RemoveAll() {
    std::lock_guard<std::mutex> lock(g_hookMutex);

    HookLog("InlineHook::RemoveAll() called - removing %zu hooks", g_hooks.size());

    for (auto& h : g_hooks) {
        if (h.installed) {
            DWORD oldProtect;
            if (VirtualProtect(h.target, h.patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                memcpy(h.target, h.origBytes, h.patchSize);
                VirtualProtect(h.target, h.patchSize, oldProtect, &oldProtect);
                FlushInstructionCache(GetCurrentProcess(), h.target, h.patchSize);
            }
            h.installed = false;
        }
    }
    g_hooks.clear();

    // Also remove deep hooks
    for (auto& d : g_deepHooks) {
        if (d.installed) {
            DWORD oldProtect;
            if (VirtualProtect(d.hookAddr, d.patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                memcpy(d.hookAddr, d.origBytes, d.patchSize);
                VirtualProtect(d.hookAddr, d.patchSize, oldProtect, &oldProtect);
                FlushInstructionCache(GetCurrentProcess(), d.hookAddr, d.patchSize);
            }
            d.installed = false;
        }
        if (d.trampoline) {
            VirtualFree(d.trampoline, 0, MEM_RELEASE);
            d.trampoline = nullptr;
        }
    }
    g_deepHooks.clear();

    if (!g_trampolinePools.empty()) {
        for (uint8_t* pool : g_trampolinePools) {
            if (pool) {
                HookLog("InlineHook::RemoveAll() - freeing trampoline pool at %p", pool);
                VirtualFree(pool, 0, MEM_RELEASE);
            }
        }
        g_trampolinePools.clear();
    } else if (g_trampolinePool) {
        // Backward compatibility for any pool not tracked in the vector.
        HookLog("InlineHook::RemoveAll() - freeing trampoline pool at %p", g_trampolinePool);
        VirtualFree(g_trampolinePool, 0, MEM_RELEASE);
    }
    g_trampolinePool = nullptr;
    g_trampolineOffset = 0;
}

// ============================================================================
// Deep Hook Implementation
// ============================================================================
//
// A "deep hook" patches the function body PAST an external hook's JMP patch.
// This catches callers that use saved trampolines (e.g. Streamline's internal
// DXGI calls) which bypass the JMP at byte 0.
//
// The hook undoes the initial push prolog and redirects to a caller-provided
// wrapper function with the same calling convention as the original. A full
// trampoline is built containing the complete original prolog, allowing the
// wrapper to call through to the real function and capture the return value.

// Read original (unpatched) function bytes from the DLL file on disk.
static bool ReadOrigBytesFromDisk(void* funcAddr, uint8_t* outBuf, int count) {
    HMODULE hMod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)funcAddr, &hMod) ||
        !hMod) {
        return false;
    }

    char modPath[MAX_PATH];
    if (!GetModuleFileNameA(hMod, modPath, MAX_PATH))
        return false;

    uintptr_t rva = (uintptr_t)funcAddr - (uintptr_t)hMod;

    HANDLE hFile =
        CreateFileA(modPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    bool success = false;
    DWORD br;

    IMAGE_DOS_HEADER dosH;
    if (!ReadFile(hFile, &dosH, sizeof(dosH), &br, nullptr) || dosH.e_magic != IMAGE_DOS_SIGNATURE) {
        CloseHandle(hFile);
        return false;
    }

    if (SetFilePointer(hFile, dosH.e_lfanew, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
        CloseHandle(hFile);
        return false;
    }

    DWORD sig;
    if (!ReadFile(hFile, &sig, 4, &br, nullptr) || sig != IMAGE_NT_SIGNATURE) {
        CloseHandle(hFile);
        return false;
    }

    IMAGE_FILE_HEADER fh;
    if (!ReadFile(hFile, &fh, sizeof(fh), &br, nullptr)) {
        CloseHandle(hFile);
        return false;
    }

    // Skip optional header to reach section headers
    if (SetFilePointer(hFile, fh.SizeOfOptionalHeader, nullptr, FILE_CURRENT) == INVALID_SET_FILE_POINTER) {
        CloseHandle(hFile);
        return false;
    }

    for (WORD i = 0; i < fh.NumberOfSections; i++) {
        IMAGE_SECTION_HEADER sh;
        if (!ReadFile(hFile, &sh, sizeof(sh), &br, nullptr))
            break;

        if (rva >= sh.VirtualAddress && rva < sh.VirtualAddress + sh.Misc.VirtualSize) {
            DWORD fileOff = sh.PointerToRawData + (DWORD)(rva - sh.VirtualAddress);
            if (SetFilePointer(hFile, fileOff, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER) {
                if (ReadFile(hFile, outBuf, (DWORD)count, &br, nullptr) && (int)br == count)
                    success = true;
            }
            break;
        }
    }

    CloseHandle(hFile);
    return success;
}

void* InstallDeepHook(void* target, void* wrapperFn) {
#ifndef _WIN64
    HookLog("DeepHook: Only supported on x64");
    return nullptr;
#else
    if (!target || !wrapperFn)
        return nullptr;

    std::lock_guard<std::mutex> lock(g_hookMutex);

    // Check not already deep-hooked
    for (auto& d : g_deepHooks) {
        if (d.target == target && d.installed) {
            HookLog("DeepHook: Target %p already deep-hooked", target);
            return nullptr;
        }
    }

    const uint8_t* code = (const uint8_t*)target;

    // Step 1: Detect existing hook at byte 0
    int existingJmpSize = 0;
    if (code[0] == 0xE9) {
        existingJmpSize = 5;
    } else if (code[0] == 0xFF && code[1] == 0x25) {
        existingJmpSize = 14;
    } else {
        HookLog("DeepHook: No external hook at byte 0 of %p (byte=0x%02X)", target, code[0]);
        return nullptr;
    }

    HookLog("DeepHook: External %d-byte JMP detected at %p", existingJmpSize, target);

    // Step 2: Read original (unpatched) bytes from DLL on disk
    uint8_t origDiskBytes[64];
    if (!ReadOrigBytesFromDisk(target, origDiskBytes, 64)) {
        HookLog("DeepHook: Failed to read original bytes from disk for %p", target);
        return nullptr;
    }

    // Log original bytes from disk
    HookLog("DeepHook: Original disk bytes at %p:", target);
    for (int i = 0; i < 32; i += 8) {
        HookLog("  [%02d] %02X %02X %02X %02X %02X %02X %02X %02X", i, origDiskBytes[i], origDiskBytes[i + 1],
                origDiskBytes[i + 2], origDiskBytes[i + 3], origDiskBytes[i + 4], origDiskBytes[i + 5],
                origDiskBytes[i + 6], origDiskBytes[i + 7]);
    }

    // Step 3: Determine resume offset (first instruction boundary >= external JMP size)
    int resumeOffset = 0;
    while (resumeOffset < existingJmpSize) {
        int len = GetInstructionLength(origDiskBytes + resumeOffset, true);
        if (len == 0) {
            HookLog("DeepHook: Failed to decode instruction at disk offset %d", resumeOffset);
            return nullptr;
        }
        resumeOffset += len;
    }

    HookLog("DeepHook: Resume offset = %d (past %d-byte external JMP)", resumeOffset, existingJmpSize);

    // Verify live bytes at resumeOffset match disk (external hook only patches [0, jmpSize))
    bool bytesMatch = (memcmp(code + resumeOffset, origDiskBytes + resumeOffset, 16) == 0);
    if (!bytesMatch) {
        HookLog("DeepHook: WARNING - live bytes at +%d differ from disk!", resumeOffset);
        for (int i = 0; i < 16; i++)
            HookLog("  live[%02d]=0x%02X  disk[%02d]=0x%02X", resumeOffset + i, code[resumeOffset + i],
                    resumeOffset + i, origDiskBytes[resumeOffset + i]);
    }

    // Step 4: Count push instructions in [0, resumeOffset) to determine stack undo
    // These pushes are executed by the external hook's trampoline before reaching
    // our patch point. We undo them so the wrapper sees the original call state.
    int numPushes = 0;
    {
        int pos = 0;
        while (pos < resumeOffset) {
            uint8_t b = origDiskBytes[pos];
            if (b >= 0x50 && b <= 0x57) {
                // push rax-rdi (1 byte, no REX)
                numPushes++;
                pos++;
            } else if (b >= 0x40 && b <= 0x4F && pos + 1 < resumeOffset && origDiskBytes[pos + 1] >= 0x50 &&
                       origDiskBytes[pos + 1] <= 0x57) {
                // REX prefix (0x40-0x4F) + push (0x50-0x57)
                numPushes++;
                pos += 2;
            } else {
                HookLog("DeepHook: Non-push instruction at prolog offset %d (byte=0x%02X) - cannot undo", pos, b);
                return nullptr;
            }
        }
    }
    int stackUndo = numPushes * 8;
    HookLog("DeepHook: Prolog has %d pushes (%d bytes of stack to undo)", numPushes, stackUndo);

    // Step 5: Determine how many bytes to displace at resume offset (need >= patch size)
    const uint8_t* resumeCode = code + resumeOffset;
    int displaceSize = 0;
    // The in-place patch is: add rsp,N (4 or 7 bytes) + jmp [rip+0] addr (14 bytes)
    int undoEncodingSize = (stackUndo <= 127) ? 4 : 7;
    int neededPatchSize = undoEncodingSize + PATCH_SIZE;
    while (displaceSize < neededPatchSize) {
        int len = GetInstructionLength(resumeCode + displaceSize, true);
        if (len == 0) {
            HookLog("DeepHook: Failed to decode at resume+%d (byte=0x%02X)", displaceSize, resumeCode[displaceSize]);
            return nullptr;
        }
        displaceSize += len;
    }

    HookLog("DeepHook: Will displace %d bytes at offset %d (patch needs %d)", displaceSize, resumeOffset,
            neededPatchSize);

    // Step 6: Allocate executable memory nearby for the full trampoline
    // The trampoline contains: original prolog [0, resumeOffset+displaceSize) + JMP to continue
    uint8_t* trampoline = nullptr;
    {
        uintptr_t tgt = (uintptr_t)target;
        for (uintptr_t delta = 0x10000; delta < 0x7FFF0000ULL; delta += 0x10000) {
            if (tgt > delta) {
                uintptr_t tryAddr = (tgt - delta + 0xFFFF) & ~(uintptr_t)0xFFFF;
                trampoline =
                    (uint8_t*)VirtualAlloc((void*)tryAddr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (trampoline)
                    break;
            }
            uintptr_t tryAddr = ((tgt + delta) + 0xFFFF) & ~(uintptr_t)0xFFFF;
            trampoline = (uint8_t*)VirtualAlloc((void*)tryAddr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (trampoline)
                break;
        }
    }
    if (!trampoline) {
        trampoline = (uint8_t*)VirtualAlloc(nullptr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    if (!trampoline) {
        HookLog("DeepHook: Failed to allocate trampoline memory");
        return nullptr;
    }
    memset(trampoline, 0xCC, 256);  // Fill with INT3 for safety

    HookLog("DeepHook: Trampoline at %p (distance=%lld)", trampoline,
            (long long)((intptr_t)trampoline - (intptr_t)target));

    // Step 7: Build the full trampoline
    // Layout: [original prolog bytes 0..resumeOffset] [displaced bytes with RIP fixups] [JMP continue]
    int tOff = 0;

    // Copy original push instructions [0, resumeOffset) from disk
    memcpy(trampoline, origDiskBytes, resumeOffset);
    tOff = resumeOffset;

    // Copy displaced bytes [resumeOffset, resumeOffset+displaceSize) with RIP-relative fixups
    int srcOff = 0;
    bool fixupFailed = false;
    uintptr_t deepPendingAbsCallTarget = 0;
    bool deepHasPendingAbsCall = false;
    int deepPendingCallInstrOff = -1;
    while (srcOff < displaceSize) {
        int instrLen = GetInstructionLength(resumeCode + srcOff, true);

        const auto shortBranchResult = TryRelocateExternalShortControlTransfer(
            resumeCode + srcOff, reinterpret_cast<uintptr_t>(resumeCode + srcOff), instrLen,
            reinterpret_cast<uintptr_t>(resumeCode), displaceSize, trampoline, &tOff, true, "DeepHook");
        if (shortBranchResult == ShortControlRelocationResult::kFailed) {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return nullptr;
        }
        if (shortBranchResult == ShortControlRelocationResult::kHandled) {
            srcOff += instrLen;
            continue;
        }

        memcpy(trampoline + tOff, resumeCode + srcOff, instrLen);

        int dispOff = GetRipRelativeDispOffset(resumeCode + srcOff, instrLen, true);
        if (dispOff >= 0) {
            int32_t origDisp;
            memcpy(&origDisp, resumeCode + srcOff + dispOff, 4);
            uintptr_t absTarget = (uintptr_t)(resumeCode + srcOff + instrLen) + origDisp;
            uintptr_t newInstrEnd = (uintptr_t)(trampoline + tOff + instrLen);
            int64_t newDisp = (int64_t)absTarget - (int64_t)newInstrEnd;

            if (newDisp > INT32_MAX || newDisp < INT32_MIN) {
                uint8_t op = resumeCode[srcOff];
                if (op == 0xE9 || op == 0xE8) {
                    if (op == 0xE9) {
                        // JMP: FF 25 00 00 00 00 [8-byte address]
                        trampoline[tOff] = 0xFF;
                        trampoline[tOff + 1] = 0x25;
                        trampoline[tOff + 2] = 0;
                        trampoline[tOff + 3] = 0;
                        trampoline[tOff + 4] = 0;
                        trampoline[tOff + 5] = 0;
                        memcpy(trampoline + tOff + 6, &absTarget, 8);
                        tOff += 14;
                    } else {
                        // CALL: write FF 15 with placeholder; patch disp after the
                        // JMP-continue block when we know the ptr location.
                        deepPendingCallInstrOff = tOff;
                        trampoline[tOff] = 0xFF;
                        trampoline[tOff + 1] = 0x15;
                        trampoline[tOff + 2] = 0;  // placeholder
                        trampoline[tOff + 3] = 0;
                        trampoline[tOff + 4] = 0;
                        trampoline[tOff + 5] = 0;
                        tOff += 6;
                        deepPendingAbsCallTarget = absTarget;
                        deepHasPendingAbsCall = true;
                    }
                    srcOff += instrLen;
                    continue;
                }
                HookLog("DeepHook: RIP fixup out of range at resume+%d", srcOff);
                fixupFailed = true;
                break;
            }
            int32_t newDisp32 = (int32_t)newDisp;
            memcpy(trampoline + tOff + dispOff, &newDisp32, 4);
            HookLog("DeepHook: Fixed RIP-relative at +%d: origDisp=0x%08X -> newDisp=0x%08X", srcOff,
                    (unsigned)origDisp, (unsigned)newDisp32);
        }
        tOff += instrLen;
        srcOff += instrLen;
    }

    if (fixupFailed) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return nullptr;
    }

    // JMP to (target + resumeOffset + displaceSize) — function body continues here
    uintptr_t continueAddr = (uintptr_t)target + resumeOffset + displaceSize;
    trampoline[tOff++] = 0xFF;
    trampoline[tOff++] = 0x25;
    trampoline[tOff++] = 0x00;
    trampoline[tOff++] = 0x00;
    trampoline[tOff++] = 0x00;
    trampoline[tOff++] = 0x00;
    memcpy(&trampoline[tOff], &continueAddr, 8);
    tOff += 8;

    // If a CALL abs conversion was deferred, write its target pointer here and
    // patch the displacement back into the FF 15 instruction now that we know
    // the exact ptr location.
    if (deepHasPendingAbsCall) {
        int32_t disp = tOff - (deepPendingCallInstrOff + 6);
        memcpy(trampoline + deepPendingCallInstrOff + 2, &disp, 4);
        memcpy(&trampoline[tOff], &deepPendingAbsCallTarget, 8);
        tOff += 8;
    }

    FlushInstructionCache(GetCurrentProcess(), trampoline, tOff);

    HookLog("DeepHook: Trampoline built, %d bytes (prolog=%d, displaced=%d, jmp=%d)", tOff, resumeOffset, displaceSize,
            PATCH_SIZE);

    // Step 8: Build the in-place patch at resumeOffset
    // Format: add rsp, <stackUndo> ; jmp [rip+0] <wrapperFn>
    uint8_t patchBuf[32];
    int pOff = 0;

    if (stackUndo <= 127) {
        patchBuf[pOff++] = 0x48;                // REX.W
        patchBuf[pOff++] = 0x83;                // ADD r/m64, imm8
        patchBuf[pOff++] = 0xC4;                // ModRM: RSP
        patchBuf[pOff++] = (uint8_t)stackUndo;  // imm8
    } else {
        patchBuf[pOff++] = 0x48;  // REX.W
        patchBuf[pOff++] = 0x81;  // ADD r/m64, imm32
        patchBuf[pOff++] = 0xC4;  // ModRM: RSP
        uint32_t undoVal = (uint32_t)stackUndo;
        memcpy(&patchBuf[pOff], &undoVal, 4);
        pOff += 4;
    }

    // jmp [rip+0] <wrapperFn>
    patchBuf[pOff++] = 0xFF;
    patchBuf[pOff++] = 0x25;
    patchBuf[pOff++] = 0x00;
    patchBuf[pOff++] = 0x00;
    patchBuf[pOff++] = 0x00;
    patchBuf[pOff++] = 0x00;
    uintptr_t wrapAddr = (uintptr_t)wrapperFn;
    memcpy(&patchBuf[pOff], &wrapAddr, 8);
    pOff += 8;

    // Fill remaining displaced bytes with NOP
    while (pOff < displaceSize)
        patchBuf[pOff++] = 0x90;

    // Step 9: Patch the live code at resumeOffset
    DeepHookEntry entry = {};
    entry.target = target;
    entry.hookAddr = (void*)resumeCode;
    entry.patchSize = displaceSize;
    memcpy(entry.origBytes, resumeCode, displaceSize);
    entry.trampoline = trampoline;
    entry.installed = false;

    DWORD oldProtect;
    if (!VirtualProtect((void*)resumeCode, displaceSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        HookLog("DeepHook: VirtualProtect failed (error=%lu)", GetLastError());
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return nullptr;
    }

    // Write INT3 first for safety (atomic single-byte write)
    volatile uint8_t* pPatch = (volatile uint8_t*)resumeCode;
    pPatch[0] = 0xCC;
    FlushInstructionCache(GetCurrentProcess(), (void*)resumeCode, 1);

    // Fill remaining with INT3
    for (int i = 1; i < displaceSize; i++)
        pPatch[i] = 0xCC;

    // Write the actual patch (add rsp + jmp wrapper)
    memcpy((void*)resumeCode, patchBuf, displaceSize);

    DWORD dummy;
    VirtualProtect((void*)resumeCode, displaceSize, oldProtect, &dummy);
    FlushInstructionCache(GetCurrentProcess(), (void*)resumeCode, displaceSize);

    entry.installed = true;
    g_deepHooks.push_back(entry);

    HookLog("DeepHook: SUCCESS at %p+%d (trampoline=%p, displaced=%d, continues at %p)", target, resumeOffset,
            trampoline, displaceSize, (void*)continueAddr);
    return trampoline;
#endif
}

bool RemoveDeepHook(void* target) {
    if (!target)
        return false;

    std::lock_guard<std::mutex> lock(g_hookMutex);

    for (auto& d : g_deepHooks) {
        if (d.target == target && d.installed) {
            DWORD oldProtect;
            if (VirtualProtect(d.hookAddr, d.patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                memcpy(d.hookAddr, d.origBytes, d.patchSize);
                VirtualProtect(d.hookAddr, d.patchSize, oldProtect, &oldProtect);
                FlushInstructionCache(GetCurrentProcess(), d.hookAddr, d.patchSize);
            }
            d.installed = false;
            if (d.trampoline) {
                VirtualFree(d.trampoline, 0, MEM_RELEASE);
                d.trampoline = nullptr;
            }
            HookLog("DeepHook: Removed at %p", target);
            return true;
        }
    }

    HookLog("DeepHook: No hook found for %p", target);
    return false;
}

// ============================================================================
// Bypass Trampoline — execute real function body past an external E9/FF25 hook
// ============================================================================

void* CreateBypassTrampoline(void* target) {
    if (!target)
        return nullptr;

    std::lock_guard<std::mutex> lock(g_hookMutex);

    const uint8_t* code = (const uint8_t*)target;
#ifdef _WIN64
    constexpr bool kIs64Bit = true;
#else
    constexpr bool kIs64Bit = false;
#endif

    // Step 1: Detect external hook at byte 0
    int existingJmpSize = 0;
    if (code[0] == 0xE9) {
        existingJmpSize = 5;
    } else if (code[0] == 0xFF && code[1] == 0x25) {
        existingJmpSize = 14;
    } else {
        HookLog("BypassTrampoline: No external hook at byte 0 of %p (byte=0x%02X)", target, code[0]);
        return nullptr;
    }

    HookLog("BypassTrampoline: External %d-byte JMP detected at %p", existingJmpSize, target);

    // Step 2: Read original (unpatched) bytes from DLL on disk
    uint8_t origDiskBytes[64];
    if (!ReadOrigBytesFromDisk(target, origDiskBytes, 64)) {
        HookLog("BypassTrampoline: Failed to read original bytes from disk for %p", target);
        return nullptr;
    }

    HookLog("BypassTrampoline: Original disk bytes at %p:", target);
    for (int i = 0; i < 32; i += 8) {
        HookLog("  [%02d] %02X %02X %02X %02X %02X %02X %02X %02X", i, origDiskBytes[i], origDiskBytes[i + 1],
                origDiskBytes[i + 2], origDiskBytes[i + 3], origDiskBytes[i + 4], origDiskBytes[i + 5],
                origDiskBytes[i + 6], origDiskBytes[i + 7]);
    }

    // Step 3: Find first instruction boundary >= external JMP size
    int resumeOffset = 0;
    while (resumeOffset < existingJmpSize) {
        int len = GetInstructionLength(origDiskBytes + resumeOffset, kIs64Bit);
        if (len == 0) {
            HookLog("BypassTrampoline: Failed to decode instruction at disk offset %d", resumeOffset);
            return nullptr;
        }
        resumeOffset += len;
    }

    HookLog("BypassTrampoline: Resume offset = %d (past %d-byte external JMP)", resumeOffset, existingJmpSize);

    // Verify live bytes at resumeOffset match disk (external hook only patches [0, jmpSize))
    bool bytesMatch = (memcmp(code + resumeOffset, origDiskBytes + resumeOffset, 16) == 0);
    if (!bytesMatch) {
        HookLog("BypassTrampoline: WARNING - live bytes at +%d differ from disk!", resumeOffset);
        for (int i = 0; i < 16; i++)
            HookLog("  live[%02d]=0x%02X  disk[%02d]=0x%02X", resumeOffset + i, code[resumeOffset + i],
                    resumeOffset + i, origDiskBytes[resumeOffset + i]);
    }

    // Step 4: Allocate trampoline slot near the target
    uint8_t* trampoline = GetTrampolineSlot(target);
    if (!trampoline) {
        HookLog("BypassTrampoline: Failed to allocate trampoline slot near %p", target);
        return nullptr;
    }

    // Step 5: Copy original instructions to trampoline with RIP-relative fixups.
    // The source bytes are from disk (original code), but RIP-relative addresses
    // must be computed as if the instructions live at their original location.
    int trampolineOffset = 0;
    int srcOffset = 0;
    uintptr_t pendingAbsCallTarget = 0;
    bool hasPendingAbsCall = false;
    int pendingCallInstrOffset = -1;

    while (srcOffset < resumeOffset) {
        int instrLen = GetInstructionLength(origDiskBytes + srcOffset, kIs64Bit);
        if (instrLen == 0) {
            HookLog("BypassTrampoline: Instruction decode failed at offset %d", srcOffset);
            return nullptr;
        }

        const auto shortBranchResult = TryRelocateExternalShortControlTransfer(
            origDiskBytes + srcOffset, reinterpret_cast<uintptr_t>(reinterpret_cast<uint8_t*>(target) + srcOffset),
            instrLen, reinterpret_cast<uintptr_t>(target), resumeOffset, trampoline, &trampolineOffset, kIs64Bit,
            "BypassTrampoline");
        if (shortBranchResult == ShortControlRelocationResult::kFailed) {
            return nullptr;
        }
        if (shortBranchResult == ShortControlRelocationResult::kHandled) {
            srcOffset += instrLen;
            continue;
        }

        memcpy(trampoline + trampolineOffset, origDiskBytes + srcOffset, instrLen);

        // Fix up RIP-relative addressing.
        // The original instruction was at (target + srcOffset), so compute the
        // absolute target from the original location, then adjust the displacement
        // for the trampoline location.
        int dispOff = GetRipRelativeDispOffset(origDiskBytes + srcOffset, instrLen, kIs64Bit);
        if (dispOff >= 0) {
            int32_t origDisp;
            memcpy(&origDisp, origDiskBytes + srcOffset + dispOff, 4);

            // Absolute target = original instruction end + displacement
            uintptr_t absTarget = (uintptr_t)((uint8_t*)target + srcOffset + instrLen) + origDisp;

            // New displacement from trampoline position
            uintptr_t newInstrEnd = (uintptr_t)(trampoline + trampolineOffset + instrLen);
            int64_t newDisp = (int64_t)absTarget - (int64_t)newInstrEnd;

            HookLog("BypassTrampoline: RIP fixup at srcOff=%d, absTarget=%p, newDisp=0x%llX", srcOffset,
                    (void*)absTarget, (long long)newDisp);

            if (newDisp > INT32_MAX || newDisp < INT32_MIN) {
#ifdef _WIN64
                uint8_t opcode = origDiskBytes[srcOffset];
                if (opcode == 0xE9) {
                    trampoline[trampolineOffset] = 0xFF;
                    trampoline[trampolineOffset + 1] = 0x25;
                    trampoline[trampolineOffset + 2] = 0x00;
                    trampoline[trampolineOffset + 3] = 0x00;
                    trampoline[trampolineOffset + 4] = 0x00;
                    trampoline[trampolineOffset + 5] = 0x00;
                    memcpy(trampoline + trampolineOffset + 6, &absTarget, 8);
                    trampolineOffset += 14;
                    srcOffset += instrLen;
                    continue;
                } else if (opcode == 0xE8) {
                    pendingCallInstrOffset = trampolineOffset;
                    trampoline[trampolineOffset] = 0xFF;
                    trampoline[trampolineOffset + 1] = 0x15;
                    trampoline[trampolineOffset + 2] = 0;
                    trampoline[trampolineOffset + 3] = 0;
                    trampoline[trampolineOffset + 4] = 0;
                    trampoline[trampolineOffset + 5] = 0;
                    trampolineOffset += 6;
                    pendingAbsCallTarget = absTarget;
                    hasPendingAbsCall = true;
                    srcOffset += instrLen;
                    continue;
                }
#endif

                HookLog("BypassTrampoline: RIP fixup out of range at offset %d (opcode=0x%02X)", srcOffset,
                        origDiskBytes[srcOffset]);
                return nullptr;
            }

            int32_t newDisp32 = (int32_t)newDisp;
            memcpy(trampoline + trampolineOffset + dispOff, &newDisp32, 4);
        }

        trampolineOffset += instrLen;
        srcOffset += instrLen;
    }

    // Step 6: Write JMP back to the real code PAST the external hook
    void* jumpTarget = (void*)((uint8_t*)target + resumeOffset);
    HookLog("BypassTrampoline: Writing JMP back from trampoline+%d to %p (target+%d)", trampolineOffset, jumpTarget,
            resumeOffset);
    WriteJump(trampoline + trampolineOffset, jumpTarget);
    trampolineOffset += PATCH_SIZE;

    // Patch pending absolute CALL if needed
    if (hasPendingAbsCall) {
        int32_t disp = trampolineOffset - (pendingCallInstrOffset + 6);
        memcpy(trampoline + pendingCallInstrOffset + 2, &disp, 4);
        memcpy(trampoline + trampolineOffset, &pendingAbsCallTarget, 8);
        trampolineOffset += 8;
        HookLog("BypassTrampoline: Patched absolute CALL target at trampoline+%d", trampolineOffset - 8);
    }

    FlushInstructionCache(GetCurrentProcess(), trampoline, trampolineOffset);

    HookLog("BypassTrampoline: Created at %p (%d bytes) — bypasses %d-byte external hook at %p", trampoline,
            trampolineOffset, existingJmpSize, target);

    return trampoline;
}

}  // namespace InlineHook
