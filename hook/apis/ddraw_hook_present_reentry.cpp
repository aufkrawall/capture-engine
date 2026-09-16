#include "ddraw_hook_internal.h"

#include "../wrappers/inline_hook.h"

#include <array>
#include <unordered_map>

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
// Session 20260916_021049 finally named the caller, on the first occurrence
// after this reporting shipped:
//
//   Re-entered from gameoverlayrenderer.dll+0x76ACC; CE's saved original is
//   DDRAW.dll+0x37C50; the entry point recorded before CE patched the slot was
//   DDRAW.dll+0x37C50
//
// CE's saved original is genuine DirectDraw, so the two overlays are not holding
// each other's vtable pointer. The other injector sits *below* CE: CE calls
// DDRAW+0x37C50, that injector owns the function's entry, and it re-issues the
// presentation through the surface vtable - which is CE's detour. That also
// explains session 20260916_013230, where CE answered the cycle by calling the
// entry point it had recorded before patching the slot: the same address, the
// same patch, 32,768 levels in two milliseconds.
//
// So CE may never answer a nested presentation with the pointer it saved. But
// returning DD_OK is not free either: the nested call in 20260916_021049 was on
// the *primary surface* - the game's real screen flip - and dropping one per
// frame is what left Gothic II showing its menu while the 3D scene ran.
//
// A nested presentation is therefore answered by running the real implementation
// past the foreign entry patch, through a bypass trampoline built from the
// module's own on-disk bytes, exactly as DXGIShared does for a patched
// dxgi!Present. It is allowed once per outermost presentation; a re-entry from
// inside the bypass, or one with no bypass to use, still returns without calling
// anything, so this cannot recurse however the chain above CE is wired.

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

// Only a built trampoline is cached. A negative answer is not durable: the
// injector that patches DirectDraw's entry can install its hook at any point
// after CE installed its own, and caching "no patch" would keep CE dropping
// nested presentations for the rest of the run. Re-probing costs a VirtualQuery
// and two bytes.
std::mutex g_PresentBypassMutex;
std::unordered_map<void*, void*> g_PresentBypasses;
std::unordered_map<void*, uint32_t> g_PresentBypassProbeLogs;

// One line per outcome per target, so a per-frame nested presentation cannot
// turn this into a log flood.
bool ShouldLogBypassProbe(void* target, uint32_t outcome) {
    const auto existing = g_PresentBypassProbeLogs.find(target);
    if (existing != g_PresentBypassProbeLogs.end() && existing->second == outcome)
        return false;
    g_PresentBypassProbeLogs[target] = outcome;
    return true;
}

bool SavedOriginalCarriesForeignEntryPatch(void* savedOriginal) {
    if (!savedOriginal)
        return false;
    MEMORY_BASIC_INFORMATION memory = {};
    if (VirtualQuery(savedOriginal, &memory, sizeof(memory)) != sizeof(memory) || memory.State != MEM_COMMIT ||
        (memory.Protect & PAGE_GUARD) != 0) {
        return false;
    }
    constexpr DWORD kExecutable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((memory.Protect & kExecutable) == 0)
        return false;
    const auto* code = static_cast<const unsigned char*>(savedOriginal);
    return ce::ddraw_present_policy::EntryLooksInlinePatched(code, 2);
}

}  // namespace

void* AcquireDirectDrawPresentEntryBypass(void* ddraw_hook_savedOriginal, const char* ddraw_hook_operation) {


    if (!ddraw_hook_savedOriginal || HookIsShuttingDown())
        return nullptr;

    {
        std::lock_guard<std::mutex> lock(g_PresentBypassMutex);
        const auto existing = g_PresentBypasses.find(ddraw_hook_savedOriginal);
        if (existing != g_PresentBypasses.end())
            return existing->second;
    }

    // Only a patched entry needs a bypass. An unpatched original is either
    // genuinely DirectDraw's - in which case the nested call did not arrive
    // through it - or something CE cannot reason about, and building a
    // trampoline over ordinary code would be a guess.
    const bool patched = SavedOriginalCarriesForeignEntryPatch(ddraw_hook_savedOriginal);
    void* bypass = patched ? InlineHook::CreateBypassTrampoline(ddraw_hook_savedOriginal) : nullptr;

    bool report = false;
    {
        std::lock_guard<std::mutex> lock(g_PresentBypassMutex);
        if (bypass) {
            const auto [entry, added] = g_PresentBypasses.try_emplace(ddraw_hook_savedOriginal, bypass);
            if (!added)
                return entry->second;
        }
        report = ShouldLogBypassProbe(ddraw_hook_savedOriginal, bypass ? 2u : (patched ? 1u : 0u));
    }

    if (report) {
        char savedName[MAX_PATH + 32] = {};
        DescribeCodeAddress(ddraw_hook_savedOriginal, savedName, sizeof(savedName));
        const char* operation = ddraw_hook_operation ? ddraw_hook_operation : "presentation";
        if (bypass) {
            HookLogImportant(
                "DDraw: CE's saved %s original at %s carries a foreign entry patch - built a bypass trampoline at %p "
                "so a nested presentation runs the real implementation instead of being dropped",
                operation, savedName, bypass);
        } else if (patched) {
            HookLogImportant(
                "DDraw: CE's saved %s original at %s carries a foreign entry patch, but no bypass trampoline could be "
                "built - a nested presentation is dropped rather than recursing",
                operation, savedName);
        } else {
            HookLogImportant(
                "DDraw: CE's saved %s original at %s carries no foreign entry patch - a nested presentation is "
                "dropped rather than calling it",
                operation, savedName);
        }
    }
    return bypass;

}

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
                                void* ddraw_hook_returnAddress, void* ddraw_hook_savedOriginal,
                                const char* ddraw_hook_answer) {


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
        "DDraw: Presentation re-entered CE's own detour (operation=%s surface=%p occurrence=%u answered=%s). "
        "Re-entered from %s; CE's saved original is %s; the entry point recorded before CE patched the slot was %s",
        ddraw_hook_operation ? ddraw_hook_operation : "unknown", ddraw_hook_surface, occurrence,
        ddraw_hook_answer ? ddraw_hook_answer : "unknown", caller, savedOriginal,
        pristine ? pristineName : "<none recorded>");

}
