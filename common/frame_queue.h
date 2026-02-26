#pragma once

#include <d3d11.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>

// Frame data that can represent either inject or framegrab mode
struct QueuedFrame {
    // For inject mode (D3D12 shared handles)
    HANDLE sharedHandle = nullptr;
    HANDLE fenceHandle = nullptr;
    uint64_t fenceValue = 0;
    int32_t luidLow = 0;
    int32_t luidHigh = 0;
    uint32_t sourcePid = 0;
    uint32_t ringIndex = 0;  // Index in the SharedMemory ring buffer
    uint32_t format = 0;     // DXGI_FORMAT or API-specific format from shared memory

    // For framegrab mode (D3D11 texture)
    ID3D11Texture2D* texture = nullptr;  // AddRef'd by producer, Release'd by consumer

    // Common fields
    int64_t timestamp = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool isInjectMode = false;  // true = use inject fields, false = use framegrab fields
    bool isHDR = false;         // New: Signals Rec.2100 PQ content

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
        // Release any remaining textures
        std::lock_guard<std::mutex> lock(mtx);
        for (auto& frame : buffer) {
            if (!frame.isInjectMode && frame.texture) {
                frame.texture->Release();
            }
        }
        buffer.clear();
    }

    // Producer: Push a frame (non-blocking)
    // Returns false if queue is full (frame will be dropped)
    bool Push(const QueuedFrame& frame, bool countAsDrop = true) {
        std::lock_guard<std::mutex> lock(mtx);

        if (buffer.size() >= maxCapacity) {
            // Queue full - drop oldest frame to make room
            auto& oldest = buffer.front();
            if (!oldest.isInjectMode && oldest.texture) {
                oldest.texture->Release();
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

        buffer.push_back(frame);
        cv.notify_one();
        return true;
    }

    // Consumer: Pop a frame (blocking with timeout)
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

        frame = buffer.front();
        buffer.pop_front();
        return true;
    }

    // Peek at front without removing (for duplicate detection)
    bool Peek(QueuedFrame& frame) const {
        std::lock_guard<std::mutex> lock(mtx);
        if (buffer.empty())
            return false;
        frame = buffer.front();
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

    uint64_t GetDroppedCount() const {
        return droppedFrames.load(std::memory_order_relaxed);
    }
    void ResetDroppedCount() {
        droppedFrames.store(0, std::memory_order_relaxed);
    }

    // Clear any pending frames
    void Clear() {
        std::lock_guard<std::mutex> lock(mtx);
        for (auto& frame : buffer) {
            if (!frame.isInjectMode && frame.texture) {
                frame.texture->Release();
            }
        }
        buffer.clear();
        droppedFrames.store(0, std::memory_order_relaxed);
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
