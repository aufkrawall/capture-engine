#pragma once

#include <cstddef>
#include <cstdint>

// Pure policy behind ce::module_pin::IsReadableCode.
//
// Kept free of Windows types so the decision can be exercised without
// conjuring real mappings; module_pin.cpp static_asserts these constants
// against the Win32 MEM_*/PAGE_* values it feeds in.
namespace ce::module_pin_policy {

inline constexpr unsigned long kStateCommit = 0x1000;

inline constexpr unsigned long kProtectNoAccess = 0x01;
inline constexpr unsigned long kProtectGuard = 0x100;

// PAGE_EXECUTE (0x10) is deliberately absent: it grants execute without read.
// No real image .text is mapped that way, so refusing it costs nothing and
// keeps the probe from depending on the hardware happening to allow the read.
inline constexpr unsigned long kProtectExecuteRead = 0x20;
inline constexpr unsigned long kProtectExecuteReadWrite = 0x40;
inline constexpr unsigned long kProtectExecuteWriteCopy = 0x80;

inline constexpr unsigned long kReadableCodeProtectMask =
    kProtectExecuteRead | kProtectExecuteReadWrite | kProtectExecuteWriteCopy;

// True when `count` bytes starting at `address` may be read as code, given the
// one region a memory query reported for `address`.
//
// The range must stay inside that region, because the next region can be
// unmapped or unreadable. Callers spanning a region boundary must clamp `count`
// to what this region covers and re-query for the remainder: a module's .text
// splits into several regions as soon as anyone leaves a page at a different
// protection, and refusing such a target outright would silently drop a hook.
inline bool IsQueriedRangeReadableCode(bool querySucceeded, uintptr_t regionBase, size_t regionSize,
                                       unsigned long state, unsigned long protect, uintptr_t address, size_t count) {
    if (!querySucceeded || address == 0 || count == 0)
        return false;
    if (state != kStateCommit)
        return false;
    if ((protect & (kProtectNoAccess | kProtectGuard)) != 0)
        return false;
    if ((protect & kReadableCodeProtectMask) == 0)
        return false;
    if (address < regionBase)
        return false;
    const uintptr_t offset = address - regionBase;
    if (offset >= regionSize)
        return false;
    return count <= regionSize - offset;
}

}  // namespace ce::module_pin_policy
