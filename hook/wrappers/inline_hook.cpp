/**
 * Minimal Inline Hook Implementation
 *
 * Contains:
 * - Length Disassembly Engine (LDE) for x86/x64
 * - Trampoline allocation near target (within ±2GB for RIP-relative fixups)
 * - Hook installation and removal
 */

#include "inline_hook.h"

#include <cstring>
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

static uint8_t *GetTrampolineSlot(void *nearAddr) {
  // Check if existing pool is close enough (within ±2GB)
  if (g_trampolinePool) {
    uintptr_t poolAddr = (uintptr_t)g_trampolinePool;
    uintptr_t targetAddr = (uintptr_t)nearAddr;
    int64_t distance = (int64_t)targetAddr - (int64_t)poolAddr;

    // If pool is too far away, free it and allocate a new one
    if (distance > INT32_MAX || distance < INT32_MIN) {
      HookLog("GetTrampolineSlot: Existing pool too far from target (distance=%lld), allocating new pool", (long long)distance);
      VirtualFree(g_trampolinePool, 0, MEM_RELEASE);
      g_trampolinePool = nullptr;
      g_trampolineOffset = 0;
    }
  }

  if (!g_trampolinePool) {
    g_trampolinePool = AllocateTrampolinePool(nearAddr);
    if (!g_trampolinePool)
      return nullptr;
    g_trampolineOffset = 0;
  }
  if (g_trampolineOffset + TRAMPOLINE_ENTRY_SIZE > TRAMPOLINE_POOL_SIZE)
    return nullptr;

  uint8_t *slot = g_trampolinePool + g_trampolineOffset;
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

bool Install(void *target, void *detour, void **outTrampoline) {
  if (!target || !detour || !outTrampoline)
    return false;

  std::lock_guard<std::mutex> lock(g_hookMutex);

  // Check if already hooked
  for (auto &h : g_hooks) {
    if (h.target == target && h.installed) {
      HookLog("InlineHook: Target %p already hooked", target);
      return false;
    }
  }

#ifdef _WIN64
  bool is64bit = true;
#else
  bool is64bit = false;
#endif

  // Determine how many bytes to copy (must be >= PATCH_SIZE on instruction
  // boundary)
  const uint8_t *code = (const uint8_t *)target;
  int copySize = 0;
  while (copySize < PATCH_SIZE) {
    int len = GetInstructionLength(code + copySize, is64bit);
    if (len == 0) {
      HookLog("InlineHook: Failed to decode instruction at %p+%d "
              "(byte=0x%02X)",
              target, copySize, code[copySize]);
      return false;
    }
    copySize += len;
  }

  HookLog("InlineHook: Hooking %p, patch=%d bytes, detour=%p, is64bit=%d",
          target, copySize, detour, is64bit ? 1 : 0);

  // Dump original bytes for diagnosis
  HookLog("InlineHook: Original bytes at %p:", target);
  for (int i = 0; i < copySize && i < 16; i++) {
    HookLog("  [%02d] 0x%02X", i, code[i]);
  }

  // Allocate trampoline
  uint8_t *trampoline = GetTrampolineSlot(target);
  if (!trampoline) {
    HookLog("InlineHook: Failed to allocate trampoline");
    return false;
  }
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
        HookLog("InlineHook: RIP-relative fixup out of range at %p+%d", target,
                srcOffset);
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

  // Patch the target function
  DWORD oldProtect;
  if (!VirtualProtect(target, copySize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
    HookLog("InlineHook: VirtualProtect failed (error=%lu)", GetLastError());
    return false;
  }

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

  if (g_trampolinePool) {
    VirtualFree(g_trampolinePool, 0, MEM_RELEASE);
    g_trampolinePool = nullptr;
    g_trampolineOffset = 0;
  }
}

} // namespace InlineHook
