#pragma once

// Screenshot task queue. One worker owns everything that has to wait: the GPU
// readback fence and the filesystem write. Nothing here may run on a render or
// present thread.

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

#include "../../common/shared_defs.h"

struct ID3D12Device;
struct ID3D12Fence;
struct ID3D12Resource;
struct ID3D12CommandAllocator;
struct ID3D12GraphicsCommandList;

// A submitted D3D12 backbuffer copy the worker still has to collect. The render
// thread records, submits and signals, then hands ownership over without
// waiting. Waiting there instead is a hard freeze: the copy is submitted on the
// game's own swapchain queue, and a frame-generation runtime only drains that
// queue once its presenter thread makes progress - which it cannot do while the
// present call it is driving is blocked inside our hook.
struct ScreenshotDx12Readback {
    ScreenshotDx12Readback() = default;
    ScreenshotDx12Readback(const ScreenshotDx12Readback&) = delete;
    ScreenshotDx12Readback& operator=(const ScreenshotDx12Readback&) = delete;
    ScreenshotDx12Readback(ScreenshotDx12Readback&& other) noexcept {
        *this = std::move(other);
    }
    ScreenshotDx12Readback& operator=(ScreenshotDx12Readback&& other) noexcept;

    // Drops every reference without releasing. Only for handing ownership on, or
    // for deliberately stranding resources the GPU may still be reading.
    void Disown() noexcept;

    ID3D12Device* device = nullptr;
    ID3D12Fence* fence = nullptr;
    ID3D12Resource* buffer = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* commandList = nullptr;
    uint64_t fenceValue = 0;
    uint64_t bufferSize = 0;
    bool submitted = false;
};

// Releases every interface the readback holds. Only safe once the GPU has
// finished with them, so the worker calls it after the fence completes and the
// producer only calls it on paths where nothing was submitted.
void ReleaseScreenshotDx12Readback(ScreenshotDx12Readback& readback);

struct ScreenshotTask {
    SharedMemoryLayout* sharedMemory = nullptr;
    uint64_t requestId = 0;
    ScreenshotRawHeaderV2 header{};
    std::vector<uint8_t> pixels;
    std::wstring partPath;
    std::wstring readyPath;
    char completionEventName[128]{};
    ScreenshotDx12Readback readback;
};

size_t ScreenshotBoundedStringLength(const char* text, size_t capacity);

void CompleteScreenshotRequestForEvent(SharedMemoryLayout* sharedMemory, uint64_t requestId,
                                       ScreenshotRequestStatus status, uint32_t error,
                                       ScreenshotPayloadKind payloadKind, const char* eventName);

// Claims the worker before any GPU work is recorded. A copy that has been
// submitted can then never be stranded by a busy queue: releasing resources the
// GPU still reads would be a use-after-free, and the only other way out would be
// the blocking wait this design exists to remove.
bool ReserveScreenshotWorkerSlot();
void ReleaseScreenshotWorkerSlot();
void SubmitReservedScreenshotTask(ScreenshotTask&& task);

// Reserve-and-submit for producers that already hold the finished pixels.
bool EnqueueScreenshotTask(ScreenshotTask&& task);
