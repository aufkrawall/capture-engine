#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace ce::dx12 {

// Owns a saved vtable method binding from before the slot is patched. Readers never choose a global predecessor.
// Patch must publish *original before making detour callable, as VTableHook::Create does.
template <typename Target>
class ExecuteDispatchRegistry {
public:
    struct Binding {
        void** vtable;
        Target original;
    };
    enum class InstallResult { kCaptured, kKnown, kFollower, kFailed, kRetired };
    struct Installation {
        InstallResult result;
        Target original = nullptr;
    };

    template <typename Patch>
    Installation Install(void** vtable, Target current, Target detour, Patch&& patch) {
        if (!vtable || !current)
            return {InstallResult::kFailed};
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto existing = originals_.find(vtable);
        if (existing != originals_.end() && existing->second.TargetNow()) {
            if (current == detour)
                return {InstallResult::kKnown, existing->second.TargetNow()};
            if (current != existing->second.TargetNow())
                return {InstallResult::kFollower, existing->second.TargetNow()};
        }
        // Reserve the node before patching: allocation failure must not leave a callable detour with no binding.
        typename std::map<void**, Entry>::iterator entry;
        bool inserted = false;
        try {
            const auto result = originals_.try_emplace(vtable);
            entry = result.first;
            inserted = result.second;
        } catch (...) {
            return {InstallResult::kFailed};
        }
        const Target previous = entry->second.original;
        Target captured = previous;
        entry->second.pending = &captured;
        generation_.fetch_add(1, std::memory_order_release);
        bool succeeded = false;
        try {
            succeeded = patch(&captured);
        } catch (...) {
            succeeded = false;  // Run the same binding rollback as a rejected patch.
        }
        // Reset/replacement can reenter the cold operation. The stack output remains valid even if its
        // record was retired; never republish that transaction or read an erased map node afterward.
        entry = originals_.find(vtable);
        if (entry == originals_.end() || entry->second.pending != &captured)
            return {InstallResult::kRetired, captured};
        entry->second.pending = nullptr;
        if (!succeeded || !captured || captured == detour) {
            if (inserted)
                originals_.erase(entry);
            else
                entry->second.original = previous;
            generation_.fetch_add(1, std::memory_order_release);
            return {InstallResult::kFailed};
        }
        entry->second.original = captured;
        generation_.fetch_add(1, std::memory_order_release);
        return {InstallResult::kCaptured, captured};
    }

    // The caller supplies the receiver's own live slot when it has not been intercepted. Never cache that
    // fallback: another owner may change the live slot without changing this registry's generation.
    Target Resolve(void** vtable, Target live, Target detour) const {
        if (!vtable)
            return nullptr;
        static thread_local Cache cache;
        const uint64_t generation = generation_.load(std::memory_order_acquire);
        if (cache.instance == instance_ && cache.generation == generation && cache.vtable == vtable)
            return cache.original;
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto found = originals_.find(vtable);
        if (found == originals_.end() || !found->second.TargetNow())
            return live != detour ? live : nullptr;
        const Target original = found->second.TargetNow();
        cache = {instance_, generation_.load(std::memory_order_relaxed), vtable, original};
        return original;
    }

    // A live foreign entry can still forward to CE after registry reset. Retained patch evidence is
    // CE's predecessor in that chain; using the foreign top entry would form foreign -> CE -> foreign.
    // Resolve the saved/cache pair first, then exact interception evidence, then an untracked live slot.
    // Cold fact readers are never called for an established binding, preserving the common-path cost.
    template <typename Recover, typename ReadLive>
    Target ResolveInterception(void** vtable, Target detour, Recover&& recover, ReadLive&& readLive) const {
        if (!vtable)
            return nullptr;
        if (const Target saved = Resolve(vtable, nullptr, detour))
            return saved;
        if (const Target retained = recover(); retained && retained != detour)
            return retained;
        const Target live = readLive();
        return live != detour ? live : nullptr;
    }

    bool HasBinding(void** vtable) const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto found = originals_.find(vtable);
        return found != originals_.end() && found->second.TargetNow();
    }

    std::vector<Binding> Snapshot() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        std::vector<Binding> result;
        result.reserve(originals_.size());
        for (const auto& [vtable, entry] : originals_) {
            const Target original = entry.TargetNow();
            if (original)
                result.push_back({vtable, original});
        }
        return result;
    }

    uint64_t Generation() const {
        return generation_.load(std::memory_order_acquire);
    }

    // Retiring registry evidence is separate from detaching the physical slot. A still-intercepted slot can
    // recover its exact predecessor from the patch primitive; it cannot inherit an unrelated queue's target.
    void Reset() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        originals_.clear();
        generation_.fetch_add(1, std::memory_order_release);
    }

private:
    struct Entry {
        Target original = nullptr;
        Target* pending = nullptr;  // Only borrowed while the recursive cold-operation lock is held.
        Target TargetNow() const {
            return pending ? *pending : original;
        }
    };
    struct Cache {
        uint64_t instance = 0;
        uint64_t generation = 0;
        void** vtable = nullptr;
        Target original = nullptr;
    };
    inline static std::atomic<uint64_t> nextInstance_{0};
    const uint64_t instance_ = nextInstance_.fetch_add(1, std::memory_order_relaxed) + 1;
    mutable std::recursive_mutex mutex_;
    std::map<void**, Entry> originals_;
    std::atomic<uint64_t> generation_{0};
};

}  // namespace ce::dx12
