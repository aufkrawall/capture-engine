#pragma once

#include <cstddef>
#include <cstdint>

// Classic (non-Ex) D3D9 devices reject shared resources in the Windows runtime's
// own API validation: the method loads the device's factory and refuses unless the
// factory's internal "created by Direct3DCreate9Ex" BOOL is set. That layout is
// undocumented and moves between d3d9.dll builds, so it is located per process and
// must pass every layer below before CE writes anything:
//   1. decode: the Ex-only CheckResourceResidency loads a device field, compares a
//      dword field of the loaded object and branches (no fixed byte signatures);
//   2. data: on CE's own Ex device the field holds the Ex factory and the dword is
//      TRUE; on the classic target it holds that device's factory and is FALSE;
//   3. function: the caller proves the unlock (shared create rejected without it,
//      accepted with it) before relying on it.
// Any miss leaves the classic device untouched and capture keeps its fallback.
namespace ce::d3d9_ex_flag {

enum class CodeArch : uint8_t { X86, X64 };

struct Layout {
    uint32_t factoryFieldOffset = 0;  // device object -> IDirect3D9 factory pointer
    uint32_t exFlagOffset = 0;        // factory object -> BOOL "is Ex"
    bool operator==(const Layout&) const = default;
};

struct Candidate {
    Layout layout;
    uint32_t codeOffset = 0;  // byte offset of the load inside the scanned function
};

inline constexpr size_t kScanBytes = 256;
inline constexpr uint32_t kMaxFieldOffset = 0x20000;
inline constexpr size_t kMaxCandidates = 8;

// Decodes `load base+disp32 -> r; cmp dword [r+disp32], reg|0; jcc` sequences.
// Returns the number of distinct layouts written to `out` (at most `maxOut`).
size_t FindCandidates(const uint8_t* code, size_t size, CodeArch arch, Candidate* out, size_t maxOut);

class MemoryReader {
public:
    virtual bool Read(uintptr_t address, void* out, size_t size) const = 0;

protected:
    ~MemoryReader() = default;
};

enum class ObjectCheck : uint8_t { Match, ReadFailed, FactoryMismatch, FlagMismatch };
const char* ObjectCheckName(ObjectCheck check);

// The device must hold `factory` at the layout's field and the factory's dword
// must equal `expectedFlag` exactly (0 for classic, 1 for Ex).
ObjectCheck CheckObjects(const MemoryReader& reader, const Layout& layout, uintptr_t device, uintptr_t factory,
                         uint32_t expectedFlag);

struct Selection {
    bool found = false;
    Layout layout;
    size_t rejected = 0;
    ObjectCheck lastReject = ObjectCheck::Match;
};

// First decoded candidate that matches CE's own Ex reference device and factory.
Selection SelectLayout(const MemoryReader& reader, const Candidate* candidates, size_t count, uintptr_t exDevice,
                       uintptr_t exFactory);

enum class UnlockDecision : uint8_t { Unlock, NotNeeded, NoLayout, NoDeviceLock };
const char* UnlockDecisionName(UnlockDecision decision);

struct UnlockInputs {
    bool deviceIsEx = false;
    bool layoutKnown = false;
    bool multithreaded = false;    // D3DCREATE_MULTITHREADED: other threads may call the device
    bool deviceLockKnown = false;  // the runtime's device lock layout was decoded and proven
    bool ceOwnedFactory = false;   // a CE-private factory no application thread uses
};

// The window repoints only the device's factory field, so a multithreaded game
// device needs the runtime's own device lock to keep its other threads out of it;
// without a proven lock layout it keeps the fallback. CE-private devices have no
// application callers.
UnlockDecision DecideUnlock(const UnlockInputs& inputs);

}  // namespace ce::d3d9_ex_flag
