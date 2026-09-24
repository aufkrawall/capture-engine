#include "present_reentry_guard.h"

#include "ddraw_present_policy.h"
#include "hook_common.h"

#include "../wrappers/inline_hook.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_map>

// The bypass and reporting mechanics, mirrored from the proven DirectDraw path
// (ddraw_hook_present_reentry.cpp - see its header comment for the failure this
// exists for). Only a built trampoline is cached: a negative answer is not
// durable, because the injector that patches an entry can install its hook at
// any point after CE installed its own, and caching "no patch" would keep CE
// dropping nested presentations for the rest of the run.

namespace ce::present_reentry {

namespace {

namespace policy = ce::ddraw_present_policy;

struct ThreadState {
    int depth = 0;
    bool bypassUsed = false;
    long lastResult = 0;
    bool haveLastResult = false;
};

// Families are process-wide (one per API plus a few for tests). A slot wraps by
// construction count; the detour units declare at most three.
constexpr unsigned kMaxFamilies = 32;
thread_local ThreadState t_familyState[kMaxFamilies];
std::atomic<unsigned> g_familySlots{0};

ThreadState& StateFor(unsigned slot) {
    return t_familyState[slot % kMaxFamilies];
}

// Which module a return address belongs to, as "name+0x<offset>".
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

bool SavedOriginalCarriesForeignEntryPatch(void* savedOriginal) {
    if (!savedOriginal)
        return false;
    MEMORY_BASIC_INFORMATION memory = {};
    if (VirtualQuery(savedOriginal, &memory, sizeof(memory)) != sizeof(memory) || memory.State != MEM_COMMIT ||
        (memory.Protect & PAGE_GUARD) != 0) {
        return false;
    }
    constexpr DWORD kExecutable =
        PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((memory.Protect & kExecutable) == 0)
        return false;
    const auto* code = static_cast<const unsigned char*>(savedOriginal);
    return policy::EntryLooksInlinePatched(code, 2);
}

std::mutex g_presentBypassMutex;
std::unordered_map<void*, void*> g_presentBypasses;
std::unordered_map<void*, uint32_t> g_presentBypassProbeLogs;

// One line per outcome per target, so a per-frame nested presentation cannot
// turn this into a log flood.
bool ShouldLogBypassProbe(void* target, uint32_t outcome) {
    const auto existing = g_presentBypassProbeLogs.find(target);
    if (existing != g_presentBypassProbeLogs.end() && existing->second == outcome)
        return false;
    g_presentBypassProbeLogs[target] = outcome;
    return true;
}

}  // namespace

void* BuildInlineEntryBypass(void* target) {
    return InlineHook::CreateBypassTrampoline(target);
}

void* AcquirePresentEntryBypass(void* savedOriginal, const char* operation, BypassBuilder builder) {
    if (!savedOriginal || HookIsShuttingDown())
        return nullptr;

    {
        std::lock_guard<std::mutex> lock(g_presentBypassMutex);
        const auto existing = g_presentBypasses.find(savedOriginal);
        if (existing != g_presentBypasses.end())
            return existing->second;
    }

    // Only a patched entry needs a bypass. An unpatched original is either
    // genuinely the runtime's - in which case the nested call did not arrive
    // through it - or something CE cannot reason about, and calling it is what
    // the mutual-hook cycle is made of.
    const bool patched = SavedOriginalCarriesForeignEntryPatch(savedOriginal);
    void* bypass = nullptr;
    if (patched) {
        BypassBuilder build = builder ? builder : &BuildInlineEntryBypass;
        bypass = build(savedOriginal);
    }

    bool report = false;
    {
        std::lock_guard<std::mutex> lock(g_presentBypassMutex);
        if (bypass) {
            const auto [entry, added] = g_presentBypasses.try_emplace(savedOriginal, bypass);
            if (!added)
                return entry->second;
        }
        report = ShouldLogBypassProbe(savedOriginal, bypass ? 2u : (patched ? 1u : 0u));
    }

    if (report) {
        char savedName[MAX_PATH + 32] = {};
        DescribeCodeAddress(savedOriginal, savedName, sizeof(savedName));
        const char* label = operation ? operation : "presentation";
        if (bypass) {
            HookLogImportant(
                "%s: CE's saved original at %s carries a foreign entry patch - built a bypass trampoline at %p so a "
                "nested presentation runs the real implementation instead of being dropped",
                label, savedName, bypass);
        } else if (patched) {
            HookLogImportant(
                "%s: CE's saved original at %s carries a foreign entry patch, but no bypass trampoline could be "
                "built - a nested presentation is dropped rather than recursing",
                label, savedName);
        } else {
            HookLogImportant(
                "%s: CE's saved original at %s carries no foreign entry patch - a nested presentation is dropped "
                "rather than calling it",
                label, savedName);
        }
    }
    return bypass;
}

void NoteNestedPresentation(void* savedOriginal, void* returnAddress, const char* answer) {
    static std::atomic<uint32_t> s_nestedLogCount{0};
    const uint32_t occurrence = s_nestedLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (occurrence > 4 && (occurrence & (occurrence - 1)) != 0)
        return;

    char caller[MAX_PATH + 32] = {};
    DescribeCodeAddress(returnAddress, caller, sizeof(caller));
    char savedName[MAX_PATH + 32] = {};
    DescribeCodeAddress(savedOriginal, savedName, sizeof(savedName));

    HookLogImportant(
        "Present: A presentation re-entered CE's own detour (occurrence=%u answered=%s). Re-entered from %s; "
        "CE's saved original is %s",
        occurrence, answer ? answer : "unknown", caller, savedName);
}

PresentReentryFamily::PresentReentryFamily(const char* operation, long apiSuccessResult) noexcept
    : operation_(operation), apiSuccessResult_(apiSuccessResult), slot_(g_familySlots.fetch_add(1)) {
}

PresentReentryScope::PresentReentryScope(const PresentReentryFamily& family) : family_(&family) {
    ThreadState& state = StateFor(family.slot());
    nestedLevel_ = state.depth;
    ++state.depth;
}

PresentReentryScope::~PresentReentryScope() {
    ThreadState& state = StateFor(family_->slot());
    if (--state.depth == 0)
        state.bypassUsed = false;
}

void PresentReentryScope::RecordResult(long result) {
    ThreadState& state = StateFor(family_->slot());
    state.lastResult = result;
    state.haveLastResult = true;
}

bool PresentReentryScope::MayRunRealImplementation(bool bypassAvailable) const {
    const ThreadState& state = StateFor(family_->slot());
    return policy::NestedPresentationMayRunRealImplementation(nestedLevel_, bypassAvailable, state.bypassUsed);
}

void PresentReentryScope::MarkBypassUsed() {
    StateFor(family_->slot()).bypassUsed = true;
}

long PresentReentryScope::DropResult() const {
    const ThreadState& state = StateFor(family_->slot());
    return state.haveLastResult ? state.lastResult : family_->apiSuccessResult();
}

}  // namespace ce::present_reentry
