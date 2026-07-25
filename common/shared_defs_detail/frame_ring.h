#pragma once

#include <intrin.h>  // for _mm_pause
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>  // for memcpy in seqlock helpers
#include <type_traits>

#include "../build_identity.h"

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

#include "capture_state.h"

// Frame slots, the frame ring buffer, and the shared staging buffer.

#pragma pack(push, 8)

struct alignas(8) FrameSlot {
    uint64_t fenceValue;             // GPU fence value for synchronization
    int64_t timestamp;               // QPC timestamp (ticks, not ms - use QPCToMs for conversion)
    uint32_t frameIndex;             // Sequential frame number from hook
    int32_t textureIndex;            // Index of shared texture (0..SHARED_TEXTURE_SLOT_COUNT-1)
    uint32_t sourcePid;              // Source process ID (required for OpenProcess/DuplicateHandle)
    std::atomic<uint32_t> valid{0};  // 1 if slot has unread data, 0 if empty/consumed
    uint32_t padding;                // Explicit padding to reach 32 bytes (8+8+4+4+4+4=32)
};

// Ring buffer for frame metadata (lock-free SPSC)
// This struct lives inside SharedMemoryLayout (cross-process shared memory) and
// therefore has a fixed binary layout. It cannot use LockFreeRingBuffer<T> from
// ring_buffer.h, which is a heap-allocated in-process template. Both serve SPSC
// use cases but have fundamentally different ownership and layout requirements.
// Uses std::atomic for proper memory ordering across threads/processes.
// Cache line padding prevents false sharing between producer/consumer indices.
struct FrameRingBuffer {
    FrameSlot slots[FRAME_RING_SIZE]{};  // Default-initialize all slots

    // Producer index - isolated on its own cache line
    alignas(64) std::atomic<uint32_t> writeIndex{0};  // Next slot to write (hook/producer)

    // Lease acknowledgement index - isolated on its own cache line. This can
    // intentionally trail ingestion while queued frames still reference the
    // producer textures.
    alignas(64) std::atomic<uint32_t> readIndex{0};

    // Metadata ingestion index - the next slot the media ingest thread has not
    // observed yet. Producers use this only for empty->nonempty event
    // coalescing; texture/ring reuse remains governed by readIndex.
    alignas(64) std::atomic<uint32_t> ingestIndex{0};

    // Dropped frame counter - can share with readIndex (both consumer-side)
    std::atomic<uint32_t> droppedFrames{0};  // Frames dropped due to buffer full

    // Helper methods for safe atomic access
    uint32_t load_write_index_acquire() const {
        return writeIndex.load(std::memory_order_acquire);
    }
    uint32_t load_read_index_acquire() const {
        return readIndex.load(std::memory_order_acquire);
    }
    uint32_t load_ingest_index_acquire() const {
        return ingestIndex.load(std::memory_order_acquire);
    }
    uint32_t load_write_index_relaxed() const {
        return writeIndex.load(std::memory_order_relaxed);
    }
    uint32_t load_read_index_relaxed() const {
        return readIndex.load(std::memory_order_relaxed);
    }

    void store_write_index_release(uint32_t idx) {
        writeIndex.store(idx, std::memory_order_release);
    }
    void store_read_index_release(uint32_t idx) {
        readIndex.store(idx, std::memory_order_release);
    }
    void store_ingest_index_release(uint32_t idx) {
        ingestIndex.store(idx, std::memory_order_release);
    }
};

// D3D9 Shmem Fallback Buffer
// Used when shared handles are not available (e.g. legacy D3D9 on Win11)
// Moving to separate shared memory to reduce 32-bit address space consumption
//
// OPTIMIZATION: For 32-bit builds, MAX dimensions are reduced to 2560x1440
// (25MB total vs 66MB) to conserve limited address space. Full 4K support
// remains available in 64-bit builds.
struct ShmemBuffer {
    static const int SLOT_COUNT = 2;

    // Metadata at the beginning to ensure consistent ABI between 32-bit and 64-bit
    std::atomic<int> writeSlot{0};
    std::atomic<bool> slotReady[SLOT_COUNT];
    uint32_t validWidth{0};
    uint32_t validHeight{0};
    uint32_t pitch{0};
    uint32_t max_width{0};
    uint32_t max_height{0};
    uint32_t slot_size{0};  // Size of one slot in bytes

    ShmemBuffer() {
        writeSlot.store(0);
        for (int i = 0; i < SLOT_COUNT; ++i) {
            slotReady[i].store(false);
        }
    }

    // Helper methods
    void mark_ready(int slot) {
        if (slot >= 0 && slot < SLOT_COUNT) {
            slotReady[slot].store(true, std::memory_order_release);
        }
    }

    bool check_ready(int slot) const {
        if (slot >= 0 && slot < SLOT_COUNT) {
            return slotReady[slot].load(std::memory_order_acquire);
        }
        return false;
    }

    void reset_ready(int slot) {
        if (slot >= 0 && slot < SLOT_COUNT) {
            slotReady[slot].store(false, std::memory_order_relaxed);
        }
    }

    // Data follows immediately after this struct
    uint8_t* GetData(int slot) {
        if (slot < 0 || slot >= SLOT_COUNT)
            return nullptr;
        // Align to 16 bytes for SIMD operations if needed
        size_t headerSize = (sizeof(ShmemBuffer) + 15) & ~15;
        uint8_t* base = reinterpret_cast<uint8_t*>(this) + headerSize;
        return base + (slot * slot_size);
    }

    // Calculate actual size needed for given resolution
    static constexpr size_t CalculateSize(uint32_t width, uint32_t height) {
        size_t headerSize = (sizeof(ShmemBuffer) + 15) & ~15;
        return headerSize + (SLOT_COUNT * width * height * 4);
    }
};

enum SharedSystemMetricsValidity : uint32_t {
    SYSTEM_METRIC_GPU_USAGE_VALID = 1u << 0,
    SYSTEM_METRIC_VRAM_USAGE_VALID = 1u << 1,
    SYSTEM_METRIC_VRAM_TOTAL_VALID = 1u << 2,
};

enum SharedSystemMetricsAdapterSource : uint32_t {
    SYSTEM_METRICS_ADAPTER_UNAVAILABLE = 0,
    SYSTEM_METRICS_ADAPTER_HOOK_LUID = 1,
    SYSTEM_METRICS_ADAPTER_PROCESS_ENGINE = 2,
    SYSTEM_METRICS_ADAPTER_RETAINED_PROCESS_ENGINE = 3,
    SYSTEM_METRICS_ADAPTER_CAPTURE_DEVICE = 4,
};

// Main Shared Memory Structure

#pragma pack(pop)
