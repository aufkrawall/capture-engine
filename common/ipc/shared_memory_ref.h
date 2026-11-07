#pragma once

// A recording session's handle on the inject session's shared memory, read by several of its threads while one
// thread may retarget it: when the session stops being the current recording it switches to a private copy
// (common/capture/detached_live_state.h), so nothing it does afterwards reaches the hook or a newer recording.
// Every access loads the target once, atomically. A retarget only ever swaps one valid layout for another while
// threads run; it is set to null only after they have joined (unmapping).

#include <atomic>
#include "common/capture/recording_memory.h"

struct SharedMemoryLayout;

namespace ce {

class SharedMemoryRef {
public:
    SharedMemoryRef() = default;
    SharedMemoryRef(const SharedMemoryRef&) = delete;
    SharedMemoryRef& operator=(const SharedMemoryRef&) = delete;

    SharedMemoryRef& operator=(SharedMemoryLayout* layout) noexcept {
        memory_.Retarget(layout, nullptr);
        return *this;
    }
    SharedMemoryLayout* get() const noexcept {
        return const_cast<SharedMemoryLayout*>(memory_.get());
    }
    SharedMemoryLayout* operator->() const noexcept { return get(); }
    operator SharedMemoryLayout*() const noexcept { return get(); }

    uint64_t Detach(const SharedMemoryLayout& fallbackLive, SharedMemoryLayout& detached) {
        return memory_.Detach(fallbackLive, detached);
    }
    RecordingMemory::Publication Publish() { return memory_.Publish(); }

private:
    RecordingMemory memory_;
};

}  // namespace ce
