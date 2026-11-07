// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>

#include "common/capture/detached_live_state.h"

struct SharedMemoryLayout;
struct ShmemBuffer;

namespace ce {

// One pipeline's route to recording state. Retarget waits only for short state
// publications, never codec/GPU work. Both layouts and the pixel buffer remain
// alive until the pipeline's workers have joined. Reads may finish on the old
// layout; writes must finish before Retarget returns and another recording starts.
class RecordingMemory {
public:
    class Publication {
    public:
        explicit Publication(RecordingMemory& owner)
            : lock_(owner.publications_), layout_(owner.layout_.load(std::memory_order_acquire)) {}
        SharedMemoryLayout* operator->() const { return layout_; }
        explicit operator bool() const { return layout_ != nullptr; }

    private:
        std::shared_lock<std::shared_mutex> lock_;
        SharedMemoryLayout* layout_;
    };

    const SharedMemoryLayout* get() const { return layout_.load(std::memory_order_acquire); }
    const SharedMemoryLayout* operator->() const { return get(); }
    explicit operator bool() const { return get() != nullptr; }
    Publication Publish() { return Publication(*this); }
    ShmemBuffer* PixelBuffer() const { return pixels_.load(std::memory_order_acquire); }

    uint64_t Retarget(SharedMemoryLayout* layout, ShmemBuffer* pixels) {
        std::unique_lock lock(publications_);
        if (layout_.load(std::memory_order_relaxed) == layout && pixels_.load(std::memory_order_relaxed) == pixels)
            return 0;  // no ownership transition to report
        pixels_.store(pixels, std::memory_order_release);
        layout_.store(layout, std::memory_order_release);
        return ++generation_;
    }

    uint64_t Detach(const SharedMemoryLayout& fallbackLive, SharedMemoryLayout& detached) {
        std::unique_lock lock(publications_);
        const SharedMemoryLayout* current = layout_.load(std::memory_order_relaxed);
        if (current == &detached)
            return 0;
        // Copy after the last old publication, while new publications are
        // excluded. A pre-barrier copy could lose the producer's final update.
        detached_live_state::CopyForDetachedSession(current ? *current : fallbackLive, detached);
        layout_.store(&detached, std::memory_order_release);
        return ++generation_;
    }

    // Standalone encoder texture setup can provide the initial binding. It must
    // never put the live layout back after the owner has detached the recording.
    void InitializeIfEmpty(SharedMemoryLayout* layout) {
        if (!layout)
            return;
        std::unique_lock lock(publications_);
        if (!layout_.load(std::memory_order_relaxed)) {
            layout_.store(layout, std::memory_order_release);
            ++generation_;
        }
    }

private:
    std::shared_mutex publications_;
    std::atomic<SharedMemoryLayout*> layout_{nullptr};
    std::atomic<ShmemBuffer*> pixels_{nullptr};
    uint64_t generation_ = 0;
};

}  // namespace ce
