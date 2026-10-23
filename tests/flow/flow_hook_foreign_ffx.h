#pragma once

#include "hook/ffx/ffx_hook_internal.h"
#include "hook/hooking/inline_hook_internal.h"
#include "hook/hooking/inline_hook_lde.h"

// Independent five-byte detours for the fake tool. Its saved originals resume in AMD's body, as Microsoft
// Detours does; neither the cached-pointer router nor an export breakpoint sees its output calls.
namespace {
struct ForeignFFXEntry {
    void* target = nullptr;
    unsigned char saved[32]{};
    unsigned char patch[32]{};
    size_t size = 0;
};
ForeignFFXEntry g_foreignFFXEntries[3];
bool g_foreignFFXConfigureWasDisarmed = false;

bool WriteForeignFFXEntry(void* target, const void* expected, const void* replacement, size_t size) {
    ce::hook_patch::ThreadQuiescence quiescence(target, size);
    if (!quiescence.IsReady() || std::memcmp(target, expected, size) != 0)
        return false;
    DWORD protection = 0;
    if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &protection))
        return false;
    std::memcpy(target, replacement, size);
    DWORD ignored = 0;
    const bool protectedAgain = VirtualProtect(target, size, protection, &ignored) != FALSE;
    return FlushInstructionCache(GetCurrentProcess(), target, size) != FALSE && protectedAgain;
}
}  // namespace

extern "C" __declspec(dllexport) void CEFlow_PauseFFXEntryObservation(bool pause) {
    if (pause) {
        SuspendFfxCreateContextBreakpoint("flow foreign FFX install");
        g_foreignFFXConfigureWasDisarmed = ffx_hook_g_ffxConfigureVehPermanentlyDisarmed.exchange(true);
        RestoreFfxConfigureBreakpointIfCurrent(ffx_hook_g_ffxConfigureTarget.load(), "flow foreign FFX install");
    } else {
        ffx_hook_g_ffxConfigureVehPermanentlyDisarmed.store(g_foreignFFXConfigureWasDisarmed);
        ResumeFfxCreateContextBreakpoint("flow foreign FFX installed");
    }
}

extern "C" __declspec(dllexport) bool CEFlow_InstallForeignFFXEntry(void* target, void* handler, void** original,
                                                                    unsigned index) {
    using namespace InlineHook;
    if (index >= 3 || !target || !handler || !original)
        return false;
    std::lock_guard<std::mutex> lock(g_hookMutex);
    auto& entry = g_foreignFFXEntries[index];
    const auto* code = static_cast<const uint8_t*>(target);
    int size = 0;
    while (size < 5) {
        const int length = GetInstructionLength(code + size, true);
        // The fake's prefixes are stack/register saves. Refuse a relative operand rather than invent a relocation.
        if (length <= 0 || size + length > 32 || GetRipRelativeDispOffset(code + size, length, true) >= 0)
            return false;
        size += length;
    }
    auto* trampoline = GetTrampolineSlot(target);
    if (!trampoline)
        return false;
    std::memcpy(trampoline, code, size);
    if (!WriteJump(trampoline + size, static_cast<uint8_t*>(target) + size) || !WriteJump(trampoline + 48, handler))
        return false;
    const int64_t distance = reinterpret_cast<intptr_t>(trampoline + 48) - reinterpret_cast<intptr_t>(target) - 5;
    if (distance < INT32_MIN || distance > INT32_MAX || !FinalizeCurrentTrampoline(trampoline, 64))
        return false;
    entry.target = target;
    entry.size = static_cast<size_t>(size);
    std::memcpy(entry.saved, code, size);
    std::memset(entry.patch, 0xCC, size);
    entry.patch[0] = 0xE9;
    const auto displacement = static_cast<int32_t>(distance);
    std::memcpy(entry.patch + 1, &displacement, sizeof(displacement));
    *original = trampoline;
    return WriteForeignFFXEntry(target, entry.saved, entry.patch, entry.size);
}

extern "C" __declspec(dllexport) bool CEFlow_DetachForeignFFXEntries() {
    bool restored = true;
    for (auto& entry : g_foreignFFXEntries) {
        if (entry.target) {
            restored &= WriteForeignFFXEntry(entry.target, entry.patch, entry.saved, entry.size);
            entry.target = nullptr;
        }
    }
    return restored;
}
