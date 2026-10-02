#pragma once

/**
 * Sequence Lock for Lock-Free Config Reloads
 *
 * A sequence lock allows readers to access shared data without blocking
 * writers. Writers increment an odd sequence number before writing and even
 * after, allowing readers to detect concurrent modifications and retry.
 *
 * Usage:
 *   SequenceLock<GraphicsConfig> gfxLock;
 *
 *   // Writer (host process)
 *   gfxLock.Write(config, [](GraphicsConfig& dest, const GraphicsConfig& src) {
 *       dest = src;
 *   });
 *
 *   // Reader (hook)
 *   GraphicsConfig localCopy;
 *   gfxLock.Read(localCopy, [](const GraphicsConfig& src, GraphicsConfig& dest)
 * { dest = src;
 *   });
 */

#include <atomic>
#include <cstdint>
#include <intrin.h>  // for _mm_pause
#include <thread>
#include <type_traits>

namespace ce {

template <typename T>
class SequenceLock {
public:
    static_assert(sizeof(T) <= 1024, "SequenceLock data should fit in cache line for efficiency");
    static_assert(std::is_trivially_copyable_v<T>, "SequenceLock requires trivially copyable data");

    SequenceLock() : sequence_(0), data_{} {}

    // Non-copyable, non-movable
    SequenceLock(const SequenceLock&) = delete;
    SequenceLock& operator=(const SequenceLock&) = delete;

    /**
     * Read data safely. If a write occurs during the read, it will retry.
     *
     * @param out Output buffer to copy data into
     * @param copyFunc Function to perform the copy: copyFunc(const T& src, T&
     * dest)
     * @return true if successful, false if too many retries
     */
    template <typename CopyFunc>
    bool Read(T& out, CopyFunc copyFunc) const {
        constexpr int MAX_RETRIES = 100;

        for (int retry = 0; retry < MAX_RETRIES; ++retry) {
            uint32_t seqBefore = sequence_.load(std::memory_order_acquire);

            // If odd, writer is active - spin briefly
            while (seqBefore & 1) {
                std::this_thread::yield();
                seqBefore = sequence_.load(std::memory_order_acquire);
            }

            // Copy data
            copyFunc(data_, out);

            // Check sequence after read
            uint32_t seqAfter = sequence_.load(std::memory_order_acquire);

            // If sequence hasn't changed, we got consistent data
            if (seqBefore == seqAfter) {
                return true;
            }

            // Otherwise retry
        }

        return false;  // Too many retries
    }

    /**
     * Read data safely with default copy (uses operator=)
     */
    bool Read(T& out) const {
        return Read(out, [](const T& src, T& dest) { dest = src; });
    }

    /**
     * Write data safely.
     *
     * @param source Source data to copy from
     * @param copyFunc Function to perform the copy: copyFunc(T& dest, const T&
     * src)
     */
    template <typename CopyFunc>
    void Write(const T& source, CopyFunc copyFunc) {
        // Use CAS loop to avoid the wrap-around race window where two fetch_add
        // calls leave the sequence momentarily even while the writer is still active.
        uint32_t seq = sequence_.load(std::memory_order_relaxed);
        uint32_t desired;
        do {
            desired = seq + 1;
            if ((desired & 1) == 0) {
                desired++;  // Skip even values to stay locked (odd)
            }
        } while (!sequence_.compare_exchange_weak(seq, desired, std::memory_order_acq_rel, std::memory_order_relaxed));

        // Write data
        copyFunc(data_, source);

        // Increment to even (unlock)
        sequence_.store(desired + 1, std::memory_order_release);
    }

    /**
     * Write data safely with default copy (uses operator=)
     */
    void Write(const T& source) {
        Write(source, [](T& dest, const T& src) { dest = src; });
    }

    /**
     * Get current sequence number (for external version tracking)
     */
    uint32_t GetSequence() const {
        return sequence_.load(std::memory_order_acquire);
    }

    /**
     * Check if a write is in progress
     */
    bool IsWriting() const {
        return (sequence_.load(std::memory_order_acquire) & 1) != 0;
    }

private:
    // Sequence number: even = readable, odd = writing
    alignas(64) std::atomic<uint32_t> sequence_;

    // The actual data
    alignas(alignof(T)) T data_;
};

/**
 * VersionedConfig - Combines sequence lock with version tracking
 *
 * Optimized for the common case where config rarely changes.
 * Readers track the last version they read and only copy when it changes.
 */
template <typename T>
class VersionedConfig {
public:
    /**
     * Read config only if version changed since last read
     *
     * @param out Output buffer
     * @param lastVersion Input/Output: last known version (updated on change)
     * @return true if config was updated
     */
    bool ReadIfChanged(T& out, uint32_t& lastVersion) const {
        constexpr int MAX_RETRIES = 100;

        for (int retry = 0; retry < MAX_RETRIES; ++retry) {
            uint32_t seqBefore = lock_.GetSequence();

            if (seqBefore == lastVersion) {
                return false;  // No change
            }

            if (seqBefore & 1) {
                std::this_thread::yield();
                continue;
            }

            if (!lock_.Read(out)) {
                return false;
            }

            uint32_t seqAfter = lock_.GetSequence();
            if (seqBefore == seqAfter) {
                lastVersion = seqAfter;
                return true;
            }
        }

        return false;  // Read failed or kept racing with writers
    }

    /**
     * Force a read regardless of version
     */
    bool Read(T& out) const {
        return lock_.Read(out);
    }

    /**
     * Write new config
     */
    void Write(const T& source) {
        lock_.Write(source);
    }

    /**
     * Get current version
     */
    uint32_t GetVersion() const {
        return lock_.GetSequence();
    }

private:
    SequenceLock<T> lock_;
};

}  // namespace ce

// C-compatible wrapper for shared memory usage
// std::atomic<uint32_t> has the same size/alignment as uint32_t on all supported
// platforms, making the ABI compatible with C consumers.
extern "C" {
typedef struct {
    std::atomic<uint32_t> sequence;
    char data[1024];
} SequenceLockHeader;

inline uint32_t SequenceLock_ReadBegin(const SequenceLockHeader* header) {
    uint32_t seq;
    while ((seq = header->sequence.load(std::memory_order_acquire)) & 1u) {
        _mm_pause();
    }
    return seq;
}

inline bool SequenceLock_ReadRetry(const SequenceLockHeader* header, uint32_t startSeq) {
    uint32_t current = header->sequence.load(std::memory_order_acquire);
    return (current != startSeq) || (current & 1);
}
}
