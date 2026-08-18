// Screenshot worker: the one thread allowed to wait. It collects the submitted
// D3D12 readback and performs the filesystem write, so render and present
// threads only ever record, submit and hand over.

#include "screenshot_worker.h"

#include "hook_common.h"
#include "screenshot_hook.h"

#include <d3d12.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

// A live device that has not signalled within this window is wedged well past
// the driver's own timeout detection, which marks the device removed. Slicing
// the wait keeps that check - and hook shutdown - responsive.
constexpr DWORD kReadbackWaitSliceMs = 1000;
constexpr int kReadbackWaitSlices = 10;

bool WriteAll(HANDLE file, const void* data, uint64_t size) {
    const auto* cursor = static_cast<const uint8_t*>(data);
    while (size != 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(size, 16ULL * 1024ULL * 1024ULL));
        DWORD written = 0;
        if (!WriteFile(file, cursor, chunk, &written, nullptr) || written != chunk)
            return false;
        cursor += written;
        size -= written;
    }
    return true;
}

void SignalCompletionEvent(const char* eventName) {
    const size_t length = ScreenshotBoundedStringLength(eventName, 128);
    if (length == 0 || length == 128)
        return;
    HANDLE event = OpenEventA(EVENT_MODIFY_STATE, FALSE, eventName);
    if (event) {
        SetEvent(event);
        CloseHandle(event);
    }
}

bool WriteTaskPayload(const ScreenshotTask& task, uint32_t& error) {
    error = ERROR_SUCCESS;
    HANDLE file = CreateFileW(task.partPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }

    bool success = WriteAll(file, &task.header, sizeof(task.header)) &&
                   WriteAll(file, task.pixels.data(), task.header.payloadSize);
    if (success && !FlushFileBuffers(file)) {
        error = GetLastError();
        success = false;
    }
    if (!CloseHandle(file)) {
        if (success)
            error = GetLastError();
        success = false;
    }
    if (!success) {
        if (error == ERROR_SUCCESS)
            error = ERROR_WRITE_FAULT;
        DeleteFileW(task.partPath.c_str());
        return false;
    }

    if (!MoveFileExW(task.partPath.c_str(), task.readyPath.c_str(), MOVEFILE_WRITE_THROUGH)) {
        error = GetLastError();
        DeleteFileW(task.partPath.c_str());
        return false;
    }
    return true;
}

// True when the readback resources are provably idle and safe to release.
bool ReadbackIsIdle(const ScreenshotDx12Readback& readback, bool completed, uint32_t error) {
    // A removed device has already discarded the work referencing them.
    return completed || error == ERROR_DEVICE_REMOVED ||
           (readback.fence && readback.fence->GetCompletedValue() >= readback.fenceValue);
}

// Waits for the submitted copy, then lifts the pixels out of the readback heap.
bool AwaitDx12Copy(const ScreenshotDx12Readback& readback, const std::atomic<bool>& abort, uint32_t& error) {
    if (readback.fence->GetCompletedValue() >= readback.fenceValue)
        return true;

    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent) {
        error = GetLastError();
        return false;
    }
    if (FAILED(readback.fence->SetEventOnCompletion(readback.fenceValue, fenceEvent))) {
        CloseHandle(fenceEvent);
        error = ERROR_GEN_FAILURE;
        return false;
    }

    bool completed = false;
    for (int slice = 0; slice < kReadbackWaitSlices; ++slice) {
        if (WaitForSingleObject(fenceEvent, kReadbackWaitSliceMs) == WAIT_OBJECT_0) {
            completed = true;
            break;
        }
        if (readback.device && FAILED(readback.device->GetDeviceRemovedReason())) {
            // A removed device never signals, and its resources went with it.
            error = ERROR_DEVICE_REMOVED;
            break;
        }
        if (abort.load(std::memory_order_acquire)) {
            error = ERROR_OPERATION_ABORTED;
            break;
        }
        HookLogImportant("[Screenshot] DX12 readback still pending after %d ms (fence=%llu target=%llu)",
                         (slice + 1) * static_cast<int>(kReadbackWaitSliceMs),
                         static_cast<unsigned long long>(readback.fence->GetCompletedValue()),
                         static_cast<unsigned long long>(readback.fenceValue));
    }
    CloseHandle(fenceEvent);
    if (!completed && error == ERROR_SUCCESS)
        error = ERROR_TIMEOUT;
    return completed;
}

bool CopyReadbackPixels(ScreenshotTask& task, uint32_t& error) {
    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(task.readback.bufferSize)};
    if (FAILED(task.readback.buffer->Map(0, &readRange, &mapped)) || !mapped) {
        error = ERROR_READ_FAULT;
        return false;
    }

    bool copied = false;
    try {
        const auto* pixels = static_cast<const uint8_t*>(mapped);
        task.pixels.assign(pixels, pixels + static_cast<size_t>(task.header.payloadSize));
        copied = true;
    } catch (...) {
        error = ERROR_NOT_ENOUGH_MEMORY;
    }
    const D3D12_RANGE writtenRange{0, 0};
    task.readback.buffer->Unmap(0, &writtenRange);
    return copied;
}

bool CollectDx12Readback(ScreenshotTask& task, const std::atomic<bool>& abort, uint32_t& error) {
    error = ERROR_SUCCESS;
    if (!task.readback.fence || !task.readback.buffer) {
        error = ERROR_INVALID_DATA;
        task.readback.Disown();
        return false;
    }

    const bool completed = AwaitDx12Copy(task.readback, abort, error);
    const bool resolved = completed && CopyReadbackPixels(task, error);

    if (ReadbackIsIdle(task.readback, completed, error)) {
        ReleaseScreenshotDx12Readback(task.readback);
    } else {
        // The copy is still in flight on a live device. Releasing now would hand
        // the GPU freed memory, so these interfaces are deliberately stranded
        // rather than recycled - a wedged GPU is already the larger failure.
        HookLogImportant(
            "[Screenshot] DX12 readback abandoned while still in flight (error=%u); retaining its resources so "
            "the GPU cannot read freed memory",
            error);
        task.readback.Disown();
    }
    if (!resolved)
        HookLogImportant("[Screenshot] DX12 readback failed: error=%u", error);
    return resolved;
}

class ScreenshotWorkerQueue {
public:
    ScreenshotWorkerQueue() : thread_(&ScreenshotWorkerQueue::Run, this) {}

    ~ScreenshotWorkerQueue() {
        stopping_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex_);
        }
        condition_.notify_one();
        if (thread_.joinable())
            thread_.join();
    }

    bool Reserve() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_.load(std::memory_order_acquire) || reserved_)
            return false;
        reserved_ = true;
        return true;
    }

    void ReleaseReservation() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            reserved_ = false;
            hasTask_ = false;
            task_ = {};
        }
        condition_.notify_one();
    }

    void SubmitReserved(ScreenshotTask&& task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            task_ = std::move(task);
            hasTask_ = true;
        }
        condition_.notify_one();
    }

private:
    void Run() {
        for (;;) {
            ScreenshotTask task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [&]() { return stopping_.load(std::memory_order_acquire) || hasTask_; });
                if (!hasTask_)
                    return;
                task = std::move(task_);
                task_ = {};
                hasTask_ = false;
            }

            uint32_t error = ERROR_SUCCESS;
            bool success = true;
            if (task.readback.submitted)
                success = CollectDx12Readback(task, stopping_, error);
            if (success)
                success = WriteTaskPayload(task, error);

            const uint64_t currentRequestId =
                task.sharedMemory->runtimeState.screenshotRequestId.load(std::memory_order_acquire);
            ReleaseReservation();
            if (currentRequestId != task.requestId) {
                DeleteFileW(task.partPath.c_str());
                DeleteFileW(task.readyPath.c_str());
                if (currentRequestId == 0) {
                    uint32_t writing = static_cast<uint32_t>(ScreenshotRequestStatus::Writing);
                    task.sharedMemory->runtimeState.screenshotStatus.compare_exchange_strong(
                        writing, static_cast<uint32_t>(ScreenshotRequestStatus::Idle), std::memory_order_acq_rel,
                        std::memory_order_acquire);
                }
            } else {
                CompleteScreenshotRequestForEvent(
                    task.sharedMemory, task.requestId,
                    success ? ScreenshotRequestStatus::Succeeded : ScreenshotRequestStatus::Failed, error,
                    success ? ScreenshotPayloadKind::RawV2 : ScreenshotPayloadKind::None, task.completionEventName);
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    ScreenshotTask task_;
    bool reserved_ = false;
    bool hasTask_ = false;
};

std::mutex g_workerMutex;
ScreenshotWorkerQueue* g_worker = nullptr;

ScreenshotWorkerQueue* AcquireWorker() {
    if (!g_worker) {
        try {
            g_worker = new ScreenshotWorkerQueue();
        } catch (...) {
            g_worker = nullptr;
        }
    }
    return g_worker;
}

}  // namespace

ScreenshotDx12Readback& ScreenshotDx12Readback::operator=(ScreenshotDx12Readback&& other) noexcept {
    if (this == &other)
        return *this;
    device = other.device;
    fence = other.fence;
    buffer = other.buffer;
    allocator = other.allocator;
    commandList = other.commandList;
    fenceValue = other.fenceValue;
    bufferSize = other.bufferSize;
    submitted = other.submitted;
    other.Disown();
    return *this;
}

void ScreenshotDx12Readback::Disown() noexcept {
    device = nullptr;
    fence = nullptr;
    buffer = nullptr;
    allocator = nullptr;
    commandList = nullptr;
    fenceValue = 0;
    bufferSize = 0;
    submitted = false;
}

void ReleaseScreenshotDx12Readback(ScreenshotDx12Readback& readback) {
    if (readback.commandList)
        readback.commandList->Release();
    if (readback.allocator)
        readback.allocator->Release();
    if (readback.fence)
        readback.fence->Release();
    if (readback.buffer)
        readback.buffer->Release();
    readback.Disown();
}

size_t ScreenshotBoundedStringLength(const char* text, size_t capacity) {
    if (!text)
        return capacity;
    for (size_t i = 0; i < capacity; ++i) {
        if (text[i] == '\0')
            return i;
    }
    return capacity;
}

void CompleteScreenshotRequestForEvent(SharedMemoryLayout* sharedMemory, uint64_t requestId,
                                       ScreenshotRequestStatus status, uint32_t error,
                                       ScreenshotPayloadKind payloadKind, const char* eventName) {
    if (!sharedMemory || requestId == 0 ||
        sharedMemory->runtimeState.screenshotRequestId.load(std::memory_order_acquire) != requestId) {
        return;
    }

    sharedMemory->runtimeState.screenshotError.store(error, std::memory_order_relaxed);
    sharedMemory->runtimeState.screenshotPayloadKind.store(static_cast<uint32_t>(payloadKind),
                                                           std::memory_order_relaxed);
    sharedMemory->runtimeState.screenshotStatus.store(static_cast<uint32_t>(status), std::memory_order_release);
    sharedMemory->runtimeState.screenshotCompletedRequestId.store(requestId, std::memory_order_release);
    SignalCompletionEvent(eventName);
}

bool ReserveScreenshotWorkerSlot() {
    std::lock_guard<std::mutex> lock(g_workerMutex);
    ScreenshotWorkerQueue* worker = AcquireWorker();
    return worker && worker->Reserve();
}

void ReleaseScreenshotWorkerSlot() {
    std::lock_guard<std::mutex> lock(g_workerMutex);
    if (g_worker)
        g_worker->ReleaseReservation();
}

void SubmitReservedScreenshotTask(ScreenshotTask&& task) {
    std::lock_guard<std::mutex> lock(g_workerMutex);
    if (g_worker)
        g_worker->SubmitReserved(std::move(task));
}

bool EnqueueScreenshotTask(ScreenshotTask&& task) {
    std::lock_guard<std::mutex> lock(g_workerMutex);
    ScreenshotWorkerQueue* worker = AcquireWorker();
    if (!worker || !worker->Reserve())
        return false;
    worker->SubmitReserved(std::move(task));
    return true;
}

void ShutdownScreenshotWorker() {
    ScreenshotWorkerQueue* worker = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_workerMutex);
        worker = g_worker;
        g_worker = nullptr;
    }
    delete worker;
}
