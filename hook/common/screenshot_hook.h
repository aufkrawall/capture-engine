#pragma once

#include <stdint.h>

// Forward declarations for D3D11 types (avoids pulling in d3d11.h which conflicts with extern "C" blocks)
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

// Save a D3D11 texture as a BMP file.
// Reads the texture pixels via staging texture and writes a 24-bit BMP.
// Called from the render thread inside the present hook.
// Returns true on success.
bool SaveD3D11TextureAsBMP(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                           const char* outputPath);

// Write raw BGRA pixel data as a 24-bit BMP file (blocking).
bool WriteBMPFile(const char* outputPath, const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t rowPitch);

// Write raw BGRA pixel data as a 24-bit BMP file (non-blocking).
// Copies pixels to a buffer and starts a background thread. Returns immediately.
void WriteBMPFileAsync(const char* outputPath, const uint8_t* pixels, uint32_t width, uint32_t height,
                       uint32_t rowPitch);

#ifdef __d3d12_h__
// Save a D3D12 backbuffer as a BMP file.
// Uses a readback heap + command list on the game's D3D12 device.
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;
bool SaveDX12TextureAsBMP(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer,
                          const char* outputPath);
#endif
