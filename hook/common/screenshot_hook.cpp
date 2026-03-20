// Screenshot capture utilities for the hook DLL.
// Provides BMP writing for all graphics APIs (DX11, DX10, DX9, OpenGL, DX12, Vulkan).
// BMP writing is done on a background thread to avoid blocking the game's render thread.

#include "screenshot_hook.h"
#include "hook_common.h"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <string.h>
#include <windows.h>
#include <vector>

// BMP file header (14 bytes) + DIB header (40 bytes) = 54 bytes
#pragma pack(push, 1)
struct BMPFileHeader {
    uint16_t type;      // 'BM'
    uint32_t fileSize;  // Total file size
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t offsetData;  // Offset to pixel data
};

struct BMPInfoHeader {
    uint32_t size;  // Size of this header (40)
    int32_t width;
    int32_t height;        // Negative = top-down
    uint16_t planes;       // Must be 1
    uint16_t bitCount;     // 24 for BGR
    uint32_t compression;  // 0 = BI_RGB (uncompressed)
    uint32_t sizeImage;    // Image size (can be 0 for BI_RGB)
    int32_t xPelsPerMeter;
    int32_t yPelsPerMeter;
    uint32_t clrUsed;
    uint32_t clrImportant;
};
#pragma pack(pop)

// Write raw BGRA pixel data as a 24-bit BMP file (blocking - use only from worker thread)
static bool WriteBMPFileSync(const char* outputPath, const uint8_t* pixels, uint32_t width, uint32_t height,
                             uint32_t rowPitch) {
    uint32_t bmpRowPitch = (width * 3 + 3) & ~3u;
    uint32_t pixelDataSize = bmpRowPitch * height;

    BMPFileHeader fileHeader = {};
    fileHeader.type = 0x4D42;
    fileHeader.offsetData = sizeof(BMPFileHeader) + sizeof(BMPInfoHeader);
    fileHeader.fileSize = fileHeader.offsetData + pixelDataSize;

    BMPInfoHeader infoHeader = {};
    infoHeader.size = sizeof(BMPInfoHeader);
    infoHeader.width = static_cast<int32_t>(width);
    infoHeader.height = -static_cast<int32_t>(height);
    infoHeader.planes = 1;
    infoHeader.bitCount = 24;
    infoHeader.compression = 0;
    infoHeader.sizeImage = pixelDataSize;

    HANDLE hFile = CreateFileA(outputPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        HookLog("[Screenshot] CreateFile(%s) failed: error=%lu", outputPath, GetLastError());
        return false;
    }

    DWORD written = 0;
    WriteFile(hFile, &fileHeader, sizeof(fileHeader), &written, NULL);
    WriteFile(hFile, &infoHeader, sizeof(infoHeader), &written, NULL);

    std::vector<uint8_t> bmpRow(bmpRowPitch);
    const uint8_t* srcRow = pixels;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* src = srcRow;
        uint8_t* dst = bmpRow.data();
        for (uint32_t x = 0; x < width; ++x) {
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst += 3;
            src += 4;
        }
        WriteFile(hFile, bmpRow.data(), bmpRowPitch, &written, NULL);
        srcRow += rowPitch;
    }

    CloseHandle(hFile);
    HookLog("[Screenshot] Saved: %s (%ux%u)", outputPath, width, height);
    return true;
}

// Async BMP write: copies pixels to a buffer and starts a worker thread.
// The render thread returns immediately after the memcpy.
struct ScreenshotTask {
    std::vector<uint8_t> pixels;  // BGRA pixel data
    uint32_t width;
    uint32_t height;
    uint32_t rowPitch;
    std::string outputPath;
};

static DWORD WINAPI ScreenshotWorker(LPVOID param) {
    ScreenshotTask* task = static_cast<ScreenshotTask*>(param);
    WriteBMPFileSync(task->outputPath.c_str(), task->pixels.data(), task->width, task->height, task->rowPitch);
    delete task;
    return 0;
}

// Public API: write BMP file from BGRA pixels (blocking, for direct use)
bool WriteBMPFile(const char* outputPath, const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t rowPitch) {
    return WriteBMPFileSync(outputPath, pixels, width, height, rowPitch);
}

// Save BGRA pixels as BMP asynchronously. Copies pixels and starts a worker thread.
// Returns immediately (render thread is not blocked by disk I/O).
static void SavePixelsAsync(const char* outputPath, const uint8_t* pixels, uint32_t width, uint32_t height,
                            uint32_t rowPitch) {
    auto* task = new ScreenshotTask;
    task->width = width;
    task->height = height;
    task->rowPitch = rowPitch;
    task->outputPath = outputPath;

    // Copy pixels to task buffer (this is the only part that runs on the render thread)
    uint32_t dataSize = rowPitch * height;
    task->pixels.assign(pixels, pixels + dataSize);

    // Start worker thread
    HANDLE hThread = CreateThread(nullptr, 0, ScreenshotWorker, task, 0, nullptr);
    if (hThread) {
        CloseHandle(hThread);  // Detach - worker deletes itself
    } else {
        // Thread creation failed - do it synchronously
        WriteBMPFileSync(outputPath, pixels, width, height, rowPitch);
        delete task;
    }
}

// Public async BMP writer (for Vulkan layer and other non-D3D11 paths)
void WriteBMPFileAsync(const char* outputPath, const uint8_t* pixels, uint32_t width, uint32_t height,
                       uint32_t rowPitch) {
    SavePixelsAsync(outputPath, pixels, width, height, rowPitch);
}

// ---- HDR Raw File Writing ----
// HDR raw files have a 32-byte header followed by raw pixel data.
// The controller reads this and encodes as AVIF via FFmpeg.

struct HDRRawTask {
    HDRRawHeader header;
    std::vector<uint8_t> pixels;
    std::string outputPath;
};

static DWORD WINAPI HDRRawWorker(LPVOID param) {
    HDRRawTask* task = static_cast<HDRRawTask*>(param);

    HANDLE hFile =
        CreateFileA(task->outputPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hFile, &task->header, sizeof(task->header), &written, NULL);
        WriteFile(hFile, task->pixels.data(), static_cast<DWORD>(task->pixels.size()), &written, NULL);
        CloseHandle(hFile);
        HookLog("[Screenshot] Saved HDR raw: %s (%ux%u)", task->outputPath.c_str(), task->header.width,
                task->header.height);
    }

    delete task;
    return 0;
}

void WriteHDRRawAsync(const char* outputPath, const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t rowPitch,
                      uint32_t format, bool isPQ) {
    auto* task = new HDRRawTask;
    task->header.magic = kHDRRawMagic;
    task->header.width = width;
    task->header.height = height;
    task->header.format = format;
    task->header.rowPitch = rowPitch;
    task->header.isPQ = isPQ ? 1 : 0;
    task->header.reserved[0] = 0;
    task->header.reserved[1] = 0;
    task->outputPath = outputPath;

    uint32_t dataSize = rowPitch * height;
    task->pixels.assign(pixels, pixels + dataSize);

    HANDLE hThread = CreateThread(nullptr, 0, HDRRawWorker, task, 0, nullptr);
    if (hThread) {
        CloseHandle(hThread);
    } else {
        // Fallback: write synchronously
        HANDLE hFile = CreateFileA(outputPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(hFile, &task->header, sizeof(task->header), &written, NULL);
            WriteFile(hFile, task->pixels.data(), static_cast<DWORD>(task->pixels.size()), &written, NULL);
            CloseHandle(hFile);
        }
        delete task;
    }
}

// Save D3D11 texture as HDR raw (R10G10B10A2 or R16G16B16A16F)
void SaveD3D11TextureAsHDR(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture, bool isPQ,
                           const char* outputPath) {
    if (!device || !context || !texture || !outputPath)
        return;

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    uint32_t format = (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? kHDRFormatR16F : kHDRFormatR10;

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr)) {
        HookLog("[Screenshot] HDR CreateTexture2D failed: hr=0x%08X", hr);
        return;
    }

    context->CopyResource(staging, texture);

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        staging->Release();
        HookLog("[Screenshot] HDR Map failed: hr=0x%08X", hr);
        return;
    }

    WriteHDRRawAsync(outputPath, static_cast<const uint8_t*>(mapped.pData), desc.Width, desc.Height, mapped.RowPitch,
                     format, isPQ);

    context->Unmap(staging, 0);
    staging->Release();
}

// Save D3D12 texture as HDR raw
bool SaveDX12TextureAsHDR(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer, bool isPQ,
                          const char* outputPath) {
    if (!device || !queue || !backBuffer || !outputPath)
        return false;

    D3D12_RESOURCE_DESC bbDesc = backBuffer->GetDesc();
    uint32_t width = static_cast<uint32_t>(bbDesc.Width);
    uint32_t height = static_cast<uint32_t>(bbDesc.Height);

    uint32_t bytesPerPixel = (bbDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 : 4;
    UINT64 rowPitch = (width * bytesPerPixel + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                      ~(UINT64)(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    UINT64 bufferSize = rowPitch * height;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = bufferSize;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* readback = nullptr;
    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(hr))
        return false;

    ID3D12CommandAllocator* cmdAlloc = nullptr;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
    if (FAILED(hr)) {
        readback->Release();
        return false;
    }

    ID3D12GraphicsCommandList* cmdList = nullptr;
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc, nullptr, IID_PPV_ARGS(&cmdList));
    if (FAILED(hr)) {
        cmdAlloc->Release();
        readback->Release();
        return false;
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    footprint.Footprint.Format = bbDesc.Format;
    footprint.Footprint.Width = width;
    footprint.Footprint.Height = height;
    footprint.Footprint.Depth = 1;
    footprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = backBuffer;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = readback;
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint = footprint;

    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmdList->ResourceBarrier(1, &barrier);
    cmdList->Close();

    ID3D12CommandList* cmdLists[] = {cmdList};
    queue->ExecuteCommandLists(1, cmdLists);

    ID3D12Fence* fence = nullptr;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) {
        cmdList->Release();
        cmdAlloc->Release();
        readback->Release();
        return false;
    }

    HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence, 1);
    fence->SetEventOnCompletion(1, fenceEvent);

    bool done = (WaitForSingleObject(fenceEvent, 500) == WAIT_OBJECT_0);
    CloseHandle(fenceEvent);

    if (!done) {
        fence->Release();
        cmdList->Release();
        cmdAlloc->Release();
        readback->Release();
        return false;
    }

    void* mappedData = nullptr;
    D3D12_RANGE readRange = {0, static_cast<SIZE_T>(bufferSize)};
    readback->Map(0, &readRange, &mappedData);

    uint32_t format = (bbDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? kHDRFormatR16F : kHDRFormatR10;
    WriteHDRRawAsync(outputPath, static_cast<const uint8_t*>(mappedData), width, height,
                     static_cast<uint32_t>(rowPitch), format, isPQ);

    D3D12_RANGE writtenRange = {0, 0};
    readback->Unmap(0, &writtenRange);

    fence->Release();
    cmdList->Release();
    cmdAlloc->Release();
    readback->Release();
    return true;
}

// Save a D3D11 texture as BMP (DX11/DX10 screenshot path)
// Copies pixels on render thread, writes BMP on worker thread.
bool SaveD3D11TextureAsBMP(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                           const char* outputPath) {
    if (!device || !context || !texture || !outputPath)
        return false;

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = desc.Width;
    stagingDesc.Height = desc.Height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = desc.Format;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D* staging = nullptr;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr)) {
        HookLog("[Screenshot] CreateTexture2D failed: hr=0x%08X", hr);
        return false;
    }

    context->CopyResource(staging, texture);

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        staging->Release();
        HookLog("[Screenshot] Map failed: hr=0x%08X", hr);
        return false;
    }

    // Copy pixels and start async write (render thread returns immediately after this)
    SavePixelsAsync(outputPath, static_cast<const uint8_t*>(mapped.pData), desc.Width, desc.Height, mapped.RowPitch);

    context->Unmap(staging, 0);
    staging->Release();
    return true;
}

// DX12 Screenshot
bool SaveDX12TextureAsBMP(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer,
                          const char* outputPath) {
    if (!device || !queue || !backBuffer || !outputPath)
        return false;

    D3D12_RESOURCE_DESC bbDesc = backBuffer->GetDesc();
    uint32_t width = static_cast<uint32_t>(bbDesc.Width);
    uint32_t height = static_cast<uint32_t>(bbDesc.Height);

    UINT64 rowPitch =
        (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(UINT64)(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    UINT64 bufferSize = rowPitch * height;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = bufferSize;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* readback = nullptr;
    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(hr)) {
        HookLog("[Screenshot] DX12 CreateCommittedResource failed: hr=0x%08X", hr);
        return false;
    }

    ID3D12CommandAllocator* cmdAlloc = nullptr;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
    if (FAILED(hr)) {
        readback->Release();
        return false;
    }

    ID3D12GraphicsCommandList* cmdList = nullptr;
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc, nullptr, IID_PPV_ARGS(&cmdList));
    if (FAILED(hr)) {
        cmdAlloc->Release();
        readback->Release();
        return false;
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    footprint.Footprint.Format = bbDesc.Format;
    footprint.Footprint.Width = width;
    footprint.Footprint.Height = height;
    footprint.Footprint.Depth = 1;
    footprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = backBuffer;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = readback;
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint = footprint;

    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmdList->ResourceBarrier(1, &barrier);
    cmdList->Close();

    ID3D12CommandList* cmdLists[] = {cmdList};
    queue->ExecuteCommandLists(1, cmdLists);

    ID3D12Fence* fence = nullptr;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) {
        cmdList->Release();
        cmdAlloc->Release();
        readback->Release();
        return false;
    }

    HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence, 1);
    fence->SetEventOnCompletion(1, fenceEvent);

    // Wait up to 500ms for GPU copy
    bool done = (WaitForSingleObject(fenceEvent, 500) == WAIT_OBJECT_0);
    CloseHandle(fenceEvent);

    if (!done) {
        HookLog("[Screenshot] DX12 fence wait timed out");
        fence->Release();
        cmdList->Release();
        cmdAlloc->Release();
        readback->Release();
        return false;
    }

    // Map pixels and start async write
    void* mappedData = nullptr;
    D3D12_RANGE readRange = {0, static_cast<SIZE_T>(bufferSize)};
    readback->Map(0, &readRange, &mappedData);

    const uint8_t* pixels = static_cast<const uint8_t*>(mappedData);

    // DX12 swapchains may use RGBA format (R8G8B8A8) instead of BGRA (B8G8R8A8).
    // BMP writer expects BGRA, so swap R/B channels if needed.
    bool isRGBA = (bbDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || bbDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    if (isRGBA) {
        uint32_t srcPitch = static_cast<uint32_t>(rowPitch);
        std::vector<uint8_t> swapped(srcPitch * height);
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* src = pixels + y * srcPitch;
            uint8_t* dst = swapped.data() + y * srcPitch;
            for (uint32_t x = 0; x < width; ++x) {
                dst[0] = src[2];  // B <- R
                dst[1] = src[1];  // G
                dst[2] = src[0];  // R <- B
                dst[3] = src[3];  // A
                src += 4;
                dst += 4;
            }
        }
        SavePixelsAsync(outputPath, swapped.data(), width, height, srcPitch);
    } else {
        SavePixelsAsync(outputPath, pixels, width, height, static_cast<uint32_t>(rowPitch));
    }

    D3D12_RANGE writtenRange = {0, 0};
    readback->Unmap(0, &writtenRange);

    fence->Release();
    cmdList->Release();
    cmdAlloc->Release();
    readback->Release();
    return true;
}
