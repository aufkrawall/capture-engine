// Screenshot capture utilities shared by the injected graphics backends. The
// D3D12 backbuffer readback is submitted here and collected by the worker, so no
// present-path thread ever waits on the GPU; every other backend copies its
// mapped pixels before the render thread returns.

#include "screenshot_hook.h"

#include "hook_common.h"
#include "screenshot_worker.h"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace {

constexpr uint32_t kMaximumScreenshotDimension = 16384;

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
            return encoding == ScreenshotColorEncoding::BT2020_PQ ||
                   encoding == ScreenshotColorEncoding::BT709_G22 || encoding == ScreenshotColorEncoding::SRGB;
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

bool GetD3D11PixelDescription(DXGI_FORMAT format, ce::presentation_color::Encoding presentationEncoding,
                              ScreenshotPixelFormat& pixelFormat,
                              ScreenshotColorEncoding& colorEncoding) {
    switch (format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            if (presentationEncoding != ce::presentation_color::Encoding::Sdr709)
                return false;
            pixelFormat = ScreenshotPixelFormat::BGRA8;
            colorEncoding = ScreenshotColorEncoding::SRGB;
            return true;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            if (presentationEncoding != ce::presentation_color::Encoding::Sdr709)
                return false;
            pixelFormat = ScreenshotPixelFormat::RGBA8;
            colorEncoding = ScreenshotColorEncoding::SRGB;
            return true;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            pixelFormat = ScreenshotPixelFormat::R10G10B10A2;
            if (presentationEncoding == ce::presentation_color::Encoding::Hdr10Pq) {
                colorEncoding = ScreenshotColorEncoding::BT2020_PQ;
                return true;
            }
            if (presentationEncoding == ce::presentation_color::Encoding::Sdr709) {
                colorEncoding = ScreenshotColorEncoding::BT709_G22;
                return true;
            }
            return false;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            if (presentationEncoding != ce::presentation_color::Encoding::LinearScRgb)
                return false;
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
    const size_t length = ScreenshotBoundedStringLength(sharedMemory->runtimeState.screenshotCompletionEventName,
                                              sizeof(sharedMemory->runtimeState.screenshotCompletionEventName));
    if (length < sizeof(eventName)) {
        std::copy_n(sharedMemory->runtimeState.screenshotCompletionEventName, length, eventName);
    }
    CompleteScreenshotRequestForEvent(sharedMemory, requestId, status, error, payloadKind, eventName);
}

// Resolves the destination paths and header for a request and claims it by
// moving its status to Writing. Pixels arrive afterwards - from the caller for
// the synchronous backends, from the worker for the submitted D3D12 copy - so
// both share one description of what is being written.
static bool BuildScreenshotTask(SharedMemoryLayout* sharedMemory, uint64_t requestId, uint32_t width, uint32_t height,
                                uint32_t rowPitch, ScreenshotPixelFormat pixelFormat,
                                ScreenshotColorEncoding colorEncoding, ScreenshotTask& task) {
    if (!sharedMemory || requestId == 0 || GetPendingScreenshotRequestId(sharedMemory) != requestId)
        return false;

    uint64_t payloadSize = 0;
    const size_t pathLength = ScreenshotBoundedStringLength(sharedMemory->runtimeState.screenshotPath,
                                                            sizeof(sharedMemory->runtimeState.screenshotPath));
    const size_t eventLength =
        ScreenshotBoundedStringLength(sharedMemory->runtimeState.screenshotCompletionEventName,
                                      sizeof(sharedMemory->runtimeState.screenshotCompletionEventName));
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
    return true;
}

bool QueueScreenshotPixels(SharedMemoryLayout* sharedMemory, uint64_t requestId, const uint8_t* pixels, uint32_t width,
                           uint32_t height, uint32_t rowPitch, ScreenshotPixelFormat pixelFormat,
                           ScreenshotColorEncoding colorEncoding) {
    if (!pixels)
        return false;

    ScreenshotTask task;
    if (!BuildScreenshotTask(sharedMemory, requestId, width, height, rowPitch, pixelFormat, colorEncoding, task))
        return false;

    try {
        task.pixels.assign(pixels, pixels + static_cast<size_t>(task.header.payloadSize));
    } catch (...) {
        CompleteScreenshotRequest(sharedMemory, requestId, ScreenshotRequestStatus::Failed, ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }

    if (!EnqueueScreenshotTask(std::move(task))) {
        CompleteScreenshotRequest(sharedMemory, requestId, ScreenshotRequestStatus::Busy, ERROR_BUSY);
        return false;
    }
    return true;
}

bool SaveD3D11TextureAsScreenshotRaw(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                                     SharedMemoryLayout* sharedMemory, uint64_t requestId,
                                     ce::presentation_color::Encoding presentationEncoding) {
    if (!device || !context || !texture || !sharedMemory || requestId == 0)
        return false;

    D3D11_TEXTURE2D_DESC sourceDesc{};
    texture->GetDesc(&sourceDesc);
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    ScreenshotPixelFormat pixelFormat{};
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    ScreenshotColorEncoding colorEncoding{};
    if (!GetD3D11PixelDescription(sourceDesc.Format, presentationEncoding, pixelFormat, colorEncoding)) {
        HookLog("[Screenshot] Unsupported D3D11 presentation contract: format=%u encoding=%s",
                static_cast<unsigned>(sourceDesc.Format), ce::presentation_color::Describe(presentationEncoding));
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
                                    SharedMemoryLayout* sharedMemory, uint64_t requestId,
                                    ce::presentation_color::Encoding presentationEncoding) {
    if (!device || !queue || !backBuffer || !sharedMemory || requestId == 0)
        return false;

    const D3D12_RESOURCE_DESC sourceDesc = backBuffer->GetDesc();
    if (sourceDesc.Width == 0 || sourceDesc.Width > std::numeric_limits<uint32_t>::max())
        return false;
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    ScreenshotPixelFormat pixelFormat{};
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    ScreenshotColorEncoding colorEncoding{};
    if (!GetD3D11PixelDescription(sourceDesc.Format, presentationEncoding, pixelFormat, colorEncoding)) {
        HookLog("[Screenshot] Unsupported D3D12 presentation contract: format=%u encoding=%s",
                static_cast<unsigned>(sourceDesc.Format), ce::presentation_color::Describe(presentationEncoding));
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowSize = 0;
    UINT64 bufferSize = 0;
    device->GetCopyableFootprints(&sourceDesc, 0, 1, 0, &footprint, &rows, &rowSize, &bufferSize);
    if (bufferSize == 0 || footprint.Footprint.RowPitch > std::numeric_limits<uint32_t>::max())
        return false;

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
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

    // Claim the worker before recording anything. Once the copy is submitted it
    // has to be collected somewhere, and the only alternatives - waiting here or
    // releasing resources the GPU still reads - are a freeze and a
    // use-after-free respectively.
    if (!ReserveScreenshotWorkerSlot()) {
        CompleteScreenshotRequest(sharedMemory, requestId, ScreenshotRequestStatus::Busy, ERROR_BUSY);
        return false;
    }

    ScreenshotTask task;
    if (!BuildScreenshotTask(sharedMemory, requestId, static_cast<uint32_t>(sourceDesc.Width), sourceDesc.Height,
                             footprint.Footprint.RowPitch, pixelFormat, colorEncoding, task)) {
        ReleaseScreenshotWorkerSlot();
        return false;
    }

    ScreenshotDx12Readback readback;
    readback.device = device;
    readback.bufferSize = bufferSize;
    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&readback.buffer));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&readback.allocator));
    if (SUCCEEDED(hr)) {
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, readback.allocator, nullptr,
                                       IID_PPV_ARGS(&readback.commandList));
    }
    if (SUCCEEDED(hr))
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&readback.fence));
    if (FAILED(hr)) {
        // Nothing was submitted yet, so releasing here cannot race the GPU.
        ReleaseScreenshotDx12Readback(readback);
        ReleaseScreenshotWorkerSlot();
        HookLog("[Screenshot] D3D12 readback resource creation failed: hr=0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    readback.commandList->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = backBuffer;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.buffer;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    readback.commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    readback.commandList->ResourceBarrier(1, &barrier);
    hr = readback.commandList->Close();
    if (FAILED(hr)) {
        ReleaseScreenshotDx12Readback(readback);
        ReleaseScreenshotWorkerSlot();
        HookLog("[Screenshot] D3D12 readback command list close failed: hr=0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    // The copy rides the queue the caller already presents on, so it is ordered
    // against the game's own work without any cross-queue wait - adding one here
    // would inject CE into a frame-generation runtime's scheduling.
    ID3D12CommandList* lists[] = {readback.commandList};
    queue->ExecuteCommandLists(1, lists);
    readback.fenceValue = 1;
    if (FAILED(queue->Signal(readback.fence, readback.fenceValue))) {
        // The signal never reached the queue, but the copy did. The resources
        // stay alive rather than being released under in-flight GPU work.
        readback.Disown();
        ReleaseScreenshotWorkerSlot();
        HookLog("[Screenshot] D3D12 readback fence signal failed; abandoning the request");
        return false;
    }
    readback.submitted = true;

    task.readback = std::move(readback);
    SubmitReservedScreenshotTask(std::move(task));
    return true;
}
