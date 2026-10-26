#include "iat_hook.h"

#include <string>
#include <vector>

#include "common/platform/loaded_pe_image.h"
#include "iat_hook_internal.h"

// Retargets CE's forwarding originals from a resident runtime to the custom DLL that replaces it.
// Names are resolved outside both registry locks: GetProcAddress can enter the loader, and a
// thread holding the loader lock may be waiting for one of these locks in DetourGetProcAddress.
namespace IATHook {

namespace {

struct PendingOriginal {
    std::string name;
    void** slot = nullptr;       // dynamic hook: the original variable itself
    void** iatEntry = nullptr;   // tracked slot: identifies the record
    void* hookFunction = nullptr;
    void* expected = nullptr;
    void* target = nullptr;
};

void NoteMissing(OriginalRetargetResult& result, const std::string& name) {
    if (result.missing++ == 0) {
        result.firstMissing = name;
    }
}

}  // namespace

OriginalRetargetResult RetargetOriginals(HMODULE from, HMODULE to) {
    OriginalRetargetResult result;
    const auto image = ce::pe::LoadedImage::Open(from);
    if (!image || !to) {
        return result;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(image->base);
    const size_t size = image->size;
    const auto inFrom = [&](const void* address) {
        const uintptr_t value = reinterpret_cast<uintptr_t>(address);
        return value >= base && value - base < size;
    };

    std::vector<PendingOriginal> dynamic;
    {
        std::lock_guard<std::mutex> lock(g_DynamicHookLock);
        for (const auto& [name, entry] : g_DynamicHooks) {
            void* current = entry.outOriginal ? *reinterpret_cast<void* volatile*>(entry.outOriginal) : nullptr;
            if (inFrom(current)) {
                dynamic.push_back({name, entry.outOriginal, nullptr, nullptr, current, nullptr});
            }
        }
    }
    for (PendingOriginal& pending : dynamic) {
        pending.target = reinterpret_cast<void*>(GetProcAddress(to, pending.name.c_str()));
        if (!pending.target) {
            NoteMissing(result, pending.name);
            continue;
        }
        // A concurrent writer (a first GetProcAddress interception filling a null original)
        // never stores a superseded address over a moved one: it only fills nulls.
        if (InterlockedCompareExchangePointer(pending.slot, pending.target, pending.expected) == pending.expected) {
            ++result.dynamicHooks;
        }
    }

    std::vector<PendingOriginal> tracked;
    {
        std::lock_guard<std::mutex> lock(g_PatchLock);
        for (const PatchedEntry& entry : g_PatchedEntries) {
            if (inFrom(entry.originalFunction)) {
                tracked.push_back(
                    {entry.functionName, nullptr, entry.iatEntry, entry.hookFunction, entry.originalFunction, nullptr});
            }
        }
    }
    for (PendingOriginal& pending : tracked) {
        pending.target = reinterpret_cast<void*>(GetProcAddress(to, pending.name.c_str()));
        if (!pending.target) {
            NoteMissing(result, pending.name);
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_PatchLock);
        for (PatchedEntry& entry : g_PatchedEntries) {
            for (const PendingOriginal& pending : tracked) {
                if (pending.target && entry.iatEntry == pending.iatEntry && entry.hookFunction == pending.hookFunction &&
                    entry.originalFunction == pending.expected) {
                    entry.originalFunction = pending.target;
                    ++result.trackedSlots;
                    break;
                }
            }
        }
    }
    return result;
}

}  // namespace IATHook
