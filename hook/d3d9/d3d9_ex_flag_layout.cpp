#include "hook/d3d9/d3d9_ex_flag_layout.h"

#include <cstring>

namespace ce::d3d9_ex_flag {
namespace {

struct BaseDisp32 {
    uint8_t reg = 0;
    uint8_t base = 0;
    uint32_t disp = 0;
};

// ModRM mod=10 with a plain base register and disp32. SIB forms (rm=100) are not
// accepted: the runtime's object field accesses never need an index.
bool DecodeBaseDisp32(const uint8_t* p, size_t avail, uint8_t rexR, uint8_t rexB, BaseDisp32& out) {
    if (avail < 5)
        return false;
    const uint8_t modrm = p[0];
    if ((modrm & 0xC0) != 0x80 || (modrm & 0x07) == 0x04)
        return false;
    out.reg = static_cast<uint8_t>(((modrm >> 3) & 0x07) | (rexR << 3));
    out.base = static_cast<uint8_t>((modrm & 0x07) | (rexB << 3));
    std::memcpy(&out.disp, p + 1, sizeof(out.disp));
    return true;
}

// Pointer-sized load `mov r, [base+disp32]`. Returns the instruction length or 0.
size_t DecodePointerLoad(const uint8_t* p, size_t avail, CodeArch arch, uint8_t& dest, uint32_t& disp) {
    size_t pos = 0;
    uint8_t rexR = 0;
    uint8_t rexB = 0;
    if (arch == CodeArch::X64) {
        if (avail < 1 || (p[0] & 0xF8) != 0x48)
            return 0;
        rexR = (p[0] >> 2) & 1;
        rexB = p[0] & 1;
        pos = 1;
    }
    if (avail < pos + 1 || p[pos] != 0x8B)
        return 0;
    BaseDisp32 operand;
    if (!DecodeBaseDisp32(p + pos + 1, avail - pos - 1, rexR, rexB, operand))
        return 0;
    dest = operand.reg;
    disp = operand.disp;
    return pos + 1 + 5;
}

// Dword compare of `[base+disp32]` against a register or immediate zero.
size_t DecodeDwordCompare(const uint8_t* p, size_t avail, CodeArch arch, uint8_t expectedBase, uint32_t& disp) {
    size_t pos = 0;
    uint8_t rexR = 0;
    uint8_t rexB = 0;
    if (arch == CodeArch::X64 && avail >= 1 && (p[0] & 0xF0) == 0x40) {
        if (p[0] & 0x08)
            return 0;  // REX.W: a qword compare is not the BOOL
        rexR = (p[0] >> 2) & 1;
        rexB = p[0] & 1;
        pos = 1;
    }
    if (avail < pos + 1)
        return 0;
    const uint8_t opcode = p[pos];
    BaseDisp32 operand;
    if (opcode == 0x39 || opcode == 0x3B) {
        if (!DecodeBaseDisp32(p + pos + 1, avail - pos - 1, rexR, rexB, operand) || operand.base != expectedBase)
            return 0;
        disp = operand.disp;
        return pos + 1 + 5;
    }
    if (opcode == 0x83) {
        if (!DecodeBaseDisp32(p + pos + 1, avail - pos - 1, 0, rexB, operand) || operand.base != expectedBase)
            return 0;
        if ((operand.reg & 0x07) != 0x07)
            return 0;  // /7 is CMP
        if (avail < pos + 1 + 5 + 1 || p[pos + 1 + 5] != 0x00)
            return 0;
        disp = operand.disp;
        return pos + 1 + 5 + 1;
    }
    return 0;
}

size_t DecodeConditionalJump(const uint8_t* p, size_t avail) {
    if (avail >= 2 && (p[0] == 0x74 || p[0] == 0x75))
        return 2;
    if (avail >= 6 && p[0] == 0x0F && (p[1] == 0x84 || p[1] == 0x85))
        return 6;
    return 0;
}

bool PlausibleLayout(const Layout& layout, CodeArch arch) {
    const uint32_t pointerSize = arch == CodeArch::X64 ? 8u : 4u;
    return layout.factoryFieldOffset != 0 && layout.factoryFieldOffset < kMaxFieldOffset &&
           layout.factoryFieldOffset % pointerSize == 0 && layout.exFlagOffset < kMaxFieldOffset &&
           layout.exFlagOffset % 4 == 0;
}

bool AddressPlus(uintptr_t base, uint32_t offset, uintptr_t& out) {
    if (base == 0 || base > UINTPTR_MAX - offset)
        return false;
    out = base + offset;
    return true;
}

}  // namespace

size_t FindCandidates(const uint8_t* code, size_t size, CodeArch arch, Candidate* out, size_t maxOut) {
    if (!code || !out || maxOut == 0)
        return 0;
    size_t found = 0;
    for (size_t i = 0; i < size && found < maxOut; ++i) {
        uint8_t dest = 0;
        Layout layout;
        const size_t loadLen = DecodePointerLoad(code + i, size - i, arch, dest, layout.factoryFieldOffset);
        if (loadLen == 0)
            continue;
        const size_t cmpLen = DecodeDwordCompare(code + i + loadLen, size - i - loadLen, arch, dest,
                                                 layout.exFlagOffset);
        if (cmpLen == 0)
            continue;
        if (DecodeConditionalJump(code + i + loadLen + cmpLen, size - i - loadLen - cmpLen) == 0)
            continue;
        if (!PlausibleLayout(layout, arch))
            continue;
        bool duplicate = false;
        for (size_t k = 0; k < found; ++k)
            duplicate = duplicate || out[k].layout == layout;
        if (!duplicate)
            out[found++] = {layout, static_cast<uint32_t>(i)};
    }
    return found;
}

const char* ObjectCheckName(ObjectCheck check) {
    switch (check) {
        case ObjectCheck::Match:
            return "match";
        case ObjectCheck::ReadFailed:
            return "read-failed";
        case ObjectCheck::FactoryMismatch:
            return "factory-mismatch";
        case ObjectCheck::FlagMismatch:
            return "flag-mismatch";
    }
    return "?";
}

ObjectCheck CheckObjects(const MemoryReader& reader, const Layout& layout, uintptr_t device, uintptr_t factory,
                         uint32_t expectedFlag) {
    uintptr_t fieldAddress = 0;
    uintptr_t flagAddress = 0;
    if (!AddressPlus(device, layout.factoryFieldOffset, fieldAddress) ||
        !AddressPlus(factory, layout.exFlagOffset, flagAddress))
        return ObjectCheck::ReadFailed;
    uintptr_t storedFactory = 0;
    if (!reader.Read(fieldAddress, &storedFactory, sizeof(storedFactory)))
        return ObjectCheck::ReadFailed;
    if (storedFactory != factory)
        return ObjectCheck::FactoryMismatch;
    uint32_t flag = 0;
    if (!reader.Read(flagAddress, &flag, sizeof(flag)))
        return ObjectCheck::ReadFailed;
    return flag == expectedFlag ? ObjectCheck::Match : ObjectCheck::FlagMismatch;
}

Selection SelectLayout(const MemoryReader& reader, const Candidate* candidates, size_t count, uintptr_t exDevice,
                       uintptr_t exFactory) {
    Selection selection;
    for (size_t i = 0; candidates && i < count; ++i) {
        const ObjectCheck check = CheckObjects(reader, candidates[i].layout, exDevice, exFactory, 1);
        if (check == ObjectCheck::Match) {
            selection.found = true;
            selection.layout = candidates[i].layout;
            return selection;
        }
        selection.rejected++;
        selection.lastReject = check;
    }
    return selection;
}

const char* UnlockDecisionName(UnlockDecision decision) {
    switch (decision) {
        case UnlockDecision::Unlock:
            return "unlock";
        case UnlockDecision::NotNeeded:
            return "ex-device";
        case UnlockDecision::NoLayout:
            return "no-layout";
        case UnlockDecision::NoDeviceLock:
            return "no-device-lock";
    }
    return "?";
}

UnlockDecision DecideUnlock(const UnlockInputs& inputs) {
    if (inputs.deviceIsEx)
        return UnlockDecision::NotNeeded;
    if (!inputs.layoutKnown)
        return UnlockDecision::NoLayout;
    if (inputs.multithreaded && !inputs.deviceLockKnown && !inputs.ceOwnedFactory)
        return UnlockDecision::NoDeviceLock;
    return UnlockDecision::Unlock;
}

}  // namespace ce::d3d9_ex_flag
