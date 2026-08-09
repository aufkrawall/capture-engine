#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace DXGIShared {
namespace detail {

// Max Present-shaped NULL callback slots CE proactively patches per Steam
// overlay build, and the max instruction distance between the slot load and
// the `call (e)ax` that dispatches through it.
constexpr size_t kSteamNullCallbackMaxSlots = 8;
constexpr size_t kSteamNullCallbackMaxCallLead = 16;

// Scan an executable range for Steam's OverlayHookD3D3 NULL-callback pattern:
//   x64: 48 8B 05 <disp32> ... FF D0   (mov rax,[rip+disp]; ...; call rax)
//   x86: A1 <abs32> ... FF D0          (mov eax,[abs32]; ...; call eax)
//        or 8B 05 <abs32> ... FF D0
// The loaded slot must lie inside [moduleStart, moduleEnd). Writes absolute
// slot addresses (not call sites) into slotsOut and returns the count. This is
// pure memory scanning; unit tests exercise both patterns.
inline size_t FindSteamNullCallbackSlotCandidates(const uint8_t* code, size_t codeSize,
                                                  uintptr_t moduleStart, uintptr_t moduleEnd,
                                                  uintptr_t* slotsOut, size_t maxSlots,
                                                  bool x64Pattern) {
    if (!code || !slotsOut || maxSlots == 0) {
        return 0;
    }

    size_t found = 0;
    if (x64Pattern) {
        for (size_t i = 0; i + 7 <= codeSize && found < maxSlots; ++i) {
            if (code[i] != 0x48 || code[i + 1] != 0x8B || code[i + 2] != 0x05) {
                continue;
            }
            int32_t disp = 0;
            std::memcpy(&disp, code + i + 3, sizeof(disp));
            const uintptr_t instrEnd = moduleStart + i + 7;
            const uintptr_t slot = static_cast<uintptr_t>(static_cast<intptr_t>(instrEnd) + disp);
            if (slot < moduleStart || slot >= moduleEnd) {
                continue;
            }
            bool callRax = false;
            const size_t searchEnd = (i + 7 + kSteamNullCallbackMaxCallLead < codeSize)
                                         ? i + 7 + kSteamNullCallbackMaxCallLead
                                         : codeSize;
            for (size_t j = i + 7; j + 1 < searchEnd; ++j) {
                if (code[j] == 0xFF && code[j + 1] == 0xD0) {
                    callRax = true;
                    break;
                }
            }
            if (!callRax) {
                continue;
            }
            slotsOut[found++] = slot;
        }
    } else {
        for (size_t i = 0; i + 6 <= codeSize && found < maxSlots; ++i) {
            uintptr_t slot = 0;
            size_t instrLen = 0;
            if (code[i] == 0xA1) {
                std::memcpy(&slot, code + i + 1, sizeof(uint32_t));
                instrLen = 5;
            } else if (code[i] == 0x8B && code[i + 1] == 0x05) {
                std::memcpy(&slot, code + i + 2, sizeof(uint32_t));
                instrLen = 6;
            } else {
                continue;
            }
            if (slot < moduleStart || slot >= moduleEnd) {
                continue;
            }
            bool callEax = false;
            const size_t searchEnd = (i + instrLen + kSteamNullCallbackMaxCallLead < codeSize)
                                         ? i + instrLen + kSteamNullCallbackMaxCallLead
                                         : codeSize;
            for (size_t j = i + instrLen; j + 1 < searchEnd; ++j) {
                if (code[j] == 0xFF && code[j + 1] == 0xD0) {
                    callEax = true;
                    break;
                }
            }
            if (!callEax) {
                continue;
            }
            slotsOut[found++] = slot;
        }
    }
    return found;
}

} // namespace detail
} // namespace DXGIShared
