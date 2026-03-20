#pragma once

#include <stdint.h>

// Forward declarations for D3D11 types (avoids pulling in d3d11.h which conflicts with extern "C" blocks)
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

// HDR raw file header (32 bytes) - written before pixel data for HDR captures.
// The controller reads this header to determine format and metadata for AVIF encoding.
struct HDRRawHeader {
    uint32_t magic;  // 0x52415748 ("HRAW")
    uint32_t width;
    uint32_t height;
    uint32_t format;  // 0 = R10G10B10A2, 1 = R16G16B16A16F
    uint32_t rowPitch;
    uint32_t isPQ;  // 1 = PQ (HDR10), 0 = scRGB
    uint32_t reserved[2];
};

static constexpr uint32_t kHDRRawMagic = 0x52415748;  // "HRAW"
static constexpr uint32_t kHDRFormatR10 = 0;
static constexpr uint32_t kHDRFormatR16F = 1;

// Save a D3D11 texture as a BMP file (SDR only).
// Reads the texture pixels via staging texture and writes a 24-bit BMP.
// Returns true on success.
bool SaveD3D11TextureAsBMP(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                           const char* outputPath);

// Save a D3D11 texture as an HDR raw file (for R10G10B10A2 or R16G16B16A16F).
// Writes the HDRRawHeader followed by raw pixel data.
// isPQ: true if the texture uses PQ transfer (HDR10), false for scRGB (linear FP16).
void SaveD3D11TextureAsHDR(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture, bool isPQ,
                           const char* outputPath);

// Write raw BGRA pixel data as a 24-bit BMP file (blocking).
bool WriteBMPFile(const char* outputPath, const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t rowPitch);

// Write raw BGRA pixel data as a 24-bit BMP file (non-blocking).
void WriteBMPFileAsync(const char* outputPath, const uint8_t* pixels, uint32_t width, uint32_t height,
                       uint32_t rowPitch);

// Write HDR raw pixel data (non-blocking). Writes header + raw pixels on worker thread.
void WriteHDRRawAsync(const char* outputPath, const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t rowPitch,
                      uint32_t format, bool isPQ);

#ifdef __d3d12_h__
// Save a D3D12 backbuffer. Detects HDR and saves as BMP (SDR) or RAW (HDR).
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;
bool SaveDX12TextureAsBMP(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer,
                          const char* outputPath);
bool SaveDX12TextureAsHDR(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer, bool isPQ,
                          const char* outputPath);
#endif
