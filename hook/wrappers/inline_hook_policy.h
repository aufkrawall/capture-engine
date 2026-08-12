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
//
// The shadow-space save is what dxgi!CDXGISwapChain::Present opens with, so refusing it
// (as a pushes-only rule does) rules out deep-hooking Present at all. Anything else is
// refused: an unrecognized prolog cannot be undone safely.
//
// Returns false when a byte sequence is unrecognized or an instruction would run past
// `prologLength`; on success `*stackDeltaOut` is the total number of bytes RSP dropped.
inline bool TryComputeDeepHookPrologStackDelta(const unsigned char* prologBytes, int prologLength,
                                               int* stackDeltaOut) {
    if (!prologBytes || prologLength < 0) {
        return false;
    }

    int delta = 0;
    int pos = 0;
    while (pos < prologLength) {
        const unsigned char b = prologBytes[pos];

        // push r64 (with or without a REX prefix)
        if (b >= 0x50 && b <= 0x57) {
            delta += 8;
            pos += 1;
            continue;
        }
        if (b >= 0x40 && b <= 0x4F && pos + 1 < prologLength && prologBytes[pos + 1] >= 0x50 &&
            prologBytes[pos + 1] <= 0x57) {
            delta += 8;
            pos += 2;
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

    if (stackDeltaOut) {
        *stackDeltaOut = delta;
    }
    return true;
}

}  // namespace ce::inline_hook_policy
