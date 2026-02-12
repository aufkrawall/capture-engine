/**
 * Custom Overlay - DX9 Backend Implementation
 */

#include "custom_overlay_dx9.h"
#include <cstring>

namespace CustomOverlay {

// Custom vertex format for DX9
struct DX9Vertex {
  float x, y, z, rhw; // Transformed position
  DWORD color;        // Diffuse color
  float u, v;         // Texture coords
};

#define D3DFVF_CUSTOMVERTEX (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

DX9Backend::DX9Backend(IDirect3DDevice9 *dev) : device(dev) {}

DX9Backend::~DX9Backend() { Shutdown(); }

bool DX9Backend::Initialize(int fontTextureWidth, int fontTextureHeight,
                            const uint8_t *fontTextureData) {
  if (initialized || !device)
    return false;

  // Create font texture
  HRESULT hr = device->CreateTexture(fontTextureWidth, fontTextureHeight, 1,
                                     D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
                                     D3DPOOL_DEFAULT, &fontTexture, nullptr);
  if (FAILED(hr))
    return false;

  // Lock and copy texture data (convert RGBA to ARGB)
  D3DLOCKED_RECT lockedRect;
  hr = fontTexture->LockRect(0, &lockedRect, nullptr, D3DLOCK_DISCARD);
  if (SUCCEEDED(hr)) {
    for (int y = 0; y < fontTextureHeight; y++) {
      uint8_t *dst = (uint8_t *)lockedRect.pBits + y * lockedRect.Pitch;
      const uint8_t *src = fontTextureData + y * fontTextureWidth * 4;
      for (int x = 0; x < fontTextureWidth; x++) {
        // RGBA -> ARGB (BGRA in memory)
        dst[0] = src[2]; // B
        dst[1] = src[1]; // G
        dst[2] = src[0]; // R
        dst[3] = src[3]; // A
        dst += 4;
        src += 4;
      }
    }
    fontTexture->UnlockRect(0);
  }

  // Create vertex buffer
  vertexBufferSize = 4096;
  hr = device->CreateVertexBuffer((UINT)(vertexBufferSize * sizeof(DX9Vertex)),
                                  D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                  D3DFVF_CUSTOMVERTEX, D3DPOOL_DEFAULT,
                                  &vertexBuffer, nullptr);
  if (FAILED(hr))
    return false;

  // Create index buffer
  indexBufferSize = 8192;
  hr = device->CreateIndexBuffer((UINT)(indexBufferSize * sizeof(uint16_t)),
                                 D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                 D3DFMT_INDEX16, D3DPOOL_DEFAULT, &indexBuffer,
                                 nullptr);
  if (FAILED(hr))
    return false;

  initialized = true;
  return true;
}

void DX9Backend::Shutdown() {
  if (stateBlock) {
    stateBlock->Release();
    stateBlock = nullptr;
  }
  if (indexBuffer) {
    indexBuffer->Release();
    indexBuffer = nullptr;
  }
  if (vertexBuffer) {
    vertexBuffer->Release();
    vertexBuffer = nullptr;
  }
  if (fontTexture) {
    fontTexture->Release();
    fontTexture = nullptr;
  }
  initialized = false;
}

void DX9Backend::Render(const std::vector<DrawVertex> &vertices,
                        const std::vector<uint16_t> &indices,
                        const std::vector<DrawCommand> &commands,
                        int viewportWidth, int viewportHeight) {
  if (!initialized || !device || vertices.empty())
    return;

  // Create state block to save state
  device->CreateStateBlock(D3DSBT_ALL, &stateBlock);
  stateBlock->Capture();

  // Resize buffers if needed
  if (vertices.size() > vertexBufferSize) {
    if (vertexBuffer)
      vertexBuffer->Release();
    vertexBufferSize = vertices.size() * 2;
    device->CreateVertexBuffer((UINT)(vertexBufferSize * sizeof(DX9Vertex)),
                               D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                               D3DFVF_CUSTOMVERTEX, D3DPOOL_DEFAULT,
                               &vertexBuffer, nullptr);
  }

  if (indices.size() > indexBufferSize) {
    if (indexBuffer)
      indexBuffer->Release();
    indexBufferSize = indices.size() * 2;
    device->CreateIndexBuffer((UINT)(indexBufferSize * sizeof(uint16_t)),
                              D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                              D3DFMT_INDEX16, D3DPOOL_DEFAULT, &indexBuffer,
                              nullptr);
  }

  // Convert and upload vertices
  void *vbPtr;
  if (SUCCEEDED(vertexBuffer->Lock(0, 0, &vbPtr, D3DLOCK_DISCARD))) {
    DX9Vertex *dst = (DX9Vertex *)vbPtr;
    for (const auto &v : vertices) {
      dst->x = v.x;
      dst->y = v.y;
      dst->z = 0.0f;
      dst->rhw = 1.0f;
      // Convert ABGR to ARGB
      uint8_t r = (v.color >> 0) & 0xFF;
      uint8_t g = (v.color >> 8) & 0xFF;
      uint8_t b = (v.color >> 16) & 0xFF;
      uint8_t a = (v.color >> 24) & 0xFF;
      dst->color = (a << 24) | (r << 16) | (g << 8) | b;
      dst->u = v.u;
      dst->v = v.v;
      dst++;
    }
    vertexBuffer->Unlock();
  }

  // Upload indices
  void *ibPtr;
  if (SUCCEEDED(indexBuffer->Lock(0, 0, &ibPtr, D3DLOCK_DISCARD))) {
    memcpy(ibPtr, indices.data(), indices.size() * sizeof(uint16_t));
    indexBuffer->Unlock();
  }

  // Set render state
  device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  device->SetRenderState(D3DRS_ZENABLE, FALSE);
  device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  device->SetRenderState(D3DRS_LIGHTING, FALSE);

  device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
  device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
  device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

  device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

  device->SetStreamSource(0, vertexBuffer, 0, sizeof(DX9Vertex));
  device->SetIndices(indexBuffer);
  device->SetFVF(D3DFVF_CUSTOMVERTEX);

  // Draw commands
  for (const auto &cmd : commands) {
    if (cmd.useTexture) {
      device->SetTexture(0, fontTexture);
    } else {
      device->SetTexture(0, nullptr);
    }

    device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, cmd.vertexOffset, 0,
                                 (UINT)vertices.size(), cmd.indexOffset,
                                 cmd.indexCount / 3);
  }

  // Restore state
  stateBlock->Apply();
  stateBlock->Release();
  stateBlock = nullptr;
}

} // namespace CustomOverlay
