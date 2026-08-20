#include "module_pin.h"

#include <algorithm>
#include <mutex>
#include <unordered_set>

#include "hook_common.h"
#include "module_pin_policy.h"

static_assert(ce::module_pin_policy::kStateCommit == MEM_COMMIT, "policy MEM_COMMIT drifted");
static_assert(ce::module_pin_policy::kProtectNoAccess == PAGE_NOACCESS, "policy PAGE_NOACCESS drifted");
static_assert(ce::module_pin_policy::kProtectGuard == PAGE_GUARD, "policy PAGE_GUARD drifted");
static_assert(ce::module_pin_policy::kProtectExecuteRead == PAGE_EXECUTE_READ, "policy PAGE_EXECUTE_READ drifted");
static_assert(ce::module_pin_policy::kProtectExecuteReadWrite == PAGE_EXECUTE_READWRITE,
              "policy PAGE_EXECUTE_READWRITE drifted");
static_assert(ce::module_pin_policy::kProtectExecuteWriteCopy == PAGE_EXECUTE_WRITECOPY,
              "policy PAGE_EXECUTE_WRITECOPY drifted");

namespace ce::module_pin {

namespace {

// One log line per module. The wrapper initialization pass re-runs these
// resolutions on every hook-thread tick, so an unconditional line would be pure
// hot-path noise; the first pin per image is the state transition worth seeing.
std::mutex g_PinLogMutex;
std::unordered_set<HMODULE> g_PinnedLogged;

bool NoteFirstPin(HMODULE module) {
    std::lock_guard<std::mutex> lock(g_PinLogMutex);
    return g_PinnedLogged.insert(module).second;
}

void LogFirstPin(HMODULE module, const char* how, const char* what) {
    if (!NoteFirstPin(module))
        return;
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(module, path, MAX_PATH) == 0)
        path[0] = '\0';
    HookLogImportant("ModulePin: pinned %s=%s at %p (%s) - CE holds code pointers into it for the process lifetime",
                     how, what, reinterpret_cast<void*>(module), path);
}

}  // namespace

HMODULE PinByName(const char* moduleName) {
    if (!moduleName || moduleName[0] == '\0')
        return nullptr;
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN, moduleName, &module) || !module)
        return nullptr;
    LogFirstPin(module, "name", moduleName);
    return module;
}

HMODULE PinOwnerOfAddress(const void* address) {
    if (!address)
        return nullptr;
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCSTR>(address), &module) ||
        !module) {
        return nullptr;
    }
    LogFirstPin(module, "address", "code-pointer owner");
    return module;
}

bool IsReadableCode(const void* address, size_t count) {
    if (!address || count == 0)
        return false;
    uintptr_t cursor = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = cursor + count;
    if (end < cursor)
        return false;

    // Walk every region the range touches instead of demanding a single one.
    // A module's .text splits the moment anyone leaves a page at a different
    // protection, and a target that merely sits near such a split is still
    // perfectly readable - refusing it would silently drop a working hook.
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION region = {};
        const bool queried = VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) ==
                             sizeof(region);
        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(region.BaseAddress);
        const size_t regionSize = static_cast<size_t>(region.RegionSize);
        const uintptr_t regionEnd = regionBase + regionSize;
        const size_t coveredHere =
            (queried && regionEnd > cursor) ? static_cast<size_t>(std::min<uintptr_t>(end, regionEnd) - cursor) : 0;
        if (coveredHere == 0)
            return false;
        if (!ce::module_pin_policy::IsQueriedRangeReadableCode(queried, regionBase, regionSize, region.State,
                                                              region.Protect, cursor, coveredHere)) {
            return false;
        }
        cursor += coveredHere;
    }
    return true;
}

}  // namespace ce::module_pin
