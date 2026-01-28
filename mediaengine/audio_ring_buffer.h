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
 * - droppedSamples counter tracks total drops for gap compensation
 * - Downstream encoder should check GetAndClearDroppedSamples() and insert silence
 */
class AudioRingBuffer {
public:
    AudioRingBuffer(size_t capacitySamples)
        : capacity(capacitySamples), readPos(0), writePos(0), available(0), droppedSamples(0)
    {
        buffer.resize(capacity, 0.0f);
    }

    // Push new samples into the buffer.
    // Returns number of samples written.
    // If buffer full, drops new data and increments droppedSamples counter.
    size_t Write(const float* data, size_t count);

    // Pull samples from the buffer.
    // Returns number of samples read.
    // If not enough data, returns what is available (Reader must handle underflow/silence).
    size_t Read(float* outData, size_t count);

    // Peek only (for analysis)
    size_t Peek(float* outData, size_t count);

    // Skip samples (consume without copying)
    size_t Skip(size_t count);

    // Clear buffer (reset)
    void Clear();

    // Get available samples to read
    size_t GetAvailable() const { return available.load(); }

    // Get free space to write
    size_t GetFree() const { return capacity - available.load(); }

    // Get total capacity
    size_t GetCapacity() const { return capacity; }

    // Check overflow status (debug)
    bool HasOverflowed() const { return overflowFlag.load(); }
    void ClearOverflow() { overflowFlag.store(false); }

    // Get total dropped samples since last clear (for gap compensation)
    // Returns the count AND resets to zero atomically
    size_t GetAndClearDroppedSamples() { return droppedSamples.exchange(0); }

    // Get dropped samples without clearing (for diagnostics)
    size_t GetDroppedSamples() const { return droppedSamples.load(); }

private:
    std::vector<float> buffer;
    size_t capacity;
    size_t readPos;
    size_t writePos;
    std::atomic<size_t> available;
    std::atomic<bool> overflowFlag{false};
    std::atomic<size_t> droppedSamples{0};  // Track samples dropped due to overflow
    std::mutex mutex;
};
