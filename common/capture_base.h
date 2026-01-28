#pragma once

// Shared capture infrastructure for both hook (inject) and captureengine (WGC)
// This file contains the common ring buffer, async thread, and IPC signaling patterns.

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <thread>
#include "shared_defs.h"

// Shared capture constants
static constexpr int CAPTURE_TEXTURE_COUNT = 8;  // Ring buffer size for textures
static constexpr int CAPTURE_RING_SIZE = 8;      // Pending frame ring size

// Pending frame metadata for async capture thread
struct PendingCaptureFrame {
    int64_t timestampQPC;           // QPC timestamp when frame was submitted
    uint64_t fenceValue;            // GPU fence value for sync
    uint32_t backBufferIndex;       // Back buffer index in swapchain (inject) or texture ring index (WGC)
    uint64_t completionFenceValue;  // Value to signal when capture complete
    void* apiData;                  // API-specific data (swapchain pointer, texture pointer, etc.)
    uint64_t syncObject;            // Generic sync object handle (e.g. Vulkan Semaphore)
};

// Shared capture state - base class for all capture implementations
// Used by: DX11Capture, DX12 CaptureContext, WGCCaptureEngine
class CaptureBase {
public:
    // Shared texture handles (exported to other processes)
    HANDLE sharedTextureHandles[CAPTURE_TEXTURE_COUNT] = {};
    HANDLE sharedFenceHandle = NULL;

    // Capture dimensions and format
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;  // DXGI_FORMAT cast to uint32
    int32_t luidLow = 0;
    int32_t luidHigh = 0;

    // Ring buffer state
    int writeIndex = 0;
    uint64_t fenceValue = 0;

    // Initialization state
    bool initialized = false;

    // Async capture thread ring buffer (lock-free SPSC)
    PendingCaptureFrame pendingRing[CAPTURE_RING_SIZE];
    std::atomic<uint32_t> pendingWriteIdx{0};
    std::atomic<uint32_t> pendingReadIdx{0};

    // Async capture thread
    std::thread captureThread;
    std::atomic<bool> captureThreadRunning{false};
    std::atomic<bool> captureThreadShutdown{false};
    HANDLE captureEvent = NULL;

    // Completion fence for back-pressure
    uint64_t completionFenceValue = 0;
    uint64_t pendingCaptureWaitValue = 0;

    // Recording session tracking
    std::atomic<int> recordingSessionID{0};

    // Frame statistics
    std::atomic<uint32_t> droppedFrames{0};  // Count of frames dropped due to ring full

    virtual ~CaptureBase() { StopCaptureThread(); }

    // Check if ring buffer has space
    bool HasPendingSpace() const
    {
        uint32_t wIdx = pendingWriteIdx.load(std::memory_order_acquire);
        return (wIdx - pendingReadIdx.load(std::memory_order_relaxed)) < CAPTURE_RING_SIZE;
    }

    // Enqueue a frame for async capture
    bool EnqueueFrame(int64_t timestampQPC, uint64_t gpuFenceValue, uint32_t backBufferIndex, void* apiData,
                      uint64_t syncObject = 0)
    {
        uint32_t wIdx = pendingWriteIdx.load(std::memory_order_acquire);
        if (wIdx - pendingReadIdx.load(std::memory_order_relaxed) >= CAPTURE_RING_SIZE) {
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
            return false;  // Ring full, frame dropped
        }

        PendingCaptureFrame& f = pendingRing[wIdx % CAPTURE_RING_SIZE];
        f.timestampQPC = timestampQPC;
        f.fenceValue = gpuFenceValue;
        f.backBufferIndex = backBufferIndex;
        f.apiData = apiData;
        f.syncObject = syncObject;

        completionFenceValue++;
        f.completionFenceValue = completionFenceValue;
        pendingCaptureWaitValue = completionFenceValue;

        pendingWriteIdx.store(wIdx + 1, std::memory_order_release);
        if (captureEvent) SetEvent(captureEvent);

        return true;
    }

    // Cleanup shared handles to prevent resource leaks
    void CleanupSharedHandles()
    {
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            if (sharedTextureHandles[i]) {
                CloseHandle(sharedTextureHandles[i]);
                sharedTextureHandles[i] = NULL;
            }
        }
        if (sharedFenceHandle) {
            CloseHandle(sharedFenceHandle);
            sharedFenceHandle = NULL;
        }
    }

    // Publish shared handles to IPC shared memory
    // Note: sharedMem pointer is passed rather than using global g_IPC
    // to support both hook (g_IPC->GetSharedMem()) and WGC (g_pSharedMem)
    void PublishToSharedMemory(SharedMemoryLayout* sharedMem)
    {
        if (!sharedMem) return;

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            sharedMem->sharedHandles[i] = (uint64_t)sharedTextureHandles[i];
        }
        sharedMem->fenceShareHandle = (uint64_t)sharedFenceHandle;
        sharedMem->width = width;
        sharedMem->height = height;
        sharedMem->format = format;
        sharedMem->luidLowPart = luidLow;
        sharedMem->luidHighPart = luidHigh;
        sharedMem->sourcePid = GetCurrentProcessId();
    }

    // Advance write index (called after copy completes)
    int AdvanceWriteIndex()
    {
        int idx = writeIndex;
        writeIndex = (writeIndex + 1) % CAPTURE_TEXTURE_COUNT;
        return idx;
    }

    // Signal frame ready to IPC ring buffer
    void SignalFrameReady(SharedMemoryLayout* sharedMem, int textureIndex, int64_t timestamp, uint64_t gpuFenceValue)
    {
        if (!sharedMem) return;

        auto& ring = sharedMem->frameRing;
        uint32_t wIdx = ring.writeIndex.load(std::memory_order_relaxed);
        uint32_t rIdx = ring.readIndex.load(std::memory_order_acquire);
        if ((uint32_t)(wIdx - rIdx) >= (uint32_t)FRAME_RING_SIZE) {
            ring.droppedFrames.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto& slot = ring.slots[wIdx % FRAME_RING_SIZE];

        // Write frame metadata
        slot.timestamp = timestamp;
        slot.textureIndex = textureIndex;
        slot.fenceValue = gpuFenceValue;
        slot.frameIndex = wIdx;
        slot.sourcePid = GetCurrentProcessId();

        // Memory barrier: Ensure all above writes are visible before setting valid flag
        // This prevents the reader from seeing valid=1 with stale/uninitialized data
        std::atomic_thread_fence(std::memory_order_release);

        slot.valid.store(1, std::memory_order_release);

        // Publish write index with release semantics to synchronize with reader
        ring.writeIndex.store(wIdx + 1, std::memory_order_release);
    }

    // Start async capture thread
    template <typename ThreadFunc>
    void StartCaptureThread(ThreadFunc func)
    {
        if (captureThreadRunning) return;

        captureEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        captureThreadShutdown = false;
        captureThread = std::thread(func);
        // Do NOT detach. We want to join on shutdown.
    }

    // Stop async capture thread
    void StopCaptureThread()
    {
        captureThreadShutdown = true;
        if (captureEvent) SetEvent(captureEvent);

        if (captureThread.joinable()) {
            captureThread.join();
        }

        if (captureEvent) {
            CloseHandle(captureEvent);
            captureEvent = NULL;
        }

        captureThreadRunning = false;
    }

    // Reset for new recording session
    void ResetForNewRecording()
    {
        droppedFrames.store(0, std::memory_order_relaxed);
        pendingWriteIdx.store(0, std::memory_order_relaxed);
        pendingReadIdx.store(0, std::memory_order_relaxed);
        completionFenceValue = 0;
        pendingCaptureWaitValue = 0;
        recordingSessionID++;
    }

    // Virtual methods - implemented by each API/capture source
    virtual void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) = 0;
    virtual void Cleanup() = 0;
};
