#pragma once

#include "hook/d3d9/d3d9_ex_flag_layout.h"

#include <cstddef>
#include <cstdint>

// The classic D3D9 runtime serializes every device API call through a lock owner
// embedded in the device object: a CRITICAL_SECTION followed by a gate dword that
// CLockD3D tests before EnterCriticalSection/LeaveCriticalSection. The gate is
// nonzero exactly for D3DCREATE_MULTITHREADED devices; single-threaded devices
// never touch the section. Both offsets are private and per build, so they are
// decoded from a runtime method that takes the lock and must then pass the data
// layer on CE's own multithreaded Ex device (gate set, a real CRITICAL_SECTION at
// the owner) before CE enters that lock on any device.
namespace ce::d3d9_device_lock {

using ce::d3d9_ex_flag::CodeArch;
using ce::d3d9_ex_flag::MemoryReader;

struct LockLayout {
    uint32_t ownerOffset = 0;  // device object -> CRITICAL_SECTION (the lock owner)
    uint32_t gateOffset = 0;   // lock owner -> dword the runtime tests before locking
    bool operator==(const LockLayout&) const = default;
};

inline constexpr uint32_t kMaxLockFieldOffset = 0x100;

// sizeof(RTL_CRITICAL_SECTION) of the decoded architecture: the gate lies past it.
constexpr uint32_t CriticalSectionSize(CodeArch arch) {
    return arch == CodeArch::X64 ? 40u : 24u;
}

enum class LockCall : uint8_t { Other, Enter, Leave };

// Names the function an indirect `call [slot]` reaches; the hook reads the import
// slot and compares it with the loaded EnterCriticalSection/LeaveCriticalSection.
class ImportResolver {
public:
    virtual LockCall Resolve(uintptr_t slotAddress) const = 0;

protected:
    ~ImportResolver() = default;
};

struct LockDecode {
    bool found = false;
    LockLayout layout;
    LockCall call = LockCall::Other;
    uint32_t gateCodeOffset = 0;  // byte offset of the gate compare inside the scanned code
};

// Decodes, inside `code` (which lives at `codeAddress`):
//   lea r, [base+disp8]                 the owner offset (first non-stack positive lea);
//   cmp dword [R+disp8], 0|reg; je|jne  the gate offset, past the CRITICAL_SECTION;
//   push R (x86) | rcx==R or mov rcx,R (x64); call [import]
// on the path the branch takes when the gate is set, with the import resolving to
// EnterCriticalSection or LeaveCriticalSection.
LockDecode DecodeLockLayout(const uint8_t* code, size_t size, uintptr_t codeAddress, CodeArch arch,
                            const ImportResolver& imports);

enum class LockCheck : uint8_t { Match, ReadFailed, GateMismatch, NotCriticalSection };
const char* LockCheckName(LockCheck check);

// The gate must be nonzero exactly when `expectLocking` (the device was created with
// D3DCREATE_MULTITHREADED). A locking device must also hold an initialized
// CRITICAL_SECTION at the owner: DebugInfo is -1 (no debug record) or points to a
// record of type RTL_CRITSECT_TYPE whose CriticalSection field points back at it.
LockCheck CheckDeviceLock(const MemoryReader& reader, const LockLayout& layout, uintptr_t device, bool expectLocking,
                          CodeArch arch);

}  // namespace ce::d3d9_device_lock
