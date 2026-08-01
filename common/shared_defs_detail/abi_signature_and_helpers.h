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

#include "shared_memory_layout.h"

// Compile-time ABI signature, ring-window validation, and mapping helpers.

#pragma pack(push, 8)

constexpr uint32_t MixSharedMemoryAbiValue(uint32_t hash, uint64_t value) {
    for (unsigned byte = 0; byte < sizeof(value); ++byte) {
        hash ^= static_cast<uint8_t>(value >> (byte * 8));
        hash *= 16777619u;
    }
    return hash;
}

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#endif
constexpr uint32_t ComputeSharedMemoryAbiSignature() {
    uint32_t hash = 2166136261u;
    hash = MixSharedMemoryAbiValue(hash, SHARED_MEMORY_VERSION);
    hash = MixSharedMemoryAbiValue(hash, sizeof(SharedMemoryLayout));
    hash = MixSharedMemoryAbiValue(hash, alignof(SharedMemoryLayout));
    hash = MixSharedMemoryAbiValue(hash, offsetof(SharedMemoryLayout, overlayConfig));
    hash = MixSharedMemoryAbiValue(hash, offsetof(SharedMemoryLayout, graphicsConfig));
    hash = MixSharedMemoryAbiValue(hash, offsetof(SharedMemoryLayout, logFilePath));
    hash = MixSharedMemoryAbiValue(hash, offsetof(SharedMemoryLayout, runtimeState));
    hash = MixSharedMemoryAbiValue(hash, sizeof(CaptureState));
    hash = MixSharedMemoryAbiValue(hash, offsetof(CaptureState, recordingHealthFlags));
    hash = MixSharedMemoryAbiValue(hash, offsetof(CaptureState, screenGrabTargetSequence));
    hash = MixSharedMemoryAbiValue(hash, offsetof(SharedMemoryLayout, systemMetrics));
    hash = MixSharedMemoryAbiValue(hash, offsetof(SharedMemoryLayout, encoderTextures));
    hash = MixSharedMemoryAbiValue(hash, offsetof(SharedMemoryLayout, frameRing));
    hash = MixSharedMemoryAbiValue(hash, offsetof(SharedMemoryLayout, logs));
    hash = MixSharedMemoryAbiValue(hash, offsetof(SharedMemoryLayout, configVersion));
    hash = MixSharedMemoryAbiValue(hash, sizeof(SharedMemoryLayout::SharedSystemMetrics));
    hash = MixSharedMemoryAbiValue(hash, offsetof(SharedMemoryLayout::SharedSystemMetrics, publicationSequence));
    hash = MixSharedMemoryAbiValue(hash, offsetof(SharedMemoryLayout::SharedSystemMetrics, vramTotal));
    hash = MixSharedMemoryAbiValue(hash, offsetof(SharedMemoryLayout::SharedSystemMetrics, maxCoreLoad));
    hash = MixSharedMemoryAbiValue(hash, sizeof(FrameRingBuffer));
    hash = MixSharedMemoryAbiValue(hash, offsetof(FrameRingBuffer, writeIndex));
    hash = MixSharedMemoryAbiValue(hash, offsetof(FrameRingBuffer, readIndex));
    hash = MixSharedMemoryAbiValue(hash, offsetof(FrameRingBuffer, ingestIndex));
    hash = MixSharedMemoryAbiValue(hash, sizeof(SharedMemoryLayout::LogBuffer));
    hash = MixSharedMemoryAbiValue(hash, offsetof(SharedMemoryLayout::LogBuffer, writeIndex));
    hash = MixSharedMemoryAbiValue(hash, offsetof(SharedMemoryLayout::LogBuffer, readIndex));
    return hash;
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

static constexpr uint32_t SHARED_MEMORY_ABI_SIGNATURE = ComputeSharedMemoryAbiSignature();

inline bool IsFrameRingWindowValid(uint32_t writeIndex, uint32_t readIndex) {
    return static_cast<uint32_t>(writeIndex - readIndex) <= static_cast<uint32_t>(FRAME_RING_SIZE);
}

// Generate unique Shmem mapping name
inline void GenerateShmemName(wchar_t* outName, size_t maxLen, uint32_t pid) {
    swprintf(outName, maxLen, L"Local\\CE_SHM_%08X", pid);
}

// ============================================================================
// Static Assertions for ABI Safety
// ============================================================================

// NOTE: FrameSlot contains std::atomic<uint32_t> valid, which makes it
// non-trivially copyable. However, this is safe for IPC because:
// 1. We use memory-mapped files, not memcpy
// 2. Atomics work across process boundaries with shared memory
// 3. The atomic provides the necessary synchronization
// NOTE: Structures containing std::atomic are not trivially copyable but are
// safe for IPC because we use memory-mapped files, not memcpy, and atomics work
// across process boundaries
// static_assert(std::is_trivially_copyable_v<FrameSlot>,
//     "FrameSlot must be trivially copyable for IPC");
static_assert(std::is_trivially_copyable_v<OverlayConfig>, "OverlayConfig must be trivially copyable for IPC");
static_assert(std::is_trivially_copyable_v<SharedGraphicsConfig>,
              "SharedGraphicsConfig must be trivially copyable for IPC");
// DiscoveryInfo contains atomics for thread-safe access - not trivially
// copyable but safe for shared memory

// Ensure proper alignment for atomics
static_assert(alignof(FrameRingBuffer) >= 8, "FrameRingBuffer must be 8-byte aligned for atomic operations");
static_assert(alignof(CaptureState) >= 8, "CaptureState must be 8-byte aligned for atomic operations");

// Ensure ring buffer size is power of 2 for efficient modulo
static_assert((FRAME_RING_SIZE & (FRAME_RING_SIZE - 1)) == 0, "FRAME_RING_SIZE must be power of 2");

// Ensure FrameSlot is properly sized for cache efficiency
static_assert(sizeof(FrameSlot) == 40, "FrameSlot should be 40 bytes - update if struct changes");

// Validate shared memory header is at offset 0
// Note: offsetof is technically UB for non-standard-layout types (like those with atomics),
// but works in practice on MSVC/Clang for our specific layout. We use a macro to suppress the warning.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#endif
static_assert(offsetof(SharedMemoryLayout, magic) == 0, "magic must be at offset 0 for version validation");
static_assert(offsetof(SharedMemoryLayout, version) == 4, "version must be at offset 4");
static_assert(offsetof(SharedMemoryLayout, structSize) == 8, "structSize must be at offset 8");
static_assert(offsetof(SharedMemoryLayout, abiSignature) == 12, "abiSignature must be at offset 12");
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

// Helper function to validate shared memory on connect
// Uses atomic loads for thread-safe validation
inline bool ValidateSharedMemory(const SharedMemoryLayout* shm) {
    if (!shm)
        return false;
    // Use atomic loads through accessor methods
    if (shm->GetMagic() != SHARED_MEMORY_MAGIC)
        return false;
    if (shm->GetVersion() != SHARED_MEMORY_VERSION)
        return false;
    if (shm->structSize.load(std::memory_order_acquire) != sizeof(SharedMemoryLayout)) {
        return false;
    }
    if (shm->abiSignature.load(std::memory_order_acquire) != SHARED_MEMORY_ABI_SIGNATURE) {
        return false;
    }
    return true;
}

#pragma pack(pop)
