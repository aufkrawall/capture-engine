#pragma once

#include <cstddef>
#include <cstdint>

#include "hook/overlay/overlay_compat_detail/module_address_cache.h"
#include "hook/overlay/overlay_compat_detail/module_table.h"

namespace ce::overlay_compat {

// What per-call classifiers said about one code address. They scan the module's path for a few dozen
// tokens on every Present and ExecuteCommandLists, and the answer cannot change while the module stays
// mapped and the identified-overlay list stands, so one small table per thread keeps it. The reference
// stays valid until the N-th later miss on the same thread.
template <typename Verdict, std::size_t N>
class CallerVerdictCache {
public:
    // resolve(Verdict&) fills the verdict and returns whether it may be kept.
    template <typename Resolve>
    const Verdict& Get(const void* address, std::uint64_t unloadGeneration, std::uint64_t overlayListGeneration,
                       bool cacheable, Resolve&& resolve) {
        if (cacheable) {
            for (const Entry& entry : entries_) {
                if (entry.address == address && entry.unloadGeneration == unloadGeneration &&
                    entry.overlayListGeneration == overlayListGeneration) {
                    return entry.verdict;
                }
            }
        }
        Entry& entry = entries_[next_];
        next_ = (next_ + 1) % N;
        entry.address = nullptr;
        if (resolve(entry.verdict) && cacheable) {
            entry.address = address;
            entry.unloadGeneration = unloadGeneration;
            entry.overlayListGeneration = overlayListGeneration;
        }
        return entry.verdict;
    }

private:
    struct Entry {
        const void* address = nullptr;
        std::uint64_t unloadGeneration = 0;
        std::uint64_t overlayListGeneration = 0;
        Verdict verdict{};
    };
    Entry entries_[N] = {};
    std::size_t next_ = 0;
};

// A lookup keyed on what can change a verdict: an unload, or a new overlay list. Without the unload
// notification nothing could invalidate an entry, so every call resolves.
template <typename Verdict, std::size_t N, typename Resolve>
inline const Verdict& GetCallerVerdict(CallerVerdictCache<Verdict, N>& cache, const void* address, Resolve&& resolve) {
    const bool cacheable = address != nullptr && module_address_cache::IsEnabled();
    return cache.Get(address, cacheable ? module_address_cache::UnloadGeneration() : 0,
                     IdentifiedOverlayPathRefreshSequence().load(std::memory_order_acquire), cacheable, resolve);
}

}  // namespace ce::overlay_compat
