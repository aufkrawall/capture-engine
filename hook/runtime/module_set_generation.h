#pragma once

// A version of the process's module set: it changes with every DLL load and unload the loader
// reports (LdrRegisterDllNotification). Lets a per-frame caller keep the answer of a module check
// (a version resource, a loader lookup) until a module comes or goes. 0 when notifications are not
// active: the caller then cannot tell and checks every time.

#include <atomic>

unsigned long long LoadedModuleSetGeneration();

// `compute`'s answer, kept while the module set stays at `current` (LoadedModuleSetGeneration();
// read before computing, so a module arriving meanwhile is checked again next time).
template <typename Compute>
bool CachedForModuleSet(unsigned long long current, std::atomic<unsigned long long>& generation,
                        std::atomic<bool>& answer, Compute&& compute) {
    if (current != 0 && generation.load(std::memory_order_acquire) == current)
        return answer.load(std::memory_order_relaxed);
    const bool value = compute();
    answer.store(value, std::memory_order_relaxed);
    generation.store(current, std::memory_order_release);
    return value;
}
