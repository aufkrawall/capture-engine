#pragma once

// Debug Invariant Checking for CaptureEngine
// These functions validate internal state and catch corruption early.
// All checks are gated by _DEBUG or g_DebugLoggingEnabled to avoid production
// overhead.

#include "shared_defs.h"
#include "validation.h"

namespace ce {

// Check ring buffer invariants
inline void CheckRingBufferInvariants([[maybe_unused]] const FrameRingBuffer& rb) {
#ifdef _DEBUG
    if (!g_DebugLoggingEnabled)
        return;

    uint32_t write = rb.writeIndex.load(std::memory_order_relaxed);
    uint32_t read = rb.readIndex.load(std::memory_order_relaxed);

    // Write should never be more than RING_SIZE ahead of read
    uint32_t diff = write - read;  // Handles wraparound correctly for unsigned
    CE_ASSERT_MSG(diff <= FRAME_RING_SIZE, "ring buffer overflow detected");

    // Indices after modulo should be within bounds
    CE_ASSERT_MSG((write % FRAME_RING_SIZE) < (uint32_t)FRAME_RING_SIZE, "write index out of bounds");
    CE_ASSERT_MSG((read % FRAME_RING_SIZE) < (uint32_t)FRAME_RING_SIZE, "read index out of bounds");
#endif
}

// Check shared memory invariants
inline void CheckSharedMemoryInvariants([[maybe_unused]] const SharedMemoryLayout* shm) {
#ifdef _DEBUG
    if (!g_DebugLoggingEnabled || !shm)
        return;

    // Magic number check
    CE_ASSERT_MSG(shm->magic == SHARED_MEMORY_MAGIC, "shared memory magic mismatch");

    // Version check
    CE_ASSERT_MSG(shm->version >= SHARED_MEMORY_MIN_VERSION, "shared memory version too old");
    CE_ASSERT_MSG(shm->version <= SHARED_MEMORY_VERSION, "shared memory version too new");

    // Ring buffer check
    CheckRingBufferInvariants(shm->frameRing);
#endif
}

// Check capture state invariants
inline void CheckCaptureStateInvariants([[maybe_unused]] const CaptureState& state) {
#ifdef _DEBUG
    if (!g_DebugLoggingEnabled)
        return;

    // If recording, start time should be set
    if (state.isRecording.load(std::memory_order_relaxed)) {
        CE_ASSERT_MSG(state.recordingStartTime.load(std::memory_order_relaxed) > 0, "recording but start time not set");
    }
#endif
}

// Periodic health check - call every N frames in main loop
inline void PeriodicHealthCheck([[maybe_unused]] const SharedMemoryLayout* shm, [[maybe_unused]] uint64_t frameNumber) {
#ifdef _DEBUG
    if (!g_DebugLoggingEnabled)
        return;

    // Only check every 1000 frames to minimize overhead
    if ((frameNumber % 1000) != 0)
        return;

    if (shm) {
        CheckSharedMemoryInvariants(shm);
        CheckCaptureStateInvariants(shm->runtimeState);
    }

    CE_LOG_DEBUG("Health", "check passed frame=%llu", frameNumber);
#endif
}

// Validate texture index is within bounds
inline bool ValidateTextureIndex(int32_t idx, const char* context = "texture") {
    if (idx < 0 || idx >= 8) {
        CE_LOG_ERROR("Bounds", "%s index %d out of range [0,8)", context, idx);
        return false;
    }
    return true;
}

// Validate frame ring index
inline bool ValidateFrameRingIndex(uint32_t idx) {
    if (idx >= (uint32_t)FRAME_RING_SIZE) {
        CE_LOG_ERROR("Bounds", "frame ring index %u >= %d", idx, FRAME_RING_SIZE);
        return false;
    }
    return true;
}

}  // namespace ce
