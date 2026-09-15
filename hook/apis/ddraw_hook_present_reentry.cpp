#include "ddraw_hook_internal.h"

#include <array>

#include <psapi.h>

// Refusing to re-enter CE's own presentation detours.
//
// CE hooks the DirectDraw surface vtable's Flip/Blt/BltFast slots and keeps the
// pointer it replaced as "the original". A second overlay that hooks the same
// slots does the same thing, and installed in the wrong order each one's saved
// original is the other's detour: calling it is an infinite mutual call.
//
// Gothic II session 20260916_011148 is that cycle with Steam's
// gameoverlayrenderer - three frames repeating down eight megabytes of the
// render thread's stack, a full 4K overlay composite at every level. From
// outside it looked like a game presenting 3,600 times at ~270/s for thirteen
// seconds; not one flip completed.
//
// A nested presentation therefore returns without calling anything. That is the
// only response that cannot recurse by construction. Session 20260916_013230
// is why it is stated that strongly: an earlier attempt called a presentation
// entry point captured from the vtable before CE patched it and validated as
// ddraw-owned, and the cycle continued anyway - 32,768 levels in two
// milliseconds, the game dead five seconds later with no dump. Whatever
// re-enters CE here is not answered by handing it another function to call, so
// the cost is one dropped presentation and the log names the caller instead.

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

// Which module a return address belongs to. Naming the module that re-enters
// CE is the whole diagnostic: it separates a co-resident overlay from
// DirectDraw itself from CE calling back into its own detour, and none of the
// three can be told apart from the fact of the recursion alone.
void DescribeCodeAddress(void* address, char* buffer, size_t capacity) {
    if (!buffer || capacity == 0)
        return;
    buffer[0] = '\0';
    HMODULE module = nullptr;
    if (!address ||
        !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(address), &module) ||
        !module) {
        snprintf(buffer, capacity, "<unknown module>");
        return;
    }

    char path[MAX_PATH] = {};
    const char* baseName = "<unnamed>";
    if (GetModuleFileNameA(module, path, static_cast<DWORD>(sizeof(path))) != 0) {
        baseName = path;
        for (const char* cursor = path; *cursor != '\0'; ++cursor) {
            if (*cursor == '\\' || *cursor == '/')
                baseName = cursor + 1;
        }
    }
    const uintptr_t offset =
        reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module);
    snprintf(buffer, capacity, "%s+0x%zX", baseName, static_cast<size_t>(offset));
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
        "DDraw: Recorded %u of 3 DirectDraw-owned presentation entry points for vtable=%p (diagnostic only - a "
        "presentation cycle is answered by not calling anything)",
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

void NoteDirectDrawPresentCycle(void* ddraw_hook_surface, size_t ddraw_hook_slot, const char* ddraw_hook_operation,
                                void* ddraw_hook_returnAddress, void* ddraw_hook_savedOriginal) {


    static std::atomic<uint32_t> s_cycleLogCount{0};
    const uint32_t occurrence = s_cycleLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (occurrence > 4 && (occurrence & (occurrence - 1)) != 0)
        return;

    char caller[MAX_PATH + 32] = {};
    DescribeCodeAddress(ddraw_hook_returnAddress, caller, sizeof(caller));
    char savedOriginal[MAX_PATH + 32] = {};
    DescribeCodeAddress(ddraw_hook_savedOriginal, savedOriginal, sizeof(savedOriginal));

    void* pristine = ResolveDirectDrawOwnedPresentFunction(ddraw_hook_surface, ddraw_hook_slot);
    char pristineName[MAX_PATH + 32] = {};
    DescribeCodeAddress(pristine, pristineName, sizeof(pristineName));

    HookLogImportant(
        "DDraw: Presentation re-entered CE's own detour (operation=%s surface=%p occurrence=%u) - returning without "
        "presenting rather than recursing. Re-entered from %s; CE's saved original is %s; the entry point recorded "
        "before CE patched the slot was %s",
        ddraw_hook_operation ? ddraw_hook_operation : "unknown", ddraw_hook_surface, occurrence, caller,
        savedOriginal, pristine ? pristineName : "<none recorded>");

}
