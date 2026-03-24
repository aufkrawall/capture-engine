#pragma once

#include <d3d11.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

// Frame data that can represent either inject or framegrab mode
// Non-copyable to prevent accidental texture double-release.
// Use std::move() to transfer ownership.
struct QueuedFrame {
    QueuedFrame() = default;
    ~QueuedFrame() = default;
    QueuedFrame(const QueuedFrame&) = delete;
    QueuedFrame& operator=(const QueuedFrame&) = delete;
    QueuedFrame(QueuedFrame&& other) noexcept {
        *this = std::move(other);
    }
    QueuedFrame& operator=(QueuedFrame&& other) noexcept {
        if (this != &other) {
            sharedHandle = other.sharedHandle;
            other.sharedHandle = nullptr;
            fenceHandle = other.fenceHandle;
            other.fenceHandle = nullptr;
            fenceValue = other.fenceValue;
            other.fenceValue = 0;
            luidLow = other.luidLow;
            other.luidLow = 0;
            luidHigh = other.luidHigh;
            other.luidHigh = 0;
            sourcePid = other.sourcePid;
            other.sourcePid = 0;
            ringIndex = other.ringIndex;
            other.ringIndex = 0;
            frameIndex = other.frameIndex;
            other.frameIndex = 0;
            textureIndex = other.textureIndex;
            other.textureIndex = -1;
            enqueueQpc = other.enqueueQpc;
            other.enqueueQpc = 0;
            deferCount = other.deferCount;
            other.deferCount = 0;
            format = other.format;
            other.format = 0;
            texture = other.texture;
            other.texture = nullptr;
            timestamp = other.timestamp;
            other.timestamp = 0;
            rawTimestamp = other.rawTimestamp;
            other.rawTimestamp = 0;
            width = other.width;
            other.width = 0;
            height = other.height;
            other.height = 0;
            isInjectMode = other.isInjectMode;
            other.isInjectMode = false;
            isHDR = other.isHDR;
            other.isHDR = false;
            duplicateSourceTimestamp = other.duplicateSourceTimestamp;
            other.duplicateSourceTimestamp = false;
            captureLeft = other.captureLeft;
            other.captureLeft = 0;
            captureTop = other.captureTop;
            other.captureTop = 0;
            isShmem = other.isShmem;
            other.isShmem = false;
            shmemSlot = other.shmemSlot;
            other.shmemSlot = 0;
        }
        return *this;
    }
    // For inject mode (D3D12 shared handles)
    HANDLE sharedHandle = nullptr;
    HANDLE fenceHandle = nullptr;
    uint64_t fenceValue = 0;
    int32_t luidLow = 0;
    int32_t luidHigh = 0;
    uint32_t sourcePid = 0;
    uint32_t ringIndex = 0;  // Index in the SharedMemory ring buffer
    uint32_t frameIndex = 0;
    int32_t textureIndex = -1;
    int64_t enqueueQpc = 0;
    uint32_t deferCount = 0;
    uint32_t format = 0;     // DXGI_FORMAT or API-specific format from shared memory

    // For framegrab mode (D3D11 texture)
    ID3D11Texture2D* texture = nullptr;  // AddRef'd by producer, Release'd by consumer

    // Common fields
    int64_t timestamp = 0;
    int64_t rawTimestamp = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool isInjectMode = false;  // true = use inject fields, false = use framegrab fields
    bool isHDR = false;         // New: Signals Rec.2100 PQ content
    bool duplicateSourceTimestamp = false;
    int32_t captureLeft = 0;    // Screen-space origin for partial-capture cursor overlay
    int32_t captureTop = 0;

    // For shmem fallback (D3D9 Win11)
    bool isShmem = false;
    uint32_t shmemSlot = 0;
};

// Thread-safe circular buffer for frames
// Provides buffering between capture (producer) and encoding (consumer)
// to absorb encoder spike latency and prevent visual stutter
class FrameQueue {
public:
    explicit FrameQueue(size_t capacity = 3) : maxCapacity(capacity) {}

    // Call when recording starts to begin grace period
    void StartRecording() {
        std::lock_guard<std::mutex> lock(mtx);
        recordingStartTime = std::chrono::steady_clock::now();
        droppedFrames.store(0, std::memory_order_relaxed);
    }

    ~FrameQueue() {
        std::vector<ID3D11Texture2D*> texturesToRelease;
        {
            std::lock_guard<std::mutex> lock(mtx);
            texturesToRelease.reserve(buffer.size());
            for (auto& frame : buffer) {
                if (!frame.isInjectMode && frame.texture) {
                    texturesToRelease.push_back(frame.texture);
                }
            }
            buffer.clear();
        }
        for (auto* texture : texturesToRelease) {
            texture->Release();
        }
    }

    // Producer: Push a frame (non-blocking, moves ownership).
    // The queue always takes ownership; if it is already full, the oldest queued
    // frame is dropped to make room. Use GetDroppedCount() for overflow telemetry.
    bool Push(QueuedFrame&& frame, bool countAsDrop = true) {
        ID3D11Texture2D* textureToRelease = nullptr;
        {
            std::lock_guard<std::mutex> lock(mtx);

            if (buffer.size() >= maxCapacity) {
                // Queue full - drop oldest frame to make room
                auto& oldest = buffer.front();
                if (!oldest.isInjectMode && oldest.texture) {
                    textureToRelease = oldest.texture;
                    oldest.texture = nullptr;
                }
                buffer.pop_front();

                // Only count as dropped if outside grace period (first 2 seconds)
                if (countAsDrop) {
                    auto elapsed = std::chrono::steady_clock::now() - recordingStartTime;
                    if (elapsed > GRACE_PERIOD) {
                        droppedFrames.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }

            buffer.push_back(std::move(frame));
            cv.notify_one();
        }
        if (textureToRelease) {
            textureToRelease->Release();
        }
        return true;
    }

    // Consumer: Pop a frame (blocking with timeout, moves ownership)
    // Returns false if timeout elapsed with no frame available
    bool Pop(QueuedFrame& frame, int timeoutMs) {
        std::unique_lock<std::mutex> lock(mtx);

        if (buffer.empty()) {
            // Wait for frame with timeout
            auto result =
                cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return !buffer.empty() || !running; });
            if (!result || buffer.empty()) {
                return false;  // Timeout or shutdown
            }
        }

        frame = std::move(buffer.front());
        buffer.pop_front();
        return true;
    }

    // Peek at front timestamp without removing (for duplicate detection)
    // Returns false if queue is empty
    bool PeekTimestamp(int64_t& timestamp) const {
        std::lock_guard<std::mutex> lock(mtx);
        if (buffer.empty())
            return false;
        timestamp = buffer.front().timestamp;
        return true;
    }

    // Query methods
    size_t Size() const {
        std::lock_guard<std::mutex> lock(mtx);
        return buffer.size();
    }

    bool IsFull() const {
        std::lock_guard<std::mutex> lock(mtx);
        return buffer.size() >= maxCapacity;
    }

    bool IsEmpty() const {
        std::lock_guard<std::mutex> lock(mtx);
        return buffer.empty();
    }

    size_t Capacity() const {
        return maxCapacity;
    }

    uint64_t GetDroppedCount() const {
        return droppedFrames.load(std::memory_order_relaxed);
    }
    void ResetDroppedCount() {
        droppedFrames.store(0, std::memory_order_relaxed);
    }

    // Clear any pending frames
    void Clear() {
        std::vector<ID3D11Texture2D*> texturesToRelease;
        {
            std::lock_guard<std::mutex> lock(mtx);
            texturesToRelease.reserve(buffer.size());
            for (auto& frame : buffer) {
                if (!frame.isInjectMode && frame.texture) {
                    texturesToRelease.push_back(frame.texture);
                }
            }
            buffer.clear();
            droppedFrames.store(0, std::memory_order_relaxed);
        }
        for (auto* texture : texturesToRelease) {
            texture->Release();
        }
    }

    // Signal shutdown to unblock waiting consumers
    void Shutdown() {
        std::lock_guard<std::mutex> lock(mtx);
        running = false;
        cv.notify_all();
    }

private:
    static constexpr std::chrono::seconds GRACE_PERIOD{2};  // 2-second grace period

    std::deque<QueuedFrame> buffer;
    mutable std::mutex mtx;
    std::condition_variable cv;
    size_t maxCapacity;
    bool running = true;
    std::atomic<uint64_t> droppedFrames{0};
    std::chrono::steady_clock::time_point recordingStartTime{};  // For grace period
};
