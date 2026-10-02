#pragma once

#include <atomic>
#include <cstdint>

namespace ce {

// One cache per calling thread and registry. The loader reads both state and generation under the
// registry mutex. An unchanged registry requires only an acquire load on the presentation thread.
// This caches a value, not a lifetime pin: the owner must still drain callbacks before destruction.
template <typename State>
class CallbackSnapshotCache {
public:
    struct Snapshot {
        State state = {};
        uint64_t generation = 0;
    };

    template <typename Loader>
    State Resolve(void* key, const std::atomic<uint64_t>& generation, Loader&& loader) {
        const uint64_t observed = generation.load(std::memory_order_acquire);
        if (!valid_ || key_ != key || snapshot_.generation != observed) {
            snapshot_ = loader(key);
            key_ = key;
            valid_ = true;
        }
        return snapshot_.state;
    }

private:
    void* key_ = nullptr;
    Snapshot snapshot_ = {};
    bool valid_ = false;
};

}  // namespace ce
