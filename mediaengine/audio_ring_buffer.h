#pragma once

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <mutex>
#include <vector>

/**
 * Thread-safe Ring Buffer for audio samples (float).
 * Decouples Writer (Audio Capture) from Reader (Video Encoder).
 *
 * Overflow Handling:
 * - When buffer is full, new samples are dropped (not old)
 * - droppedSamples counter tracks total drops of NEW samples for gap compensation
 * - retainedSamples counter tracks OLD samples trimmed to keep the newest audio
 * - Downstream encoder should check GetAndClearDroppedSamples() and insert
 * silence
 */
class AudioRingBuffer {
public:
    AudioRingBuffer(size_t capacitySamples)
        : capacity(capacitySamples),
          readPos(0),
          writePos(0),
          available(0),
          droppedSamples(0) {
        buffer.resize(capacitySamples, 0.0f);
    }

    // Push new samples into the buffer.
    // Returns number of samples written.
    // If buffer full, drops new data and increments droppedSamples counter.
    size_t Write(const float* data, size_t count);

    // Atomically makes room by dropping the OLDEST samples if needed, then
    // writes all of `data`. Used by the capture thread to ensure recent audio
    // always reaches the ring buffer without a race between GetFree()/Skip()/
    // Write(). Trims are tracked separately via GetAndClearRetainedSamples() so
    // telemetry can distinguish latency management from dropped new input.
    // Returns count (always writes all data unless count > capacity).
    size_t WriteRetainNew(const float* data, size_t count);

    // Pull samples from the buffer.
    // Returns number of samples read.
    // If not enough data, returns what is available (Reader must handle
    // underflow/silence).
    size_t Read(float* outData, size_t count);

    // Peek only (for analysis)
    size_t Peek(float* outData, size_t count);

    // Skip (consume without copying) the oldest `count` samples.
    // Does NOT increment droppedSamples; this is intentional latency
    // management. True data-loss tracking is done only by Write() overflow.
    size_t Skip(size_t count);

    // Clear buffer (reset)
    void Clear();

    // Get available samples to read
    size_t GetAvailable() const {
        return available.load(std::memory_order_acquire);
    }

    // Get free space to write
    size_t GetFree() const {
        const size_t currentCapacity = capacity.load(std::memory_order_acquire);
        const size_t currentAvailable = available.load(std::memory_order_acquire);
        return currentAvailable < currentCapacity ? currentCapacity - currentAvailable : 0;
    }

    // Get total capacity
    size_t GetCapacity() const {
        return capacity.load(std::memory_order_acquire);
    }

    // Grow the backing capacity to at least `newCapacitySamples`. Used to defer
    // full ring allocation until a source actually starts capturing (e.g. an
    // app-audio source whose target process may never run), so idle sources do
    // not pre-allocate the large CFR retention buffer.
    //
    // Only grows, never shrinks, and only when the buffer is currently EMPTY
    // (available == 0) so no buffered audio can be lost. Mutex-protected, so it
    // is safe against concurrent Write/Read/Skip. Returns true if the capacity is
    // already sufficient or was successfully grown; false if it could not grow
    // because the buffer was non-empty.
    bool EnsureCapacity(size_t newCapacitySamples) {
        std::lock_guard<std::mutex> lock(mutex);
        const size_t currentCapacity = capacity.load(std::memory_order_relaxed);
        if (newCapacitySamples <= currentCapacity) {
            return true;
        }
        if (available.load(std::memory_order_acquire) != 0) {
            return false;  // refuse to realloc while holding buffered audio
        }
        // Allocate into a temporary so an allocation/length failure leaves the
        // live ring, its indices, and the published capacity untouched.
        std::vector<float> replacement;
        try {
            replacement.assign(newCapacitySamples, 0.0f);
        } catch (const std::exception&) {
            return false;
        }
        buffer.swap(replacement);
        capacity.store(newCapacitySamples, std::memory_order_release);
        readPos = 0;
        writePos = 0;
        return true;
    }

    // Check overflow status (debug)
    bool HasOverflowed() const {
        return overflowFlag.load();
    }
    void ClearOverflow() {
        overflowFlag.store(false);
    }

    // Get total dropped samples since last clear (for gap compensation)
    // Returns the count AND resets to zero atomically
    size_t GetAndClearDroppedSamples() {
        return droppedSamples.exchange(0);
    }

    // Get total oldest samples trimmed since last clear (telemetry only).
    // Returns the count AND resets to zero atomically.
    size_t GetAndClearRetainedSamples() {
        return retainedSamples.exchange(0);
    }

private:
    std::vector<float> buffer;
    std::atomic<size_t> capacity;
    size_t readPos;
    size_t writePos;
    std::atomic<size_t> available;
    std::atomic<bool> overflowFlag{false};
    std::atomic<size_t> droppedSamples{0};   // Track samples dropped due to overflow
    std::atomic<size_t> retainedSamples{0};  // Track oldest samples trimmed to keep latest audio
    std::mutex mutex;
};
