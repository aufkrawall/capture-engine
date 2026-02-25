/**
 * Minimal Inline Hook Implementation
 *
 * Contains:
 * - Length Disassembly Engine (LDE) for x86/x64
 * - Trampoline allocation near target (within ±2GB for RIP-relative fixups)
 * - Hook installation and removal
 */

#include "inline_hook.h"

#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <vector>
#include <windows.h>

// Forward declaration for HookLog (defined in hook_common.cpp)
void HookLog(const char *fmt, ...);

namespace InlineHook {

// ============================================================================
// Length Disassembly Engine (LDE)
// ============================================================================

// Opcode property flags
enum : uint8_t {
  OP_NONE = 0x00,
  OP_MODRM = 0x01,   // Has ModR/M byte
  OP_IMM8 = 0x02,    // Has 8-bit immediate
  OP_IMM32 = 0x04,   // Has 32-bit immediate (16-bit with 66h prefix)
  OP_2BYTE = 0x08,   // Is 0F-prefixed (set during parsing, not in table)
  OP_SPECIAL = 0x80, // Needs special handling
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
static int ParseModRM(const uint8_t *code, bool hasAddrPrefix) {
  uint8_t modrm = code[0];
  uint8_t mod = (modrm >> 6) & 3;
  uint8_t rm = modrm & 7;
  int extra = 1; // The ModR/M byte itself

  if (mod == 3) {
    // Register direct - no extra bytes
    return extra;
  }

  bool use16bit = false;
#ifndef _WIN64
  use16bit = !hasAddrPrefix; // Default 32-bit, 67h switches to 16-bit
  // Actually on 32-bit, default is 32-bit addressing, 67h switches to 16
  use16bit = hasAddrPrefix;
#else
  use16bit = hasAddrPrefix; // 64-bit: 67h switches to 32-bit addressing
  // (we don't support 16-bit addressing in 64-bit mode)
  // In 64-bit mode with 67h, addressing uses 32-bit regs but same ModR/M
  // encoding
#endif

  if (!use16bit) {
    // 32/64-bit addressing
    if (rm == 4) {
      extra++; // SIB byte
      uint8_t sib = code[1];
      uint8_t base = sib & 7;
      if (mod == 0 && base == 5) {
        extra += 4; // disp32
      }
    }

    if (mod == 0) {
      if (rm == 5) {
        extra += 4; // disp32 (or RIP-relative in x64)
      }
    } else if (mod == 1) {
      extra += 1; // disp8
    } else if (mod == 2) {
      extra += 4; // disp32
    }
  } else {
    // 16-bit addressing (rare, mainly for x86 with 67h prefix)
    if (mod == 0 && rm == 6) {
      extra += 2; // disp16
    } else if (mod == 1) {
      extra += 1; // disp8
    } else if (mod == 2) {
      extra += 2; // disp16
    }
  }

  return extra;
}

// Get the length of an instruction at the given address.
// Returns 0 if the instruction cannot be decoded (unsupported/unknown).
static int GetInstructionLength(const uint8_t *code, bool is64bit) {
  const uint8_t *p = code;
  bool hasOpSize = false;   // 66h prefix
  bool hasAddrSize = false; // 67h prefix
  bool hasREX_W = false;    // REX.W prefix (x64 only)

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
      p++; // LOCK, REPNE, REP
    } else if (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 ||
               b == 0x65) {
      p++; // Segment overrides
    } else if (is64bit && (b >= 0x40 && b <= 0x4F)) {
      hasREX_W = (b & 0x08) != 0;
      p++; // REX prefix
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
      p++; // third opcode byte
      int modrm = ParseModRM(p, hasAddrSize);
      return (int)(p + modrm - code);
    }
    if (opcode == 0x3A) {
      p++; // third opcode byte
      int modrm = ParseModRM(p, hasAddrSize);
      return (int)(p + modrm + 1 - code); // +1 for imm8
    }

    flags = g_twoByteOpcodes[opcode];
  } else {
    flags = g_oneByteOpcodes[opcode];
  }

  // 3. Handle special opcodes
  if (flags & OP_SPECIAL) {
    if (!isTwoByte) {
      switch (opcode) {
      case 0x9A: // CALL FAR (x86 only)
        return (int)(p - code) + (hasOpSize ? 4 : 6);
      case 0xA0: // MOV AL, moffs
      case 0xA2: // MOV moffs, AL
        return (int)(p - code) +
               (is64bit && !hasAddrSize ? 8 : (hasAddrSize ? 2 : 4));
      case 0xA1: // MOV rAX, moffs
      case 0xA3: // MOV moffs, rAX
        return (int)(p - code) +
               (is64bit && !hasAddrSize ? 8 : (hasAddrSize ? 2 : 4));
      case 0xC2: // RET imm16
      case 0xCA: // RETF imm16
        return (int)(p - code) + 2;
      case 0xC4: // VEX 3-byte prefix (x64) or LES (x86)
        if (is64bit) {
          p += 2; // VEX payload bytes
          p++;    // opcode after VEX
          // VEX instructions always have ModR/M
          int modrm = ParseModRM(p, hasAddrSize);
          // Some VEX opcodes have imm8 - check the VEX map
          // For safety, check if the original 0F 3A map was selected (pp bits)
          uint8_t vexByte1 = code[p - code - 3];
          uint8_t mmmmm = vexByte1 & 0x1F;
          if (mmmmm == 3) { // 0F 3A map: has imm8
            return (int)(p + modrm + 1 - code);
          }
          return (int)(p + modrm - code);
        }
        return 0; // LES - unsupported, shouldn't appear in prologues
      case 0xC5:  // VEX 2-byte prefix (x64) or LDS (x86)
        if (is64bit) {
          p += 1; // VEX payload byte
          p++;    // opcode after VEX
          int modrm = ParseModRM(p, hasAddrSize);
          return (int)(p + modrm - code);
        }
        return 0; // LDS - unsupported
      case 0xC8:  // ENTER imm16, imm8
        return (int)(p - code) + 3;
      case 0xEA: // JMP FAR (x86 only)
        return (int)(p - code) + (hasOpSize ? 4 : 6);
      case 0xF6: // Group 3 byte - TEST r/m8, imm8 (reg=0) or NOT/NEG/MUL/etc
      {
        int modrm = ParseModRM(p, hasAddrSize);
        uint8_t reg = (p[0] >> 3) & 7;
        int immSize = (reg == 0 || reg == 1) ? 1 : 0; // TEST has imm8
        return (int)(p + modrm + immSize - code);
      }
      case 0xF7: // Group 3 dword - TEST r/m32, imm32 (reg=0) or NOT/NEG/MUL/etc
      {
        int modrm = ParseModRM(p, hasAddrSize);
        uint8_t reg = (p[0] >> 3) & 7;
        int immSize = (reg == 0 || reg == 1) ? (hasOpSize ? 2 : 4) : 0;
        return (int)(p + modrm + immSize - code);
      }
      default:
        return 0; // Unknown special
      }
    }
    return 0; // Unknown two-byte special
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
      p += 2; // 66h prefix: 16-bit immediate instead of 32-bit
    } else {
      p += 4;
    }
    // Special: MOV r64, imm64 (B8-BF with REX.W)
    if (!isTwoByte && opcode >= 0xB8 && opcode <= 0xBF && hasREX_W) {
      p += 4; // Additional 4 bytes for 64-bit immediate (total 8)
    }
  }

  int len = (int)(p - code);
  return (len > 0 && len <= 15) ? len : 0; // x86 max instruction = 15 bytes
}

// ============================================================================
// RIP-Relative Instruction Fixup
// ============================================================================

// Check if an instruction at 'code' uses PC-relative addressing.
// For both 32-bit and 64-bit: CALL/JMP/Jcc rel32 need fixup.
// For 64-bit only: RIP-relative ModR/M addressing also needs fixup.
// Returns the offset of the 32-bit displacement within the instruction,
// or -1 if not PC-relative.
static int GetRipRelativeDispOffset(const uint8_t *code, int instrLen,
                                    bool is64bit) {
  const uint8_t *p = code;

  // Skip prefixes
  while (p < code + instrLen) {
    uint8_t b = *p;
    if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
        b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 ||
        b == 0x65 || (is64bit && b >= 0x40 && b <= 0x4F)) {
      p++;
    } else {
      break;
    }
  }

  const uint8_t *opcodeStart = p;
  uint8_t opcode = *p++;
  bool isTwoByte = false;

  if (opcode == 0x0F) {
    opcode = *p++;
    isTwoByte = true;

    // 3-byte escape: 0F 38 or 0F 3A
    if (opcode == 0x38 || opcode == 0x3A) {
      p++; // skip third opcode byte
    }
  }

  // Check for CALL/JMP rel32 (PC-relative on BOTH 32-bit and 64-bit)
  if (!isTwoByte && (opcode == 0xE8 || opcode == 0xE9)) {
    // The 32-bit displacement starts right after the opcode
    int result = (int)(p - code);
    HookLog("GetRipRelativeDispOffset: Found %s rel32 at offset %d",
            opcode == 0xE8 ? "CALL" : "JMP", result);
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
    return (int)(p + 1 - code); // disp32 starts after ModR/M
  }

  return -1;
}

// ============================================================================
// Hook State
// ============================================================================

struct HookEntry {
  void *target;
  void *trampoline;
  uint8_t origBytes[32];
  int patchSize;
  bool installed;
};

static std::vector<HookEntry> g_hooks;
static std::mutex g_hookMutex;
static uint8_t *g_trampolinePool = nullptr;
static std::vector<uint8_t *> g_trampolinePools;
static size_t g_trampolineOffset = 0;
static constexpr size_t TRAMPOLINE_POOL_SIZE = 4096;
static constexpr size_t TRAMPOLINE_ENTRY_SIZE = 64; // Max per hook

#ifdef _WIN64
static constexpr int PATCH_SIZE = 14; // FF 25 00 00 00 00 + 8-byte address
#else
static constexpr int PATCH_SIZE = 5; // E9 + 4-byte relative offset
#endif

// Allocate executable memory near the target (within ±2GB for x64)
static uint8_t *AllocateTrampolinePool(void *nearAddr) {
#ifdef _WIN64
  // Try to allocate within ±2GB of target for RIP-relative fixups
  uintptr_t target = (uintptr_t)nearAddr;
  uintptr_t low = target > 0x7FFF0000ULL ? target - 0x7FFF0000ULL : 0x10000ULL;
  uintptr_t high = target + 0x7FFF0000ULL;

  MEMORY_BASIC_INFORMATION mbi;
  for (uintptr_t addr = low; addr < high;) {
    if (VirtualQuery((void *)addr, &mbi, sizeof(mbi)) == 0)
      break;

    if (mbi.State == MEM_FREE && mbi.RegionSize >= TRAMPOLINE_POOL_SIZE) {
      // Align to allocation granularity (64KB)
      uintptr_t aligned = (addr + 0xFFFF) & ~(uintptr_t)0xFFFF;
      if (aligned + TRAMPOLINE_POOL_SIZE <= addr + mbi.RegionSize) {
        void *p =
            VirtualAlloc((void *)aligned, TRAMPOLINE_POOL_SIZE,
                         MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p)
          return (uint8_t *)p;
      }
    }
    addr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
  }
#endif
  // Fallback: allocate anywhere
  return (uint8_t *)VirtualAlloc(nullptr, TRAMPOLINE_POOL_SIZE,
                                 MEM_COMMIT | MEM_RESERVE,
                                 PAGE_EXECUTE_READWRITE);
}

static bool IsTrampolinePoolNearTarget(const uint8_t *pool, void *nearAddr) {
#ifdef _WIN64
  if (!pool || !nearAddr)
    return false;
  uintptr_t poolAddr = reinterpret_cast<uintptr_t>(pool);
  uintptr_t targetAddr = reinterpret_cast<uintptr_t>(nearAddr);
  int64_t distance = static_cast<int64_t>(targetAddr) -
                     static_cast<int64_t>(poolAddr);
  return distance <= INT32_MAX && distance >= INT32_MIN;
#else
  (void)pool;
  (void)nearAddr;
  return true;
#endif
}

static uint8_t *GetTrampolineSlot(void *nearAddr) {
  bool needNewPool = false;
  if (!g_trampolinePool) {
    needNewPool = true;
  } else if (!IsTrampolinePoolNearTarget(g_trampolinePool, nearAddr)) {
#ifdef _WIN64
    uintptr_t poolAddr = reinterpret_cast<uintptr_t>(g_trampolinePool);
    uintptr_t targetAddr = reinterpret_cast<uintptr_t>(nearAddr);
    int64_t distance = static_cast<int64_t>(targetAddr) -
                       static_cast<int64_t>(poolAddr);
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
    uint8_t *newPool = AllocateTrampolinePool(nearAddr);
    if (!newPool)
      return nullptr;
    g_trampolinePool = newPool;
    g_trampolineOffset = 0;
    g_trampolinePools.push_back(g_trampolinePool);
    HookLog("GetTrampolineSlot: New pool allocated at %p (total pools=%zu)",
            g_trampolinePool, g_trampolinePools.size());
  }

  uint8_t *slot = g_trampolinePool + g_trampolineOffset;
  HookLog("GetTrampolineSlot: Allocating slot at offset %zu (addr=%p) for target %p", 
          g_trampolineOffset, slot, nearAddr);
  g_trampolineOffset += TRAMPOLINE_ENTRY_SIZE;
  return slot;
}

// Write an absolute jump at 'dest' to 'target'
static void WriteJump(uint8_t *dest, void *target) {
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
  HookLog("WriteJump: x86 JMP rel32 -> %p at %p (rel=0x%08X, dest+5=%p)",
          target, dest, (unsigned)rel, (void *)(dest + 5));
  // Verify: dest+5 + rel should equal target
  uintptr_t verify = (uintptr_t)(dest + 5) + rel;
  HookLog("WriteJump: Verification: dest+5(0x%p) + rel(0x%08X) = 0x%p "
          "(expected %p)",
          (void *)(dest + 5), (unsigned)rel, (void *)verify, target);
#endif
}

// ============================================================================
// Public API
// ============================================================================

// Check if the target function appears to already be hooked.
// Returns true if we detect a common hook pattern at the start of the function.
static bool IsAlreadyHooked(const uint8_t *code, bool is64bit) {
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
    HookLog("InlineHook: IsAlreadyHooked: Detected JMP rel32 at %p (jumps to %p, disp=0x%08X)",
            (void *)code, (void *)jumpTarget, (unsigned)disp);
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
    if (VirtualQuery((void *)addrPtr, &mbi, sizeof(mbi)) > 0 &&
        mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {
      jumpTarget = *(uintptr_t *)addrPtr;
    }
    HookLog("InlineHook: IsAlreadyHooked: Detected JMP [rip+disp32] at %p (indirect addr=%p, target=%p)",
            (void *)code, (void *)addrPtr, (void *)jumpTarget);
    return true;
  }

  // CALL rel32 (E8) - less common but sometimes used
  if (firstByte == 0xE8) {
    int32_t disp;
    memcpy(&disp, code + 1, 4);
    uintptr_t callTarget = (uintptr_t)(code + 5) + disp;
    HookLog("InlineHook: IsAlreadyHooked: Detected CALL rel32 at %p (calls %p, disp=0x%08X)",
            (void *)code, (void *)callTarget, (unsigned)disp);
    return true;
  }

  return false;
}

// Try to restore a function that has been hooked by patching a known prologue.
// This is used when we detect a stale hook from a previous process.
// Returns true if restoration was successful.
static bool TryRestoreHookedFunction(void *target, bool is64bit) {
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

  uint8_t *code = (uint8_t *)target;

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
      HookLog("InlineHook: TryRestoreHookedFunction: Bytes 5+ look like mov [rsp+%02Xh], r%x",
              code[9], (regByte >> 3) & 7);
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
    HookLog("InlineHook: TryRestoreHookedFunction: Cannot infer prologue from offset pattern (byte9=0x%02X)", code[9]);
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
    HookLog("  [%02d] 0x%02X", i, ((uint8_t *)target)[i]);
  }

  return true;
}

bool Install(void *target, void *detour, void **outTrampoline) {
  // Use existing optional hook logger; avoid absolute-path file writes from injected code.
  auto LogDirect = [](const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
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
  for (auto &h : g_hooks) {
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
  const uint8_t *code = (const uint8_t *)target;

  // Dump first bytes of target
  // SECURITY FIX: Use safe string concatenation
  char firstBytes[64] = {0};
  size_t remaining = sizeof(firstBytes) - 1;
  char *dest = firstBytes;
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
    // Parse the JMP instruction to find where the overlay's hook goes
    LogDirect("External hook detected, attempting chain hooking...");
    
    if (code[0] == 0xE9 && !is64bit) {
      // 32-bit JMP rel32
      int32_t rel32 = *(const int32_t*)(code + 1);
      uintptr_t overlayTarget = (uintptr_t)target + 5 + rel32;
      LogDirect("JMP rel32 detected: rel32=0x%08X, overlay target=%p", 
               (unsigned)rel32, (void*)overlayTarget);
      HookLog("InlineHook: Chaining to overlay hook at %p", (void*)overlayTarget);
      
      // Check if the overlay target is valid memory
      MEMORY_BASIC_INFORMATION mbi;
      if (VirtualQuery((void*)overlayTarget, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        LogDirect("Overlay target memory: Base=%p, Protect=0x%X", mbi.BaseAddress, mbi.Protect);
        
        // Check if it's executable memory
        if (mbi.State == MEM_COMMIT && 
            (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
          LogDirect("Overlay target is executable, attempting hook...");
          
          // Try to install our hook at the overlay's target
          // First, check if there's another JMP there (multi-level chaining)
          const uint8_t* overlayCode = (const uint8_t*)overlayTarget;
          if (overlayCode[0] == 0xE9) {
            LogDirect("Overlay target also has JMP, following chain...");
            int32_t rel32_2 = *(const int32_t*)(overlayCode + 1);
            uintptr_t finalTarget = overlayTarget + 5 + rel32_2;
            LogDirect("Second JMP target: %p", (void*)finalTarget);
            overlayTarget = finalTarget;
          }
          
          // Try to install hook at the overlay target
          if (!IsAlreadyHooked((const uint8_t*)overlayTarget, is64bit)) {
            LogDirect("Attempting recursive Install at overlay target %p", (void*)overlayTarget);
            // Recursive call - but we need to save the original first
            // Actually, we can't do a recursive call because we'll detect it as already hooked
            // Instead, we need to directly install the hook here
            
            // For now, let's try installing at overlay target
            // We'll skip the "already hooked by us" check since this is a new target
            uintptr_t chainTarget = overlayTarget;
            void* chainDetour = detour;
            void** chainOriginal = outTrampoline;
            
            // Decode instructions at overlay target
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
               
               // Allocate trampoline for chain hook
               uint8_t* chainTrampoline = GetTrampolineSlot((void*)chainTarget);
               if (!chainTrampoline) {
                 LogDirect("FAILED: Could not allocate trampoline for chain hook");
                 return false;
               }
               LogDirect("Chain hook trampoline allocated at %p", chainTrampoline);
               
               // Copy original instructions to trampoline (no RIP-relative fixups needed for chain hooks
               // since we're copying from overlay code, not original d3d9)
               memcpy(chainTrampoline, chainCode, chainCopySize);
               
               // Add JMP back to overlay code after copied bytes
               // JMP rel32 from trampoline+copySize to chainTarget+copySize
               uint8_t* jmpSite = chainTrampoline + chainCopySize;
               jmpSite[0] = 0xE9; // JMP rel32
               int32_t jmpOffset = (int32_t)((uintptr_t)(chainCode + chainCopySize) - (uintptr_t)(jmpSite + 5));
               memcpy(jmpSite + 1, &jmpOffset, 4);
               LogDirect("Chain trampoline: copied %d bytes, JMP at offset %d -> %p (rel=0x%08X)",
                        chainCopySize, chainCopySize, (void*)(chainCode + chainCopySize), (unsigned)jmpOffset);
               
               // Create the hook entry
               HookEntry newHook = {};
               newHook.target = (void*)chainTarget;
               newHook.trampoline = chainTrampoline;
               newHook.patchSize = chainCopySize;
               newHook.installed = false;
               memcpy(newHook.origBytes, chainCode, chainCopySize);
               
               // Save trampoline to caller's storage - this is what detour calls to invoke overlay code
               if (chainOriginal) {
                 *chainOriginal = chainTrampoline;
               }
               
               // Change memory protection
               DWORD oldProtect;
               if (VirtualProtect((void*)chainTarget, chainCopySize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                 // Write JMP to detour
                 uint8_t patch[5];
                 patch[0] = 0xE9; // JMP rel32
                 int32_t offset = (int32_t)((uintptr_t)chainDetour - chainTarget - 5);
                 memcpy(patch + 1, &offset, 4);
                 memcpy((void*)chainTarget, patch, 5);
                 
                 // Fill remaining with NOPs if copySize > 5
                 for (int i = 5; i < chainCopySize; i++) {
                   ((uint8_t*)chainTarget)[i] = 0x90;
                 }
                 
                 // Flush instruction cache
                 FlushInstructionCache(GetCurrentProcess(), (void*)chainTarget, chainCopySize);
                 
                 // Restore protection
                 DWORD dummy;
                 VirtualProtect((void*)chainTarget, chainCopySize, oldProtect, &dummy);
                 
                 newHook.installed = true;
                 g_hooks.push_back(newHook);
                 
                 LogDirect("SUCCESS: Chain hook installed at %p -> %p (trampoline=%p)", 
                          (void*)chainTarget, chainDetour, chainTrampoline);
                 HookLog("InlineHook: Chain hook installed at %p (trampoline=%p)", 
                        (void*)chainTarget, chainTrampoline);
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
    } else if (code[0] == 0xFF && code[1] == 0x25) {
      // JMP [addr] - indirect jump
      LogDirect("JMP [addr] detected, not yet supported for chain hooking");
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
      HookLog("InlineHook: Failed to decode instruction at %p+%d "
              "(byte=0x%02X)",
              target, copySize, code[copySize]);
      return false;
    }
    copySize += len;
  }

  LogDirect("Instructions decoded, copySize=%d bytes", copySize);
  HookLog("InlineHook: Hooking %p, patch=%d bytes, detour=%p, is64bit=%d",
          target, copySize, detour, is64bit ? 1 : 0);

  // Dump original bytes for diagnosis
  // SECURITY FIX: Use safe string concatenation
  char bytesStr[256] = {0};
  size_t bytesRemaining = sizeof(bytesStr) - 1;
  char *bytesDest = bytesStr;
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
  uint8_t *trampoline = GetTrampolineSlot(target);
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
  while (srcOffset < copySize) {
    int instrLen = GetInstructionLength(code + srcOffset, is64bit);

    // Log instruction being copied
    HookLog("InlineHook: Copying instruction at offset %d, len=%d:", srcOffset,
            instrLen);
    for (int i = 0; i < instrLen && i < 8; i++) {
      HookLog("  [%02d] 0x%02X", i, code[srcOffset + i]);
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
      uintptr_t newInstrEnd =
          (uintptr_t)(trampoline + trampolineOffset + instrLen);
      int64_t newDisp = (int64_t)absTarget - (int64_t)newInstrEnd;

      HookLog("InlineHook: PC-relative fixup at srcOff=%d, dispOff=%d, "
              "origDisp=0x%08X, absTarget=%p, newInstrEnd=%p, newDisp=0x%08llX",
              srcOffset, dispOff, (unsigned)origDisp, (void *)absTarget,
              (void *)newInstrEnd, (long long)newDisp);

      if (newDisp > INT32_MAX || newDisp < INT32_MIN) {
        // Check if this is a JMP rel32 (0xE9) or CALL rel32 (0xE8) that we can convert to absolute
        uint8_t opcode = code[srcOffset];
        if (opcode == 0xE9 || opcode == 0xE8) {
          // Convert to absolute jump/call using 14-byte sequence:
          // FF 25 00 00 00 00 [8-byte absolute address]
          // For JMP: just jump to absTarget
          // For CALL: same but it's a call
          HookLog("InlineHook: Converting %s rel32 to absolute at offset %d (target=%p)",
                  opcode == 0xE9 ? "JMP" : "CALL", srcOffset, (void *)absTarget);
          
          // Write absolute jump/call
          trampoline[trampolineOffset] = 0xFF;      // JMP/CALL [rip+0]
          trampoline[trampolineOffset + 1] = opcode == 0xE9 ? 0x25 : 0x15;  // 0x25=JMP, 0x15=CALL
          trampoline[trampolineOffset + 2] = 0x00;
          trampoline[trampolineOffset + 3] = 0x00;
          trampoline[trampolineOffset + 4] = 0x00;
          trampoline[trampolineOffset + 5] = 0x00;
          memcpy(trampoline + trampolineOffset + 6, &absTarget, 8);
          trampolineOffset += 14;  // 14 bytes for absolute jump
          srcOffset += instrLen;
          continue;  // Skip the normal fixup path
        }
        
        HookLog("InlineHook: RIP-relative fixup out of range at %p+%d (opcode=0x%02X)", target,
                srcOffset, opcode);
        return false;
      }

      int32_t newDisp32 = (int32_t)newDisp;
      memcpy(trampoline + trampolineOffset + dispOff, &newDisp32, 4);
    } else {
      HookLog("InlineHook: No PC-relative fixup needed for instruction at "
              "offset %d",
              srcOffset);
    }

    trampolineOffset += instrLen;
    srcOffset += instrLen;
  }

  // Add jump back to original function after the patched area
  void *jumpTarget = (void *)(code + copySize);
  HookLog(
      "InlineHook: Writing jump back from trampoline+%d to %p (original+%d)",
      trampolineOffset, jumpTarget, copySize);
  WriteJump(trampoline + trampolineOffset, jumpTarget);
  trampolineOffset += PATCH_SIZE;

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

  // Write INT3 first for safety (atomic single-byte write)
  volatile uint8_t *pTarget = (volatile uint8_t *)target;
  pTarget[0] = 0xCC;
  FlushInstructionCache(GetCurrentProcess(), target, 1);

  // Fill remaining patch area with INT3
  for (int i = 1; i < copySize; i++) {
    pTarget[i] = 0xCC;
  }

  // Write the jump to the detour function
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
  pTarget[0] = 0xE9; // JMP rel32 opcode
  int32_t rel =
      (int32_t)((uintptr_t)detour - (uintptr_t)((uint8_t *)target + 5));
  memcpy((void *)(pTarget + 1), &rel, 4);
  HookLog("InlineHook: Target JMP at %p -> %p (rel=0x%08X)", target, detour,
          (unsigned)rel);
  // Verify the calculation
  uintptr_t verify = (uintptr_t)((uint8_t *)target + 5) + rel;
  HookLog("InlineHook: Verification: %p + 5 + 0x%08X = %p (expected %p)",
          target, (unsigned)rel, (void *)verify, detour);
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
  HookLog("InlineHook: Installed hook at %p -> %p (trampoline=%p)", target,
          detour, trampoline);
  return true;
}

bool Remove(void *target) {
  if (!target)
    return false;

  std::lock_guard<std::mutex> lock(g_hookMutex);

  for (auto &h : g_hooks) {
    if (h.target == target && h.installed) {
      DWORD oldProtect;
      if (VirtualProtect(h.target, h.patchSize, PAGE_EXECUTE_READWRITE,
                         &oldProtect)) {
        memcpy(h.target, h.origBytes, h.patchSize);
        VirtualProtect(h.target, h.patchSize, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), h.target, h.patchSize);
        h.installed = false;
        HookLog("InlineHook: Removed hook at %p", target);
        return true;
      }
      HookLog("InlineHook: Failed to remove hook at %p (VirtualProtect "
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

  for (auto &h : g_hooks) {
    if (h.installed) {
      DWORD oldProtect;
      if (VirtualProtect(h.target, h.patchSize, PAGE_EXECUTE_READWRITE,
                         &oldProtect)) {
        memcpy(h.target, h.origBytes, h.patchSize);
        VirtualProtect(h.target, h.patchSize, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), h.target, h.patchSize);
      }
      h.installed = false;
    }
  }
  g_hooks.clear();

  if (!g_trampolinePools.empty()) {
    for (uint8_t *pool : g_trampolinePools) {
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

} // namespace InlineHook
