#include "hook/d3d9/d3d9_device_lock_layout.h"

#include <cstring>

namespace ce::d3d9_device_lock {
namespace {

constexpr uint8_t kRegRcx = 1;
constexpr size_t kNoPath = SIZE_MAX;
constexpr uint16_t kCriticalSectionDebugType = 0;  // RTL_CRITSECT_TYPE

uint32_t PointerSize(CodeArch arch) {
    return arch == CodeArch::X64 ? 8u : 4u;
}

// ModRM mod=01 (disp8) with a plain, non-stack base; returns the base or -1.
int DecodeDisp8Base(uint8_t modrm, uint8_t rexB) {
    const uint8_t rm = modrm & 0x07;
    if ((modrm & 0xC0) != 0x40 || rm == 0x04 || (rm == 0x05 && rexB == 0))
        return -1;
    return rm | (rexB << 3);
}

// `lea r, [base+disp8]` with a positive displacement: the device's lock owner.
bool DecodeOwnerLea(const uint8_t* p, size_t avail, CodeArch arch, uint32_t& disp) {
    size_t pos = 0;
    uint8_t rexB = 0;
    if (arch == CodeArch::X64) {
        if (avail < 1 || (p[0] & 0xF8) != 0x48)
            return false;
        rexB = p[0] & 1;
        pos = 1;
    }
    if (avail < pos + 3 || p[pos] != 0x8D || DecodeDisp8Base(p[pos + 1], rexB) < 0)
        return false;
    const uint8_t byte = p[pos + 2];
    if (byte == 0 || (byte & 0x80) != 0)
        return false;
    disp = static_cast<uint32_t>(byte);
    return true;
}

struct GateCompare {
    uint8_t base = 0;
    uint32_t disp = 0;
    size_t length = 0;
};

// Dword `cmp [base+disp8], 0` or `cmp [base+disp8], reg` (a register the runtime zeroed).
bool DecodeGateCompare(const uint8_t* p, size_t avail, CodeArch arch, GateCompare& out) {
    size_t pos = 0;
    uint8_t rexB = 0;
    if (arch == CodeArch::X64 && avail >= 1 && (p[0] & 0xF0) == 0x40) {
        if (p[0] & 0x08)
            return false;  // REX.W: a qword compare is not the gate
        rexB = p[0] & 1;
        pos = 1;
    }
    if (avail < pos + 3)
        return false;
    const uint8_t opcode = p[pos];
    const uint8_t modrm = p[pos + 1];
    const int base = DecodeDisp8Base(modrm, rexB);
    const uint8_t dispByte = p[pos + 2];
    if (base < 0 || dispByte == 0 || (dispByte & 0x80) != 0)
        return false;
    size_t length = pos + 3;
    if (opcode == 0x83) {
        if (((modrm >> 3) & 0x07) != 0x07 || avail < length + 1 || p[length] != 0x00)
            return false;  // /7 is CMP, against immediate zero only
        length += 1;
    } else if (opcode != 0x39) {
        return false;
    }
    out = {static_cast<uint8_t>(base), static_cast<uint32_t>(dispByte), length};
    return true;
}

// Where execution continues when the gate is set: past `je`, or at the `jne` target.
size_t LockedPathOffset(const uint8_t* code, size_t size, size_t at) {
    int64_t next = 0;
    int64_t displacement = 0;
    bool jumpsWhenSet = false;
    if (at + 2 <= size && (code[at] == 0x74 || code[at] == 0x75)) {
        next = static_cast<int64_t>(at + 2);
        const uint8_t raw = code[at + 1];
        displacement = (raw & 0x80) ? static_cast<int64_t>(raw) - 256 : static_cast<int64_t>(raw);
        jumpsWhenSet = code[at] == 0x75;
    } else if (at + 6 <= size && code[at] == 0x0F && (code[at + 1] == 0x84 || code[at + 1] == 0x85)) {
        int32_t rel = 0;
        std::memcpy(&rel, code + at + 2, sizeof(rel));
        next = static_cast<int64_t>(at + 6);
        displacement = rel;
        jumpsWhenSet = code[at + 1] == 0x85;
    } else {
        return kNoPath;
    }
    const int64_t path = jumpsWhenSet ? next + displacement : next;
    return path >= 0 && path < static_cast<int64_t>(size) ? static_cast<size_t>(path) : kNoPath;
}

// `mov rcx, base` as REX.W 8B /r (reg=rcx) or REX.W 89 /r (rm=rcx).
bool DecodeMoveToRcx(const uint8_t* p, size_t avail, uint8_t base) {
    if (avail < 3)
        return false;
    const uint8_t low = base & 0x07;
    const uint8_t high = base >> 3;
    if (p[0] == (0x48 | high) && p[1] == 0x8B && p[2] == (0xC0 | (kRegRcx << 3) | low))
        return true;
    return p[0] == (0x48 | (high << 2)) && p[1] == 0x89 && p[2] == (0xC0 | (low << 3) | kRegRcx);
}

// The lock owner (in `base`) passed to an indirect call through an import slot.
LockCall DecodeLockCall(const uint8_t* code, size_t size, size_t at, uintptr_t codeAddress, CodeArch arch,
                        uint8_t base, const ImportResolver& imports) {
    size_t pos = at;
    if (arch == CodeArch::X86) {
        if (base > 7 || pos >= size || code[pos] != 0x50 + base)
            return LockCall::Other;  // push base
        pos += 1;
        if (pos + 6 > size || code[pos] != 0xFF || code[pos + 1] != 0x15)
            return LockCall::Other;
        uint32_t slot = 0;
        std::memcpy(&slot, code + pos + 2, sizeof(slot));
        return imports.Resolve(slot);
    }
    if (base != kRegRcx) {
        if (!DecodeMoveToRcx(code + pos, size - pos, base))
            return LockCall::Other;
        pos += 3;
    }
    if (pos < size && code[pos] == 0x48)
        pos += 1;  // REX.W on the indirect call
    if (pos + 6 > size || code[pos] != 0xFF || code[pos + 1] != 0x15)
        return LockCall::Other;
    int32_t rel = 0;
    std::memcpy(&rel, code + pos + 2, sizeof(rel));
    const auto next = static_cast<int64_t>(codeAddress + pos + 6);
    return imports.Resolve(static_cast<uintptr_t>(next + rel));
}

bool ReadPointer(const MemoryReader& reader, uintptr_t address, CodeArch arch, uint64_t& out) {
    out = 0;
    return reader.Read(address, &out, PointerSize(arch));
}

}  // namespace

LockDecode DecodeLockLayout(const uint8_t* code, size_t size, uintptr_t codeAddress, CodeArch arch,
                            const ImportResolver& imports) {
    LockDecode decode;
    if (!code)
        return decode;
    uint32_t owner = 0;
    bool ownerFound = false;
    for (size_t i = 0; i < size && !ownerFound; ++i)
        ownerFound = DecodeOwnerLea(code + i, size - i, arch, owner);
    if (!ownerFound || owner % PointerSize(arch) != 0 || owner >= kMaxLockFieldOffset)
        return decode;

    for (size_t i = 0; i < size; ++i) {
        // A REX byte right before belongs to this instruction (it may widen it or
        // change its base); the compare is decoded only from that prefix.
        if (arch == CodeArch::X64 && i > 0 && (code[i - 1] & 0xF0) == 0x40)
            continue;
        GateCompare gate;
        if (!DecodeGateCompare(code + i, size - i, arch, gate))
            continue;
        if (gate.disp < CriticalSectionSize(arch) || gate.disp % 4 != 0 || gate.disp >= kMaxLockFieldOffset)
            continue;
        const size_t path = LockedPathOffset(code, size, i + gate.length);
        if (path == kNoPath)
            continue;
        const LockCall call = DecodeLockCall(code, size, path, codeAddress, arch, gate.base, imports);
        if (call == LockCall::Other)
            continue;
        decode.found = true;
        decode.layout = {owner, gate.disp};
        decode.call = call;
        decode.gateCodeOffset = static_cast<uint32_t>(i);
        return decode;
    }
    return decode;
}

const char* LockCheckName(LockCheck check) {
    switch (check) {
        case LockCheck::Match:
            return "match";
        case LockCheck::ReadFailed:
            return "lock-read-failed";
        case LockCheck::GateMismatch:
            return "lock-gate-mismatch";
        case LockCheck::NotCriticalSection:
            return "lock-not-critical-section";
    }
    return "?";
}

LockCheck CheckDeviceLock(const MemoryReader& reader, const LockLayout& layout, uintptr_t device, bool expectLocking,
                          CodeArch arch) {
    const uint64_t span = static_cast<uint64_t>(layout.ownerOffset) + layout.gateOffset;
    if (device == 0 || device > UINTPTR_MAX - span)
        return LockCheck::ReadFailed;
    const uintptr_t owner = device + layout.ownerOffset;
    uint32_t gate = 0;
    if (!reader.Read(owner + layout.gateOffset, &gate, sizeof(gate)))
        return LockCheck::ReadFailed;
    if ((gate != 0) != expectLocking)
        return LockCheck::GateMismatch;
    if (!expectLocking)
        return LockCheck::Match;

    uint64_t debugInfo = 0;
    if (!ReadPointer(reader, owner, arch, debugInfo))
        return LockCheck::ReadFailed;
    const uint64_t noDebugInfo = arch == CodeArch::X64 ? UINT64_MAX : UINT32_MAX;
    if (debugInfo == noDebugInfo)
        return LockCheck::Match;
    if (debugInfo == 0 || debugInfo > UINTPTR_MAX - 2 * PointerSize(arch))
        return LockCheck::NotCriticalSection;
    uint16_t type = 0xFFFF;
    uint64_t backPointer = 0;
    if (!reader.Read(static_cast<uintptr_t>(debugInfo), &type, sizeof(type)) ||
        !ReadPointer(reader, static_cast<uintptr_t>(debugInfo) + PointerSize(arch), arch, backPointer))
        return LockCheck::NotCriticalSection;
    return type == kCriticalSectionDebugType && backPointer == owner ? LockCheck::Match
                                                                     : LockCheck::NotCriticalSection;
}

}  // namespace ce::d3d9_device_lock
