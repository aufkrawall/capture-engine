#pragma once

// A recording session's handle on the inject session's shared memory, read by several of its threads while one
// thread may retarget it: when the session stops being the current recording it switches to a private copy
// (common/capture/detached_live_state.h), so nothing it does afterwards reaches the hook or a newer recording.
// Every access loads the target once, atomically. A retarget only ever swaps one valid layout for another while
// threads run; it is set to null only after they have joined (unmapping).

#include <atomic>

struct SharedMemoryLayout;

namespace ce {

class SharedMemoryRef {
public:
    SharedMemoryRef() = default;
    SharedMemoryRef(const SharedMemoryRef&) = delete;
    SharedMemoryRef& operator=(const SharedMemoryRef&) = delete;

    SharedMemoryRef& operator=(SharedMemoryLayout* layout) noexcept {
        target_.store(layout, std::memory_order_release);
        return *this;
    }
    SharedMemoryLayout* get() const noexcept { return target_.load(std::memory_order_acquire); }
    SharedMemoryLayout* operator->() const noexcept { return get(); }
    operator SharedMemoryLayout*() const noexcept { return get(); }

private:
    std::atomic<SharedMemoryLayout*> target_{nullptr};
};

}  // namespace ce
