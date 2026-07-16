// Screenshot capture utilities shared by the injected graphics backends.
// GPU readback remains synchronous, but mapped pixels are copied before the
// render thread returns and all filesystem work is serialized on one worker.

#include "screenshot_hook.h"

#include "hook_common.h"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kMaximumScreenshotDimension = 16384;

struct ScreenshotTask {
    SharedMemoryLayout* sharedMemory = nullptr;
    uint64_t requestId = 0;
    ScreenshotRawHeaderV2 header{};
    std::vector<uint8_t> pixels;
    std::wstring partPath;
    std::wstring readyPath;
    char completionEventName[128]{};
};

size_t BoundedStringLength(const char* text, size_t capacity) {
    if (!text)
        return capacity;
    for (size_t i = 0; i < capacity; ++i) {
        if (text[i] == '\0')
            return i;
    }
    return capacity;
}

bool Utf8ToWide(const char* text, size_t length, std::wstring& result) {
    result.clear();
    if (!text || length == 0 || length > static_cast<size_t>(std::numeric_limits<int>::max()))
        return false;

    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, static_cast<int>(length), nullptr, 0);
    if (count <= 0)
        return false;
    result.resize(static_cast<size_t>(count));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, static_cast<int>(length), result.data(), count) ==
           count;
}

bool BuildReadyPath(const std::wstring& partPath, std::wstring& readyPath) {
    constexpr wchar_t suffix[] = L".part";
    constexpr size_t suffixLength = (sizeof(suffix) / sizeof(suffix[0])) - 1;
    if (partPath.size() <= suffixLength ||
        partPath.compare(partPath.size() - suffixLength, suffixLength, suffix) != 0) {
        return false;
    }
    readyPath.assign(partPath.data(), partPath.size() - suffixLength);
    readyPath += L".ready";
    return true;
}

uint32_t BytesPerPixel(ScreenshotPixelFormat format) {
    switch (format) {
        case ScreenshotPixelFormat::BGRA8:
        case ScreenshotPixelFormat::RGBA8:
        case ScreenshotPixelFormat::R10G10B10A2:
            return 4;
        case ScreenshotPixelFormat::RGBA16F:
            return 8;
        default:
            return 0;
    }
}

bool IsFormatEncodingPairValid(ScreenshotPixelFormat format, ScreenshotColorEncoding encoding) {
    switch (format) {
        case ScreenshotPixelFormat::BGRA8:
        case ScreenshotPixelFormat::RGBA8:
            return encoding == ScreenshotColorEncoding::SRGB;
        case ScreenshotPixelFormat::R10G10B10A2:
            return encoding == ScreenshotColorEncoding::BT2020_PQ;
        case ScreenshotPixelFormat::RGBA16F:
            return encoding == ScreenshotColorEncoding::LinearScRGB;
        default:
            return false;
    }
}

bool ValidatePixelLayout(uint32_t width, uint32_t height, uint32_t rowPitch, ScreenshotPixelFormat format,
                         ScreenshotColorEncoding encoding, uint64_t& payloadSize) {
    payloadSize = 0;
    const uint32_t bytesPerPixel = BytesPerPixel(format);
    if (width == 0 || height == 0 || width > kMaximumScreenshotDimension || height > kMaximumScreenshotDimension ||
        bytesPerPixel == 0 || !IsFormatEncodingPairValid(format, encoding)) {
        return false;
    }

    const uint64_t minimumRowPitch = static_cast<uint64_t>(width) * bytesPerPixel;
    if (rowPitch < minimumRowPitch || rowPitch % bytesPerPixel != 0 ||
        static_cast<uint64_t>(rowPitch) - minimumRowPitch > 65535ULL) {
        return false;
    }
    if (height != 0 && rowPitch > std::numeric_limits<uint64_t>::max() / height)
        return false;
    payloadSize = static_cast<uint64_t>(rowPitch) * height;
    return payloadSize <= static_cast<uint64_t>(std::numeric_limits<size_t>::max()) &&
           payloadSize <= std::numeric_limits<uint64_t>::max() - sizeof(ScreenshotRawHeaderV2);
}

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
    const size_t length = BoundedStringLength(eventName, 128);
    if (length == 0 || length == 128)
        return;
    HANDLE event = OpenEventA(EVENT_MODIFY_STATE, FALSE, eventName);
    if (event) {
        SetEvent(event);
        CloseHandle(event);
    }
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

class ScreenshotWorkerQueue {
public:
    ScreenshotWorkerQueue() : thread_(&ScreenshotWorkerQueue::Run, this) {}

    ~ScreenshotWorkerQueue() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_one();
        if (thread_.joinable())
            thread_.join();
    }

    bool Enqueue(ScreenshotTask&& task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || occupied_)
            return false;
        occupied_ = true;
        task_ = std::move(task);
        condition_.notify_one();
        return true;
    }

private:
    void Run() {
        for (;;) {
            ScreenshotTask task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [&]() { return stopping_ || !task_.pixels.empty(); });
                if (stopping_ && task_.pixels.empty())
                    return;
                task = std::move(task_);
            }

            uint32_t error = ERROR_SUCCESS;
            const bool success = WriteTaskPayload(task, error);
            const uint64_t currentRequestId =
                task.sharedMemory->runtimeState.screenshotRequestId.load(std::memory_order_acquire);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                occupied_ = false;
                task_ = {};
            }
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
    std::thread thread_;
    ScreenshotTask task_;
    bool occupied_ = false;
    bool stopping_ = false;
};

std::mutex g_workerMutex;
ScreenshotWorkerQueue* g_worker = nullptr;

bool EnqueueOnWorker(ScreenshotTask&& task) {
    std::lock_guard<std::mutex> lock(g_workerMutex);
    if (!g_worker) {
        try {
            g_worker = new ScreenshotWorkerQueue();
        } catch (...) {
            g_worker = nullptr;
            return false;
        }
    }
    return g_worker->Enqueue(std::move(task));
}

bool GetD3D11PixelDescription(DXGI_FORMAT format, ScreenshotPixelFormat& pixelFormat,
                              ScreenshotColorEncoding& colorEncoding) {
    switch (format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            pixelFormat = ScreenshotPixelFormat::BGRA8;
            colorEncoding = ScreenshotColorEncoding::SRGB;
            return true;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            pixelFormat = ScreenshotPixelFormat::RGBA8;
            colorEncoding = ScreenshotColorEncoding::SRGB;
            return true;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            pixelFormat = ScreenshotPixelFormat::R10G10B10A2;
            colorEncoding = ScreenshotColorEncoding::BT2020_PQ;
            return true;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            pixelFormat = ScreenshotPixelFormat::RGBA16F;
            colorEncoding = ScreenshotColorEncoding::LinearScRGB;
            return true;
        default:
            return false;
    }
}

}  // namespace

uint64_t GetPendingScreenshotRequestId(SharedMemoryLayout* sharedMemory) {
    if (!sharedMemory)
        return 0;
    const uint64_t requestId = sharedMemory->runtimeState.screenshotRequestId.load(std::memory_order_acquire);
    if (requestId == 0 ||
        sharedMemory->runtimeState.screenshotCompletedRequestId.load(std::memory_order_acquire) == requestId ||
        sharedMemory->runtimeState.screenshotStatus.load(std::memory_order_acquire) !=
            static_cast<uint32_t>(ScreenshotRequestStatus::Pending)) {
        return 0;
    }
    return requestId;
}

void CompleteScreenshotRequest(SharedMemoryLayout* sharedMemory, uint64_t requestId, ScreenshotRequestStatus status,
                               uint32_t error, ScreenshotPayloadKind payloadKind) {
    if (!sharedMemory ||
        sharedMemory->runtimeState.screenshotCompletedRequestId.load(std::memory_order_acquire) == requestId)
        return;
    char eventName[128]{};
    const size_t length = BoundedStringLength(sharedMemory->runtimeState.screenshotCompletionEventName,
                                              sizeof(sharedMemory->runtimeState.screenshotCompletionEventName));
    if (length < sizeof(eventName)) {
        std::copy_n(sharedMemory->runtimeState.screenshotCompletionEventName, length, eventName);
    }
    CompleteScreenshotRequestForEvent(sharedMemory, requestId, status, error, payloadKind, eventName);
}

bool QueueScreenshotPixels(SharedMemoryLayout* sharedMemory, uint64_t requestId, const uint8_t* pixels, uint32_t width,
                           uint32_t height, uint32_t rowPitch, ScreenshotPixelFormat pixelFormat,
                           ScreenshotColorEncoding colorEncoding) {
    if (!sharedMemory || requestId == 0 || !pixels || GetPendingScreenshotRequestId(sharedMemory) != requestId) {
        return false;
    }

    uint64_t payloadSize = 0;
    const size_t pathLength = BoundedStringLength(sharedMemory->runtimeState.screenshotPath,
                                                  sizeof(sharedMemory->runtimeState.screenshotPath));
    const size_t eventLength = BoundedStringLength(sharedMemory->runtimeState.screenshotCompletionEventName,
                                                   sizeof(sharedMemory->runtimeState.screenshotCompletionEventName));
    ScreenshotTask task;
    try {
        if (!ValidatePixelLayout(width, height, rowPitch, pixelFormat, colorEncoding, payloadSize) || pathLength == 0 ||
            pathLength == sizeof(sharedMemory->runtimeState.screenshotPath) || eventLength == 0 ||
            eventLength == sizeof(sharedMemory->runtimeState.screenshotCompletionEventName) ||
            !Utf8ToWide(sharedMemory->runtimeState.screenshotPath, pathLength, task.partPath) ||
            !BuildReadyPath(task.partPath, task.readyPath)) {
            CompleteScreenshotRequest(sharedMemory, requestId, ScreenshotRequestStatus::Failed, ERROR_INVALID_DATA);
            return false;
        }
    } catch (...) {
        CompleteScreenshotRequest(sharedMemory, requestId, ScreenshotRequestStatus::Failed, ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }

    uint32_t expected = static_cast<uint32_t>(ScreenshotRequestStatus::Pending);
    if (!sharedMemory->runtimeState.screenshotStatus.compare_exchange_strong(
            expected, static_cast<uint32_t>(ScreenshotRequestStatus::Writing), std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }

    try {
        task.sharedMemory = sharedMemory;
        task.requestId = requestId;
        task.header.pixelFormat = static_cast<uint32_t>(pixelFormat);
        task.header.colorEncoding = static_cast<uint32_t>(colorEncoding);
        task.header.width = width;
        task.header.height = height;
        task.header.rowPitch = rowPitch;
        task.header.payloadSize = payloadSize;
        task.header.totalSize = sizeof(ScreenshotRawHeaderV2) + payloadSize;
        task.header.requestId = requestId;
        std::copy_n(sharedMemory->runtimeState.screenshotCompletionEventName, eventLength, task.completionEventName);
        task.pixels.assign(pixels, pixels + static_cast<size_t>(payloadSize));
    } catch (...) {
        CompleteScreenshotRequest(sharedMemory, requestId, ScreenshotRequestStatus::Failed, ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }

    if (!EnqueueOnWorker(std::move(task))) {
        CompleteScreenshotRequest(sharedMemory, requestId, ScreenshotRequestStatus::Busy, ERROR_BUSY);
        return false;
    }
    return true;
}

bool SaveD3D11TextureAsScreenshotRaw(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                                     SharedMemoryLayout* sharedMemory, uint64_t requestId) {
    if (!device || !context || !texture || !sharedMemory || requestId == 0)
        return false;

    D3D11_TEXTURE2D_DESC sourceDesc{};
    texture->GetDesc(&sourceDesc);
    ScreenshotPixelFormat pixelFormat{};
    ScreenshotColorEncoding colorEncoding{};
    if (!GetD3D11PixelDescription(sourceDesc.Format, pixelFormat, colorEncoding)) {
        HookLog("[Screenshot] Unsupported D3D11 format: %u", static_cast<unsigned>(sourceDesc.Format));
        return false;
    }

    ID3D11Texture2D* copySource = texture;
    ID3D11Texture2D* resolved = nullptr;
    D3D11_TEXTURE2D_DESC copyDesc = sourceDesc;
    if (sourceDesc.SampleDesc.Count > 1) {
        copyDesc.SampleDesc.Count = 1;
        copyDesc.SampleDesc.Quality = 0;
        copyDesc.Usage = D3D11_USAGE_DEFAULT;
        copyDesc.BindFlags = 0;
        copyDesc.CPUAccessFlags = 0;
        copyDesc.MiscFlags = 0;
        HRESULT resolveHr = device->CreateTexture2D(&copyDesc, nullptr, &resolved);
        if (FAILED(resolveHr))
            return false;
        context->ResolveSubresource(resolved, 0, texture, 0, sourceDesc.Format);
        copySource = resolved;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = copyDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.SampleDesc.Quality = 0;

    ID3D11Texture2D* staging = nullptr;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr)) {
        if (resolved)
            resolved->Release();
        HookLog("[Screenshot] D3D11 staging texture creation failed: hr=0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    context->CopyResource(staging, copySource);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    bool queued = false;
    if (SUCCEEDED(hr)) {
        queued =
            QueueScreenshotPixels(sharedMemory, requestId, static_cast<const uint8_t*>(mapped.pData), sourceDesc.Width,
                                  sourceDesc.Height, mapped.RowPitch, pixelFormat, colorEncoding);
        context->Unmap(staging, 0);
    } else {
        HookLog("[Screenshot] D3D11 staging map failed: hr=0x%08X", static_cast<unsigned>(hr));
    }
    staging->Release();
    if (resolved)
        resolved->Release();
    return queued;
}

bool SaveDX12TextureAsScreenshotRaw(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer,
                                    SharedMemoryLayout* sharedMemory, uint64_t requestId) {
    if (!device || !queue || !backBuffer || !sharedMemory || requestId == 0)
        return false;

    const D3D12_RESOURCE_DESC sourceDesc = backBuffer->GetDesc();
    if (sourceDesc.Width == 0 || sourceDesc.Width > std::numeric_limits<uint32_t>::max())
        return false;
    ScreenshotPixelFormat pixelFormat{};
    ScreenshotColorEncoding colorEncoding{};
    if (!GetD3D11PixelDescription(sourceDesc.Format, pixelFormat, colorEncoding)) {
        HookLog("[Screenshot] Unsupported D3D12 format: %u", static_cast<unsigned>(sourceDesc.Format));
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowSize = 0;
    UINT64 bufferSize = 0;
    device->GetCopyableFootprints(&sourceDesc, 0, 1, 0, &footprint, &rows, &rowSize, &bufferSize);
    if (bufferSize == 0 || footprint.Footprint.RowPitch > std::numeric_limits<uint32_t>::max())
        return false;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = bufferSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* readback = nullptr;
    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(hr))
        return false;
    ID3D12CommandAllocator* allocator = nullptr;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(hr)) {
        readback->Release();
        return false;
    }
    ID3D12GraphicsCommandList* commandList = nullptr;
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&commandList));
    if (FAILED(hr)) {
        allocator->Release();
        readback->Release();
        return false;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = backBuffer;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    commandList->ResourceBarrier(1, &barrier);
    hr = commandList->Close();
    if (FAILED(hr)) {
        commandList->Release();
        allocator->Release();
        readback->Release();
        return false;
    }
    ID3D12CommandList* lists[] = {commandList};
    queue->ExecuteCommandLists(1, lists);

    ID3D12Fence* fence = nullptr;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fenceEvent = nullptr;
    if (SUCCEEDED(hr))
        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(hr) || !fenceEvent || FAILED(queue->Signal(fence, 1)) ||
        FAILED(fence->SetEventOnCompletion(1, fenceEvent)) ||
        WaitForSingleObject(fenceEvent, INFINITE) != WAIT_OBJECT_0) {
        if (fenceEvent)
            CloseHandle(fenceEvent);
        if (fence)
            fence->Release();
        commandList->Release();
        allocator->Release();
        readback->Release();
        return false;
    }
    CloseHandle(fenceEvent);

    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(bufferSize)};
    hr = readback->Map(0, &readRange, &mapped);
    bool queued = false;
    if (SUCCEEDED(hr) && mapped) {
        queued = QueueScreenshotPixels(sharedMemory, requestId, static_cast<const uint8_t*>(mapped),
                                       static_cast<uint32_t>(sourceDesc.Width), sourceDesc.Height,
                                       footprint.Footprint.RowPitch, pixelFormat, colorEncoding);
        const D3D12_RANGE writtenRange{0, 0};
        readback->Unmap(0, &writtenRange);
    }

    fence->Release();
    commandList->Release();
    allocator->Release();
    readback->Release();
    return queued;
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
