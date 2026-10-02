#pragma once

namespace ce::inline_hook_policy {

inline constexpr int kExternalPrependPatchSize = 5;

inline bool IsPrependChainableEntryJump(unsigned char firstByte, unsigned char secondByte, bool is64Bit) {
    return firstByte == 0xE9 || (is64Bit && firstByte == 0xFF && secondByte == 0x25);
}

inline bool ShouldRestoreOwnedPatch(bool liveBytesMatchInstalledBytes) {
    return liveBytesMatchInstalledBytes;
}

inline bool IsVerifiedExternalHookResumeOffset(int resumeOffset, int externalJumpSize, bool liveBytesMatchDisk) {
    return resumeOffset >= externalJumpSize && liveBytesMatchDisk;
}

inline bool ShouldExtendExternalHookResumeOffset(int firstCandidateOffset, int selectedOffset,
                                                 bool selectedLiveBytesMatchDisk) {
    return selectedLiveBytesMatchDisk && firstCandidateOffset > 0 && selectedOffset > firstCandidateOffset;
}

// Deep-hook prolog analysis.
//
// A deep hook patches the body at `resumeOffset`, past a foreign entry jump, so every
// original instruction in [0, resumeOffset) has already executed when control reaches CE's
// wrapper. The wrapper must be entered with the stack exactly as the caller left it, so the
// patch has to undo whatever those instructions did to RSP. Only shapes with a statically
// known RSP effect are accepted:
//
//   push r64              50+r / REX 50+r            RSP -= 8
//   sub rsp, imm8         48 83 EC ib                RSP -= ib
//   sub rsp, imm32        48 81 EC id                RSP -= id
//   mov [rsp(+disp)], r64 REX.W 89 /r, SIB base=RSP  RSP unchanged (shadow-space save)
//   lea rbp, [rsp+disp]   48 8D 6C 24 ib / 48 8D AC 24 id   RSP unchanged, RBP clobbered
//
// The shadow-space save is what dxgi!CDXGISwapChain::Present opens with, so refusing it
// (as a pushes-only rule does) rules out deep-hooking Present at all. Anything else is
// refused: an unrecognized prolog cannot be undone safely.
//
// The frame-pointer setup clobbers RBP, a callee-saved register, so it is accepted only after
// the prolog pushed the caller's RBP: the undo then reloads RBP from that push slot before it
// releases the stack. dxgi!CreateSwapChainForHwnd sets RBP at +11, so a body hook placed past
// a 14-byte entry span (resume offset 16) needs exactly this shape.
struct DeepHookPrologUndo {
    int stackDelta = 0;
    bool restoreRbp = false;
    int rbpSlotOffsetFromPrologRsp = 0;  // where the pushed RBP lives, relative to RSP after the prolog
};

// Returns false when a byte sequence is unrecognized or an instruction would run past
// `prologLength`.
inline bool TryAnalyzeDeepHookProlog(const unsigned char* prologBytes, int prologLength, DeepHookPrologUndo* out) {
    if (!prologBytes || prologLength < 0) {
        return false;
    }

    int delta = 0;
    int pos = 0;
    int rbpPushDelta = -1;  // stack delta just after `push rbp`
    bool rbpClobbered = false;
    while (pos < prologLength) {
        const unsigned char b = prologBytes[pos];

        // push r64 (with or without a REX prefix); 55 / 40 55 is push rbp, REX.B 41 55 is push r13
        if (b >= 0x50 && b <= 0x57) {
            delta += 8;
            if (b == 0x55 && rbpPushDelta < 0 && !rbpClobbered) {
                rbpPushDelta = delta;
            }
            pos += 1;
            continue;
        }
        if (b >= 0x40 && b <= 0x4F && pos + 1 < prologLength && prologBytes[pos + 1] >= 0x50 &&
            prologBytes[pos + 1] <= 0x57) {
            delta += 8;
            if (prologBytes[pos + 1] == 0x55 && (b & 0x01u) == 0u && rbpPushDelta < 0 && !rbpClobbered) {
                rbpPushDelta = delta;
            }
            pos += 2;
            continue;
        }

        // lea rbp, [rsp+disp8|disp32] — only once the caller's RBP is on the stack
        if (b == 0x48 && pos + 4 < prologLength && prologBytes[pos + 1] == 0x8D && prologBytes[pos + 3] == 0x24 &&
            (prologBytes[pos + 2] == 0x6C || prologBytes[pos + 2] == 0xAC)) {
            const int instructionLength = prologBytes[pos + 2] == 0x6C ? 5 : 8;
            if (rbpPushDelta < 0 || pos + instructionLength > prologLength) {
                return false;
            }
            rbpClobbered = true;
            pos += instructionLength;
            continue;
        }

        // sub rsp, imm8 / imm32
        if (b == 0x48 && pos + 3 < prologLength && prologBytes[pos + 1] == 0x83 && prologBytes[pos + 2] == 0xEC) {
            const unsigned char imm8 = prologBytes[pos + 3];
            if ((imm8 & 0x80u) != 0u) {
                // A negative imm8 grows the frame back instead of reserving it — not a
                // prolog shape CE may undo blindly.
                return false;
            }
            delta += static_cast<int>(imm8);
            pos += 4;
            continue;
        }
        if (b == 0x48 && pos + 6 < prologLength && prologBytes[pos + 1] == 0x81 && prologBytes[pos + 2] == 0xEC) {
            unsigned int imm = 0;
            for (int i = 0; i < 4; ++i) {
                imm |= static_cast<unsigned int>(prologBytes[pos + 3 + i]) << (8 * i);
            }
            if (imm > 0x7FFFFFFFu) {
                return false;
            }
            delta += static_cast<int>(imm);
            pos += 7;
            continue;
        }

        // mov [rsp(+disp8|disp32)], r64 — REX.W (0x48..0x4F) 89 /r with rm=100 and SIB base=RSP.
        if (b >= 0x48 && b <= 0x4F && pos + 3 < prologLength && prologBytes[pos + 1] == 0x89) {
            const unsigned char modrm = prologBytes[pos + 2];
            const unsigned char mod = static_cast<unsigned char>(modrm >> 6);
            const unsigned char rm = static_cast<unsigned char>(modrm & 0x07);
            const unsigned char sib = prologBytes[pos + 3];
            const bool sibIsPlainRsp = sib == 0x24;  // scale=1, index=none, base=RSP
            if (rm == 0x04 && mod != 0x03 && sibIsPlainRsp) {
                const int instructionLength = (mod == 0x00) ? 4 : (mod == 0x01 ? 5 : 8);
                if (pos + instructionLength > prologLength) {
                    return false;
                }
                pos += instructionLength;
                continue;
            }
        }

        return false;
    }

    if (out) {
        out->stackDelta = delta;
        out->restoreRbp = rbpClobbered;
        out->rbpSlotOffsetFromPrologRsp = rbpClobbered ? delta - rbpPushDelta : 0;
    }
    return true;
}

// The stack-only form: refuses any prolog whose undo would also have to restore a register.
inline bool TryComputeDeepHookPrologStackDelta(const unsigned char* prologBytes, int prologLength,
                                               int* stackDeltaOut) {
    DeepHookPrologUndo undo;
    if (!TryAnalyzeDeepHookProlog(prologBytes, prologLength, &undo) || undo.restoreRbp) {
        return false;
    }
    if (stackDeltaOut) {
        *stackDeltaOut = undo.stackDelta;
    }
    return true;
}

}  // namespace ce::inline_hook_policy
