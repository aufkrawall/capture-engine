#pragma once

#include <stdint.h>

#include "../../common/shared_defs.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;

uint64_t GetPendingScreenshotRequestId(SharedMemoryLayout* sharedMemory);

void CompleteScreenshotRequest(SharedMemoryLayout* sharedMemory, uint64_t requestId, ScreenshotRequestStatus status,
                               uint32_t error, ScreenshotPayloadKind payloadKind = ScreenshotPayloadKind::None);

bool QueueScreenshotPixels(SharedMemoryLayout* sharedMemory, uint64_t requestId, const uint8_t* pixels,
                           uint32_t width, uint32_t height, uint32_t rowPitch, ScreenshotPixelFormat pixelFormat,
                           ScreenshotColorEncoding colorEncoding);

bool SaveD3D11TextureAsScreenshotRaw(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                                     SharedMemoryLayout* sharedMemory, uint64_t requestId);

bool SaveDX12TextureAsScreenshotRaw(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer,
                                    SharedMemoryLayout* sharedMemory, uint64_t requestId);

void ShutdownScreenshotWorker();
