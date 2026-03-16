#pragma once

#include <algorithm>
#include <atomic>
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
        buffer.resize(capacity, 0.0f);
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
        return capacity - available.load(std::memory_order_acquire);
    }

    // Get total capacity
    size_t GetCapacity() const {
        return capacity;
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
    size_t capacity;
    size_t readPos;
    size_t writePos;
    std::atomic<size_t> available;
    std::atomic<bool> overflowFlag{false};
    std::atomic<size_t> droppedSamples{0};   // Track samples dropped due to overflow
    std::atomic<size_t> retainedSamples{0};  // Track oldest samples trimmed to keep latest audio
    std::mutex mutex;
};
