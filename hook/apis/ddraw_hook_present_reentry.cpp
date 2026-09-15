#include "ddraw_hook_internal.h"

#include <array>

#include <psapi.h>

// Breaking a presentation hook cycle with a co-resident overlay.
//
// CE hooks the DirectDraw surface vtable's Flip/Blt/BltFast slots and keeps the
// pointer it replaced as "the original". A second overlay that hooks the same
// slots does the same thing. Install them in the wrong order - CE, then the
// other overlay re-hooking on top - and each one's saved original is the
// other's detour, so calling "the original" is calling the other overlay, which
// calls "its original", which is CE again.
//
// Gothic II session 20260916_011148 is that cycle, with Steam's
// gameoverlayrenderer: three frames repeating down eight megabytes of the
// render thread's stack, each level running a full 4K overlay composite. What
// the log looked like from outside was a game presenting 3,600 times at ~270/s
// for thirteen seconds; what actually happened was 3,600 recursion levels and
// not one completed flip, which is why nothing reached the screen, why the
// freeze watchdog never armed, and why the presentation-failure diagnostic -
// which sits after the call that never returns - stayed silent.
//
// The escape is a presentation entry point that provably belongs to DirectDraw
// itself, captured from the vtable before CE patches it and accepted only when
// it lies inside ddraw.dll. A nested detour calls that instead of its saved
// original, so the cycle ends at its first bounce with the flip still
// performed. Without one, the nested call returns without presenting: one
// dropped frame is a bounded cost, a hang is not.

namespace {

struct PresentEntryPoints {
    void** vtable = nullptr;
    std::array<void*, 3> owned = {};  // Flip, Blt, BltFast - null when not ddraw-owned
};

constexpr size_t kFlipIndex = 0;
constexpr size_t kBltIndex = 1;
constexpr size_t kBltFastIndex = 2;

std::mutex g_PresentEntryMutex;
std::vector<PresentEntryPoints> g_PresentEntries;

size_t SlotToIndex(size_t slot) {
    if (slot == DDSURFACE7_VTABLE_FLIP)
        return kFlipIndex;
    if (slot == DDSURFACE7_VTABLE_BLT)
        return kBltIndex;
    if (slot == DDSURFACE7_VTABLE_BLTFAST)
        return kBltFastIndex;
    return static_cast<size_t>(-1);
}

// A function CE is willing to call as an escape has to belong to DirectDraw.
// Anything else is another overlay's detour, which is exactly what must not be
// re-entered.
bool IsDirectDrawOwnedCode(void* candidate) {
    if (!candidate)
        return false;
    HMODULE ddrawModule = GetModuleHandleA("ddraw.dll");
    if (!ddrawModule)
        return false;
    MODULEINFO info = {};
    if (!GetModuleInformation(GetCurrentProcess(), ddrawModule, &info, sizeof(info)))
        return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
    const uintptr_t value = reinterpret_cast<uintptr_t>(candidate);
    return value >= base && value < base + info.SizeOfImage;
}

}  // namespace

void RecordDirectDrawPresentEntryPoints(void** ddraw_hook_surfaceVTable) {


    if (!ddraw_hook_surfaceVTable)
        return;

    PresentEntryPoints entry;
    entry.vtable = ddraw_hook_surfaceVTable;
    const std::array<size_t, 3> slots = {DDSURFACE7_VTABLE_FLIP, DDSURFACE7_VTABLE_BLT, DDSURFACE7_VTABLE_BLTFAST};
    unsigned ownedCount = 0;
    for (size_t i = 0; i < slots.size(); ++i) {
        void* current = ddraw_hook_surfaceVTable[slots[i]];
        if (IsDirectDrawOwnedCode(current)) {
            entry.owned[i] = current;
            ++ownedCount;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_PresentEntryMutex);
        for (const auto& existing : g_PresentEntries) {
            if (existing.vtable == ddraw_hook_surfaceVTable)
                return;
        }
        g_PresentEntries.push_back(entry);
    }

    HookLogImportant(
        "DDraw: Recorded %u of 3 DirectDraw-owned presentation entry points for vtable=%p - these are what breaks a "
        "hook cycle with a co-resident overlay",
        ownedCount, ddraw_hook_surfaceVTable);

}

void* ResolveDirectDrawOwnedPresentFunction(void* ddraw_hook_surface,  size_t ddraw_hook_slot) {


    const size_t index = SlotToIndex(ddraw_hook_slot);
    if (!ddraw_hook_surface || index > kBltFastIndex)
        return nullptr;

    void** vtable = *(void***)ddraw_hook_surface;
    std::lock_guard<std::mutex> lock(g_PresentEntryMutex);
    for (const auto& entry : g_PresentEntries) {
        if (entry.vtable == vtable)
            return entry.owned[index];
    }
    return nullptr;

}

void* AcquireDirectDrawPresentCycleEscape(void* ddraw_hook_surface,  size_t ddraw_hook_slot, 
                                                 const char* ddraw_hook_operation) {


    void* escape = ResolveDirectDrawOwnedPresentFunction(ddraw_hook_surface, ddraw_hook_slot);

    static std::atomic<uint32_t> s_cycleLogCount{0};
    const uint32_t occurrence = s_cycleLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (occurrence <= 4 || (occurrence & (occurrence - 1)) == 0) {
        HookLogImportant(
            "DDraw: Presentation hook cycle detected (operation=%s surface=%p occurrence=%u) - another overlay's "
            "\"original\" re-enters CE's detour; %s",
            ddraw_hook_operation ? ddraw_hook_operation : "unknown", ddraw_hook_surface, occurrence,
            escape ? "calling DirectDraw's own entry point to break it"
                   : "no DirectDraw-owned entry point was recorded, so this presentation is dropped");
    }
    return escape;

}
