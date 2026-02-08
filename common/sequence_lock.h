#pragma once

/**
 * Sequence Lock for Lock-Free Config Reloads
 * 
 * A sequence lock allows readers to access shared data without blocking writers.
 * Writers increment an odd sequence number before writing and even after,
 * allowing readers to detect concurrent modifications and retry.
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
 *   gfxLock.Read(localCopy, [](const GraphicsConfig& src, GraphicsConfig& dest) {
 *       dest = src;
 *   });
 */

#include <atomic>
#include <cstdint>
#include <thread>

namespace ce {

template<typename T>
class SequenceLock {
public:
    static_assert(sizeof(T) <= 1024, "SequenceLock data should fit in cache line for efficiency");
    
    SequenceLock() : sequence_(0) {
        // Initialize data to zero
        memset(&data_, 0, sizeof(T));
    }
    
    // Non-copyable, non-movable
    SequenceLock(const SequenceLock&) = delete;
    SequenceLock& operator=(const SequenceLock&) = delete;
    
    /**
     * Read data safely. If a write occurs during the read, it will retry.
     * 
     * @param out Output buffer to copy data into
     * @param copyFunc Function to perform the copy: copyFunc(const T& src, T& dest)
     * @return true if successful, false if too many retries
     */
    template<typename CopyFunc>
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
     * @param copyFunc Function to perform the copy: copyFunc(T& dest, const T& src)
     */
    template<typename CopyFunc>
    void Write(const T& source, CopyFunc copyFunc) {
        // Increment to odd (lock)
        uint32_t seq = sequence_.fetch_add(1, std::memory_order_acq_rel) + 1;
        
        // Ensure odd (in case of overflow)
        if ((seq & 1) == 0) {
            sequence_.fetch_add(1, std::memory_order_acq_rel);
            ++seq;
        }
        
        // Write data
        copyFunc(data_, source);
        
        // Increment to even (unlock)
        sequence_.store(seq + 1, std::memory_order_release);
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
template<typename T>
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
        uint32_t currentVersion = lock_.GetSequence();
        
        if (currentVersion == lastVersion || lock_.IsWriting()) {
            return false;  // No change or write in progress
        }
        
        if (lock_.Read(out)) {
            lastVersion = currentVersion;
            return true;
        }
        
        return false;  // Read failed (too many retries)
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

} // namespace ce

// C-compatible wrapper for shared memory usage
extern "C" {
    // These functions can be called from C code
    typedef struct {
        uint32_t sequence;
        char data[1024];  // Flexible array for config data
    } SequenceLockHeader;
    
    inline uint32_t SequenceLock_ReadBegin(const SequenceLockHeader* header) {
        return header->sequence;
    }
    
    inline bool SequenceLock_ReadRetry(const SequenceLockHeader* header, uint32_t startSeq) {
        uint32_t current = header->sequence;
        // Retry if sequence changed or is odd (write in progress)
        return (current != startSeq) || (current & 1);
    }
}
