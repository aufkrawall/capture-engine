#pragma once

// Shared capture infrastructure for both hook (inject) and captureengine (WGC)
// This file contains the common ring buffer, async thread, and IPC signaling
// patterns.

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <thread>
#include "shared_defs.h"

// Shared capture constants
static constexpr int CAPTURE_TEXTURE_COUNT = SHARED_TEXTURE_SLOT_COUNT;  // Ring buffer size for textures
static constexpr int CAPTURE_RING_SIZE = 8;                              // Pending frame ring size

// A queued inject frame keeps its producer texture alive until the media side
// has copied or deliberately dropped it. Producers must call this immediately
// before writing a ring texture; reusing a referenced slot would silently turn
// an older queued frame into newer visual content.
inline bool IsCaptureTextureSlotOutstanding(const SharedMemoryLayout* sharedMem, int32_t textureIndex) {
    if (!sharedMem || textureIndex < 0)
        return false;

    const FrameRingBuffer& ring = sharedMem->frameRing;
    const uint32_t readIndex = ring.readIndex.load(std::memory_order_acquire);
    const uint32_t writeIndex = ring.writeIndex.load(std::memory_order_acquire);
    uint32_t depth = writeIndex - readIndex;
    uint32_t scanBegin = readIndex;
    if (depth > FRAME_RING_SIZE) {
        // Corrupt/overrun metadata must not cause an unbounded scan. Inspect
        // every physical ring slot, including all content that can still exist.
        depth = FRAME_RING_SIZE;
        scanBegin = writeIndex - FRAME_RING_SIZE;
    }

    for (uint32_t offset = 0; offset < depth; ++offset) {
        const FrameSlot& slot = ring.slots[(scanBegin + offset) % FRAME_RING_SIZE];
        if (slot.valid.load(std::memory_order_acquire) != 0 && slot.textureIndex == textureIndex)
            return true;
    }
    return false;
}

// A resource generation must stay published until every frame that references
// it has been copied or dropped by the media process. FrameSlot deliberately
// stores only the texture index (the handles live in generation-wide fields),
// so replacing those fields while an old slot is valid would make the consumer
// open the new generation for old frame metadata.
inline bool HasOutstandingCaptureFrameLeases(const SharedMemoryLayout* sharedMem) {
    if (!sharedMem)
        return false;

    const FrameRingBuffer& ring = sharedMem->frameRing;
    const uint32_t readIndex = ring.readIndex.load(std::memory_order_acquire);
    const uint32_t writeIndex = ring.writeIndex.load(std::memory_order_acquire);
    uint32_t depth = writeIndex - readIndex;
    uint32_t scanBegin = readIndex;
    if (depth > FRAME_RING_SIZE) {
        depth = FRAME_RING_SIZE;
        scanBegin = writeIndex - FRAME_RING_SIZE;
    }

    for (uint32_t offset = 0; offset < depth; ++offset) {
        if (ring.slots[(scanBegin + offset) % FRAME_RING_SIZE].valid.load(std::memory_order_acquire) != 0)
            return true;
    }
    return false;
}

template <typename GpuReadyPredicate>
inline int32_t FindAvailableCaptureTextureSlotIf(const SharedMemoryLayout* sharedMem, int32_t firstTextureIndex,
                                                 uint32_t textureCount, GpuReadyPredicate&& gpuReady,
                                                 uint32_t* cpuBusyCount = nullptr, uint32_t* gpuBusyCount = nullptr) {
    if (textureCount == 0 || textureCount > static_cast<uint32_t>(SHARED_TEXTURE_SLOT_COUNT))
        return -1;
    uint32_t cpuBusy = 0;
    uint32_t gpuBusy = 0;
    const uint32_t first = static_cast<uint32_t>(firstTextureIndex < 0 ? 0 : firstTextureIndex) % textureCount;
    for (uint32_t offset = 0; offset < textureCount; ++offset) {
        const int32_t candidate = static_cast<int32_t>((first + offset) % textureCount);
        if (IsCaptureTextureSlotOutstanding(sharedMem, candidate)) {
            ++cpuBusy;
            continue;
        }
        if (!gpuReady(candidate)) {
            ++gpuBusy;
            continue;
        }
        if (cpuBusyCount)
            *cpuBusyCount = cpuBusy;
        if (gpuBusyCount)
            *gpuBusyCount = gpuBusy;
        return candidate;
    }
    if (cpuBusyCount)
        *cpuBusyCount = cpuBusy;
    if (gpuBusyCount)
        *gpuBusyCount = gpuBusy;
    return -1;
}

inline int32_t FindAvailableCaptureTextureSlot(const SharedMemoryLayout* sharedMem, int32_t firstTextureIndex,
                                               uint32_t textureCount = CAPTURE_TEXTURE_COUNT) {
    return FindAvailableCaptureTextureSlotIf(sharedMem, firstTextureIndex, textureCount, [](int32_t) { return true; });
}

// Result of asking a per-slot GPU copy query whether its slot can be reused.
enum class CaptureCopyQuerySlotState {
    Ready,          // The slot may be written again.
    GpuBusy,        // The issued copy has not completed yet.
    QueryUnusable,  // The query cannot answer (device removed, driver rejection).
};

// An EVENT copy query only answers GetData() meaningfully once it has been
// issued with End(). Both D3D10 and D3D11 return DXGI_ERROR_INVALID_CALL for a
// query that was created but never issued, so a selector that only accepts S_OK
// would classify every fresh slot as busy. Nothing would ever be written, End()
// would therefore never run, and capture would wedge with zero frames and no
// diagnostics - which is exactly how DX10 inject capture hung in "preparing".
// A never-issued query is trivially ready, and a query that cannot answer at all
// must not be allowed to block reuse either.
inline CaptureCopyQuerySlotState ClassifyCaptureCopyQuerySlot(bool queryPresent, bool queryIssued, HRESULT getDataHr) {
    if (!queryPresent || !queryIssued)
        return CaptureCopyQuerySlotState::Ready;
    if (getDataHr == S_OK)
        return CaptureCopyQuerySlotState::Ready;
    if (getDataHr == S_FALSE)
        return CaptureCopyQuerySlotState::GpuBusy;
    return CaptureCopyQuerySlotState::QueryUnusable;
}

// Pending frame metadata for async capture thread
struct PendingCaptureFrame {
    int64_t timestampQPC = 0;           // QPC timestamp when frame was submitted
    uint64_t fenceValue = 0;            // GPU fence value for sync
    uint32_t backBufferIndex = 0;       // Back buffer index in swapchain (inject) or
                                        // texture ring index (WGC)
    uint64_t completionFenceValue = 0;  // Value to signal when capture complete
    void* apiData = nullptr;            // API-specific data (swapchain pointer, texture pointer, etc.)
    uint64_t syncObject = 0;            // Generic sync object handle (e.g. Vulkan Semaphore)
};

// Shared capture state - base class for all capture implementations
// Used by: DX11Capture, DX12 CaptureContext, WGCCaptureEngine
class CaptureBase {
public:
    // Shared texture handles (exported to other processes)
    // CRITICAL FIX: Use atomic handles to prevent double-close race conditions
    std::atomic<HANDLE> sharedTextureHandles[CAPTURE_TEXTURE_COUNT]{};
    // Legacy IDXGIResource::GetSharedHandle values are KMT identifiers owned by
    // the resource, not Win32 handles. Only entries explicitly marked owned may
    // be passed to CloseHandle (for example, CreateSharedHandle NT handles).
    std::atomic<bool> sharedTextureHandleOwned[CAPTURE_TEXTURE_COUNT]{};
    std::atomic<HANDLE> sharedFenceHandle{NULL};
    std::atomic<bool> sharedFenceHandleOwned{false};

    // Capture dimensions and format
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;  // DXGI_FORMAT cast to uint32
    int32_t luidLow = 0;
    int32_t luidHigh = 0;

    // Ring buffer state
    std::atomic<int> writeIndex{0};
    uint64_t fenceValue = 0;

    // Initialization state
    bool initialized = false;

    // Async capture thread ring buffer (lock-free SPSC)
    PendingCaptureFrame pendingRing[CAPTURE_RING_SIZE]{};
    std::atomic<uint32_t> pendingWriteIdx{0};
    std::atomic<uint32_t> pendingReadIdx{0};

    // Async capture thread
    std::thread captureThread;
    std::atomic<bool> captureThreadRunning{false};
    std::atomic<bool> captureThreadShutdown{false};
    HANDLE captureEvent = NULL;

    // Completion fence for back-pressure
    std::atomic<uint64_t> completionFenceValue{0};
    std::atomic<uint64_t> pendingCaptureWaitValue{0};

    // Recording session tracking
    std::atomic<int> recordingSessionID{0};

    // Frame statistics
    std::atomic<uint32_t> droppedFrames{0};  // Count of frames dropped due to ring full

    virtual ~CaptureBase() {
        StopCaptureThread();
    }

    // Check if ring buffer has space
    bool HasPendingSpace() const {
        const uint32_t wIdx = pendingWriteIdx.load(std::memory_order_relaxed);
        return (wIdx - pendingReadIdx.load(std::memory_order_acquire)) < CAPTURE_RING_SIZE;
    }

    // Enqueue a frame for async capture
    bool EnqueueFrame(int64_t timestampQPC, uint64_t gpuFenceValue, uint32_t backBufferIndex, void* apiData,
                      uint64_t syncObject = 0) {
        const uint32_t wIdx = pendingWriteIdx.load(std::memory_order_relaxed);
        if (wIdx - pendingReadIdx.load(std::memory_order_acquire) >= CAPTURE_RING_SIZE) {
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
            return false;  // Ring full, frame dropped
        }

        PendingCaptureFrame& f = pendingRing[wIdx % CAPTURE_RING_SIZE];
        f.timestampQPC = timestampQPC;
        f.fenceValue = gpuFenceValue;
        f.backBufferIndex = backBufferIndex;
        f.apiData = apiData;
        f.syncObject = syncObject;

        uint64_t val = completionFenceValue.fetch_add(1, std::memory_order_acq_rel) + 1;
        f.completionFenceValue = val;
        pendingCaptureWaitValue.store(val, std::memory_order_release);

        pendingWriteIdx.store(wIdx + 1, std::memory_order_release);
        if (captureEvent)
            SetEvent(captureEvent);

        return true;
    }

    // Cleanup shared handles to prevent resource leaks
    // CRITICAL FIX: Use exchange() to atomically get and clear handles
    // This prevents double-close if called from multiple threads
    void CleanupSharedHandles() {
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            HANDLE h = sharedTextureHandles[i].exchange(NULL, std::memory_order_acq_rel);
            const bool owned = sharedTextureHandleOwned[i].exchange(false, std::memory_order_acq_rel);
            if (h && owned) {
                CloseHandle(h);
            }
        }
        // Fence handle: check ownership flag (KMT handles from GetSharedHandle
        // must not be CloseHandle'd). Use acq_rel for consistency with textures.
        HANDLE h = sharedFenceHandle.exchange(NULL, std::memory_order_acq_rel);
        const bool fenceOwned = sharedFenceHandleOwned.exchange(false, std::memory_order_acq_rel);
        if (h && fenceOwned) {
            CloseHandle(h);
        }
    }

    // Publish shared handles to IPC shared memory
    // Note: sharedMem pointer is passed rather than using global g_IPC
    // to support both hook (g_IPC->GetSharedMem()) and WGC (g_pSharedMem)
    // These handles are being published for the first time on this thread,
    // so relaxed loads are safe — no concurrent consumer can read them yet.
    void PublishToSharedMemory(SharedMemoryLayout* sharedMem) {
        if (!sharedMem)
            return;

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            sharedMem->SetSharedHandle(i, (uint64_t)sharedTextureHandles[i].load(std::memory_order_relaxed));
        }
        sharedMem->SetFenceShareHandle((uint64_t)sharedFenceHandle.load(std::memory_order_relaxed));
        sharedMem->SetWidth(width);
        sharedMem->SetHeight(height);
        sharedMem->SetFormat(format);
        sharedMem->SetLuidLowPart(luidLow);
        sharedMem->SetLuidHighPart(luidHigh);
        sharedMem->SetLuidSourcePid(GetCurrentProcessId());
        sharedMem->SetSourcePid(GetCurrentProcessId());
    }

    // Advance write index (called after copy completes)
    int AdvanceWriteIndex() {
        int idx = writeIndex.load(std::memory_order_acquire);
        for (;;) {
            int wrappedIdx = idx % CAPTURE_TEXTURE_COUNT;
            int next = wrappedIdx + 1;
            if (next >= CAPTURE_TEXTURE_COUNT) {
                next = 0;
            }
            if (writeIndex.compare_exchange_weak(idx, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return wrappedIdx;
            }
        }
    }

    // Signal frame ready to IPC ring buffer
    bool SignalFrameReady(SharedMemoryLayout* sharedMem, int textureIndex, int64_t timestamp, uint64_t gpuFenceValue,
                          bool* transitionedFromEmpty = nullptr) {
        if (transitionedFromEmpty)
            *transitionedFromEmpty = false;
        if (!sharedMem)
            return false;

        auto& ring = sharedMem->frameRing;
        // CRITICAL FIX: Use acquire ordering to see consumer's readIndex updates
        uint32_t wIdx = ring.writeIndex.load(std::memory_order_acquire);
        uint32_t rIdx = ring.readIndex.load(std::memory_order_acquire);
        if ((uint32_t)(wIdx - rIdx) >= (uint32_t)FRAME_RING_SIZE) {
            ring.droppedFrames.fetch_add(1, std::memory_order_relaxed);
            sharedMem->runtimeState.injectProducerMetadataFullDrops.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        // readIndex is a texture-lease acknowledgement and may deliberately
        // lag after the consumer has ingested all metadata. Event coalescing
        // must compare against the independent ingest cursor or a refill can
        // leave the consumer asleep forever.
        const bool wasEmpty = wIdx == ring.ingestIndex.load(std::memory_order_acquire);
        auto& slot = ring.slots[wIdx % FRAME_RING_SIZE];

        // Write frame metadata
        slot.timestamp = timestamp;
        slot.displayTimingSequence = 0;
        slot.textureIndex = textureIndex;
        slot.fenceValue = gpuFenceValue;
        slot.frameIndex = wIdx;
        slot.sourcePid = GetCurrentProcessId();
        slot.captureFlags = SHARED_FRAME_CAPTURE_NONE;
        slot.displayTimingGeneration = 0;

        slot.valid.store(1, std::memory_order_release);

        // Publish write index with release semantics to synchronize with reader
        ring.writeIndex.store(wIdx + 1, std::memory_order_release);
        if (transitionedFromEmpty)
            *transitionedFromEmpty = wasEmpty;
        return true;
    }

    // Start async capture thread
    template <typename ThreadFunc>
    void StartCaptureThread(ThreadFunc func) {
        bool expected = false;
        if (!captureThreadRunning.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
            return;
        }

        // A joinable thread means StopCaptureThread() was not completed. Starting a
        // second std::thread would terminate the process when it overwrote the first.
        if (captureThread.joinable()) {
            captureThreadRunning.store(false, std::memory_order_release);
            return;
        }

        captureEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (!captureEvent) {
            captureThreadRunning.store(false, std::memory_order_release);
            return;
        }

        captureThreadShutdown.store(false, std::memory_order_release);
        try {
            captureThread = std::thread(func);
        } catch (...) {
            CloseHandle(captureEvent);
            captureEvent = NULL;
            captureThreadRunning.store(false, std::memory_order_release);
            return;
        }
        // Do NOT detach. We want to join on shutdown.
    }

    // Stop async capture thread
    void StopCaptureThread() {
        captureThreadShutdown.store(true, std::memory_order_release);
        if (captureEvent)
            SetEvent(captureEvent);

        if (captureThread.joinable()) {
            captureThread.join();
        }

        if (captureEvent) {
            CloseHandle(captureEvent);
            captureEvent = NULL;
        }

        captureThreadRunning.store(false, std::memory_order_release);
    }

    // Reset for new recording session
    void ResetForNewRecording() {
        droppedFrames.store(0, std::memory_order_relaxed);
        pendingWriteIdx.store(0, std::memory_order_relaxed);
        pendingReadIdx.store(0, std::memory_order_relaxed);
        writeIndex.store(0, std::memory_order_relaxed);
        completionFenceValue.store(0, std::memory_order_relaxed);
        pendingCaptureWaitValue.store(0, std::memory_order_relaxed);
        for (auto& pending : pendingRing) {
            pending = {};
        }
        recordingSessionID.fetch_add(1, std::memory_order_relaxed);
    }

    // Virtual methods - implemented by each API/capture source
    virtual void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) = 0;
    virtual void Cleanup() = 0;
};
