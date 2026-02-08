#pragma once

/**
 * Unified Ring Buffer Template for CaptureEngine
 *
 * This template provides a type-safe, lock-free ring buffer for SPSC (Single Producer Single Consumer)
 * scenarios. It supports both fixed-size arrays (for shared memory) and dynamic storage.
 *
 * Features:
 * - Lock-free SPSC operation using std::atomic
 * - Configurable memory ordering (default: acquire/release for safety)
 * - Optional overwrite vs drop policies
 * - Cache-line padding to prevent false sharing
 * - Type-safe with compile-time size
 *
 * Usage:
 *   // Fixed size ring buffer for shared memory
 *   LockFreeRingBuffer<FrameSlot, 32> frameRing;
 *
 *   // Dynamic ring buffer for audio
 *   LockFreeRingBuffer<float> audioRing(4096);
 *
 * Note: For shared memory, use the fixed-size variant with std::atomic fields in the element type.
 */

#include <atomic>
#include <cstdint>
#include <new>
#include <optional>
#include <type_traits>
#include <vector>

namespace ce {

// Policy for handling full buffer
enum class RingBufferPolicy {
    DropNew,    // Drop new elements when full (default for capture)
    DropOld,    // Drop oldest elements when full
    Overwrite,  // Overwrite oldest (circular buffer style)
    Block       // Block producer until space available (not recommended for real-time)
};

// Memory ordering configuration
struct RingBufferOrdering {
    std::memory_order writeIndexLoad;
    std::memory_order writeIndexStore;
    std::memory_order readIndexLoad;
    std::memory_order readIndexStore;
    std::memory_order elementAccess;

    // Default: acquire/release for safe cross-thread synchronization
    static constexpr RingBufferOrdering AcquireRelease()
    {
        return {
            std::memory_order_acquire,  // writeIndexLoad
            std::memory_order_release,  // writeIndexStore
            std::memory_order_acquire,  // readIndexLoad
            std::memory_order_release,  // readIndexStore
            std::memory_order_relaxed   // elementAccess (protected by index fences)
        };
    }

    // Sequential consistency for debugging/testing
    static constexpr RingBufferOrdering Sequential()
    {
        return {std::memory_order_seq_cst, std::memory_order_seq_cst, std::memory_order_seq_cst,
                std::memory_order_seq_cst, std::memory_order_seq_cst};
    }

    // Relaxed for single-threaded or already-synchronized scenarios
    static constexpr RingBufferOrdering Relaxed()
    {
        return {std::memory_order_relaxed, std::memory_order_relaxed, std::memory_order_relaxed,
                std::memory_order_relaxed, std::memory_order_relaxed};
    }
};

// Default ordering
inline constexpr RingBufferOrdering DefaultOrdering = RingBufferOrdering::AcquireRelease();

// Base class for type erasure and common functionality
template <typename T>
class RingBufferBase {
public:
    virtual ~RingBufferBase() = default;

    // Core operations (to be implemented by derived)
    virtual bool Push(const T& item) = 0;
    virtual bool Pop(T& item) = 0;
    virtual bool Peek(T& item) const = 0;
    virtual bool Skip() = 0;
    virtual void Clear() = 0;

    // Status queries
    virtual size_t Size() const = 0;
    virtual size_t Capacity() const = 0;
    virtual bool Empty() const = 0;
    virtual bool Full() const = 0;
    virtual size_t Available() const = 0;

    // Statistics
    virtual uint64_t DroppedCount() const = 0;
    virtual void ResetDroppedCount() = 0;
};

// Lock-free ring buffer with fixed size (for shared memory)
template <typename T, size_t N>
class LockFreeRingBuffer : public RingBufferBase<T> {
    static_assert(N > 0, "Ring buffer size must be greater than 0");
    static_assert((N & (N - 1)) == 0, "Ring buffer size must be power of 2 for efficient modulo");

public:
    static constexpr size_t CapacityValue = N;
    static constexpr size_t IndexMask = N - 1;

    explicit LockFreeRingBuffer(RingBufferPolicy policy = RingBufferPolicy::DropNew,
                                RingBufferOrdering ordering = DefaultOrdering)
        : policy_(policy), ordering_(ordering)
    {
    }

    // Non-copyable, non-movable (contains atomics)
    LockFreeRingBuffer(const LockFreeRingBuffer&) = delete;
    LockFreeRingBuffer& operator=(const LockFreeRingBuffer&) = delete;
    LockFreeRingBuffer(LockFreeRingBuffer&&) = delete;
    LockFreeRingBuffer& operator=(LockFreeRingBuffer&&) = delete;

    // Push an item into the ring buffer
    // Returns true if successful, false if dropped
    bool Push(const T& item) override
    {
        uint32_t wIdx = writeIndex_.load(ordering_.writeIndexLoad);
        uint32_t rIdx = readIndex_.load(ordering_.readIndexLoad);

        if ((wIdx - rIdx) >= N) {
            // Buffer full
            droppedCount_.fetch_add(1, std::memory_order_relaxed);

            switch (policy_) {
                case RingBufferPolicy::DropNew:
                    return false;  // Drop the new item

                case RingBufferPolicy::DropOld:
                    // Advance read index to make room
                    readIndex_.store(rIdx + 1, ordering_.readIndexStore);
                    break;

                case RingBufferPolicy::Overwrite:
                    // Will overwrite oldest after increment
                    break;

                case RingBufferPolicy::Block:
                    // Blocking not supported in lock-free implementation
                    return false;
            }
        }

        // Write element
        buffer_[wIdx & IndexMask] = item;

        // Memory fence before publishing
        std::atomic_thread_fence(std::memory_order_release);

        // Publish write
        writeIndex_.store(wIdx + 1, ordering_.writeIndexStore);

        return true;
    }

    // Pop an item from the ring buffer
    // Returns true if successful, false if empty
    bool Pop(T& item) override
    {
        uint32_t rIdx = readIndex_.load(ordering_.readIndexLoad);
        uint32_t wIdx = writeIndex_.load(ordering_.writeIndexLoad);

        if (rIdx == wIdx) {
            return false;  // Empty
        }

        // Read element
        item = buffer_[rIdx & IndexMask];

        // Memory fence after reading
        std::atomic_thread_fence(std::memory_order_acquire);

        // Publish read
        readIndex_.store(rIdx + 1, ordering_.readIndexStore);

        return true;
    }

    // Peek at next item without removing
    // Returns true if successful, false if empty
    bool Peek(T& item) const override
    {
        uint32_t rIdx = readIndex_.load(ordering_.readIndexLoad);
        uint32_t wIdx = writeIndex_.load(ordering_.writeIndexLoad);

        if (rIdx == wIdx) {
            return false;  // Empty
        }

        item = buffer_[rIdx & IndexMask];
        return true;
    }

    // Skip next item
    // Returns true if successful, false if empty
    bool Skip() override
    {
        uint32_t rIdx = readIndex_.load(ordering_.readIndexLoad);
        uint32_t wIdx = writeIndex_.load(ordering_.writeIndexLoad);

        if (rIdx == wIdx) {
            return false;  // Empty
        }

        readIndex_.store(rIdx + 1, ordering_.readIndexStore);
        return true;
    }

    // Clear all items
    void Clear() override
    {
        // Simply reset indices - atomic operation ensures visibility
        readIndex_.store(writeIndex_.load(std::memory_order_relaxed), ordering_.readIndexStore);
    }

    // Get current size (number of elements)
    size_t Size() const override
    {
        uint32_t wIdx = writeIndex_.load(ordering_.writeIndexLoad);
        uint32_t rIdx = readIndex_.load(ordering_.readIndexLoad);
        return wIdx - rIdx;
    }

    // Get capacity
    size_t Capacity() const override { return N; }

    // Check if empty
    bool Empty() const override
    {
        return readIndex_.load(ordering_.readIndexLoad) == writeIndex_.load(ordering_.writeIndexLoad);
    }

    // Check if full
    bool Full() const override
    {
        uint32_t wIdx = writeIndex_.load(ordering_.writeIndexLoad);
        uint32_t rIdx = readIndex_.load(ordering_.readIndexLoad);
        return (wIdx - rIdx) >= N;
    }

    // Get available space
    size_t Available() const override { return N - Size(); }

    // Get dropped count
    uint64_t DroppedCount() const override { return droppedCount_.load(std::memory_order_relaxed); }

    // Reset dropped count
    void ResetDroppedCount() override { droppedCount_.store(0, std::memory_order_relaxed); }

    // Direct access to indices (for advanced use)
    uint32_t WriteIndex() const { return writeIndex_.load(ordering_.writeIndexLoad); }
    uint32_t ReadIndex() const { return readIndex_.load(ordering_.readIndexLoad); }
    void SetWriteIndex(uint32_t idx) { writeIndex_.store(idx, ordering_.writeIndexStore); }
    void SetReadIndex(uint32_t idx) { readIndex_.store(idx, ordering_.readIndexStore); }

    // Access buffer directly (use with caution)
    T& operator[](size_t index) { return buffer_[index & IndexMask]; }
    const T& operator[](size_t index) const { return buffer_[index & IndexMask]; }

protected:
    // Buffer storage
    alignas(alignof(T)) T buffer_[N];

    // Producer index - isolated on its own cache line
    alignas(64) std::atomic<uint32_t> writeIndex_{0};

    // Consumer index - isolated on its own cache line
    alignas(64) std::atomic<uint32_t> readIndex_{0};

    // Statistics - can share cache line
    std::atomic<uint64_t> droppedCount_{0};

    // Configuration
    RingBufferPolicy policy_;
    RingBufferOrdering ordering_;
};

// Dynamic ring buffer for non-shared memory scenarios
template <typename T>
class DynamicRingBuffer : public RingBufferBase<T> {
public:
    explicit DynamicRingBuffer(size_t capacity = 1024, RingBufferPolicy policy = RingBufferPolicy::DropNew,
                               RingBufferOrdering ordering = DefaultOrdering)
        : capacity_(capacity), policy_(policy), ordering_(ordering)
    {
        buffer_.resize(capacity);
    }

    // Non-copyable
    DynamicRingBuffer(const DynamicRingBuffer&) = delete;
    DynamicRingBuffer& operator=(const DynamicRingBuffer&) = delete;

    // Movable
    DynamicRingBuffer(DynamicRingBuffer&& other) noexcept
        : buffer_(std::move(other.buffer_)), capacity_(other.capacity_), writeIndex_(other.writeIndex_.load()),
          readIndex_(other.readIndex_.load()), droppedCount_(other.droppedCount_.load()), policy_(other.policy_),
          ordering_(other.ordering_)
    {
    }

    DynamicRingBuffer& operator=(DynamicRingBuffer&& other) noexcept
    {
        if (this != &other) {
            buffer_ = std::move(other.buffer_);
            capacity_ = other.capacity_;
            writeIndex_.store(other.writeIndex_.load());
            readIndex_.store(other.readIndex_.load());
            droppedCount_.store(other.droppedCount_.load());
            policy_ = other.policy_;
            ordering_ = other.ordering_;
        }
        return *this;
    }

    bool Push(const T& item) override
    {
        uint32_t wIdx = writeIndex_.load(ordering_.writeIndexLoad);
        uint32_t rIdx = readIndex_.load(ordering_.readIndexLoad);

        if ((wIdx - rIdx) >= capacity_) {
            droppedCount_.fetch_add(1, std::memory_order_relaxed);

            switch (policy_) {
                case RingBufferPolicy::DropNew:
                    return false;
                case RingBufferPolicy::DropOld:
                    readIndex_.store(rIdx + 1, ordering_.readIndexStore);
                    break;
                case RingBufferPolicy::Overwrite:
                    break;
                case RingBufferPolicy::Block:
                    return false;
            }
        }

        buffer_[wIdx % capacity_] = item;
        std::atomic_thread_fence(std::memory_order_release);
        writeIndex_.store(wIdx + 1, ordering_.writeIndexStore);

        return true;
    }

    bool Pop(T& item) override
    {
        uint32_t rIdx = readIndex_.load(ordering_.readIndexLoad);
        uint32_t wIdx = writeIndex_.load(ordering_.writeIndexLoad);

        if (rIdx == wIdx) {
            return false;
        }

        item = buffer_[rIdx % capacity_];
        std::atomic_thread_fence(std::memory_order_acquire);
        readIndex_.store(rIdx + 1, ordering_.readIndexStore);

        return true;
    }

    bool Peek(T& item) const override
    {
        uint32_t rIdx = readIndex_.load(ordering_.readIndexLoad);
        uint32_t wIdx = writeIndex_.load(ordering_.writeIndexLoad);

        if (rIdx == wIdx) {
            return false;
        }

        item = buffer_[rIdx % capacity_];
        return true;
    }

    bool Skip() override
    {
        uint32_t rIdx = readIndex_.load(ordering_.readIndexLoad);
        uint32_t wIdx = writeIndex_.load(ordering_.writeIndexLoad);

        if (rIdx == wIdx) {
            return false;
        }

        readIndex_.store(rIdx + 1, ordering_.readIndexStore);
        return true;
    }

    void Clear() override { readIndex_.store(writeIndex_.load(std::memory_order_relaxed), ordering_.readIndexStore); }

    size_t Size() const override
    {
        uint32_t wIdx = writeIndex_.load(ordering_.writeIndexLoad);
        uint32_t rIdx = readIndex_.load(ordering_.readIndexLoad);
        return wIdx - rIdx;
    }

    size_t Capacity() const override { return capacity_; }

    bool Empty() const override
    {
        return readIndex_.load(ordering_.readIndexLoad) == writeIndex_.load(ordering_.writeIndexLoad);
    }

    bool Full() const override
    {
        uint32_t wIdx = writeIndex_.load(ordering_.writeIndexLoad);
        uint32_t rIdx = readIndex_.load(ordering_.readIndexLoad);
        return (wIdx - rIdx) >= capacity_;
    }

    size_t Available() const override { return capacity_ - Size(); }

    uint64_t DroppedCount() const override { return droppedCount_.load(std::memory_order_relaxed); }

    void ResetDroppedCount() override { droppedCount_.store(0, std::memory_order_relaxed); }

    // Resize the buffer (clears existing data)
    void Resize(size_t newCapacity)
    {
        buffer_.resize(newCapacity);
        capacity_ = newCapacity;
        Clear();
    }

private:
    std::vector<T> buffer_;
    size_t capacity_;

    alignas(64) std::atomic<uint32_t> writeIndex_{0};
    alignas(64) std::atomic<uint32_t> readIndex_{0};
    std::atomic<uint64_t> droppedCount_{0};

    RingBufferPolicy policy_;
    RingBufferOrdering ordering_;
};

// Type alias for common use cases
template <size_t N>
using FrameRingBufferUnified = LockFreeRingBuffer<struct FrameSlot, N>;

using AudioRingBufferUnified = DynamicRingBuffer<float>;

template <typename T, size_t N>
using PendingFrameRingBuffer = LockFreeRingBuffer<T, N>;

}  // namespace ce

// Legacy compatibility - allow use in C-style shared memory
// This macro creates a ring buffer with proper alignment for shared memory
#define CE_DECLARE_RING_BUFFER(type, name, size) alignas(64) ce::LockFreeRingBuffer<type, size> name

// For use in shared memory layouts
#define CE_RING_BUFFER_SIZE_CHECK(buf, expected_size) \
    static_assert(sizeof(buf) == expected_size, "Ring buffer size mismatch")
